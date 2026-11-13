#include "options/options_panel.h"

#include <imgui.h>
#include <imgui_internal.h>  // RenderNavCursor for the sidebar entries

#include <algorithm>
#include <cfloat>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "app/config.h"
#include "app/theme.h"
#include "game.h"
#include "options/display.h"
#include "options/input.h"

namespace supersaga {
SettingsIO settings_io(Config& config) {
    return SettingsIO{
        [&config](const std::string& k, const std::string& d) { return config.get(k, d); },
        [&config](const std::string& k, const std::string& v) { config.set(k, v); },
        [&config]() { config.save(); },
    };
}
}  // namespace supersaga

namespace {
using namespace supersaga;
constexpr bool kShowWidescreen = game::kWidescreen;
using namespace supersaga::display;
using supersaga::input::Button;

// The rebind state lives across frames; it is created on first draw for this Config.
struct PanelState {
    Config* config = nullptr;
    std::unique_ptr<input::Manager> input;
    struct ExtraTab {
        std::string name;
        std::function<void(Config&)> draw;
        bool before_builtin = false;
    };
    std::vector<ExtraTab> extra_tabs;
    std::string select_tab;  // applied once on the next draw
    std::string current_tab;  // the page shown in the sidebar layout
    bool menu_open = false;
    std::function<void()> on_quit;
    ImFont* heading_font = nullptr;
    ImFont* small_font = nullptr;
    ImTextureID logo = 0;
    ImVec2 logo_size{0, 0};
    // Controller / keyboard focus hops between the sidebar and the page (see draw_options_menu).
    struct NavEntry {
        ImGuiID id;
        std::string page;  // empty for Resume / Settings / Quit
    };
    std::vector<NavEntry> sidebar;     // last frame's sidebar entries, top to bottom
    std::vector<NavEntry> sidebar_next;
    ImGuiID focus_target = 0;          // give this sidebar entry the focus when it is drawn
    ImGuiID cell_focused = 0;          // binding cell focused last frame (Backspace clears it)
    bool focus_page = false;           // put focus on the page's first control next frame
    ImGuiID left_from = 0;             // Left was pressed on this page control...
    int left_wait = 0;                 // ...and is checked again after this many frames
};
PanelState& state() {
    static PanelState s;
    return s;
}

void help_marker(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

template <typename E>
bool enum_combo(const char* label, E& value) {
    bool changed = false;
    if (ImGui::BeginCombo(label, name(value))) {
        for (int i = 0; i < static_cast<int>(E::Count); ++i) {
            const E v = static_cast<E>(i);
            if (ImGui::Selectable(name(v), v == value)) {
                value = v;
                changed = true;
            }
            if (v == value) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

const char* aspect_label(Aspect a) {
    switch (a) {
        case Aspect::Native: return "Off (original 3:2)";
        case Aspect::R16x10: return "16:10";
        case Aspect::R16x9: return "16:9";
        default: return name(a);
    }
}

void display_tab(Config& config) {
    Settings s = load([&](const std::string& k, const std::string& d) { return config.get(k, d); });
    bool changed = false;

    theme::section("Size and shape");
    int scale = static_cast<int>(s.scale);
    changed |= ImGui::RadioButton("Pixel perfect (integer scaling)", &scale, int(ScaleMode::Integer));
    help_marker("Every GBA pixel becomes an exact block of screen pixels, so nothing shimmers or "
                "blurs. Leaves a small border when the window is not an exact multiple.");
    changed |= ImGui::RadioButton("Fit to window", &scale, int(ScaleMode::Fit));
    help_marker("Fills as much of the window as possible. Some pixels end up one screen pixel "
                "wider than others.");
    s.scale = static_cast<ScaleMode>(scale);

    // Widescreen shows extra level at the sides (Gba::set_wide_margin); the host applies
    // display::wide_margin(aspect) and presents Gba::wide_framebuffer().
    if (kShowWidescreen) {
        if (ImGui::BeginCombo("Widescreen view", aspect_label(s.aspect))) {
            for (int i = 0; i < int(Aspect::Count); ++i) {
                const Aspect a = Aspect(i);
                if (ImGui::Selectable(aspect_label(a), a == s.aspect)) {
                    s.aspect = a;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        help_marker("Shows more of the room at the left and right instead of stretching the "
                    "picture. Pixels always stay square, and text boxes stay where they were. "
                    "Screens without a scrolling room behind them, like menus and the title "
                    "screen, stay 3:2, and past the edge of a room the sides are black.");
    }

    bool fullscreen = config.get("display.fullscreen", "0") == "1";
    if (theme::toggle("Fullscreen", &fullscreen)) {
        config.set("display.fullscreen", fullscreen ? "1" : "0");
        if (SDL_Window* w = SDL_GetKeyboardFocus()) SDL_SetWindowFullscreen(w, fullscreen);
    }
    help_marker("F11 also switches fullscreen.");

    theme::section("Look");
    bool gba_colors = s.color == ColorMode::GbaLcd;
    if (theme::toggle("Original GBA screen colors", &gba_colors)) {
        s.color = gba_colors ? ColorMode::GbaLcd : ColorMode::Raw;
        changed = true;
    }
    help_marker("Softer, less saturated colors like the original unlit GBA screen. The game's "
                "colors were chosen for that screen, so this is how they were meant to look.");

    if (ImGui::BeginCombo("Frame blending", s.blend == Blend::Off ? "Off"
                                           : s.blend == Blend::Mix ? "Blend two frames"
                                                                   : "LCD ghosting")) {
        const char* labels[] = {"Off", "Blend two frames", "LCD ghosting"};
        for (int i = 0; i < int(Blend::Count); ++i)
            if (ImGui::Selectable(labels[i], int(s.blend) == i)) {
                s.blend = Blend(i);
                changed = true;
            }
        ImGui::EndCombo();
    }
    help_marker("GBA games flicker some sprites every other frame on purpose, counting on the "
                "slow GBA screen to smooth it out. Blending averages frames to match. LCD "
                "ghosting also leaves short trails like the real screen.");

    const char* overlay_labels[] = {"None", "Pixel grid", "LCD subpixels", "Scanlines"};
    int ov = int(s.overlay);
    if (ImGui::Combo("Screen overlay", &ov, overlay_labels, int(Overlay::Count))) {
        s.overlay = Overlay(ov);
        changed = true;
    }
    help_marker("Pixel grid draws thin lines between pixels like the GBA's visible pixel gaps. "
                "LCD subpixels adds red, green and blue stripes. Overlays need at least 2x "
                "scaling (3x for subpixels).");
    ImGui::BeginDisabled(s.overlay == Overlay::None);
    changed |= ImGui::SliderInt("Overlay strength", &s.overlay_strength, 0, 100, "%d%%");
    ImGui::EndDisabled();

    if (changed) store(s, [&](const std::string& k, const std::string& v) { config.set(k, v); });

}

// Key bindings tables: a name column, then one column per device, each cell a field that
// rebinds when activated (Enter, controller A or a click) and clears on Delete, controller X or
// a right click.
using input::Source;
enum class CellAction { None, Rebind, Clear };

std::string remap_prompt_for(Source src) {
    (void)src;
    return "Press any key...";
}

bool begin_bindings_table(const char* id, const char* first, float first_width) {
    if (!ImGui::BeginTable(id, 3, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchSame |
                                      ImGuiTableFlags_PadOuterX))
        return false;
    ImGui::TableSetupColumn(first, ImGuiTableColumnFlags_WidthFixed, std::max(first_width, 110.0f));
    ImGui::TableSetupColumn("Keyboard / Mouse");
    ImGui::TableSetupColumn("Controller");
    // Plain text headers: TableHeadersRow makes them focusable, which gets in the way of
    // keyboard and controller navigation.
    ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::palette().ink_muted);
    for (int i = 0; i < 3; ++i) {
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(ImGui::TableGetColumnName(i));
    }
    ImGui::PopStyleColor();
    return true;
}

CellAction binding_cell(input::Manager& in, Source src, bool waiting, const std::string& text) {
    ImGui::PushID(int(src));
    ImGui::BeginDisabled(in.remapping() && !waiting);
    CellAction act = CellAction::None;
    if (theme::binding_cell("##cell", text.c_str(), waiting) && !in.remapping()) act = CellAction::Rebind;
    if (ImGui::IsItemFocused()) state().cell_focused = ImGui::GetItemID();
    if (!in.remapping() && act == CellAction::None) {
        // Backspace is the Mac delete key, so it clears here instead of going back to the sidebar.
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right) ||
            (ImGui::IsItemFocused() && (ImGui::IsKeyPressed(ImGuiKey_Delete, false) ||
                                        ImGui::IsKeyPressed(ImGuiKey_Backspace, false) ||
                                        ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false))))
            act = CellAction::Clear;
    }
    ImGui::EndDisabled();
    ImGui::PopID();
    return act;
}

void controls_tab(input::Manager& in, Config& config) {
    const Button waiting = in.remap_target();
    theme::section("Buttons");

    ImGui::TextWrapped("Select a binding and press Enter, A or click it, then press the new key "
                       "or controller button. Keyboard and controller bindings are kept apart, so "
                       "both keep working. Delete (Backspace) or X on a controller clears one.");
    ImGui::Text("Controllers connected: %d", in.gamepad_count());
    help_marker("Any button your controller reports can be bound, including paddles, back "
                "buttons and extra buttons. Mouse buttons 4 and 5 go in the keyboard column. "
                "Buttons remapped by Steam Input or another remapper work as whatever they send.");
    ImGui::Spacing();

    if (begin_bindings_table("bindings", "GBA button", ImGui::CalcTextSize("GBA button").x)) {
        static constexpr Button kOrder[] = {Button::Up, Button::Down, Button::Left, Button::Right,
                                            Button::A,  Button::B,    Button::L,    Button::R,
                                            Button::Start, Button::Select};
        for (Button b : kOrder) {
            ImGui::PushID(int(b));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(input::name(b));
            for (Source src : {Source::Keyboard, Source::Controller}) {
                ImGui::TableNextColumn();
                const bool wait = waiting == b && in.remap_source() == src;
                switch (binding_cell(in, src, wait, wait ? remap_prompt_for(src) : in.describe(b, src))) {
                    case CellAction::Rebind: in.start_remap(b, src); break;
                    case CellAction::Clear: in.clear(b, src); break;
                    default: break;
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (input::kActionCount > 0) {
        ImGui::Spacing();
        theme::section("Extra controls");
        ImGui::TextWrapped("Optional buttons for things the GBA does with held buttons. They start "
                           "unbound, and are a good use for paddles, back buttons and other extras.");
        const input::Action act_waiting = in.action_remap_target();
        if (begin_bindings_table("actions", "Action", ImGui::CalcTextSize("Action").x)) {
            for (int i = 0; i < input::kActionCount; ++i) {
                const auto a = input::Action(i);
                ImGui::PushID(300 + i);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(input::name(a));
                for (Source src : {Source::Keyboard, Source::Controller}) {
                    ImGui::TableNextColumn();
                    const bool wait = act_waiting == a && in.remap_source() == src;
                    switch (binding_cell(in, src, wait, wait ? remap_prompt_for(src) : in.describe(a, src))) {
                        case CellAction::Rebind: in.start_remap(a, src); break;
                        case CellAction::Clear: in.clear(a, src); break;
                        default: break;
                    }
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

    ImGui::Spacing();
    theme::section("Turbo");
    {
        const input::TurboSettings& t = in.config().turbo;
        bool on = config.get_bool("input.turbo", t.enabled);
        if (theme::toggle("Turbo buttons", &on)) {
            config.set_bool("input.turbo", on);
            in.reload();
        }
        help_marker("While you hold a turbo button, the game sees it pressed over and over. "
                    "Handy for rapid fire, but turn it off for a button you need to hold, "
                    "such as B to inhale.");
        ImGui::BeginDisabled(!on);
        int rate = std::clamp(config.get_int("input.turbo.rate", t.rate), input::kTurboMinRate,
                              input::kTurboMaxRate);
        if (ImGui::SliderInt("Turbo rate", &rate, input::kTurboMinRate, input::kTurboMaxRate,
                             rate == 1 ? "%d press per second" : "%d presses per second",
                             ImGuiSliderFlags_AlwaysClamp)) {
            config.set("input.turbo.rate", std::to_string(rate));
            in.reload();
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Turbo on:");
        static constexpr Button kTurboButtons[] = {Button::A, Button::B, Button::L, Button::R};
        std::uint16_t mask = t.buttons;
        bool mask_changed = false;
        for (Button b : kTurboButtons) {
            ImGui::SameLine();
            ImGui::PushID(200 + int(b));
            const std::uint16_t bit = std::uint16_t(1u << int(b));
            bool sel = (mask & bit) != 0;
            if (theme::checkbox(input::name(b), &sel)) {
                mask = sel ? std::uint16_t(mask | bit) : std::uint16_t(mask & ~bit);
                mask_changed = true;
            }
            ImGui::PopID();
        }
        if (mask_changed) {
            config.set("input.turbo.buttons", input::turbo_buttons_to_string(mask));
            in.reload();
        }
        ImGui::EndDisabled();
    }

    ImGui::Spacing();
    theme::section("Fast forward and rewind");
    ImGui::TextWrapped("Hold Tab (or click the right stick) to fast forward, and hold ` (the key "
                       "above Tab, or click the left stick) to rewind. Change the buttons under "
                       "Shortcuts below.");
    {
        const char* speeds[] = {"2x", "4x"};
        int speed = config.get_int("speed.fast_forward", 2) >= 4 ? 1 : 0;
        if (ImGui::Combo("Fast forward speed", &speed, speeds, 2))
            config.set("speed.fast_forward", speed ? "4" : "2");
        bool sound = config.get_bool("speed.fast_forward_sound", false);
        if (theme::toggle("Sound while fast forwarding", &sound)) config.set_bool("speed.fast_forward_sound", sound);
        help_marker("Off: fast forward is silent. On: the game's sound plays faster along "
                    "with the game.");
        bool rewind = config.get_bool("rewind.enabled", true);
        if (theme::toggle("Rewind", &rewind)) config.set_bool("rewind.enabled", rewind);
        help_marker("Keeps the last 10 seconds of play so you can step back, at twice normal "
                    "speed. Rewind is off in RetroAchievements hardcore mode, like save states and "
                    "cheats.");
    }

    ImGui::Spacing();
    theme::section("Shortcuts");
    ImGui::TextWrapped("For a controller shortcut, hold the buttons together (up to two) and "
                       "let go. F1, F11 and Esc are kept for the menu and fullscreen.");
    const input::Hotkey hk_waiting = in.hotkey_remap_target();
    if (begin_bindings_table("shortcuts", "Action", ImGui::CalcTextSize(input::name(input::Hotkey::FastForwardToggle)).x)) {
        for (int i = 0; i < input::kHotkeyCount; ++i) {
            const auto h = input::Hotkey(i);
            ImGui::PushID(100 + i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(input::name(h));
            for (Source src : {Source::Keyboard, Source::Controller}) {
                ImGui::TableNextColumn();
                const bool wait = hk_waiting == h && in.remap_source() == src;
                const std::string prompt = remap_prompt_for(src);
                switch (binding_cell(in, src, wait, wait ? prompt : in.describe(h, src))) {
                    case CellAction::Rebind: in.start_remap(h, src); break;
                    case CellAction::Clear: in.clear(h, src); break;
                    default: break;
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::BeginDisabled(in.remapping());
    if (ImGui::Button("Rebind all")) in.start_remap();
    ImGui::SameLine();
    if (ImGui::Button("Reset to defaults")) in.reset_to_defaults();
    ImGui::EndDisabled();
    if (in.remapping()) {
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) in.cancel_remap();
    }

    int dz = config.get_int("input.deadzone", in.config().deadzone_percent);
    if (ImGui::SliderInt("Stick dead zone", &dz, 5, 95, "%d%%")) {
        config.set("input.deadzone", std::to_string(dz));
        in.reload();
    }
    help_marker("How far a stick or trigger must move before it counts as a press.");

}
}  // namespace

namespace {
bool any_pressed(std::initializer_list<ImGuiKey> keys) {
    for (ImGuiKey k : keys)
        if (ImGui::IsKeyPressed(k)) return true;
    return false;
}

// One sidebar entry, after Bookie's sidebar: a rounded row that fills on hover. The selected page
// gets the raised surface. icon is optional.
bool nav_item(const char* label, bool selected, float height, const char* icon = nullptr, bool sub = false,
              const std::string& page = {}) {
    const auto& c = theme::palette();
    auto& st = state();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 size(ImGui::GetContentRegionAvail().x, height);
    const bool pressed = theme::nav_button(label, size);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 end(pos.x + size.x, pos.y + size.y);
    if (selected || hovered) dl->AddRectFilled(pos, end, theme::u32(selected ? c.raised : c.high, selected ? 1.0f : 0.6f), 8.0f);
    // Keyboard / controller focus: the same ring as every other control.
    st.sidebar_next.push_back({ImGui::GetItemID(), page});
    if (st.focus_target == ImGui::GetItemID()) {  // moved here by update_focus_hops
        ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
        ImGui::SetNavCursorVisible(true);
        st.focus_target = 0;
    }

    if (ImGui::GetItemID() == GImGui->NavId && GImGui->NavCursorVisible)
        dl->AddRect(ImVec2(pos.x - 1, pos.y - 1), ImVec2(end.x + 1, end.y + 1), theme::u32(c.highlight), 9.0f, 0, 2.0f);
    const ImVec4 text = selected || hovered ? c.ink : sub ? c.ink_muted : ImVec4(c.ink.x, c.ink.y, c.ink.z, 0.85f);
    float x = pos.x + 12.0f;
    if (icon) {
        const ImVec2 is = ImGui::CalcTextSize(icon);
        dl->AddText(ImVec2(x, pos.y + (size.y - is.y) * 0.5f), theme::u32(text), icon);
        x += is.x + 9.0f;
    }
    ImGui::PushFont(theme::font_button());
    const ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(x, pos.y + (size.y - ts.y) * 0.5f), theme::u32(text), label);
    ImGui::PopFont();
    return pressed;
}

struct Page {
    std::string name;
    std::string description;
    std::function<void()> draw;
};

// The line under each page title.
const char* default_description(const std::string& name) {
    if (name == "Game") return "Your game file, how it starts, and shortcuts.";
    if (name == "Display") return "How the game is drawn on your screen.";
    if (name == "Controls") return "Keyboard and controller buttons, turbo, fast forward and shortcuts.";
    if (name == "States") return "Save states: save and load anywhere.";
    if (name == "Cheats") return "Optional changes to the game.";
    if (name == "Achievements") return "RetroAchievements sign-in and progress.";
    if (name == "Updates") return "Your version and new releases.";
    if (name == "About") return "Version, credits and licenses.";
    return "";
}

std::vector<Page> collect_pages(PanelState& st, supersaga::Config& config) {
    std::vector<Page> pages;
    auto extras = [&](bool before) {
        for (auto& t : st.extra_tabs)
            if (t.before_builtin == before)
                pages.push_back({t.name, default_description(t.name), [&t, &config] { if (t.draw) t.draw(config); }});
    };
    extras(true);
    pages.push_back({"Display", default_description("Display"), [&config] { display_tab(config); }});
    pages.push_back({"Controls", default_description("Controls"), [&st, &config] { controls_tab(*st.input, config); }});
    extras(false);
    return pages;
}

void ensure_input(PanelState& st, supersaga::Config& config) {
    if (st.config != &config || !st.input) {
        st.config = &config;
        st.input = std::make_unique<input::Manager>(settings_io(config));
        st.input->set_shortcuts_enabled(false);
    }
}

// Draws the selected page: a large title, a muted description, then the page's sections (cards)
// in a scrollable body.
void draw_page(PanelState& st, const Page& page) {
    const auto& c = theme::palette();
    constexpr float kPadX = 32.0f, kPadTop = 26.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    // Not NavFlattened: Up and Down stay on the page. ImGui scores an item that is both beside
    // and slightly above or below as almost straight up or down, so from the edge of a card it
    // would pick a sidebar entry over the next card's first control. The sidebar is reached with
    // Left or B instead (update_focus_hops).
    const bool pane = ImGui::BeginChild("page", ImVec2(0, 0), ImGuiChildFlags_None,
                                        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    if (pane) {
        ImGui::SetCursorPos(ImVec2(kPadX, kPadTop));
        ImGui::PushFont(theme::font_title());
        ImGui::TextUnformatted((page.name + " Settings").c_str());
        ImGui::PopFont();
        if (!page.description.empty()) {
            ImGui::SetCursorPosX(kPadX);
            ImGui::TextColored(c.ink_muted, "%s", page.description.c_str());
        }
        ImGui::Dummy(ImVec2(0, 6.0f));
        // The body scrolls under the fixed title; its scrollbar sits near the panel edge.
        ImGui::SetCursorPosX(kPadX);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 8.0f);
        // NavFlattened (here and on each card): the d-pad moves straight through the cards
        // without having to "enter" each one first.
        const bool body = ImGui::BeginChild("body", ImVec2(-14.0f, -16.0f), ImGuiChildFlags_NavFlattened);
        ImGui::PopStyleVar(2);
        if (body) {
            // Text outside a section wraps where the cards end.
            ImGui::PushItemWidth(std::min(ImGui::GetContentRegionAvail().x * 0.5f, ImGui::GetFontSize() * 14.0f));
            ImGui::PushTextWrapPos(0.0f);
            if (st.focus_page) {
                ImGui::SetKeyboardFocusHere();  // the first control on the page
                st.focus_page = false;
            }
            page.draw();
            theme::end_section();
            ImGui::PopTextWrapPos();
            ImGui::PopItemWidth();
            ImGui::Dummy(ImVec2(0, 8.0f));
        }
        ImGui::EndChild();
    }
    ImGui::EndChild();
}
}  // namespace

void draw_options_panel(supersaga::Config& config) {
    auto& st = state();
    ensure_input(st, config);
    std::vector<Page> pages = collect_pages(st, config);
    if (!st.select_tab.empty()) {
        st.current_tab = std::move(st.select_tab);
        st.select_tab.clear();
    }
    auto it = std::find_if(pages.begin(), pages.end(), [&](const Page& p) { return p.name == st.current_tab; });
    if (it == pages.end()) {
        it = pages.begin();
        if (it != pages.end()) st.current_tab = it->name;
    }
    if (st.current_tab != "Controls" && st.input->remapping()) st.input->cancel_remap();  // left mid-rebind
    if (it != pages.end()) draw_page(st, *it);
    st.input->take_message();
}

bool options_panel_handle_event(const SDL_Event& event) {
    auto& st = state();
    return st.input ? st.input->handle_event(event) : false;
}

void register_options_tab(const char* name, std::function<void(supersaga::Config&)> draw, bool before_builtin) {
    auto& tabs = state().extra_tabs;
    for (auto& t : tabs)
        if (t.name == name) {
            t.draw = std::move(draw);
            return;
        }
    tabs.push_back({name, std::move(draw), before_builtin});
}

void options_select_tab(const char* name) { state().select_tab = name; }

void options_menu_toggle() { options_menu_set_open(!state().menu_open); }

void options_menu_set_open(bool open) {
    auto& st = state();
    if (!open && st.input && st.input->remapping()) st.input->cancel_remap();
    st.menu_open = open;
}

bool options_menu_open() { return state().menu_open; }

void set_options_menu_quit_handler(std::function<void()> on_quit) { state().on_quit = std::move(on_quit); }

void set_options_menu_fonts(ImFont* heading, ImFont* small) {
    state().heading_font = heading;
    state().small_font = small;
}

namespace {
// Controller and keyboard focus for the menu. ImGui's directional navigation does not reach the
// sidebar entries from the page's cards, so the sidebar is driven here, before anything is drawn:
//   sidebar: Up / Down move between entries, Right opens the focused page and moves into it,
//            B closes the menu (like Resume).
//   page:    B goes back to the sidebar, and so does Left when nothing on the page is to the left.
void update_focus_hops(PanelState& st, bool& open) {
    ImGuiContext& g = *GImGui;
    if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) || (st.input && st.input->remapping()) || g.ActiveId ||
        g.NavId == 0) {
        st.left_wait = 0;
        return;
    }
    auto it = std::find_if(st.sidebar.begin(), st.sidebar.end(), [&](const PanelState::NavEntry& e) { return e.id == g.NavId; });
    const bool on_cell = st.cell_focused != 0 && st.cell_focused == g.NavId;
    st.cell_focused = 0;
    const bool back = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight) || (!on_cell && ImGui::IsKeyPressed(ImGuiKey_Backspace));
    const bool up = any_pressed({ImGuiKey_UpArrow, ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadLStickUp});
    const bool down = any_pressed({ImGuiKey_DownArrow, ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadLStickDown});
    const bool left = any_pressed({ImGuiKey_LeftArrow, ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadLStickLeft});
    const bool right = any_pressed({ImGuiKey_RightArrow, ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickRight});
    if (it != st.sidebar.end()) {
        if (up || down || left || right) ImGui::NavMoveRequestCancel();  // handled here instead
        if (up && it != st.sidebar.begin()) st.focus_target = std::prev(it)->id;
        if (down && std::next(it) != st.sidebar.end()) st.focus_target = std::next(it)->id;
        if (right) {
            if (!it->page.empty()) st.current_tab = it->page;
            st.focus_page = true;
        }
        if (back) open = false;
        return;
    }
    auto selected = [&]() -> ImGuiID {
        for (const auto& e : st.sidebar)
            if (!e.page.empty() && e.page == st.current_tab) return e.id;
        return st.sidebar.empty() ? 0 : st.sidebar.front().id;
    };
    if (back) {
        st.focus_target = selected();
    } else if (left) {
        st.left_from = g.NavId;
        st.left_wait = 3;  // a directional move inside the page lands within a frame or two
    } else if (st.left_wait > 0 && --st.left_wait == 0 && g.NavId == st.left_from) {
        st.focus_target = selected();
    }
}
}  // namespace

void set_options_menu_logo(ImTextureID tex, ImVec2 size) {
    state().logo = tex;
    state().logo_size = size;
}

void draw_options_menu(supersaga::Config& config) {
    auto& st = state();
    if (!st.menu_open) return;
    const auto& c = theme::palette();

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    // Dim the paused game behind the menu with the theme's modal dim color (set in theme.cpp).
    ImGui::GetBackgroundDrawList()->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y),
                                                  ImGui::GetColorU32(ImGuiCol_ModalWindowDimBg));
    // A fixed 3:2 panel in window points. It never grows past kMaxWidth, so it stays the same
    // size in fullscreen and on HiDPI screens, and shrinks (keeping 3:2) in small windows.
    constexpr float kMaxWidth = 960.0f, kMargin = 24.0f;
    float w = std::min(vp->Size.x - 2 * kMargin, kMaxWidth);
    float h = w / 1.5f;
    if (h > vp->Size.y - 2 * kMargin) {
        h = vp->Size.y - 2 * kMargin;
        w = h * 1.5f;
    }
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
    bool open = true;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, c.surface);
    const bool shown = ImGui::Begin("Settings", nullptr,
                                    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    if (shown) {
        ensure_input(st, config);
        std::vector<Page> pages = collect_pages(st, config);
        if (!st.select_tab.empty()) {
            st.current_tab = std::move(st.select_tab);
            st.select_tab.clear();
        }
        if (st.current_tab.empty() && !pages.empty()) st.current_tab = pages.front().name;

        update_focus_hops(st, open);
        st.sidebar_next.clear();
        // Sidebar, after Bookie's: the game's logo, Resume, a Settings group with every page
        // indented behind a thin rule, Quit, and the close hint at the bottom.
        const float nav_w = std::max(ImGui::GetFontSize() * 11.0f, w * 0.24f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 2.0f));
        if (ImGui::BeginChild("nav", ImVec2(nav_w, 0), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 wp = ImGui::GetWindowPos();
            const float wh = ImGui::GetWindowHeight();
            dl->AddLine(ImVec2(wp.x + nav_w - 1.0f, wp.y), ImVec2(wp.x + nav_w - 1.0f, wp.y + wh), theme::u32(c.line));

            // Logo, left aligned, in a fixed 64 px header like Bookie's brand row.
            constexpr float kHeader = 64.0f, kLogoH = 26.0f;
            if (st.logo && st.logo_size.y > 0) {
                float lh = kLogoH, lw = lh * st.logo_size.x / st.logo_size.y;
                const float max_w = nav_w - 2 * 14.0f - 12.0f;
                if (lw > max_w) {
                    lw = max_w;
                    lh = lw * st.logo_size.y / st.logo_size.x;
                }
                ImGui::SetCursorPos(ImVec2(14.0f + 12.0f, (kHeader - lh) * 0.5f + 4.0f));
                ImGui::Image(st.logo, ImVec2(lw, lh));
            }
            ImGui::SetCursorPosY(kHeader + 6.0f);

            const ImGuiStyle& style = ImGui::GetStyle();
            const float line = ImGui::GetTextLineHeight();
            const float hint_h = line * 1.6f;
            const float n = float(pages.size());
            const int top_items = 2 + (st.on_quit ? 1 : 0);  // Resume, Settings, Quit
            const float gaps = 12.0f * 3.0f;
            float room = wh - ImGui::GetCursorPosY() - gaps - 12.0f;
            const bool show_hint = room - hint_h >= (n + float(top_items)) * (line + 14.0f);
            if (show_hint) room -= hint_h;
            const float item_h = std::clamp(room / (n + float(top_items)) - style.ItemSpacing.y, line + 4.0f, 34.0f);
            const float sub_h = std::min(item_h, 30.0f);

            // In game the top entry resumes play; opened from the launcher it just closes the menu.
            if (st.on_quit ? nav_item("Resume game", false, item_h, SUPERSAGA_ICON_PLAY)
                           : nav_item("Close", false, item_h, SUPERSAGA_ICON_X))
                open = false;
            if (ImGui::IsWindowAppearing()) ImGui::SetItemDefaultFocus();
            ImGui::Dummy(ImVec2(0, 8.0f));
            if (nav_item("Settings", false, item_h, SUPERSAGA_ICON_SETTINGS) && !pages.empty())
                st.current_tab = pages.front().name;
            // Pages, indented behind a vertical rule under the Settings icon.
            const float rule_x = 12.0f + ImGui::CalcTextSize(SUPERSAGA_ICON_SETTINGS).x * 0.5f;
            const float indent = rule_x + 10.0f;
            ImGui::Dummy(ImVec2(0, 2.0f));
            const ImVec2 rule_top = ImGui::GetCursorScreenPos();
            ImGui::Indent(indent);
            for (const Page& p : pages) {
                if (nav_item(p.name.c_str(), p.name == st.current_tab, sub_h, nullptr, true, p.name)) st.current_tab = p.name;
            }
            ImGui::Unindent(indent);
            const float rule_bottom = ImGui::GetCursorScreenPos().y - style.ItemSpacing.y;
            dl->AddRectFilled(ImVec2(rule_top.x + rule_x - 1.0f, rule_top.y), ImVec2(rule_top.x + rule_x + 1.0f, rule_bottom),
                              theme::u32(c.line), 1.0f);
            ImGui::Dummy(ImVec2(0, 8.0f));
            if (st.on_quit && nav_item("Quit to launcher", false, item_h, SUPERSAGA_ICON_LOG_OUT)) {
                open = false;
                st.on_quit();
            }
            if (show_hint) {
                ImGui::SetCursorPosY(wh - hint_h);
                ImGui::SetCursorPosX(14.0f + 12.0f);
                if (st.small_font) ImGui::PushFont(st.small_font);
                ImGui::TextColored(c.ink_faint, "F1 or Esc closes this menu");
                if (st.small_font) ImGui::PopFont();
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        st.sidebar.swap(st.sidebar_next);

        ImGui::SameLine(0.0f, 0.0f);
        draw_options_panel(config);
    }
    ImGui::End();
    if (!open) options_menu_set_open(false);
}
