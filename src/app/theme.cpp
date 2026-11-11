#include "app/theme.h"

#include <imgui_internal.h>

#include <algorithm>
#include <cstring>
#include <string_view>
#include <unordered_map>

extern const unsigned char supersaga_font_dmsans_regular[];
extern const unsigned supersaga_font_dmsans_regular_size;
extern const unsigned char supersaga_font_dmsans_semibold[];
extern const unsigned supersaga_font_dmsans_semibold_size;
extern const unsigned char supersaga_font_dmsans_bold[];
extern const unsigned supersaga_font_dmsans_bold_size;
extern const unsigned char supersaga_font_lucide_icons[];
extern const unsigned supersaga_font_lucide_icons_size;

namespace supersaga::theme {

namespace {
ImFont* g_body = nullptr;
ImFont* g_small = nullptr;
ImFont* g_button = nullptr;
ImFont* g_title = nullptr;
bool g_in_section = false;

ImVec4 mix(const ImVec4& a, const ImVec4& b, float t) {
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}
ImVec4 alpha(const ImVec4& c, float a) { return ImVec4(c.x, c.y, c.z, a); }

ImFont* add_font(const unsigned char* data, unsigned size, float px, float scale) {
    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;
    cfg.OversampleH = 2;
    ImFont* f = io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(data), int(size), px * scale, &cfg);
    // Merge the icons in, a little smaller than the text and centered on it.
    static const ImWchar icon_ranges[] = {0xE000, 0xE5FF, 0};
    ImFontConfig icons;
    icons.FontDataOwnedByAtlas = false;
    icons.MergeMode = true;
    icons.PixelSnapH = true;
    icons.GlyphMinAdvanceX = px * 0.95f * scale;
    icons.GlyphOffset = ImVec2(0, px * 0.12f * scale);
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(supersaga_font_lucide_icons),
                                   int(supersaga_font_lucide_icons_size), px * 0.95f * scale, &icons, icon_ranges);
    return f;
}
}  // namespace

void apply(float ui_scale) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    // Fonts are rasterized at the pixel density and scaled back, so text stays sharp on HiDPI.
    const float px = ui_scale > 0 ? ui_scale : 1.0f;
    g_body = add_font(supersaga_font_dmsans_regular, supersaga_font_dmsans_regular_size, 17.0f, px);
    g_small = add_font(supersaga_font_dmsans_regular, supersaga_font_dmsans_regular_size, 15.0f, px);
    g_button = add_font(supersaga_font_dmsans_semibold, supersaga_font_dmsans_semibold_size, 17.0f, px);
    g_title = add_font(supersaga_font_dmsans_bold, supersaga_font_dmsans_bold_size, 30.0f, px);
    io.FontDefault = g_body;
    io.FontGlobalScale = 1.0f / px;

    const Palette& c = palette();
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    s.WindowRounding = 16.0f;
    s.ChildRounding = 12.0f;
    s.FrameRounding = 8.0f;
    s.PopupRounding = 10.0f;
    s.GrabRounding = 8.0f;
    s.ScrollbarRounding = 8.0f;
    s.TabRounding = 8.0f;
    s.WindowPadding = ImVec2(24, 22);
    s.FramePadding = ImVec2(12, 7);
    s.ItemSpacing = ImVec2(10, 10);
    s.ItemInnerSpacing = ImVec2(10, 6);
    s.ScrollbarSize = 10.0f;
    s.GrabMinSize = 14.0f;
    s.WindowBorderSize = 1.0f;
    s.ChildBorderSize = 1.0f;
    s.FrameBorderSize = 1.0f;
    s.PopupBorderSize = 1.0f;
    s.SeparatorTextBorderSize = 1.0f;
    s.SeparatorTextPadding = ImVec2(0, 4);
    s.WindowTitleAlign = ImVec2(0.5f, 0.5f);

    ImVec4* col = s.Colors;
    col[ImGuiCol_Text] = c.ink;
    col[ImGuiCol_TextDisabled] = c.ink_faint;
    col[ImGuiCol_WindowBg] = c.surface;
    col[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    col[ImGuiCol_PopupBg] = c.card;
    col[ImGuiCol_Border] = c.line;
    col[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    col[ImGuiCol_FrameBg] = c.raised;
    col[ImGuiCol_FrameBgHovered] = c.high;
    col[ImGuiCol_FrameBgActive] = c.high;
    col[ImGuiCol_TitleBg] = c.surface;
    col[ImGuiCol_TitleBgActive] = c.card;
    col[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    col[ImGuiCol_ScrollbarGrab] = alpha(c.ink, 0.14f);
    col[ImGuiCol_ScrollbarGrabHovered] = alpha(c.ink, 0.24f);
    col[ImGuiCol_ScrollbarGrabActive] = alpha(c.ink, 0.32f);
    col[ImGuiCol_CheckMark] = c.accent;
    col[ImGuiCol_SliderGrab] = c.accent;
    col[ImGuiCol_SliderGrabActive] = c.accent_hover;
    // Plain ImGui buttons are the outline kind; theme::button draws the accent ones.
    col[ImGuiCol_Button] = c.raised;
    col[ImGuiCol_ButtonHovered] = c.high;
    col[ImGuiCol_ButtonActive] = mix(c.high, c.ink, 0.08f);
    col[ImGuiCol_Header] = c.accent_soft;
    col[ImGuiCol_HeaderHovered] = c.high;
    col[ImGuiCol_HeaderActive] = alpha(c.accent, 0.25f);
    col[ImGuiCol_Separator] = c.line;
    col[ImGuiCol_SeparatorHovered] = c.line;
    col[ImGuiCol_SeparatorActive] = c.accent;
    col[ImGuiCol_Tab] = c.raised;
    col[ImGuiCol_TabHovered] = c.high;
    col[ImGuiCol_TabSelected] = c.high;
    col[ImGuiCol_TableHeaderBg] = c.raised;
    col[ImGuiCol_TableBorderStrong] = c.line;
    col[ImGuiCol_TableBorderLight] = c.line;
    col[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    col[ImGuiCol_TableRowBgAlt] = alpha(c.ink, 0.025f);
    col[ImGuiCol_TextSelectedBg] = alpha(c.accent, 0.35f);
    col[ImGuiCol_NavCursor] = c.highlight;
    col[ImGuiCol_ModalWindowDimBg] = alpha(mix(c.surface, ImVec4(0, 0, 0, 1), 0.5f), 0.6f);
}

ImFont* font_body() { return g_body; }
ImFont* font_small() { return g_small; }
ImFont* font_button() { return g_button; }
ImFont* font_title() { return g_title; }

void draw_backdrop(ImDrawList* dl, ImVec2 mn, ImVec2 mx) {
    const Palette& c = palette();
    dl->AddRectFilled(mn, mx, u32(c.surface));
    const float w = mx.x - mn.x, h = mx.y - mn.y;
    // Two soft glows: the game's glow color top right, the accent bottom left.
    auto glow = [&](ImVec2 at, float radius, const ImVec4& color, float strength) {
        constexpr int kSteps = 32;
        for (int i = kSteps; i >= 1; --i)
            dl->AddCircleFilled(at, radius * float(i) / kSteps, u32(color, strength / kSteps), 64);
    };
    glow(ImVec2(mn.x + w * 0.75f, mn.y + h * 0.2f), std::max(w, h) * 0.55f, c.glow, 0.9f);
    glow(ImVec2(mn.x + w * 0.15f, mn.y + h * 0.95f), std::max(w, h) * 0.45f, c.accent, 0.25f);
}

bool nav_button(const char* id, ImVec2 size) {
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
    const bool pressed = ImGui::InvisibleButton(id, size, ImGuiButtonFlags_EnableNav);
    ImGui::PopStyleColor();
    return pressed;
}

bool button(const char* label, ImVec2 size, ButtonKind kind, const char* icon) {
    const Palette& c = palette();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool pressed = nav_button(label, size);
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    const bool focused = ImGui::IsItemFocused() && GImGui->NavCursorVisible;
    const ImVec2 a = pos, b(pos.x + size.x, pos.y + size.y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rounding = 12.0f;
    const float alpha = ImGui::GetStyle().Alpha;  // dimmed inside BeginDisabled()
    auto u32 = [&](const ImVec4& col) { return theme::u32(col, alpha); };

    ImVec4 bg(0, 0, 0, 0), text = c.ink;
    bool border = false;
    switch (kind) {
        case ButtonKind::Primary:
            bg = held ? c.accent_active : hovered ? c.accent_hover : c.accent;
            text = c.on_accent;
            break;
        case ButtonKind::Outline:
            bg = held ? mix(c.high, c.ink, 0.08f) : hovered ? c.high : c.raised;
            border = true;
            break;
        case ButtonKind::Ghost:
            if (hovered || held) bg = c.raised;
            text = hovered ? c.ink : c.ink_muted;
            break;
    }
    if (bg.w > 0) dl->AddRectFilled(a, b, u32(bg), rounding);
    if (border) dl->AddRect(a, b, u32(c.line), rounding, 0, 1.0f);
    if (focused) {
        const float o = 3.0f;
        dl->AddRect(ImVec2(a.x - o, a.y - o), ImVec2(b.x + o, b.y + o), u32(c.highlight), rounding + o, 0, 2.0f);
    }

    ImGui::PushFont(g_button);
    const char* end = ImGui::FindRenderedTextEnd(label);
    const ImVec2 ts = ImGui::CalcTextSize(label, end);
    const float gap = 8.0f;
    const float iw = icon ? ImGui::CalcTextSize(icon).x : 0.0f;
    const float total = ts.x + (icon ? iw + gap : 0.0f);
    float x = a.x + (size.x - total) * 0.5f;
    const float y = a.y + (size.y - ts.y) * 0.5f;
    if (icon) {
        dl->AddText(ImVec2(x, y), u32(text), icon);
        x += iw + gap;
    }
    dl->AddText(ImVec2(x, y), u32(text), label, end);
    ImGui::PopFont();
    return pressed;
}

bool toggle(const char* label, bool* value) {
    const Palette& c = palette();
    const ImGuiStyle& style = ImGui::GetStyle();
    const char* end = ImGui::FindRenderedTextEnd(label);
    const ImVec2 ts = ImGui::CalcTextSize(label, end);
    const float h = std::max(ImGui::GetFrameHeight(), ts.y);
    const float th = 22.0f, tw = 40.0f;
    ImGui::AlignTextToFramePadding();  // a help marker on the same line lines up with the label
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float label_w = ts.x > 0 ? style.ItemInnerSpacing.x + ts.x : 0.0f;
    const bool pressed = nav_button(label, ImVec2(tw + label_w, h));
    if (pressed) *value = !*value;
    const bool hovered = ImGui::IsItemHovered();
    const bool focused = ImGui::IsItemFocused() && GImGui->NavCursorVisible;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a(pos.x, pos.y + (h - th) * 0.5f), b(a.x + tw, a.y + th);
    const ImVec4 track = *value ? (hovered ? c.accent_hover : c.accent) : (hovered ? mix(c.high, c.ink, 0.08f) : c.high);
    dl->AddRectFilled(a, b, u32(track), th * 0.5f);
    if (focused) dl->AddRect(ImVec2(a.x - 3, a.y - 3), ImVec2(b.x + 3, b.y + 3), u32(c.highlight), th * 0.5f + 3, 0, 2.0f);
    const float r = th * 0.5f - 3.0f;
    const ImVec2 knob(*value ? b.x - 3.0f - r : a.x + 3.0f + r, a.y + th * 0.5f);
    dl->AddCircleFilled(knob, r, u32(*value ? c.on_accent : c.ink), 24);
    if (ts.x > 0)
        dl->AddText(ImVec2(pos.x + tw + style.ItemInnerSpacing.x, pos.y + (h - ts.y) * 0.5f),
                    ImGui::GetColorU32(ImGuiCol_Text), label, end);
    return pressed;
}

bool checkbox(const char* label, bool* value) {
    const Palette& c = palette();
    const ImGuiStyle& style = ImGui::GetStyle();
    const char* end = ImGui::FindRenderedTextEnd(label);
    const ImVec2 ts = ImGui::CalcTextSize(label, end);
    const float h = std::max(ImGui::GetFrameHeight(), ts.y);
    const float bs = 18.0f;
    ImGui::AlignTextToFramePadding();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float label_w = ts.x > 0 ? style.ItemInnerSpacing.x + ts.x : 0.0f;
    const bool pressed = nav_button(label, ImVec2(bs + label_w, h));
    if (pressed) *value = !*value;
    const bool hovered = ImGui::IsItemHovered();
    const bool focused = ImGui::IsItemFocused() && GImGui->NavCursorVisible;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a(pos.x, pos.y + (h - bs) * 0.5f), b(a.x + bs, a.y + bs);
    const float rounding = 4.0f;
    if (*value) {
        dl->AddRectFilled(a, b, u32(hovered ? c.accent_hover : c.accent), rounding);
        ImGui::PushFont(g_small);
        const ImVec2 is = ImGui::CalcTextSize(SUPERSAGA_ICON_CHECK);
        dl->AddText(ImVec2(a.x + (bs - is.x) * 0.5f, a.y + (bs - is.y) * 0.5f), u32(c.on_accent),
                    SUPERSAGA_ICON_CHECK);
        ImGui::PopFont();
    } else {
        dl->AddRectFilled(a, b, u32(hovered ? c.high : c.raised), rounding);
        dl->AddRect(a, b, u32(hovered ? c.ink_faint : c.line), rounding, 0, 1.0f);
    }
    if (focused) dl->AddRect(ImVec2(a.x - 3, a.y - 3), ImVec2(b.x + 3, b.y + 3), u32(c.highlight), rounding + 3, 0, 2.0f);
    if (ts.x > 0)
        dl->AddText(ImVec2(pos.x + bs + style.ItemInnerSpacing.x, pos.y + (h - ts.y) * 0.5f),
                    ImGui::GetColorU32(ImGuiCol_Text), label, end);
    return pressed;
}

bool binding_cell(const char* id, const char* text, bool waiting, float width) {
    const Palette& c = palette();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float w = width > 0 ? width : ImGui::GetContentRegionAvail().x;
    const float h = ImGui::GetFrameHeight();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool pressed = nav_button(id, ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    const bool focused = ImGui::IsItemFocused() && GImGui->NavCursorVisible;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a = pos, b(pos.x + w, pos.y + h);
    const float rounding = 8.0f;
    dl->AddRectFilled(a, b, u32(waiting ? c.accent_soft : hovered ? c.high : c.raised), rounding);
    dl->AddRect(a, b, u32(waiting ? c.accent : c.line), rounding, 0, 1.0f);
    if (focused) dl->AddRect(ImVec2(a.x - 3, a.y - 3), ImVec2(b.x + 3, b.y + 3), u32(c.highlight), rounding + 3, 0, 2.0f);
    const bool none = !waiting && std::string_view(text) == "None";
    const ImVec4 ink = waiting ? c.accent : none ? c.ink_faint : c.ink;
    const float ty = a.y + (h - ImGui::GetFontSize()) * 0.5f;
    const ImVec2 ta(a.x + style.FramePadding.x, ty), tb(b.x - style.FramePadding.x, b.y);
    ImGui::PushStyleColor(ImGuiCol_Text, ink);
    ImGui::RenderTextEllipsis(dl, ta, tb, tb.x, tb.x, text, nullptr, nullptr);
    ImGui::PopStyleColor();
    return pressed;
}

void centered_text(const char* text, ImFont* font, ImVec4 color) {
    if (font) ImGui::PushFont(font);
    float w = ImGui::CalcTextSize(text).x;
    ImGui::SetCursorPosX((ImGui::GetWindowSize().x - w) * 0.5f);
    if (color.w > 0) ImGui::TextColored(color, "%s", text);
    else ImGui::TextUnformatted(text);
    if (font) ImGui::PopFont();
}

void section(const char* title) {
    end_section();
    const Palette& c = palette();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, c.card);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0f, 16.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
    ImGui::BeginChild(title, ImVec2(-6.0f, 0),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding |
                          ImGuiChildFlags_NavFlattened);  // controller focus moves straight through the cards
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    g_in_section = true;
    ImGui::PushFont(g_button);
    const char* end = ImGui::FindRenderedTextEnd(title);
    ImGui::TextUnformatted(title, end);
    ImGui::PopFont();
    // Inputs take about half the card so their labels stay readable beside them.
    ImGui::PushItemWidth(std::min(ImGui::GetContentRegionAvail().x * 0.5f, ImGui::GetFontSize() * 14.0f));
    ImGui::PushTextWrapPos(0.0f);
}

void end_section() {
    if (!g_in_section) return;
    g_in_section = false;
    ImGui::PopTextWrapPos();
    ImGui::PopItemWidth();
    ImGui::EndChild();
    ImGui::Dummy(ImVec2(0, 4.0f));
}

void logo_image(ImTextureID tex, ImVec2 tex_size, float height, float max_width) {
    if (!tex || tex_size.x <= 0 || tex_size.y <= 0) return;
    float h = height, w = h * tex_size.x / tex_size.y;
    if (w > max_width) {
        w = max_width;
        h = w * tex_size.y / tex_size.x;
    }
    // Keep the reserved height fixed so the rest of the card does not move between games.
    const float top = ImGui::GetCursorPosY() + (height - h) * 0.5f;
    const float start = ImGui::GetCursorPosY();
    ImGui::SetCursorPos(ImVec2((ImGui::GetWindowSize().x - w) * 0.5f, top));
    ImGui::Image(tex, ImVec2(w, h));
    ImGui::SetCursorPosY(start + height + ImGui::GetStyle().ItemSpacing.y);
}

void trim_logo(unsigned char* rgba, int& w, int& h) {
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (rgba[(y * w + x) * 4 + 3] > 8) {
                x0 = std::min(x0, x);
                x1 = std::max(x1, x);
                y0 = std::min(y0, y);
                y1 = std::max(y1, y);
            }
    if (x1 < 0) return;  // fully transparent: leave it
    const int nw = x1 - x0 + 1, nh = y1 - y0 + 1;
    for (int y = 0; y < nh; ++y)
        std::memmove(rgba + size_t(y) * nw * 4, rgba + (size_t(y + y0) * w + x0) * 4, size_t(nw) * 4);
    w = nw;
    h = nh;
}

}  // namespace supersaga::theme
