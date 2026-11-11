#pragma once
// Writes a 240x160 BGR555 frame as a 24-bit BMP (debug/testing dumps).

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace supersaga {

inline bool write_bmp(const std::string& path, const std::uint16_t* px, int w = 240, int h = 160) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    const int row = (w * 3 + 3) & ~3;
    const std::uint32_t data = std::uint32_t(row * h), size = 54 + data;
    std::uint8_t hdr[54] = {'B', 'M'};
    auto put = [&](int off, std::uint32_t v, int n) {
        for (int i = 0; i < n; ++i) hdr[off + i] = std::uint8_t(v >> (8 * i));
    };
    put(2, size, 4);
    put(10, 54, 4);
    put(14, 40, 4);
    put(18, std::uint32_t(w), 4);
    put(22, std::uint32_t(-h), 4);
    put(26, 1, 2);
    put(28, 24, 2);
    put(34, data, 4);
    f.write(reinterpret_cast<char*>(hdr), 54);
    std::vector<std::uint8_t> line(row, 0);
    auto ex = [](std::uint16_t v) { return std::uint8_t((v << 3) | (v >> 2)); };
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            std::uint16_t c = px[y * w + x];
            line[x * 3 + 0] = ex((c >> 10) & 31);
            line[x * 3 + 1] = ex((c >> 5) & 31);
            line[x * 3 + 2] = ex(c & 31);
        }
        f.write(reinterpret_cast<char*>(line.data()), row);
    }
    return bool(f);
}

}  // namespace supersaga
