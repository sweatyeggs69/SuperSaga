#include "options/input.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

#include "app/states.h"

namespace supersaga::input {

namespace {
// Order the remap walks through, D-pad first like most setup screens.
constexpr Button kRemapOrder[] = {Button::Up, Button::Down, Button::Left, Button::Right, Button::A,
                                  Button::B,  Button::L,    Button::R,    Button::Start, Button::Select};

std::string trim(const std::string& s) {
    auto b = s.find_first_not_of(" \t\r\n");
    auto e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Keys the shell uses itself; they can never be bound to a GBA button.
bool reserved_key(SDL_Scancode sc) {
    return sc == SDL_SCANCODE_ESCAPE || (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12);
}
// Shortcuts may use function keys, except the ones the shell keeps (F1 menu, F11 fullscreen).
bool reserved_hotkey_key(SDL_Scancode sc) {
    return sc == SDL_SCANCODE_ESCAPE || sc == SDL_SCANCODE_F1 || sc == SDL_SCANCODE_F11;
}

Binding key(SDL_Scancode sc) { return {Binding::Kind::Key, int(sc), 0}; }
Binding pad(SDL_GamepadButton b) { return {Binding::Kind::PadButton, int(b), 0}; }
Binding axis(SDL_GamepadAxis a, int dir) { return {Binding::Kind::PadAxis, int(a), dir}; }
Binding joy_button(int i) { return {Binding::Kind::JoyButton, i, 0}; }
Binding joy_axis(int i, int dir) { return {Binding::Kind::JoyAxis, i, dir}; }
Binding joy_hat(int i, int dir) { return {Binding::Kind::JoyHat, i, dir}; }
Binding mouse(int button) { return {Binding::Kind::Mouse, button, 0}; }

// Left and right clicks drive the menus, so only the other mouse buttons can be bound.
bool bindable_mouse(int button) {
    return button == SDL_BUTTON_MIDDLE || button == SDL_BUTTON_X1 || button == SDL_BUTTON_X2;
}

const char* hat_name(int dir) {
    switch (dir) {
        case SDL_HAT_UP: return "up";
        case SDL_HAT_DOWN: return "down";
        case SDL_HAT_LEFT: return "left";
        case SDL_HAT_RIGHT: return "right";
        default: return nullptr;
    }
}
// A hat reports a bit mask; a diagonal binds its vertical half.
int hat_direction(Uint8 value) {
    if (value & SDL_HAT_UP) return SDL_HAT_UP;
    if (value & SDL_HAT_DOWN) return SDL_HAT_DOWN;
    if (value & SDL_HAT_LEFT) return SDL_HAT_LEFT;
    if (value & SDL_HAT_RIGHT) return SDL_HAT_RIGHT;
    return 0;
}
}  // namespace

const char* name(Button b) {
    switch (b) {
        case Button::A: return "A";
        case Button::B: return "B";
        case Button::Select: return "Select";
        case Button::Start: return "Start";
        case Button::Right: return "Right";
        case Button::Left: return "Left";
        case Button::Up: return "Up";
        case Button::Down: return "Down";
        case Button::R: return "R";
        case Button::L: return "L";
        default: return "?";
    }
}

std::string to_string(const Binding& b) {
    switch (b.kind) {
        case Binding::Kind::Key: {
            const char* n = SDL_GetScancodeName(SDL_Scancode(b.code));
            return std::string("key:") + (n && *n ? n : std::to_string(b.code).c_str());
        }
        case Binding::Kind::PadButton: {
            const char* n = SDL_GetGamepadStringForButton(SDL_GamepadButton(b.code));
            return std::string("pad:") + (n ? n : "?");
        }
        case Binding::Kind::PadAxis: {
            const char* n = SDL_GetGamepadStringForAxis(SDL_GamepadAxis(b.code));
            return std::string("axis:") + (n ? n : "?") + (b.dir < 0 ? "-" : "+");
        }
        case Binding::Kind::JoyButton: return "joy:button" + std::to_string(b.code);
        case Binding::Kind::JoyAxis: return "joy:axis" + std::to_string(b.code) + (b.dir < 0 ? "-" : "+");
        case Binding::Kind::JoyHat: {
            const char* n = hat_name(b.dir);
            return "joy:hat" + std::to_string(b.code) + (n ? n : "?");
        }
        case Binding::Kind::Mouse:
            switch (b.code) {
                case SDL_BUTTON_MIDDLE: return "mouse:middle";
                case SDL_BUTTON_X1: return "mouse:x1";
                case SDL_BUTTON_X2: return "mouse:x2";
                default: return "mouse:" + std::to_string(b.code);
            }
    }
    return "?";
}

std::string display_name(const Binding& b) {
    switch (b.kind) {
        case Binding::Kind::Key: {
            switch (SDL_Scancode(b.code)) {
                case SDL_SCANCODE_UP: return "Up Arrow";
                case SDL_SCANCODE_DOWN: return "Down Arrow";
                case SDL_SCANCODE_LEFT: return "Left Arrow";
                case SDL_SCANCODE_RIGHT: return "Right Arrow";
                case SDL_SCANCODE_RETURN: return "Enter";
                case SDL_SCANCODE_KP_ENTER: return "Keypad Enter";
                case SDL_SCANCODE_SPACE: return "Space";
                case SDL_SCANCODE_PAGEUP: return "Page Up";
                case SDL_SCANCODE_PAGEDOWN: return "Page Down";
#ifdef __APPLE__
                case SDL_SCANCODE_LGUI: return "Left Command";
                case SDL_SCANCODE_RGUI: return "Right Command";
                case SDL_SCANCODE_LALT: return "Left Option";
                case SDL_SCANCODE_RALT: return "Right Option";
#elif defined(_WIN32)
                case SDL_SCANCODE_LGUI: return "Left Windows";
                case SDL_SCANCODE_RGUI: return "Right Windows";
#endif
                default: break;
            }
            const char* n = SDL_GetScancodeName(SDL_Scancode(b.code));
            return n && *n ? n : "Key " + std::to_string(b.code);
        }
        case Binding::Kind::PadButton:
            switch (SDL_GamepadButton(b.code)) {
                case SDL_GAMEPAD_BUTTON_SOUTH: return "A Button";
                case SDL_GAMEPAD_BUTTON_EAST: return "B Button";
                case SDL_GAMEPAD_BUTTON_WEST: return "X Button";
                case SDL_GAMEPAD_BUTTON_NORTH: return "Y Button";
                case SDL_GAMEPAD_BUTTON_BACK: return "Back Button";
                case SDL_GAMEPAD_BUTTON_GUIDE: return "Home Button";
                case SDL_GAMEPAD_BUTTON_START: return "Start Button";
                case SDL_GAMEPAD_BUTTON_LEFT_STICK: return "Left Stick Press";
                case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return "Right Stick Press";
                case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return "Left Bumper";
                case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return "Right Bumper";
                case SDL_GAMEPAD_BUTTON_DPAD_UP: return "Up D-Pad";
                case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return "Down D-Pad";
                case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return "Left D-Pad";
                case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return "Right D-Pad";
                case SDL_GAMEPAD_BUTTON_MISC1: return "Share Button";
                case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1: return "Right Paddle 1";
                case SDL_GAMEPAD_BUTTON_LEFT_PADDLE1: return "Left Paddle 1";
                case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2: return "Right Paddle 2";
                case SDL_GAMEPAD_BUTTON_LEFT_PADDLE2: return "Left Paddle 2";
                case SDL_GAMEPAD_BUTTON_TOUCHPAD: return "Touchpad Press";
                case SDL_GAMEPAD_BUTTON_MISC2: return "Extra Button 2";
                case SDL_GAMEPAD_BUTTON_MISC3: return "Extra Button 3";
                case SDL_GAMEPAD_BUTTON_MISC4: return "Extra Button 4";
                case SDL_GAMEPAD_BUTTON_MISC5: return "Extra Button 5";
                case SDL_GAMEPAD_BUTTON_MISC6: return "Extra Button 6";
                default: return "Controller Button " + std::to_string(b.code);
            }
        case Binding::Kind::PadAxis: {
            const bool neg = b.dir < 0;
            switch (SDL_GamepadAxis(b.code)) {
                case SDL_GAMEPAD_AXIS_LEFTX: return neg ? "Left Stick Left" : "Left Stick Right";
                case SDL_GAMEPAD_AXIS_LEFTY: return neg ? "Left Stick Up" : "Left Stick Down";
                case SDL_GAMEPAD_AXIS_RIGHTX: return neg ? "Right Stick Left" : "Right Stick Right";
                case SDL_GAMEPAD_AXIS_RIGHTY: return neg ? "Right Stick Up" : "Right Stick Down";
                case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: return "Left Trigger";
                case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: return "Right Trigger";
                default: return "Controller Axis " + std::to_string(b.code) + (neg ? "-" : "+");
            }
        }
        // Raw indices count from 1 in menus, as controller test tools show them.
        case Binding::Kind::JoyButton: return "Joystick Button " + std::to_string(b.code + 1);
        case Binding::Kind::JoyAxis:
            return "Joystick Axis " + std::to_string(b.code + 1) + (b.dir < 0 ? "-" : "+");
        case Binding::Kind::JoyHat: {
            std::string d = hat_name(b.dir) ? hat_name(b.dir) : "?";
            d[0] = char(std::toupper(static_cast<unsigned char>(d[0])));
            return "Joystick Hat " + std::to_string(b.code + 1) + " " + d;
        }
        case Binding::Kind::Mouse:
            switch (b.code) {
                case SDL_BUTTON_MIDDLE: return "Middle Mouse Button";
                case SDL_BUTTON_X1: return "Mouse Button 4";
                case SDL_BUTTON_X2: return "Mouse Button 5";
                default: return "Mouse Button " + std::to_string(b.code);
            }
    }
    return "?";
}

namespace {
bool parse(const std::string& text, Binding& out, bool hotkey);
}

bool from_string(const std::string& text, Binding& out) { return parse(text, out, false); }

namespace {
bool parse(const std::string& text, Binding& out, bool hotkey) {
    const std::string s = trim(text);
    auto colon = s.find(':');
    if (colon == std::string::npos) return false;
    const std::string kind = lower(trim(s.substr(0, colon)));
    const std::string value = trim(s.substr(colon + 1));
    if (kind == "key") {
        SDL_Scancode sc = SDL_GetScancodeFromName(value.c_str());
        if (sc == SDL_SCANCODE_UNKNOWN) {
            char* end = nullptr;
            long n = std::strtol(value.c_str(), &end, 10);
            if (end && *end == 0 && n > 0 && n < SDL_SCANCODE_COUNT) sc = SDL_Scancode(n);
        }
        if (sc == SDL_SCANCODE_UNKNOWN || (hotkey ? reserved_hotkey_key(sc) : reserved_key(sc))) return false;
        out = key(sc);
        return true;
    }
    if (kind == "pad") {
        SDL_GamepadButton b = SDL_GetGamepadButtonFromString(lower(value).c_str());
        if (b == SDL_GAMEPAD_BUTTON_INVALID) return false;
        out = pad(b);
        return true;
    }
    if (kind == "axis") {
        if (value.size() < 2) return false;
        const char sign = value.back();
        if (sign != '+' && sign != '-') return false;
        SDL_GamepadAxis a = SDL_GetGamepadAxisFromString(lower(value.substr(0, value.size() - 1)).c_str());
        if (a == SDL_GAMEPAD_AXIS_INVALID) return false;
        out = axis(a, sign == '-' ? -1 : 1);
        return true;
    }
    if (hotkey) return false;  // shortcuts use keys and standard controller buttons only
    auto index_after = [&](const std::string& v, const char* prefix, std::string& rest) {
        const size_t n = std::strlen(prefix);
        if (v.compare(0, n, prefix) != 0) return -1;
        size_t i = n;
        while (i < v.size() && std::isdigit(static_cast<unsigned char>(v[i]))) ++i;
        if (i == n || i - n > 3) return -1;
        rest = v.substr(i);
        return std::atoi(v.substr(n, i - n).c_str());
    };
    if (kind == "joy") {
        const std::string v = lower(value);
        std::string rest;
        int i;
        if ((i = index_after(v, "button", rest)) >= 0 && rest.empty()) {
            out = joy_button(i);
            return true;
        }
        if ((i = index_after(v, "axis", rest)) >= 0 && (rest == "+" || rest == "-")) {
            out = joy_axis(i, rest == "-" ? -1 : 1);
            return true;
        }
        if ((i = index_after(v, "hat", rest)) >= 0) {
            for (int d : {SDL_HAT_UP, SDL_HAT_DOWN, SDL_HAT_LEFT, SDL_HAT_RIGHT})
                if (rest == hat_name(d)) {
                    out = joy_hat(i, d);
                    return true;
                }
        }
        return false;
    }
    if (kind == "mouse") {
        const std::string v = lower(value);
        int b = v == "middle" ? SDL_BUTTON_MIDDLE : v == "x1" ? SDL_BUTTON_X1 : v == "x2" ? SDL_BUTTON_X2 : 0;
        if (!bindable_mouse(b)) return false;
        out = mouse(b);
        return true;
    }
    return false;
}
}  // namespace

const char* name(Action a) {
    switch (a) {
        default: return "?";
    }
}

const char* key_name(Action a) {
    switch (a) {
        default: return "?";
    }
}

const char* name(Hotkey h) {
    switch (h) {
        case Hotkey::SaveState: return "Save state";
        case Hotkey::SaveNewSlot: return "Save to new slot";
        case Hotkey::LoadState: return "Load state";
        case Hotkey::SlotPrev: return "Previous slot";
        case Hotkey::SlotNext: return "Next slot";
        case Hotkey::FastForward: return "Fast forward (hold)";
        case Hotkey::FastForwardToggle: return "Fast forward (toggle)";
        case Hotkey::Rewind: return "Rewind (hold)";
        default: return "?";
    }
}

const char* key_name(Hotkey h) {
    switch (h) {
        case Hotkey::SaveState: return "save_state";
        case Hotkey::SaveNewSlot: return "save_new_slot";
        case Hotkey::LoadState: return "load_state";
        case Hotkey::SlotPrev: return "slot_prev";
        case Hotkey::SlotNext: return "slot_next";
        case Hotkey::FastForward: return "fast_forward";
        case Hotkey::FastForwardToggle: return "fast_forward_toggle";
        case Hotkey::Rewind: return "rewind";
        default: return "?";
    }
}

std::string to_string(const Chord& c) {
    std::string s;
    for (const auto& b : c) {
        if (!s.empty()) s += "+";
        s += to_string(b);
    }
    return s;
}

bool chord_from_string(const std::string& text, Chord& out) {
    // Split before each "+key:" / "+pad:" so key names containing '+' ("Keypad +") survive.
    const std::string s = trim(text);
    Chord c;
    size_t p = 0;
    while (p < s.size()) {
        size_t next = std::string::npos;
        for (const char* tag : {"+key:", "+pad:", "+axis:"}) {
            size_t f = lower(s).find(tag, p + 1);
            if (f != std::string::npos) next = std::min(next, f);
        }
        Binding b;
        if (!parse(s.substr(p, next == std::string::npos ? std::string::npos : next - p), b, true)) return false;
        if (std::find(c.begin(), c.end(), b) == c.end()) c.push_back(b);
        if (next == std::string::npos) break;
        p = next + 1;
    }
    // One key on its own, or one or two controller buttons.
    if (c.empty() || c.size() > 2) return false;
    for (const auto& b : c) {
        if (b.kind == Binding::Kind::PadAxis) return false;
        if (b.kind == Binding::Kind::Key && c.size() != 1) return false;
    }
    out = std::move(c);
    return true;
}

std::string join_chords(const std::vector<Chord>& list) {
    if (list.empty()) return "none";
    std::string s;
    for (const auto& c : list) {
        if (!s.empty()) s += " | ";
        s += to_string(c);
    }
    return s;
}

std::vector<Chord> split_chords(const std::string& text) {
    std::vector<Chord> out;
    size_t p = 0;
    while (p <= text.size()) {
        size_t bar = text.find(" | ", p);
        std::string item = trim(text.substr(p, bar == std::string::npos ? std::string::npos : bar - p));
        Chord c;
        if (!item.empty() && chord_from_string(item, c) && std::find(out.begin(), out.end(), c) == out.end())
            out.push_back(std::move(c));
        if (bar == std::string::npos) break;
        p = bar + 3;
    }
    return out;
}

Config default_config() {
    Config c;
    auto& b = c.buttons;
    auto at = [&](Button x) -> std::vector<Binding>& { return b[int(x)]; };
    at(Button::A) = {key(SDL_SCANCODE_X), pad(SDL_GAMEPAD_BUTTON_SOUTH), pad(SDL_GAMEPAD_BUTTON_NORTH)};
    at(Button::B) = {key(SDL_SCANCODE_Z), pad(SDL_GAMEPAD_BUTTON_EAST), pad(SDL_GAMEPAD_BUTTON_WEST)};
    at(Button::Select) = {key(SDL_SCANCODE_BACKSPACE), key(SDL_SCANCODE_RSHIFT), pad(SDL_GAMEPAD_BUTTON_BACK)};
    at(Button::Start) = {key(SDL_SCANCODE_RETURN), pad(SDL_GAMEPAD_BUTTON_START)};
    at(Button::Right) = {key(SDL_SCANCODE_RIGHT), pad(SDL_GAMEPAD_BUTTON_DPAD_RIGHT), axis(SDL_GAMEPAD_AXIS_LEFTX, 1)};
    at(Button::Left) = {key(SDL_SCANCODE_LEFT), pad(SDL_GAMEPAD_BUTTON_DPAD_LEFT), axis(SDL_GAMEPAD_AXIS_LEFTX, -1)};
    at(Button::Up) = {key(SDL_SCANCODE_UP), pad(SDL_GAMEPAD_BUTTON_DPAD_UP), axis(SDL_GAMEPAD_AXIS_LEFTY, -1)};
    at(Button::Down) = {key(SDL_SCANCODE_DOWN), pad(SDL_GAMEPAD_BUTTON_DPAD_DOWN), axis(SDL_GAMEPAD_AXIS_LEFTY, 1)};
    at(Button::R) = {key(SDL_SCANCODE_S), pad(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER), axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 1)};
    at(Button::L) = {key(SDL_SCANCODE_A), pad(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER), axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 1)};
    // Shortcuts: Page Up / Page Down save and load, F-keys for the rest, and Back (Select) held with a shoulder or D-pad side on a controller.
    auto hk = [&](Hotkey h) -> std::vector<Chord>& { return c.hotkeys[int(h)]; };
    const Binding back = pad(SDL_GAMEPAD_BUTTON_BACK);
    hk(Hotkey::SaveState) = {{key(SDL_SCANCODE_PAGEUP)}, {back, pad(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)}};
    hk(Hotkey::SaveNewSlot) = {{key(SDL_SCANCODE_F9)}, {back, pad(SDL_GAMEPAD_BUTTON_DPAD_UP)}};
    hk(Hotkey::LoadState) = {{key(SDL_SCANCODE_PAGEDOWN)}, {back, pad(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)}};
    hk(Hotkey::SlotPrev) = {{key(SDL_SCANCODE_F6)}, {back, pad(SDL_GAMEPAD_BUTTON_DPAD_LEFT)}};
    hk(Hotkey::SlotNext) = {{key(SDL_SCANCODE_F7)}, {back, pad(SDL_GAMEPAD_BUTTON_DPAD_RIGHT)}};
    // Hold Tab or click the right stick to fast forward; hold ` (above Tab) or click the left
    // stick to rewind. The toggle starts unbound.
    hk(Hotkey::FastForward) = {{key(SDL_SCANCODE_TAB)}, {pad(SDL_GAMEPAD_BUTTON_RIGHT_STICK)}};
    hk(Hotkey::Rewind) = {{key(SDL_SCANCODE_GRAVE)}, {pad(SDL_GAMEPAD_BUTTON_LEFT_STICK)}};
    return c;
}

std::string join(const std::vector<Binding>& list) {
    if (list.empty()) return "none";
    std::string s;
    for (const auto& b : list) {
        if (!s.empty()) s += " | ";
        s += to_string(b);
    }
    return s;
}

std::vector<Binding> split(const std::string& text) {
    // Split on " | " (with spaces) so a key literally named "|" could never be misread.
    std::vector<Binding> out;
    size_t p = 0;
    while (p <= text.size()) {
        size_t bar = text.find(" | ", p);
        std::string item = trim(text.substr(p, bar == std::string::npos ? std::string::npos : bar - p));
        Binding b;
        if (!item.empty() && from_string(item, b) && std::find(out.begin(), out.end(), b) == out.end())
            out.push_back(b);
        if (bar == std::string::npos) break;
        p = bar + 3;
    }
    return out;
}

std::string turbo_buttons_to_string(std::uint16_t buttons) {
    std::string out;
    for (int i = 0; i < kButtonCount; ++i) {
        if (!(buttons & (1u << i))) continue;
        if (!out.empty()) out += ", ";
        out += name(Button(i));
    }
    return out.empty() ? "none" : out;
}

std::uint16_t turbo_buttons_from_string(const std::string& text) {
    std::uint16_t out = 0;
    std::size_t p = 0;
    while (p <= text.size()) {
        std::size_t comma = text.find(',', p);
        if (comma == std::string::npos) comma = text.size();
        const std::string item = lower(trim(text.substr(p, comma - p)));
        for (int i = 0; i < kButtonCount; ++i)
            if (item == lower(name(Button(i)))) out |= std::uint16_t(1u << i);
        p = comma + 1;
    }
    return out;
}

Config load(const SettingsIO& io) {
    Config c = default_config();
    if (!io.get) return c;
    c.deadzone_percent = std::clamp(std::atoi(io.get("input.deadzone", "40").c_str()), 5, 95);
    for (int i = 0; i < kButtonCount; ++i) {
        const std::string v = trim(io.get(std::string("input.") + name(Button(i)), ""));
        if (v.empty()) continue;  // keep default
        if (lower(v) == "none") c.buttons[i].clear();
        else if (auto list = split(v); !list.empty()) c.buttons[i] = std::move(list);
    }
    {
        const std::string on = lower(trim(io.get("input.turbo", "")));
        if (on == "1" || on == "true" || on == "yes" || on == "on") c.turbo.enabled = true;
        else if (on == "0" || on == "false" || on == "no" || on == "off") c.turbo.enabled = false;
        const std::string rate = trim(io.get("input.turbo.rate", ""));
        if (!rate.empty())
            c.turbo.rate = std::clamp(std::atoi(rate.c_str()), kTurboMinRate, kTurboMaxRate);
        const std::string buttons = trim(io.get("input.turbo.buttons", ""));
        if (!buttons.empty()) c.turbo.buttons = turbo_buttons_from_string(buttons);
    }
    // Save/load state used F5/F8 before Page Up/Page Down; settings still holding that old default
    // move to the new one (a binding the user picked themselves is kept).
    const Config defaults = default_config();
    auto old_default = [&](Hotkey h, SDL_Scancode was) {
        auto list = defaults.hotkeys[int(h)];
        list.front() = {key(was)};
        return join_chords(list);
    };
    const std::string old_save = old_default(Hotkey::SaveState, SDL_SCANCODE_F5);
    const std::string old_load = old_default(Hotkey::LoadState, SDL_SCANCODE_F8);
    for (int i = 0; i < kHotkeyCount; ++i) {
        const std::string v = trim(io.get(std::string("input.hotkey.") + key_name(Hotkey(i)), ""));
        if (v.empty()) continue;
        if ((Hotkey(i) == Hotkey::SaveState && v == old_save) || (Hotkey(i) == Hotkey::LoadState && v == old_load))
            continue;  // keep the new default
        if (lower(v) == "none") c.hotkeys[i].clear();
        else if (auto list = split_chords(v); !list.empty()) c.hotkeys[i] = std::move(list);
    }
    for (int i = 0; i < kActionCount; ++i) {
        const std::string v = trim(io.get(std::string("input.action.") + key_name(Action(i)), ""));
        if (v.empty() || lower(v) == "none") continue;  // unbound by default
        c.actions[i] = split(v);
    }
    return c;
}

void store(const Config& c, const SettingsIO& io) {
    if (!io.set) return;
    io.set("input.deadzone", std::to_string(c.deadzone_percent));
    io.set("input.turbo", c.turbo.enabled ? "true" : "false");
    io.set("input.turbo.rate", std::to_string(std::clamp(c.turbo.rate, kTurboMinRate, kTurboMaxRate)));
    io.set("input.turbo.buttons", turbo_buttons_to_string(c.turbo.buttons));
    for (Button b : kRemapOrder) io.set(std::string("input.") + name(b), join(c.buttons[int(b)]));
    for (int i = 0; i < kHotkeyCount; ++i)
        io.set(std::string("input.hotkey.") + key_name(Hotkey(i)), join_chords(c.hotkeys[i]));
    for (int i = 0; i < kActionCount; ++i)
        io.set(std::string("input.action.") + key_name(Action(i)), join(c.actions[i]));
}

// ---------------------------------------------------------------------------------------

Manager::Manager(SettingsIO io) : io_(std::move(io)) {
    config_ = load(io_);
    // First run (or keys added since): write the full control map so users can edit settings.ini by hand.
    if (io_.get && io_.set) {
        const std::string missing = "\x01";
        bool complete = io_.get("input.deadzone", missing) != missing &&
                        io_.get("input.turbo", missing) != missing &&
                        io_.get("input.turbo.rate", missing) != missing &&
                        io_.get("input.turbo.buttons", missing) != missing;
        for (int i = 0; complete && i < kButtonCount; ++i)
            complete = io_.get(std::string("input.") + name(Button(i)), missing) != missing;
        for (int i = 0; complete && i < kHotkeyCount; ++i)
            complete = io_.get(std::string("input.hotkey.") + key_name(Hotkey(i)), missing) != missing;
        for (int i = 0; complete && i < kActionCount; ++i)
            complete = io_.get(std::string("input.action.") + key_name(Action(i)), missing) != missing;
        if (!complete) save();
    }
    // Controllers connected before this object existed already sent their ADDED events.
    int count = 0;
    if (SDL_JoystickID* ids = SDL_GetJoysticks(&count)) {
        for (int i = 0; i < count; ++i) {
            if (SDL_IsGamepad(ids[i])) open_gamepad(ids[i]);
            else open_joystick(ids[i]);
        }
        SDL_free(ids);
    }
}

Manager::~Manager() {
    for (auto& d : devices_) {
        if (d.pad) SDL_CloseGamepad(d.pad);
        else if (d.joy) SDL_CloseJoystick(d.joy);
    }
}

void Manager::open_gamepad(SDL_JoystickID id) {
    if (device(id)) return;
    SDL_Gamepad* p = SDL_OpenGamepad(id);
    if (!p) return;
    Device d;
    d.pad = p;
    d.joy = SDL_GetGamepadJoystick(p);
    d.id = id;
    d.mapped_buttons.assign(std::max(0, SDL_GetNumJoystickButtons(d.joy)), false);
    d.mapped_axes.assign(std::max(0, SDL_GetNumJoystickAxes(d.joy)), false);
    d.mapped_hats.assign(std::max(0, SDL_GetNumJoystickHats(d.joy)), false);
    int n = 0;
    if (SDL_GamepadBinding** binds = SDL_GetGamepadBindings(p, &n)) {
        auto mark = [](std::vector<bool>& v, int index) {
            if (index >= 0 && index < int(v.size())) v[index] = true;
        };
        for (int i = 0; i < n; ++i) {
            const SDL_GamepadBinding& b = *binds[i];
            if (b.input_type == SDL_GAMEPAD_BINDTYPE_BUTTON) mark(d.mapped_buttons, b.input.button);
            else if (b.input_type == SDL_GAMEPAD_BINDTYPE_AXIS) mark(d.mapped_axes, b.input.axis.axis);
            else if (b.input_type == SDL_GAMEPAD_BINDTYPE_HAT) mark(d.mapped_hats, b.input.hat.hat);
        }
        SDL_free(binds);
    }
    devices_.push_back(std::move(d));
}

void Manager::open_joystick(SDL_JoystickID id) {
    if (device(id)) return;
    SDL_Joystick* j = SDL_OpenJoystick(id);
    if (!j) return;
    Device d;
    d.joy = j;
    d.id = id;
    devices_.push_back(std::move(d));
}

void Manager::close_device(SDL_JoystickID id) {
    for (auto it = devices_.begin(); it != devices_.end(); ++it)
        if (it->id == id) {
            if (it->pad) SDL_CloseGamepad(it->pad);
            else SDL_CloseJoystick(it->joy);
            devices_.erase(it);
            message_ = "Controller disconnected";
            return;
        }
}

const Manager::Device* Manager::device(SDL_JoystickID id) const {
    for (const auto& d : devices_)
        if (d.id == id) return &d;
    return nullptr;
}

bool Manager::held(const Binding& b, int threshold) const {
    auto unmapped = [](const std::vector<bool>& mapped, int i) { return i >= int(mapped.size()) || !mapped[i]; };
    switch (b.kind) {
        case Binding::Kind::Key: {
            const bool* k = SDL_GetKeyboardState(nullptr);
            return k && b.code >= 0 && b.code < SDL_SCANCODE_COUNT && k[b.code];
        }
        case Binding::Kind::Mouse:
            return (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_MASK(b.code)) != 0;
        default: break;
    }
    for (const auto& d : devices_) {
        switch (b.kind) {
            case Binding::Kind::PadButton:
                if (d.pad && SDL_GetGamepadButton(d.pad, SDL_GamepadButton(b.code))) return true;
                break;
            case Binding::Kind::PadAxis:
                if (d.pad) {
                    const int v = SDL_GetGamepadAxis(d.pad, SDL_GamepadAxis(b.code));
                    if (b.dir < 0 ? v < -threshold : v > threshold) return true;
                }
                break;
            // Raw inputs a gamepad layout already covers are read through that layout instead.
            case Binding::Kind::JoyButton:
                if (unmapped(d.mapped_buttons, b.code) && b.code < SDL_GetNumJoystickButtons(d.joy) &&
                    SDL_GetJoystickButton(d.joy, b.code))
                    return true;
                break;
            case Binding::Kind::JoyAxis:
                if (unmapped(d.mapped_axes, b.code) && b.code < SDL_GetNumJoystickAxes(d.joy)) {
                    const int v = SDL_GetJoystickAxis(d.joy, b.code);
                    if (b.dir < 0 ? v < -threshold : v > threshold) return true;
                }
                break;
            case Binding::Kind::JoyHat:
                if (unmapped(d.mapped_hats, b.code) && b.code < SDL_GetNumJoystickHats(d.joy) &&
                    (SDL_GetJoystickHat(d.joy, b.code) & b.dir))
                    return true;
                break;
            default: break;
        }
    }
    return false;
}

bool Manager::held(Action a, int threshold) const {
    for (const auto& b : config_.actions[int(a)])
        if (held(b, threshold)) return true;
    return false;
}

void Manager::save() const {
    store(config_, io_);
    if (io_.save) io_.save();
}

std::uint16_t Manager::poll() const {
    if (remapping()) return 0;
    const int threshold = 32767 * config_.deadzone_percent / 100;
    // Buttons that fired a shortcut stay away from the game until every pad releases them.
    std::erase_if(suppressed_, [&](int code) {
        for (const auto& d : devices_)
            if (d.pad && SDL_GetGamepadButton(d.pad, SDL_GamepadButton(code))) return false;
        return true;
    });
    std::uint16_t mask = 0;
    for (int i = 0; i < kButtonCount; ++i) {
        bool down = false;
        for (const auto& b : config_.buttons[i]) {
            if (b.kind == Binding::Kind::PadButton &&
                std::find(suppressed_.begin(), suppressed_.end(), b.code) != suppressed_.end())
                continue;
            if ((down = held(b, threshold))) break;
        }
        if (down) mask |= std::uint16_t(1u << i);
    }
    // Opposite directions cannot be pressed at once on real hardware.
    auto bit = [](Button b) { return std::uint16_t(1u << int(b)); };
    if ((mask & bit(Button::Left)) && (mask & bit(Button::Right))) mask &= ~(bit(Button::Left) | bit(Button::Right));
    if ((mask & bit(Button::Up)) && (mask & bit(Button::Down))) mask &= ~(bit(Button::Up) | bit(Button::Down));
    return mask;
}

bool Manager::hotkey_held(Hotkey h) const {
    if (h == Hotkey::Count || remapping() || !shortcuts_enabled_) return false;
    const int threshold = 32767 * config_.deadzone_percent / 100;
    for (const auto& c : config_.hotkeys[int(h)]) {
        bool all = !c.empty();
        for (const auto& b : c)
            if (!held(b, threshold)) { all = false; break; }
        if (all) return true;
    }
    return false;
}

std::uint16_t Manager::poll_frame() {
    // Extra actions (none for this game yet) would be added here, after turbo, from
    // held(Action, threshold).
    return turbo_.step(poll(), config_.turbo);
}

void Manager::start_remap(Button only, Source source) {
    start_remap();
    for (int i = 0; i < int(std::size(kRemapOrder)); ++i)
        if (kRemapOrder[i] == only) remap_index_ = i;
    remap_single_ = true;
    remap_source_ = source;
    message_.clear();
}

Button Manager::remap_target() const { return remapping() ? kRemapOrder[remap_index_] : Button::Count; }

void Manager::start_remap(Hotkey only, Source source) {
    if (only == Hotkey::Count) return;
    remap_index_ = -1;
    remap_single_ = false;
    action_remap_ = Action::Count;
    hotkey_remap_ = only;
    remap_source_ = source;
    capture_.clear();
    message_.clear();
}

void Manager::clear(Hotkey h, Source source) {
    if (h == Hotkey::Count) return;
    auto& list = config_.hotkeys[int(h)];
    list.erase(std::remove_if(list.begin(), list.end(),
                              [&](const Chord& c) { return !c.empty() && matches(source, c.front()); }),
               list.end());
    save();
    message_ = std::string(name(h)) + " unbound";
}

void Manager::start_remap(Action only, Source source) {
    if (only == Action::Count) return;
    start_remap();  // arms the stick axes
    remap_index_ = -1;
    hotkey_remap_ = Hotkey::Count;
    action_remap_ = only;
    remap_source_ = source;
    message_.clear();
}

void Manager::clear(Action a, Source source) {
    if (a == Action::Count) return;
    auto& list = config_.actions[int(a)];
    list.erase(std::remove_if(list.begin(), list.end(), [&](const Binding& b) { return matches(source, b); }),
               list.end());
    save();
    message_ = std::string(name(a)) + " unbound";
}

void Manager::clear(Button b, Source source) {
    if (b == Button::Count) return;
    auto& list = config_.buttons[int(b)];
    list.erase(std::remove_if(list.begin(), list.end(), [&](const Binding& x) { return matches(source, x); }),
               list.end());
    save();
    message_ = std::string(name(b)) + ": " + describe(b);
}

void Manager::cancel_remap() {
    if (!remapping()) return;
    remap_index_ = -1;
    remap_single_ = false;
    hotkey_remap_ = Hotkey::Count;
    action_remap_ = Action::Count;
    capture_.clear();
    config_ = load(io_);  // drop unsaved changes
    message_ = "Remap cancelled";
}

void Manager::reload() {
    if (!remapping()) config_ = load(io_);
}

void Manager::start_remap() {
    remap_index_ = 0;
    remap_single_ = false;
    action_remap_ = Action::Count;
    remap_source_ = Source::Any;
    const int threshold = 32767 * config_.deadzone_percent / 100;
    for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; ++a) {
        bool centered = true;
        for (const auto& d : devices_)
            if (d.pad && std::abs(int(SDL_GetGamepadAxis(d.pad, SDL_GamepadAxis(a)))) > threshold) centered = false;
        axis_armed_[a] = centered;
    }
    for (int a = 0; a < kMaxJoyAxes; ++a) {
        bool centered = true;
        for (const auto& d : devices_)
            if (a < SDL_GetNumJoystickAxes(d.joy) && std::abs(int(SDL_GetJoystickAxis(d.joy, a))) > threshold)
                centered = false;
        joy_axis_armed_[a] = centered;
    }
    message_ = "Rebinding controls. Esc keeps the current binding.";
}

std::string Manager::remap_prompt() const {
    if (!remapping()) return {};
    const char* what = remap_source_ == Source::Keyboard ? "Press a key"
                       : remap_source_ == Source::Controller ? "Press a controller button"
                                                              : nullptr;
    auto now = [&](std::string d) { return "  (now: " + d + ")"; };
    if (what) {
        if (hotkey_remap_ != Hotkey::Count)
            return std::string(what) + " for " + name(hotkey_remap_) + now(describe(hotkey_remap_, remap_source_));
        if (action_remap_ != Action::Count)
            return std::string(what) + " for " + name(action_remap_) + now(describe(action_remap_, remap_source_));
        const Button b = kRemapOrder[remap_index_];
        return std::string(what) + " for " + name(b) + now(describe(b, remap_source_));
    }
    if (hotkey_remap_ != Hotkey::Count)
        return std::string("Press a key or controller button(s) for ") + name(hotkey_remap_) +
               "  (now: " + describe(hotkey_remap_) + ")";
    if (action_remap_ != Action::Count)
        return std::string("Press a key or controller button for ") + name(action_remap_) +
               "  (now: " + describe(action_remap_) + ")";
    const Button b = kRemapOrder[remap_index_];
    return std::string("Press a key or pad button for ") + name(b) + "  (now: " + describe(b) + ")";
}

void Manager::assign(const Binding& bind) {
    if (!matches(remap_source_, bind)) {
        advance();  // the other device: keep the current binding
        return;
    }
    // An input drives one GBA button or extra action only, and a key is either that or a shortcut.
    for (auto& list : config_.buttons) list.erase(std::remove(list.begin(), list.end(), bind), list.end());
    for (auto& list : config_.actions) list.erase(std::remove(list.begin(), list.end(), bind), list.end());
    for (auto& list : config_.hotkeys) list.erase(std::remove(list.begin(), list.end(), Chord{bind}), list.end());
    // Replace this button's bindings of the same device type (keyboard and mouse vs controller).
    auto& list = action_remap_ != Action::Count ? config_.actions[int(action_remap_)]
                                                : config_.buttons[int(kRemapOrder[remap_index_])];
    const bool is_key = is_keyboard(bind);
    list.erase(std::remove_if(list.begin(), list.end(), [&](const Binding& b) { return is_keyboard(b) == is_key; }),
               list.end());
    list.insert(list.begin(), bind);
    advance();
}

void Manager::advance() {
    if (action_remap_ != Action::Count) {
        const Action done = action_remap_;
        action_remap_ = Action::Count;
        save();
        message_ = std::string(name(done)) + ": " + describe(done);
        return;
    }
    ++remap_index_;
    if (remap_single_ || remap_index_ >= int(std::size(kRemapOrder))) {
        remap_index_ = -1;
        remap_single_ = false;
        save();
        message_ = "Controls saved";
    }
}

void Manager::assign_hotkey(const Chord& chord) {
    const Hotkey target = hotkey_remap_;
    hotkey_remap_ = Hotkey::Count;
    capture_.clear();
    if (target == Hotkey::Count || chord.empty()) return;
    // A chord triggers one shortcut, and a key used for a shortcut stops driving a GBA button.
    for (auto& list : config_.hotkeys) list.erase(std::remove(list.begin(), list.end(), chord), list.end());
    const bool is_key = chord.front().kind == Binding::Kind::Key;
    if (is_key) {
        for (auto& list : config_.buttons)
            list.erase(std::remove(list.begin(), list.end(), chord.front()), list.end());
        for (auto& list : config_.actions)
            list.erase(std::remove(list.begin(), list.end(), chord.front()), list.end());
    }
    // Replace this shortcut's binding of the same device type, like the button rebinds do.
    auto& list = config_.hotkeys[int(target)];
    list.erase(std::remove_if(list.begin(), list.end(),
                              [&](const Chord& c) { return (c.front().kind == Binding::Kind::Key) == is_key; }),
               list.end());
    list.insert(list.begin(), chord);
    save();
    message_ = std::string(name(target)) + ": " + describe(target);
}

void Manager::fire(Hotkey h) {
    switch (h) {
        case Hotkey::SaveState: states::quick_save(); break;
        case Hotkey::SaveNewSlot: states::save_new_slot(); break;
        case Hotkey::LoadState: states::quick_load(); break;
        case Hotkey::SlotPrev: states::slot_step(-1); break;
        case Hotkey::SlotNext: states::slot_step(+1); break;
        case Hotkey::FastForwardToggle:
            ff_latched_ = !ff_latched_;
            message_ = ff_latched_ ? "Fast forward on" : "Fast forward off";
            break;
        default: break;  // FastForward and Rewind are read with hotkey_held()
    }
}

void Manager::reset_to_defaults() {
    remap_index_ = -1;
    hotkey_remap_ = Hotkey::Count;
    action_remap_ = Action::Count;
    capture_.clear();
    config_ = default_config();
    save();
    message_ = "Controls reset to defaults";
}

std::string Manager::describe(Button b, Source source) const {
    std::string s;
    for (const auto& bind : config_.buttons[int(b)]) {
        if (!matches(source, bind)) continue;
        if (!s.empty()) s += ", ";
        s += display_name(bind);
    }
    return s.empty() ? "None" : s;
}

std::string Manager::describe(Hotkey h, Source source) const {
    std::string s;
    for (const auto& c : config_.hotkeys[int(h)]) {
        if (c.empty() || !matches(source, c.front())) continue;
        if (!s.empty()) s += ", ";
        std::string part;
        for (const auto& b : c) part += (part.empty() ? "" : " + ") + display_name(b);  // "Back Button + Right Bumper"
        s += part;
    }
    return s.empty() ? "None" : s;
}

std::string Manager::describe(Action a, Source source) const {
    std::string s;
    for (const auto& bind : config_.actions[int(a)]) {
        if (!matches(source, bind)) continue;
        if (!s.empty()) s += ", ";
        s += display_name(bind);
    }
    return s.empty() ? "None" : s;
}

std::string Manager::take_message() {
    std::string m;
    m.swap(message_);
    return m;
}

bool Manager::handle_event(const SDL_Event& e) {
    switch (e.type) {
        case SDL_EVENT_JOYSTICK_ADDED:
        case SDL_EVENT_GAMEPAD_ADDED: {
            const SDL_JoystickID id = e.type == SDL_EVENT_GAMEPAD_ADDED ? e.gdevice.which : e.jdevice.which;
            if (device(id)) return false;  // already open
            // Controllers SDL knows a layout for open as gamepads; anything else as a raw joystick.
            if (SDL_IsGamepad(id)) open_gamepad(id);
            else if (e.type == SDL_EVENT_JOYSTICK_ADDED) open_joystick(id);
            if (const Device* d = device(id)) {
                const char* n = d->pad ? SDL_GetGamepadName(d->pad) : SDL_GetJoystickName(d->joy);
                message_ = std::string("Controller connected: ") + (n ? n : "gamepad");
            }
            return false;
        }
        case SDL_EVENT_JOYSTICK_REMOVED:
        case SDL_EVENT_GAMEPAD_REMOVED:
            close_device(e.type == SDL_EVENT_GAMEPAD_REMOVED ? e.gdevice.which : e.jdevice.which);
            return false;
        case SDL_EVENT_GAMEPAD_REMAPPED:
            // A new layout can change which raw inputs it covers.
            if (const Device* d = device(e.gdevice.which); d && d->pad) {
                const SDL_JoystickID id = d->id;
                close_device(id);
                open_gamepad(id);
                message_.clear();
            }
            return false;
        default: break;
    }
    if (hotkey_remap_ != Hotkey::Count) {
        switch (e.type) {
            case SDL_EVENT_KEY_DOWN: {
                if (e.key.repeat) return true;
                const SDL_Scancode sc = e.key.scancode;
                if (sc == SDL_SCANCODE_ESCAPE) {
                    hotkey_remap_ = Hotkey::Count;  // keep the current binding
                    capture_.clear();
                } else if (remap_source_ == Source::Controller) {
                    hotkey_remap_ = Hotkey::Count;  // the other device: keep the current binding
                    capture_.clear();
                } else if (!reserved_hotkey_key(sc)) {
                    assign_hotkey({key(sc)});
                }
                return true;
            }
            case SDL_EVENT_KEY_UP:
                return false;  // releases go on to the menu, so the key that started this isn't stuck
            case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
            case SDL_EVENT_JOYSTICK_AXIS_MOTION:
            case SDL_EVENT_JOYSTICK_HAT_MOTION:
                return true;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
                const Binding b = pad(SDL_GamepadButton(e.gbutton.button));
                if (remap_source_ == Source::Keyboard) {
                    hotkey_remap_ = Hotkey::Count;  // the other device: keep the current binding
                    suppressed_.push_back(b.code);
                    return true;
                }
                if (capture_.size() < 2 && std::find(capture_.begin(), capture_.end(), b) == capture_.end())
                    capture_.push_back(b);
                return true;
            }
            case SDL_EVENT_GAMEPAD_BUTTON_UP:
                // The chord is whatever was held together before the first release.
                if (!capture_.empty()) {
                    for (const auto& b : capture_) suppressed_.push_back(b.code);
                    assign_hotkey(capture_);
                }
                return false;  // the menu still sees the release
            default:
                return false;
        }
    }

    if (!remapping()) {
        if (!shortcuts_enabled_) return false;
        // Shortcuts. A controller chord fires when its last button goes down; the longest
        // matching chord wins so Back+R beats a lone Back.
        if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
            for (int h = 0; h < kHotkeyCount; ++h)
                for (const auto& c : config_.hotkeys[h])
                    if (c.size() == 1 && c[0].kind == Binding::Kind::Key && c[0].code == int(e.key.scancode)) {
                        fire(Hotkey(h));
                        return true;
                    }
        } else if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
            SDL_Gamepad* p = SDL_GetGamepadFromID(e.gbutton.which);
            int best = -1;
            const Chord* best_chord = nullptr;
            for (int h = 0; h < kHotkeyCount && p; ++h)
                for (const auto& c : config_.hotkeys[h]) {
                    bool has = false, all = true;
                    for (const auto& b : c) {
                        if (b.kind != Binding::Kind::PadButton) { all = false; break; }
                        if (b.code == int(e.gbutton.button)) has = true;
                        if (!SDL_GetGamepadButton(p, SDL_GamepadButton(b.code))) all = false;
                    }
                    if (has && all && (!best_chord || c.size() > best_chord->size())) {
                        best = h;
                        best_chord = &c;
                    }
                }
            if (best_chord) {
                for (const auto& b : *best_chord)
                    if (std::find(suppressed_.begin(), suppressed_.end(), b.code) == suppressed_.end())
                        suppressed_.push_back(b.code);
                fire(Hotkey(best));
                return true;
            }
        }
        return false;
    }

    const int threshold = 32767 * config_.deadzone_percent / 100;
    switch (e.type) {
        case SDL_EVENT_KEY_DOWN: {
            if (e.key.repeat) return true;
            const SDL_Scancode sc = e.key.scancode;
            if (sc == SDL_SCANCODE_ESCAPE) {
                advance();
            } else if (!reserved_key(sc)) {
                assign(key(sc));
            }
            return true;
        }
        case SDL_EVENT_KEY_UP:
            return false;  // releases go on to the menu, so the key that started this isn't stuck
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            assign(pad(SDL_GamepadButton(e.gbutton.button)));
            return true;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
            const int a = e.gaxis.axis, v = e.gaxis.value;
            if (a < 0 || a >= SDL_GAMEPAD_AXIS_COUNT) return true;
            if (std::abs(v) < threshold / 2) {
                axis_armed_[a] = true;
            } else if (axis_armed_[a] && std::abs(v) > threshold) {
                axis_armed_[a] = false;  // must return to center before it can bind again
                assign(axis(SDL_GamepadAxis(a), v < 0 ? -1 : 1));
            }
            return true;
        }
        // Raw joystick input binds only when no gamepad layout covers it, so a standard
        // controller's buttons still bind by name and only its extras bind by number.
        case SDL_EVENT_JOYSTICK_BUTTON_DOWN: {
            const Device* d = device(e.jbutton.which);
            const int i = e.jbutton.button;
            if (d && (i >= int(d->mapped_buttons.size()) || !d->mapped_buttons[i])) assign(joy_button(i));
            return true;
        }
        case SDL_EVENT_JOYSTICK_AXIS_MOTION: {
            const Device* d = device(e.jaxis.which);
            const int a = e.jaxis.axis, v = e.jaxis.value;
            if (!d || a >= kMaxJoyAxes || (a < int(d->mapped_axes.size()) && d->mapped_axes[a])) return true;
            if (std::abs(v) < threshold / 2) {
                joy_axis_armed_[a] = true;
            } else if (joy_axis_armed_[a] && std::abs(v) > threshold) {
                joy_axis_armed_[a] = false;
                assign(joy_axis(a, v < 0 ? -1 : 1));
            }
            return true;
        }
        case SDL_EVENT_JOYSTICK_HAT_MOTION: {
            const Device* d = device(e.jhat.which);
            const int h = e.jhat.hat;
            if (d && (h >= int(d->mapped_hats.size()) || !d->mapped_hats[h]))
                if (const int dir = hat_direction(e.jhat.value)) assign(joy_hat(h, dir));
            return true;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (!bindable_mouse(e.button.button)) return false;  // left/right clicks still work the menu
            if (remap_source_ == Source::Controller) return false;
            assign(mouse(e.button.button));
            return true;
        case SDL_EVENT_JOYSTICK_BUTTON_UP:
            return true;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            return false;
        default:
            return false;
    }
}

}  // namespace supersaga::input
