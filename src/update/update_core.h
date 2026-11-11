#pragma once
// Update logic that needs no network, UI or SDL, so it can be unit tested: release versions,
// the GitHub "latest release" JSON, picking this platform's download, SHA256SUMS.txt and zip
// extraction (Windows updates).
//
// Release files (built by .github/workflows/build.yml; keep these names in sync):
//   SuperSaga-<version>-macOS.dmg, SuperSaga-<version>-Windows-x64.exe, SuperSaga-<version>-Linux-x64.AppImage,
//   SHA256SUMS.txt ("<sha256>  <file name>" per line).

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace supersaga::update {

// ---- versions ----
struct Version {
    int major = 0, minor = 0, patch = 0;
    std::string pre;  // "beta.1" in 1.2.0-beta.1; a pre-release sorts before the release
};
// Accepts "1.2.3", "v1.2.3", "1.2", "1.2.3-rc.1". nullopt if it doesn't start with a number.
std::optional<Version> parse_version(const std::string& text);
// <0, 0, >0 like strcmp.
int compare_versions(const Version& a, const Version& b);
std::string to_string(const Version& v);

// ---- GitHub release ----
struct Asset {
    std::string name, url;  // url: browser_download_url
    std::uint64_t size = 0;
    std::string sha256;     // from the asset's "digest" ("sha256:<hex>") when GitHub provides it
};
struct Release {
    std::string tag, name, notes, page_url;  // notes: the release body (Markdown)
    bool draft = false, prerelease = false;
    std::vector<Asset> assets;
    const Asset* find(const std::string& name) const;
};
// Parses https://api.github.com/repos/<owner>/<repo>/releases/latest. nullopt + error on failure.
std::optional<Release> parse_release_json(const std::string& json, std::string* error = nullptr);

enum class Platform { MacOS, Windows, LinuxX64, Other };
Platform current_platform();
// Name of the release file for a platform and version: "SuperSaga-1.2.0-macOS.dmg". Empty for Other.
std::string asset_name(Platform p, const std::string& version);
// The download for this platform in a release (matched by suffix, so the version inside the
// file name doesn't have to equal the tag exactly). nullptr if there is none.
const Asset* pick_asset(const Release& r, Platform p);

// ---- checksums ----
// Finds the line for `file` in SHA256SUMS.txt text ("<hex>  name" or "<hex> *name"). Returns
// the lowercase hex digest, or "" when the file isn't listed.
std::string checksum_for(const std::string& sums_text, const std::string& file);

// ---- zip (Windows updates) ----
// Extracts every file in `zip` under `dest` (created if needed). Rejects absolute paths and
// "..". Supports stored and deflated entries (deflate needs zlib; SUPERSAGA_HAVE_ZLIB). Verifies
// each entry's CRC-32. On failure returns false with a user-facing reason.
bool extract_zip(const std::filesystem::path& zip, const std::filesystem::path& dest, std::string* error);
bool zip_supported();

// ---- release notes ----
// Release notes (GitHub Markdown) as simple display lines.
struct NoteLine {
    enum Kind { Text, Heading, Bullet, Blank } kind = Text;
    std::string text;
};
std::vector<NoteLine> format_notes(const std::string& markdown);

}  // namespace supersaga::update
