#include "app/fsutil.h"

#include <cstdlib>
#include <fstream>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#endif

namespace supersaga {
namespace fs = std::filesystem;

fs::path utf8_path(const std::string& utf8) {
    std::u8string u(utf8.begin(), utf8.end());
    return fs::path(u);
}

std::string path_utf8(const fs::path& p) {
    std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

std::tm local_time(std::time_t t) {
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return tm;
}

fs::path home_dir() {
#if defined(_WIN32)
    PWSTR p = nullptr;
    fs::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &p))) out = fs::path(p);
    CoTaskMemFree(p);
    return out;
#else
    const char* h = std::getenv("HOME");
    return h ? utf8_path(h) : fs::path();
#endif
}

std::string file_url(const fs::path& p) {
    std::u8string g = p.generic_u8string();
    std::string s(g.begin(), g.end());
    return std::string("file://") + (s.rfind('/', 0) == 0 ? "" : "/") + s;
}

bool move_to_trash(const fs::path& p, fs::path* where) {
    std::error_code ec;
    if (!fs::exists(fs::symlink_status(p, ec))) return false;
#if defined(_WIN32)
    // SHFileOperation needs a double-NUL-terminated list.
    std::wstring from = fs::absolute(p, ec).wstring();
    from.push_back(L'\0');
    from.push_back(L'\0');
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    bool ok = SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
    if (ok && where) *where = fs::path();
    return ok;
#elif defined(__APPLE__)
    fs::path trash = home_dir() / ".Trash";
    fs::create_directories(trash, ec);
    fs::path dst = trash / p.filename();
    for (int n = 2; fs::exists(fs::symlink_status(dst, ec)); ++n)
        dst = trash / (p.stem().string() + " " + std::to_string(n) + p.extension().string());
    fs::rename(p, dst, ec);
    if (ec) return false;
    if (where) *where = dst;
    return true;
#else
    // freedesktop.org Trash spec: files/ plus an info/<name>.trashinfo record.
    const char* xdg = std::getenv("XDG_DATA_HOME");
    fs::path base = (xdg && *xdg) ? utf8_path(xdg) : home_dir() / ".local" / "share";
    fs::path files = base / "Trash" / "files", info = base / "Trash" / "info";
    fs::create_directories(files, ec);
    fs::create_directories(info, ec);
    std::string name = p.filename().string();
    for (int n = 2; fs::exists(fs::symlink_status(files / name, ec)) || fs::exists(info / (name + ".trashinfo"), ec);
         ++n)
        name = p.stem().string() + "." + std::to_string(n) + p.extension().string();
    {
        std::time_t t = std::time(nullptr);
        std::tm tm = local_time(t);
        char stamp[32];
        std::strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%S", &tm);
        std::ofstream ti(info / (name + ".trashinfo"));
        ti << "[Trash Info]\nPath=" << fs::absolute(p, ec).string() << "\nDeletionDate=" << stamp << "\n";
    }
    fs::rename(p, files / name, ec);
    if (ec) {
        fs::remove(info / (name + ".trashinfo"), ec);
        return false;
    }
    if (where) *where = files / name;
    return true;
#endif
}

}  // namespace supersaga
