#include "app/config.h"

#include <SDL3/SDL.h>

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace supersaga {

namespace {
std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n"), e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}
}  // namespace

Config::Config(std::filesystem::path pref_dir) : pref_dir_(std::move(pref_dir)) {
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

}  // namespace supersaga
