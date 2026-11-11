// RetroAchievements glue, offline: the ROM hash, the GBA memory map handed to rcheevos, and a full
// rc_client session (login, game load, unlock) against canned server replies, reading memory
// from a real supersaga::gba::Gba. No network and no ROM needed.
#include <rc_client.h>
#include <rc_consoles.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "app/achievements.h"
#include "gba/gba.h"

namespace fs = std::filesystem;
using supersaga::gba::Gba;

static int failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

static const Gba* g_gba = nullptr;
static std::vector<std::string> g_requests;
static std::vector<std::uint32_t> g_unlocked;
static bool g_blank = false;  // reads return zeros (an achievement must be false before it can trigger)

static std::uint32_t RC_CCONV read_memory(std::uint32_t address, std::uint8_t* buffer, std::uint32_t n, rc_client_t*) {
    std::uint32_t got = supersaga::ra_read_memory(g_gba, address, buffer, n);
    if (g_blank) std::memset(buffer, 0, got);
    return got;
}

static void RC_CCONV server_call(const rc_api_request_t* request, rc_client_server_callback_t callback, void* data,
                                 rc_client_t*) {
    std::string post = request->post_data ? request->post_data : "";
    g_requests.push_back(post);
    std::string body = "{\"Success\":false,\"Error\":\"unexpected\"}";
    if (post.find("r=login2") != std::string::npos)
        body = "{\"Success\":true,\"User\":\"Player\",\"Token\":\"TOKEN\",\"Score\":10,\"SoftcoreScore\":0,"
               "\"Messages\":0,\"Permissions\":1,\"AccountType\":\"Registered\"}";
    else if (post.find("r=achievementsets") != std::string::npos)
        body = "{\"Success\":true,\"GameId\":770,\"Title\":\"Test\",\"ConsoleId\":5,"
               "\"ImageIconUrl\":\"http://server/Images/1.png\",\"RichPresenceGameId\":770,\"RichPresencePatch\":\"\","
               "\"Sets\":[{\"AchievementSetId\":1,\"GameId\":770,\"Title\":null,\"Type\":\"core\","
               "\"ImageIconUrl\":\"http://server/Images/1.png\",\"Achievements\":["
               // SRAM bytes 0 and 1 (0x48000 in the rcheevos GBA map)
               "{\"ID\":1,\"Title\":\"SRAM\",\"Description\":\"d\",\"Flags\":3,\"Points\":5,"
               "\"MemAddr\":\"0xH48000=171_0xH48001=205\",\"Author\":\"a\",\"BadgeName\":\"00001\","
               "\"Created\":1,\"Modified\":1},"
               // an SRAM byte that is never 0x42
               "{\"ID\":2,\"Title\":\"Never\",\"Description\":\"d\",\"Flags\":3,\"Points\":5,"
               "\"MemAddr\":\"0xH48002=66\",\"Author\":\"a\",\"BadgeName\":\"00002\",\"Created\":1,\"Modified\":1}"
               "],\"Leaderboards\":[]}]}";
    else if (post.find("r=startsession") != std::string::npos)
        body = "{\"Success\":true,\"Unlocks\":[],\"HardcoreUnlocks\":[]}";
    else if (post.find("r=awardachievement") != std::string::npos)
        body = "{\"Success\":true,\"Score\":15,\"SoftcoreScore\":0,\"AchievementID\":1,\"AchievementsRemaining\":1}";
    else if (post.find("r=ping") != std::string::npos)
        body = "{\"Success\":true}";
    rc_api_server_response_t r{};
    r.body = body.c_str();
    r.body_length = body.size();
    r.http_status_code = 200;
    callback(&r, data);
}

static void RC_CCONV on_event(const rc_client_event_t* e, rc_client_t*) {
    if (e->type == RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED) g_unlocked.push_back(e->achievement->id);
}

static void RC_CCONV expect_ok(int result, const char* error, rc_client_t*, void*) {
    if (result != RC_OK) std::fprintf(stderr, "rc_client error %d: %s\n", result, error ? error : "");
    CHECK(result == RC_OK);
}

int main() {
    // MD5 (RFC 1321 test vectors).
    CHECK(supersaga::ra_md5_hex(reinterpret_cast<const std::uint8_t*>("abc"), 3) == "900150983cd24fb0d6963f7d28e17f72");
    CHECK(supersaga::ra_md5_hex(nullptr, 0) == "d41d8cd98f00b204e9800998ecf8427e");

    // A Gba whose battery save starts with AB CD: SRAM is visible at 0x48000.
    fs::path dir = fs::temp_directory_path() / "supersaga_achievements_test";
    fs::create_directories(dir);
    fs::path sav = dir / "test.sav";
    {
        std::vector<char> data(0x8000, 0);
        data[0] = char(0xAB);
        data[1] = char(0xCD);
        std::ofstream(sav, std::ios::binary).write(data.data(), std::streamsize(data.size()));
    }
    auto machine = std::make_unique<Gba>(std::vector<std::uint8_t>(0x1000, 0), sav);  // too big for a Windows stack
    Gba& gba = *machine;
    g_gba = &gba;

    std::uint8_t b[4] = {};
    CHECK(supersaga::ra_read_memory(&gba, 0x48000, b, 2) == 2 && b[0] == 0xAB && b[1] == 0xCD);
    CHECK(supersaga::ra_read_memory(&gba, 0x50000, b, 1) == 1 && b[0] == 0);  // past this test's 32 KiB SRAM
    CHECK(supersaga::ra_read_memory(&gba, 0x57FFF, b, 4) == 1);              // end of the map
    CHECK(supersaga::ra_read_memory(&gba, 0x58000, b, 1) == 0);
    CHECK(supersaga::ra_read_memory(&gba, 0x7FFE, b, 4) == 4);                // IWRAM into EWRAM
    // No machine attached: addresses in the map read as 0, past it as nothing.
    CHECK(supersaga::ra_read_memory(nullptr, 0x40, b, 4) == 4 && b[0] == 0 && b[3] == 0);
    CHECK(supersaga::ra_read_memory(nullptr, 0x57FFF, b, 4) == 1);
    CHECK(supersaga::ra_read_memory(nullptr, 0x58000, b, 1) == 0);
    CHECK(gba.iwram().size() == 0x8000 && gba.ewram().size() == 0x40000);

    // Full session against canned replies.
    rc_client_t* client = rc_client_create(&read_memory, &server_call);
    rc_client_set_event_handler(client, &on_event);
    rc_client_begin_login_with_password(client, "Player", "secret", &expect_ok, nullptr);
    CHECK(rc_client_get_user_info(client) != nullptr);
    // The app loads the game between frames, when no machine is attached to the reader: the
    // achievements must still come up active, not Unsupported.
    g_gba = nullptr;
    rc_client_begin_load_game(client, "35ae64b0f27e60107c14ab956f6cdf70", &expect_ok, nullptr);
    CHECK(rc_client_is_game_loaded(client));
    const rc_client_game_t* game = rc_client_get_game_info(client);
    CHECK(game && game->console_id == RC_CONSOLE_GAMEBOY_ADVANCE && game->id == 770);
    {
        rc_client_achievement_list_t* list =
            rc_client_create_achievement_list(client, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE, RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
        CHECK(list != nullptr);
        for (std::uint32_t i = 0; list && i < list->num_buckets; ++i)
            CHECK(list->buckets[i].bucket_type != RC_CLIENT_ACHIEVEMENT_BUCKET_UNSUPPORTED);
        rc_client_destroy_achievement_list(list);
    }
    g_gba = &gba;
    g_blank = true;
    for (int i = 0; i < 3; ++i) rc_client_do_frame(client);
    CHECK(g_unlocked.empty());
    g_blank = false;  // the SRAM pattern appears
    for (int i = 0; i < 3; ++i) rc_client_do_frame(client);
    CHECK(g_unlocked.size() == 1 && g_unlocked[0] == 1);
    bool awarded = false;
    for (const std::string& r : g_requests) awarded |= r.find("r=awardachievement") != std::string::npos;
    CHECK(awarded);
    // The ROM is identified by its hash only.
    for (const std::string& r : g_requests) CHECK(r.size() < 1024);
    rc_client_destroy(client);

    std::error_code ec;
    fs::remove_all(dir, ec);
    if (failures == 0) std::printf("achievements: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
