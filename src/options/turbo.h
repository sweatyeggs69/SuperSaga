#pragma once
// Turbo (auto-fire): while a turbo button is held, the game sees it pressed and released
// repeatedly. Counted in emulated frames so it stays in step with the game and stops while
// the game is paused. No SDL here so it can be tested headless.
#include <algorithm>
#include <array>
#include <cstdint>

namespace supersaga::input {

inline constexpr int kTurboMinRate = 1;  // presses per second
inline constexpr int kTurboMaxRate = 5;
inline constexpr int kTurboDefaultRate = 5;
inline constexpr int kTurboFps = 60;     // the GBA runs at about 59.73 frames per second

struct TurboSettings {
    bool enabled = false;
    int rate = kTurboDefaultRate;  // presses per second, clamped to 1..5
    std::uint16_t buttons = 0x3;   // KEYINPUT bits that use turbo (default A and B)
};

// Frames for one press plus release at the given rate (12 at 5 per second).
inline int turbo_period(int rate) {
    return kTurboFps / std::clamp(rate, kTurboMinRate, kTurboMaxRate);
}

// Tracks how long each button has been held and turns held turbo buttons into presses.
// Call step() once per emulated frame with the real button mask.
class TurboState {
public:
    std::uint16_t step(std::uint16_t mask, const TurboSettings& s) {
        const int period = turbo_period(s.rate);
        const int on = (period + 1) / 2;  // pressed for the first half of each period
        for (int i = 0; i < 16; ++i) {
            const std::uint16_t bit = std::uint16_t(1u << i);
            if (!(mask & bit)) {
                held_[i] = 0;
                continue;
            }
            const int frame = held_[i];
            held_[i] = (held_[i] + 1) % period;
            // The first frame of a hold is always a press, so a quick tap still registers.
            if (s.enabled && (s.buttons & bit) && frame >= on) mask &= std::uint16_t(~bit);
        }
        return mask;
    }
    void reset() { held_.fill(0); }

private:
    std::array<int, 16> held_{};
};

}  // namespace supersaga::input
