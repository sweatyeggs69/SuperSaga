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
};

struct RomLoadResult {
    bool ok = false;
    std::string error;
    Rom rom;
};

// Reads the file, parses the GBA cartridge header and hashes it.
// ok is true only when the file is a valid GBA ROM; check rom.hash_matches
// to know whether it is the exact dump this project targets.
RomLoadResult load_rom(const std::filesystem::path& path);

// Looks for a ROM next to the executable, in the working directory and in ./rom.
std::filesystem::path find_default_rom(const std::filesystem::path& exe_dir);

}  // namespace supersaga
