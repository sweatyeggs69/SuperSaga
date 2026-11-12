#include "update/update_ui.h"

#include <SDL3/SDL.h>
#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include "app/config.h"
#include "app/i18n.h"
#include "app/theme.h"
#include "app/toast.h"
#include "update/update_core.h"
#include "update/updater.h"

namespace supersaga::update_ui {
using i18n::tr;
using i18n::trf;

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
        case State::Idle: ImGui::TextDisabled("%s", tr("Not checked yet.")); break;
        case State::Checking: ImGui::TextUnformatted(tr("Checking for updates...")); break;
        case State::UpToDate:
            ImGui::TextColored(c.good, "%s", trf("You have the latest version ({0}).", st.current).c_str());
            break;
        case State::Available:
            ImGui::TextColored(c.highlight, "%s", trf("SuperSaga {0} is available. You have {1}.", st.latest, st.current).c_str());
            break;
        case State::Downloading: {
            ImGui::TextUnformatted(trf("Downloading SuperSaga {0}...", st.latest).c_str());
            std::string label = st.progress >= 0 && st.size
                                    ? trf("{0} of {1}", megabytes(st.progress * double(st.size)), megabytes(double(st.size)))
                                    : std::string();
            ImGui::ProgressBar(st.progress >= 0 ? float(st.progress) : -1.0f * float(ImGui::GetTime()),
                               ImVec2(-1, 0), label.empty() ? nullptr : label.c_str());
            break;
        }
        case State::Verifying: ImGui::TextUnformatted(tr("Checking the download...")); break;
        case State::Ready:
            ImGui::TextColored(c.good, "%s", trf("SuperSaga {0} is downloaded and verified.", st.latest).c_str());
            ImGui::TextDisabled("%s", tr("SuperSaga saves your game, then restarts as the new version. Saves, save states "
                                         "and settings are kept."));
            break;
        case State::Failed:
            if (st.latest.empty())  // the check itself didn't work: not alarming, it retries next launch
                ImGui::TextDisabled("%s", st.message.empty() ? tr("Couldn't check for updates right now.") : tr(st.message.c_str()));
            else
                ImGui::TextColored(c.bad, "%s", st.message.empty() ? tr("The update failed.") : tr(st.message.c_str()));
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
            if (ImGui::Button(tr("Download and install"))) {
                ui().install_error.clear();
                u.download();
            }
            ImGui::SameLine();
        }
        if (ImGui::Button(installable ? tr("View on GitHub") : tr("Open the download page"))) open_page(st);
        ImGui::SameLine();
        bool skipped = config.get_string("updates.skip") == st.latest;
        if (!skipped && ImGui::Button(tr("Skip this version"))) {
            config.set_string("updates.skip", st.latest);
            if (popup) ImGui::CloseCurrentPopup();
        }
        if (skipped) ImGui::TextDisabled("%s", trf("(skipped: you won't be reminded about {0})", st.latest).c_str());
    } else if (st.state == State::Downloading) {
        if (ImGui::Button(tr("Cancel"))) u.cancel();
    } else if (st.state == State::Ready) {
        if (ImGui::Button(tr("Restart and update"))) {
            std::string err;
            if (u.install(&err)) {
                ui().install_error.clear();
                if (ui().on_restart) ui().on_restart();
            } else {
                ui().install_error = trf("Couldn't install the update: {0} Nothing was changed.", tr(err.c_str()));
            }
        }
    } else if (st.state == State::Failed) {
        if (ImGui::Button(tr("Try again"))) {
            ui().install_error.clear();
            if (newer && installable) u.download();
            else u.check(true);
        }
        if (newer) {
            ImGui::SameLine();
            if (ImGui::Button(tr("Open the download page"))) open_page(st);
        }
    }

    if (newer && !st.notes.empty()) {
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::TextColored(c.ink_muted, "%s", trf("What's new in {0}", st.latest).c_str());
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
    toast::show(tr("Update available"), trf("SuperSaga {0} is out. Open Settings (F1), Updates tab.", st.latest), nullptr, 0, 0,
                6000, toast::Style::Info);
}

void draw_tab(Updater& u, Config& config) {
    theme::section(tr("Version"));
    ImGui::AlignTextToFramePadding();
    ImGui::Text("SuperSaga %s", u.status().current.c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(u.busy());
    if (ImGui::Button(tr("Check for updates"))) {
        ui().install_error.clear();
        u.check(true);
    }
    ImGui::EndDisabled();
    draw_panel(u, config, false);

    theme::section(tr("Automatic checks"));
    bool check = config.get_bool("updates.check", true);
    if (theme::toggle(tr("Check for updates when SuperSaga opens"), &check)) config.set_bool("updates.check", check);
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("SuperSaga asks GitHub whether a newer version is out. Nothing about you, your game or your "
                                 "saves is sent, and nothing installs without your OK."));
    ImGui::PopTextWrapPos();
}

void draw_launcher_notice(Updater& u, Config& config) {
    const auto& c = theme::palette();
    Status st = u.status();
    bool show = (st.state == State::Available && config.get_string("updates.skip") != st.latest) ||
                st.state == State::Downloading || st.state == State::Verifying || st.state == State::Ready;
    if (show) {
        ImGui::PushFont(theme::font_small());
        std::string text = st.state == State::Ready ? trf("SuperSaga {0} is ready to install", st.latest)
                                                    : trf("SuperSaga {0} is available", st.latest);
        std::string button = st.state == State::Ready ? tr("Install...") : tr("Update...");
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
        ImGui::OpenPopup(tr("Update SuperSaga"));
        ui().open_popup = false;
    }
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowSize(ImVec2(std::min(560.0f, ds.x - 40), 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal(tr("Update SuperSaga"), nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
        draw_panel(u, config, true);
        ImGui::Dummy(ImVec2(0, 4));
        if (ImGui::Button(tr("Close"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

}  // namespace supersaga::update_ui
