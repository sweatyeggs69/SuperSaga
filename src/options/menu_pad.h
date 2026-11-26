#pragma once
// Controller input for the menus (Dear ImGui), fed from SDL events.
//
// The ImGui SDL3 backend reads controllers by polling their current state every frame, and in its
// default mode only the first controller SDL lists. That has two problems:
//   - A button pressed to capture a binding is swallowed as an event, but the backend still sees
//     it held on the next frame and acts on it: the face button that clears a binding cleared the
//     new one, the cancel button jumped back to the sidebar, and confirm started another rebind.
//   - With two or more controllers (or a virtual one listed first), the menus ignored the others.
// So the backend's own gamepad handling is turned off and every controller's events come through
// here instead. An event the app consumed (binding capture, shortcuts) never reaches the menus,
// and neither does the release of that button or the rest of that stick push.
#include <SDL3/SDL.h>

namespace supersaga::menu_pad {

// Call once after ImGui_ImplSDL3_Init: stops the backend from polling controllers itself.
void init();
// Every SDL event, after the app has handled it. consumed: the app used it (binding capture or a
// shortcut), so the menus must not see this press.
void handle_event(const SDL_Event& e, bool consumed);
// Call between ImGui_ImplSDL3_NewFrame and ImGui::NewFrame (the backend clears the gamepad flag).
void new_frame();
// Call on every refresh that runs no ImGui frame (gameplay). Controller events keep arriving while
// no menu is drawn; ImGui would queue them all and replay them when the settings menu opens, so a
// B press made while playing closed the menu again right away (east is Back in the sidebar).
void discard_queued();
// Call when the settings menu opens: drops queued input, and buttons or sticks held right now are
// ignored until let go, so the press that opened the menu (or one still held) can't act in it.
void reset();

}  // namespace supersaga::menu_pad
