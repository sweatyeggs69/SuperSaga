#pragma once
// Platform helpers for the launcher: ROM verification, save location and shortcuts.

#include <filesystem>
#include <string>

#include "app/fsutil.h"  // move_to_trash, utf8 paths
#include "rom.h"

namespace supersaga {

class Config;

struct VerifiedRom {
    bool ok = false;
    std::string error;  // user-facing reason when !ok
    std::filesystem::path path;
    Rom rom;
};

// Loads and verifies a ROM file (must be the dump described in game.h).
VerifiedRom verify_rom(const std::filesystem::path& path);

// <pref dir>/saves/<sha1>.sav. On first use, copies an existing "<rom>.sav" next to the ROM
// (the location older builds used) so progress carries over; the original is left in place.
std::filesystem::path save_path_for(const Config& config, const VerifiedRom& rom);

// Absolute path of the running executable.
std::filesystem::path executable_path();

enum class ShortcutPlace { Applications, Desktop };
enum class ShortcutState { None, Ours, Foreign };
struct ShortcutResult {
    bool ok = false;
    bool needs_confirm = false;  // a same-named item exists that SuperSaga did not create
    std::string message;
    std::filesystem::path path;
};
// One fixed shortcut per place, named and drawn like the app (SuperSaga, with its icon):
//   macOS:   "<~/Applications | ~/Desktop>/SuperSaga.app" (tiny bundle, marked with
//            CFBundleIdentifier tech.mcclain.supersaga.shortcut, running this app with --play)
//   Linux:   supersaga-play.desktop in ~/.local/share/applications or ~/Desktop
//   Windows: "SuperSaga.lnk" on the Desktop or in the Start menu Programs folder
// Older builds named the macOS/Windows shortcut after the game (<game::kFileSafeTitle>); such a
// shortcut counts as ours and is moved to the Trash when the new one is created or removed.
std::filesystem::path shortcut_path(ShortcutPlace place);
ShortcutState shortcut_state(ShortcutPlace place);
// Creates the shortcut, or updates ours in place to point at the running app. If a same-named
// item exists that is not ours, nothing is changed unless replace_foreign is true (it is then
// moved to the Trash first).
ShortcutResult create_shortcut(ShortcutPlace place, bool replace_foreign = false);
// Moves our shortcut to the Trash. Foreign items are never touched.
ShortcutResult remove_shortcut(ShortcutPlace place);

}  // namespace supersaga
