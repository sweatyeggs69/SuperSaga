#pragma once
// Display post-processing for the GBA frame: scaling layout, aspect ratios, GBA LCD color
// emulation, interframe blending and screen overlays. No SDL dependency, so the logic is
// testable headless; src/presenter.* drives it with SDL.
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace supersaga::display {

inline constexpr int kWidth = 240;
inline constexpr int kHeight = 160;
inline constexpr int kPixels = kWidth * kHeight;

// Integer: every GBA pixel becomes an exact block of screen pixels (pixel perfect).
// Fit: largest size that fits the window at the chosen aspect, fractional scale allowed.
enum class ScaleMode { Integer, Fit, Count };

// Shape of the game view. Native is the GBA's own 240x160 (3:2). The wider shapes never
// stretch: they ask the renderer to draw extra columns of the level at the sides, so pixels
// stay square (see wide_width). Pixels are always square.
// Only shapes up to 16:9 are offered, as in the other ports: the game only places characters
// around the original view, so much wider views would show them popping in at the sides.
enum class Aspect { Native, R16x10, R16x9, Count };

// Widest view the runtime may be asked for, and the per-side margin that implies.
inline constexpr int kMaxWideWidth = 284;
inline constexpr int kMaxWideMargin = (kMaxWideWidth - kWidth) / 2;

// Width in GBA pixels of a 160-line view with this shape (even, so margins are equal).
int wide_width(Aspect a);
// Extra columns per side for this shape: (wide_width - 240) / 2.
inline int wide_margin(Aspect a) { return (wide_width(a) - kWidth) / 2; }

// Grid: dark lines between pixels. Lcd: RGB subpixel stripes plus grid, like the GBA panel.
// Scanlines: darkened horizontal line at the bottom of each pixel row.
enum class Overlay { None, Grid, Lcd, Scanlines, Count };

// Raw: BGR555 expanded straight to RGB888 (what most emulators show by default).
// GbaLcd: emulates the washed out, low contrast color response of the original GBA screen.
enum class ColorMode { Raw, GbaLcd, Count };

// Mix: average of this frame and the previous one (fixes 30 Hz flicker effects).
// Ghost: exponential trail like the slow response of the original GBA LCD.
enum class Blend { Off, Mix, Ghost, Count };

struct Settings {
    ScaleMode scale = ScaleMode::Integer;
    Aspect aspect = Aspect::Native;
    Overlay overlay = Overlay::None;
    int overlay_strength = 40;  // percent, 0..100
    ColorMode color = ColorMode::Raw;
    Blend blend = Blend::Off;
};

const char* name(ScaleMode m);
const char* name(Aspect a);
const char* name(Overlay o);
const char* name(ColorMode c);
const char* name(Blend b);

// Settings live in the app's key/value config under display.* keys. Missing keys and bad
// values fall back to defaults, so old config files keep working.
using Getter = std::function<std::string(const std::string& key, const std::string& fallback)>;
using Setter = std::function<void(const std::string& key, const std::string& value)>;
Settings load(const Getter& get);
void store(const Settings& s, const Setter& set);

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
};

struct Layout {
    Rect dest;              // where the frame goes, in output (physical) pixels
    int scale_x = 0;        // integer factors when pixel perfect, else 0
    int scale_y = 0;
    bool pixel_perfect = false;
};

// Width/height ratio for an aspect.
double aspect_ratio(Aspect a);

// Chooses where to draw a src_w x src_h frame (square pixels) inside an out_w x out_h output.
// Integer mode uses the largest whole-number scale that fits; if even 1x does not fit it
// falls back to Fit.
Layout compute_layout(int out_w, int out_h, ScaleMode mode, int src_w = kWidth, int src_h = kHeight);

// Converts BGR555 GBA pixels to 0xAARRGGBB through a lookup table per color mode.
class ColorPipeline {
public:
    ColorPipeline();
    void convert(const std::uint16_t* src, std::uint32_t* dst, int count, ColorMode mode) const;
    std::uint32_t lookup(std::uint16_t bgr555, ColorMode mode) const {
        return lut_[static_cast<int>(mode)][bgr555 & 0x7FFF];
    }

private:
    std::array<std::vector<std::uint32_t>, static_cast<int>(ColorMode::Count)> lut_;
};

// Blends consecutive 0xAARRGGBB frames of `count` pixels in place. A size change restarts.
class FrameBlender {
public:
    void process(std::uint32_t* frame, int count, Blend mode);
    void reset() { have_prev_ = false; }

private:
    std::vector<std::uint32_t> prev_;  // previous source frame (Mix) or previous output (Ghost)
    bool have_prev_ = false;
    Blend last_mode_ = Blend::Off;
};

// Builds a w x h multiply mask (0xAARRGGBB, white = no change) for the given overlay,
// aligned to the pixel cells of a cells_x x cells_y frame drawn at that size. Returns false (and leaves
// `out` empty) when there is nothing to draw, e.g. Overlay::None or cells too small.
bool build_overlay(Overlay overlay, int strength_percent, int w, int h,
                   std::vector<std::uint32_t>& out, int cells_x = kWidth, int cells_y = kHeight);

}  // namespace supersaga::display
