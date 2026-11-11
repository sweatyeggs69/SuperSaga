#pragma once
// Keeps a secret (the RetroAchievements login token, never a password) out of settings.ini:
//   macOS:   the login Keychain (generic password, service "SuperSaga RetroAchievements")
//   Windows: Credential Manager (generic credential "SuperSaga/RetroAchievements/<user>")
//   Linux:   <pref dir>/retroachievements.token, readable only by the user (0600)

#include <filesystem>
#include <optional>
#include <string>

namespace supersaga::credentials {

bool store(const std::filesystem::path& pref_dir, const std::string& user, const std::string& secret);
std::optional<std::string> load(const std::filesystem::path& pref_dir, const std::string& user);
void erase(const std::filesystem::path& pref_dir, const std::string& user);
// Where the secret is kept, for the settings page ("your Keychain", ...).
const char* location();

}  // namespace supersaga::credentials
