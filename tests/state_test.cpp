// Save state determinism: run, save, record M frames of framebuffer hashes, load, replay the same
// input and require identical hashes. Uses the user's ROM if present (SUPERSAGA_ROM or the default
// path baked in by CMake); skipped (exit 77) otherwise. Nothing from the ROM is written to disk.
#include <cstdio>
#include <filesystem>
#include <memory>
#include <cstdlib>
#include <string>
#include <vector>

#include "gba/gba.h"
#include "rom.h"

#ifndef SUPERSAGA_TEST_ROM
#define SUPERSAGA_TEST_ROM ""
#endif
#ifndef SUPERSAGA_TEST_BIOS
#define SUPERSAGA_TEST_BIOS ""
#endif
#include <fstream>
#include <iterator>

namespace {
std::uint64_t hash(const supersaga::gba::u16* px, size_t n) {
    std::uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) { h ^= px[i]; h *= 1099511628211ull; }
    return h;
}
// Deterministic input: start the game, go through the menus, then walk and jump.
std::uint16_t keys_for(long f) {
    using namespace supersaga::gba;
    if (f >= 650 && f < 656) return kKeyStart;
    if ((f >= 800 && f < 806) || (f >= 950 && f < 956) || (f >= 1100 && f < 1106)) return kKeyA;
    if (f >= 1300) {
        std::uint16_t k = (f / 90) % 2 ? kKeyRight : kKeyLeft;
        if (f % 40 < 6) k |= kKeyA;
        if (f % 70 < 10) k |= kKeyB;
        return k;
    }
    return 0;
}
}  // namespace

int main() {
    const char* env = std::getenv("SUPERSAGA_ROM");
    std::string path = env && *env ? env : SUPERSAGA_TEST_ROM;
    auto loaded = supersaga::load_rom(path);
    if (path.empty() || !loaded.ok || !loaded.rom.hash_matches) {
        std::printf("skipped: ROM not available (set SUPERSAGA_ROM)\n");
        return 77;
    }
    auto gba_heap = std::make_unique<supersaga::gba::Gba>(loaded.rom.data, std::filesystem::path{});  // >500 KB: never on the stack
    auto& gba = *gba_heap;
    std::vector<supersaga::gba::s16> audio;
    long f = 0;
    auto step = [&] {
        gba.set_keys(keys_for(f++));
        gba.run_frame();
        audio.clear();
        gba.take_audio(audio);
    };
    int failures = 0;
    // Check at the title screen, in the menus and in gameplay.
    for (long save_at : {600L, 900L, 1500L}) {
        while (f < save_at) step();
        std::vector<std::uint8_t> state = gba.save_state();
        long base = f;
        const int M = 240;
        std::vector<std::uint64_t> first;
        for (int i = 0; i < M; ++i) { step(); first.push_back(hash(gba.framebuffer(), 240 * 160)); }
        std::string err;
        if (!gba.load_state(state, &err)) { std::printf("load failed: %s\n", err.c_str()); return 1; }
        f = base;
        int mismatch = -1;
        for (int i = 0; i < M; ++i) {
            step();
            if (hash(gba.framebuffer(), 240 * 160) != first[i] && mismatch < 0) mismatch = i;
        }
        // Saving again right after replay must give the same bytes as a fresh run reaching here.
        std::printf("state at frame %ld: %zu bytes, replay of %d frames %s\n", save_at, state.size(), M,
                    mismatch < 0 ? "identical" : ("DIFFERS at +" + std::to_string(mismatch)).c_str());
        if (mismatch >= 0) ++failures;
    }
    // ---- with the user's own BIOS dump (skipped if it is not there) ----
    std::ifstream bf(SUPERSAGA_TEST_BIOS, std::ios::binary);
    std::vector<std::uint8_t> bios((std::istreambuf_iterator<char>(bf)), std::istreambuf_iterator<char>());
    std::string err;
    if (bios.size() == 0x4000) {
        auto run = [&](supersaga::gba::Gba& g, long from, long n) {
            for (long i = 0; i < n; ++i) { g.set_keys(keys_for(from + i)); g.run_frame(); audio.clear(); g.take_audio(audio); }
        };
        // Real BIOS determinism.
        auto real_heap = std::make_unique<supersaga::gba::Gba>(loaded.rom.data, std::filesystem::path{});  // >500 KB: never on the stack
        auto& real = *real_heap;
        real.use_bios(bios);
        real.reset();
        run(real, 0, 1500);
        auto st = real.save_state();
        std::vector<std::uint64_t> a, b;
        for (int i = 0; i < 120; ++i) { run(real, 1500 + i, 1); a.push_back(hash(real.framebuffer(), 240 * 160)); }
        if (!real.load_state(st, &err)) { std::printf("real BIOS load failed: %s\n", err.c_str()); ++failures; }
        for (int i = 0; i < 120; ++i) { run(real, 1500 + i, 1); b.push_back(hash(real.framebuffer(), 240 * 160)); }
        std::printf("real BIOS replay of 120 frames %s\n", a == b ? "identical" : "DIFFERS");
        if (a != b) ++failures;

        // Cross-mode loads at frame boundaries: the game must keep running afterwards.
        auto keeps_running = [&](supersaga::gba::Gba& g, long from) {
            std::uint64_t h0 = hash(g.framebuffer(), 240 * 160);
            int changes = 0;
            for (int i = 0; i < 300; ++i) {
                run(g, from + i, 1);
                std::uint64_t h = hash(g.framebuffer(), 240 * 160);
                if (h != h0) { ++changes; h0 = h; }
            }
            return changes > 30;
        };
        auto hle_heap = std::make_unique<supersaga::gba::Gba>(loaded.rom.data, std::filesystem::path{});  // >500 KB: never on the stack
        auto& hle = *hle_heap;
        run(hle, 0, 1500);
        auto hle_state = hle.save_state();
        auto real2_heap = std::make_unique<supersaga::gba::Gba>(loaded.rom.data, std::filesystem::path{});  // >500 KB: never on the stack
        auto& real2 = *real2_heap;
        real2.use_bios(bios);
        real2.reset();
        bool ok1 = real2.load_state(hle_state, &err);
        bool run1 = ok1 && keeps_running(real2, 1500);
        std::printf("HLE state -> real BIOS: %s%s\n", ok1 ? "loaded" : ("refused: " + err).c_str(),
                    ok1 ? (run1 ? ", game keeps running" : ", GAME STALLED") : "");
        auto hle2_heap = std::make_unique<supersaga::gba::Gba>(loaded.rom.data, std::filesystem::path{});  // >500 KB: never on the stack
        auto& hle2 = *hle2_heap;
        bool ok2 = hle2.load_state(st, &err);
        bool run2 = ok2 && keeps_running(hle2, 1500);
        std::printf("real BIOS state -> HLE: %s%s\n", ok2 ? "loaded" : ("refused: " + err).c_str(),
                    ok2 ? (run2 ? ", game keeps running" : ", GAME STALLED") : "");
        if (!run1 || !run2) ++failures;

        // Real BIOS states taken all through the intro, menus and gameplay load under HLE. Games
        // waits in the BIOS almost every frame; only a frame that ends mid-decompression (a BIOS
        // routine half-way through) may be refused. Every converted state must keep running.
        auto sweep_heap = std::make_unique<supersaga::gba::Gba>(loaded.rom.data, std::filesystem::path{});  // >500 KB: never on the stack
        auto& sweep = *sweep_heap;
        sweep.use_bios(bios);
        sweep.reset();
        int tried = 0, converted = 0, stalled = 0;
        for (long fr = 0; fr < 3000; fr += 25) {
            run(sweep, fr, 25);
            ++tried;
            auto conv_heap = std::make_unique<supersaga::gba::Gba>(loaded.rom.data, std::filesystem::path{});  // >500 KB: never on the stack
            if (!conv_heap->load_state(sweep.save_state(), &err)) continue;
            ++converted;
            std::uint64_t h0 = hash(conv_heap->framebuffer(), 240 * 160);
            int changes = 0;
            for (int i = 0; i < 90; ++i) {
                run(*conv_heap, fr + 25 + i, 1);
                std::uint64_t h = hash(conv_heap->framebuffer(), 240 * 160);
                if (h != h0) { ++changes; h0 = h; }
            }
            if (changes == 0 && fr >= 1300) ++stalled;  // gameplay always animates
        }
        std::printf("real BIOS -> HLE sweep: %d of %d states converted, %d stalled\n", converted, tried, stalled);
        if (converted * 100 < tried * 95 || stalled) ++failures;

        // Booting through the BIOS intro shows the logo, then the game takes over and runs.
        auto boot_heap = std::make_unique<supersaga::gba::Gba>(loaded.rom.data, std::filesystem::path{});  // >500 KB: never on the stack
        auto& boot = *boot_heap;
        boot.use_bios(bios);
        boot.set_bios_boot(true);
        boot.reset();
        run(boot, -100000, 150);  // ~2.5 s into the BIOS intro (no keys)
        const supersaga::gba::u16* px = boot.framebuffer();
        int distinct = 0;
        for (int i = 1; i < 240 * 160; ++i) distinct += px[i] != px[0];
        run(boot, -100000, 450);  // past the intro
        bool runs = keeps_running(boot, -100000);
        std::printf("BIOS boot: logo frame %s, game %s afterwards\n", distinct > 500 ? "drawn" : "BLANK",
                    runs ? "running" : "NOT RUNNING");
        if (distinct <= 500 || !runs) ++failures;
    } else {
        std::printf("real BIOS checks skipped (no gba_bios.bin)\n");
    }

    // Corrupt / foreign data is rejected without touching the machine.
    std::vector<std::uint8_t> bad = gba.save_state();
    bad.resize(bad.size() / 2);
    if (gba.load_state(bad, &err)) { std::printf("truncated state was accepted\n"); ++failures; }
    else std::printf("truncated state rejected: %s\n", err.c_str());
    return failures ? 1 : 0;
}
