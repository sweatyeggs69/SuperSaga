#include "app/platform.h"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <system_error>
#include <vector>

#include "app/config.h"
#include "app/fsutil.h"
#include "game.h"

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <sys/stat.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

// The app icon (rendered by supersaga_icon at build time, 512x512 PNG), written into shortcuts.
extern const unsigned char supersaga_image_icon[];
extern const unsigned supersaga_image_icon_size;

namespace supersaga {
namespace fs = std::filesystem;

namespace {
// Shortcuts carry the app's own name and icon. Early test builds named them after the game.
constexpr const char* kShortcutName = "SuperSaga";
constexpr const char* kLegacyShortcutName = game::kFileSafeTitle;

#if defined(_WIN32)
// Marker in the .lnk description so SuperSaga recognizes its own shortcut.
constexpr const wchar_t* kLnkMarker = L"(SuperSaga shortcut)";

fs::path known_folder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    fs::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) out = fs::path(p);
    CoTaskMemFree(p);
    return out;
}

struct ComInit {
    bool ok;
    ComInit() : ok(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {}
    ~ComInit() { if (ok) CoUninitialize(); }
};

// Reads a .lnk's description; empty if it is not a readable shortcut.
std::wstring lnk_description(const fs::path& lnk) {
    ComInit com;
    IShellLinkW* link = nullptr;
    std::wstring desc;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                reinterpret_cast<void**>(&link))))
        return desc;
    IPersistFile* file = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file)))) {
        if (SUCCEEDED(file->Load(lnk.wstring().c_str(), STGM_READ))) {
            wchar_t buf[INFOTIPSIZE] = {};
            if (SUCCEEDED(link->GetDescription(buf, INFOTIPSIZE))) desc = buf;
        }
        file->Release();
    }
    link->Release();
    return desc;
}

bool write_lnk(const fs::path& lnk, const fs::path& exe, std::string* error) {
    ComInit com;
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                reinterpret_cast<void**>(&link)))) {
        *error = "Windows could not create a shortcut object.";
        return false;
    }
    link->SetPath(exe.wstring().c_str());
    link->SetArguments(L"--play");
    link->SetWorkingDirectory(exe.parent_path().wstring().c_str());
    link->SetIconLocation(exe.wstring().c_str(), 0);
    std::wstring desc = fs::path(std::string(game::kTitle) + " ").wstring();
    desc += kLnkMarker;
    link->SetDescription(desc.c_str());
    IPersistFile* file = nullptr;
    bool ok = false;
    if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file)))) {
        ok = SUCCEEDED(file->Save(lnk.wstring().c_str(), TRUE));
        file->Release();
    }
    link->Release();
    if (!ok) *error = "Could not save " + path_utf8(lnk);
    return ok;
}
#endif

std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    return out + "'";
}

std::string xml_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            default: out += c;
        }
    }
    return out;
}
}  // namespace

VerifiedRom verify_rom(const fs::path& path) {
    VerifiedRom v;
    std::error_code ec;
    v.path = fs::absolute(path, ec);
    if (ec) v.path = path;
    if (!fs::exists(v.path, ec)) {
        v.error = "The ROM file could not be found:\n" + v.path.string();
        return v;
    }
    auto loaded = load_rom(v.path);
    if (!loaded.ok) {
        v.error = "That file is not a Game Boy Advance ROM.";
        return v;
    }
    if (!loaded.rom.hash_matches) {
        const auto& h = loaded.rom.header;
        v.error = "That ROM is not the supported dump.\n"
                  "Found: " + h.title + " [" + h.game_code + "] rev " + std::to_string(h.version) + "\n"
                  "Needed: " + std::string(game::kTitleWithRegion) + " [" + game::kGameCode + "] rev " +
                  std::to_string(game::kRevision) + "\n"
                  "SHA-1 " + loaded.rom.sha1;
        return v;
    }
    v.rom = std::move(loaded.rom);
    v.ok = true;
    return v;
}

fs::path save_path_for(const Config& config, const VerifiedRom& rom) {
    fs::path dir = config.pref_dir() / "saves";
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::path save = dir / (rom.rom.sha1 + ".sav");
    if (!fs::exists(save, ec)) {
        fs::path legacy = rom.path;
        legacy.replace_extension(".sav");
        if (fs::exists(legacy, ec)) fs::copy_file(legacy, save, ec);
    }
    return save;
}

fs::path executable_path() {
#if defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof buf;
    if (_NSGetExecutablePath(buf, &size) == 0) {
        std::error_code ec;
        fs::path p = fs::canonical(buf, ec);
        return ec ? fs::path(buf) : p;
    }
    return {};
#elif defined(_WIN32)
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return n ? fs::path(std::wstring(buf, n)) : fs::path();
#else
    std::error_code ec;
    return fs::read_symlink("/proc/self/exe", ec);
#endif
}

namespace {
constexpr const char* kShortcutId = "tech.mcclain.supersaga.shortcut";
constexpr const char* kOldShortcutId = "tech.mcclain.supersaga.play";  // earlier builds
}  // namespace

namespace {
fs::path shortcut_path_named(ShortcutPlace place, const char* name) {
#if defined(__APPLE__)
    fs::path dir = home_dir() / (place == ShortcutPlace::Desktop ? "Desktop" : "Applications");
    return dir / (std::string(name) + ".app");
#elif defined(_WIN32)
    fs::path dir = known_folder(place == ShortcutPlace::Desktop ? FOLDERID_Desktop : FOLDERID_Programs);
    return dir.empty() ? fs::path() : dir / (std::string(name) + ".lnk");
#else
    (void)name;  // the .desktop file name never changed; only its Name= line did
    fs::path dir = place == ShortcutPlace::Desktop ? home_dir() / "Desktop"
                                                   : home_dir() / ".local" / "share" / "applications";
    return dir / "supersaga-play.desktop";
#endif
}

// A shortcut from an older build, saved under the game's name. Empty where the name did not change.
fs::path legacy_shortcut_path(ShortcutPlace place) {
#if defined(__APPLE__) || defined(_WIN32)
    return shortcut_path_named(place, kLegacyShortcutName);
#else
    (void)place;
    return {};
#endif
}

bool exists_at(const fs::path& p) {
    std::error_code ec;
    return !p.empty() && fs::exists(fs::symlink_status(p, ec));
}

ShortcutState state_at(const fs::path& p) {
    if (!exists_at(p)) return ShortcutState::None;
#if defined(__APPLE__)
    std::ifstream f(p / "Contents" / "Info.plist");
    std::string plist((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    bool ours = plist.find(kShortcutId) != std::string::npos || plist.find(kOldShortcutId) != std::string::npos;
#elif defined(_WIN32)
    bool ours = lnk_description(p).find(kLnkMarker) != std::wstring::npos;
#else
    std::ifstream f(p);
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    bool ours = text.find("X-SuperSaga-Shortcut=true") != std::string::npos;
#endif
    return ours ? ShortcutState::Ours : ShortcutState::Foreign;
}

// Moves an older build's game-named shortcut to the Trash, if it is ours.
void retire_legacy_shortcut(ShortcutPlace place) {
    fs::path old = legacy_shortcut_path(place);
    if (!old.empty() && old != shortcut_path(place) && state_at(old) == ShortcutState::Ours) move_to_trash(old);
}

#if !defined(_WIN32)  // Windows shortcuts use the icon inside SuperSaga.exe
std::vector<unsigned char> icon_png() {
    return std::vector<unsigned char>(supersaga_image_icon, supersaga_image_icon + supersaga_image_icon_size);
}
#endif

#if defined(__APPLE__)
// A minimal .icns holding the 512x512 PNG (type ic09); Finder scales it for smaller sizes.
bool write_icns(const fs::path& out) {
    std::vector<unsigned char> png = icon_png();
    auto be32 = [](std::ofstream& f, std::uint32_t v) {
        const char b[4] = {char(v >> 24), char(v >> 16), char(v >> 8), char(v)};
        f.write(b, 4);
    };
    std::ofstream f(out, std::ios::binary | std::ios::trunc);
    f.write("icns", 4);
    be32(f, std::uint32_t(8 + 8 + png.size()));
    f.write("ic09", 4);
    be32(f, std::uint32_t(8 + png.size()));
    f.write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size()));
    return bool(f);
}
#elif !defined(_WIN32)
// The icon for .desktop shortcuts, in the user's icon folder. Referenced by absolute path so it
// shows up without an icon-cache refresh, also when SuperSaga runs as an AppImage.
fs::path write_linux_icon() {
    fs::path base;
    if (const char* x = std::getenv("XDG_DATA_HOME"); x && *x) base = utf8_path(x);
    else base = home_dir() / ".local" / "share";
    fs::path dir = base / "icons" / "hicolor" / "512x512" / "apps";
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::path out = dir / "supersaga.png";
    std::vector<unsigned char> png = icon_png();
    std::ofstream f(out, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size()));
    return f ? out : fs::path();
}
#endif
}  // namespace

fs::path shortcut_path(ShortcutPlace place) { return shortcut_path_named(place, kShortcutName); }

ShortcutState shortcut_state(ShortcutPlace place) {
    ShortcutState st = state_at(shortcut_path(place));
    // An older build's shortcut still counts as ours, so the launcher offers to update it.
    if (st == ShortcutState::None && state_at(legacy_shortcut_path(place)) == ShortcutState::Ours) return ShortcutState::Ours;
    return st;
}

ShortcutResult remove_shortcut(ShortcutPlace place) {
    ShortcutResult r;
    fs::path p = shortcut_path(place);
    ShortcutState st = state_at(p);
    if (st == ShortcutState::None) {
        fs::path old = legacy_shortcut_path(place);
        if (!old.empty() && state_at(old) == ShortcutState::Ours) { p = old; st = ShortcutState::Ours; }
    }
    switch (st) {
        case ShortcutState::None: r.ok = true; r.message = "There is no shortcut to remove."; return r;
        case ShortcutState::Foreign: r.message = p.string() + " was not created by SuperSaga, so it was left alone."; return r;
        case ShortcutState::Ours: break;
    }
    fs::path where;
    if (!move_to_trash(p, &where)) { r.message = "Could not move " + p.string() + " to the Trash."; return r; }
    retire_legacy_shortcut(place);
    r.ok = true;
    r.path = where;
    r.message = "Shortcut moved to the Trash.";
    return r;
}

ShortcutResult create_shortcut(ShortcutPlace place, bool replace_foreign) {
    ShortcutResult r;
    fs::path exe = executable_path();
    if (exe.empty()) {
        r.message = "Could not find the SuperSaga program on disk.";
        return r;
    }
    fs::path target = shortcut_path(place);
    if (target.empty()) {
        r.message = "Creating shortcuts is not available on this system yet.";
        return r;
    }
    ShortcutState state = state_at(target);
#if defined(__APPLE__)
    // Never offer to replace SuperSaga.app itself when it lives where the shortcut would go.
    for (fs::path p = exe.parent_path(); p.has_relative_path(); p = p.parent_path()) {
        std::error_code ec;
        if (p.extension() == ".app" && exists_at(target) && fs::equivalent(p, target, ec)) {
            r.message = "SuperSaga itself is already at " + target.string() + ".";
            return r;
        }
    }
#endif
    if (state == ShortcutState::Foreign) {
        if (!replace_foreign) {
            r.needs_confirm = true;
            r.path = target;
            r.message = target.string() + " already exists and was not created by SuperSaga. Replace it? "
                        "(It will be moved to the Trash.)";
            return r;
        }
        if (!move_to_trash(target)) {
            r.message = "Could not move the existing " + target.string() + " to the Trash.";
            return r;
        }
    }
    bool updating = state == ShortcutState::Ours || shortcut_state(place) == ShortcutState::Ours;
    std::error_code ec;
#if defined(__APPLE__)
    fs::path contents = target / "Contents";
    // Ours: rewrite the files in place (same bundle, so the Dock/Finder entry stays put).
    fs::create_directories(contents / "MacOS", ec);
    fs::create_directories(contents / "Resources", ec);
    if (ec) {
        r.message = "Could not create " + target.string() + ": " + ec.message();
        return r;
    }
    std::string launch;
    fs::path bundle;
    for (fs::path p = exe.parent_path(); p.has_relative_path(); p = p.parent_path()) {
        if (p.extension() == ".app") { bundle = p; break; }
    }
    if (!bundle.empty()) {
        launch = "exec /usr/bin/open -a " + shell_quote(bundle.string()) + " --args --play\n";
    } else {
        launch = "exec " + shell_quote(exe.string()) + " --play \"$@\"\n";
    }
    // The SuperSaga icon, also when running a plain build/supersaga that has no bundle to copy from.
    {
        fs::path icon = bundle.empty() ? fs::path() : bundle / "Contents" / "Resources" / "AppIcon.icns";
        if (icon.empty() || !fs::exists(icon, ec) ||
            !fs::copy_file(icon, contents / "Resources" / "AppIcon.icns", fs::copy_options::overwrite_existing, ec))
            write_icns(contents / "Resources" / "AppIcon.icns");
    }
    {
        std::ofstream s(contents / "MacOS" / "play", std::ios::trunc);
        s << "#!/bin/sh\n# Created by SuperSaga: starts the game directly.\n" << launch;
    }
    chmod((contents / "MacOS" / "play").c_str(), 0755);
    {
        std::ofstream p(contents / "Info.plist", std::ios::trunc);
        p << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
             "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
             "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
             "<plist version=\"1.0\"><dict>\n"
             "  <key>CFBundleName</key><string>" << xml_escape(kShortcutName) << "</string>\n"
             "  <key>CFBundleDisplayName</key><string>" << xml_escape(kShortcutName) << "</string>\n"
             "  <key>CFBundleIdentifier</key><string>" << kShortcutId << "</string>\n"
             "  <key>CFBundleExecutable</key><string>play</string>\n"
             "  <key>CFBundleIconFile</key><string>AppIcon</string>\n"
             "  <key>CFBundlePackageType</key><string>APPL</string>\n"
             "  <key>CFBundleShortVersionString</key><string>1.0</string>\n"
             "  <key>LSMinimumSystemVersion</key><string>11.0</string>\n"
             "</dict></plist>\n";
    }
    // Let Finder/Launch Services notice the updated bundle.
    fs::last_write_time(target, fs::file_time_type::clock::now(), ec);
#elif defined(_WIN32)
    fs::create_directories(target.parent_path(), ec);
    std::string err;
    if (!write_lnk(target, exe, &err)) {  // IPersistFile::Save overwrites ours in place
        r.message = err;
        return r;
    }
#else
    fs::create_directories(target.parent_path(), ec);
    // Inside an AppImage the executable lives in a temporary mount; point at the AppImage file.
    fs::path launch = exe;
    if (const char* appimage = std::getenv("APPIMAGE"); appimage && *appimage) launch = utf8_path(appimage);
    fs::path icon = write_linux_icon();
    {
        std::ofstream d(target, std::ios::trunc);
        d << "[Desktop Entry]\nType=Application\nName=" << kShortcutName << "\nExec=\"" << launch.string()
          << "\" --play\nIcon=" << (icon.empty() ? std::string("supersaga") : icon.string()) << "\nStartupWMClass=SuperSaga\nTerminal=false\nCategories=Game;\nX-SuperSaga-Shortcut=true\n";
        if (!d) {
            r.message = "Could not write " + target.string();
            return r;
        }
    }
    chmod(target.c_str(), 0755);
#endif
    retire_legacy_shortcut(place);
    r.ok = true;
    r.path = target;
    r.message = std::string(updating ? "Shortcut updated:\n" : "Shortcut created:\n") + target.string();
    return r;
}

}  // namespace supersaga
