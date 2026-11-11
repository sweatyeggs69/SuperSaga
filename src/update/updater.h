#pragma once
// In-app updates from GitHub Releases.
//
//   check()     asks GitHub for the latest release (background thread)
//   download()  fetches this platform's file and SHA256SUMS.txt, verifies the SHA-256 (and
//               GitHub's own asset digest when present), and stages the new version next to
//               the installed one
//   install()   swaps the staged version in (main thread); the app then quits and relaunch()
//               starts the new version after saves are flushed
//
// Only the app itself is replaced: SuperSaga.app (macOS), the single SuperSaga.exe (Windows), or the
// AppImage file (Linux). Settings, saves, save states, the ROM and the BIOS
// live elsewhere and are never touched. A copy that can't replace itself (read-only folder,
// running from the disk image, not an AppImage) still gets notified and is pointed at the
// release page.
//
// Testing: SUPERSAGA_UPDATE_URL overrides the release feed (plain http allowed for localhost only).

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace supersaga::update {

enum class State {
    Idle,         // nothing checked yet
    Checking,
    UpToDate,
    Available,    // a newer version exists
    Downloading,
    Verifying,    // checksum + unpacking/staging
    Ready,        // staged; install() will apply it
    Failed,       // see Status::message
};

struct Status {
    State state = State::Idle;
    std::string current;      // running version
    std::string latest;       // newest released version (when known)
    std::string notes;        // release notes (Markdown)
    std::string page_url;     // release page on GitHub
    std::string asset;        // file name for this platform ("" if the release has none)
    std::uint64_t size = 0;   // its size in bytes
    std::string message;      // user-facing detail (errors, "can't install here" reasons)
    double progress = -1;     // 0..1 while downloading, -1 when unknown
    bool manual = false;      // the last check was started by the user
    bool can_install = false; // this copy can replace itself (else: open the release page)
    std::string install_note; // why it can't, when !can_install
    unsigned serial = 0;      // changes whenever anything above changes
};

class Updater {
public:
    explicit Updater(std::string current_version);
    ~Updater();
    Updater(const Updater&) = delete;
    Updater& operator=(const Updater&) = delete;

    Status status() const;
    bool busy() const;

    void check(bool manual);
    void download();  // after Available
    void cancel();

    // After Ready. Replaces the installed app with the staged one; on failure everything is
    // put back as it was. The caller then quits normally and calls relaunch() at the very end.
    bool install(std::string* error);
    bool installed() const { return installed_; }
    // Starts the (new) app. Call after the old one has saved and shut down.
    void relaunch();

    // Removes leftovers of a previous update (old app copy, staging files). Call at startup.
    static void cleanup_leftovers();
    // The release feed: GitHub's latest-release API for this repository (or SUPERSAGA_UPDATE_URL).
    static std::string feed_url();
    static std::string releases_page();

    struct Shared;

private:
    std::shared_ptr<Shared> s_;
    std::thread worker_;
    bool installed_ = false;
    void start(void (*job)(const std::shared_ptr<Shared>&));
};

}  // namespace supersaga::update
