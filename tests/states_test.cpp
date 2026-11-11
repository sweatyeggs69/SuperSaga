// Save state slots without the game: Store save/load/rename/remove on a blank machine, auto-load
// decision, and fallback when the Auto state is unusable. No ROM or game data needed.
#include <cstdio>
#include <filesystem>
#include <memory>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "app/states.h"
#include "gba/gba.h"

namespace {
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)
}  // namespace

int main() {
    using namespace supersaga::states;
    namespace fs = std::filesystem;
    // Startup decision table: rom_ok, --play, autoload, auto exists, auto usable, shift.
    CHECK(choose_startup(true, false, false, true, true, false) == Startup::Launcher);
    CHECK(choose_startup(true, true, false, true, true, false) == Startup::BootGame);       // --play only
    CHECK(choose_startup(true, false, true, true, true, false) == Startup::ResumeAuto);     // autoload
    CHECK(choose_startup(true, true, true, true, true, false) == Startup::ResumeAuto);      // shortcut + autoload
    CHECK(choose_startup(true, false, true, false, false, false) == Startup::BootGame);     // no Auto state yet
    CHECK(choose_startup(true, false, true, true, false, false) == Startup::LauncherWithError);  // unusable
    CHECK(choose_startup(true, false, true, true, true, true) == Startup::Launcher);        // Shift bypass
    CHECK(choose_startup(false, false, true, true, true, false) == Startup::LauncherWithError);  // bad ROM
    CHECK(choose_startup(false, false, false, false, false, false) == Startup::Launcher);

    // "Save to new slot": first empty slot after the current one, wrapping; -1 when all are full.
    {
        std::vector<bool> used(kSlots + 1, false);
        CHECK(next_free_slot(used, 1) == 2);
        used[2] = used[3] = true;
        CHECK(next_free_slot(used, 1) == 4);
        CHECK(next_free_slot(used, 10) == 1);
        used[0] = true;  // Auto never counts
        for (int s = 4; s <= kSlots; ++s) used[size_t(s)] = true;
        CHECK(next_free_slot(used, 5) == 1);
        used[1] = true;
        CHECK(next_free_slot(used, 5) == -1);
    }

    // Quick save / load default to the Auto slot; F6 / F7 cycle Auto, 1..10, Auto.
    CHECK(kDefaultSlot == kAutoSlot);
    CHECK(step_slot(kAutoSlot, +1) == 1);
    CHECK(step_slot(kSlots, +1) == kAutoSlot);
    CHECK(step_slot(kAutoSlot, -1) == kSlots);
    CHECK(step_slot(1, -1) == kAutoSlot);
    CHECK(step_slot(5, +1) == 6);
    {
        std::vector<bool> used(kSlots + 1, false);
        CHECK(next_free_slot(used, kAutoSlot) == 1);  // "Save to new slot" from Auto starts at slot 1
    }

    fs::path dir = fs::temp_directory_path() / "supersaga_states_test";
    fs::remove_all(dir);
    const std::string sha = "0123456789abcdef0123456789abcdef01234567";
    auto machine_heap = std::make_unique<supersaga::gba::Gba>(std::vector<std::uint8_t>(), std::filesystem::path{});  // >500 KB: never on the stack
    auto& machine = *machine_heap;  // blank machine, nothing executed
    {
        Store store(dir / "states", sha, dir / "saves" / "x.sav");
        CHECK(!store.info(kAutoSlot).exists);
        std::string err;
        CHECK(store.save(kAutoSlot, machine, &err));
        CHECK(store.save(3, machine, &err, "boss"));
        CHECK(store.info(kAutoSlot).exists && store.info(kAutoSlot).error.empty());
        CHECK(store.info(3).label == "boss");
        CHECK(store.rename(3, "before boss", &err) && store.info(3).label == "before boss");
        auto other_heap = std::make_unique<supersaga::gba::Gba>(std::vector<std::uint8_t>(), std::filesystem::path{});  // >500 KB: never on the stack
        auto& other = *other_heap;
        CHECK(store.load(kAutoSlot, other, &err));
        CHECK(store.can_undo_load());
    }
    {
        // Same files seen from another ROM: refused, so auto-load falls back to a normal boot.
        Store foreign(dir / "states", "ffffffffffffffffffffffffffffffffffffffff", dir / "saves" / "x.sav");
        CHECK(!foreign.info(kAutoSlot).error.empty());
        bool usable = foreign.info(kAutoSlot).exists && foreign.info(kAutoSlot).error.empty();
        CHECK(choose_startup(true, false, true, foreign.info(kAutoSlot).exists, usable, false) ==
              Startup::LauncherWithError);
    }
    {
        // A damaged Auto state is reported as unusable.
        Store store(dir / "states", sha, dir / "saves" / "x.sav");
        std::ofstream(store.file(kAutoSlot), std::ios::binary | std::ios::trunc) << "SuperSagaSTAT garbage";
        store.refresh();
        CHECK(store.info(kAutoSlot).exists && !store.info(kAutoSlot).error.empty());
        auto m2_heap = std::make_unique<supersaga::gba::Gba>(std::vector<std::uint8_t>(), std::filesystem::path{});  // >500 KB: never on the stack
        auto& m2 = *m2_heap;
        std::string err;
        CHECK(!store.load(kAutoSlot, m2, &err) && !err.empty());
    }
    fs::remove_all(dir);
    if (failures) { std::fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
    std::printf("states tests passed\n");
    return 0;
}
