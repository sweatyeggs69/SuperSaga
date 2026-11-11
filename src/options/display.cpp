#include "options/display.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace supersaga::display {

const char* name(ScaleMode m) {
    switch (m) {
        case ScaleMode::Integer: return "Integer";
        case ScaleMode::Fit: return "Fit";
        default: return "?";
    }
}

const char* name(Aspect a) {
    switch (a) {
        case Aspect::Native: return "3:2";
        case Aspect::R16x10: return "16:10";
        case Aspect::R16x9: return "16:9";
        default: return "?";
    }
}

const char* name(Overlay o) {
    switch (o) {
        case Overlay::None: return "Off";
        case Overlay::Grid: return "Grid";
        case Overlay::Lcd: return "LCD";
        case Overlay::Scanlines: return "Scanlines";
        default: return "?";
    }
}

const char* name(ColorMode c) {
    switch (c) {
        case ColorMode::Raw: return "Raw";
        case ColorMode::GbaLcd: return "GBA LCD";
        default: return "?";
    }
}

const char* name(Blend b) {
    switch (b) {
        case Blend::Off: return "Off";
        case Blend::Mix: return "Mix";
        case Blend::Ghost: return "Ghost";
        default: return "?";
    }
}

namespace {
template <typename E>
bool parse_enum(const std::string& value, E& out) {
    for (int i = 0; i < static_cast<int>(E::Count); ++i)
        if (value == name(static_cast<E>(i))) {
            out = static_cast<E>(i);
            return true;
        }
    return false;
}

std::string trim(const std::string& s) {
    auto b = s.find_first_not_of(" \t\r\n");
    auto e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}
}  // namespace

void store(const Settings& s, const Setter& set) {
    set("display.scale", name(s.scale));
    set("display.aspect", name(s.aspect));
    set("display.overlay", name(s.overlay));
    set("display.overlay_strength", std::to_string(s.overlay_strength));
    set("display.color", name(s.color));
    set("display.blend", name(s.blend));
}

Settings load(const Getter& get) {
    Settings s;
    parse_enum(trim(get("display.scale", "")), s.scale);
    const std::string aspect = trim(get("display.aspect", ""));
    if (!parse_enum(aspect, s.aspect) && (aspect == "21:9" || aspect == "32:9"))
        s.aspect = Aspect::R16x9;  // removed wider shapes: the widest one still offered
    parse_enum(trim(get("display.overlay", "")), s.overlay);
    parse_enum(trim(get("display.color", "")), s.color);
    parse_enum(trim(get("display.blend", "")), s.blend);
    try {
        s.overlay_strength = std::clamp(std::stoi(get("display.overlay_strength", "40")), 0, 100);
    } catch (...) {
    }
    return s;
}

double aspect_ratio(Aspect a) {
    switch (a) {
        case Aspect::R16x10: return 16.0 / 10.0;
        case Aspect::R16x9: return 16.0 / 9.0;
        default: return double(kWidth) / double(kHeight);
    }
}

int wide_width(Aspect a) {
    int w = int(std::lround(kHeight * aspect_ratio(a) / 2.0)) * 2;
    return std::clamp(w, kWidth, kMaxWideWidth);
}

Layout compute_layout(int out_w, int out_h, ScaleMode mode, int src_w, int src_h) {
    out_w = std::max(out_w, 1);
    out_h = std::max(out_h, 1);
    src_w = std::max(src_w, 1);
    src_h = std::max(src_h, 1);
    Layout l;
    const int s = std::min(out_w / src_w, out_h / src_h);
    if (mode == ScaleMode::Integer && s >= 1) {
        l.scale_x = l.scale_y = s;
        l.pixel_perfect = true;
        l.dest.w = src_w * s;
        l.dest.h = src_h * s;
    } else {
        const double f = std::min(double(out_w) / src_w, double(out_h) / src_h);
        l.dest.w = std::max(1, int(std::lround(src_w * f)));
        l.dest.h = std::max(1, int(std::lround(src_h * f)));
    }
    l.dest.x = (out_w - l.dest.w) / 2;
    l.dest.y = (out_h - l.dest.h) / 2;
    return l;
}

// ---------------------------------------------------------------------------------------
// Color

namespace {
std::uint32_t pack(double r, double g, double b) {
    auto c = [](double v) { return std::uint32_t(std::clamp(int(std::lround(v * 255.0)), 0, 255)); };
    return 0xFF000000u | (c(r) << 16) | (c(g) << 8) | c(b);
}
}  // namespace

ColorPipeline::ColorPipeline() {
    for (auto& t : lut_) t.resize(0x8000);
    for (int i = 0; i < 0x8000; ++i) {
        const int r5 = i & 31, g5 = (i >> 5) & 31, b5 = (i >> 10) & 31;

        // Raw: replicate the top bits so 31 maps to 255 exactly.
        auto expand = [](int v) { return ((v << 3) | (v >> 2)) / 255.0; };
        lut_[int(ColorMode::Raw)][i] = pack(expand(r5), expand(g5), expand(b5));

        // GBA LCD: the unlit reflective panel has a steep response curve (dark values get
        // crushed), its color filters bleed into each other, and the whole image is dim
        // and desaturated. Model: linearize with a high display gamma, mix channels, then
        // re-encode for an sRGB monitor. This is the widely used model from higan/bsnes.
        constexpr double lcd_gamma = 4.0, out_gamma = 2.2;
        const double lr = std::pow(r5 / 31.0, lcd_gamma);
        const double lg = std::pow(g5 / 31.0, lcd_gamma);
        const double lb = std::pow(b5 / 31.0, lcd_gamma);
        const double scale = 255.0 / 280.0;
        const double r = std::pow((0 * lb + 50 * lg + 255 * lr) / 255.0, 1.0 / out_gamma) * scale;
        const double g = std::pow((30 * lb + 230 * lg + 10 * lr) / 255.0, 1.0 / out_gamma) * scale;
        const double b = std::pow((220 * lb + 10 * lg + 50 * lr) / 255.0, 1.0 / out_gamma) * scale;
        lut_[int(ColorMode::GbaLcd)][i] = pack(r, g, b);
    }
}

void ColorPipeline::convert(const std::uint16_t* src, std::uint32_t* dst, int count, ColorMode mode) const {
    const auto& t = lut_[static_cast<int>(mode)];
    for (int i = 0; i < count; ++i) dst[i] = t[src[i] & 0x7FFF];
}

// ---------------------------------------------------------------------------------------
// Interframe blending

void FrameBlender::process(std::uint32_t* frame, int count, Blend mode) {
    if (mode != last_mode_ || prev_.size() != std::size_t(count)) {
        have_prev_ = false;
        last_mode_ = mode;
    }
    if (mode == Blend::Off) return;
    if (prev_.size() != std::size_t(count)) prev_.assign(std::size_t(count), 0);
    if (!have_prev_) {
        std::copy(frame, frame + count, prev_.begin());
        have_prev_ = true;
        return;
    }

    for (int i = 0; i < count; ++i) {
        const std::uint32_t cur = frame[i], old = prev_[i];
        std::uint32_t out;
        if (mode == Blend::Mix) {
            // Per channel average without unpacking: (a & b) + ((a ^ b) >> 1), rounding up.
            out = (cur | old) - (((cur ^ old) & 0xFEFEFEFEu) >> 1);
            prev_[i] = cur;
        } else {
            // Ghost: 5/8 new + 3/8 of what was on screen last frame, so trails fade over
            // a few frames like the GBA's slow pixels.
            auto ch = [&](int s) {
                const std::uint32_t a = (cur >> s) & 0xFF, b = (old >> s) & 0xFF;
                return ((a * 5 + b * 3 + 4) >> 3) << s;
            };
            out = 0xFF000000u | ch(16) | ch(8) | ch(0);
            prev_[i] = out;
        }
        frame[i] = out;
    }
}

// ---------------------------------------------------------------------------------------
// Overlays

namespace {
struct AxisCell {
    bool edge;     // last output pixel of a GBA pixel cell
    double phase;  // 0..1 position of this output pixel's center within its cell
};

std::vector<AxisCell> axis_cells(int out, int cells) {
    std::vector<AxisCell> v(out);
    for (int p = 0; p < out; ++p) {
        const long long c0 = (long long)p * cells / out;
        const long long c1 = (long long)(p + 1) * cells / out;
        const double u = (p + 0.5) * cells / out;
        v[p].edge = (c1 != c0) || p == out - 1;
        v[p].phase = u - std::floor(u);
    }
    return v;
}

struct Rgb {
    double r = 1, g = 1, b = 1;
};
}  // namespace

bool build_overlay(Overlay overlay, int strength_percent, int w, int h,
                   std::vector<std::uint32_t>& out, int cells_x, int cells_y) {
    out.clear();
    const double s = std::clamp(strength_percent, 0, 100) / 100.0;
    if (overlay == Overlay::None || s <= 0.0 || w <= 0 || h <= 0) return false;

    if (cells_x <= 0 || cells_y <= 0) return false;
    const double cell_w = double(w) / cells_x, cell_h = double(h) / cells_y;
    if (overlay == Overlay::Lcd && cell_w < 3.0) overlay = Overlay::Grid;  // no room for stripes
    const bool lines_x = cell_w >= 2.0, lines_y = cell_h >= 2.0;
    if (overlay == Overlay::Grid && !lines_x && !lines_y) return false;
    if (overlay == Overlay::Scanlines && !lines_y) return false;

    const auto ax = axis_cells(w, cells_x);
    const auto ay = axis_cells(h, cells_y);
    const double dark = 1.0 - s;

    std::vector<Rgb> col(w);
    std::vector<double> row(h, 1.0);
    for (int x = 0; x < w; ++x) {
        Rgb c;
        if ((overlay == Overlay::Grid || overlay == Overlay::Lcd) && lines_x && ax[x].edge) {
            c = {dark, dark, dark};
        } else if (overlay == Overlay::Lcd) {
            // One GBA pixel = three vertical subpixel stripes, red, green, blue.
            const double keep = 1.0 - 0.75 * s;
            if (ax[x].phase < 1.0 / 3.0) c = {1, keep, keep};
            else if (ax[x].phase < 2.0 / 3.0) c = {keep, 1, keep};
            else c = {keep, keep, 1};
        }
        col[x] = c;
    }
    for (int y = 0; y < h; ++y) {
        if (overlay == Overlay::Scanlines)
            row[y] = (ay[y].edge || ay[y].phase >= 0.7) ? dark : 1.0;
        else if (lines_y && ay[y].edge)
            row[y] = dark;
    }

    out.resize(std::size_t(w) * h);
    for (int y = 0; y < h; ++y) {
        std::uint32_t* line = out.data() + std::size_t(y) * w;
        for (int x = 0; x < w; ++x) {
            const Rgb& c = col[x];
            line[x] = pack(c.r * row[y], c.g * row[y], c.b * row[y]);
        }
    }
    return true;
}

}  // namespace supersaga::display
