#include "app/config.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace supersaga {

namespace {
std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n"), e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

// UTF-8 to a path on every platform (std::filesystem::path(std::string) uses the ANSI code page on Windows).
std::filesystem::path from_utf8(const char* s) {
    const std::string u(s);
    return std::filesystem::path(std::u8string(u.begin(), u.end()));
}

std::string to_utf8(const std::filesystem::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

// The folder the player sees the app in: the AppImage's folder on Linux, the folder holding the
// .app bundle on macOS, the .exe's folder on Windows.
std::filesystem::path app_folder() {
#if defined(__linux__)
    if (const char* a = SDL_getenv("APPIMAGE"); a && *a) return from_utf8(a).parent_path();
#endif
    const char* base = SDL_GetBasePath();
    if (!base || !*base) return {};
    std::filesystem::path dir = from_utf8(base).parent_path();  // drop the trailing separator
#if defined(__APPLE__)
    for (std::filesystem::path p = dir; !p.empty() && p != p.root_path(); p = p.parent_path())
        if (p.extension() == ".app") return p.parent_path();
#endif
    return dir;
}
}  // namespace

std::filesystem::path portable_dir() {
    const std::filesystem::path app = app_folder();
    std::error_code ec;
    if (app.empty() || !std::filesystem::is_regular_file(app / "portable.txt", ec)) return {};
    const std::filesystem::path dir = app / "userdata";
    std::filesystem::create_directories(dir, ec);
    if (!std::filesystem::is_directory(dir, ec)) {
        const std::u8string shown = dir.u8string();
        std::fprintf(stderr, "%s: portable.txt found, but %s can't be created; using the user folder\n", "supersaga",
                     std::string(shown.begin(), shown.end()).c_str());
        return {};
    }
    return dir;
}

Config::Config(std::filesystem::path pref_dir) : pref_dir_(std::move(pref_dir)) {
    if (pref_dir_.empty()) {
        pref_dir_ = portable_dir();
        if (!pref_dir_.empty()) {
            portable_ = true;
            app_dir_ = pref_dir_.parent_path();
        }
    }
    if (pref_dir_.empty()) {
        if (char* p = SDL_GetPrefPath("SuperSaga", "SuperSaga")) {
            pref_dir_ = p;
            SDL_free(p);
        } else {
            pref_dir_ = ".";
        }
    }
    std::error_code ec;
    std::filesystem::create_directories(pref_dir_, ec);
    file_ = pref_dir_ / "settings.ini";
    load();
}

bool Config::load() {
    values_.clear();
    std::ifstream f(file_);
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        std::string t = trim(line);
        if (t.empty() || t[0] == '#' || t[0] == ';') continue;
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        values_[trim(t.substr(0, eq))] = trim(t.substr(eq + 1));
    }
    return true;
}

bool Config::save() const {
    std::filesystem::path tmp = file_;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) return false;
        f << "# SuperSaga settings\n";
        for (const auto& [k, v] : values_) f << k << " = " << v << '\n';
        if (!f) return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, file_, ec);
    return !ec;
}

std::string Config::get_string(const std::string& key, const std::string& def) const {
    auto it = values_.find(key);
    return it == values_.end() ? def : it->second;
}

int Config::get_int(const std::string& key, int def) const {
    auto it = values_.find(key);
    if (it == values_.end()) return def;
    char* end = nullptr;
    long v = std::strtol(it->second.c_str(), &end, 10);
    return end && *end == '\0' && !it->second.empty() ? int(v) : def;
}

bool Config::get_bool(const std::string& key, bool def) const {
    auto it = values_.find(key);
    if (it == values_.end()) return def;
    const std::string& v = it->second;
    if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
    if (v == "0" || v == "false" || v == "no" || v == "off") return false;
    return def;
}

float Config::get_float(const std::string& key, float def) const {
    auto it = values_.find(key);
    if (it == values_.end()) return def;
    char* end = nullptr;
    float v = std::strtof(it->second.c_str(), &end);
    return end && *end == '\0' && !it->second.empty() ? v : def;
}

void Config::set_string(const std::string& key, const std::string& value) {
    // Keep the file line-based: newlines are not allowed in values.
    std::string v = value;
    for (char& c : v)
        if (c == '\n' || c == '\r') c = ' ';
    auto it = values_.find(key);
    if (it != values_.end() && it->second == v) return;
    values_[key] = v;
    save();
}

void Config::set_int(const std::string& key, int value) { set_string(key, std::to_string(value)); }
void Config::set_bool(const std::string& key, bool value) { set_string(key, value ? "true" : "false"); }
void Config::set_float(const std::string& key, float value) {
    std::ostringstream s;
    s << value;
    set_string(key, s.str());
}

void Config::erase(const std::string& key) {
    if (values_.erase(key)) save();
}


std::string Config::get_file(const std::string& key) const {
    std::string v = get_string(key);
    if (v.empty() || !portable_) return v;
    const std::filesystem::path p = from_utf8(v.c_str());
    if (p.is_absolute() || p.has_root_name()) return v;
    return to_utf8((app_dir_ / p).lexically_normal());
}

void Config::set_file(const std::string& key, const std::string& utf8_path) {
    if (portable_ && !utf8_path.empty()) {
        const std::filesystem::path p = from_utf8(utf8_path.c_str());
        if (p.is_absolute()) {
            const std::filesystem::path rel = p.lexically_normal().lexically_relative(app_dir_.lexically_normal());
            if (!rel.empty() && *rel.begin() != "..") {
                set_string(key, to_utf8(rel));
                return;
            }
        }
    }
    set_string(key, utf8_path);
}

}  // namespace supersaga
