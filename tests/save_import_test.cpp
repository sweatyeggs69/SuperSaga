// Tests for the save importer with synthetic buffers (no real save data).
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "app/save_import.h"

namespace {
int failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

using Buf = std::vector<std::uint8_t>;

Buf fake_sram() {
    Buf b(supersaga::kSramSize, 0xFF);
    std::memcpy(b.data(), supersaga::game::kSaveHeader.data(), supersaga::game::kSaveHeader.size());
    b[0x40] = 0x01;  // some non-blank data even while the header is not set
    for (int i = 0; i < 116; ++i) b[0x200 + i] = std::uint8_t(i * 7);
    return b;
}
}  // namespace

int main() {
    using supersaga::normalize_save;
    const Buf sram = fake_sram();

    {  // raw 8 KiB EEPROM
        auto r = normalize_save(sram, "Saga.sav");
        CHECK(r.ok);
        CHECK(r.data == sram);
        CHECK(r.warning.empty());
    }
    {  // RetroArch .srm with a 16-byte RTC trailer
        Buf b = sram;
        b.insert(b.end(), 16, 0x12);
        auto r = normalize_save(b, "Saga.srm");
        CHECK(r.ok);
        CHECK(r.data == sram);
    }
    {  // 64 KiB padded with 0xFF
        Buf b = sram;
        b.insert(b.end(), 64 * 1024 - supersaga::kSramSize, 0xFF);
        auto r = normalize_save(b, "x.sav");
        CHECK(r.ok && r.data == sram);
    }
    {  // 32 KiB mirrored
        Buf b;
        for (int i = 0; i < 4; ++i) b.insert(b.end(), sram.begin(), sram.end());
        auto r = normalize_save(b, "x.sav");
        CHECK(r.ok && r.data == sram);
        CHECK(r.format.find("mirrored") != std::string::npos);
    }
    {  // 32 KiB padded with 0x00 (flash carts round EEPROM saves up)
        Buf b = sram;
        b.insert(b.end(), 32 * 1024 - supersaga::kSramSize, 0x00);
        auto r = normalize_save(b, "x.sav");
        CHECK(r.ok && r.data == sram);
    }
    {  // 64 KiB with real data past the EEPROM part (an SRAM or Flash save from another game)
        Buf b = sram;
        Buf other(64 * 1024 - supersaga::kSramSize, 0x55);
        b.insert(b.end(), other.begin(), other.end());
        auto r = normalize_save(b, "x.sav");
        CHECK(!r.ok);
    }
    {  // DeSmuME .dsv footer
        Buf b = sram;
        const char* footer = "|<--Snip above here to create a raw sav by excluding this DeSmuME savedata footer:";
        b.insert(b.end(), footer, footer + std::strlen(footer));
        b.insert(b.end(), 24, 0x01);
        auto r = normalize_save(b, "x.dsv");
        CHECK(r.ok && r.data == sram);
    }
    {  // save states are rejected
        CHECK(!normalize_save(sram, "Saga.ss1").ok);
        CHECK(!normalize_save(sram, "Saga.state").ok);
        CHECK(!normalize_save(sram, "Saga.state3").ok);
        Buf b = sram;
        std::memcpy(b.data(), "RASTATE", 7);
        CHECK(!normalize_save(b, "x.sav").ok);
    }
    {  // other save sizes and junk sizes are rejected
        Buf sram32(32 * 1024, 0x00);
        std::memcpy(sram32.data() + 0x4000, "OTHERGAME!", 10);
        CHECK(!normalize_save(sram32, "x.sav").ok);
        CHECK(!normalize_save(Buf(512, 0), "x.sav").ok);
        CHECK(!normalize_save(Buf(1000, 0), "x.sav").ok);
        CHECK(!normalize_save(Buf(), "x.sav").ok);
    }
    if (!supersaga::game::kSaveHeader.empty()) {  // the save signature: every byte counts
        using supersaga::game::kSaveHeader;
        Buf ok = fake_sram();
        CHECK(supersaga::has_game_header(ok));
        CHECK(normalize_save(ok, "x.sav").ok);
        for (std::size_t at : {std::size_t(0), kSaveHeader.size() / 2, kSaveHeader.size() - 1}) {
            Buf bad = ok;
            bad[at] ^= 0x01;
            CHECK(!supersaga::has_game_header(bad));
            CHECK(!normalize_save(bad, "x.sav").ok);
        }
        Buf other(supersaga::kSramSize, 0x00);
        std::memcpy(other.data(), "OTHERGAME!", 10);
        CHECK(!normalize_save(other, "x.sav").ok);
    }
    {  // a blank save is accepted with a warning
        auto blank = normalize_save(Buf(supersaga::kSramSize, 0xFF), "x.sav");
        CHECK(blank.ok && !blank.warning.empty());
    }
    {  // backups: newest first, pruned to `keep`
        namespace fs = std::filesystem;
        fs::path dir = fs::temp_directory_path() / "supersaga_save_test";
        fs::remove_all(dir);
        fs::create_directories(dir / "backups");
        fs::path save = dir / "abc.sav";
        CHECK(supersaga::write_file_atomic(save, sram));
        for (int i = 0; i < 13; ++i) CHECK(!supersaga::backup_save(save, dir / "backups", 10).empty());
        auto list = supersaga::list_backups(dir / "backups", "abc");
        CHECK(list.size() == 10);
        Buf back;
        CHECK(supersaga::read_file(list.front(), back) && back == sram);
        fs::remove_all(dir);
    }

    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("save import tests passed\n");
    return 0;
}
