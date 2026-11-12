#include "options/presenter.h"

#include <algorithm>
#include <sstream>

namespace supersaga {

using namespace display;

Presenter::Presenter(SDL_Renderer* renderer, SettingsIO io) : renderer_(renderer), io_(std::move(io)) {
    reload();
    // We place the frame ourselves, in physical pixels.
    SDL_SetRenderLogicalPresentation(renderer_, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);
}

Presenter::~Presenter() {
    if (overlay_) SDL_DestroyTexture(overlay_);
    if (screen_) SDL_DestroyTexture(screen_);
}

void Presenter::reload() {
    if (io_.get) settings_ = load(io_.get);
}

void Presenter::show_message(const std::string& text, double seconds) {
    message_.clear();
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) message_.push_back(line);
    message_until_ns_ = SDL_GetTicksNS() + std::uint64_t(seconds * 1e9);
}

void Presenter::present(const std::uint16_t* frame, int width, const std::string& prompt) {
    draw(frame, width, prompt);
    SDL_RenderPresent(renderer_);
}

void Presenter::draw(const std::uint16_t* frame, int width, const std::string& prompt) {
    width = std::clamp(width, 1, kMaxWideWidth);
    if (!screen_ || width != screen_w_) {
        if (screen_) SDL_DestroyTexture(screen_);
        screen_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, width, kHeight);
        SDL_SetTextureScaleMode(screen_, SDL_SCALEMODE_NEAREST);
        screen_w_ = width;
    }
    const int count = width * kHeight;
    rgb_.resize(std::size_t(count));
    colors_.convert(frame, rgb_.data(), count, settings_.color);
    blender_.process(rgb_.data(), count, settings_.blend);
    SDL_UpdateTexture(screen_, nullptr, rgb_.data(), width * int(sizeof(std::uint32_t)));
    redraw(prompt);
}

void Presenter::redraw(const std::string& prompt) {
    if (!screen_) return;
    // The layout below is in physical pixels. The UI frame before this one may have left the
    // render scale at the display density (ImGui draws at 2x on Retina), which would draw the
    // game doubled and partly off screen, so always reset it here.
    SDL_SetRenderScale(renderer_, 1.0f, 1.0f);
    int out_w = 0, out_h = 0;
    SDL_GetCurrentRenderOutputSize(renderer_, &out_w, &out_h);
    const Layout layout = compute_layout(out_w, out_h, settings_.scale, screen_w_, kHeight);
    const SDL_FRect dst{float(layout.dest.x), float(layout.dest.y), float(layout.dest.w), float(layout.dest.h)};

    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
    SDL_RenderClear(renderer_);
    SDL_RenderTexture(renderer_, screen_, nullptr, &dst);

    // The overlay is a multiply mask at the exact output size, rebuilt only when it changes.
    if (layout.dest.w != overlay_w_ || layout.dest.h != overlay_h_ || settings_.overlay != overlay_kind_ ||
        settings_.overlay_strength != overlay_strength_ || screen_w_ != overlay_cells_) {
        overlay_cells_ = screen_w_;
        overlay_w_ = layout.dest.w;
        overlay_h_ = layout.dest.h;
        overlay_kind_ = settings_.overlay;
        overlay_strength_ = settings_.overlay_strength;
        if (overlay_) SDL_DestroyTexture(overlay_);
        overlay_ = nullptr;
        if (build_overlay(overlay_kind_, overlay_strength_, overlay_w_, overlay_h_, overlay_pixels_, screen_w_, kHeight)) {
            overlay_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC,
                                         overlay_w_, overlay_h_);
            if (overlay_) {
                SDL_UpdateTexture(overlay_, nullptr, overlay_pixels_.data(), overlay_w_ * 4);
                SDL_SetTextureBlendMode(overlay_, SDL_BLENDMODE_MOD);
                SDL_SetTextureScaleMode(overlay_, SDL_SCALEMODE_NEAREST);
            }
        }
        overlay_pixels_.clear();
        overlay_pixels_.shrink_to_fit();
    }
    if (overlay_) SDL_RenderTexture(renderer_, overlay_, nullptr, &dst);

    if (!prompt.empty()) draw_text_lines({prompt}, true);
    if (!message_.empty()) {
        if (SDL_GetTicksNS() < message_until_ns_) draw_text_lines(message_, false);
        else message_.clear();
    }
    if (!capture_path_.empty()) {
        if (SDL_Surface* shot = SDL_RenderReadPixels(renderer_, nullptr)) {
            if (!SDL_SaveBMP(shot, capture_path_.c_str()))
                SDL_Log("supersaga: could not save %s: %s", capture_path_.c_str(), SDL_GetError());
            SDL_DestroySurface(shot);
        }
        capture_path_.clear();
    }
}

namespace {
// SDL's debug font only has ASCII, so translated text drops its accents ("Contrôles" ->
// "Controles") and anything else becomes '?'.
std::string ascii_for_debug_font(const std::string& in) {
    static const char kLatin1[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size();) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        unsigned cp = c;
        size_t n = 1;
        if (c >= 0xF0) { cp = '?'; n = 4; }
        else if (c >= 0xE0) { cp = '?'; n = 3; }
        else if (c >= 0xC0 && i + 1 < in.size()) { cp = ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(in[i + 1]) & 0x3Fu); n = 2; }
        i += n;
        if (cp < 0x80) out += char(cp);
        else if (cp == 0xDF) out += "ss";
        else if (cp == 0xC6) out += "AE";
        else if (cp == 0xE6) out += "ae";
        else if (cp == 0xAB || cp == 0xBB) out += '"';
        else if (cp == 0xB7) out += '.';
        else if (cp == 0xA0) out += ' ';
        else if (cp >= 0xC0 && cp <= 0xFF) out += kLatin1[cp - 0xC0];
        else out += '?';
    }
    return out;
}
}  // namespace

// Small on-screen text using SDL's built-in 8x8 debug font, scaled for high-DPI screens.
void Presenter::draw_text_lines(const std::vector<std::string>& raw_lines, bool top) {
    std::vector<std::string> lines;
    lines.reserve(raw_lines.size());
    for (const auto& l : raw_lines) lines.push_back(ascii_for_debug_font(l));
    int out_w = 0, out_h = 0;
    SDL_GetCurrentRenderOutputSize(renderer_, &out_w, &out_h);
    const float scale = float(std::max(1, std::min(out_w / 480, out_h / 320)));
    const float w = float(out_w) / scale, h = float(out_h) / scale;
    const float line_h = 11.0f, pad = 6.0f;
    size_t longest = 0;
    for (const auto& l : lines) longest = std::max(longest, l.size());
    const float box_w = std::min(w - 2 * pad, float(longest) * 8.0f + 2 * pad);
    const float box_h = float(lines.size()) * line_h + 2 * pad - 3.0f;
    const float x = pad, y = top ? pad : h - box_h - pad;

    SDL_SetRenderScale(renderer_, scale, scale);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    // Theme colors (theme.h), repeated here because the presenter has no ImGui dependency.
    SDL_SetRenderDrawColor(renderer_, 0x0A, 0x14, 0x30, 220);  // navy
    const SDL_FRect box{x, y, box_w, box_h};
    SDL_RenderFillRect(renderer_, &box);
    SDL_SetRenderDrawColor(renderer_, 0xE2, 0xD6, 0x96, 255);  // pale gold
    for (size_t i = 0; i < lines.size(); ++i)
        SDL_RenderDebugText(renderer_, x + pad, y + pad + float(i) * line_h, lines[i].c_str());
    SDL_SetRenderScale(renderer_, 1.0f, 1.0f);
}

}  // namespace supersaga
