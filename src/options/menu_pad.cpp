#include "options/menu_pad.h"

#include <imgui.h>
#include <imgui_impl_sdl3.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace supersaga::menu_pad {
namespace {

// Same layout and dead zone as the ImGui SDL3 backend.
constexpr int kStickDeadZone = 8000;

struct ButtonKey {
    SDL_GamepadButton button;
    ImGuiKey key;
};
constexpr ButtonKey kButtons[] = {
    {SDL_GAMEPAD_BUTTON_START, ImGuiKey_GamepadStart},
    {SDL_GAMEPAD_BUTTON_BACK, ImGuiKey_GamepadBack},
    {SDL_GAMEPAD_BUTTON_WEST, ImGuiKey_GamepadFaceLeft},
    {SDL_GAMEPAD_BUTTON_EAST, ImGuiKey_GamepadFaceRight},
    {SDL_GAMEPAD_BUTTON_NORTH, ImGuiKey_GamepadFaceUp},
    {SDL_GAMEPAD_BUTTON_SOUTH, ImGuiKey_GamepadFaceDown},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT, ImGuiKey_GamepadDpadLeft},
    {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, ImGuiKey_GamepadDpadRight},
    {SDL_GAMEPAD_BUTTON_DPAD_UP, ImGuiKey_GamepadDpadUp},
    {SDL_GAMEPAD_BUTTON_DPAD_DOWN, ImGuiKey_GamepadDpadDown},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, ImGuiKey_GamepadL1},
    {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, ImGuiKey_GamepadR1},
    {SDL_GAMEPAD_BUTTON_LEFT_STICK, ImGuiKey_GamepadL3},
    {SDL_GAMEPAD_BUTTON_RIGHT_STICK, ImGuiKey_GamepadR3},
};

struct AxisKey {
    SDL_GamepadAxis axis;
    int dir;  // +1: positive values, -1: negative values
    int dead_zone;
    ImGuiKey key;
};
constexpr AxisKey kAxes[] = {
    {SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 1, 0, ImGuiKey_GamepadL2},
    {SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 1, 0, ImGuiKey_GamepadR2},
    {SDL_GAMEPAD_AXIS_LEFTX, -1, kStickDeadZone, ImGuiKey_GamepadLStickLeft},
    {SDL_GAMEPAD_AXIS_LEFTX, 1, kStickDeadZone, ImGuiKey_GamepadLStickRight},
    {SDL_GAMEPAD_AXIS_LEFTY, -1, kStickDeadZone, ImGuiKey_GamepadLStickUp},
    {SDL_GAMEPAD_AXIS_LEFTY, 1, kStickDeadZone, ImGuiKey_GamepadLStickDown},
    {SDL_GAMEPAD_AXIS_RIGHTX, -1, kStickDeadZone, ImGuiKey_GamepadRStickLeft},
    {SDL_GAMEPAD_AXIS_RIGHTX, 1, kStickDeadZone, ImGuiKey_GamepadRStickRight},
    {SDL_GAMEPAD_AXIS_RIGHTY, -1, kStickDeadZone, ImGuiKey_GamepadRStickUp},
    {SDL_GAMEPAD_AXIS_RIGHTY, 1, kStickDeadZone, ImGuiKey_GamepadRStickDown},
};

struct Pad {
    SDL_JoystickID id = 0;
    std::uint64_t held = 0;        // buttons down, as the menus see them
    std::uint64_t suppressed = 0;  // consumed presses: ignored until released
    int axes[SDL_GAMEPAD_AXIS_COUNT] = {};
    bool axis_suppressed[SDL_GAMEPAD_AXIS_COUNT] = {};  // until the axis returns to rest
};
std::vector<Pad> g_pads;

Pad& pad(SDL_JoystickID id) {
    for (auto& p : g_pads)
        if (p.id == id) return p;
    g_pads.push_back({});
    g_pads.back().id = id;
    return g_pads.back();
}

float axis_amount(int value, const AxisKey& a) {
    const float v = float(value) * float(a.dir);
    if (v <= float(a.dead_zone)) return 0.0f;
    return std::min(1.0f, (v - float(a.dead_zone)) / (32767.0f - float(a.dead_zone)));
}

// Sends the merged state of every controller. ImGui drops events that don't change a key.
void push() {
    if (!ImGui::GetCurrentContext()) return;
    ImGuiIO& io = ImGui::GetIO();
    for (const auto& b : kButtons) {
        bool down = false;
        for (const auto& p : g_pads) down |= (p.held >> b.button) & 1;
        io.AddKeyEvent(b.key, down);
    }
    for (const auto& a : kAxes) {
        float v = 0.0f;
        for (const auto& p : g_pads) v = std::max(v, axis_amount(p.axes[a.axis], a));
        io.AddKeyAnalogEvent(a.key, v > 0.1f, v);
    }
}

void log_connected(SDL_JoystickID id) {
    const char* name = SDL_GetGamepadNameForID(id);
    const SDL_GamepadType type = SDL_GetGamepadTypeForID(id);
    char guid[64] = {};
    SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), guid, sizeof guid);
    char* mapping = SDL_GetGamepadMappingForID(id);
    std::fprintf(stderr, "supersaga: controller connected: %s (type %s, vendor %04x, product %04x, guid %s)\n",
                 name ? name : "?", SDL_GetGamepadStringForType(type) ? SDL_GetGamepadStringForType(type) : "unknown",
                 SDL_GetJoystickVendorForID(id), SDL_GetJoystickProductForID(id), guid);
    if (mapping) {
        std::fprintf(stderr, "supersaga:   mapping: %s\n", mapping);
        SDL_free(mapping);
    }
}

}  // namespace

void init() { ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_Manual, nullptr, 0); }

void handle_event(const SDL_Event& e, bool consumed) {
    switch (e.type) {
        case SDL_EVENT_GAMEPAD_ADDED:
            log_connected(e.gdevice.which);
            pad(e.gdevice.which);
            return;
        case SDL_EVENT_JOYSTICK_ADDED:
            // Controllers SDL has no gamepad layout for still work for bindings (as raw joystick
            // inputs), but not in the menus. Say so, so a missing layout is easy to spot.
            if (!SDL_IsGamepad(e.jdevice.which)) {
                const char* name = SDL_GetJoystickNameForID(e.jdevice.which);
                std::fprintf(stderr,
                             "supersaga: joystick connected without a gamepad layout: %s (vendor %04x, product %04x)\n",
                             name ? name : "?", SDL_GetJoystickVendorForID(e.jdevice.which),
                             SDL_GetJoystickProductForID(e.jdevice.which));
            }
            return;
        case SDL_EVENT_GAMEPAD_REMOVED:
            std::fprintf(stderr, "supersaga: controller disconnected\n");
            g_pads.erase(std::remove_if(g_pads.begin(), g_pads.end(), [&](const Pad& p) { return p.id == e.gdevice.which; }),
                         g_pads.end());
            push();
            return;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
            const int b = e.gbutton.button;
            if (b < 0 || b >= 64) return;
            Pad& p = pad(e.gbutton.which);
            const std::uint64_t bit = std::uint64_t(1) << b;
            if (consumed) {
                p.suppressed |= bit;
                p.held &= ~bit;
            } else if (!(p.suppressed & bit)) {
                p.held |= bit;
            }
            push();
            return;
        }
        case SDL_EVENT_GAMEPAD_BUTTON_UP: {
            const int b = e.gbutton.button;
            if (b < 0 || b >= 64) return;
            Pad& p = pad(e.gbutton.which);
            const std::uint64_t bit = std::uint64_t(1) << b;
            p.suppressed &= ~bit;
            p.held &= ~bit;
            push();
            return;
        }
        case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
            const int a = e.gaxis.axis;
            if (a < 0 || a >= SDL_GAMEPAD_AXIS_COUNT) return;
            Pad& p = pad(e.gaxis.which);
            const int v = e.gaxis.value;
            const bool rest = v > -kStickDeadZone && v < kStickDeadZone;
            if (consumed && !rest) p.axis_suppressed[a] = true;
            if (rest) p.axis_suppressed[a] = false;
            p.axes[a] = p.axis_suppressed[a] ? 0 : v;
            push();
            return;
        }
        default:
            return;
    }
}

void new_frame() {
    if (!ImGui::GetCurrentContext()) return;
    // Releases that happened while the window was in the background send no event (SDL holds
    // controller events back then), so let go of anything the controller no longer holds.
    bool changed = false;
    for (auto& p : g_pads) {
        SDL_Gamepad* gp = SDL_GetGamepadFromID(p.id);
        if (!gp) continue;
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT && b < 64; ++b) {
            const std::uint64_t bit = std::uint64_t(1) << b;
            if (((p.held | p.suppressed) & bit) && !SDL_GetGamepadButton(gp, SDL_GamepadButton(b))) {
                changed |= (p.held & bit) != 0;
                p.held &= ~bit;
                p.suppressed &= ~bit;
            }
        }
        for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; ++a) {
            const int v = SDL_GetGamepadAxis(gp, SDL_GamepadAxis(a));
            if (v > -kStickDeadZone && v < kStickDeadZone && (p.axes[a] != 0 || p.axis_suppressed[a])) {
                changed |= p.axes[a] != 0;
                p.axes[a] = 0;
                p.axis_suppressed[a] = false;
            }
        }
    }
    if (changed) push();
    ImGuiIO& io = ImGui::GetIO();
    if ((io.ConfigFlags & ImGuiConfigFlags_NavEnableGamepad) && (!g_pads.empty() || SDL_HasGamepad()))
        io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
}

}  // namespace supersaga::menu_pad
