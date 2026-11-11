// Save state slots without the game: Store save/load/rename/remove on a blank machine, auto-load
// decision, fallback when no state is usable, and moving the old Auto state into Slot 1. No ROM
// or game data needed.
#include <cstdio>
#include <filesystem>
#include <memory>
#include <filesystem>
#include <fstream>
#include <iterator>
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
    // Startup decision table: rom_ok, --play, autoload, any state, a usable state, shift.
    CHECK(choose_startup(true, false, false, true, true, false) == Startup::Launcher);
    CHECK(choose_startup(true, true, false, true, true, false) == Startup::BootGame);       // --play only
    CHECK(choose_startup(true, false, true, true, true, false) == Startup::Resume);         // autoload
    CHECK(choose_startup(true, true, true, true, true, false) == Startup::Resume);          // shortcut + autoload
    CHECK(choose_startup(true, false, true, false, false, false) == Startup::BootGame);     // no state yet
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
        used[0] = true;  // index 0 is not a slot
        for (int s = 4; s <= kSlots; ++s) used[size_t(s)] = true;
        CHECK(next_free_slot(used, 5) == 1);
        used[1] = true;
        CHECK(next_free_slot(used, 5) == -1);
    }

    // Quick save / load default to Slot 1; F6 / F7 cycle 1..10, then back to 1.
    CHECK(kDefaultSlot == 1);
    CHECK(step_slot(1, +1) == 2);
    CHECK(step_slot(kSlots, +1) == 1);
    CHECK(step_slot(1, -1) == kSlots);
    CHECK(step_slot(5, +1) == 6);
    {
        std::vector<bool> used(kSlots + 1, false);
        used[1] = true;
        CHECK(next_free_slot(used, kDefaultSlot) == 2);  // "Save to new slot" from Slot 1 goes to Slot 2
    }

    fs::path dir = fs::temp_directory_path() / "supersaga_states_test";
    fs::remove_all(dir);
    const std::string sha = "0123456789abcdef0123456789abcdef01234567";
    auto machine_heap = std::make_unique<supersaga::gba::Gba>(std::vector<std::uint8_t>(), std::filesystem::path{});  // >500 KB: never on the stack
    auto& machine = *machine_heap;  // blank machine, nothing executed
    {
        Store store(dir / "states", sha, dir / "saves" / "x.sav");
        CHECK(!store.info(1).exists);
        std::string err;
        CHECK(store.save(1, machine, &err));
        CHECK(store.save(3, machine, &err, "boss"));
        CHECK(store.info(1).exists && store.info(1).error.empty());
        CHECK(store.info(3).label == "boss");
        CHECK(store.rename(3, "before boss", &err) && store.info(3).label == "before boss");
        auto other_heap = std::make_unique<supersaga::gba::Gba>(std::vector<std::uint8_t>(), std::filesystem::path{});  // >500 KB: never on the stack
        auto& other = *other_heap;
        CHECK(store.load(1, other, &err));
        CHECK(store.can_undo_load());
    }
    {
        // Same files seen from another ROM: refused, so auto-load falls back to a normal boot.
        Store foreign(dir / "states", "ffffffffffffffffffffffffffffffffffffffff", dir / "saves" / "x.sav");
        CHECK(!foreign.info(1).error.empty());
        bool usable = foreign.info(1).exists && foreign.info(1).error.empty();
        CHECK(choose_startup(true, false, true, foreign.info(1).exists, usable, false) ==
              Startup::LauncherWithError);
    }
    {
        // A damaged state is reported as unusable.
        Store store(dir / "states", sha, dir / "saves" / "x.sav");
        std::ofstream(store.file(1), std::ios::binary | std::ios::trunc) << "SuperSagaSTAT garbage";
        store.refresh();
        CHECK(store.info(1).exists && !store.info(1).error.empty());
        auto m2_heap = std::make_unique<supersaga::gba::Gba>(std::vector<std::uint8_t>(), std::filesystem::path{});  // >500 KB: never on the stack
        auto& m2 = *m2_heap;
        std::string err;
        CHECK(!store.load(1, m2, &err) && !err.empty());
    }
    {
        // The old Auto state becomes Slot 1; slots 1..2 move up to 2..3, slot 5 (after the gap) stays.
        fs::path d = dir / "migrate";
        fs::create_directories(d);
        auto put = [&](const std::string& name, const std::string& text) { std::ofstream(d / name, std::ios::binary) << text; };
        auto get = [&](const std::string& name) {
            std::ifstream f(d / name, std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        };
        put("auto.ssst", "A"); put("auto.png", "Apng");
        put("slot-1.ssst", "1"); put("slot-1.png", "1png"); put("slot-1.bak", "1bak");
        put("slot-2.ssst", "2");
        put("slot-3.png", "stale");  // thumbnail left without a state
        put("slot-5.ssst", "5");
        CHECK(migrate_auto_slot(d, "ssst") == 2);
        CHECK(!fs::exists(d / "auto.ssst") && !fs::exists(d / "auto.png"));
        CHECK(get("slot-1.ssst") == "A" && get("slot-1.png") == "Apng" && !fs::exists(d / "slot-1.bak"));
        CHECK(get("slot-2.ssst") == "1" && get("slot-2.png") == "1png" && get("slot-2.bak") == "1bak");
        CHECK(get("slot-3.ssst") == "2" && !fs::exists(d / "slot-3.png"));
        CHECK(!fs::exists(d / "slot-4.ssst"));
        CHECK(get("slot-5.ssst") == "5");
        CHECK(migrate_auto_slot(d, "ssst") == 0);  // nothing left to move
        fs::remove_all(d);
        // No numbered states: Auto simply becomes Slot 1.
        fs::create_directories(d);
        put("auto.ssst", "A");
        CHECK(migrate_auto_slot(d, "ssst") == 0);
        CHECK(get("slot-1.ssst") == "A");
        fs::remove_all(d);
    }
    {
        // Store picks the most recent usable state for Continue / auto-load.
        Store store(dir / "latest", sha, dir / "saves" / "x.sav");
        CHECK(store.latest() == -1);
        std::string err;
        CHECK(store.save(4, machine, &err));
        CHECK(store.latest() == 4);
    }
    fs::remove_all(dir);
    if (failures) { std::fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
    std::printf("states tests passed\n");
    return 0;
}
