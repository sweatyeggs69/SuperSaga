#include "app/app.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>
#include <stb_image.h>

#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "app/achievements.h"
#include "app/cheats.h"
#include "app/bmp.h"
#include "app/config.h"
#include "app/fsutil.h"
#include "app/platform.h"
#include "app/save_import.h"
#include "app/rewind.h"
#include "app/states.h"
#include "app/licenses.h"
#include "app/theme.h"
#include "app/toast.h"
#include "game.h"
#include "gba/gba.h"
#include "options/input.h"
#include "options/options_panel.h"
#include "options/display.h"
#include "options/presenter.h"

extern const unsigned char supersaga_image_logo[];
extern const unsigned supersaga_image_logo_size;
#include "sha1.h"
#include "update/update_ui.h"
#include "update/updater.h"

namespace supersaga {
namespace fs = std::filesystem;

namespace {

constexpr int kW = gba::kScreenWidth, kH = gba::kScreenHeight;
constexpr const char* kRetailBiosSha1 = "300c20df6731a33952ded8c436f7f186d25d3492";
constexpr double kFrameSeconds = double(gba::kCyclesPerFrame) / gba::kCpuHz;

// Keeps a row of buttons inside the settings page: stays on the same line only when the next
// button still fits, otherwise starts a new line.
void same_line_if_fits(const char* next_label) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float next_w = ImGui::CalcTextSize(next_label, nullptr, true).x + style.FramePadding.x * 2.0f;
    const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    if (ImGui::GetItemRectMax().x + style.ItemSpacing.x + next_w <= right) ImGui::SameLine();
}

void draw_test_pattern(std::array<std::uint16_t, kW * kH>& fb, std::uint32_t frame) {
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            auto r = std::uint16_t((x * 31) / (kW - 1)), g = std::uint16_t((y * 31) / (kH - 1));
            auto b = std::uint16_t((frame / 2 + x + y) & 31);
            fb[y * kW + x] = std::uint16_t(r | (g << 5) | (b << 10));
        }
}

class App {
public:
    explicit App(const AppOptions& opt) : opt_(opt), config_(opt.pref_dir) {}
    int run();

private:
    enum class Screen { FirstRun, Launcher, Game };

    bool init_window();
    void shutdown();
    void load_configured_rom();
    void accept_rom(const fs::path& path);
    void open_rom_dialog();
    void open_bios_dialog();
    std::optional<std::vector<std::uint8_t>> load_bios(std::string* error);
    std::unique_ptr<gba::Gba> make_gba(bool with_save);
    void start_attract();
    void start_game();
    void handle_event(const SDL_Event& e);
    void update_emulation();
    void draw_ui();
    void draw_first_run();
    void draw_launcher();
    void draw_game_tab(Config& config);
    void draw_save_section();
    void draw_shortcut_controls(bool big);
    // Save states
    void ensure_store();
    SDL_Texture* thumb_texture(int slot);
    bool save_slot(int slot);
    bool save_new_slot();
    void quick_save();
    bool load_slot(int slot);
    void auto_save();
    void startup();
    std::string hotkey_hint(bool brief) const;
    void show_toast(const std::string& text, int slot, toast::Style style = toast::Style::Success);
    void draw_states_tab();
    void draw_cheats_tab(Config& config);
    bool cheats_allowed() const;
    bool rewind_allowed() const;
    void update_speed();  // fast forward and rewind, read once per refresh
    rewind::History rewind_;
    const gba::Gba* rewind_owner_ = nullptr;  // the game the history belongs to
    int rewind_tick_ = 0;
    bool rewinding_ = false;
    bool rewind_key_was_held_ = false;
    int ff_speed_ = 1;  // emulated frames per normal frame; 1 when not fast forwarding
    std::unique_ptr<states::Store> store_;
    int slot_ = states::kDefaultSlot;
    bool slot_was_auto_ = false;  // the saved current slot was the old Auto slot (0)
    std::array<SDL_Texture*, states::kSlots + 1> thumb_tex_{};
    std::array<std::time_t, states::kSlots + 1> thumb_at_{};
    std::string states_msg_;
    bool states_msg_err_ = false;
    int rename_slot_ = -1, delete_slot_ = -1, file_slot_ = 1;
    char rename_buf_[64] = {};
    std::optional<std::string> picked_state_import_, picked_state_export_;
    struct StateAction { long frame; char op; int slot; };
    std::vector<StateAction> state_script_;
    size_t state_script_pos_ = 0;
    ShortcutPlace pending_shortcut_place_ = ShortcutPlace::Applications;
    bool confirm_shortcut_ = false;
    std::string bios_checked_path_ = "\x01", bios_status_;
    fs::path current_save_path();
    void prepare_import(const fs::path& file);
    void apply_pending_save();
    void export_save(const fs::path& dst);
    void save_message(std::string text, bool error);
    void set_menu_open(bool open);
    void return_to_launcher();
    void draw_backdrop();
    void begin_card(const char* id, ImVec2 size);
    void draw_card_logo();
    void capture_window(const std::string& path);
    void end_card();

    AppOptions opt_;
    Config config_;
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* frame_tex_ = nullptr;
    SDL_Texture* logo_tex_ = nullptr;  // the SuperSaga logo on the launcher card
    int logo_w_ = 0, logo_h_ = 0;
    void load_logo();
    SDL_AudioStream* audio_ = nullptr;
    std::unique_ptr<Presenter> presenter_;           // in-game video (display.* options)
    std::unique_ptr<input::Manager> controls_;       // keyboard/gamepad (input.* options)
    std::unique_ptr<Achievements> achievements_;     // RetroAchievements (ra.* options)
    std::unique_ptr<update::Updater> updater_;       // GitHub Releases updates (updates.* options)

    Screen screen_ = Screen::FirstRun;
    VerifiedRom rom_;
    std::string rom_error_;
    std::string info_;  // transient message (shortcut created, ...)
    double info_until_ = 0;
    std::unique_ptr<gba::Gba> attract_;  // launcher backdrop: the game's own attract loop, never saved
    std::unique_ptr<gba::Gba> game_;
    bool game_paused_ = false;
    std::array<std::uint16_t, kW * kH> pattern_{};
    std::vector<std::int16_t> audio_buf_;
    long frames_ = 0;
    bool running_ = true;
    double game_started_at_ = 0;
    bool menu_was_open_ = false;

    // Frame pacing: with vsync on a ~60 Hz display one emulated frame per refresh; otherwise a
    // time accumulator; without vsync, sleep-based pacing.
    bool vsync_ = false;
    double refresh_hz_ = 0;
    bool lockstep_ = false;  // one emulated frame per refresh
    double audio_fill_avg_ = -1;
    Uint64 audio_log_at_ = 0;
    void update_refresh_rate();
    void update_audio_rate();
    std::vector<std::pair<long, std::uint16_t>> script_;  // --input, in game frames
    size_t script_pos_ = 0;
    std::uint16_t script_keys_ = 0;
    long game_frames_ = 0;

    std::mutex pick_mutex_;
    std::optional<std::string> picked_rom_, picked_bios_, picked_import_, picked_export_;

    // Save import / restore: data waiting for the user's confirmation.
    struct PendingSave {
        std::vector<std::uint8_t> data;
        std::string source, format, warning;
    };
    std::optional<PendingSave> pending_save_;
    bool open_save_confirm_ = false;
    bool open_backup_list_ = false;
    std::string save_msg_;
    bool save_msg_error_ = false;
    fs::path import_from_cli_;
    std::string pick_error_;
};

// ---- setup ---------------------------------------------------------------------------------

bool App::init_window() {
    config_.erase("ui.language");  // left by 1.1.x, which had a launcher language setting
    window_ = SDL_CreateWindow("SuperSaga", opt_.window_w > 0 ? opt_.window_w : 960,
                               opt_.window_h > 0 ? opt_.window_h : 640,
                               SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
    if (!window_) return false;
    SDL_SetWindowMinimumSize(window_, 720, 480);
    renderer_ = SDL_CreateRenderer(window_, nullptr);
    if (!renderer_) return false;
    vsync_ = SDL_SetRenderVSync(renderer_, 1);
    update_refresh_rate();
    frame_tex_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_XBGR1555, SDL_TEXTUREACCESS_STREAMING, kW, kH);
    SDL_SetTextureScaleMode(frame_tex_, SDL_SCALEMODE_NEAREST);
    load_logo();

    SDL_AudioSpec spec{SDL_AUDIO_S16, 2, gba::kAudioRate};
    if (!opt_.mute)
        audio_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (audio_) SDL_ResumeAudioStreamDevice(audio_);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // layout is fixed; settings live in settings.ini
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ImGui_ImplSDL3_InitForSDLRenderer(window_, renderer_);
    ImGui_ImplSDLRenderer3_Init(renderer_);
    theme::apply(SDL_GetWindowPixelDensity(window_));
    set_options_menu_fonts(theme::font_button(), theme::font_small());
    if (config_.get_string("display.fullscreen", "0") == "1") SDL_SetWindowFullscreen(window_, true);
    toast::set_renderer(renderer_);
    presenter_ = std::make_unique<Presenter>(renderer_, settings_io(config_));
    controls_ = std::make_unique<input::Manager>(settings_io(config_));
    achievements_ = std::make_unique<Achievements>(config_, renderer_);
    updater_ = std::make_unique<update::Updater>(SUPERSAGA_VERSION);
    register_options_tab("Game", [this](Config& c) { draw_game_tab(c); }, /*before_builtin=*/true);
    register_options_tab("States", [this](Config&) { draw_states_tab(); });
    register_options_tab("Cheats", [this](Config& c) { draw_cheats_tab(c); });
    achievements_->set_restart_handler([this] {
        if (!game_) return;
        game_->flush_save();
        game_.reset();
        game_ = make_gba(true);
        presenter_->reset_history();
    });
    register_options_tab("Achievements", [this](Config&) { achievements_->draw_tab(); });
    // Shortcuts and the save on quit use the current slot, Slot 1 until the player picks another.
    // (0 was the Auto slot, removed in favor of Slot 1.)
    slot_ = config_.get_int("states.current_slot", states::kDefaultSlot);
    slot_was_auto_ = slot_ == 0;
    if (slot_ < states::kFirstSlot || slot_ > states::kSlots) slot_ = states::kDefaultSlot;
    states::set_hotkeys({[this] { if (game_ && screen_ == Screen::Game) quick_save(); },
                         [this] { if (game_ && screen_ == Screen::Game) load_slot(slot_); },
                         [this](int d) {
                             if (!game_ || screen_ != Screen::Game) return;
                             slot_ = states::step_slot(slot_, d);
                             config_.set_int("states.current_slot", slot_);
                             ensure_store();
                             const states::SlotInfo& i = store_ ? store_->info(slot_) : states::SlotInfo{};
                             show_toast("Save state slot: " + states::slot_name(slot_) + (i.exists ? "" : " (empty)"),
                                        i.exists ? slot_ : -1, toast::Style::Info);
                         },
                         [this] { if (game_ && screen_ == Screen::Game) save_new_slot(); }});
    register_options_tab("Updates", [this](Config& c) { update_ui::draw_tab(*updater_, c); });
    register_options_tab("About", [](Config&) {
        const auto& c = theme::palette();
        ImGui::TextUnformatted("SuperSaga " SUPERSAGA_VERSION);
        ImGui::PushFont(theme::font_small());
        ImGui::TextColored(c.ink_muted, "An unofficial project. Not affiliated with Nintendo.");
        if (ImGui::TextLink("https://github.com/sweatyeggs69/SuperSaga")) SDL_OpenURL("https://github.com/sweatyeggs69/SuperSaga");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Open in your browser");
        ImGui::PopFont();
        auto block = [](const char* title, std::string_view text) {
            if (!ImGui::CollapsingHeader(title)) return;
            ImGui::PushFont(theme::font_small());
            ImGui::BeginChild(title, ImVec2(0, 260), ImGuiChildFlags_Borders);
            ImGui::PushTextWrapPos(0);
            ImGui::TextUnformatted(text.data(), text.data() + text.size());
            ImGui::PopTextWrapPos();
            ImGui::EndChild();
            ImGui::PopFont();
        };
        block("SuperSaga license (MIT)", licenses::supersaga());
        block("Third-party notices", licenses::notices());
        block("DM Sans font license (SIL OFL 1.1)", licenses::font_ofl());
    });
    update_ui::set_restart_handler([this] { running_ = false; });  // relaunched after shutdown()
    // No network in scripted test runs, unless a test feed is set.
    if (opt_.frames < 0 || std::getenv("SUPERSAGA_UPDATE_URL")) update_ui::init(*updater_, config_);
    SDL_ShowWindow(window_);
    return true;
}

void App::shutdown() {
    auto_save();
    achievements_.reset();  // sends unlocks still in flight
    toast::set_renderer(nullptr);
    for (SDL_Texture*& t : thumb_tex_)
        if (t) { SDL_DestroyTexture(t); t = nullptr; }
    if (game_) game_->flush_save();
    game_.reset();
    attract_.reset();
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    controls_.reset();
    presenter_.reset();
    if (audio_) SDL_DestroyAudioStream(audio_);
    if (frame_tex_) SDL_DestroyTexture(frame_tex_);
    if (logo_tex_) SDL_DestroyTexture(logo_tex_);
    if (renderer_) SDL_DestroyRenderer(renderer_);
    if (window_) SDL_DestroyWindow(window_);
}

void App::load_configured_rom() {
    std::string path = config_.get_string("rom.path");
    if (path.empty()) return;
    VerifiedRom v = verify_rom(utf8_path(path));
    if (v.ok && v.rom.sha1 == config_.get_string("rom.sha1", v.rom.sha1)) {
        rom_ = std::move(v);
    } else {
        rom_error_ = v.ok ? "The saved ROM changed since it was chosen. Please choose it again." : v.error;
    }
}

void App::accept_rom(const fs::path& path) {
    VerifiedRom v = verify_rom(path);
    if (!v.ok) {
        rom_error_ = v.error;
        return;
    }
    rom_error_.clear();
    config_.set_string("rom.path", path_utf8(v.path));
    config_.set_string("rom.sha1", v.rom.sha1);
    rom_ = std::move(v);
    game_.reset();
    store_.reset();
    screen_ = Screen::Launcher;
    start_attract();
}

void App::open_rom_dialog() {
    static const SDL_DialogFileFilter filters[] = {{"Game Boy Advance ROM", "gba"}, {"All files", "*"}};
    std::string start = rom_.ok ? rom_.path.parent_path().string() : std::string();
    SDL_ShowOpenFileDialog(
        [](void* self, const char* const* files, int) {
            auto* app = static_cast<App*>(self);
            std::lock_guard<std::mutex> lock(app->pick_mutex_);
            if (!files) app->pick_error_ = SDL_GetError();
            else if (files[0]) app->picked_rom_ = files[0];
        },
        this, window_, filters, 2, start.empty() ? nullptr : start.c_str(), false);
}

void App::open_bios_dialog() {
    static const SDL_DialogFileFilter filters[] = {{"GBA BIOS", "bin;rom"}, {"All files", "*"}};
    SDL_ShowOpenFileDialog(
        [](void* self, const char* const* files, int) {
            auto* app = static_cast<App*>(self);
            std::lock_guard<std::mutex> lock(app->pick_mutex_);
            if (files && files[0]) app->picked_bios_ = files[0];
        },
        this, window_, filters, 2, nullptr, false);
}

std::optional<std::vector<std::uint8_t>> App::load_bios(std::string* error) {
    fs::path p = !opt_.bios.empty() ? opt_.bios : utf8_path(config_.get_string("bios.path"));
    if (p.empty()) return std::nullopt;
    std::ifstream f(p, std::ios::binary);
    std::vector<std::uint8_t> bios((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::string hash = to_hex(sha1(bios.data(), bios.size()));
    if (bios.size() != 0x4000 || hash != kRetailBiosSha1) {
        if (error) *error = "The BIOS file is not the retail GBA BIOS; using the built-in BIOS.";
        return std::nullopt;
    }
    return bios;
}

std::unique_ptr<gba::Gba> App::make_gba(bool with_save) {
    auto g = std::make_unique<gba::Gba>(rom_.rom.data, with_save ? save_path_for(config_, rom_) : fs::path());
    g->verbose = opt_.verbose;
    std::string err;
    if (auto bios = load_bios(&err)) {
        g->use_bios(std::move(*bios));
        // Real games boot through the BIOS intro (logo + chime); the launcher backdrop skips it.
        g->set_bios_boot(with_save);
        g->reset();
    } else if (!err.empty()) {
        info_ = err;
        info_until_ = ImGui::GetTime() + 6;
    }
    if (with_save && achievements_) achievements_->game_started(*g, rom_.rom.data);
    return g;
}

void App::start_attract() {
    if (!rom_.ok || opt_.test_pattern) return;
    attract_ = make_gba(false);
}

void App::start_game() {
    if (!rom_.ok && !opt_.test_pattern) return;
    if (!game_ && rom_.ok) game_ = make_gba(true);
    attract_.reset();
    game_paused_ = false;
    screen_ = Screen::Game;
    game_started_at_ = ImGui::GetTime();
    if (audio_) SDL_ClearAudioStream(audio_);
    presenter_->reload();
    controls_->reload();
    presenter_->show_message("F1 or Esc: settings menu", 3.0);
    set_options_menu_quit_handler([this] { return_to_launcher(); });
}

// ---- main loop -----------------------------------------------------------------------------

void App::set_menu_open(bool open) {
    options_menu_set_open(open);
}

void App::return_to_launcher() {
    controls_->set_fast_forward_latched(false);
    auto_save();
    options_menu_set_open(false);
    game_paused_ = true;
    if (game_) game_->flush_save();
    set_options_menu_quit_handler({});
    screen_ = Screen::Launcher;
}

void App::handle_event(const SDL_Event& e) {
    if (e.type == SDL_EVENT_QUIT) { running_ = false; return; }
    if (e.type == SDL_EVENT_WINDOW_DISPLAY_CHANGED || e.type == SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED)
        update_refresh_rate();
    bool key = e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat;
    if (key && e.key.key == SDLK_F11) {
        bool fs_on = !(SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN);
        SDL_SetWindowFullscreen(window_, fs_on);
        config_.set_string("display.fullscreen", fs_on ? "1" : "0");  // keep the menu checkbox in sync
        return;
    }
    if (options_menu_open()) {
        // The menu owns input: rebinding captures first, then ImGui. Nothing reaches the game.
        if (options_panel_handle_event(e)) return;
        if (key && (e.key.key == SDLK_F1 || e.key.key == SDLK_ESCAPE)) { set_menu_open(false); return; }
        ImGui_ImplSDL3_ProcessEvent(&e);
        return;
    }
    if (key && e.key.key == SDLK_F1 && screen_ != Screen::FirstRun) { set_menu_open(true); return; }
    if (screen_ == Screen::Game) {
        if (controls_->handle_event(e)) return;  // gamepad hotplug / remap capture
        if (key && e.key.key == SDLK_ESCAPE) {
            if (opt_.test_pattern && !rom_.ok) { running_ = false; return; }
            set_menu_open(true);
        }
        return;
    }
    controls_->handle_event(e);  // gamepad hotplug
    ImGui_ImplSDL3_ProcessEvent(&e);
}

void App::update_emulation() {
    audio_buf_.clear();
    const std::uint16_t* pixels = nullptr;
    if (screen_ == Screen::Game && opt_.test_pattern) {
        draw_test_pattern(pattern_, std::uint32_t(frames_));
        pixels = pattern_.data();
    } else if (screen_ == Screen::Game && game_ && options_menu_open()) {
        pixels = game_->framebuffer();  // paused under the settings menu
    } else if (screen_ == Screen::Game && game_ && rewinding_) {
        // Rewind: step back through the history, silently, one snapshot per frame.
        if (const std::vector<std::uint8_t>* back = rewind_.step_back()) {
            game_->load_state(*back);
        }
        pixels = game_->framebuffer();
    } else if (screen_ == Screen::Game && game_) {
        game_->set_wide_margin(game::kWidescreen ? display::wide_margin(presenter_->settings().aspect) : 0);
        while (script_pos_ < script_.size() && script_[script_pos_].first <= game_frames_)
            script_keys_ = script_[script_pos_++].second;
        const std::uint16_t keys = std::uint16_t(controls_->poll_frame() | script_keys_);
        game_->set_keys(keys);
        game_->run_frame();
        if (cheats_allowed()) {
            cheats::apply(game_->ewram_mut(), cheats::Flags{config_.get_bool("cheats.hp", false),
                                                            config_.get_bool("cheats.bp", false),
                                                            config_.get_bool("cheats.coins", false)});
        }
        if (achievements_) achievements_->frame(*game_);
        ++game_frames_;
        while (state_script_pos_ < state_script_.size() && state_script_[state_script_pos_].frame <= game_frames_) {
            const StateAction& a = state_script_[state_script_pos_++];
            if (a.op == 's') save_slot(a.slot);
            else if (a.op == 'l') load_slot(a.slot);
        }
        game_->take_audio(audio_buf_);
        pixels = game_->framebuffer();
        // Rewind history: a snapshot every other frame (rewinding plays them back one per frame,
        // so it runs at twice normal speed).
        if (rewind_allowed()) {
            if (++rewind_tick_ >= 2) {
                rewind_tick_ = 0;
                rewind_.set_limits(10 * 30, std::size_t(64) << 20);  // the last 10 seconds
                rewind_.push(game_->save_state());
            }
        } else if (!rewind_.empty()) {
            rewind_.clear();
        }
    } else if (game_ && game_paused_) {
        pixels = game_->framebuffer();
    } else if (attract_) {
        attract_->run_frame();
        attract_->take_audio(audio_buf_);
        if (!config_.get_bool("launcher.music", false)) audio_buf_.clear();
        pixels = attract_->framebuffer();
    }
    if (pixels) SDL_UpdateTexture(frame_tex_, nullptr, pixels, kW * sizeof(std::uint16_t));
    // Safety cap (~200 ms queued, of game time); normal sync comes from update_audio_rate().
    if (audio_ && !audio_buf_.empty() && SDL_GetAudioStreamQueued(audio_) < gba::kAudioRate / 5 * 4 * ff_speed_) {
        const bool ff_mute = ff_speed_ > 1 && !config_.get_bool("speed.fast_forward_sound", false);
        SDL_SetAudioStreamGain(audio_, opt_.silent_audio || ff_mute ? 0.0f : screen_ == Screen::Game ? 1.0f : 0.35f);
        SDL_PutAudioStreamData(audio_, audio_buf_.data(), int(audio_buf_.size() * sizeof(std::int16_t)));
    }
}

void App::update_refresh_rate() {
    refresh_hz_ = 0;
    if (const SDL_DisplayMode* m = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window_))) {
        if (m->refresh_rate_numerator > 0 && m->refresh_rate_denominator > 0)
            refresh_hz_ = double(m->refresh_rate_numerator) / m->refresh_rate_denominator;
        else
            refresh_hz_ = m->refresh_rate;
    }
    const double gba_hz = 1.0 / kFrameSeconds;  // 59.7275
    lockstep_ = vsync_ && refresh_hz_ > 0 && std::fabs(refresh_hz_ / gba_hz - 1.0) < 0.015;
    if (opt_.verbose)
        std::fprintf(stderr, "supersaga: vsync %s, display %.3f Hz, %s\n", vsync_ ? "on" : "off", refresh_hz_,
                     lockstep_ ? "one frame per refresh" : vsync_ ? "accumulator pacing" : "timer pacing");
}

// Dynamic rate control: the game makes 32768 Hz audio per emulated frame. In lockstep the game runs
// at the display rate, so play the audio faster/slower by that ratio, plus at most +/-0.5% to keep
// about 3 frames of audio queued (no underruns, no growing latency).
void App::update_audio_rate() {
    if (!audio_) return;
    const double frame_bytes = gba::kAudioRate * kFrameSeconds * 4;  // stereo s16
    double fill = double(SDL_GetAudioStreamQueued(audio_)) / frame_bytes;  // in emulated frames
    audio_fill_avg_ = audio_fill_avg_ < 0 ? fill : audio_fill_avg_ * 0.95 + fill * 0.05;
    // While fast forwarding the sound plays faster too (same samples, played back quicker, no
    // filtering), and each refresh uses ff_speed_ frames of audio, so keep that much more queued.
    const double target = 2.0 + ff_speed_;
    double base = (lockstep_ ? refresh_hz_ * kFrameSeconds : 1.0) * ff_speed_;
    double err = std::clamp((audio_fill_avg_ - target) / target, -1.0, 1.0);
    double ratio = base * (1.0 + 0.005 * err);
    SDL_SetAudioStreamFrequencyRatio(audio_, float(ratio));
    if (opt_.verbose) {
        Uint64 now = SDL_GetTicksNS();
        if (now >= audio_log_at_) {
            std::fprintf(stderr, "supersaga: audio queue %.2f frames (avg %.2f), rate ratio %.5f\n", fill, audio_fill_avg_, ratio);
            audio_log_at_ = now + 1000000000ull;
        }
    }
}

void App::draw_backdrop() {
    int ow = 0, oh = 0;
    SDL_GetRenderOutputSize(renderer_, &ow, &oh);
    bool have_frame = attract_ || (game_ && game_paused_);
    if (!have_frame) return;
    // Cover the window, keep the aspect ratio, dim it so the UI reads clearly.
    float scale = std::max(float(ow) / kW, float(oh) / kH);
    SDL_FRect dst{(ow - kW * scale) * 0.5f, (oh - kH * scale) * 0.5f, kW * scale, kH * scale};
    SDL_SetTextureColorMod(frame_tex_, 120, 105, 150);
    SDL_SetTextureScaleMode(frame_tex_, SDL_SCALEMODE_LINEAR);
    SDL_RenderTexture(renderer_, frame_tex_, nullptr, &dst);
    SDL_SetTextureColorMod(frame_tex_, 255, 255, 255);
}

void App::capture_window(const std::string& path) {
    SDL_SetRenderScale(renderer_, 1, 1);
    if (SDL_Surface* shot = SDL_RenderReadPixels(renderer_, nullptr)) {
        SDL_SaveBMP(shot, path.c_str());
        SDL_DestroySurface(shot);
    }
}

void App::load_logo() {
    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load_from_memory(supersaga_image_logo, int(supersaga_image_logo_size), &w, &h, &n, 4);
    if (!px) return;  // the launcher falls back to the text title
    // Trimmed to its visible pixels so it is drawn at the same height as every other port's logo.
    theme::trim_logo(px, w, h);
    logo_tex_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, h);
    if (logo_tex_) {
        SDL_UpdateTexture(logo_tex_, nullptr, px, w * 4);
        SDL_SetTextureBlendMode(logo_tex_, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(logo_tex_, SDL_SCALEMODE_LINEAR);
        logo_w_ = w;
        logo_h_ = h;
        set_options_menu_logo((ImTextureID)(intptr_t)logo_tex_, ImVec2(float(w), float(h)));
    }
    stbi_image_free(px);
}

void App::begin_card(const char* id, ImVec2 size) {
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::palette().card);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28.0f, 28.0f));
    ImGui::Begin(id, nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void App::end_card() { ImGui::End(); }

// The logo at the top of the launcher cards: the same height in every port.
void App::draw_card_logo() {
    constexpr float kLogoHeight = 60.0f;
    if (logo_tex_) {
        theme::logo_image((ImTextureID)(intptr_t)logo_tex_, ImVec2(float(logo_w_), float(logo_h_)), kLogoHeight,
                          ImGui::GetContentRegionAvail().x);
    } else {
        ImGui::Dummy(ImVec2(0, (kLogoHeight - ImGui::GetFontSize() * 30.0f / 17.0f) * 0.5f));
        theme::centered_text("SuperSaga", theme::font_title(), theme::palette().accent);
    }
}

void App::draw_first_run() {
    const auto& c = theme::palette();
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    begin_card("##firstrun", ImVec2(std::min(560.0f, ds.x - 40), 0));
    draw_card_logo();
    theme::centered_text((std::string(game::kTitle) + ", native on your computer").c_str(), theme::font_small(), c.ink_muted);
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::PushTextWrapPos(0);
    ImGui::TextWrapped(
        "To play, choose your own copy of the game: a ROM file dumped from your cartridge of %s.",
        game::kTitleWithRegion);
    ImGui::PushFont(theme::font_small());
    ImGui::TextColored(c.ink_muted, "SuperSaga only remembers where the file is. It never copies, uploads or changes it.");
    ImGui::PopFont();
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, 8));
    float bw = ImGui::GetContentRegionAvail().x;
    bool back = ImGui::IsKeyPressed(ImGuiKey_Backspace, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false);
    if (theme::button("Choose ROM...", ImVec2(bw, 50), theme::ButtonKind::Primary, SUPERSAGA_ICON_FOLDER_OPEN))
        open_rom_dialog();
    if (ImGui::IsWindowAppearing()) ImGui::SetItemDefaultFocus();
    if (!rom_error_.empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(c.bad, "%s", rom_error_.c_str());
        ImGui::PopTextWrapPos();
    }
    if (back) ImGui::SetKeyboardFocusHere();
    if (theme::button("Quit", ImVec2(bw, 44), theme::ButtonKind::Outline, SUPERSAGA_ICON_LOG_OUT)) running_ = false;
    end_card();
}

namespace {
std::string time_ago(std::time_t t) {
    const std::time_t now = std::time(nullptr);
    const long long d = t > 0 && now > t ? (long long)(now - t) : 0;
    auto unit = [](long long n, const char* one) {
        return std::to_string(n) + " " + one + (n == 1 ? "" : "s") + " ago";
    };
    if (d < 60) return "just now";
    if (d < 3600) return unit(d / 60, "minute");
    if (d < 86400) return unit(d / 3600, "hour");
    return unit(d / 86400, "day");
}
}  // namespace

void App::draw_launcher() {
    const auto& c = theme::palette();
    using theme::ButtonKind;
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    begin_card("##launcher", ImVec2(std::min(460.0f, ds.x - 40), 0));
    draw_card_logo();
    theme::centered_text(game::kTitleWithRegion, theme::font_small(), c.ink_muted);
    ImGui::Dummy(ImVec2(0, 6));

    const ImGuiStyle& style = ImGui::GetStyle();
    float bw = ImGui::GetContentRegionAvail().x;
    const float half = (bw - style.ItemSpacing.x) * 0.5f;
    // Keyboard/gamepad: the main button starts focused; B / Backspace moves the focus to Quit.
    bool back = !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) &&
                (ImGui::IsKeyPressed(ImGuiKey_Backspace, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false));
    bool focus_main = ImGui::IsWindowAppearing();
    ensure_store();
    const int latest = store_ ? store_->latest() : -1;
    const states::SlotInfo* latest_info = latest > 0 ? &store_->info(latest) : nullptr;
    bool has_latest = latest_info != nullptr;
    if (game_ && game_paused_) {
        if (theme::button("Resume", ImVec2(bw, 50), ButtonKind::Primary, SUPERSAGA_ICON_PLAY)) start_game();
        if (focus_main) ImGui::SetItemDefaultFocus();
        if (theme::button("Restart", ImVec2(bw, 44), ButtonKind::Outline, SUPERSAGA_ICON_RESTART)) {
            game_->flush_save();
            game_.reset();
            start_game();
        }
    } else if (has_latest) {
        if (theme::button("Continue", ImVec2(bw, 50), ButtonKind::Primary, SUPERSAGA_ICON_PLAY))
            load_slot(latest);
        if (focus_main) ImGui::SetItemDefaultFocus();
        if (theme::button("Play from the title screen", ImVec2(bw, 44), ButtonKind::Outline, SUPERSAGA_ICON_RESTART))
            start_game();
    } else {
        if (theme::button("Play", ImVec2(bw, 50), ButtonKind::Primary, SUPERSAGA_ICON_PLAY)) start_game();
        if (focus_main) ImGui::SetItemDefaultFocus();
    }
    // Shortcuts are created from Settings > Game.
    if (theme::button("Settings", ImVec2(half, 44), ButtonKind::Outline, SUPERSAGA_ICON_SETTINGS)) set_menu_open(true);
    ImGui::SameLine();
    if (back) ImGui::SetKeyboardFocusHere();
    if (theme::button("Quit", ImVec2(half, 44), ButtonKind::Outline, SUPERSAGA_ICON_LOG_OUT)) running_ = false;

    // Status lines only; the Settings button already says where settings live.
    const bool show_info = !info_.empty() && ImGui::GetTime() < info_until_;
    if (!rom_error_.empty() || show_info) {
        ImGui::PushFont(theme::font_small());
        ImGui::PushTextWrapPos(0);
        if (!rom_error_.empty()) ImGui::TextColored(c.bad, "%s", rom_error_.c_str());
        if (show_info) ImGui::TextColored(c.highlight, "%s", info_.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
    }
    update_ui::draw_launcher_notice(*updater_, config_);

    // Footer: ROM status on the left, most recent save state and version on the right.
    constexpr float kFooterTrim = 10.0f;
    ImGui::Dummy(ImVec2(0, 2));
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddLine(p, ImVec2(p.x + bw, p.y), theme::u32(c.line), 1.0f);
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::PushFont(theme::font_small());
        const ImVec2 q = ImGui::GetCursorScreenPos();
        const float lh = ImGui::GetTextLineHeight();
        dl->AddCircleFilled(ImVec2(q.x + 3, q.y + lh * 0.5f), 3.0f, theme::u32(c.good), 12);
        const ImU32 faint = theme::u32(c.ink_faint);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(q.x + 12, q.y), faint, "ROM verified");
        std::string right = "v" SUPERSAGA_VERSION;
        if (has_latest && !(game_ && game_paused_)) right = "Saved " + time_ago(std::time_t(latest_info->saved_at)) + "  ·  " + right;
        const float rw = ImGui::CalcTextSize(right.c_str()).x;
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(q.x + bw - rw, q.y), faint, right.c_str());
        // The line reserves less than its full height: the card's bottom padding plus the font's
        // descent and line gap left the footer looking further from the edge than the sides.
        ImGui::Dummy(ImVec2(bw, std::max(1.0f, lh - kFooterTrim)));
        ImGui::PopFont();
    }

    end_card();
}

void App::ensure_store() {
    if (store_ || !rom_.ok) return;
    store_ = std::make_unique<states::Store>(config_.pref_dir() / "states" / rom_.rom.sha1, rom_.rom.sha1,
                                             save_path_for(config_, rom_));
    // The old Auto state became Slot 1 and pushed slots up by one: keep pointing at the same state
    // (Slot 1 if Auto was the current slot).
    if (store_->shifted() > 0 && !slot_was_auto_ && slot_ <= store_->shifted()) {
        slot_ += 1;
        config_.set_int("states.current_slot", slot_);
    }
}

SDL_Texture* App::thumb_texture(int slot) {
    ensure_store();
    if (!store_) return nullptr;
    const states::SlotInfo& i = store_->info(slot);
    size_t k = size_t(slot);
    if (!i.exists || i.thumb.empty() || !i.error.empty()) {
        if (thumb_tex_[k]) { SDL_DestroyTexture(thumb_tex_[k]); thumb_tex_[k] = nullptr; }
        return nullptr;
    }
    if (!thumb_tex_[k] || thumb_at_[k] != i.saved_at) {
        if (!thumb_tex_[k]) {
            thumb_tex_[k] = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_XBGR1555, SDL_TEXTUREACCESS_STATIC, kW, kH);
            SDL_SetTextureScaleMode(thumb_tex_[k], SDL_SCALEMODE_LINEAR);
        }
        SDL_UpdateTexture(thumb_tex_[k], nullptr, i.thumb.data(), kW * 2);
        thumb_at_[k] = i.saved_at;
    }
    return thumb_tex_[k];
}

// The user's current save state shortcuts (rebindable in Controls). brief: first binding only.
std::string App::hotkey_hint(bool brief) const {
    auto d = [&](input::Hotkey h) {
        std::string s = controls_ ? controls_->describe(h) : std::string();
        if (brief) s = s.substr(0, s.find(','));
        return s.empty() ? std::string("unbound") : s;
    };
    if (brief)
        return d(input::Hotkey::SaveState) + " save  " + d(input::Hotkey::SaveNewSlot) + " new  " +
               d(input::Hotkey::LoadState) + " load  " +
               d(input::Hotkey::SlotPrev) + "/" + d(input::Hotkey::SlotNext) + " slot";
    return "save " + d(input::Hotkey::SaveState) + "; save to new slot " + d(input::Hotkey::SaveNewSlot) + "; load " + d(input::Hotkey::LoadState) + "; previous slot " +
           d(input::Hotkey::SlotPrev) + "; next slot " + d(input::Hotkey::SlotNext);
}

void App::show_toast(const std::string& text, int slot, toast::Style style) {
    std::vector<std::uint8_t> rgba;
    if (slot >= 0 && store_) {
        const states::SlotInfo& i = store_->info(slot);
        if (!i.thumb.empty()) {
            rgba.resize(size_t(kW) * kH * 4);
            for (size_t p = 0; p < i.thumb.size(); ++p) {
                std::uint16_t c = i.thumb[p];
                auto ex = [](std::uint16_t v) { return std::uint8_t((v << 3) | (v >> 2)); };
                rgba[p * 4 + 0] = ex(c & 31);
                rgba[p * 4 + 1] = ex((c >> 5) & 31);
                rgba[p * 4 + 2] = ex((c >> 10) & 31);
                rgba[p * 4 + 3] = 255;
            }
        }
    }
    toast::show(text, {}, rgba.empty() ? nullptr : rgba.data(), rgba.empty() ? 0 : kW, rgba.empty() ? 0 : kH,
                style == toast::Style::Warning ? 4500 : 2500, style);
}

bool App::save_slot(int slot) {
    ensure_store();
    if (!store_ || !game_) return false;
    std::string err;
    bool overwrote = store_->info(slot).exists;
    bool ok = store_->save(slot, *game_, &err);
    if (ok) show_toast(overwrote ? "Slot " + std::to_string(slot) + " overwritten"
                                 : "State saved to slot " + std::to_string(slot),
                       slot);
    else toast::show("Could not save the state", err, nullptr, 0, 0, 4500, toast::Style::Warning);
    states_msg_ = ok ? (overwrote ? "Overwrote " : "Saved to ") + states::slot_name(slot) + "." : err;
    states_msg_err_ = !ok;
    return ok;
}

// Saves into the next empty numbered slot after the current one and makes it current.
bool App::save_new_slot() {
    ensure_store();
    if (!store_ || !game_) return false;
    std::vector<bool> used(states::kSlots + 1, false);
    for (int s = 1; s <= states::kSlots; ++s) used[size_t(s)] = store_->info(s).exists;
    int s = states::next_free_slot(used, slot_);
    if (s < 0) {
        std::string why = "All " + std::to_string(states::kSlots) + " slots are full. Delete one in the States tab, " +
                          "or save over the current slot.";
        toast::show("No empty slot", why, nullptr, 0, 0, 4500, toast::Style::Warning);
        states_msg_ = why;
        states_msg_err_ = true;
        return false;
    }
    slot_ = s;
    config_.set_int("states.current_slot", slot_);
    return save_slot(s);
}

// The Save state shortcut: overwrite the current slot (default) or use a new slot each time.
void App::quick_save() {
    if (config_.get_string("states.quick_save", "overwrite") == "new") save_new_slot();
    else save_slot(slot_);
}

bool App::load_slot(int slot) {
    ensure_store();
    if (!store_) return false;
    if (std::string why; achievements_ && !achievements_->can_load_state(&why)) {
        show_toast(why, -1, toast::Style::Warning);
        states_msg_ = why;
        states_msg_err_ = true;
        return false;
    }
    const states::SlotInfo& info = store_->info(slot);
    if (!info.exists || !info.error.empty()) {
        std::string why = info.exists ? info.error : states::slot_name(slot) + " is empty.";
        show_toast(why, -1, toast::Style::Warning);
        states_msg_ = why;
        states_msg_err_ = true;
        return false;
    }
    bool created = !game_;
    if (created) game_ = make_gba(true);
    std::string err;
    if (!store_->load(slot, *game_, &err)) {
        if (created) game_.reset();
        toast::show("Could not load the state", err, nullptr, 0, 0, 4500, toast::Style::Warning);
        states_msg_ = err;
        states_msg_err_ = true;
        return false;
    }
    start_game();
    presenter_->reset_history();
    rewind_.clear();
    if (achievements_) achievements_->state_loaded();
    show_toast("State loaded from slot " + std::to_string(slot), slot);
    states_msg_ = "Loaded " + states::slot_name(slot) + ".";
    states_msg_err_ = false;
    return true;
}

// Startup (no ROM argument): launcher, or straight into the game for --play / the shortcut and
// for "Auto-load save state on launch". Never shows a launcher frame when going into the game.
void App::startup() {
    load_configured_rom();
    SDL_PumpEvents();  // so a Shift held while launching is seen
    bool shift = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
    bool autoload = config_.get_bool("states.autoload", false);
    bool exists = false, usable = false;
    int resume = -1;
    std::string why;
    if (rom_.ok) {
        ensure_store();
        resume = store_->latest();
        usable = resume > 0;
        for (int s = states::kFirstSlot; s <= states::kSlots && !usable; ++s) {
            const states::SlotInfo& i = store_->info(s);
            if (i.exists && !exists) why = states::slot_name(s) + ": " + i.error;
            exists = exists || i.exists;
        }
        exists = exists || usable;
    } else {
        why = rom_error_.empty() ? "No ROM is chosen yet." : rom_error_;
    }
    switch (states::choose_startup(rom_.ok, opt_.play, autoload, exists, usable, shift)) {
        case states::Startup::Resume:
            if (load_slot(resume)) return;
            why = states_msg_;
            break;
        case states::Startup::BootGame:
            start_game();
            return;
        case states::Startup::LauncherWithError:
            break;
        case states::Startup::Launcher:
            return;
    }
    toast::show(autoload ? "Couldn't auto-load the save state" : "Couldn't start the game", why, nullptr, 0, 0,
                6000, toast::Style::Warning);
}

void App::auto_save() {
    if (!game_ || game_frames_ < 60) return;
    ensure_store();
    if (store_) {
        std::string err;
        if (!store_->save(slot_, *game_, &err)) std::fprintf(stderr, "supersaga: auto save failed: %s\n", err.c_str());
    }
}

void App::draw_states_tab() {
    const auto& c = theme::palette();
    ensure_store();
    if (!store_) {
        ImGui::TextUnformatted("Choose your ROM first (Game tab).");
        return;
    }
    ImGui::TextWrapped("Shortcuts in game: %s. Current: %s. Change them in Controls.", hotkey_hint(true).c_str(),
                       states::slot_name(slot_).c_str());
    ImGui::BeginDisabled(!store_->can_undo_load() || !game_ || (achievements_ && achievements_->hardcore()));
    if (ImGui::Button("Undo last load")) {
        std::string err;
        bool ok = store_->undo_load(*game_, &err);
        if (ok) presenter_->reset_history();
        if (ok && achievements_) achievements_->state_loaded();
        states_msg_ = ok ? "Went back to before the last load." : err;
        states_msg_err_ = !ok;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Show folder")) SDL_OpenURL(file_url(store_->dir()).c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(!game_);
    if (ImGui::Button("Save to new slot")) save_new_slot();
    ImGui::EndDisabled();
    {
        int mode = config_.get_string("states.quick_save", "overwrite") == "new" ? 1 : 0;
        const char* modes[] = {"Overwrite the current slot", "Save to a new slot each time"};
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Save state shortcut:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(300);
        if (ImGui::Combo("##quicksave", &mode, modes, 2))
            config_.set_string("states.quick_save", mode == 1 ? "new" : "overwrite");
    }
    if (!states_msg_.empty()) ImGui::TextColored(states_msg_err_ ? c.bad : c.good, "%s", states_msg_.c_str());
    ImGui::Separator();

    // Fixed-size cards with constant padding and border, so nothing moves on hover or click.
    const float pad = 12.0f, gap = 12.0f;
    float avail = ImGui::GetContentRegionAvail().x;
    int cols = std::max(1, int((avail + gap) / (220.0f + gap)));
    float card_w = std::floor((avail - gap * float(cols - 1)) / float(cols));
    float thumb_w = card_w - 2 * pad, thumb_h = std::floor(thumb_w * 2.0f / 3.0f);
    ImGui::PushFont(theme::font_small());
    float small_line = ImGui::GetTextLineHeight();
    constexpr float kSlotButtonH = 34.0f;
    ImGui::PopFont();
    float card_h = 2 * pad + thumb_h + 8 + ImGui::GetTextLineHeight() + 4 + small_line + 10 + 8 + kSlotButtonH;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
    int col = 0;
    for (int slot = states::kFirstSlot; slot <= states::kSlots; ++slot) {
        const states::SlotInfo& i = store_->info(slot);
        bool usable = i.exists && i.error.empty();
        ImGui::PushID(slot);
        bool current = slot == slot_;
        ImGui::PushStyleColor(ImGuiCol_Border, current ? c.highlight : ImVec4(1, 1, 1, 0.10f));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.5f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 6));
        ImGui::BeginChild("##card", ImVec2(card_w, card_h),
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && !current) {
            ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
            ImGui::GetWindowDrawList()->AddRect(wp, ImVec2(wp.x + ws.x, wp.y + ws.y), theme::u32(c.accent, 0.6f),
                                                ImGui::GetStyle().ChildRounding, 0, 1.5f);
        }
        ImVec2 tp = ImGui::GetCursorScreenPos();
        if (SDL_Texture* t = thumb_texture(slot)) {
            ImGui::Image((ImTextureID)(intptr_t)t, ImVec2(thumb_w, thumb_h));
        } else {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(tp, ImVec2(tp.x + thumb_w, tp.y + thumb_h), theme::u32(c.surface), 10);
            const char* empty = i.exists ? "Unusable" : "Empty";
            ImVec2 ts = ImGui::CalcTextSize(empty);
            dl->AddText(ImVec2(tp.x + (thumb_w - ts.x) / 2, tp.y + (thumb_h - ts.y) / 2), theme::u32(c.ink_muted), empty);
            ImGui::Dummy(ImVec2(thumb_w, thumb_h));
        }
        if (ImGui::IsItemClicked()) {
            slot_ = slot;
            config_.set_int("states.current_slot", slot_);
        }
        ImGui::Dummy(ImVec2(0, 2));
        std::string title = states::slot_name(slot) + (i.label.empty() ? "" : ": " + i.label);
        // One line, cut with "..." so the card never grows.
        while (title.size() > 4 && ImGui::CalcTextSize(title.c_str()).x > thumb_w) title.resize(title.size() - 4), title += "...";
        ImGui::TextColored(current ? c.highlight : c.ink, "%s", title.c_str());
        ImGui::PushFont(theme::font_small());
        if (usable) {
            char when[64];
            std::tm tm = local_time(i.saved_at);
            std::strftime(when, sizeof when, "%b %d, %H:%M", &tm);
            ImGui::TextColored(c.ink_muted, "%s  |  %s played", when, states::format_playtime(i.playtime_frames).c_str());
        } else {
            ImGui::TextColored(i.exists ? c.bad : c.ink_muted, "%s", i.exists ? i.error.c_str() : "No state saved yet");
        }
        ImGui::PopFont();
        // Save and Load share the row; the more menu is a square button on the right.
        ImGui::Dummy(ImVec2(0, 2));
        const float bh = kSlotButtonH, bgap = 6.0f;
        const float bw = std::floor((thumb_w - bh - 2 * bgap) / 2);
        ImGui::BeginDisabled(!game_);
        if (theme::button("Save", ImVec2(bw, bh), theme::ButtonKind::Outline)) save_slot(slot);
        ImGui::EndDisabled();
        ImGui::SameLine(0, bgap);
        ImGui::BeginDisabled(!usable);
        if (theme::button("Load", ImVec2(bw, bh), theme::ButtonKind::Primary)) {
            slot_ = slot;
            config_.set_int("states.current_slot", slot_);
            if (load_slot(slot)) set_menu_open(false);  // straight back into the game
        }
        ImGui::EndDisabled();
        ImGui::SameLine(0, bgap);
        if (theme::button("...##more", ImVec2(bh, bh), theme::ButtonKind::Outline)) ImGui::OpenPopup("more");
        if (ImGui::BeginPopup("more")) {
            if (ImGui::MenuItem("Rename...", nullptr, false, usable)) {
                rename_slot_ = slot;
                std::snprintf(rename_buf_, sizeof rename_buf_, "%s", i.label.c_str());
            }
            if (ImGui::MenuItem("Export...", nullptr, false, i.exists)) {
                file_slot_ = slot;
                static const SDL_DialogFileFilter f[] = {{"SuperSaga save state", "ssst"}};
                std::string def = (rom_.path.parent_path() / (std::string(game::kFileSafeTitle) + " " + states::slot_name(slot) + ".ssst")).string();
                SDL_ShowSaveFileDialog(
                    [](void* self, const char* const* files, int) {
                        auto* app = static_cast<App*>(self);
                        std::lock_guard<std::mutex> lock(app->pick_mutex_);
                        if (files && files[0]) app->picked_state_export_ = files[0];
                    },
                    this, window_, f, 1, def.c_str());
            }
            if (ImGui::MenuItem("Import into this slot...")) {
                file_slot_ = slot;
                static const SDL_DialogFileFilter f[] = {{"SuperSaga save state", "ssst"}, {"All files", "*"}};
                SDL_ShowOpenFileDialog(
                    [](void* self, const char* const* files, int) {
                        auto* app = static_cast<App*>(self);
                        std::lock_guard<std::mutex> lock(app->pick_mutex_);
                        if (files && files[0]) app->picked_state_import_ = files[0];
                    },
                    this, window_, f, 2, nullptr, false);
            }
            if (ImGui::MenuItem("Delete...", nullptr, false, i.exists)) delete_slot_ = slot;
            ImGui::EndPopup();
        }
        ImGui::EndChild();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor();
        ImGui::PopID();
        if (++col < cols) ImGui::SameLine();
        else col = 0;
    }
    ImGui::PopStyleVar();  // card gap

    if (rename_slot_ >= 0) { ImGui::OpenPopup("Rename state"); }
    if (ImGui::BeginPopupModal("Rename state", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Label for %s:", states::slot_name(rename_slot_).c_str());
        ImGui::SetNextItemWidth(320);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        bool enter = ImGui::InputText("##label", rename_buf_, sizeof rename_buf_, ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::Button("Save") || enter) {
            std::string err;
            if (!store_->rename(rename_slot_, rename_buf_, &err)) { states_msg_ = err; states_msg_err_ = true; }
            rename_slot_ = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) { rename_slot_ = -1; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    if (delete_slot_ >= 0) { ImGui::OpenPopup("Delete state?"); }
    if (ImGui::BeginPopupModal("Delete state?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Delete %s? It is moved to the Trash.", states::slot_name(delete_slot_).c_str());
        if (ImGui::Button("Delete")) {
            std::string err;
            bool ok = store_->remove(delete_slot_, &err);
            states_msg_ = ok ? states::slot_name(delete_slot_) + " moved to the Trash." : err;
            states_msg_err_ = !ok;
            delete_slot_ = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) { delete_slot_ = -1; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
}

void App::save_message(std::string text, bool error) {
    save_msg_ = std::move(text);
    save_msg_error_ = error;
}

fs::path App::current_save_path() { return rom_.ok ? save_path_for(config_, rom_) : fs::path(); }

void App::prepare_import(const fs::path& file) {
    std::vector<std::uint8_t> bytes;
    if (!read_file(file, bytes)) {
        save_message("Could not read " + file.string(), true);
        return;
    }
    NormalizedSave n = normalize_save(bytes, file.filename().string());
    if (!n.ok) {
        save_message(n.error, true);
        return;
    }
    pending_save_ = PendingSave{std::move(n.data), file.filename().string(), n.format, n.warning};
    open_save_confirm_ = true;
}

void App::apply_pending_save() {
    if (!pending_save_ || !rom_.ok) return;
    fs::path save = current_save_path();
    bool was_playing = game_ != nullptr;
    if (game_) game_->flush_save();  // so the backup has the latest progress
    fs::path backup = backup_save(save, save.parent_path() / "backups");
    if (!write_file_atomic(save, pending_save_->data)) {
        save_message("Could not write the save file " + save.string(), true);
        pending_save_.reset();
        return;
    }
    // The running game would overwrite the imported save on exit, so restart it from the new save.
    game_.reset();
    if (was_playing) {
        game_ = make_gba(true);
        game_paused_ = screen_ != Screen::Game;
    }
    save_message("Imported " + pending_save_->source + (backup.empty() ? "." : ". The previous save was backed up.") +
                     (was_playing ? " The game was restarted." : ""),
                 false);
    pending_save_.reset();
}

void App::export_save(const fs::path& dst) {
    fs::path save = current_save_path();
    if (game_) game_->flush_save();
    std::vector<std::uint8_t> data;
    std::error_code ec;
    if (save.empty() || !fs::exists(save, ec) || !read_file(save, data)) {
        save_message("There is no save to export yet. Play the game first.", true);
        return;
    }
    data.resize(kSramSize, 0xFF);
    fs::path out = dst;
    if (out.extension().empty()) out += ".sav";
    if (!write_file_atomic(out, data)) {
        save_message("Could not write " + out.string(), true);
        return;
    }
    save_message("Exported to " + out.string() + " (raw " + std::to_string(game::kSaveSize / 1024) + " KiB, works in mGBA, VBA-M and RetroArch).", false);
}

void App::draw_save_section() {
    const auto& c = theme::palette();
    theme::section("Saves");
    fs::path saves = config_.pref_dir() / "saves";
    ImGui::TextWrapped("%s", saves.string().c_str());
    if (ImGui::Button("Show save folder")) {
        std::error_code ec;
        fs::create_directories(saves, ec);
        SDL_OpenURL(file_url(saves).c_str());
    }
    ImGui::BeginDisabled(!rom_.ok);
    same_line_if_fits("Import save...");
    if (ImGui::Button("Import save...")) {
        static const SDL_DialogFileFilter filters[] = {{"Game saves", "sav;srm;sa1;dsv"}, {"All files", "*"}};
        SDL_ShowOpenFileDialog(
            [](void* self, const char* const* files, int) {
                auto* app = static_cast<App*>(self);
                std::lock_guard<std::mutex> lock(app->pick_mutex_);
                if (files && files[0]) app->picked_import_ = files[0];
            },
            this, window_, filters, 2, nullptr, false);
    }
    same_line_if_fits("Export save...");
    if (ImGui::Button("Export save...")) {
        static const SDL_DialogFileFilter filters[] = {{"Game save", "sav"}};
        std::string def = (fs::path(rom_.path).parent_path() / (std::string(game::kFileSafeTitle) + ".sav")).string();
        SDL_ShowSaveFileDialog(
            [](void* self, const char* const* files, int) {
                auto* app = static_cast<App*>(self);
                std::lock_guard<std::mutex> lock(app->pick_mutex_);
                if (files && files[0]) app->picked_export_ = files[0];
            },
            this, window_, filters, 1, def.c_str());
    }
    same_line_if_fits("Restore backup...");
    if (ImGui::Button("Restore backup...")) open_backup_list_ = true;
    ImGui::EndDisabled();
    ImGui::TextDisabled("Imports .sav / .srm battery saves from mGBA, VBA-M, RetroArch and others.");
    if (!save_msg_.empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(save_msg_error_ ? c.bad : c.good, "%s", save_msg_.c_str());
        ImGui::PopTextWrapPos();
    }

    if (open_save_confirm_) {
        ImGui::OpenPopup("Replace your save?");
        open_save_confirm_ = false;
    }
    if (ImGui::BeginPopupModal("Replace your save?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (pending_save_) {
            ImGui::Text("Use %s as your save?", pending_save_->source.c_str());
            ImGui::TextDisabled("%s", pending_save_->format.c_str());
            ImGui::TextUnformatted("Your current save is backed up first (the last 10 are kept).");
            if (game_)
                ImGui::TextColored(c.highlight, "Importing restarts the game; unsaved progress is lost.");
            if (!pending_save_->warning.empty()) ImGui::TextColored(c.highlight, "%s", pending_save_->warning.c_str());
        }
        ImGui::Dummy(ImVec2(0, 4));
        if (ImGui::Button("Replace save")) {
            apply_pending_save();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            pending_save_.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (open_backup_list_) {
        ImGui::OpenPopup("Restore a backup");
        open_backup_list_ = false;
    }
    if (ImGui::BeginPopupModal("Restore a backup", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        fs::path save = current_save_path();
        auto backups = list_backups(save.parent_path() / "backups", save.stem().string());
        if (backups.empty()) ImGui::TextUnformatted("No backups yet. One is made every time a save is replaced.");
        for (const auto& b : backups) {
            // <sha1>-YYYYmmdd-HHMMSS.sav -> "YYYY-mm-dd HH:MM:SS"
            std::string n = b.stem().string();
            std::string stamp = n.size() >= 15 ? n.substr(n.size() - 15) : n;
            std::string label = stamp.size() == 15 ? stamp.substr(0, 4) + "-" + stamp.substr(4, 2) + "-" +
                                                         stamp.substr(6, 2) + " " + stamp.substr(9, 2) + ":" +
                                                         stamp.substr(11, 2) + ":" + stamp.substr(13, 2)
                                                   : n;
            if (ImGui::Button(label.c_str(), ImVec2(320, 0))) {
                std::vector<std::uint8_t> data;
                if (read_file(b, data)) {
                    data.resize(kSramSize, 0xFF);
                    pending_save_ = PendingSave{std::move(data), "the backup from " + label, "backup", ""};
                    open_save_confirm_ = true;
                }
                ImGui::CloseCurrentPopup();
            }
        }
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

bool App::cheats_allowed() const { return !(achievements_ && achievements_->hardcore()); }

// Rewind keeps working in softcore, but RetroAchievements hardcore turns it off like save states.
bool App::rewind_allowed() const { return cheats_allowed() && config_.get_bool("rewind.enabled", true); }

void App::update_speed() {
    if (game_.get() != rewind_owner_) {  // a new or restarted game: its history starts over
        rewind_.clear();
        rewind_owner_ = game_.get();
        rewind_tick_ = 0;
    }
    const bool playing = screen_ == Screen::Game && game_ && !opt_.test_pattern && !options_menu_open();
    const bool rewind_key = playing && controls_->hotkey_held(input::Hotkey::Rewind);
    const bool was_rewinding = rewinding_;
    rewinding_ = rewind_key && rewind_allowed();
    if (rewind_key && !rewind_key_was_held_ && !rewinding_) {
        presenter_->show_message(cheats_allowed() ? "Rewind is turned off (F1 > Controls)"
                                                  : "Rewind is off in RetroAchievements hardcore mode",
                                 3.0);
    }
    rewind_key_was_held_ = rewind_key;
    if (rewinding_ && !was_rewinding && audio_) SDL_ClearAudioStream(audio_);  // stop the sound at once

    const bool ff = playing && !rewinding_ &&
                    (controls_->hotkey_held(input::Hotkey::FastForward) || controls_->fast_forward_latched());
    const int speed = ff ? (config_.get_int("speed.fast_forward", 2) >= 4 ? 4 : 2) : 1;
    if (speed != ff_speed_) {
        // Back to normal speed: drop the fast audio still queued so the sound catches up at once.
        if (speed == 1 && audio_) SDL_ClearAudioStream(audio_);
        audio_fill_avg_ = -1;
        ff_speed_ = speed;
    }
    if (rewinding_) presenter_->show_message(rewind_.depth() ? "<< Rewind" : "<< Rewind (no older history)", 0.2);
    else if (ff_speed_ > 1) presenter_->show_message(">> Fast forward " + std::to_string(ff_speed_) + "x", 0.2);
}

void App::draw_cheats_tab(Config& config) {
    theme::section("Mario and Luigi");
    const bool allowed = cheats_allowed();
    ImGui::BeginDisabled(!allowed);
    struct Toggle { const char* label; const char* key; const char* help; };
    static constexpr Toggle kToggles[] = {
        {"Infinite HP", "cheats.hp", "Both brothers' HP stays full, so damage never drains it."},
        {"Infinite BP", "cheats.bp", "Bros. Points refill after every Bros. Attack."},
        {"Max coins", "cheats.coins", "Your coins stay at 9999."},
    };
    for (const Toggle& t : kToggles) {
        bool on = config.get_bool(t.key, false);
        if (theme::toggle(t.label, &on)) config.set_bool(t.key, on);
        ImGui::PushFont(theme::font_small());
        ImGui::TextDisabled("%s", t.help);
        ImGui::PopFont();
    }
    ImGui::EndDisabled();
    ImGui::Dummy(ImVec2(0, 4.0f));
    ImGui::PushFont(theme::font_small());
    if (!allowed) ImGui::TextDisabled("Cheats are off while RetroAchievements hardcore mode is on.");
    else ImGui::TextDisabled("HP and BP refill up to each brother's current maximum. Cheats stay on until you turn them off.");
    ImGui::PopFont();
}

void App::draw_game_tab(Config& config) {
    (void)config;
    const auto& c = theme::palette();
    theme::section("Game ROM");
    if (rom_.ok) {
        if (rom_.rom.header_only)
            ImGui::TextColored(c.highlight, "%s, header match (fingerprint not pinned yet)", game::kTitleWithRegion);
        else
            ImGui::TextColored(c.good, "%s, verified", game::kTitleWithRegion);
        ImGui::PushFont(theme::font_small());
        ImGui::TextColored(c.ink_muted, "%s [%s] rev %u", rom_.rom.header.title.c_str(), rom_.rom.header.game_code.c_str(),
                           unsigned(rom_.rom.header.version));
        ImGui::TextColored(c.ink_muted, "SHA-1 %s", rom_.rom.sha1.c_str());
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(c.ink_muted, "%s", path_utf8(rom_.path).c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
    } else {
        ImGui::TextColored(c.bad, "No ROM chosen");
    }
    if (ImGui::Button("Change ROM...")) open_rom_dialog();
    if (!rom_error_.empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(c.bad, "%s", rom_error_.c_str());
        ImGui::PopTextWrapPos();
    }

    theme::section("Startup");
    bool autoload = config_.get_bool("states.autoload", false);
    if (theme::toggle("Auto-load save state on launch", &autoload)) config_.set_bool("states.autoload", autoload);
    ImGui::TextDisabled("Opening SuperSaga goes straight back to where you left off (your most recent save state).");
    ImGui::TextDisabled("Hold Shift while opening SuperSaga to see this menu instead.");

    draw_save_section();

    theme::section("BIOS (optional)");
    std::string bios = config_.get_string("bios.path");
    if (bios != bios_checked_path_) {  // verify once per change, not every frame
        bios_checked_path_ = bios;
        bios_status_.clear();
        if (!bios.empty() && !load_bios(&bios_status_) && bios_status_.empty())
            bios_status_ = "The BIOS file could not be read; using the built-in BIOS.";
    }
    if (bios.empty()) {
        ImGui::TextWrapped("Built-in BIOS. No file is needed.");
    } else {
        ImGui::TextWrapped("%s", bios.c_str());
        if (bios_status_.empty())
            ImGui::TextColored(c.good, "Retail GBA BIOS verified: the Game Boy logo plays when the game starts, "
                                       "and the game runs on this BIOS.");
        else
            ImGui::TextColored(c.bad, "%s", bios_status_.c_str());
    }
    if (ImGui::Button(bios.empty() ? "Use a BIOS file..." : "Use the built-in BIOS")) {
        if (bios.empty()) open_bios_dialog();
        else config_.erase("bios.path");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Takes effect the next time the game starts.");

    theme::section("Shortcut");
    ImGui::TextWrapped("A shortcut starts the game directly, skipping the launcher.");
    draw_shortcut_controls(false);
}

void App::draw_shortcut_controls(bool big) {
    const auto& c = theme::palette();
#if defined(__APPLE__)
    const char* apps_name = "Applications";
#elif defined(_WIN32)
    const char* apps_name = "Start menu";
#else
    const char* apps_name = "applications menu";
#endif
    auto report = [&](const ShortcutResult& r) {
        info_ = r.message;
        info_until_ = ImGui::GetTime() + 10;
        if (r.needs_confirm) {
            confirm_shortcut_ = true;
        }
    };
    for (ShortcutPlace place : {ShortcutPlace::Applications, ShortcutPlace::Desktop}) {
        ImGui::PushID(int(place));
        std::string where = place == ShortcutPlace::Desktop ? "Desktop" : apps_name;
        ShortcutState st = shortcut_state(place);
        std::string label = (st == ShortcutState::Ours ? "Update " : "Add to ") + where;
        if (st == ShortcutState::Ours) label = "Update " + where + " shortcut";
        // In the popup the buttons fill the row; Remove sits at the end of it.
        const float row = ImGui::GetContentRegionAvail().x, remove_w = 110.0f;
        const float main_w = st == ShortcutState::Ours ? row - remove_w - ImGui::GetStyle().ItemSpacing.x : row;
        bool pressed = big ? theme::big_button(label.c_str(), ImVec2(main_w, 46)) : ImGui::Button(label.c_str());
        if (pressed) {
            pending_shortcut_place_ = place;
            report(create_shortcut(place));
        }
        if (st == ShortcutState::Ours) {
            ImGui::SameLine();
            bool remove = big ? theme::big_button("Remove", ImVec2(remove_w, 46), false) : ImGui::Button("Remove shortcut");
            if (remove) report(remove_shortcut(place));
        }
        ImGui::PopID();
    }
    if (!info_.empty() && ImGui::GetTime() < info_until_) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(c.highlight, "%s", info_.c_str());
        ImGui::PopTextWrapPos();
    }
    if (confirm_shortcut_) {
        ImGui::OpenPopup("Replace existing item?");
        confirm_shortcut_ = false;
    }
    if (ImGui::BeginPopupModal("Replace existing item?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("%s", shortcut_path(pending_shortcut_place_).string().c_str());
        ImGui::TextUnformatted("already exists and was not made by SuperSaga.");
        ImGui::TextUnformatted("Replace it? The existing item is moved to the Trash.");
        if (ImGui::Button("Replace")) {
            report(create_shortcut(pending_shortcut_place_, true));
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Keep it")) {
            info_ = "Left the existing item alone.";
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void App::draw_ui() {
    if (screen_ != Screen::Game) {
        ImDrawList* bg = ImGui::GetBackgroundDrawList();
        ImVec2 ds = ImGui::GetIO().DisplaySize;
        bool have_frame = attract_ || (game_ && game_paused_);
        if (have_frame) {
            // Tint the game backdrop toward the theme's surface so the card reads clearly.
            bg->AddRectFilledMultiColor(ImVec2(0, 0), ds, theme::u32(theme::palette().surface, 0.45f),
                                        theme::u32(theme::palette().surface, 0.45f),
                                        theme::u32(theme::palette().surface, 0.8f),
                                        theme::u32(theme::palette().surface, 0.8f));
        } else {
            theme::draw_backdrop(bg, ImVec2(0, 0), ds);
        }
    }
    switch (screen_) {
        case Screen::FirstRun: draw_first_run(); break;
        case Screen::Launcher: if (!options_menu_open()) draw_launcher(); break;
        case Screen::Game: break;
    }
    if (options_menu_open()) draw_options_menu(config_);
    toast::draw();
}

int App::run() {
    for (size_t p = 0; p < opt_.input_script.size();) {
        size_t comma = opt_.input_script.find(',', p);
        std::string item = opt_.input_script.substr(p, comma == std::string::npos ? std::string::npos : comma - p);
        size_t colon = item.find(':');
        if (colon != std::string::npos)
            script_.emplace_back(std::strtol(item.c_str(), nullptr, 10),
                                 std::uint16_t(std::strtol(item.c_str() + colon + 1, nullptr, 16)));
        if (comma == std::string::npos) break;
        p = comma + 1;
    }
    for (size_t p = 0; p < opt_.state_script.size();) {
        size_t comma = opt_.state_script.find(',', p);
        std::string item = opt_.state_script.substr(p, comma == std::string::npos ? std::string::npos : comma - p);
        size_t colon = item.find(':');
        if (colon != std::string::npos && colon + 2 <= item.size())
            state_script_.push_back({std::strtol(item.c_str(), nullptr, 10), item[colon + 1],
                                     int(std::strtol(item.c_str() + colon + 2, nullptr, 10))});
        if (comma == std::string::npos) break;
        p = comma + 1;
    }
    if (!init_window()) {
        std::fprintf(stderr, "supersaga: could not create window: %s\n", SDL_GetError());
        return 1;
    }

    if (!opt_.rom.empty()) {
        VerifiedRom v = verify_rom(opt_.rom);
        if (v.ok) {
            rom_ = std::move(v);
            start_game();
        } else {
            rom_error_ = v.error;
        }
    } else {
        startup();
    }
    if (opt_.test_pattern && screen_ != Screen::Game) start_game();
    if (screen_ != Screen::Game) {
        screen_ = rom_.ok ? Screen::Launcher : Screen::FirstRun;
        start_attract();
    }
    if (opt_.open_settings && screen_ != Screen::FirstRun) {
        if (!opt_.open_tab.empty()) {
            std::string tab = opt_.open_tab;
            tab[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(tab[0])));
            options_select_tab(tab.c_str());
        }
        set_menu_open(true);
    }
    if (!opt_.select_rom.empty()) {
        std::lock_guard<std::mutex> lock(pick_mutex_);
        picked_rom_ = path_utf8(opt_.select_rom);
    }

    Uint64 next = SDL_GetTicksNS();
    Uint64 last = next, acc = 0;
    double lock_acc = 0;
    const Uint64 frame_ns = Uint64(kFrameSeconds * 1e9);
    while (running_) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) handle_event(e);
        update_ui::tick(*updater_, config_);
        {
            std::lock_guard<std::mutex> lock(pick_mutex_);
            if (picked_rom_) { accept_rom(utf8_path(*picked_rom_)); picked_rom_.reset(); }
            if (picked_import_) {
                prepare_import(utf8_path(*picked_import_));
                picked_import_.reset();
                if (pending_save_) options_menu_set_open(true);
            }
            if (picked_export_) { export_save(utf8_path(*picked_export_)); picked_export_.reset(); }
            if (picked_state_export_ && store_) {
                std::string err;
                bool ok = store_->export_slot(file_slot_, utf8_path(*picked_state_export_), &err);
                states_msg_ = ok ? "Exported " + states::slot_name(file_slot_) + "." : err;
                states_msg_err_ = !ok;
                picked_state_export_.reset();
            }
            if (picked_state_import_ && store_) {
                std::string err;
                bool ok = store_->import_into(file_slot_, utf8_path(*picked_state_import_), &err);
                states_msg_ = ok ? "Imported into " + states::slot_name(file_slot_) + "." : err;
                states_msg_err_ = !ok;
                picked_state_import_.reset();
            }
            if (picked_bios_) {
                config_.set_string("bios.path", *picked_bios_);
                std::string err;
                if (!load_bios(&err)) {
                    config_.erase("bios.path");
                    info_ = err.empty() ? "Could not read that BIOS file." : err;
                } else {
                    info_ = "BIOS set. It is used the next time the game starts.";
                }
                info_until_ = ImGui::GetTime() + 8;
                picked_bios_.reset();
            }
        }

        update_speed();

        // How many emulated frames this refresh.
        int steps = 1;
        if (lockstep_) {
            // One frame per refresh, but keep real time: if vsync stops blocking (window hidden or
            // occluded) or refreshes are missed, skip or catch up instead of changing game speed.
            const double refresh_ns = 1e9 / refresh_hz_;
            Uint64 now = SDL_GetTicksNS();
            lock_acc += double(now - last) - refresh_ns;
            last = now;
            if (lock_acc < -2 * refresh_ns) {  // running ahead
                steps = 0;
                lock_acc += refresh_ns;
                SDL_DelayNS(Uint64(refresh_ns * 0.5));
            } else if (lock_acc > 2 * refresh_ns) {  // fell behind
                steps = 2;
                lock_acc -= refresh_ns;
            }
            lock_acc = std::clamp(lock_acc, -4 * refresh_ns, 4 * refresh_ns);
        } else if (vsync_) {
            Uint64 now = SDL_GetTicksNS();
            acc += now - last;
            last = now;
            acc = std::min<Uint64>(acc, frame_ns * 4);
            steps = 0;
            while (acc >= frame_ns && steps < 2) {
                acc -= frame_ns;
                ++steps;
            }
        }
        steps *= ff_speed_;
        bool reached_end = false;
        for (int i = 0; i < steps; ++i) {
            update_emulation();
            ++frames_;
            if (opt_.frames >= 0 && frames_ >= opt_.frames) { reached_end = true; break; }
        }
        update_audio_rate();
        if (achievements_) achievements_->idle(game_.get());  // network replies, pings, toasts

        bool menu = options_menu_open();
        if (menu_was_open_ && !menu) {  // apply what changed in the settings menu
            presenter_->reload();
            controls_->reload();
            if (audio_) SDL_ClearAudioStream(audio_);
            if (game_) game_->set_wide_margin(game::kWidescreen ? display::wide_margin(presenter_->settings().aspect) : 0);
        }
        menu_was_open_ = menu;

        bool toast = toast::active();
        if (screen_ == Screen::Game && !menu) {
            if (!opt_.screenshot.empty() && reached_end && !toast) presenter_->capture_next(opt_.screenshot.string());
            if (steps == 0) {
                // No new emulated frame this refresh: show the same image again without re-blending.
                presenter_->redraw(controls_->remap_prompt());
            } else if (opt_.test_pattern || !game_) {
                presenter_->draw(pattern_.data(), gba::kScreenWidth, controls_->remap_prompt());
            } else {
                presenter_->draw(game_->wide_framebuffer(), game_->wide_width(), controls_->remap_prompt());
            }
            if (toast) {  // small save-state notice with the slot's thumbnail
                ImGui_ImplSDLRenderer3_NewFrame();
                ImGui_ImplSDL3_NewFrame();
                ImGui::NewFrame();
                toast::draw();
                ImGui::Render();
                ImGuiIO& io = ImGui::GetIO();
                SDL_SetRenderScale(renderer_, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
                ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer_);
                SDL_SetRenderScale(renderer_, 1, 1);
                if (!opt_.screenshot.empty() && reached_end) capture_window(opt_.screenshot.string());
            }
            SDL_RenderPresent(renderer_);
            std::string msg = controls_->take_message();
            if (!msg.empty()) presenter_->show_message(msg);
        } else {
            ImGui_ImplSDLRenderer3_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
            draw_ui();
            ImGui::Render();
            ImGuiIO& io = ImGui::GetIO();
            SDL_SetRenderScale(renderer_, 1, 1);
            if (screen_ == Screen::Game) {
                // Paused game under the settings menu.
                if (opt_.test_pattern || !game_) presenter_->draw(pattern_.data(), gba::kScreenWidth, controls_->remap_prompt());
                else presenter_->draw(game_->wide_framebuffer(), game_->wide_width(), controls_->remap_prompt());
            } else {
                SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
                SDL_RenderClear(renderer_);
                draw_backdrop();
            }
            SDL_SetRenderScale(renderer_, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
            ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer_);
            if (!opt_.screenshot.empty() && reached_end) capture_window(opt_.screenshot.string());
            SDL_RenderPresent(renderer_);
        }

        if (reached_end) running_ = false;
        if (!vsync_) {  // fallback: sleep-based pacing
            next += frame_ns;
            Uint64 now = SDL_GetTicksNS();
            if (next > now) SDL_DelayPrecise(next - now);
            else if (now - next > frame_ns * 5) next = now;
        }
    }
    if (!opt_.dump_frame.empty() && game_) write_bmp(opt_.dump_frame.string(), game_->framebuffer());
    shutdown();
    updater_->relaunch();  // only does something after an update was installed
    return 0;
}

}  // namespace

int run_app(const AppOptions& options) {
    auto app = std::make_unique<App>(options);  // large object: keep it off the stack
    return app->run();
}

}  // namespace supersaga
