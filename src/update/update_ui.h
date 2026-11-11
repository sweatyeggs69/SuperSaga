#pragma once
// Update UI: the "Updates" settings tab, the launcher's "update available" notice and toasts.
// Settings: updates.check (bool, default true) and updates.skip (a version the user skipped).
#include <functional>

namespace supersaga {
class Config;
namespace update { class Updater; }

namespace update_ui {

// Call once at startup (after the window exists). Starts the automatic check when enabled.
void init(update::Updater& updater, Config& config);
// Call every frame on the main thread. Shows a toast when an automatic check finds an update.
void tick(update::Updater& updater, Config& config);
// The settings menu tab.
void draw_tab(update::Updater& updater, Config& config);
// A compact notice for the launcher card (draws nothing when there's no update).
void draw_launcher_notice(update::Updater& updater, Config& config);
// Called after the new version is installed: the app should save and quit; it is restarted by
// Updater::relaunch() once the app has shut down.
void set_restart_handler(std::function<void()> handler);

}  // namespace update_ui
}  // namespace supersaga
