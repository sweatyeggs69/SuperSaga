// Headless checks for controller input: a virtual SDL gamepad is attached and driven, and the
// GBA button mask from input::Manager is compared with what the default bindings promise
// (README "Controls" table). Keyboard state cannot be faked headless, so keys are checked
// through the binding table only.
#include <SDL3/SDL.h>

#include <cstdio>
#include <map>
#include <string>

#include "app/states.h"
#include "options/input.h"
#include "options/menu_pad.h"

#include <imgui.h>

using namespace supersaga::input;

static int failures = 0;
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                   \
        }                                                                                 \
    } while (0)

namespace {
constexpr std::uint16_t bit(Button b) { return std::uint16_t(1u << int(b)); }

SDL_Joystick* g_joy = nullptr;

void settle() {
    SDL_UpdateJoysticks();
    SDL_Event e;
    while (SDL_PollEvent(&e)) {}
}
void press(SDL_GamepadButton b, bool down) {
    SDL_SetJoystickVirtualButton(g_joy, b, down);
    settle();
}
void tilt(SDL_GamepadAxis a, Sint16 v) {
    SDL_SetJoystickVirtualAxis(g_joy, a, v);
    settle();
}
void release_all() {
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b) SDL_SetJoystickVirtualButton(g_joy, b, false);
    // A virtual pad's raw trigger range is -32768 (released) to 32767 (pulled); 0 is half way.
    for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; ++a)
        SDL_SetJoystickVirtualAxis(g_joy, a, a >= SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? -32768 : 0);
    settle();
}

bool has_key(const Config& c, Button b, SDL_Scancode sc) {
    for (const auto& x : c.buttons[int(b)])
        if (x.kind == Binding::Kind::Key && x.code == int(sc)) return true;
    return false;
}

void keyboard_defaults() {
    const Config c = default_config();
    CHECK(has_key(c, Button::Up, SDL_SCANCODE_UP));
    CHECK(has_key(c, Button::Down, SDL_SCANCODE_DOWN));
    CHECK(has_key(c, Button::Left, SDL_SCANCODE_LEFT));
    CHECK(has_key(c, Button::Right, SDL_SCANCODE_RIGHT));
    CHECK(has_key(c, Button::A, SDL_SCANCODE_X));
    CHECK(has_key(c, Button::B, SDL_SCANCODE_Z));
    CHECK(has_key(c, Button::L, SDL_SCANCODE_A));
    CHECK(has_key(c, Button::R, SDL_SCANCODE_S));
    CHECK(has_key(c, Button::Start, SDL_SCANCODE_RETURN));
    CHECK(has_key(c, Button::Select, SDL_SCANCODE_BACKSPACE));
    // No key may drive two GBA buttons.
    std::map<int, int> uses;
    for (const auto& list : c.buttons)
        for (const auto& b : list)
            if (b.kind == Binding::Kind::Key) ++uses[b.code];
    for (const auto& [code, n] : uses) CHECK(n == 1);

    // Save state on Page Up, load on Page Down; neither drives a GBA button.
    auto first_key = [&](Hotkey h) {
        for (const auto& chord : c.hotkeys[int(h)])
            if (chord.size() == 1 && chord[0].kind == Binding::Kind::Key) return chord[0].code;
        return -1;
    };
    CHECK(first_key(Hotkey::SaveState) == SDL_SCANCODE_PAGEUP);
    CHECK(first_key(Hotkey::LoadState) == SDL_SCANCODE_PAGEDOWN);
    CHECK(!uses.count(SDL_SCANCODE_PAGEUP) && !uses.count(SDL_SCANCODE_PAGEDOWN));
    CHECK(display_name(Binding{Binding::Kind::Key, SDL_SCANCODE_PAGEUP, 0}) == "Page Up");
    CHECK(display_name(Binding{Binding::Kind::Key, SDL_SCANCODE_PAGEDOWN, 0}) == "Page Down");
    // The stored form reads back (settings.ini round trip).
    Chord parsed;
    CHECK(chord_from_string(to_string(Chord{Binding{Binding::Kind::Key, SDL_SCANCODE_PAGEUP, 0}}), parsed) &&
          parsed.size() == 1 && parsed[0].code == SDL_SCANCODE_PAGEUP);

    // Settings written with the old F5/F8 defaults pick up the new keys; custom keys stay.
    std::map<std::string, std::string> ini;
    supersaga::SettingsIO io;
    io.get = [&](const std::string& k, const std::string& fb) {
        auto it = ini.find(k);
        return it == ini.end() ? fb : it->second;
    };
    ini["input.hotkey.save_state"] = "key:F5 | pad:back+pad:rightshoulder";
    ini["input.hotkey.load_state"] = "key:F8 | pad:back+pad:leftshoulder";
    Config loaded = load(io);
    CHECK(loaded.hotkeys[int(Hotkey::SaveState)] == c.hotkeys[int(Hotkey::SaveState)]);
    CHECK(loaded.hotkeys[int(Hotkey::LoadState)] == c.hotkeys[int(Hotkey::LoadState)]);
    ini["input.hotkey.save_state"] = "key:F5";
    loaded = load(io);
    CHECK(loaded.hotkeys[int(Hotkey::SaveState)].size() == 1 &&
          loaded.hotkeys[int(Hotkey::SaveState)][0][0].code == SDL_SCANCODE_F5);
}

void gamepad(Manager& m) {
    CHECK(m.gamepad_count() == 1);
    CHECK(m.poll() == 0);

    const struct { SDL_GamepadButton pad; Button gba; } buttons[] = {
        {SDL_GAMEPAD_BUTTON_SOUTH, Button::A},          {SDL_GAMEPAD_BUTTON_NORTH, Button::A},
        {SDL_GAMEPAD_BUTTON_EAST, Button::B},           {SDL_GAMEPAD_BUTTON_WEST, Button::B},
        {SDL_GAMEPAD_BUTTON_START, Button::Start},      {SDL_GAMEPAD_BUTTON_BACK, Button::Select},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, Button::L},  {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, Button::R},
        {SDL_GAMEPAD_BUTTON_DPAD_UP, Button::Up},       {SDL_GAMEPAD_BUTTON_DPAD_DOWN, Button::Down},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, Button::Left},   {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, Button::Right},
    };
    for (const auto& t : buttons) {
        press(t.pad, true);
        if (m.poll() != bit(t.gba))
            std::fprintf(stderr, "pad button %d -> mask %03x, want %s\n", int(t.pad), m.poll(), name(t.gba));
        CHECK(m.poll() == bit(t.gba));
        press(t.pad, false);
        CHECK(m.poll() == 0);
    }

    // Left stick past the dead zone moves; a small nudge does not.
    tilt(SDL_GAMEPAD_AXIS_LEFTX, 32767);
    CHECK(m.poll() == bit(Button::Right));
    tilt(SDL_GAMEPAD_AXIS_LEFTX, -32768);
    CHECK(m.poll() == bit(Button::Left));
    tilt(SDL_GAMEPAD_AXIS_LEFTX, 6000);
    CHECK(m.poll() == 0);
    tilt(SDL_GAMEPAD_AXIS_LEFTX, 0);
    tilt(SDL_GAMEPAD_AXIS_LEFTY, -32768);
    CHECK(m.poll() == bit(Button::Up));
    tilt(SDL_GAMEPAD_AXIS_LEFTY, 32767);
    CHECK(m.poll() == bit(Button::Down));
    // Full-tilt diagonal (about 70% on each axis) aims diagonally.
    tilt(SDL_GAMEPAD_AXIS_LEFTY, -23170);
    tilt(SDL_GAMEPAD_AXIS_LEFTX, 23170);
    CHECK(m.poll() == (bit(Button::Up) | bit(Button::Right)));
    release_all();

    // Triggers act as L / R.
    tilt(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 32767);
    CHECK(m.poll() == bit(Button::L));
    tilt(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -32768);
    tilt(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767);
    CHECK(m.poll() == bit(Button::R));
    release_all();

    // Held together, as in play: run right, aim with R, charge with B, jump with A.
    press(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, true);
    press(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, true);
    press(SDL_GAMEPAD_BUTTON_EAST, true);
    press(SDL_GAMEPAD_BUTTON_SOUTH, true);
    CHECK(m.poll() == (bit(Button::Right) | bit(Button::R) | bit(Button::B) | bit(Button::A)));
    release_all();

    // D-pad and stick in opposite directions cancel like real hardware.
    press(SDL_GAMEPAD_BUTTON_DPAD_LEFT, true);
    tilt(SDL_GAMEPAD_AXIS_LEFTX, 32767);
    CHECK(m.poll() == 0);
    release_all();
}

void shortcuts(Manager& m) {
    int saves = 0, loads = 0;
    supersaga::states::Hotkeys hk;
    hk.quick_save = [&] { ++saves; };
    hk.quick_load = [&] { ++loads; };
    supersaga::states::set_hotkeys(hk);

    // Back + Right Bumper saves, and neither Select nor R reaches the game while held.
    press(SDL_GAMEPAD_BUTTON_BACK, true);
    SDL_Event e{};
    e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    e.gbutton.which = SDL_GetJoystickID(g_joy);
    e.gbutton.button = SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;
    press(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, true);
    CHECK(m.handle_event(e));
    CHECK(saves == 1 && loads == 0);
    CHECK((m.poll() & (bit(Button::R) | bit(Button::Select))) == 0);
    release_all();
    CHECK(m.poll() == 0);
    // Released buttons work normally again.
    press(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, true);
    CHECK(m.poll() == bit(Button::R));
    release_all();
    supersaga::states::set_hotkeys({});
}
void binding_strings() {
    const Binding samples[] = {
        {Binding::Kind::JoyButton, 12, 0},         {Binding::Kind::JoyAxis, 4, -1},
        {Binding::Kind::JoyAxis, 0, 1},            {Binding::Kind::JoyHat, 1, SDL_HAT_LEFT},
        {Binding::Kind::Mouse, SDL_BUTTON_X1, 0},  {Binding::Kind::Mouse, SDL_BUTTON_MIDDLE, 0},
        {Binding::Kind::PadButton, SDL_GAMEPAD_BUTTON_LEFT_PADDLE2, 0},
    };
    for (const auto& b : samples) {
        Binding back;
        CHECK(from_string(to_string(b), back) && back == b);
    }
    CHECK(to_string(samples[0]) == "joy:button12");
    CHECK(display_name(samples[0]) == "Joystick Button 13");
    CHECK(display_name(samples[3]) == "Joystick Hat 2 Left");
    CHECK(display_name(samples[4]) == "Mouse Button 4");
    Binding bad;
    CHECK(!from_string("mouse:left", bad));  // left/right clicks stay with the menus
    CHECK(!from_string("joy:hat0north", bad));
    CHECK(!from_string("joy:button", bad));
    // Shortcuts keep to keys and standard controller buttons.
    Chord c;
    CHECK(!chord_from_string("joy:button3", c));

    // This game has no extra actions yet.
    CHECK(default_config().actions.empty());
}

// A controller SDL has no layout for is read as a raw joystick, and its buttons and hat bind
// by number.
void raw_joystick(Manager& m, SDL_Joystick* raw) {
    CHECK(m.gamepad_count() == 2);
    m.start_remap(Button::A);
    SDL_Event e{};
    e.type = SDL_EVENT_JOYSTICK_BUTTON_DOWN;
    e.jbutton.which = SDL_GetJoystickID(raw);
    e.jbutton.button = 9;
    CHECK(m.handle_event(e));
    CHECK(!m.remapping());
    CHECK(m.describe(Button::A).find("Joystick Button 10") != std::string::npos);
    SDL_SetJoystickVirtualButton(raw, 9, true);
    settle();
    CHECK(m.poll() == bit(Button::A));
    SDL_SetJoystickVirtualButton(raw, 9, false);
    settle();
    CHECK(m.poll() == 0);

    m.start_remap(Button::Up);
    e = {};
    e.type = SDL_EVENT_JOYSTICK_HAT_MOTION;
    e.jhat.which = SDL_GetJoystickID(raw);
    e.jhat.hat = 0;
    e.jhat.value = SDL_HAT_LEFTUP;
    CHECK(m.handle_event(e));
    CHECK(m.describe(Button::Up).find("Joystick Hat 1 Up") != std::string::npos);
    SDL_SetJoystickVirtualHat(raw, 0, SDL_HAT_RIGHTUP);
    settle();
    CHECK(m.poll() == bit(Button::Up));
    SDL_SetJoystickVirtualHat(raw, 0, SDL_HAT_CENTERED);
    settle();

    // A standard pad's own buttons still bind by name, not as raw joystick buttons.
    m.start_remap(Button::B);
    e = {};
    e.type = SDL_EVENT_JOYSTICK_BUTTON_DOWN;
    e.jbutton.which = SDL_GetJoystickID(g_joy);
    e.jbutton.button = 0;
    CHECK(m.handle_event(e));
    CHECK(m.remapping());  // ignored, still waiting
    m.cancel_remap();
    m.reset_to_defaults();
}

// Button names follow the connected controller's layout. Bindings are stored by position.
void labels_for(Uint16 vendor, Uint16 product, const char* south, const char* west, const char* lt) {
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.vendor_id = vendor;
    desc.product_id = product;
    desc.name = "SuperSaga label pad";
    const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
    CHECK(id != 0);
    settle();
    CHECK(display_name({Binding::Kind::PadButton, SDL_GAMEPAD_BUTTON_SOUTH, 0}) == south);
    CHECK(display_name({Binding::Kind::PadButton, SDL_GAMEPAD_BUTTON_WEST, 0}) == west);
    CHECK(display_name({Binding::Kind::PadAxis, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 1}) == lt);
    if (id) SDL_DetachVirtualJoystick(id);
    settle();
}

void labels() {
    // No controller connected: Xbox names.
    CHECK(display_name({Binding::Kind::PadButton, SDL_GAMEPAD_BUTTON_SOUTH, 0}) == "A Button");
    labels_for(0x057e, 0x2009, "B Button", "Y Button", "ZL Button");  // Switch Pro Controller
    labels_for(0x054c, 0x09cc, "Cross", "Square", "L2");               // DualShock 4
    labels_for(0x045e, 0x02ea, "A Button", "X Button", "Left Trigger");  // Xbox One S
}

// Menus get controller input from events; a press the app consumed (binding capture) must not
// reach them, nor its release, nor a stick push that was captured.
SDL_Event pad_button(SDL_JoystickID which, SDL_GamepadButton b, bool down) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
    e.gbutton.which = which;
    e.gbutton.button = Uint8(b);
    e.gbutton.down = down;
    return e;
}
SDL_Event pad_axis(SDL_JoystickID which, SDL_GamepadAxis a, Sint16 v) {
    SDL_Event e{};
    e.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
    e.gaxis.which = which;
    e.gaxis.axis = Uint8(a);
    e.gaxis.value = v;
    return e;
}
void frame() {
    ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
    supersaga::menu_pad::new_frame();
    ImGui::NewFrame();
    ImGui::EndFrame();
}

void menu_pad_events() {
    using supersaga::menu_pad::handle_event;
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(640, 480);
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    unsigned char* px;
    int w, h;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    frame();

    // Captured press (Nintendo Y = west, the clear button): never seen, nor its release.
    handle_event(pad_button(1, SDL_GAMEPAD_BUTTON_WEST, true), true);
    frame();
    CHECK(!ImGui::IsKeyDown(ImGuiKey_GamepadFaceLeft));
    handle_event(pad_button(1, SDL_GAMEPAD_BUTTON_WEST, false), false);
    frame();
    CHECK(!ImGui::IsKeyDown(ImGuiKey_GamepadFaceLeft));
    // The next press of it is a normal one again.
    handle_event(pad_button(1, SDL_GAMEPAD_BUTTON_WEST, true), false);
    frame();
    CHECK(ImGui::IsKeyDown(ImGuiKey_GamepadFaceLeft));
    handle_event(pad_button(1, SDL_GAMEPAD_BUTTON_WEST, false), false);
    frame();
    CHECK(!ImGui::IsKeyDown(ImGuiKey_GamepadFaceLeft));

    // Two controllers: both are heard, and one letting go doesn't release the other's press.
    handle_event(pad_button(1, SDL_GAMEPAD_BUTTON_SOUTH, true), false);
    handle_event(pad_button(2, SDL_GAMEPAD_BUTTON_SOUTH, true), false);
    handle_event(pad_button(2, SDL_GAMEPAD_BUTTON_SOUTH, false), false);
    frame();
    frame();
    CHECK(ImGui::IsKeyDown(ImGuiKey_GamepadFaceDown));
    handle_event(pad_button(1, SDL_GAMEPAD_BUTTON_SOUTH, false), false);
    frame();
    CHECK(!ImGui::IsKeyDown(ImGuiKey_GamepadFaceDown));
    handle_event(pad_button(2, SDL_GAMEPAD_BUTTON_EAST, true), false);
    frame();
    CHECK(ImGui::IsKeyDown(ImGuiKey_GamepadFaceRight));
    handle_event(pad_button(2, SDL_GAMEPAD_BUTTON_EAST, false), false);
    frame();

    // A captured stick push stays away from the menus until the stick is back at rest.
    handle_event(pad_axis(1, SDL_GAMEPAD_AXIS_LEFTY, 30000), true);
    handle_event(pad_axis(1, SDL_GAMEPAD_AXIS_LEFTY, 31000), false);
    frame();
    CHECK(!ImGui::IsKeyDown(ImGuiKey_GamepadLStickDown));
    handle_event(pad_axis(1, SDL_GAMEPAD_AXIS_LEFTY, 0), false);
    handle_event(pad_axis(1, SDL_GAMEPAD_AXIS_LEFTY, 30000), false);
    frame();
    frame();
    CHECK(ImGui::IsKeyDown(ImGuiKey_GamepadLStickDown));
    handle_event(pad_axis(1, SDL_GAMEPAD_AXIS_LEFTY, 0), false);
    frame();

    // The reported bug, end to end: a cell waits for a controller button and the player presses
    // the west button (Nintendo Y). It binds, and the menu never sees it (west clears a binding).
    {
        std::map<std::string, std::string> store;
        supersaga::SettingsIO sio;
        sio.get = [&](const std::string& k, const std::string& fb) {
            auto it = store.find(k);
            return it == store.end() ? fb : it->second;
        };
        sio.set = [&](const std::string& k, const std::string& v) { store[k] = v; };
        sio.save = [] {};
        Manager m(sio);
        m.start_remap(Button::B, Source::Controller);
        const SDL_Event down = pad_button(3, SDL_GAMEPAD_BUTTON_WEST, true);
        handle_event(down, m.handle_event(down));
        frame();
        CHECK(!m.remapping());
        CHECK(m.describe(Button::B, Source::Controller) == "X Button");
        CHECK(!ImGui::IsKeyDown(ImGuiKey_GamepadFaceLeft));
        const SDL_Event up = pad_button(3, SDL_GAMEPAD_BUTTON_WEST, false);
        handle_event(up, m.handle_event(up));
        frame();
        CHECK(!ImGui::IsKeyDown(ImGuiKey_GamepadFaceLeft));
        CHECK(m.describe(Button::B, Source::Controller) == "X Button");
    }
    ImGui::DestroyContext();
}
}  // namespace

int main() {
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    keyboard_defaults();

    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.name = "SuperSaga test pad";
    const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
    if (!id || !(g_joy = SDL_OpenJoystick(id))) {
        std::fprintf(stderr, "virtual gamepad: %s\n", SDL_GetError());
        return 1;
    }
    release_all();
    binding_strings();
    {
        std::map<std::string, std::string> store;
        supersaga::SettingsIO io;
        io.get = [&](const std::string& k, const std::string& fb) {
            auto it = store.find(k);
            return it == store.end() ? fb : it->second;
        };
        io.set = [&](const std::string& k, const std::string& v) { store[k] = v; };
        io.save = [] {};
        Manager m(io);
        gamepad(m);
        shortcuts(m);
        CHECK(m.poll_frame() == 0);
    }
    {
        SDL_VirtualJoystickDesc rd;
        SDL_INIT_INTERFACE(&rd);
        rd.type = SDL_JOYSTICK_TYPE_UNKNOWN;
        rd.naxes = 2;
        rd.nbuttons = 16;
        rd.nhats = 1;
        rd.name = "SuperSaga test arcade stick";
        const SDL_JoystickID rid = SDL_AttachVirtualJoystick(&rd);
        SDL_Joystick* raw = rid ? SDL_OpenJoystick(rid) : nullptr;
        CHECK(raw && !SDL_IsGamepad(rid));
        settle();
        if (raw) {
            std::map<std::string, std::string> store;
            supersaga::SettingsIO io;
            io.get = [&](const std::string& k, const std::string& fb) {
                auto it = store.find(k);
                return it == store.end() ? fb : it->second;
            };
            io.set = [&](const std::string& k, const std::string& v) { store[k] = v; };
            io.save = [] {};
            Manager m(io);
            raw_joystick(m, raw);
            SDL_CloseJoystick(raw);
        }
        if (rid) SDL_DetachVirtualJoystick(rid);
    }
    SDL_CloseJoystick(g_joy);
    SDL_DetachVirtualJoystick(id);
    settle();
    labels();
    menu_pad_events();
    SDL_Quit();
    if (failures) std::fprintf(stderr, "%d check(s) failed\n", failures);
    else std::printf("input: all checks passed\n");
    return failures ? 1 : 0;
}
