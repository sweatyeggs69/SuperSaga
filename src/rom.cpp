#include "rom.h"

#include <fstream>

#include "sha1.h"

namespace supersaga {
namespace fs = std::filesystem;

namespace {
std::string read_ascii(const std::vector<std::uint8_t>& d, std::size_t off, std::size_t n) {
    std::string s;
    for (std::size_t i = 0; i < n; ++i) {
        char c = char(d[off + i]);
        if (c == '\0') break;
        s += c;
    }
    return s;
}
}  // namespace

RomLoadResult load_rom(const fs::path& path) {
    RomLoadResult r;
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        r.error = "could not open " + path.string();
        return r;
    }
    f.seekg(0, std::ios::end);
    auto size = std::streamoff(f.tellg());
    f.seekg(0, std::ios::beg);
    if (size < 0xC0 || size > 32 * 1024 * 1024) {
        r.error = "file size " + std::to_string(size) + " is not a plausible GBA ROM";
        return r;
    }
    r.rom.data.resize(std::size_t(size));
    f.read(reinterpret_cast<char*>(r.rom.data.data()), size);
    if (!f) {
        r.error = "read failed";
        return r;
    }

    const auto& d = r.rom.data;
    auto& h = r.rom.header;
    h.title = read_ascii(d, 0xA0, 12);
    h.game_code = read_ascii(d, 0xAC, 4);
    h.maker = read_ascii(d, 0xB0, 2);
    h.version = d[0xBC];
    h.checksum = d[0xBD];

    // GBA header complement check over 0xA0..0xBC.
    std::uint8_t chk = 0;
    for (std::size_t i = 0xA0; i <= 0xBC; ++i) chk = std::uint8_t(chk - d[i]);
    chk = std::uint8_t(chk - 0x19);
    h.checksum_ok = (chk == h.checksum);
    if (!h.checksum_ok || d[0xB2] != 0x96) {
        r.error = "not a valid GBA ROM (header check failed)";
        return r;
    }

    r.rom.sha1 = to_hex(sha1(d.data(), d.size()));
    if (*kExpectedSha1) {
        r.rom.hash_matches = (r.rom.sha1 == kExpectedSha1) && d.size() == kExpectedSize;
    } else {
        // Fingerprint not pinned yet: fall back to the header so development can start.
        r.rom.header_only = true;
        r.rom.hash_matches = h.game_code == game::kGameCode && h.version == game::kRevision &&
                             d.size() == kExpectedSize;
    }
    r.ok = true;
    return r;
}

fs::path find_default_rom(const fs::path& exe_dir) {
    std::error_code ec;
    for (const fs::path& dir : {exe_dir, fs::current_path(ec), fs::current_path(ec) / "rom",
                                exe_dir / ".." / "rom", exe_dir / ".."}) {
        if (dir.empty() || !fs::is_directory(dir, ec)) continue;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            auto ext = e.path().extension().string();
            if (ext == ".gba" || ext == ".GBA") return e.path();
        }
    }
    return {};
}

}  // namespace supersaga
