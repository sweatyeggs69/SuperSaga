#include "app/achievements.h"

#include <vector>
#include <imgui.h>
#include <rc_client.h>
#include <rc_consoles.h>
#include <rc_error.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <system_error>
#include <unordered_map>

#include "app/config.h"
#include "app/credentials.h"
#include "app/http.h"
#include "app/theme.h"
#include "app/toast.h"
#include "gba/gba.h"

// The one stb_image implementation in the app (also used by app.cpp for the launcher logo).
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include <stb_image.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

extern "C" {
#include "rhash/md5.h"
}

#ifndef SUPERSAGA_VERSION
#define SUPERSAGA_VERSION "0.0.0"
#endif

namespace supersaga {
namespace fs = std::filesystem;

std::string ra_md5_hex(const std::uint8_t* data, std::size_t size) {
    md5_state_t st;
    md5_init(&st);
    while (size > 0) {  // md5_append takes an int length
        int n = int(std::min<std::size_t>(size, 1u << 30));
        md5_append(&st, data, n);
        data += n;
        size -= std::size_t(n);
    }
    md5_byte_t digest[16];
    md5_finish(&st, digest);
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (md5_byte_t b : digest) {
        out += hex[b >> 4];
        out += hex[b & 15];
    }
    return out;
}

std::uint32_t ra_read_memory(const gba::Gba* gba, std::uint32_t address, std::uint8_t* buffer,
                             std::uint32_t num_bytes) {
    if (!gba) {
        std::uint32_t n = 0;
        for (; n < num_bytes && address + n < 0x58000; ++n) buffer[n] = 0;
        return n;
    }
    // rcheevos GBA map: 0x00000 IWRAM (32 KiB), 0x08000 EWRAM (256 KiB), 0x48000 cartridge SRAM
    // (64 KiB window; this game has an 8 KiB EEPROM, the rest reads as 0).
    auto iw = gba->iwram(), ew = gba->ewram(), sr = gba->sram();
    for (std::uint32_t i = 0; i < num_bytes; ++i) {
        std::uint32_t a = address + i;
        if (a < 0x8000) buffer[i] = a < iw.size() ? iw[a] : 0;
        else if (a < 0x48000) buffer[i] = (a - 0x8000) < ew.size() ? ew[a - 0x8000] : 0;
        else if (a < 0x58000) buffer[i] = (a - 0x48000) < sr.size() ? sr[a - 0x48000] : 0;
        else return i;
    }
    return num_bytes;
}

namespace {
constexpr int kAchievementToastMs = 5000;
// The server adds this pseudo-achievement when it does not accept hardcore unlocks from the client.
constexpr std::uint32_t kWarningAchievementId = 101000001;

struct Badge {
    enum State { Loading, Ready, Failed } state = Loading;
    std::vector<std::uint8_t> rgba;
    int w = 0, h = 0;
    SDL_Texture* tex = nullptr;
};

struct PendingToast {
    std::string title, body, badge_url;
    int duration_ms;
    toast::Style style;
    Uint64 deadline;  // show without the badge if it has not arrived by then
};

std::string badge_file_name(const std::string& url) {
    std::string name = url.substr(url.find_last_of('/') + 1);
    std::string out;
    for (char c : name)
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-') out += c;
    return out.empty() ? "badge.png" : out;
}

}  // namespace

struct Achievements::Impl {
    Config& config;
    SDL_Renderer* renderer;
    rc_client_t* client = nullptr;
    // Cleared on destruction so network replies that arrive later are dropped.
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
    const gba::Gba* gba = nullptr;
    std::function<void()> restart;

    enum class Login { SignedOut, SigningIn, SignedIn } login = Login::SignedOut;
    std::string login_message;  // shown on the tab (errors, "signed out")
    bool login_message_error = false;
    char user_buf[128] = {};
    char pass_buf[256] = {};

    std::string game_hash;       // MD5 of the running game's ROM, empty when no session
    bool game_requested = false;  // rc_client_begin_load_game called for game_hash
    std::string game_message;
    bool game_message_error = false;
    bool frame_ran = false;
    long session_frames = 0;
    bool session_loaded_state = false;
    bool confirm_hardcore = false;
    bool restart_pending = false;  // run by idle(), outside rc_client callbacks

    std::unordered_map<std::string, Badge> badges;
    std::vector<PendingToast> pending_toasts;
    std::map<std::uint32_t, Uint64> progress_toast_at;

    Impl(Config& c, SDL_Renderer* r) : config(c), renderer(r) {}

    bool enabled() const { return config.get_bool("ra.enabled", false); }  // off until the player opts in
    fs::path badge_dir() const { return config.pref_dir() / "retroachievements" / "badges"; }

    // ---- rc_client glue --------------------------------------------------------------------

    static Impl* self(const rc_client_t* c) { return static_cast<Impl*>(rc_client_get_userdata(c)); }

    static std::uint32_t RC_CCONV read_memory(std::uint32_t address, std::uint8_t* buffer, std::uint32_t num_bytes,
                                              rc_client_t* c) {
        Impl* s = self(c);
        if (!s) return 0;
        return ra_read_memory(s->gba, address, buffer, num_bytes);
    }

    static void RC_CCONV server_call(const rc_api_request_t* request, rc_client_server_callback_t callback,
                                     void* callback_data, rc_client_t* c) {
        Impl* s = self(c);
        std::shared_ptr<bool> alive = s ? s->alive : std::make_shared<bool>(false);
        http::request(request->url, request->post_data ? request->post_data : "",
                      request->content_type ? request->content_type : "",
                      [callback, callback_data, alive](http::Response r) {
                          if (!*alive) return;
                          rc_api_server_response_t sr{};
                          sr.body = r.body.c_str();
                          sr.body_length = r.body.size();
                          sr.http_status_code = r.status ? r.status : RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
                          if (!r.error.empty() && r.body.empty()) {
                              sr.body = r.error.c_str();
                              sr.body_length = r.error.size();
                          }
                          callback(&sr, callback_data);
                      });
    }

    static void RC_CCONV log_message(const char* message, const rc_client_t*) {
        std::fprintf(stderr, "supersaga: retroachievements: %s\n", message);
    }

    void create_client() {
        client = rc_client_create(&Impl::read_memory, &Impl::server_call);
        rc_client_set_userdata(client, this);
        rc_client_set_event_handler(client, &Impl::on_event);
        rc_client_enable_logging(client, RC_CLIENT_LOG_LEVEL_WARN, &Impl::log_message);
        rc_client_set_allow_background_memory_reads(client, 0);
        rc_client_set_hardcore_enabled(client, config.get_bool("ra.hardcore", false) ? 1 : 0);
        if (const char* host = SDL_getenv("SUPERSAGA_RA_HOST"); host && *host)  // testing against a mock server
            rc_client_set_host(client, host);
        char clause[64] = {};
        rc_client_get_user_agent_clause(client, clause, sizeof(clause));
        http::set_user_agent(std::string("SuperSaga/") + SUPERSAGA_VERSION + " (" + SDL_GetPlatform() + ") " + clause);
    }

    // ---- sign-in ---------------------------------------------------------------------------

    void login_with_saved_token() {
        std::string user = config.get_string("ra.user");
        if (user.empty() || !enabled() || login != Login::SignedOut) return;
        std::optional<std::string> token = credentials::load(config.pref_dir(), user);
        if (!token || token->empty()) return;
        login = Login::SigningIn;
        rc_client_begin_login_with_token(client, user.c_str(), token->c_str(), &Impl::on_login, nullptr);
    }

    void login_with_password() {
        std::string user = user_buf;
        user.erase(0, user.find_first_not_of(" \t"));
        user.erase(user.find_last_not_of(" \t") + 1);
        if (user.empty() || !pass_buf[0]) {
            set_login_message("Enter your RetroAchievements username and password.", true);
            return;
        }
        login = Login::SigningIn;
        login_message.clear();
        rc_client_begin_login_with_password(client, user.c_str(), pass_buf, &Impl::on_login, nullptr);
        // The password is not kept anywhere once the request is built.
        volatile char* p = pass_buf;
        for (size_t i = 0; i < sizeof(pass_buf); ++i) p[i] = 0;
    }

    void set_login_message(std::string m, bool error) {
        login_message = std::move(m);
        login_message_error = error;
    }

    static void RC_CCONV on_login(int result, const char* error, rc_client_t* c, void*) {
        Impl* s = self(c);
        if (!s) return;
        s->login_done(result, error);
    }

    void login_done(int result, const char* error) {
        if (result == RC_OK) {
            const rc_client_user_t* u = rc_client_get_user_info(client);
            login = Login::SignedIn;
            login_message.clear();
            std::string user = u && u->username ? u->username : user_buf;
            if (u && u->token && !credentials::store(config.pref_dir(), user, u->token))
                set_login_message("Signed in, but the login could not be saved; you will be asked again next time.",
                                  true);
            config.set_string("ra.user", user);
            std::string name = u && u->display_name ? u->display_name : user;
            notify("Signed in to RetroAchievements", name + (u ? ", " + std::to_string(u->score) + " points" : ""),
                   u && u->avatar_url ? u->avatar_url : "", 3500, toast::Style::Success);
            maybe_load_game();
            return;
        }
        login = Login::SignedOut;
        std::string why = error && *error ? error : rc_error_str(result);
        if (result == RC_INVALID_CREDENTIALS || result == RC_EXPIRED_TOKEN || result == RC_ACCESS_DENIED) {
            // A rejected saved token is useless; forget it.
            credentials::erase(config.pref_dir(), config.get_string("ra.user"));
            set_login_message("Sign-in failed: " + why, true);
        } else {
            set_login_message("Could not sign in: " + why, true);
        }
    }

    void sign_out() {
        game_requested = false;
        rc_client_logout(client);  // also unloads the game
        credentials::erase(config.pref_dir(), config.get_string("ra.user"));
        login = Login::SignedOut;
        game_message.clear();
        set_login_message("Signed out.", false);
    }

    // ---- game ------------------------------------------------------------------------------

    void maybe_load_game() {
        if (game_hash.empty() || game_requested || !enabled() || login != Login::SignedIn) return;
        game_requested = true;
        game_message = "Loading achievements...";
        game_message_error = false;
        rc_client_begin_load_game(client, game_hash.c_str(), &Impl::on_game_loaded, nullptr);
    }

    static void RC_CCONV on_game_loaded(int result, const char* error, rc_client_t* c, void*) {
        if (Impl* s = self(c)) s->game_loaded(result, error);
    }

    void game_loaded(int result, const char* error) {
        if (result != RC_OK) {
            game_requested = result == RC_NO_GAME_LOADED;  // unknown ROM: do not ask again this session
            game_message_error = true;
            if (result == RC_NO_GAME_LOADED)
                game_message = "RetroAchievements does not know this ROM (MD5 " + game_hash + ").";
            else if (result == RC_ABORTED)
                game_message.clear();
            else
                game_message = std::string("Could not load achievements: ") + (error && *error ? error : rc_error_str(result));
            return;
        }
        game_message.clear();
        const rc_client_game_t* g = rc_client_get_game_info(client);
        rc_client_user_game_summary_t sum{};
        rc_client_get_user_game_summary(client, &sum);
        std::string body;
        if (sum.num_core_achievements == 0) {
            body = "This game has no achievements yet.";
        } else {
            body = std::to_string(sum.num_unlocked_achievements) + " of " + std::to_string(sum.num_core_achievements) +
                   " achievements unlocked";
            body += rc_client_get_hardcore_enabled(client) ? " (hardcore)" : "";
        }
        notify(g && g->title ? g->title : "RetroAchievements", body, g && g->badge_url ? g->badge_url : "", 4500,
               toast::Style::Info);
        // Hardcore requires a run without save states: a session that loaded one before the
        // achievements arrived (for example signing in mid-game) starts over.
        if (rc_client_get_hardcore_enabled(client) && session_loaded_state && restart) {
            notify("Hardcore mode", "Restarting the game so achievements can be earned.", "", 4000,
                   toast::Style::Warning);
            restart_pending = true;
        }
    }

    // ---- events ----------------------------------------------------------------------------

    static void RC_CCONV on_event(const rc_client_event_t* e, rc_client_t* c) {
        if (Impl* s = self(c)) s->event(*e);
    }

    void event(const rc_client_event_t& e) {
        const rc_client_achievement_t* a = e.achievement;
        const rc_client_leaderboard_t* lb = e.leaderboard;
        bool lb_notes = config.get_bool("ra.notify_leaderboards", true);
        switch (e.type) {
            case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
                if (a && config.get_bool("ra.notify_unlocks", true))
                    notify(a->title, std::to_string(a->points) + (a->points == 1 ? " point. " : " points. ") +
                                         (a->description ? a->description : ""),
                           a->badge_url ? a->badge_url : "", kAchievementToastMs, toast::Style::Achievement);
                break;
            case RC_CLIENT_EVENT_GAME_COMPLETED: {
                const rc_client_game_t* g = rc_client_get_game_info(client);
                bool hc = rc_client_get_hardcore_enabled(client);
                notify(std::string(hc ? "Mastered " : "Completed ") + (g && g->title ? g->title : "the game"),
                       "Every achievement unlocked. Congratulations!", g && g->badge_url ? g->badge_url : "", 7000,
                       toast::Style::Achievement);
                break;
            }
            case RC_CLIENT_EVENT_SUBSET_COMPLETED:
                if (e.subset)
                    notify(std::string("Completed ") + (e.subset->title ? e.subset->title : "a set"),
                           "Every achievement in this set unlocked.", e.subset->badge_url ? e.subset->badge_url : "",
                           6000, toast::Style::Achievement);
                break;
            case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_SHOW:
            case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_UPDATE:
                if (a && config.get_bool("ra.notify_progress", true)) {
                    Uint64 now = SDL_GetTicks();
                    Uint64& last = progress_toast_at[a->id];
                    if (last == 0 || now - last > 3000) {
                        last = now;
                        notify(a->title, std::string("Progress: ") + a->measured_progress,
                               a->badge_url ? a->badge_url : "", 2000, toast::Style::Info);
                    }
                }
                break;
            case RC_CLIENT_EVENT_LEADERBOARD_STARTED:
                if (lb && lb_notes) notify("Leaderboard attempt started", lb->title ? lb->title : "", "", 2500, toast::Style::Info);
                break;
            case RC_CLIENT_EVENT_LEADERBOARD_FAILED:
                if (lb && lb_notes) notify("Leaderboard attempt failed", lb->title ? lb->title : "", "", 2500, toast::Style::Info);
                break;
            case RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED:
                if (lb && lb_notes)
                    notify(std::string("Submitted ") + (lb->tracker_value ? lb->tracker_value : ""),
                           lb->title ? lb->title : "", "", 3500, toast::Style::Success);
                break;
            case RC_CLIENT_EVENT_LEADERBOARD_SCOREBOARD:
                if (lb && lb_notes && e.leaderboard_scoreboard)
                    notify(std::string("Rank ") + std::to_string(e.leaderboard_scoreboard->new_rank) + " of " +
                               std::to_string(e.leaderboard_scoreboard->num_entries),
                           lb->title ? lb->title : "", "", 3500, toast::Style::Info);
                break;
            case RC_CLIENT_EVENT_RESET:
                // Hardcore was switched on while playing: start over from power-on.
                restart_pending = true;
                break;
            case RC_CLIENT_EVENT_SERVER_ERROR:
                if (e.server_error)
                    notify("RetroAchievements error",
                           e.server_error->error_message ? e.server_error->error_message : "The server refused a request.",
                           "", 5000, toast::Style::Warning);
                break;
            case RC_CLIENT_EVENT_DISCONNECTED:
                notify("RetroAchievements is offline", "Unlocks are kept and sent when the connection is back.", "", 5000,
                       toast::Style::Warning);
                break;
            case RC_CLIENT_EVENT_RECONNECTED:
                notify("RetroAchievements is back online", "Pending unlocks were sent.", "", 3500, toast::Style::Success);
                break;
            default:  // challenge indicators and leaderboard trackers: not shown yet
                break;
        }
    }

    // ---- toasts and badges -----------------------------------------------------------------

    void notify(std::string title, std::string body, std::string badge_url, int duration_ms, toast::Style style) {
        if (badge_url.empty()) {
            toast::show(title, body, nullptr, 0, 0, duration_ms, style);
            return;
        }
        request_badge(badge_url);
        pending_toasts.push_back({std::move(title), std::move(body), std::move(badge_url), duration_ms, style,
                                  SDL_GetTicks() + 1500});
        flush_toasts();
    }

    void flush_toasts() {
        Uint64 now = SDL_GetTicks();
        for (size_t i = 0; i < pending_toasts.size();) {
            const PendingToast& t = pending_toasts[i];
            auto it = badges.find(t.badge_url);
            bool ready = it != badges.end() && it->second.state == Badge::Ready;
            bool give_up = it == badges.end() || it->second.state == Badge::Failed || now >= t.deadline;
            if (!ready && !give_up) { ++i; continue; }
            if (ready) toast::show(t.title, t.body, it->second.rgba.data(), it->second.w, it->second.h, t.duration_ms, t.style);
            else toast::show(t.title, t.body, nullptr, 0, 0, t.duration_ms, t.style);
            pending_toasts.erase(pending_toasts.begin() + std::ptrdiff_t(i));
        }
    }

    bool decode_badge(Badge& b, const std::string& png) {
        int w = 0, h = 0, n = 0;
        stbi_uc* px = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(png.data()), int(png.size()), &w, &h, &n, 4);
        if (!px) return false;
        b.rgba.assign(px, px + size_t(w) * size_t(h) * 4);
        b.w = w;
        b.h = h;
        b.state = Badge::Ready;
        stbi_image_free(px);
        return true;
    }

    void request_badge(const std::string& url) {
        if (url.empty() || badges.count(url)) return;
        Badge& b = badges[url];
        fs::path cached = badge_dir() / badge_file_name(url);
        {
            std::ifstream f(cached, std::ios::binary);
            std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            if (!data.empty() && decode_badge(b, data)) return;
        }
        http::request(url, "", "", [this, url, cached, alive = alive](http::Response r) {
            if (!*alive) return;
            auto it = badges.find(url);
            if (it == badges.end()) return;
            if (r.status == 200 && decode_badge(it->second, r.body)) {
                std::error_code ec;
                fs::create_directories(cached.parent_path(), ec);
                std::ofstream(cached, std::ios::binary).write(r.body.data(), std::streamsize(r.body.size()));
            } else {
                it->second.state = Badge::Failed;
            }
            flush_toasts();
        });
    }

    SDL_Texture* badge_texture(const std::string& url) {
        request_badge(url);
        auto it = badges.find(url);
        if (it == badges.end() || it->second.state != Badge::Ready || !renderer) return nullptr;
        Badge& b = it->second;
        if (!b.tex) {
            b.tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, b.w, b.h);
            if (!b.tex) return nullptr;
            SDL_SetTextureBlendMode(b.tex, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(b.tex, SDL_SCALEMODE_LINEAR);
            SDL_UpdateTexture(b.tex, nullptr, b.rgba.data(), b.w * 4);
        }
        return b.tex;
    }

    // ---- settings tab ----------------------------------------------------------------------

    void draw_tab();
    void draw_sign_in();
    void draw_game();
};

// ---- tab -------------------------------------------------------------------------------------

void Achievements::Impl::draw_sign_in() {
    const auto& c = theme::palette();
    ImGui::TextWrapped("Sign in with your RetroAchievements account to earn achievements while you play.");
    ImGui::Spacing();
    bool busy = login == Login::SigningIn;
    ImGui::BeginDisabled(busy);
    ImGui::SetNextItemWidth(320);
    bool go = ImGui::InputText("Username", user_buf, sizeof(user_buf), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SetNextItemWidth(320);
    go |= ImGui::InputText("Password", pass_buf, sizeof(pass_buf),
                           ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
    go |= ImGui::Button(busy ? "Signing in..." : "Sign in");
    ImGui::EndDisabled();
    if (go && !busy) login_with_password();
    ImGui::SameLine();
    if (ImGui::Button("Create an account")) SDL_OpenURL("https://retroachievements.org/createaccount.php");
    ImGui::PushFont(theme::font_small());
    ImGui::TextColored(c.ink_muted, "Your password goes only to retroachievements.org and is never saved. SuperSaga keeps");
    ImGui::TextColored(c.ink_muted, "just the sign-in token, in %s.", credentials::location());
    ImGui::PopFont();
}

void Achievements::Impl::draw_game() {
    const auto& c = theme::palette();
    if (game_hash.empty()) {
        ImGui::TextColored(c.ink_muted, "Start the game to see its achievements.");
        return;
    }
    if (!game_message.empty()) {
        ImGui::TextColored(game_message_error ? c.bad : c.ink_muted, "%s", game_message.c_str());
        if (game_message_error && !game_requested && login == Login::SignedIn && ImGui::Button("Try again"))
            maybe_load_game();
    }
    if (!rc_client_is_game_loaded(client)) return;
    // One summary line; the list below gets the rest of the height.
    const rc_client_game_t* g = rc_client_get_game_info(client);
    rc_client_user_game_summary_t sum{};
    rc_client_get_user_game_summary(client, &sum);
    if (sum.num_core_achievements == 0)
        ImGui::TextColored(c.ink_muted, "%s has no achievements yet.", g && g->title ? g->title : "This game");
    else
        ImGui::TextColored(c.ink_muted, "%s: %u of %u unlocked, %u of %u points", g && g->title ? g->title : "Game",
                           sum.num_unlocked_achievements, sum.num_core_achievements, sum.points_unlocked,
                           sum.points_core);

    rc_client_achievement_list_t* list = rc_client_create_achievement_list(
        client, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE, RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_PROGRESS);
    if (!list) return;
    if (ImGui::BeginChild("##ra_list", ImVec2(0, 0), ImGuiChildFlags_None)) {
        // Unlocked achievements first (recently unlocked, then the rest), then rcheevos' order.
        std::vector<uint32_t> order(list->num_buckets);
        for (uint32_t bi = 0; bi < list->num_buckets; ++bi) order[bi] = bi;
        auto rank = [&](uint32_t bi) {
            const uint8_t t = list->buckets[bi].bucket_type;
            return t == RC_CLIENT_ACHIEVEMENT_BUCKET_RECENTLY_UNLOCKED ? 0 : t == RC_CLIENT_ACHIEVEMENT_BUCKET_UNLOCKED ? 1 : 2;
        };
        std::stable_sort(order.begin(), order.end(), [&](uint32_t x, uint32_t y) { return rank(x) < rank(y); });
        for (uint32_t bi : order) {
            const rc_client_achievement_bucket_t& bucket = list->buckets[bi];
            bool only_warning = bucket.num_achievements == 1 && bucket.achievements[0]->id == kWarningAchievementId;
            if (only_warning) continue;
            ImGui::SeparatorText(bucket.label ? bucket.label : "");
            for (uint32_t i = 0; i < bucket.num_achievements; ++i) {
                const rc_client_achievement_t* a = bucket.achievements[i];
                if (a->id == kWarningAchievementId) continue;  // shown in Options instead
                ImGui::PushID(int(a->id));
                bool unlocked = a->unlocked != RC_CLIENT_ACHIEVEMENT_UNLOCKED_NONE;
                const char* url = unlocked ? a->badge_url : a->badge_locked_url;
                SDL_Texture* t = url ? badge_texture(url) : nullptr;
                if (t) ImGui::Image((ImTextureID)(intptr_t)t, ImVec2(44, 44));
                else ImGui::Dummy(ImVec2(44, 44));
                ImGui::SameLine();
                ImGui::BeginGroup();
                ImGui::TextColored(unlocked ? c.highlight : c.ink, "%s", a->title ? a->title : "");
                ImGui::SameLine();
                ImGui::TextColored(c.ink_muted, "%u pt%s", a->points, a->points == 1 ? "" : "s");
                ImGui::PushFont(theme::font_small());
                ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - 8);
                ImGui::TextColored(c.ink_muted, "%s", a->description ? a->description : "");
                ImGui::PopTextWrapPos();
                if (!unlocked && a->measured_progress[0]) {
                    ImGui::ProgressBar(a->measured_percent / 100.0f, ImVec2(200, 6), "");
                    ImGui::SameLine();
                    ImGui::TextColored(c.ink_muted, "%s", a->measured_progress);
                }
                ImGui::PopFont();
                ImGui::EndGroup();
                ImGui::PopID();
                ImGui::Spacing();
            }
        }
    }
    ImGui::EndChild();
    rc_client_destroy_achievement_list(list);
}

void Achievements::Impl::draw_tab() {
    const auto& c = theme::palette();
    if (!http::available()) {
        ImGui::TextColored(c.bad, "No network library was found, so RetroAchievements cannot connect.");
        ImGui::Spacing();
    }
    bool on = enabled();
    if (theme::toggle(login == Login::SignedIn && on ? "Achievements on" : "Earn achievements while playing", &on)) {
        config.set_bool("ra.enabled", on);
        if (on) {
            login_with_saved_token();
            maybe_load_game();
        } else {
            game_requested = false;
            rc_client_unload_game(client);
            game_message.clear();
        }
    }
    if (!on) return;

    if (login != Login::SignedIn) {
        if (!login_message.empty())
            ImGui::TextColored(login_message_error ? c.bad : c.ink_muted, "%s", login_message.c_str());
        if (login == Login::SignedOut && !config.get_string("ra.user").empty() &&
            credentials::load(config.pref_dir(), config.get_string("ra.user"))) {
            ImGui::Text("Saved account: %s", config.get_string("ra.user").c_str());
            ImGui::SameLine();
            if (ImGui::Button("Try again")) login_with_saved_token();
            ImGui::SameLine();
            if (ImGui::Button("Use another account")) sign_out();
            return;
        }
        if (!user_buf[0] && !config.get_string("ra.user").empty())
            std::snprintf(user_buf, sizeof(user_buf), "%s", config.get_string("ra.user").c_str());
        draw_sign_in();
        return;
    }

    // One line: account, then Options and Sign out on the right. The account text gets shorter, or
    // moves to its own line below, rather than running under the buttons.
    const rc_client_user_t* u = rc_client_get_user_info(client);
    const char* name = u && u->display_name ? u->display_name : "";
    char full[192], shorter[160];
    std::snprintf(full, sizeof(full), "%s, %u points (%u softcore)", name, u ? u->score : 0, u ? u->score_softcore : 0);
    std::snprintf(shorter, sizeof(shorter), "%s, %u points", name, u ? u->score : 0);
    const ImGuiStyle& st = ImGui::GetStyle();
    float buttons = ImGui::CalcTextSize("Options").x + ImGui::CalcTextSize("Sign out").x + st.FramePadding.x * 4 +
                    st.ItemSpacing.x;
    ImGui::SameLine(0, 24);
    float right = ImGui::GetWindowContentRegionMax().x - buttons;
    float room = right - ImGui::GetCursorPosX() - st.ItemSpacing.x;
    const char* account = ImGui::CalcTextSize(full).x <= room ? full : ImGui::CalcTextSize(shorter).x <= room ? shorter : nullptr;
    if (account) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(c.ink_muted, "%s", account);
        ImGui::SameLine(std::max(ImGui::GetCursorPosX(), right));
    } else {
        ImGui::SameLine(std::max(ImGui::GetCursorPosX(), right));
    }
    if (ImGui::Button("Options")) ImGui::OpenPopup("##ra_options");
    ImGui::SameLine();
    if (ImGui::Button("Sign out")) {
        sign_out();
        return;
    }
    if (!account) ImGui::TextColored(c.ink_muted, "%s", full);
    if (!login_message.empty()) ImGui::TextColored(login_message_error ? c.bad : c.ink_muted, "%s", login_message.c_str());

    if (ImGui::BeginPopup("##ra_options")) {
        bool hc = config.get_bool("ra.hardcore", false);
        if (theme::toggle("Hardcore mode", &hc)) {
            if (hc && rc_client_is_game_loaded(client)) {
                confirm_hardcore = true;
                ImGui::CloseCurrentPopup();
            } else {
                config.set_bool("ra.hardcore", hc);
                rc_client_set_hardcore_enabled(client, hc);
            }
        }
        ImGui::PushFont(theme::font_small());
        ImGui::TextColored(c.ink_muted, "Hardcore unlocks are worth more and are marked on your profile. Loading save");
        ImGui::TextColored(c.ink_muted, "states is turned off while it is on (your battery save still works).");
        if (rc_client_get_achievement_info(client, kWarningAchievementId))
            ImGui::TextColored(c.bad, "RetroAchievements does not accept hardcore unlocks from SuperSaga yet.");
        ImGui::PopFont();
        ImGui::Separator();
        bool v = config.get_bool("ra.notify_unlocks", true);
        if (theme::toggle("Show a notification when an achievement unlocks", &v)) config.set_bool("ra.notify_unlocks", v);
        v = config.get_bool("ra.notify_progress", true);
        if (theme::toggle("Show progress updates", &v)) config.set_bool("ra.notify_progress", v);
        v = config.get_bool("ra.notify_leaderboards", true);
        if (theme::toggle("Show leaderboard notices", &v)) config.set_bool("ra.notify_leaderboards", v);
        ImGui::EndPopup();
    }
    if (confirm_hardcore) {
        ImGui::OpenPopup("Turn on hardcore?");
        confirm_hardcore = false;
    }
    if (ImGui::BeginPopupModal("Turn on hardcore?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("The game restarts from the beginning. Your battery save is kept.");
        if (ImGui::Button("Restart in hardcore")) {
            config.set_bool("ra.hardcore", true);
            rc_client_set_hardcore_enabled(client, 1);  // raises a reset event: the game restarts
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::Spacing();
    draw_game();
}

// ---- public ----------------------------------------------------------------------------------

Achievements::Achievements(Config& config, SDL_Renderer* renderer) : impl_(std::make_unique<Impl>(config, renderer)) {
    impl_->create_client();
    impl_->login_with_saved_token();
}

Achievements::~Achievements() {
    impl_->restart = nullptr;
    // Give in-flight unlocks a moment to reach the server before quitting.
    Uint64 until = SDL_GetTicks() + 2000;
    while (http::pending() > 0 && SDL_GetTicks() < until) {
        http::pump();
        SDL_Delay(10);
    }
    *impl_->alive = false;  // replies still on their way are dropped
    rc_client_destroy(impl_->client);
    impl_->client = nullptr;
    for (auto& [url, b] : impl_->badges)
        if (b.tex) SDL_DestroyTexture(b.tex);
}

void Achievements::game_started(const gba::Gba& gba, const std::vector<std::uint8_t>& rom) {
    Impl& s = *impl_;
    (void)gba;  // memory is read through frame()
    s.session_frames = 0;
    s.session_loaded_state = false;
    std::string hash = ra_md5_hex(rom.data(), rom.size());
    if (hash != s.game_hash || !rc_client_is_game_loaded(s.client)) {
        if (rc_client_is_game_loaded(s.client)) rc_client_unload_game(s.client);
        s.game_hash = hash;
        s.game_requested = false;
    } else {
        rc_client_reset(s.client);
    }
    s.login_with_saved_token();
    s.maybe_load_game();
}

void Achievements::game_closed() {
    Impl& s = *impl_;
    s.gba = nullptr;
    s.game_hash.clear();
    s.game_requested = false;
    s.game_message.clear();
    if (s.client) rc_client_unload_game(s.client);
}

void Achievements::frame(const gba::Gba& gba) {
    Impl& s = *impl_;
    s.gba = &gba;
    ++s.session_frames;
    s.frame_ran = true;
    if (s.enabled() && rc_client_is_game_loaded(s.client)) rc_client_do_frame(s.client);
    s.gba = nullptr;  // memory is only read during do_frame
}

void Achievements::idle(const gba::Gba* game) {
    Impl& s = *impl_;
    // The game's achievement list arrives here, and rcheevos checks every address it uses right
    // away, so the game's memory must be readable now, not only inside frame().
    s.gba = game;
    http::pump();
    if (!s.frame_ran) rc_client_idle(s.client);
    s.gba = nullptr;
    s.frame_ran = false;
    s.flush_toasts();
    if (s.restart_pending) {
        s.restart_pending = false;
        if (s.restart) s.restart();  // recreates the Gba; game_started() resets rc_client
    }
}

void Achievements::state_loaded() {
    Impl& s = *impl_;
    s.session_loaded_state = true;
    if (rc_client_is_game_loaded(s.client)) rc_client_deserialize_progress(s.client, nullptr);
}

bool Achievements::hardcore() const {
    const Impl& s = *impl_;
    return s.enabled() && s.login == Impl::Login::SignedIn && rc_client_get_hardcore_enabled(s.client);
}

bool Achievements::can_load_state(std::string* why) const {
    if (!hardcore()) return true;
    if (why) *why = "Loading save states is off in RetroAchievements hardcore mode.";
    return false;
}

void Achievements::set_restart_handler(std::function<void()> restart) { impl_->restart = std::move(restart); }

void Achievements::draw_tab() { impl_->draw_tab(); }

}  // namespace supersaga
