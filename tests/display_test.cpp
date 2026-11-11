// Headless checks for the display options: layout math, colors, blending, overlays, settings.
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "options/display.h"
#include "options/turbo.h"

using namespace supersaga::display;

static int failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

static void layouts() {
    // Native 3:2 window at 4x: fills it exactly.
    Layout l = compute_layout(960, 640, ScaleMode::Integer);
    CHECK(l.pixel_perfect && l.scale_x == 4 && l.scale_y == 4);
    CHECK(l.dest.x == 0 && l.dest.y == 0 && l.dest.w == 960 && l.dest.h == 640);

    // 1080p native: 6x is 1440x960, centered.
    l = compute_layout(1920, 1080, ScaleMode::Integer);
    CHECK(l.scale_x == 6 && l.scale_y == 6 && l.dest.x == 240 && l.dest.y == 60);

    // Widescreen never stretches: a 16:9 view is wider in GBA pixels, scaled uniformly.
    CHECK(wide_width(Aspect::Native) == 240);
    CHECK(wide_width(Aspect::R16x9) == 284);
    CHECK(wide_width(Aspect::R16x10) == 256);
    CHECK(wide_width(Aspect::R16x9) == kMaxWideWidth);
    CHECK(wide_margin(Aspect::R16x9) == 22);
    l = compute_layout(1920, 1080, ScaleMode::Integer, wide_width(Aspect::R16x9), kHeight);
    CHECK(l.scale_x == 6 && l.scale_y == 6 && l.dest.w == 284 * 6 && l.dest.h == 960);
    l = compute_layout(3840, 2160, ScaleMode::Fit, wide_width(Aspect::R16x9), kHeight);
    CHECK(!l.pixel_perfect && l.dest.h == 2160 && l.dest.w <= 3840);

    // Tiny window falls back to fit instead of disappearing.
    l = compute_layout(200, 100, ScaleMode::Integer);
    CHECK(l.dest.w > 0 && l.dest.h > 0 && l.dest.w <= 200 && l.dest.h <= 100);

    // A sweep of window sizes: the frame always fits and keeps square pixels.
    for (int w = 100; w < 4000; w += 97)
        for (int h = 100; h < 2500; h += 89)
            for (int m = 0; m < int(ScaleMode::Count); ++m)
                for (int a = 0; a < int(Aspect::Count); ++a) {
                    const int sw = wide_width(Aspect(a));
                    Layout s = compute_layout(w, h, ScaleMode(m), sw, kHeight);
                    CHECK(s.dest.x >= 0 && s.dest.y >= 0 && s.dest.x + s.dest.w <= w && s.dest.y + s.dest.h <= h);
                    const double px = double(s.dest.w) / sw, py = double(s.dest.h) / kHeight;
                    CHECK(std::abs(px - py) / py < 0.02 || s.dest.w < 50);
                }
}

static void colors() {
    ColorPipeline c;
    CHECK(c.lookup(0x7FFF, ColorMode::Raw) == 0xFFFFFFFFu);
    CHECK(c.lookup(0x0000, ColorMode::Raw) == 0xFF000000u);
    CHECK(c.lookup(0x001F, ColorMode::Raw) == 0xFFFF0000u);  // BGR555 red
    // GBA LCD: white is dimmer and pure red is no longer pure.
    const std::uint32_t w = c.lookup(0x7FFF, ColorMode::GbaLcd);
    CHECK((w & 0xFF) < 255 && (w & 0xFF) > 200);
    const std::uint32_t r = c.lookup(0x001F, ColorMode::GbaLcd);
    CHECK(((r >> 8) & 0xFF) > 0 || (r & 0xFF) > 0);
}

static void blending() {
    std::vector<std::uint32_t> a(kPixels, 0xFFFFFFFFu), b(kPixels, 0xFF000000u);
    FrameBlender fb;
    fb.process(a.data(), kPixels, Blend::Mix);
    CHECK(a[0] == 0xFFFFFFFFu);  // first frame passes through
    fb.process(b.data(), kPixels, Blend::Mix);
    CHECK(b[0] == 0xFF808080u || b[0] == 0xFF7F7F7Fu);
    std::vector<std::uint32_t> c(kPixels, 0xFF000000u);
    fb.process(c.data(), kPixels, Blend::Off);
    CHECK(c[0] == 0xFF000000u);
}

static void overlays() {
    std::vector<std::uint32_t> m;
    CHECK(!build_overlay(Overlay::None, 50, 960, 640, m));
    CHECK(!build_overlay(Overlay::Grid, 50, 240, 160, m));  // 1x: no room for lines
    CHECK(build_overlay(Overlay::Grid, 50, 960, 640, m) && m.size() == 960u * 640u);
    CHECK(m[0] == 0xFFFFFFFFu);        // inside a cell
    CHECK(m[3] != 0xFFFFFFFFu);        // last column of the first cell
    CHECK(m[4] == 0xFFFFFFFFu);        // next cell
    CHECK(m[960 * 3] != 0xFFFFFFFFu);  // last row of the first cell
    CHECK(build_overlay(Overlay::Lcd, 50, 1440, 960, m));
    CHECK((m[0] & 0x00FF0000u) == 0x00FF0000u && (m[0] & 0xFF) < 0xFF);  // red stripe first
    CHECK(build_overlay(Overlay::Scanlines, 50, 960, 640, m));
    CHECK(m[0] == 0xFFFFFFFFu && m[960 * 3] != 0xFFFFFFFFu);
    // Wide frame: cells follow the wider source.
    CHECK(build_overlay(Overlay::Grid, 50, 284 * 4, 640, m, 284, 160));
    CHECK(m[3] != 0xFFFFFFFFu && m[4] == 0xFFFFFFFFu);
}

static void settings_roundtrip() {
    std::map<std::string, std::string> kv;
    Settings s;
    s.scale = ScaleMode::Fit;
    s.aspect = Aspect::R16x10;
    s.overlay = Overlay::Lcd;
    s.overlay_strength = 70;
    s.color = ColorMode::GbaLcd;
    s.blend = Blend::Ghost;
    store(s, [&](const std::string& k, const std::string& v) { kv[k] = v; });
    auto get = [&](const std::string& k, const std::string& d) { auto it = kv.find(k); return it == kv.end() ? d : it->second; };
    Settings t = load(get);
    CHECK(t.scale == s.scale && t.aspect == s.aspect && t.overlay == s.overlay);
    CHECK(t.overlay_strength == 70 && t.color == s.color && t.blend == s.blend);
    kv["display.aspect"] = "4:3";  // removed (stretched) setting falls back to native
    kv["display.overlay_strength"] = "x";
    t = load(get);
    CHECK(t.aspect == Aspect::Native && t.overlay_strength == 40);
    kv["display.aspect"] = "21:9";  // removed wider shapes fall back to 16:9
    CHECK(load(get).aspect == Aspect::R16x9);
    kv["display.aspect"] = "32:9";
    CHECK(load(get).aspect == Aspect::R16x9);
}

static void turbo() {
    using namespace supersaga::input;
    CHECK(turbo_period(5) == 12 && turbo_period(1) == 60);
    CHECK(turbo_period(99) == 12 && turbo_period(0) == 60);  // never faster than 5 per second
    TurboSettings t;
    t.enabled = true;
    t.rate = 5;
    t.buttons = 0x1;  // A only
    TurboState st;
    // Holding A and B for one second: A pulses 5 times, B stays held.
    int presses = 0, b_frames = 0;
    bool prev = false;
    for (int f = 0; f < 60; ++f) {
        const std::uint16_t m = st.step(0x3, t);
        const bool a = m & 0x1;
        if (a && !prev) ++presses;
        prev = a;
        if (m & 0x2) ++b_frames;
        if (f < 6) CHECK(a);       // first press lands on the first frame
        else if (f < 12) CHECK(!a);
    }
    CHECK(presses == 5 && b_frames == 60);
    // Releasing restarts the cycle so the next tap is a press right away.
    CHECK(st.step(0x0, t) == 0);
    CHECK(st.step(0x1, t) == 0x1);
    // Off: buttons pass through untouched.
    t.enabled = false;
    st.reset();
    for (int f = 0; f < 30; ++f) CHECK(st.step(0x3, t) == 0x3);
}

int main() {
    turbo();
    layouts();
    colors();
    blending();
    overlays();
    settings_roundtrip();
    if (failures) std::fprintf(stderr, "%d check(s) failed\n", failures);
    else std::printf("display tests passed\n");
    return failures ? 1 : 0;
}
