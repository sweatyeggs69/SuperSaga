#include "update/update_ui.h"

#include <SDL3/SDL.h>
#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include "app/config.h"
#include "app/theme.h"
#include "app/toast.h"
#include "update/update_core.h"
#include "update/updater.h"

namespace supersaga::update_ui {

using update::State;
using update::Status;
using update::Updater;

namespace {

struct UiState {
    std::function<void()> on_restart;
    std::string toasted;        // version we already showed a toast for
    std::string install_error;
    bool open_popup = false;
};
UiState& ui() {
    static UiState s;
    return s;
}

std::string megabytes(double bytes) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f MB", bytes / (1024.0 * 1024.0));
    return buf;
}

void draw_notes(const std::string& notes, float height) {
    auto lines = update::format_notes(notes);
    if (lines.empty()) return;
    const auto& c = theme::palette();
    ImGui::BeginChild("##notes", ImVec2(0, height), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PushTextWrapPos(0);
    for (const auto& l : lines) {
        switch (l.kind) {
            case update::NoteLine::Heading: ImGui::TextColored(c.accent, "%s", l.text.c_str()); break;
            case update::NoteLine::Bullet:
                ImGui::Bullet();
                ImGui::TextWrapped("%s", l.text.c_str());
                break;
            case update::NoteLine::Blank: ImGui::Dummy(ImVec2(0, 4)); break;
            default: ImGui::TextWrapped("%s", l.text.c_str()); break;
        }
    }
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
}

void open_page(const Status& st) {
    SDL_OpenURL((st.page_url.empty() ? Updater::releases_page() : st.page_url).c_str());
}

// Status line, actions and release notes. Shared by the settings tab and the launcher popup.
void draw_panel(Updater& u, Config& config, bool popup) {
    const auto& c = theme::palette();
    Status st = u.status();
    bool newer = st.state == State::Available || st.state == State::Downloading || st.state == State::Verifying ||
                 st.state == State::Ready || (st.state == State::Failed && !st.latest.empty() && !st.asset.empty());
    ImGui::PushTextWrapPos(0);
    switch (st.state) {
        case State::Idle: ImGui::TextDisabled("Not checked yet."); break;
        case State::Checking: ImGui::TextUnformatted("Checking for updates..."); break;
        case State::UpToDate:
            ImGui::TextColored(c.good, "You have the latest version (%s).", st.current.c_str());
            break;
        case State::Available:
            ImGui::TextColored(c.highlight, "SuperSaga %s is available. You have %s.", st.latest.c_str(), st.current.c_str());
            break;
        case State::Downloading: {
            ImGui::Text("Downloading SuperSaga %s...", st.latest.c_str());
            std::string label = st.progress >= 0 && st.size
                                    ? megabytes(st.progress * double(st.size)) + " of " + megabytes(double(st.size))
                                    : std::string();
            ImGui::ProgressBar(st.progress >= 0 ? float(st.progress) : -1.0f * float(ImGui::GetTime()),
                               ImVec2(-1, 0), label.empty() ? nullptr : label.c_str());
            break;
        }
        case State::Verifying: ImGui::TextUnformatted("Checking the download..."); break;
        case State::Ready:
            ImGui::TextColored(c.good, "SuperSaga %s is downloaded and verified.", st.latest.c_str());
            ImGui::TextDisabled("SuperSaga saves your game, then restarts as the new version. Saves, save states "
                                "and settings are kept.");
            break;
        case State::Failed:
            if (st.latest.empty())  // the check itself didn't work: not alarming, it retries next launch
                ImGui::TextDisabled("%s", st.message.empty() ? "Couldn't check for updates right now." : st.message.c_str());
            else
                ImGui::TextColored(c.bad, "%s", st.message.empty() ? "The update failed." : st.message.c_str());
            break;
    }
    if (st.state == State::Available && !st.message.empty()) ImGui::TextColored(c.bad, "%s", st.message.c_str());
    if (!ui().install_error.empty()) ImGui::TextColored(c.bad, "%s", ui().install_error.c_str());
    if (newer && !st.can_install && !st.install_note.empty()) ImGui::TextDisabled("%s", st.install_note.c_str());
    ImGui::PopTextWrapPos();

    // Actions.
    bool installable = st.can_install && !st.asset.empty() && st.message.empty();
    if (st.state == State::Available) {
        if (installable) {
            if (ImGui::Button("Download and install")) {
                ui().install_error.clear();
                u.download();
            }
            ImGui::SameLine();
        }
        if (ImGui::Button(installable ? "View on GitHub" : "Open the download page")) open_page(st);
        ImGui::SameLine();
        bool skipped = config.get_string("updates.skip") == st.latest;
        if (!skipped && ImGui::Button("Skip this version")) {
            config.set_string("updates.skip", st.latest);
            if (popup) ImGui::CloseCurrentPopup();
        }
        if (skipped) ImGui::TextDisabled("(skipped: you won't be reminded about %s)", st.latest.c_str());
    } else if (st.state == State::Downloading) {
        if (ImGui::Button("Cancel")) u.cancel();
    } else if (st.state == State::Ready) {
        if (ImGui::Button("Restart and update")) {
            std::string err;
            if (u.install(&err)) {
                ui().install_error.clear();
                if (ui().on_restart) ui().on_restart();
            } else {
                ui().install_error = "Couldn't install the update: " + err + " Nothing was changed.";
            }
        }
    } else if (st.state == State::Failed) {
        if (ImGui::Button("Try again")) {
            ui().install_error.clear();
            if (newer && installable) u.download();
            else u.check(true);
        }
        if (newer) {
            ImGui::SameLine();
            if (ImGui::Button("Open the download page")) open_page(st);
        }
    }

    if (newer && !st.notes.empty()) {
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::TextColored(c.ink_muted, "What's new in %s", st.latest.c_str());
        draw_notes(st.notes, popup ? 220.0f : 200.0f);
    }
}

}  // namespace

void set_restart_handler(std::function<void()> handler) { ui().on_restart = std::move(handler); }

void init(Updater& u, Config& config) {
    Updater::cleanup_leftovers();
    if (config.get_bool("updates.check", true)) u.check(false);
}

void tick(Updater& u, Config& config) {
    Status st = u.status();
    if (st.state != State::Available || st.manual || st.latest == ui().toasted) return;
    ui().toasted = st.latest;
    if (config.get_string("updates.skip") == st.latest) return;
    toast::show("Update available", "SuperSaga " + st.latest + " is out. Open Settings (F1), Updates tab.", nullptr, 0, 0,
                6000, toast::Style::Info);
}

void draw_tab(Updater& u, Config& config) {
    theme::section("Version");
    ImGui::AlignTextToFramePadding();
    ImGui::Text("SuperSaga %s", u.status().current.c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(u.busy());
    if (ImGui::Button("Check for updates")) {
        ui().install_error.clear();
        u.check(true);
    }
    ImGui::EndDisabled();
    draw_panel(u, config, false);

    theme::section("Automatic checks");
    bool check = config.get_bool("updates.check", true);
    if (theme::toggle("Check for updates when SuperSaga opens", &check)) config.set_bool("updates.check", check);
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("SuperSaga asks GitHub whether a newer version is out. Nothing about you, your game or your "
                        "saves is sent, and nothing installs without your OK.");
    ImGui::PopTextWrapPos();
}

void draw_launcher_notice(Updater& u, Config& config) {
    const auto& c = theme::palette();
    Status st = u.status();
    bool show = (st.state == State::Available && config.get_string("updates.skip") != st.latest) ||
                st.state == State::Downloading || st.state == State::Verifying || st.state == State::Ready;
    if (show) {
        ImGui::PushFont(theme::font_small());
        std::string text = st.state == State::Ready ? "SuperSaga " + st.latest + " is ready to install"
                                                    : "SuperSaga " + st.latest + " is available";
        std::string button = st.state == State::Ready ? "Install..." : "Update...";
        float w = ImGui::CalcTextSize(text.c_str()).x + ImGui::GetStyle().ItemSpacing.x +
                  ImGui::CalcTextSize(button.c_str()).x + ImGui::GetStyle().FramePadding.x * 2;
        ImGui::SetCursorPosX(std::max(0.0f, (ImGui::GetWindowSize().x - w) * 0.5f));
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(c.highlight, "%s", text.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton(button.c_str())) ui().open_popup = true;
        ImGui::PopFont();
    }
    if (ui().open_popup) {
        ImGui::OpenPopup("Update SuperSaga");
        ui().open_popup = false;
    }
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowSize(ImVec2(std::min(560.0f, ds.x - 40), 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Update SuperSaga", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
        draw_panel(u, config, true);
        ImGui::Dummy(ImVec2(0, 4));
        if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

}  // namespace supersaga::update_ui
