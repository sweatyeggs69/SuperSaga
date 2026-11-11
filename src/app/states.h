#pragma once
// Save state slots: <pref>/states/<rom sha1>/slot-1..10.ssst with a .png thumbnail next to
// each. A .ssst holds a small header (ROM SHA-1, build, time, playtime, label), a 240x160
// thumbnail and the compressed emulator state. No SDL dependency.

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace supersaga::gba {
class Gba;
}

namespace supersaga::states {

inline constexpr int kSlots = 10;
inline constexpr int kFirstSlot = 1;  // slots are numbered 1..kSlots

struct SlotInfo {
    int slot = 0;
    bool exists = false;
    std::string error;            // set if the file exists but can't be used
    std::time_t saved_at = 0;
    std::uint64_t playtime_frames = 0;
    std::string label;
    std::vector<std::uint16_t> thumb;  // 240x160 BGR555, empty if none
};

std::string slot_name(int slot);  // "Slot 3"
std::string format_playtime(std::uint64_t frames);

class Store {
public:
    // dir = <pref>/states/<rom sha1>. Moves a state left in the old Auto slot into Slot 1 (see
    // migrate_auto_slot).
    Store(std::filesystem::path dir, std::string rom_sha1, std::filesystem::path sav_path);

    // How many numbered slots moved up by one when the old Auto state was moved into Slot 1
    // (0 if there was nothing to move). Slots 1..shifted() are now 2..shifted()+1.
    int shifted() const { return shifted_; }

    // The usable slot saved most recently, or -1 if none. Continue and auto-load use it.
    int latest();

    const std::filesystem::path& dir() const { return dir_; }
    std::filesystem::path file(int slot) const;

    // Cached info; refreshed after every change made through this object.
    const SlotInfo& info(int slot);
    void refresh();

    // Saves the machine into a slot (keeps the previous file as .bak). label empty = keep old.
    bool save(int slot, const gba::Gba& gba, std::string* error, const std::string& label = {});
    // Loads a slot. Takes an in-memory "undo" snapshot of the current machine first and backs
    // up the battery save (.sav) so in-game progress can be recovered.
    bool load(int slot, gba::Gba& gba, std::string* error);
    bool can_undo_load() const { return undo_.has_value(); }
    bool undo_load(gba::Gba& gba, std::string* error);

    bool remove(int slot, std::string* error);  // moves the slot's files to the Trash
    bool rename(int slot, const std::string& label, std::string* error);
    bool export_slot(int slot, const std::filesystem::path& to, std::string* error);
    bool import_into(int slot, const std::filesystem::path& from, std::string* error);

private:
    std::filesystem::path dir_;
    std::string sha1_;
    std::filesystem::path sav_;
    std::vector<SlotInfo> cache_;
    bool cache_valid_ = false;
    int shifted_ = 0;
    std::optional<std::vector<std::uint8_t>> undo_;
};

// States from before the Auto slot was removed: if <dir>/auto.<ext> exists it becomes Slot 1.
// Slots 1, 2, ... up to the first empty one each move up by one to make room; if all kSlots are
// full, the last one goes to the Trash. Returns how many numbered slots moved (0 if no Auto state).
int migrate_auto_slot(const std::filesystem::path& dir, const std::string& ext);

// What to do when the app starts. With "Auto-load save state on launch" (states.autoload) on,
// opening SuperSaga goes straight into the game: into the most recent save state if there is one,
// otherwise a normal boot. If states exist but none can be used (other ROM, other build, damaged)
// the launcher shows with an explanation. Shift held at launch shows the launcher once.
enum class Startup { Launcher, BootGame, Resume, LauncherWithError };
inline Startup choose_startup(bool rom_ok, bool play_flag, bool autoload, bool any_state, bool usable_state,
                              bool shift) {
    if (!rom_ok) return (play_flag || autoload) ? Startup::LauncherWithError : Startup::Launcher;
    if (shift) return Startup::Launcher;
    if (autoload) {
        if (usable_state) return Startup::Resume;
        return any_state ? Startup::LauncherWithError : Startup::BootGame;
    }
    return play_flag ? Startup::BootGame : Startup::Launcher;
}

// The empty numbered slot that "Save to new slot" uses: the first one after `current`, wrapping
// around 1..kSlots. used[i] says whether slot i (1-based; index 0 is unused) holds a
// state. Returns -1 when every numbered slot is taken.
inline int next_free_slot(const std::vector<bool>& used, int current) {
    for (int k = 1; k <= kSlots; ++k) {
        int s = (current - 1 + k) % kSlots + 1;
        if (s >= int(used.size()) || !used[size_t(s)]) return s;
    }
    return -1;
}

// The slot the quick save / quick load shortcuts (and the save on quit) use until the user picks
// another one.
inline constexpr int kDefaultSlot = kFirstSlot;

// Previous / next slot (F6 / F7): cycles 1..kSlots, then back to 1.
inline int step_slot(int current, int delta) {
    return ((current - 1 + delta) % kSlots + kSlots) % kSlots + 1;
}

// What the Save state shortcut does (states.quick_save): overwrite the current slot, or save
// into the next empty slot and make that the current one.
enum class QuickSave { Overwrite, NewSlot };

// Hotkey actions (Page Up quick save, F9 save to new slot, Page Down quick load, F6/F7 previous/next slot by
// default). The app registers the handlers; input code (rebindable hotkeys) calls these.
struct Hotkeys {
    std::function<void()> quick_save, quick_load;
    std::function<void(int)> slot_step;
    std::function<void()> save_new_slot;
};
void set_hotkeys(Hotkeys h);
void quick_save();
void save_new_slot();
void quick_load();
void slot_step(int delta);

}  // namespace supersaga::states
