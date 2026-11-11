#pragma once
// RetroAchievements (https://retroachievements.org) through rcheevos' rc_client: sign-in,
// achievement unlocks, leaderboards, rich presence, unlock toasts and the "Achievements" tab of
// the settings menu.
//
// Settings (supersaga::Config): ra.enabled, ra.user, ra.hardcore, ra.notify_unlocks,
// ra.notify_progress, ra.notify_leaderboards. The login token (never the password) is kept by
// app/credentials. Badge images are cached in <pref dir>/retroachievements/badges/.
//
// The game is identified by the MD5 of the ROM, computed locally; the ROM itself never leaves
// the computer.

#include <SDL3/SDL.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace supersaga {
class Config;
namespace gba {
class Gba;
}

class Achievements {
public:
    Achievements(Config& config, SDL_Renderer* renderer);
    ~Achievements();
    Achievements(const Achievements&) = delete;
    Achievements& operator=(const Achievements&) = delete;

    // A game session started from power-on (a new supersaga::gba::Gba for play, never the launcher
    // backdrop). rom: the verified ROM bytes, hashed here.
    void game_started(const gba::Gba& gba, const std::vector<std::uint8_t>& rom);
    // The game session's Gba is about to be destroyed.
    void game_closed();
    // After every emulated game frame.
    void frame(const gba::Gba& gba);
    // Once per refresh (launcher, settings menu, paused, or after game frames). game: the running
    // game session's Gba, or nullptr, so replies handled here can read its memory.
    void idle(const gba::Gba* game);
    // A save state (or Undo load) replaced the game's memory: achievement progress restarts.
    void state_loaded();
    // In hardcore mode loading save states is not allowed; why says so in a sentence.
    bool can_load_state(std::string* why = nullptr) const;
    bool hardcore() const;
    // Called when the game must restart from power-on (hardcore was switched on mid-game).
    void set_restart_handler(std::function<void()> restart);

    // Draws the "Achievements" settings tab.
    void draw_tab();

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

// MD5 of data as 32 lowercase hex digits (the RetroAchievements hash for GBA ROMs).
std::string ra_md5_hex(const std::uint8_t* data, std::size_t size);
// Reads emulated memory at a RetroAchievements GBA address (0x00000 IWRAM, 0x08000 EWRAM,
// 0x48000 cartridge SRAM, up to 0x57FFF). Returns the number of bytes read. With no gba (no game
// frame is running) addresses in the map read as 0: rcheevos probes every address once when a game's
// achievements arrive, and a 0-byte answer there marks the achievement Unsupported for good.
std::uint32_t ra_read_memory(const gba::Gba* gba, std::uint32_t address, std::uint8_t* buffer,
                             std::uint32_t num_bytes);

}  // namespace supersaga
