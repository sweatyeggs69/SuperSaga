#pragma once
// On-screen notifications ("toasts"): stacked in the top-right corner, queued, drawn with Dear
// ImGui in the SuperSaga theme, readable over gameplay, and never take keyboard/mouse/gamepad input.
//
// Usage: supersaga::toast::show("State saved to slot 3", "Vegetable Valley", rgba, 240, 160);
// The host calls set_renderer() once and draw() inside every ImGui frame (launcher, menus and
// in game when active() is true).

#include <cstdint>
#include <string>

struct SDL_Renderer;

namespace supersaga::toast {

enum class Style { Info, Success, Warning, Achievement };

// title: first line (bold). body: optional second line. rgba: optional image, width x height
// pixels, 4 bytes per pixel in R,G,B,A order (any size; shown scaled to fit 80 px tall), copied
// immediately. duration_ms: time fully visible (fades are added). Thread: call from the main thread.
void show(const std::string& title, const std::string& body = {}, const std::uint8_t* rgba = nullptr,
          int width = 0, int height = 0, int duration_ms = 3000, Style style = Style::Info);

// Needed to create image textures. Call once after the renderer exists (and with nullptr
// before destroying it).
void set_renderer(SDL_Renderer* renderer);
// True while any toast is visible or queued.
bool active();
// Draws the visible toasts. Call between ImGui::NewFrame() and ImGui::Render().
void draw();
void clear();

}  // namespace supersaga::toast
