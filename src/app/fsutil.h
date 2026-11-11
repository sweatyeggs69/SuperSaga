#pragma once
// Small cross-platform file helpers (no SDL): UTF-8 paths, local time, file URLs, Trash.

#include <ctime>
#include <filesystem>
#include <string>

namespace supersaga {

// Paths are stored and passed around as UTF-8 strings (config, SDL dialogs, argv from SDL_main).
// On Windows std::filesystem::path(std::string) would use the ANSI code page, so convert explicitly.
std::filesystem::path utf8_path(const std::string& utf8);
std::string path_utf8(const std::filesystem::path& p);

std::tm local_time(std::time_t t);
std::filesystem::path home_dir();
// file:// URL for SDL_OpenURL (works for "C:\..." and "/...").
std::string file_url(const std::filesystem::path& p);

// Moves a file or folder to the user's Trash / Recycle Bin (never deletes). On success `where`
// receives the new location when known (macOS/Linux).
bool move_to_trash(const std::filesystem::path& p, std::filesystem::path* where = nullptr);

}  // namespace supersaga
