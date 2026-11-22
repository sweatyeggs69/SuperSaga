#include "rom.h"

#include <algorithm>
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
bool header_valid(const std::uint8_t* d) {
    std::uint8_t chk = 0;
    for (std::size_t i = 0xA0; i <= 0xBC; ++i) chk = std::uint8_t(chk - d[i]);
    chk = std::uint8_t(chk - 0x19);
    return chk == d[0xBD] && d[0xB2] == 0x96;
}

std::string mib(std::size_t bytes) {
    if (bytes % (1024 * 1024) == 0) return std::to_string(bytes / (1024 * 1024)) + " MiB";
    if (bytes % 1024 == 0) return std::to_string(bytes / 1024) + " KiB";
    return std::to_string(bytes) + " bytes";
}

// Files that start with a 512-byte copier/flash-cart header: the real header sits at 512. The
// size is no guide once the dump is also trimmed, so look at the headers; a size of 512 past a
// whole KiB also counts, for a copier header in front of a header that was rewritten as well.
bool has_copier_header(const std::vector<std::uint8_t>& d) {
    return d.size() >= 512 + 0xC0 && !header_valid(d.data()) &&
           (header_valid(d.data() + 512) || d.size() % 1024 == 512);
}

void join(std::string& to, const std::string& what) {
    if (!to.empty()) to += "; ";
    to += what;
}
}  // namespace

void canonical_header(std::uint8_t (&h)[32]) {
    for (auto& b : h) b = 0;
    auto put = [&](std::size_t off, const char* s, std::size_t n) {
        for (std::size_t i = 0; i < n && s[i]; ++i) h[off + i] = std::uint8_t(s[i]);
    };
    put(0x00, game::kHeaderTitle, 12);
    put(0x0C, game::kGameCode, 4);
    put(0x10, game::kMakerCode, 2);
    h[0x12] = 0x96;  // fixed value
    h[0x1C] = std::uint8_t(game::kRevision);
    std::uint8_t chk = 0;
    for (std::size_t i = 0x00; i <= 0x1C; ++i) chk = std::uint8_t(chk - h[i]);
    h[0x1D] = std::uint8_t(chk - 0x19);
}

DumpFix normalize_dump(const std::vector<std::uint8_t>& raw, const std::string& expected_sha1,
                       std::size_t expected_size, const std::uint8_t (&header)[32]) {
    DumpFix out;
    auto matches = [&](const std::vector<std::uint8_t>& d) {
        return d.size() == expected_size && to_hex(sha1(d.data(), d.size())) == expected_sha1;
    };
    if (matches(raw)) {
        out.matched = true;
        out.data = raw;
        return out;
    }

    struct Base { std::vector<std::uint8_t> data; std::string fixes; };
    std::vector<Base> bases;
    if (has_copier_header(raw))
        bases.push_back({std::vector<std::uint8_t>(raw.begin() + 512, raw.end()), "Removed a 512-byte copier header"});
    bases.push_back({raw, ""});

    for (auto& base : bases) {
        std::vector<Base> sized;
        const std::size_t n = base.data.size();
        if (n > expected_size) {
            Base b{std::vector<std::uint8_t>(base.data.begin(), base.data.begin() + std::ptrdiff_t(expected_size)), base.fixes};
            join(b.fixes, "Cut " + mib(n - expected_size) + " of overdump");
            sized.push_back(std::move(b));
        } else if (n < expected_size) {
            if (n < 0xC0) continue;
            for (std::uint8_t fill : {std::uint8_t(0xFF), std::uint8_t(0x00)}) {
                Base b{base.data, base.fixes};
                b.data.resize(expected_size, fill);
                join(b.fixes, "Restored " + mib(expected_size - n) + " of trimmed padding");
                sized.push_back(std::move(b));
            }
        } else {
            sized.push_back(std::move(base));
        }
        for (auto& s : sized) {
            if (!s.fixes.empty() && matches(s.data)) {
                out.matched = true;
                out.data = std::move(s.data);
                out.fixes = std::move(s.fixes);
                return out;
            }
            if (std::equal(std::begin(header), std::end(header), s.data.begin() + 0xA0)) continue;
            std::copy(std::begin(header), std::end(header), s.data.begin() + 0xA0);
            if (matches(s.data)) {
                out.matched = true;
                out.data = std::move(s.data);
                out.fixes = std::move(s.fixes);
                join(out.fixes, "Restored the original cartridge header");
                return out;
            }
        }
    }
    return out;
}

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
    if (size < 0xC0 || size > 32 * 1024 * 1024 + 512) {
        r.error = "file size " + std::to_string(size) + " is not a plausible GBA ROM";
        return r;
    }
    r.rom.data.resize(std::size_t(size));
    f.read(reinterpret_cast<char*>(r.rom.data.data()), size);
    if (!f) {
        r.error = "read failed";
        return r;
    }

    r.rom.file_sha1 = to_hex(sha1(r.rom.data.data(), r.rom.data.size()));

    bool cleaned = false;
    if (*kExpectedSha1) {
        std::uint8_t header[32];
        canonical_header(header);
        DumpFix fix = normalize_dump(r.rom.data, kExpectedSha1, kExpectedSize, header);
        if (fix.matched) {
            r.rom.data = std::move(fix.data);
            r.rom.fixes = std::move(fix.fixes);
            cleaned = true;
        } else if (has_copier_header(r.rom.data)) {
            r.rom.data.erase(r.rom.data.begin(), r.rom.data.begin() + 512);  // so the header reads right
        }
    }

    const auto& d = r.rom.data;
    auto& h = r.rom.header;
    h.title = read_ascii(d, 0xA0, 12);
    h.game_code = read_ascii(d, 0xAC, 4);
    h.maker = read_ascii(d, 0xB0, 2);
    h.version = d[0xBC];
    h.checksum = d[0xBD];
    h.checksum_ok = header_valid(d.data());
    r.rom.sha1 = cleaned ? std::string(kExpectedSha1) : r.rom.file_sha1;

    if (!cleaned) {
        for (const auto& o : game::kOtherReleases) {
            if (o.sha1 ? r.rom.file_sha1 == o.sha1 : h.game_code == o.game_code) {
                r.rom.other_release = o.name;
                break;
            }
        }
    }
    if (!h.checksum_ok && !cleaned) {
        r.error = "not a valid GBA ROM (header check failed)";
        return r;
    }

    if (*kExpectedSha1) {
        r.rom.hash_matches = cleaned;
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
