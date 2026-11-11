#pragma once
// Cheats menu toggles (F1 > Cheats). After every game frame the host calls apply() on the game's
// EWRAM, which tops the chosen stats back up: HP and BP to each brother's current maximum, coins
// to 9999. Nothing is written while a maximum is 0 or out of range (no file loaded yet), so the
// title screen and menus are left alone. Off while RetroAchievements hardcore mode is on.
//
// Settings (supersaga::Config): cheats.hp, cheats.bp, cheats.coins (all off by default).
#include <cstddef>
#include <cstdint>
#include <span>

#include "game.h"

namespace supersaga::cheats {

struct Flags {
    bool hp = false;
    bool bp = false;
    bool coins = false;
    bool any() const { return hp || bp || coins; }
};

namespace detail {
inline constexpr std::uint32_t kEwram = 0x02000000;
inline constexpr std::uint16_t kMaxStat = 999;  // the game's own cap for HP and BP
inline std::uint16_t get16(std::span<const std::uint8_t> m, std::size_t at) {
    return std::uint16_t(m[at] | (m[at + 1] << 8));
}
inline void set16(std::span<std::uint8_t> m, std::size_t at, std::uint16_t v) {
    m[at] = std::uint8_t(v);
    m[at + 1] = std::uint8_t(v >> 8);
}
}  // namespace detail

// Applies the enabled cheats to ewram (the 256 KiB EWRAM, starting at 0x02000000). Returns true
// when anything changed.
inline bool apply(std::span<std::uint8_t> ewram, const Flags& f) {
    using namespace detail;
    const std::size_t mario = game::kMarioStatsAddress - kEwram, luigi = game::kLuigiStatsAddress - kEwram;
    const std::size_t coins = game::kCoinsAddress - kEwram;
    if (!f.any() || ewram.size() < coins + 2 || ewram.size() < luigi + 12) return false;
    auto loaded = [&](std::size_t base) {
        const std::uint16_t hp = get16(ewram, base + 2), bp = get16(ewram, base + 8);
        return hp != 0 && hp <= kMaxStat && bp != 0 && bp <= kMaxStat;
    };
    if (!loaded(mario) || !loaded(luigi)) return false;
    bool changed = false;
    auto refill = [&](std::size_t cur, std::size_t max) {
        const std::uint16_t m = get16(ewram, max);
        if (get16(ewram, cur) != m) {
            set16(ewram, cur, m);
            changed = true;
        }
    };
    for (std::size_t base : {mario, luigi}) {
        if (f.hp) refill(base + 0, base + 2);
        if (f.bp) refill(base + 6, base + 8);
    }
    if (f.coins && get16(ewram, coins) != game::kMaxCoins) {
        set16(ewram, coins, std::uint16_t(game::kMaxCoins));
        changed = true;
    }
    return changed;
}

}  // namespace supersaga::cheats
