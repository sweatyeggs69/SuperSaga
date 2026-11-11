// SuperSaga entry point. With a window it starts the launcher app (src/app/app.cpp): first-run ROM
// setup, themed menu, options and the game. --headless runs the core without a window for
// scripted checks.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "app/app.h"
#include "app/bmp.h"
#include "app/config.h"
#include "app/fsutil.h"
#include "app/licenses.h"
#include "app/platform.h"
#include "app/save_import.h"
#include "gba/gba.h"
#include "rom.h"
#include "sha1.h"
#include "update/updater.h"

namespace {
constexpr const char* kRetailBiosSha1 = "300c20df6731a33952ded8c436f7f186d25d3492";

void fail(const std::string& msg, bool gui) {
    std::fprintf(stderr, "supersaga: %s\n", msg.c_str());
    if (gui) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "SuperSaga", msg.c_str(), nullptr);
}

struct Options {
    std::filesystem::path rom, bios, dump_frame, dump_audio, screenshot, pref_dir, select_rom;
    bool test_pattern = false, headless = false, verbose = false, play = false;
    std::string create_shortcut;  // "applications" or "desktop"
    bool remove_shortcut = false;
    bool open_settings = false;
    std::string open_tab, state_script;
    bool mute = false, silent_audio = false;
    bool prepare = false;  // --prepare: check the ROM and remember it, without a window
    int window_w = 0, window_h = 0;
    std::filesystem::path import_save, export_save;
    long frames = -1;  // stop after this many frames (-1: run until closed)
    std::string update;  // "check" or "install" (--check-update / --update-now)
    std::string script;  // headless input script: "frame:keys,frame:keys" (keys as hex mask)
};

void usage() {
    std::printf(
        "usage: supersaga [rom.gba] [options]\n"
        "  (no ROM)           open the launcher; a ROM path starts that ROM directly\n"
        "  --play             start the ROM chosen in the launcher, skipping the menu\n"
        "  --pref-dir <dir>   use another settings/saves folder\n"
        "  --select-rom <f>   act as if <f> was picked in the ROM dialog (testing)\n"
        "  --create-shortcut applications|desktop  create or update the --play shortcut and exit\n"
        "  --remove-shortcut applications|desktop  move SuperSaga's shortcut to the Trash and exit\n"
        "  --import-save <f>  import a .sav/.srm from another emulator (backs up the current save)\n"
        "  --export-save <f>  write the current save as a raw .sav\n"
        "  --test-pattern     show the SDL test pattern instead of running the game\n"
        "  --bios <file>      use your own GBA BIOS dump instead of the built-in BIOS HLE\n"
        "  --frames <n>       quit after n frames\n"
        "  --headless         run without a window (with --frames / --dump-frame)\n"
        "  --dump-frame <bmp> write the last frame to a BMP file on exit\n"
        "  --dump-audio <wav> write headless audio to a WAV file\n"
        "  --input <script>   scripted input, e.g. 300:8,310:0 (game frame:hex key mask); also in --play\n"
        "  --check-update     ask GitHub for the latest release, print the result and exit\n"
        "  --update-now       download, verify and install the latest release, then exit\n"
        "  --verbose          log unimplemented BIOS calls\n"
        "  --console          (Windows) open a console window for log output\n"
        "  --window WxH       initial window size, e.g. 1280x800\n"
        "  --licenses         print the license texts (SuperSaga, third-party notices, fonts)\n"
        "  --mute             no sound (opens no audio device; settings are not changed)\n"
        "  --prepare --game <rom>  check the ROM and remember it as the launcher's ROM, without a\n"
        "                     window; prints \"progress <0..1> <text>\" lines (for RecompLauncher)\n"
        "  --screenshot <bmp> save the window as shown (with display options) on the last --frames frame\n"
        "default keys: arrows = D-pad, X = A, Z = B, A = L, S = R, Enter = Start,\n"
        "      Backspace/Right Shift = Select (remap them in the settings menu)\n"
        "F1: settings menu (game, display, controls)   Esc: settings menu in game\n"
        "F11: fullscreen\n");
}
}  // namespace

#if defined(_WIN32)
// SuperSaga.exe is a GUI program (no console window). When started from a terminal, print to it;
// --console opens a new console window for logs.
void attach_console(bool force) {
    if (AttachConsole(ATTACH_PARENT_PROCESS) || (force && AllocConsole())) {
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
    }
}
#endif

int main(int argc, char** argv) {
#if defined(_WIN32)
    bool force_console = false, prepare = false;
    for (int i = 1; i < argc; ++i) {
        force_console |= std::string(argv[i]) == "--console";
        prepare |= std::string(argv[i]) == "--prepare";
    }
    if (!prepare) attach_console(force_console);  // --prepare output goes to the caller's pipe
#endif
    Options opt;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        auto next_path = [&]() { return supersaga::utf8_path(next()); };
        if (a == "--test-pattern") opt.test_pattern = true;
        else if (a == "--console") {}
        else if (a == "--bios") opt.bios = next_path();
        else if (a == "--frames") opt.frames = std::strtol(next().c_str(), nullptr, 10);
        else if (a == "--headless") opt.headless = true;
        else if (a == "--dump-frame") opt.dump_frame = next_path();
        else if (a == "--input") opt.script = next();
        else if (a == "--dump-audio") opt.dump_audio = next_path();
        else if (a == "--verbose") opt.verbose = true;
        else if (a == "--screenshot") opt.screenshot = next_path();
        else if (a == "--play") opt.play = true;
        else if (a == "--pref-dir") opt.pref_dir = next_path();
        else if (a == "--select-rom") opt.select_rom = next_path();
        else if (a == "--create-shortcut") opt.create_shortcut = next();
        else if (a == "--remove-shortcut") { opt.create_shortcut = next(); opt.remove_shortcut = true; }
        else if (a == "--open-settings") {
            opt.open_settings = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') opt.open_tab = argv[++i];
        }
        else if (a == "--state-script") opt.state_script = next();
        else if (a == "--mute") opt.mute = true;
        else if (a == "--prepare") opt.prepare = true;
        else if (a == "--game") opt.rom = next_path();
        else if (a == "--window") std::sscanf(next().c_str(), "%dx%d", &opt.window_w, &opt.window_h);
        else if (a == "--check-update") opt.update = "check";
        else if (a == "--update-now") opt.update = "install";
        else if (a == "--silent-audio") opt.silent_audio = true;
        else if (a == "--import-save") opt.import_save = next_path();
        else if (a == "--export-save") opt.export_save = next_path();
        else if (a.rfind("-psn_", 0) == 0) {}  // added by older macOS launch services
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else if (a == "--licenses") { supersaga::licenses::print_all(); return 0; }
        else opt.rom = supersaga::utf8_path(a);
    }
    bool gui = !opt.headless;

    // Setup step for RecompLauncher: the same check and settings as picking the ROM in the
    // launcher. Exit codes: 0 ok, 2 file not found, 3 not the supported dump.
    if (opt.prepare) {
        auto progress = [](const char* p, const char* text) {
            std::printf("progress %s %s\n", p, text);
            std::fflush(stdout);
        };
        progress("0.1", "Checking your ROM");
        std::error_code ec;
        if (opt.rom.empty() || !std::filesystem::exists(opt.rom, ec)) {
            fail("The ROM file could not be found: " + opt.rom.string(), false);
            return 2;
        }
        supersaga::VerifiedRom v = supersaga::verify_rom(opt.rom);
        if (!v.ok) {
            fail(v.error, false);
            return 3;
        }
        progress("0.9", "Saving your ROM choice");
        supersaga::Config config(opt.pref_dir);
        config.set_string("rom.path", supersaga::path_utf8(v.path));
        config.set_string("rom.sha1", v.rom.sha1);
        progress("1", "Ready");
        return 0;
    }

    if (!opt.import_save.empty() || !opt.export_save.empty()) {
        supersaga::Config config(opt.pref_dir);
        std::filesystem::path rp = !opt.rom.empty() ? opt.rom : supersaga::utf8_path(config.get_string("rom.path"));
        supersaga::VerifiedRom v = supersaga::verify_rom(rp);
        if (!v.ok) {
            fail(rp.empty() ? "No ROM is configured; choose it in the launcher first." : v.error, false);
            return 1;
        }
        std::filesystem::path save = supersaga::save_path_for(config, v);
        if (!opt.export_save.empty()) {
            std::vector<std::uint8_t> data;
            if (!supersaga::read_file(save, data)) { fail("There is no save to export yet.", false); return 1; }
            data.resize(supersaga::kSramSize, 0xFF);
            if (!supersaga::write_file_atomic(opt.export_save, data)) { fail("Could not write " + opt.export_save.string(), false); return 1; }
            std::printf("Exported %s -> %s\n", save.string().c_str(), opt.export_save.string().c_str());
        }
        if (!opt.import_save.empty()) {
            std::vector<std::uint8_t> bytes;
            if (!supersaga::read_file(opt.import_save, bytes)) { fail("Could not read " + opt.import_save.string(), false); return 1; }
            supersaga::NormalizedSave n = supersaga::normalize_save(bytes, opt.import_save.filename().string());
            if (!n.ok) { fail(n.error, false); return 1; }
            std::filesystem::path backup = supersaga::backup_save(save, save.parent_path() / "backups");
            if (!supersaga::write_file_atomic(save, n.data)) { fail("Could not write " + save.string(), false); return 1; }
            std::printf("Imported %s (%s) -> %s\n", opt.import_save.string().c_str(), n.format.c_str(), save.string().c_str());
            if (!backup.empty()) std::printf("Previous save backed up to %s\n", backup.string().c_str());
            if (!n.warning.empty()) std::printf("Note: %s\n", n.warning.c_str());
        }
        return 0;
    }

    if (!opt.update.empty()) {
        using supersaga::update::State;
        supersaga::update::Updater u(SUPERSAGA_VERSION);
        auto wait = [&] {
            while (u.busy()) SDL_Delay(50);
            return u.status();
        };
        std::printf("SuperSaga %s\n", SUPERSAGA_VERSION);
        u.check(true);
        supersaga::update::Status st = wait();
        if (st.state == State::Failed) { fail(st.message, false); return 1; }
        if (st.state == State::UpToDate) {
            std::printf("%s\n", st.latest.empty() ? st.message.c_str() : ("Up to date (latest release: " + st.latest + ").").c_str());
            return 0;
        }
        std::printf("Update available: %s (%s)\n%s\n", st.latest.c_str(), st.asset.empty() ? "no download for this computer" : st.asset.c_str(),
                    st.page_url.c_str());
        if (!st.message.empty()) std::printf("%s\n", st.message.c_str());
        if (!st.can_install) std::printf("%s\n", st.install_note.c_str());
        if (opt.update != "install") return 0;
        if (!st.can_install || !st.message.empty()) return 1;
        u.download();
        st = wait();
        if (st.state != State::Ready) { fail(st.message, false); return 1; }
        std::string err;
        if (!u.install(&err)) { fail(err, false); return 1; }
        std::printf("Installed SuperSaga %s. Open SuperSaga again to use it.\n", st.latest.c_str());
        return 0;
    }

    if (!opt.create_shortcut.empty()) {
        auto place = opt.create_shortcut == "desktop" ? supersaga::ShortcutPlace::Desktop : supersaga::ShortcutPlace::Applications;
        supersaga::ShortcutResult r = opt.remove_shortcut ? supersaga::remove_shortcut(place) : supersaga::create_shortcut(place);
        std::printf("%s\n", r.message.c_str());
        return r.ok ? 0 : 1;
    }

    // App name for the macOS app menu (About/Hide/Quit SuperSaga), audio mixers, and on Linux the
    // window class / Wayland app id, so taskbars say "SuperSaga" instead of the executable name.
    SDL_SetAppMetadata("SuperSaga", nullptr, nullptr);
#if !defined(_WIN32) && !defined(__APPLE__)
    SDL_SetHint(SDL_HINT_APP_ID, "SuperSaga");
#endif

    SDL_InitFlags flags = gui ? (SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD) : 0;
    if (!SDL_Init(flags)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    if (gui) {
        supersaga::AppOptions app;
        app.rom = opt.rom;
        app.bios = opt.bios;
        app.pref_dir = opt.pref_dir;
        app.select_rom = opt.select_rom;
        app.dump_frame = opt.dump_frame;
        app.screenshot = opt.screenshot;
        app.play = opt.play;
        app.test_pattern = opt.test_pattern;
        app.verbose = opt.verbose;
        app.frames = opt.frames;
        app.open_settings = opt.open_settings;
        app.open_tab = opt.open_tab;
        app.state_script = opt.state_script;
        app.input_script = opt.script;
        app.mute = opt.mute;
        app.window_w = opt.window_w;
        app.window_h = opt.window_h;
        app.silent_audio = opt.silent_audio;
        int rc = supersaga::run_app(app);
        SDL_Quit();
        return rc;
    }

    // ---- headless ----------------------------------------------------------------------------
    supersaga::Config config(opt.pref_dir);
    const char* base = SDL_GetBasePath();
    std::filesystem::path rom_path = !opt.rom.empty() ? opt.rom : supersaga::find_default_rom(base ? base : ".");
    if (rom_path.empty()) rom_path = supersaga::utf8_path(config.get_string("rom.path"));
    if (rom_path.empty()) {
        fail(std::string("No ROM found. Pass the path to your own ") + supersaga::game::kTitle + " dump, "
             "or put the .gba file in a 'rom' folder next to the game.", gui);
        SDL_Quit();
        return 1;
    }

    auto loaded = supersaga::load_rom(rom_path);
    if (!loaded.ok) {
        fail("Could not load " + rom_path.string() + ": " + loaded.error, gui);
        SDL_Quit();
        return 1;
    }
    const auto& rom = loaded.rom;
    std::printf("ROM: %s [%s] rev %u\nSHA-1: %s (%s)\n", rom.header.title.c_str(),
                rom.header.game_code.c_str(), rom.header.version, rom.sha1.c_str(),
                !rom.hash_matches ? "UNKNOWN DUMP" : rom.header_only ? "header match, fingerprint not pinned" : "verified");
    if (!rom.hash_matches) {
        fail(std::string("This ROM is not the expected dump of ") + supersaga::game::kTitleWithRegion + ".\n"
             "Expected SHA-1 " + std::string(supersaga::kExpectedSha1) + "\nGot " + rom.sha1, gui);
        SDL_Quit();
        return 1;
    }

    supersaga::VerifiedRom verified;
    verified.ok = true;
    verified.path = rom_path;
    verified.rom = rom;
    auto gba = std::make_unique<supersaga::gba::Gba>(rom.data, supersaga::save_path_for(config, verified));
    gba->verbose = opt.verbose;
    if (!opt.bios.empty()) {
        std::ifstream f(opt.bios, std::ios::binary);
        std::vector<std::uint8_t> bios((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::string hash = supersaga::to_hex(supersaga::sha1(bios.data(), bios.size()));
        if (bios.size() != 0x4000 || hash != kRetailBiosSha1) {
            fail("BIOS " + opt.bios.string() + " is not the expected GBA BIOS (SHA-1 " + hash + ")", gui);
            SDL_Quit();
            return 1;
        }
        gba->use_bios(std::move(bios));
        gba->reset();
        std::printf("BIOS: real BIOS dump (verified)\n");
    } else {
        std::printf("BIOS: built-in HLE\n");
    }

    // Parse the headless input script.
    std::vector<std::pair<long, std::uint16_t>> script;
    for (size_t p = 0; p < opt.script.size();) {
        size_t comma = opt.script.find(',', p);
        std::string item = opt.script.substr(p, comma == std::string::npos ? std::string::npos : comma - p);
        size_t colon = item.find(':');
        if (colon != std::string::npos)
            script.emplace_back(std::strtol(item.c_str(), nullptr, 10),
                                std::uint16_t(std::strtol(item.c_str() + colon + 1, nullptr, 16)));
        if (comma == std::string::npos) break;
        p = comma + 1;
    }

    {
        long frames = opt.frames < 0 ? 600 : opt.frames;
        size_t si = 0;
        std::vector<std::int16_t> audio, all_audio;
        std::uint16_t keys = 0;
        Uint64 t0 = SDL_GetTicksNS();
        for (long f = 0; f < frames; ++f) {
            while (si < script.size() && script[si].first <= f) keys = script[si++].second;
            gba->set_keys(keys);
            gba->run_frame();
            audio.clear();
            gba->take_audio(audio);
            if (!opt.dump_audio.empty()) all_audio.insert(all_audio.end(), audio.begin(), audio.end());
        }
        if (!opt.dump_audio.empty()) {
            std::ofstream w(opt.dump_audio, std::ios::binary);
            std::uint32_t bytes = std::uint32_t(all_audio.size() * 2), rate = supersaga::gba::kAudioRate;
            auto u32 = [&](std::uint32_t v) { w.write(reinterpret_cast<char*>(&v), 4); };
            auto u16 = [&](std::uint16_t v) { w.write(reinterpret_cast<char*>(&v), 2); };
            w.write("RIFF", 4); u32(36 + bytes); w.write("WAVEfmt ", 8); u32(16); u16(1); u16(2);
            u32(rate); u32(rate * 4); u16(4); u16(16); w.write("data", 4); u32(bytes);
            w.write(reinterpret_cast<const char*>(all_audio.data()), bytes);
        }
        double secs = double(SDL_GetTicksNS() - t0) / 1e9;
        std::printf("ran %ld frames in %.2f s (%.0f fps)\n%s\n", frames, secs, frames / secs,
                    gba->debug_state().c_str());
        if (!opt.dump_frame.empty() && !supersaga::write_bmp(opt.dump_frame.string(), gba->framebuffer()))
            fail("could not write " + opt.dump_frame.string(), false);
        gba->flush_save();
        SDL_Quit();
        return 0;
    }
}
