// Minimal SHA-1 (FIPS 180-4). Used only to identify the user's ROM dump.
#include "sha1.h"

#include <cstring>
#include <vector>

namespace supersaga {
namespace {
inline std::uint32_t rol(std::uint32_t v, int s) { return (v << s) | (v >> (32 - s)); }

void block(std::uint32_t h[5], const std::uint8_t* p) {
    std::uint32_t w[80];
    for (int i = 0; i < 16; ++i)
        w[i] = (std::uint32_t(p[i * 4]) << 24) | (std::uint32_t(p[i * 4 + 1]) << 16) |
               (std::uint32_t(p[i * 4 + 2]) << 8) | std::uint32_t(p[i * 4 + 3]);
    for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; ++i) {
        std::uint32_t f, k;
        if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else             { f = b ^ c ^ d;                    k = 0xCA62C1D6; }
        std::uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}
}  // namespace

Sha1Digest sha1(const std::uint8_t* data, std::size_t len) {
    std::uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    std::size_t full = len / 64;
    for (std::size_t i = 0; i < full; ++i) block(h, data + i * 64);

    std::uint8_t tail[128] = {};
    std::size_t rem = len % 64;
    std::memcpy(tail, data + full * 64, rem);
    tail[rem] = 0x80;
    std::size_t tail_len = (rem < 56) ? 64 : 128;
    std::uint64_t bits = std::uint64_t(len) * 8;
    for (int i = 0; i < 8; ++i) tail[tail_len - 1 - i] = std::uint8_t(bits >> (i * 8));
    for (std::size_t i = 0; i < tail_len; i += 64) block(h, tail + i);

    Sha1Digest out{};
    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 4; ++j) out[i * 4 + j] = std::uint8_t(h[i] >> (24 - j * 8));
    return out;
}

std::string to_hex(const Sha1Digest& d) {
    static const char* hex = "0123456789abcdef";
    std::string s;
    s.reserve(40);
    for (auto b : d) { s += hex[b >> 4]; s += hex[b & 15]; }
    return s;
}

}  // namespace supersaga
