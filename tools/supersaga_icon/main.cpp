// supersaga_icon: renders the SuperSaga app icon (the SuperSaga logo, assets/logo/supersaga-logo.png,
// on a warm dark rounded tile) into an .iconset folder of PNGs for iconutil, a Windows .ico or
// one PNG. The logo is read with stb_image (public domain); the logo path is compiled in.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <fstream>
#include <string>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <stb_image.h>

namespace {

struct Rgba { float r, g, b, a; };

Rgba mix(Rgba a, Rgba b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}
Rgba over(Rgba dst, Rgba src) {  // src over dst, straight alpha
    float a = src.a + dst.a * (1 - src.a);
    if (a <= 0) return {0, 0, 0, 0};
    return {(src.r * src.a + dst.r * dst.a * (1 - src.a)) / a, (src.g * src.a + dst.g * dst.a * (1 - src.a)) / a,
            (src.b * src.a + dst.b * dst.a * (1 - src.a)) / a, a};
}

// The logo, premultiplied RGBA floats, loaded once.
struct Logo {
    int w = 0, h = 0;
    std::vector<float> px;  // premultiplied r, g, b, a
    bool ok() const { return w > 0 && h > 0; }
};
const Logo& logo() {
    static Logo l = [] {
        Logo out;
        int w = 0, h = 0, n = 0;
        unsigned char* data = stbi_load(SUPERSAGA_ICON_LOGO, &w, &h, &n, 4);
        if (!data) {
            std::fprintf(stderr, "supersaga_icon: cannot read %s\n", SUPERSAGA_ICON_LOGO);
            return out;
        }
        out.w = w;
        out.h = h;
        out.px.resize(size_t(w) * h * 4);
        for (size_t i = 0; i < size_t(w) * h; ++i) {
            const float a = data[i * 4 + 3] / 255.0f;
            for (int c = 0; c < 3; ++c) out.px[i * 4 + c] = data[i * 4 + c] / 255.0f * a;
            out.px[i * 4 + 3] = a;
        }
        stbi_image_free(data);
        return out;
    }();
    return l;
}
// Area-averaged logo color over the logo-pixel box [x0,x1) x [y0,y1) (straight alpha result).
Rgba sample_logo(float x0, float y0, float x1, float y1) {
    const Logo& l = logo();
    const int ix0 = std::max(0, int(std::floor(x0))), iy0 = std::max(0, int(std::floor(y0)));
    const int ix1 = std::min(l.w, std::max(ix0 + 1, int(std::ceil(x1)))), iy1 = std::min(l.h, std::max(iy0 + 1, int(std::ceil(y1))));
    float r = 0, g = 0, b = 0, a = 0, n = 0;
    for (int y = iy0; y < iy1; ++y)
        for (int x = ix0; x < ix1; ++x) {
            const float* p = &l.px[(size_t(y) * l.w + x) * 4];
            r += p[0]; g += p[1]; b += p[2]; a += p[3]; n += 1;
        }
    if (n == 0 || a <= 0) return {0, 0, 0, 0};
    return {r / a, g / a, b / a, a / n};
}

// Superellipse "squircle" mask like macOS icons, in unit coordinates [0,1].
bool in_squircle(float u, float v) {
    float x = std::fabs(u - 0.5f) / 0.40f, y = std::fabs(v - 0.5f) / 0.40f;
    return std::pow(x, 5.0f) + std::pow(y, 5.0f) <= 1.0f;
}

// A warm dark tile with a soft red glow (original artwork) and the SuperSaga logo across it.
// `step` is the size of one sample in unit coordinates, so the logo is area-averaged.
Rgba shade(float u, float v, float step) {
    Rgba out{0, 0, 0, 0};
    if (!in_squircle(u, v)) return out;
    Rgba top{0.11f, 0.06f, 0.05f, 1}, bottom{0.24f, 0.10f, 0.06f, 1};
    out = mix(top, bottom, v);
    float g = std::hypot(u - 0.5f, (v - 0.5f) * 1.2f);
    out = over(out, {0.75f, 0.20f, 0.06f, std::max(0.0f, 0.45f - g * 1.3f)});
    const Logo& l = logo();
    if (!l.ok()) return out;
    // The logo spans 72% of the image (90% of the tile), centered.
    const float lw = 0.72f, lh = lw * float(l.h) / float(l.w);
    const float lx = (u - (1 - lw) / 2) / lw * float(l.w), ly = (v - (1 - lh) / 2) / lh * float(l.h);
    const float half = step / lw * float(l.w) / 2;
    if (lx + half < 0 || ly + half < 0 || lx - half >= float(l.w) || ly - half >= float(l.h)) return out;
    return over(out, sample_logo(lx - half, ly - half, lx + half, ly + half));
}

std::uint32_t crc32(const std::uint8_t* d, size_t n, std::uint32_t c = 0xFFFFFFFF) {
    static std::uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t v = i;
            for (int k = 0; k < 8; ++k) v = (v & 1) ? 0xEDB88320 ^ (v >> 1) : v >> 1;
            table[i] = v;
        }
        init = true;
    }
    for (size_t i = 0; i < n; ++i) c = table[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return c;
}

void be32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    for (int i = 3; i >= 0; --i) v.push_back(std::uint8_t(x >> (8 * i)));
}

// Minimal PNG writer: RGBA8, zlib "stored" (uncompressed) blocks.
bool write_png(const std::string& path, int w, int h, const std::vector<std::uint8_t>& rgba) {
    std::vector<std::uint8_t> raw;
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), rgba.begin() + y * w * 4, rgba.begin() + (y + 1) * w * 4);
    }
    std::vector<std::uint8_t> z = {0x78, 0x01};
    std::uint32_t a = 1, b = 0;
    for (std::uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    for (size_t off = 0; off < raw.size(); off += 65535) {
        size_t n = std::min<size_t>(65535, raw.size() - off);
        z.push_back(off + n >= raw.size() ? 1 : 0);
        z.push_back(std::uint8_t(n));
        z.push_back(std::uint8_t(n >> 8));
        z.push_back(std::uint8_t(~n));
        z.push_back(std::uint8_t(~n >> 8));
        z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
    }
    be32(z, (b << 16) | a);

    std::vector<std::uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    auto chunk = [&](const char* type, const std::vector<std::uint8_t>& data) {
        be32(png, std::uint32_t(data.size()));
        size_t start = png.size();
        png.insert(png.end(), type, type + 4);
        png.insert(png.end(), data.begin(), data.end());
        be32(png, ~crc32(&png[start], png.size() - start));
    };
    std::vector<std::uint8_t> ihdr;
    be32(ihdr, std::uint32_t(w));
    be32(ihdr, std::uint32_t(h));
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size()));
    return bool(f);
}

std::vector<std::uint8_t> render(int size) {
    const int ss = 4;
    std::vector<std::uint8_t> px(size_t(size) * size * 4);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            float r = 0, g = 0, b = 0, a = 0;
            for (int sy = 0; sy < ss; ++sy)
                for (int sx = 0; sx < ss; ++sx) {
                    Rgba c = shade((x + (sx + 0.5f) / ss) / size, (y + (sy + 0.5f) / ss) / size, 1.0f / (float(size) * ss));
                    r += c.r * c.a; g += c.g * c.a; b += c.b * c.a; a += c.a;
                }
            float n = float(ss * ss);
            size_t i = (size_t(y) * size + x) * 4;
            px[i + 3] = std::uint8_t(std::lround(a / n * 255));
            if (a > 0) {
                px[i + 0] = std::uint8_t(std::lround(std::min(1.0f, r / a) * 255));
                px[i + 1] = std::uint8_t(std::lround(std::min(1.0f, g / a) * 255));
                px[i + 2] = std::uint8_t(std::lround(std::min(1.0f, b / a) * 255));
            }
        }
    return px;
}

}  // namespace

// Encodes a PNG to memory by writing it to a temp file path; simpler: reuse write_png's logic.
std::vector<std::uint8_t> png_bytes(int px) {
    std::string tmp = "supersaga_icon_tmp_" + std::to_string(px) + ".png";
    if (!write_png(tmp, px, px, render(px))) return {};
    std::ifstream f(tmp, std::ios::binary);
    std::vector<std::uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    f.close();
    std::remove(tmp.c_str());
    return b;
}

// Windows .ico with PNG-compressed entries (supported since Windows Vista).
bool write_ico(const std::string& path) {
    const int sizes[] = {16, 24, 32, 48, 64, 128, 256};
    std::vector<std::vector<std::uint8_t>> imgs;
    for (int s : sizes) imgs.push_back(png_bytes(s));
    std::vector<std::uint8_t> out;
    auto le16 = [&](unsigned v) { out.push_back(std::uint8_t(v)); out.push_back(std::uint8_t(v >> 8)); };
    auto le32 = [&](std::uint32_t v) { for (int i = 0; i < 4; ++i) out.push_back(std::uint8_t(v >> (8 * i))); };
    le16(0);
    le16(1);
    le16(unsigned(imgs.size()));
    std::uint32_t offset = 6 + 16 * std::uint32_t(imgs.size());
    for (size_t i = 0; i < imgs.size(); ++i) {
        out.push_back(std::uint8_t(sizes[i] >= 256 ? 0 : sizes[i]));
        out.push_back(std::uint8_t(sizes[i] >= 256 ? 0 : sizes[i]));
        out.push_back(0);
        out.push_back(0);
        le16(1);
        le16(32);
        le32(std::uint32_t(imgs[i].size()));
        le32(offset);
        offset += std::uint32_t(imgs[i].size());
    }
    for (auto& img : imgs) out.insert(out.end(), img.begin(), img.end());
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
    return bool(f);
}

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--ico") return write_ico(argv[2]) ? 0 : 1;
    if (argc == 4 && std::string(argv[1]) == "--png") {
        int px = std::atoi(argv[2]);
        return px > 0 && write_png(argv[3], px, px, render(px)) ? 0 : 1;
    }
    if (argc < 2 || argv[1][0] == '-') {
        std::fprintf(stderr, "usage: supersaga_icon <out.iconset> | --ico <out.ico> | --png <size> <out.png>\n");
        return 2;
    }
    std::string dir = argv[1];
    const int sizes[] = {16, 32, 128, 256, 512};
    for (int s : sizes) {
        for (int scale = 1; scale <= 2; ++scale) {
            int px = s * scale;
            std::string name = dir + "/icon_" + std::to_string(s) + "x" + std::to_string(s) +
                               (scale == 2 ? "@2x" : "") + ".png";
            if (!write_png(name, px, px, render(px))) {
                std::fprintf(stderr, "could not write %s\n", name.c_str());
                return 1;
            }
        }
    }
    return 0;
}
