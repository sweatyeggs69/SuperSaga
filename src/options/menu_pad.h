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

}  // namespace supersaga::menu_pad
