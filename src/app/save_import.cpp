#include "app/save_import.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>
#include <fstream>
#include <system_error>


namespace supersaga {
namespace fs = std::filesystem;

namespace {

constexpr char kDsvFooter[] = "|<--Snip above here to create a raw sav";

std::string lower_ext(const std::string& filename) {
    std::string e = fs::path(filename).extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return e;
}

bool is_state_extension(const std::string& e) {
    if (e == ".state" || e == ".ss" || e == ".sgm" || e == ".st" || e == ".save_state") return true;
    // .ss0-.ss9 (mGBA), .st0-.st9 / .state1.. (RetroArch), .sg0-.sg9, .ml1.. (VBA)
    if (e.size() == 4 && std::isdigit(static_cast<unsigned char>(e[3])))
        return e.rfind(".ss", 0) == 0 || e.rfind(".st", 0) == 0 || e.rfind(".sg", 0) == 0 ||
               e.rfind(".ml", 0) == 0;
    if (e.rfind(".state", 0) == 0) return true;
    return false;
}

bool all_equal(const std::uint8_t* p, std::size_t n, std::uint8_t v) {
    for (std::size_t i = 0; i < n; ++i)
        if (p[i] != v) return false;
    return true;
}

}  // namespace

bool has_game_header(const std::vector<std::uint8_t>& sram) {
    const std::string_view h = game::kSaveHeader;
    return h.empty() || (sram.size() >= h.size() && std::memcmp(sram.data(), h.data(), h.size()) == 0);
}

NormalizedSave normalize_save(const std::vector<std::uint8_t>& file, const std::string& filename) {
    NormalizedSave r;
    std::string ext = lower_ext(filename);
    auto starts_with = [&](const char* magic, std::size_t n) {
        return file.size() >= n && std::memcmp(file.data(), magic, n) == 0;
    };
    if (is_state_extension(ext) || starts_with("RASTATE", 7) || starts_with("\x89PNG", 4) ||
        starts_with("RZIP", 4)) {
        r.error = "That is a save state, not a battery save. Save states from other emulators can't be "
                  "imported; use the in-game save file instead (.sav or .srm).";
        return r;
    }

    std::vector<std::uint8_t> buf = file;

    // DeSmuME .dsv: raw save followed by a text footer.
    {
        const std::size_t n = sizeof(kDsvFooter) - 1;
        if (buf.size() > n) {
            auto it = std::search(buf.begin(), buf.end(), kDsvFooter, kDsvFooter + n);
            if (it != buf.end()) {
                buf.erase(it, buf.end());
                r.format = "DeSmuME .dsv, ";
            }
        }
    }

    const std::size_t size = buf.size();
    // Small trailers appended by some emulators (RetroArch/VBA real-time-clock data, etc.).
    auto base_size = [&](std::size_t expected) {
        return size >= expected && size - expected <= 64 ? expected : 0;
    };

    auto kib = [](std::size_t n) {
        return n >= 1024 && n % 1024 == 0 ? std::to_string(n / 1024) + " KiB" : std::to_string(n) + " bytes";
    };
    const std::string ours = kib(kSramSize) + " " + game::kSaveChip;
    // Larger saves whose extra part is padding or copies of the first part (some emulators and
    // flash carts round every save up to a fixed size).
    std::size_t padded = 0;
    for (std::size_t n : {std::size_t(32 * 1024), std::size_t(64 * 1024), std::size_t(128 * 1024)})
        if (n > kSramSize && base_size(n)) padded = n;

    if (std::size_t b = base_size(kSramSize)) {
        buf.resize(b);
        r.format += size == kSramSize ? "raw " + ours
                                      : ours + " with " + std::to_string(size - b) + " extra bytes (ignored)";
    } else if (padded) {
        const std::uint8_t* tail = buf.data() + kSramSize;
        std::size_t tail_len = padded - kSramSize;
        bool padding = all_equal(tail, tail_len, 0xFF) || all_equal(tail, tail_len, 0x00);
        bool mirror = true;
        for (std::size_t off = 0; off < tail_len && mirror; off += kSramSize)
            mirror = std::memcmp(tail + off, buf.data(), kSramSize) == 0;
        if (!padding && !mirror) {
            r.error = "This save is " + kib(padded) + " and its extra data is not padding, so it is from "
                      "another game. " + std::string(game::kTitle) + " uses a " + ours + " save.";
            return r;
        }
        buf.resize(kSramSize);
        r.format += kib(padded) + " save (" + (mirror ? "mirrored" : "padded") + "), first " + kib(kSramSize) + " used";
    } else if (size == 0) {
        r.error = "The file is empty.";
        return r;
    } else {
        r.error = "Unrecognized save size (" + std::to_string(size) + " bytes). " + std::string(game::kTitle) +
                  " uses a " + ours + " save (" + std::to_string(kSramSize) + " bytes, .sav or .srm).";
        return r;
    }

    if (all_equal(buf.data(), buf.size(), 0xFF) || all_equal(buf.data(), buf.size(), 0x00)) {
        r.warning = "This save is blank, so importing it starts the game with no progress.";
    } else if (!has_game_header(buf)) {
        r.error = "This save doesn't belong to " + std::string(game::kTitle) + " (its game header is missing).";
        return r;
    }
    r.data = std::move(buf);
    r.ok = true;
    return r;
}

bool read_file(const fs::path& p, std::vector<std::uint8_t>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

bool write_file_atomic(const fs::path& p, const std::vector<std::uint8_t>& data) {
    fs::path tmp = p;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
        if (!f) return false;
    }
    std::error_code ec;
    fs::rename(tmp, p, ec);
    return !ec;
}

std::vector<fs::path> list_backups(const fs::path& backups, const std::string& stem) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (auto it = fs::directory_iterator(backups, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::string name = it->path().filename().string();
        if (name.rfind(stem + "-", 0) == 0 && it->path().extension() == ".sav") out.push_back(it->path());
    }
    // Names embed YYYYmmdd-HHMMSS, so lexical order is chronological.
    std::sort(out.begin(), out.end(), [](const fs::path& a, const fs::path& b) { return a.filename() > b.filename(); });
    return out;
}

fs::path backup_save(const fs::path& save, const fs::path& backups, std::size_t keep) {
    std::error_code ec;
    if (!fs::exists(save, ec)) return {};
    fs::create_directories(backups, ec);
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
    std::string stem = save.stem().string();
    fs::path dst = backups / (stem + "-" + stamp + ".sav");
    for (int n = 2; fs::exists(dst, ec); ++n) dst = backups / (stem + "-" + stamp + "-" + std::to_string(n) + ".sav");
    fs::copy_file(save, dst, ec);
    if (ec) return {};
    auto all = list_backups(backups, stem);
    for (std::size_t i = keep; i < all.size(); ++i) fs::remove(all[i], ec);
    return dst;
}

}  // namespace supersaga
