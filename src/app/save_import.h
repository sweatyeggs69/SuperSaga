#pragma once
// Importing and exporting battery saves from other emulators, plus save backups.
// Save size and signature come from game.h. No SDL dependency.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "game.h"

namespace supersaga {

inline constexpr std::size_t kSramSize = game::kSaveSize;

struct NormalizedSave {
    bool ok = false;
    std::string error;          // user-facing reason when !ok
    std::string format;         // what was recognized, e.g. "raw 8 KiB EEPROM", "64 KiB save (padded)"
    std::string warning;        // non-fatal note (e.g. the save is empty)
    std::vector<std::uint8_t> data;  // exactly kSramSize bytes when ok
};

// Converts a save file from mGBA, VBA-M, RetroArch (.srm), NO$GBA (raw), gpSP-style padded
// saves or DeSmuME (.dsv) into the game's raw save (game::kSaveSize bytes). Rejects save states and other save types
// with a clear message. `filename` is only used to recognize the format (extension).
NormalizedSave normalize_save(const std::vector<std::uint8_t>& file, const std::string& filename);

// True if the SRAM starts with the header this game writes on first boot (game::kSaveHeader).
// Always true while game::kSaveHeader is empty.
bool has_game_header(const std::vector<std::uint8_t>& sram);

// Copies `save` to <backups>/<stem>-YYYYmmdd-HHMMSS.sav and keeps the newest `keep` backups for
// that stem. Returns the backup path, or empty if there was nothing to back up / it failed.
std::filesystem::path backup_save(const std::filesystem::path& save, const std::filesystem::path& backups,
                                  std::size_t keep = 10);
// Backups for `stem`, newest first.
std::vector<std::filesystem::path> list_backups(const std::filesystem::path& backups, const std::string& stem);

bool read_file(const std::filesystem::path& p, std::vector<std::uint8_t>& out);
bool write_file_atomic(const std::filesystem::path& p, const std::vector<std::uint8_t>& data);

}  // namespace supersaga
