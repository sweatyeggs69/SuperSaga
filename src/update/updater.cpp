#include "update/updater.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <vector>

#include "update/http.h"
#include "update/sha256.h"
#include "app/i18n.h"
#include "update/update_core.h"

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace supersaga::update {
namespace fs = std::filesystem;
using i18n::tr;
using i18n::trf;

namespace {

constexpr const char* kRepo = "sweatyeggs69/SuperSaga";
constexpr const char* kBundleId = "tech.mcclain.supersaga";

std::string env(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

[[maybe_unused]] std::string u8(const fs::path& p) {
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

bool is_local_test_feed(const std::string& url) {
    return url.rfind("http://127.0.0.1", 0) == 0 || url.rfind("http://localhost", 0) == 0;
}

[[maybe_unused]] fs::path running_executable() {
#if defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof buf;
    if (_NSGetExecutablePath(buf, &size) != 0) return {};
    std::error_code ec;
    fs::path p = fs::canonical(buf, ec);
    return ec ? fs::path(buf) : p;
#elif defined(_WIN32)
    std::wstring buf(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), DWORD(buf.size()));
    if (n == 0 || n >= buf.size()) return {};
    buf.resize(n);
    return fs::path(buf);
#else
    std::error_code ec;
    return fs::read_symlink("/proc/self/exe", ec);
#endif
}

bool folder_writable(const fs::path& dir) {
    std::error_code ec;
    fs::path probe = dir / (".supersaga-write-test-" + std::to_string(SDL_GetTicksNS()));
    if (!fs::create_directory(probe, ec) || ec) return false;
    fs::remove(probe, ec);
    return true;
}

// Where this copy of SuperSaga lives and where the new version is staged before the swap.
struct Plan {
    bool ok = false;
    std::string reason;  // user-facing, when !ok
    fs::path target;     // SuperSaga.app / SuperSaga.exe / the .AppImage file
    fs::path dir;        // its folder
    fs::path stage;      // staged new version, in the same folder (so the swap is a rename)
};

Plan make_plan() {
    Plan p;
    Platform platform = current_platform();
#if defined(__APPLE__)
    fs::path exe = running_executable();
    fs::path bundle = exe.parent_path().parent_path().parent_path();  // SuperSaga.app/Contents/MacOS/SuperSaga
    if (exe.empty() || bundle.extension() != ".app" || exe.parent_path().filename() != "MacOS") {
        p.reason = tr("This copy of SuperSaga isn't an app bundle, so it can't update itself.");
        return p;
    }
    std::string b = bundle.string();
    if (b.find("/AppTranslocation/") != std::string::npos) {
        p.reason = tr("macOS is running SuperSaga from a temporary location. Move SuperSaga to your Applications "
                      "folder, open it from there, and it can update itself.");
        return p;
    }
    p.target = bundle;
    p.dir = bundle.parent_path();
    p.stage = p.dir / ".SuperSaga-update.app";
    if (!folder_writable(p.dir)) {
        p.reason = b.rfind("/Volumes/", 0) == 0
                       ? std::string(tr("SuperSaga is running from the disk image. Drag it to Applications to get automatic updates."))
                       : trf("SuperSaga can't write to {0}, so it can't update itself there.", p.dir.string());
        return p;
    }
#elif defined(_WIN32)
    fs::path exe = running_executable();
    if (exe.empty()) {
        p.reason = tr("Couldn't find the SuperSaga program on disk.");
        return p;
    }
    p.target = exe;
    p.dir = exe.parent_path();
    p.stage = exe;
    p.stage += ".new";  // the new SuperSaga.exe is downloaded next to the running one (same disk)
    if (!folder_writable(p.dir)) {
        p.reason = trf("SuperSaga can't write to its folder ({0}), so it can't update itself there.", u8(p.dir));
        return p;
    }
#else
    if (platform != Platform::LinuxX64) {
        p.reason = tr("There are no SuperSaga downloads for this kind of computer yet.");
        return p;
    }
    std::string appimage = env("APPIMAGE");
    if (appimage.empty()) {
        p.reason = tr("Only the AppImage version of SuperSaga updates itself. Update this copy the way you installed it.");
        return p;
    }
    p.target = fs::path(appimage);
    p.dir = p.target.parent_path();
    p.stage = p.dir / ".SuperSaga-update.AppImage";
    if (!folder_writable(p.dir)) {
        p.reason = trf("SuperSaga can't write to {0}, so it can't update itself there.", p.dir.string());
        return p;
    }
#endif
    (void)platform;
    p.ok = true;
    return p;
}

fs::path work_dir() {
    std::error_code ec;
    fs::path t = fs::temp_directory_path(ec);
    if (ec) t = ".";
    return t / "supersaga-update";
}

#if defined(__APPLE__)
// Runs a command and waits. Returns the exit code (-1 if it couldn't start).
int run_command(const std::vector<std::string>& argv, std::string* output) {
    std::vector<const char*> args;
    for (const auto& a : argv) args.push_back(a.c_str());
    args.push_back(nullptr);
    SDL_Process* proc = SDL_CreateProcess(args.data(), true);
    if (!proc) return -1;
    size_t n = 0;
    int code = -1;
    void* out = SDL_ReadProcess(proc, &n, &code);
    if (output) *output = out ? std::string(static_cast<const char*>(out), n) : std::string();
    SDL_free(out);
    SDL_DestroyProcess(proc);
    return code;
}

std::string read_text(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}
#endif

}  // namespace

struct Updater::Shared {
    mutable std::mutex mutex;
    Status status;
    std::atomic<bool> cancel{false};
    std::atomic<bool> busy{false};
    Plan plan;
    std::string feed;
    bool trusted_feed = true;  // the real GitHub feed: downloads must come from this repo's releases
    // From the last check:
    std::string asset_url, sums_url, asset_digest;
    // Prepared by download():

    template <class F> void update(F&& f) {
        std::lock_guard<std::mutex> lock(mutex);
        f(status);
        ++status.serial;
    }
    void fail(const std::string& why) {
        update([&](Status& s) {
            s.state = State::Failed;
            s.message = why;
            s.progress = -1;
        });
    }
    HttpOptions http() const {
        HttpOptions o;
        o.user_agent = "SuperSaga/" + status.current + " (updater)";
        o.allow_plain_http = !trusted_feed;
        o.cancel = &cancel;
        return o;
    }
};

std::string Updater::feed_url() {
    std::string over = env("SUPERSAGA_UPDATE_URL");
    if (!over.empty() && (over.rfind("https://", 0) == 0 || is_local_test_feed(over))) return over;
    return std::string("https://api.github.com/repos/") + kRepo + "/releases/latest";
}

std::string Updater::releases_page() { return std::string("https://github.com/") + kRepo + "/releases/latest"; }

Updater::Updater(std::string current_version) : s_(std::make_shared<Shared>()) {
    s_->status.current = std::move(current_version);
    s_->feed = feed_url();
    s_->trusted_feed = s_->feed.rfind("https://api.github.com/repos/", 0) == 0;
    s_->plan = make_plan();
    s_->status.can_install = s_->plan.ok;
    s_->status.install_note = s_->plan.reason;
}

Updater::~Updater() {
    s_->cancel = true;
    // A download may be waiting on the network; don't hold up quitting. The worker only uses
    // the shared state, which it keeps alive.
    if (worker_.joinable()) worker_.detach();
}

Status Updater::status() const {
    std::lock_guard<std::mutex> lock(s_->mutex);
    return s_->status;
}

bool Updater::busy() const { return s_->busy; }

void Updater::start(void (*job)(const std::shared_ptr<Shared>&)) {
    if (s_->busy.exchange(true)) return;
    if (worker_.joinable()) worker_.join();
    s_->cancel = false;
    std::shared_ptr<Shared> s = s_;
    worker_ = std::thread([s, job] {
        job(s);
        s->busy = false;
    });
}

void Updater::cancel() { s_->cancel = true; }

void Updater::check(bool manual) {
    {
        std::lock_guard<std::mutex> lock(s_->mutex);
        if (s_->status.state == State::Ready || installed_) return;  // keep the staged update
    }
    s_->update([&](Status& st) { st.manual = manual; });
    start([](const std::shared_ptr<Shared>& s) {
        s->update([](Status& st) {
            st.state = State::Checking;
            st.message.clear();
        });
        std::string body, err;
        int http_status = 0;
        HttpOptions o = s->http();
        o.accept = "application/vnd.github+json";
        o.status_out = &http_status;
        if (!http_get(s->feed, &body, o, &err)) {
            if (http_status == 404) {  // the repository has no release yet
                s->update([](Status& st) {
                    st.state = State::UpToDate;
                    st.latest.clear();
                    st.message = tr("No releases have been published yet.");
                });
                return;
            }
            if (http_status == 403 || http_status == 429)
                err = tr("Couldn't check for updates right now (GitHub is limiting requests). "
                         "SuperSaga will try again the next time it opens.");
            else if (http_status >= 500)
                err = tr("Couldn't check for updates right now (GitHub had a problem). Try again later.");
            s->fail(tr(err.c_str()));
            return;
        }
        auto rel = parse_release_json(body, &err);
        if (!rel) {
            s->fail(tr(err.c_str()));
            return;
        }
        auto latest = parse_version(rel->tag);
        auto current = parse_version(s->status.current);
        if (!latest || !current) {
            s->fail(trf("Couldn't understand the version number \"{0}\".", rel->tag));
            return;
        }
        bool newer = !rel->draft && !rel->prerelease && compare_versions(*latest, *current) > 0;
        const Asset* asset = pick_asset(*rel, current_platform());
        const Asset* sums = rel->find("SHA256SUMS.txt");
        std::string bad_url;
        if (s->trusted_feed) {
            std::string prefix = std::string("https://github.com/") + kRepo + "/releases/download/";
            if (asset && asset->url.rfind(prefix, 0) != 0) bad_url = asset->url;
            if (sums && sums->url.rfind(prefix, 0) != 0) bad_url = sums->url;
        }
        {
            std::lock_guard<std::mutex> lock(s->mutex);
            s->asset_url = asset ? asset->url : std::string();
            s->asset_digest = asset ? asset->sha256 : std::string();
            s->sums_url = sums ? sums->url : std::string();
        }
        s->update([&](Status& st) {
            st.latest = to_string(*latest);
            st.notes = rel->notes;
            st.page_url = rel->page_url.empty() ? Updater::releases_page() : rel->page_url;
            st.asset = asset ? asset->name : std::string();
            st.size = asset ? asset->size : 0;
            st.progress = -1;
            st.message.clear();
            st.state = newer ? State::Available : State::UpToDate;
            if (newer && !asset) st.message = tr("This release has no download for your computer.");
            else if (newer && !sums) st.message = tr("This release has no checksum file, so it can't be installed automatically.");
            else if (newer && !bad_url.empty()) st.message = tr("This release's download is hosted somewhere unexpected, so it won't be installed automatically.");
        });
    });
}

void Updater::download() {
    {
        std::lock_guard<std::mutex> lock(s_->mutex);
        if (s_->status.state != State::Available && s_->status.state != State::Failed) return;
        if (s_->status.asset.empty() || s_->asset_url.empty() || s_->sums_url.empty() || !s_->plan.ok) return;
        if (s_->status.state == State::Available && !s_->status.message.empty()) return;  // can't install this one
    }
    start([](const std::shared_ptr<Shared>& s) {
        std::string asset_name, asset_url, sums_url, digest;
        std::uint64_t size;
        Plan plan;
        {
            std::lock_guard<std::mutex> lock(s->mutex);
            asset_name = s->status.asset;
            asset_url = s->asset_url;
            sums_url = s->sums_url;
            digest = s->asset_digest;
            size = s->status.size;
            plan = s->plan;
        }
        s->update([](Status& st) {
            st.state = State::Downloading;
            st.progress = 0;
            st.message.clear();
        });
        std::string err, sums;
        if (!http_get(sums_url, &sums, s->http(), &err)) {
            s->fail(trf("Couldn't download the checksums: {0}", tr(err.c_str())));
            return;
        }
        std::string expected = checksum_for(sums, asset_name);
        if (expected.empty()) {
            s->fail(trf("The release's checksum file doesn't list {0}, so it won't be installed.", asset_name));
            return;
        }

        std::error_code ec;
        fs::path work = work_dir();
        fs::remove_all(work, ec);
        fs::create_directories(work, ec);
#if !defined(__APPLE__)
        fs::path file = plan.stage;  // download straight next to the exe / AppImage (same disk)
#else
        fs::path file = work / asset_name;
#endif
        HttpOptions o = s->http();
        o.expected_size = size;
        o.progress = [s](std::uint64_t done, std::uint64_t total) {
            s->update([&](Status& st) { st.progress = total ? std::min(1.0, double(done) / double(total)) : -1; });
        };
        if (!http_download(asset_url, file, o, &err)) {
            s->fail(tr(err.c_str()));
            return;
        }

        s->update([](Status& st) {
            st.state = State::Verifying;
            st.progress = -1;
        });
        auto file_size = fs::file_size(file, ec);
        std::string got = sha256_file_hex(file);
        if ((size && !ec && file_size != size) || got != expected || (!digest.empty() && got != digest)) {
            fs::remove(file, ec);
            s->fail(tr("The download didn't match the release's SHA-256 checksum, so it was thrown away. "
                       "Nothing was changed. Try again later."));
            return;
        }

#if defined(__APPLE__)
        fs::path mnt = work / "mnt";
        fs::create_directories(mnt, ec);
        std::string out;
        if (run_command({"/usr/bin/hdiutil", "attach", "-nobrowse", "-readonly", "-noautoopen", "-mountpoint",
                         mnt.string(), file.string()},
                        &out) != 0) {
            s->fail(tr("Couldn't open the downloaded disk image."));
            return;
        }
        fs::path app;
        for (const auto& e : fs::directory_iterator(mnt, ec)) {
            if (e.path().extension() == ".app" &&
                read_text(e.path() / "Contents" / "Info.plist").find(kBundleId) != std::string::npos) {
                app = e.path();
                break;
            }
        }
        bool copied = false;
        if (!app.empty()) {
            fs::remove_all(plan.stage, ec);
            copied = run_command({"/usr/bin/ditto", app.string(), plan.stage.string()}, &out) == 0;
        }
        run_command({"/usr/bin/hdiutil", "detach", mnt.string(), "-force"}, &out);
        fs::remove_all(work, ec);
        if (app.empty()) {
            s->fail(tr("The downloaded disk image doesn't contain SuperSaga."));
            return;
        }
        if (!copied || !fs::exists(plan.stage / "Contents" / "MacOS", ec)) {
            fs::remove_all(plan.stage, ec);
            s->fail(tr("Couldn't copy the new version next to the current one."));
            return;
        }
#elif defined(_WIN32)
        // A single self-contained SuperSaga.exe: check it is a Windows executable (MZ / PE header).
        {
            char head[2] = {};
            std::ifstream f(file, std::ios::binary);
            f.read(head, sizeof head);
            if (!f || head[0] != 'M' || head[1] != 'Z') {
                f.close();
                fs::remove(file, ec);
                s->fail(tr("The download isn't a Windows program."));
                return;
            }
        }
        fs::remove_all(work, ec);
#else
        // AppImage (type 2): an ELF file with "AI\x02" at offset 8.
        {
            char head[11] = {};
            std::ifstream f(file, std::ios::binary);
            f.read(head, sizeof head);
            if (!f || head[0] != 0x7f || head[1] != 'E' || head[2] != 'L' || head[3] != 'F' || head[8] != 'A' ||
                head[9] != 'I' || head[10] != 2) {
                fs::remove(file, ec);
                s->fail(tr("The download isn't an AppImage."));
                return;
            }
        }
        if (chmod(file.c_str(), 0755) != 0) {
            fs::remove(file, ec);
            s->fail(tr("Couldn't make the new AppImage executable."));
            return;
        }
        fs::remove_all(work, ec);
#endif
        if (s->cancel) return;
        s->update([](Status& st) {
            st.state = State::Ready;
            st.progress = 1;
        });
    });
}

bool Updater::install(std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    Plan plan;
    {
        std::lock_guard<std::mutex> lock(s_->mutex);
        if (s_->status.state != State::Ready) return fail(tr("The update isn't ready yet."));
        plan = s_->plan;
    }
    if (busy()) return fail(tr("The update isn't ready yet."));
    std::error_code ec;
#if defined(__APPLE__)
    fs::path old = plan.dir / ".SuperSaga-old.app";
    fs::remove_all(old, ec);
    fs::rename(plan.target, old, ec);
    if (ec) return fail(trf("Couldn't move the current version aside: {0}", ec.message()));
    fs::rename(plan.stage, plan.target, ec);
    if (ec) {
        std::error_code ec2;
        fs::rename(old, plan.target, ec2);
        return fail(trf("Couldn't put the new version in place: {0}", ec.message()));
    }
#elif defined(_WIN32)
    // Windows lets a running program be renamed: SuperSaga.exe becomes SuperSaga.exe.old (deleted on the
    // next start) and the verified SuperSaga.exe.new takes its place.
    fs::path old = plan.target;
    old += ".old";
    fs::remove(old, ec);
    fs::rename(plan.target, old, ec);
    if (ec) return fail(trf("Couldn't move the current version aside: {0}", ec.message()));
    fs::rename(plan.stage, plan.target, ec);
    if (ec) {
        std::error_code e;
        fs::rename(old, plan.target, e);
        return fail(trf("Couldn't put the new version in place: {0}", ec.message()));
    }
#else
    fs::rename(plan.stage, plan.target, ec);
    if (ec) return fail(trf("Couldn't replace the AppImage: {0}", ec.message()));
#endif
    installed_ = true;
    return true;
}

void Updater::relaunch() {
    if (!installed_) return;
    Plan plan;
    {
        std::lock_guard<std::mutex> lock(s_->mutex);
        plan = s_->plan;
    }
#if defined(__APPLE__)
    std::string target = plan.target.string();
    const char* args[] = {"/usr/bin/open", "-n", target.c_str(), nullptr};
#elif defined(_WIN32)
    std::string target = u8(plan.target);
    const char* args[] = {target.c_str(), nullptr};
#else
    std::string target = plan.target.string();
    const char* args[] = {target.c_str(), nullptr};
#endif
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void*)args);
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
    if (SDL_Process* p = SDL_CreateProcessWithProperties(props)) SDL_DestroyProcess(p);
    SDL_DestroyProperties(props);
}

void Updater::cleanup_leftovers() {
    std::error_code ec;
    fs::remove_all(work_dir(), ec);
#if defined(__APPLE__)
    fs::path exe = running_executable();
    fs::path bundle = exe.parent_path().parent_path().parent_path();
    if (bundle.extension() == ".app") {
        fs::remove_all(bundle.parent_path() / ".SuperSaga-old.app", ec);
        fs::remove_all(bundle.parent_path() / ".SuperSaga-update.app", ec);
    }
#elif defined(_WIN32)
    fs::path exe = running_executable();
    fs::path dir = exe.parent_path();
    if (dir.empty()) return;
    fs::remove_all(dir / ".supersaga-update", ec);  // older zip-based updates
    fs::path leftover = exe;
    leftover += ".new";
    fs::remove(leftover, ec);
    leftover = exe;
    leftover += ".old";
    fs::remove(leftover, ec);
    fs::path manifest = dir / ".supersaga-update-old.txt";
    std::ifstream f(manifest);
    if (!f) return;
    std::string line;
    bool all = true;
    std::vector<std::string> keep;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        fs::path old = fs::path(std::u8string(line.begin(), line.end()));
        // Only ever delete "<something>.old" inside SuperSaga's own folder.
        if (old.extension() != ".old" || old.parent_path().native().rfind(dir.native(), 0) != 0) continue;
        if (fs::exists(old, ec) && !fs::remove(old, ec)) { all = false; keep.push_back(line); }
    }
    f.close();
    if (all) fs::remove(manifest, ec);
    else {
        std::ofstream o(manifest, std::ios::trunc);
        for (const auto& k : keep) o << k << '\n';
    }
#else
    std::string appimage = env("APPIMAGE");
    if (!appimage.empty()) fs::remove(fs::path(appimage).parent_path() / ".SuperSaga-update.AppImage", ec);
#endif
}

}  // namespace supersaga::update
