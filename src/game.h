#pragma once
// The one place that describes the game SuperSaga runs. Everything game-specific that the rest of
// the code needs (names shown to the user, ROM fingerprint, save layout) lives here, so the
// launcher, packaging and tests do not hard-code it.
//
// Target: Mario & Luigi: Superstar Saga, USA release (AGB-A88E), revision 0.
//
// Nothing here is game data: these are only identifiers and a fingerprint of the user's own ROM.

#include <cstddef>
#include <string_view>

namespace supersaga::game {

// Shown in the launcher, dialogs and error messages.
inline constexpr const char* kTitle = "Mario & Luigi: Superstar Saga";
inline constexpr const char* kRegion = "USA";
inline constexpr const char* kTitleWithRegion = "Mario & Luigi: Superstar Saga (USA)";
// Used for shortcuts, default export names, etc. (no characters that are invalid in file names).
inline constexpr const char* kFileSafeTitle = "Mario & Luigi - Superstar Saga";

// GBA cartridge header of the supported dump.
inline constexpr const char* kHeaderTitle = "MARIO&LUIGIU";  // 0xA0, 12 bytes
inline constexpr const char* kGameCode = "A88E";             // 0xAC, 4 bytes
inline constexpr unsigned kRevision = 0;                     // 0xBC
inline constexpr std::size_t kRomSize = 16 * 1024 * 1024;

// SHA-1 of the known-good dump (lowercase hex), checked against the maintainer's own cartridge
// dump on 2026-11-11 (MD5 4b1a5897d89d9e74ec7f630eefdfd435, CRC32 E718D850). If this is ever
// emptied, a ROM is accepted on a header match instead and the launcher labels it
// "header match (fingerprint not pinned yet)".
inline constexpr const char* kExpectedSha1 = "7c303cdde5061ee329296948060b875cb50ba410";

// Battery save: 64 Kbit (8 KiB) serial EEPROM (the ROM carries the EEPROM_V124 library string).
// The runtime picks the chip from that string; this is the size of the save file it writes.
inline constexpr std::size_t kSaveSize = 8 * 1024;
inline constexpr const char* kSaveChip = "EEPROM";
// Bytes the game writes at the start of its save, used to reject saves from other games. Empty
// disables the check. Checked on 2026-11-11 against the save the game creates on first boot: its
// first 64-bit EEPROM block is two zero bytes and "GPRLAM" ("MALRPG" stored back to front). It
// holds zero bytes, so the length is explicit.
inline constexpr std::string_view kSaveHeader{"\x00\x00" "GPRLAM", 8};

// Widescreen needs per-game support in src/gba/wide_level.cpp; until then the option is hidden
// and the picture stays 3:2.
inline constexpr bool kWidescreen = false;

}  // namespace supersaga::game
