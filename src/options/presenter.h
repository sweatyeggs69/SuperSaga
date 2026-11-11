#pragma once
// Draws the GBA frame into the SDL window with the user's display settings (scaling,
// aspect ratio, GBA LCD colors, interframe blending, overlays) and a small on-screen
// message line. Settings are edited in the F1 menu (options_panel) and stored in the app config.
#include <SDL3/SDL.h>

#include <cstdint>
#include <string>
#include <vector>

#include "options/display.h"
#include "options/settings_io.h"

namespace supersaga {

class Presenter {
public:
    Presenter(SDL_Renderer* renderer, SettingsIO io);
    ~Presenter();
    Presenter(const Presenter&) = delete;
    Presenter& operator=(const Presenter&) = delete;

    // Settings now live in the F1 menu; kept so older callers compile. Always returns false.
    bool handle_key(const SDL_KeyboardEvent&) { return false; }

    // Draws a BGR555 frame `width` x 160 (240 normally, wider in widescreen) and presents.
    // `prompt` stays on screen while non-empty; messages from show_message fade after a while.
    void present(const std::uint16_t* frame, int width, const std::string& prompt = {});
    void present(const std::uint16_t* frame, const std::string& prompt = {}) {
        present(frame, display::kWidth, prompt);
    }

    // Same as present() but without SDL_RenderPresent, so a menu can be drawn on top first.
    void draw(const std::uint16_t* frame, int width, const std::string& prompt = {});
    // Draws the last frame again (for a paused game under a menu).
    void redraw(const std::string& prompt = {});
    void show_message(const std::string& text, double seconds = 2.5);

    const display::Settings& settings() const { return settings_; }
    // Saves the next presented window image (physical pixels) as a BMP, for headless checks.
    void capture_next(std::string bmp_path) { capture_path_ = std::move(bmp_path); }

    // Forget frame-blending history (call after loading a save state or any jump in time).
    void reset_history() { blender_.reset(); }

    // Re-read settings (after the options screen changed them).
    void reload();

private:
    void draw_text_lines(const std::vector<std::string>& lines, bool top);

    SDL_Renderer* renderer_;
    SettingsIO io_;
    display::Settings settings_;
    display::ColorPipeline colors_;
    display::FrameBlender blender_;
    std::vector<std::uint32_t> rgb_;
    SDL_Texture* screen_ = nullptr;
    int screen_w_ = 0;

    SDL_Texture* overlay_ = nullptr;
    std::vector<std::uint32_t> overlay_pixels_;
    int overlay_w_ = 0, overlay_h_ = 0, overlay_cells_ = 0;
    display::Overlay overlay_kind_ = display::Overlay::None;
    int overlay_strength_ = -1;

    std::string capture_path_;
    std::vector<std::string> message_;
    std::uint64_t message_until_ns_ = 0;
};

}  // namespace supersaga
