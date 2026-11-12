#pragma once
// Launcher and menu theme, shared by every GBA port (KNID, SuperSaga, ...): DM Sans text, flat
// tinted surfaces, thin borders, 12 px rounded buttons and one accent color, after the Bookie
// web app. Every port keeps this file and theme.cpp the same; only theme_colors.cpp (the game's
// palette) differs. Fonts: DM Sans (SIL OFL 1.1) and a few Lucide icons (ISC), in assets/fonts.

#include <imgui.h>

namespace supersaga::theme {

// Color tokens. Surfaces go from darkest (surface) to lightest (high); all of them are tinted
// toward the game's colors. Defined per game in theme_colors.cpp.
struct Palette {
    ImVec4 surface;        // window and menu background
    ImVec4 card;           // launcher card and settings sections
    ImVec4 raised;         // fields, outline buttons, selected sidebar entry
    ImVec4 high;           // hovered fields and buttons, slider tracks
    ImVec4 line;           // 1 px borders and separators
    ImVec4 ink;            // main text
    ImVec4 ink_muted;      // descriptions, secondary text
    ImVec4 ink_faint;      // hints, status lines
    ImVec4 accent;         // primary buttons, checkmarks, sliders, the selected page marker
    ImVec4 accent_hover;
    ImVec4 accent_active;
    ImVec4 accent_soft;    // selection backgrounds, tags
    ImVec4 on_accent;      // text on accent buttons
    ImVec4 highlight;      // keyboard / controller focus ring, notices
    ImVec4 good;           // verified / ok
    ImVec4 bad;            // errors, Quit hover
    ImVec4 glow;           // soft background glow behind the launcher
};
const Palette& palette();
inline ImU32 u32(const ImVec4& c, float alpha_mul = 1.0f) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, c.w * alpha_mul));
}

// Loads the fonts and applies the style. ui_scale is the display content scale (1.0, 2.0, ...).
// Call once after ImGui::CreateContext() and before the first NewFrame().
void apply(float ui_scale);

ImFont* font_body();     // 17 px regular
ImFont* font_small();    // 15 px regular
ImFont* font_button();   // 17 px semibold: buttons, sidebar, section titles
ImFont* font_title();    // 30 px bold: page titles

// Lucide icons, usable in any label drawn with the theme's fonts ("" ICON_PLAY " Play").
#define SUPERSAGA_ICON_PLAY "\xee\x85\x80"         // U+E140
#define SUPERSAGA_ICON_RESTART "\xee\x85\x8c"      // U+E14C rotate-ccw
#define SUPERSAGA_ICON_SETTINGS "\xee\x85\x98"     // U+E158
#define SUPERSAGA_ICON_LOG_OUT "\xee\x84\x92"      // U+E112
#define SUPERSAGA_ICON_X "\xee\x86\xb2"            // U+E1B2
#define SUPERSAGA_ICON_CHECK "\xee\x81\xb0"        // U+E070
#define SUPERSAGA_ICON_CHEVRON_DOWN "\xee\x81\xb1" // U+E071
#define SUPERSAGA_ICON_CHEVRON_RIGHT "\xee\x81\xb3"// U+E073
#define SUPERSAGA_ICON_FOLDER_OPEN "\xee\x89\x87"  // U+E247

// Background when no game frame is showing: the surface color with a soft accent glow.
void draw_backdrop(ImDrawList* dl, ImVec2 min, ImVec2 max);

enum class ButtonKind { Primary, Outline, Ghost };
// Large launcher button. Primary is filled with the accent, Outline is a raised surface with a
// border, Ghost has no background until hovered. icon is an optional SUPERSAGA_ICON_* string.
// Keyboard / controller focus shows a ring in the highlight color.
bool button(const char* label, ImVec2 size, ButtonKind kind = ButtonKind::Primary, const char* icon = nullptr);
// Older call sites: primary = accent, otherwise outline.
inline bool big_button(const char* label, ImVec2 size, bool primary = true) {
    return button(label, size, primary ? ButtonKind::Primary : ButtonKind::Outline);
}
// InvisibleButton that keyboard and controller navigation can reach, without ImGui's own focus
// rectangle: the theme's widgets draw their own ring.
bool nav_button(const char* id, ImVec2 size);
// On/off switch with its label to the right; a drop-in for ImGui::Checkbox.
bool toggle(const char* label, bool* value);
// Square checkbox with a check icon and its label to the right; a drop-in for ImGui::Checkbox.
bool checkbox(const char* label, bool* value);
// One cell of a key bindings table: a field showing the binding (or a prompt while waiting for
// input) that returns true when clicked or activated. width 0 fills the column. The cell is the
// last item, so callers can check IsItemFocused / IsItemClicked(right) after it.
bool binding_cell(const char* id, const char* text, bool waiting, float width = 0.0f);
// Small text-only button (no background or border), centered on the current line: muted text
// that brightens on hover, with the focus ring for keyboard and controller.
bool text_button(const char* label, ImFont* font = nullptr, float x = -1.0f);
// Text centered horizontally in the current window.
void centered_text(const char* text, ImFont* font = nullptr, ImVec4 color = ImVec4(0, 0, 0, 0));

// Settings pages: each section is a card with a title. section() closes the previous card and
// opens a new one; end_section() closes the last one (the settings menu calls it after each
// page, so pages only call section()).
void section(const char* title);
void end_section();

// Draws a logo image at a fixed height, centered in the current window. Logos are trimmed to
// their visible pixels when loaded (see trim_logo), so every game's logo has the same height
// whatever its shape. max_width shrinks very wide logos to fit.
void logo_image(ImTextureID tex, ImVec2 tex_size, float height, float max_width);
// Crops transparent rows and columns from an RGBA image in place. Returns the new size.
void trim_logo(unsigned char* rgba, int& w, int& h);

}  // namespace supersaga::theme
