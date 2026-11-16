#pragma once
// Controller rumble (Settings > Controls > Rumble). The GBA has no rumble, so Watcher reads the
// game's RAM after every frame and turns what just happened into a Pulse; the host plays it on
// every connected controller (input::Manager::rumble), scaled by the strength setting.
//
// Events, read from Mario & Luigi: Superstar Saga's RAM (the stats the cheats use, EWRAM
// 0x02004866 for Mario and 0x020048A2 for Luigi, +0 u16 HP, +2 u16 max HP):
//   - Mario or Luigi takes damage: HP drops with the maximum unchanged. Stronger for bigger hits.
//   - a brother is knocked out: HP reaches 0.
//
// Settings (supersaga::Config): input.rumble (on by default), input.rumble_strength (10-100,
// default 75).
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

#include "game.h"

namespace supersaga::rumble {

inline constexpr bool kDefaultOn = true;
inline constexpr int kDefaultStrength = 75;
inline constexpr int kMinStrength = 10;
// Shown next to the Controller rumble toggle.
inline constexpr const char* kEventsHelp =
    "Shakes your controller when Mario or Luigi gets hit, harder for bigger hits, and when one "
    "of them is knocked out. Works with controllers that have rumble motors.";

// One rumble: motor strengths 0..1 (low = the big, slow motor, high = the small, fast one).
struct Pulse {
    float low = 0, high = 0;
    int ms = 0;
    explicit operator bool() const { return ms > 0 && (low > 0 || high > 0); }
    float strength() const { return std::max(low, high); }
};

// The stronger of two pulses; on a tie the longer one.
inline Pulse stronger(const Pulse& a, const Pulse& b) {
    if (!a) return b;
    if (!b) return a;
    if (a.strength() != b.strength()) return a.strength() > b.strength() ? a : b;
    return a.ms >= b.ms ? a : b;
}

// Decides which pulses reach the controllers: a new pulse replaces the one still playing only
// when it is at least as strong, so a light tick never cuts a big hit short. Counts time in
// game frames (step once per emulated frame).
class Mixer {
public:
    // True when p should be played now.
    bool take(const Pulse& p) {
        if (!p) return false;
        if (left_ms_ > 0 && p.strength() < playing_.strength()) return false;
        playing_ = p;
        left_ms_ = p.ms;
        return true;
    }
    void step() { left_ms_ = std::max(0, left_ms_ - kFrameMs); }
    bool playing() const { return left_ms_ > 0; }
    void clear() { left_ms_ = 0; }

private:
    static constexpr int kFrameMs = 17;  // one GBA frame (16.74 ms)
    Pulse playing_;
    int left_ms_ = 0;
};

// Strength setting (percent) -> motor value for SDL (0..0xFFFF).
inline std::uint16_t motor(float level, int strength_percent) {
    const float v = std::clamp(level, 0.0f, 1.0f) * std::clamp(strength_percent, 0, 100) / 100.0f;
    return std::uint16_t(v * 65535.0f + 0.5f);
}

// A hit that took `lost` of `max` health: stronger and longer for bigger hits.
inline Pulse hit(int lost, int max) {
    const float share = max > 0 ? float(lost) / float(max) : 0.0f;
    const float f = std::min(1.0f, 0.45f + share * 2.0f);
    return {f, f * 0.6f, 120 + int(std::min(share, 0.5f) * 400.0f)};
}
inline constexpr Pulse kDeath{1.0f, 1.0f, 700};

namespace detail {
inline std::uint16_t get16(std::span<const std::uint8_t> m, std::size_t at) {
    return std::uint16_t(m[at] | (m[at + 1] << 8));
}
}  // namespace detail

// ---- Mario & Luigi: Superstar Saga ----

class Watcher {
public:
    // Forgets the last frame, so a jump in the numbers (a loaded state, rewind, a new game) is
    // not taken for an event.
    void reset() { primed_ = false; }

    // Call after each emulated frame with the 32 KiB IWRAM (0x03000000) and 256 KiB EWRAM.
    Pulse step(std::span<const std::uint8_t> /*iwram*/, std::span<const std::uint8_t> ewram) {
        using detail::get16;
        constexpr std::size_t ew = 0x02000000u;
        if (ewram.size() < game::kLuigiStatsAddress - ew + 4) return {};
        Frame now;
        for (int b = 0; b < 2; ++b) {
            const std::size_t at = (b == 0 ? game::kMarioStatsAddress : game::kLuigiStatsAddress) - ew;
            now.hp[b] = get16(ewram, at);
            now.max_hp[b] = get16(ewram, at + 2);
        }
        const Frame before = last_;
        const bool primed = primed_;
        last_ = now;
        primed_ = true;
        if (!primed) return {};

        Pulse out;
        for (int b = 0; b < 2; ++b) {
            // A loaded file (the game caps stats at 999) and the same maximum as last frame.
            const bool same = now.max_hp[b] == before.max_hp[b] && now.max_hp[b] != 0 && now.max_hp[b] <= 999;
            if (!same || now.hp[b] >= before.hp[b] || before.hp[b] > now.max_hp[b]) continue;
            out = stronger(out, now.hp[b] == 0 ? kDeath : hit(before.hp[b] - now.hp[b], now.max_hp[b]));
        }
        return out;
    }

private:
    struct Frame {
        std::uint16_t hp[2] = {0, 0}, max_hp[2] = {0, 0};
    };
    Frame last_;
    bool primed_ = false;
};

}  // namespace supersaga::rumble
