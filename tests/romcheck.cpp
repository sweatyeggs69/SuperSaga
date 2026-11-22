// Headless check: supersaga_romcheck --selftest, or supersaga_romcheck path/to/game.gba
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <vector>

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

// Builds a fake 64 KiB "ROM" with the real header and a 0xFF-padded tail, then checks that each
// kind of alternate dump cleans up to it, and that a changed byte is never accepted.
static int dump_selftest() {
    using Bytes = std::vector<std::uint8_t>;
    const std::size_t size = 64 * 1024;
    std::uint8_t header[32];
    supersaga::canonical_header(header);
    Bytes rom(size, 0xFF);
    std::uint32_t x = 12345;
    for (std::size_t i = 0; i < size - 4096; ++i) { x = x * 1103515245u + 12345u; rom[i] = std::uint8_t(x >> 16); }
    std::copy(std::begin(header), std::end(header), rom.begin() + 0xA0);
    const std::string want = supersaga::to_hex(supersaga::sha1(rom.data(), rom.size()));

    int fails = 0;
    auto expect = [&](const char* name, const Bytes& in, bool ok, const char* fix_part) {
        auto r = supersaga::normalize_dump(in, want, size, header);
        bool good = r.matched == ok && (!ok || (r.data == rom && r.fixes.find(fix_part) != std::string::npos));
        std::printf("%s %s%s%s\n", good ? "ok  " : "FAIL", name, r.fixes.empty() ? "" : ": ", r.fixes.c_str());
        if (!good) ++fails;
    };

    expect("exact dump", rom, true, "");

    Bytes trimmed(rom.begin(), rom.end() - 4096);
    expect("trimmed dump", trimmed, true, "trimmed padding");

    Bytes zero_pad = rom;
    for (std::size_t i = size - 4096; i < size; ++i) zero_pad[i] = 0;
    Bytes zero_rom = zero_pad;  // a ROM whose real padding is 0x00
    {
        auto r = supersaga::normalize_dump(Bytes(zero_rom.begin(), zero_rom.end() - 4096),
                                           supersaga::to_hex(supersaga::sha1(zero_rom.data(), size)), size, header);
        bool good = r.matched && r.data == zero_rom;
        std::printf("%s trimmed dump, 0x00 padding\n", good ? "ok  " : "FAIL");
        if (!good) ++fails;
    }

    Bytes over = rom;
    over.insert(over.end(), rom.begin(), rom.end());  // mirrored overdump, twice the size
    expect("overdump", over, true, "overdump");

    Bytes copier(512, 0);
    copier.insert(copier.end(), rom.begin(), rom.end());
    expect("copier header", copier, true, "copier header");

    Bytes fixed = rom;
    fixed[0xBE] = 0x12;  // reserved byte a flash-cart tool wrote
    fixed[0xB4] = 0x80;  // debug/device byte
    expect("rewritten header", fixed, true, "cartridge header");

    Bytes both(512, 0);
    both.insert(both.end(), trimmed.begin(), trimmed.end());
    both[512 + 0xBE] = 0x34;
    expect("copier header + trimmed + rewritten header", both, true, "cartridge header");

    Bytes copier_odd(512, 0);
    copier_odd.insert(copier_odd.end(), rom.begin(), rom.end() - 4000);
    expect("copier header + trimmed to an odd size", copier_odd, true, "copier header");

    Bytes copier_bad(512, 0);
    copier_bad.insert(copier_bad.end(), rom.begin(), rom.end());
    copier_bad[512 + 0xB2] = 0;  // header made invalid, too
    expect("copier header + broken header", copier_bad, true, "cartridge header");

    Bytes hacked = rom;
    hacked[0x1000] ^= 1;  // any real change (a hack, another revision) must still be rejected
    expect("changed game data", hacked, false, "");
    Bytes hacked_trim(hacked.begin(), hacked.end() - 4096);
    expect("changed game data, trimmed", hacked_trim, false, "");

    std::printf(fails ? "dump cleanup selftest failed\n" : "dump cleanup selftest ok\n");
    return fails ? 1 : 0;
}

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--dumps") == 0) return dump_selftest();
    if (argc < 2 || std::strcmp(argv[1], "--selftest") == 0) return selftest();
    auto r = supersaga::load_rom(argv[1]);
    if (!r.ok) { std::printf("error: %s\n", r.error.c_str()); return 1; }
    const auto& h = r.rom.header;
    std::printf("title=%s code=%s maker=%s rev=%u size=%zu\nsha1=%s\nfile_sha1=%s\nmatch=%s\n",
                h.title.c_str(), h.game_code.c_str(), h.maker.c_str(), h.version,
                r.rom.data.size(), r.rom.sha1.c_str(), r.rom.file_sha1.c_str(), r.rom.hash_matches ? "yes" : "no");
    if (!r.rom.fixes.empty()) std::printf("fixes=%s\n", r.rom.fixes.c_str());
    if (!r.rom.other_release.empty()) std::printf("other_release=%s\n", r.rom.other_release.c_str());
    return r.rom.hash_matches ? 0 : 2;
}
