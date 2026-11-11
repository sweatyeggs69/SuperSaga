#pragma once
// The settings menu: every display and controls option, opened with F1 in game and from the
// launcher. Owns the display.* and input.* settings in supersaga::Config; other parts of the app
// can add their own tabs (for example the launcher's Game tab) with register_options_tab.
#include <SDL3/SDL.h>

#include <functional>

struct ImFont;
#include <imgui.h>  // ImTextureID, ImVec2

#include "options/settings_io.h"

namespace supersaga {
class Config;

// Adapts Config to the get/set/save interface the display and input modules use.
SettingsIO settings_io(Config& config);
}  // namespace supersaga

// Draws the selected settings page (picked in the menu's sidebar) with Dear ImGui into the
// current window. Call between
// ImGui::NewFrame() and ImGui::Render().
void draw_options_panel(supersaga::Config& config);

// Adds a tab. Tabs order as: tabs registered with before_builtin (in registration order),
// then Display and Controls, then the other tabs in registration order. Registering the same
// name again replaces its draw function and keeps its place.
void register_options_tab(const char* name, std::function<void(supersaga::Config&)> draw,
                          bool before_builtin = false);

// Selects the named tab the next time the panel is drawn (for example "States").
void options_select_tab(const char* name);

// Forward every SDL event here (before ImGui) while the panel or menu is open. Needed for
// gamepad hotplug and for capturing the next key or button when rebinding a control.
// Returns true when the event was consumed by a rebind (do not pass it on).
bool options_panel_handle_event(const SDL_Event& event);

// ---- F1 settings menu (an overlay window on top of the paused game) ----
//
// Host contract: F1 (and Esc while open) toggles it. While options_menu_open(): pause the
// game and audio, keep drawing the last frame underneath, forward events to
// options_panel_handle_event first, and keep keys away from the game. After it closes,
// reload the Presenter and input::Manager so the changes apply.
void options_menu_toggle();
void options_menu_set_open(bool open);
bool options_menu_open();
void draw_options_menu(supersaga::Config& config);
// Fonts for the page headings and the small hint text (the theme's). Optional.
void set_options_menu_fonts(ImFont* heading, ImFont* small);
// The game's logo at the top of the sidebar (trimmed to its visible pixels). Optional.
void set_options_menu_logo(ImTextureID tex, ImVec2 size);
// Optional "Quit to launcher" button. Without a handler the button is hidden.
void set_options_menu_quit_handler(std::function<void()> on_quit);
