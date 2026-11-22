#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "game.h"

namespace supersaga {

// Fingerprint of the supported dump (see game.h). The ROM itself is never shipped.
inline constexpr const char* kExpectedSha1 = game::kExpectedSha1;
inline constexpr std::size_t kExpectedSize = game::kRomSize;

struct RomHeader {
    std::string title;      // 0xA0, 12 bytes
    std::string game_code;  // 0xAC, 4 bytes
    std::string maker;      // 0xB0, 2 bytes
    std::uint8_t version = 0;          // 0xBC
    std::uint8_t checksum = 0;         // 0xBD
    bool checksum_ok = false;
};

struct Rom {
    std::vector<std::uint8_t> data;
    RomHeader header;
    std::string sha1;
    bool hash_matches = false;
    // True when hash_matches was decided by the header only, because game::kExpectedSha1 is
    // not pinned yet. Shown to the user so an unverified dump is never reported as verified.
    bool header_only = false;
    // What load_rom changed to turn the file into the supported dump, for the user ("Removed a
    // 512-byte copier header"). Empty when the file was already the exact dump.
    std::string fixes;
    // SHA-1 of the file as read, before any fixes. sha1 above is always the cleaned-up data, so
    // saves and the stored ROM choice stay keyed to the one supported dump.
    std::string file_sha1;
    // Set when the file is a different version of the game ("the European release").
    std::string other_release;
};

struct RomLoadResult {
    bool ok = false;
    std::string error;
    Rom rom;
};

// Result of normalize_dump.
struct DumpFix {
    bool matched = false;
    std::vector<std::uint8_t> data;  // the cleaned-up ROM when matched
    std::string fixes;               // what was changed, empty when the input was already exact
};

// Tries the safe clean-ups that turn another dump of the same cartridge into the exact supported
// one: dropping a 512-byte copier header, cutting an overdump back to the real size, padding a
// trimmed dump back out (0xFF, then 0x00), and restoring header bytes (0xA0..0xBF) that a header
// fixer or flash-cart tool rewrote. A result is accepted only if its SHA-1 equals expected_sha1,
// so nothing else ever gets through. canonical_header is the 32 bytes at 0xA0 of the real dump.
DumpFix normalize_dump(const std::vector<std::uint8_t>& raw, const std::string& expected_sha1,
                       std::size_t expected_size, const std::uint8_t (&canonical_header)[32]);

// The 32 header bytes at 0xA0 of the supported dump, built from game.h.
void canonical_header(std::uint8_t (&out)[32]);

// Reads the file, parses the GBA cartridge header and hashes it.
// ok is true only when the file is a valid GBA ROM; check rom.hash_matches
// to know whether it is (or was cleaned up into) the exact dump this project targets.
RomLoadResult load_rom(const std::filesystem::path& path);

// Looks for a ROM next to the executable, in the working directory and in ./rom.
std::filesystem::path find_default_rom(const std::filesystem::path& exe_dir);

}  // namespace supersaga
