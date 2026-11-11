#include "app/toast.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <deque>
#include <vector>

#include "app/theme.h"

namespace supersaga::toast {
namespace {

constexpr int kMaxVisible = 4;
constexpr Uint64 kFadeNs = 250'000'000;  // fade in / out

struct Toast {
    std::string title, body;
    std::vector<std::uint8_t> rgba;
    int w = 0, h = 0;
    Uint64 duration_ns = 0;
    Style style = Style::Info;
    Uint64 shown_at = 0;  // 0 = still queued
    SDL_Texture* tex = nullptr;
    unsigned id = 0;
};

SDL_Renderer* g_renderer = nullptr;
std::deque<Toast> g_toasts;
unsigned g_counter = 0;

void release(Toast& t) {
    if (t.tex) SDL_DestroyTexture(t.tex);
    t.tex = nullptr;
}

ImVec4 accent(Style s) {
    const auto& c = theme::palette();
    switch (s) {
        case Style::Success: return c.good;
        case Style::Warning: return c.bad;
        case Style::Achievement: return c.highlight;
        default: return c.accent;
    }
}

}  // namespace

void show(const std::string& title, const std::string& body, const std::uint8_t* rgba, int width, int height,
          int duration_ms, Style style) {
    Toast t;
    t.title = title;
    t.body = body;
    if (rgba && width > 0 && height > 0) {
        t.rgba.assign(rgba, rgba + size_t(width) * size_t(height) * 4);
        t.w = width;
        t.h = height;
    }
    t.duration_ns = Uint64(std::max(500, duration_ms)) * 1'000'000ull;
    t.style = style;
    t.id = ++g_counter;
    g_toasts.push_back(std::move(t));
    // Keep the queue bounded (e.g. a burst of achievement unlocks).
    while (g_toasts.size() > 32) {
        release(g_toasts.front());
        g_toasts.pop_front();
    }
}

void set_renderer(SDL_Renderer* renderer) {
    if (!renderer)
        for (Toast& t : g_toasts) release(t);
    g_renderer = renderer;
}

bool active() { return !g_toasts.empty(); }

void clear() {
    for (Toast& t : g_toasts) release(t);
    g_toasts.clear();
}

void draw() {
    if (g_toasts.empty()) return;
    const Uint64 now = SDL_GetTicksNS();
    // Expire finished toasts, then start queued ones while there is room.
    for (auto it = g_toasts.begin(); it != g_toasts.end();) {
        if (it->shown_at && now >= it->shown_at + it->duration_ns + 2 * kFadeNs) {
            release(*it);
            it = g_toasts.erase(it);
        } else {
            ++it;
        }
    }
    int visible = 0;
    for (Toast& t : g_toasts) {
        if (visible >= kMaxVisible) break;
        if (!t.shown_at) t.shown_at = now;
        ++visible;
    }

    const auto& c = theme::palette();
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    float y = 16.0f;
    int shown = 0;
    for (Toast& t : g_toasts) {
        if (!t.shown_at || shown >= kMaxVisible) break;
        ++shown;
        Uint64 age = now - t.shown_at;
        float alpha = 1.0f;
        if (age < kFadeNs) alpha = float(age) / float(kFadeNs);
        else if (age > t.duration_ns + kFadeNs) alpha = 1.0f - float(age - t.duration_ns - kFadeNs) / float(kFadeNs);
        alpha = std::clamp(alpha, 0.0f, 1.0f);

        if (!t.rgba.empty() && !t.tex && g_renderer) {
            t.tex = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, t.w, t.h);
            if (t.tex) {
                SDL_UpdateTexture(t.tex, nullptr, t.rgba.data(), t.w * 4);
                SDL_SetTextureScaleMode(t.tex, t.w <= 64 ? SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_LINEAR);
                SDL_SetTextureBlendMode(t.tex, SDL_BLENDMODE_BLEND);
            }
        }

        char id[32];
        std::snprintf(id, sizeof id, "##toast%u", t.id);
        ImGui::SetNextWindowPos(ImVec2(ds.x - 16.0f, y), ImGuiCond_Always, ImVec2(1, 0));
        ImGui::SetNextWindowBgAlpha(0.94f * alpha);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 12));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 16.0f);
        ImGui::Begin(id, nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
        // Always above other windows (e.g. the F1 menu) without taking focus or input.
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (t.tex) {
            float hgt = 80.0f, wdt = hgt * float(t.w) / float(t.h);
            if (wdt > 140.0f) { wdt = 140.0f; hgt = wdt * float(t.h) / float(t.w); }
            ImGui::Image((ImTextureID)(intptr_t)t.tex, ImVec2(wdt, hgt));
            ImGui::SameLine();
            // Center the text block vertically against the thumbnail.
            float text_h = ImGui::CalcTextSize(t.title.c_str(), nullptr, false, 300.0f).y;
            if (!t.body.empty()) {
                ImGui::PushFont(theme::font_small());
                text_h += ImGui::GetStyle().ItemSpacing.y + ImGui::CalcTextSize(t.body.c_str(), nullptr, false, 320.0f).y;
                ImGui::PopFont();
            }
            if (text_h < hgt) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (hgt - text_h) * 0.5f);
        }
        ImGui::BeginGroup();
        ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddCircleFilled(ImVec2(p.x + 8, p.y + 11), t.style == Style::Achievement ? 6.0f : 5.0f,
                            theme::u32(accent(t.style), alpha));
        ImGui::SetCursorScreenPos(ImVec2(p.x + 22, p.y));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 300.0f);
        ImGui::TextUnformatted(t.title.c_str());
        ImGui::PopTextWrapPos();
        if (!t.body.empty()) {
            ImGui::PushFont(theme::font_small());
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 320.0f);
            ImGui::TextColored(c.ink_muted, "%s", t.body.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopFont();
        }
        ImGui::EndGroup();
        y += ImGui::GetWindowSize().y + 8.0f;
        ImGui::End();
        ImGui::PopStyleVar(3);
    }
}

}  // namespace supersaga::toast
