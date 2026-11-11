#pragma once
// Windowed application: first-run ROM setup, themed launcher, options, shortcuts and the game.

#include <filesystem>
#include <string>

namespace supersaga {

struct AppOptions {
    std::filesystem::path rom;          // play this ROM directly (developer/CLI use)
    std::filesystem::path bios;         // override bios.path from the settings
    std::filesystem::path pref_dir;     // override the settings/saves folder (testing)
    std::filesystem::path select_rom;   // feed this path to the ROM picker (testing, skips the dialog)
    std::filesystem::path dump_frame;   // write the last game frame to a BMP on exit
    std::filesystem::path screenshot;   // save the window (as shown) on the last --frames frame
    bool play = false;                  // --play: skip the launcher, start the configured ROM
    bool test_pattern = false;
    bool verbose = false;
    bool mute = false;                  // open no audio device (testing)
    bool silent_audio = false;          // open the device but at zero gain (testing audio sync)
    bool open_settings = false;         // open the settings menu at start (testing)
    std::string open_tab;               // with open_settings: "states" shows the States tab first
    std::string state_script;           // "frame:s3,frame:l3" save/load slot at game frames (testing)
    long frames = -1;                   // quit after this many frames
    int window_w = 0, window_h = 0;     // initial window size (0 = default)
    std::string input_script;           // "frame:hexkeys,..." merged with real input in game
};

int run_app(const AppOptions& options);

}  // namespace supersaga
