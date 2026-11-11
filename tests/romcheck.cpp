// Headless check: supersaga_romcheck --selftest, or supersaga_romcheck path/to/game.gba
#include <cstdio>
#include <cstring>

#include "rom.h"
#include "sha1.h"

static int selftest() {
    struct { const char* in; const char* want; } v[] = {
        {"", "da39a3ee5e6b4b0d3255bfef95601890afd80709"},
        {"abc", "a9993e364706816aba3e25717850c26c9cd0d89d"},
        {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
         "84983e441c3bd26ebaae4aa1f95129e5e54670f1"},
    };
    int fails = 0;
    for (auto& t : v) {
        auto got = supersaga::to_hex(supersaga::sha1(reinterpret_cast<const std::uint8_t*>(t.in), std::strlen(t.in)));
        if (got != t.want) { std::printf("FAIL sha1(\"%s\") = %s\n", t.in, got.c_str()); ++fails; }
    }
    std::printf(fails ? "sha1 selftest failed\n" : "sha1 selftest ok\n");
    return fails ? 1 : 0;
}

int main(int argc, char** argv) {
    if (argc < 2 || std::strcmp(argv[1], "--selftest") == 0) return selftest();
    auto r = supersaga::load_rom(argv[1]);
    if (!r.ok) { std::printf("error: %s\n", r.error.c_str()); return 1; }
    const auto& h = r.rom.header;
    std::printf("title=%s code=%s maker=%s rev=%u size=%zu\nsha1=%s\nmatch=%s\n",
                h.title.c_str(), h.game_code.c_str(), h.maker.c_str(), h.version,
                r.rom.data.size(), r.rom.sha1.c_str(), r.rom.hash_matches ? "yes" : "no");
    return r.rom.hash_matches ? 0 : 2;
}
