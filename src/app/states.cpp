#include "app/states.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <system_error>

#include "app/fsutil.h"
#include "app/save_import.h"
#include "gba/gba.h"

#if SUPERSAGA_HAVE_ZLIB
#include <zlib.h>
#endif

#ifndef SUPERSAGA_VERSION
#define SUPERSAGA_VERSION "dev"
#endif

namespace supersaga::states {
namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;

namespace {

constexpr char kMagic[8] = {'M', 'F', 'U', 'S', 'S', 'T', 'A', 'T'};
constexpr std::uint32_t kFileVersion = 1;
constexpr int kW = 240, kH = 160;
enum : std::uint32_t { kStored = 0, kDeflate = 1 };

Hotkeys g_hotkeys;

void put_u32(Bytes& b, std::uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(std::uint8_t(v >> (8 * i))); }
void put_u64(Bytes& b, std::uint64_t v) { for (int i = 0; i < 8; ++i) b.push_back(std::uint8_t(v >> (8 * i))); }
void put_str(Bytes& b, const std::string& s) {
    put_u32(b, std::uint32_t(s.size()));
    b.insert(b.end(), s.begin(), s.end());
}

struct Cursor {
    const Bytes& b;
    size_t p = 0;
    bool ok = true;
    std::uint32_t u32() {
        if (p + 4 > b.size()) { ok = false; return 0; }
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= std::uint32_t(b[p + i]) << (8 * i);
        p += 4;
        return v;
    }
    std::uint64_t u64() {
        std::uint64_t lo = u32(), hi = u32();
        return lo | (hi << 32);
    }
    std::string str(size_t max = 4096) {
        std::uint32_t n = u32();
        if (!ok || n > max || p + n > b.size()) { ok = false; return {}; }
        std::string s(b.begin() + long(p), b.begin() + long(p + n));
        p += n;
        return s;
    }
    Bytes blob(size_t max) {
        std::uint32_t n = u32();
        if (!ok || n > max || p + n > b.size()) { ok = false; return {}; }
        Bytes out(b.begin() + long(p), b.begin() + long(p + n));
        p += n;
        return out;
    }
};

// Compressed block: u32 method, u32 raw size, u32 stored size, data.
void put_block(Bytes& out, const Bytes& raw) {
#if SUPERSAGA_HAVE_ZLIB
    uLongf len = compressBound(uLong(raw.size()));
    Bytes z(len);
    if (compress2(z.data(), &len, raw.data(), uLong(raw.size()), 6) == Z_OK) {
        z.resize(len);
        put_u32(out, kDeflate);
        put_u32(out, std::uint32_t(raw.size()));
        put_u32(out, std::uint32_t(z.size()));
        out.insert(out.end(), z.begin(), z.end());
        return;
    }
#endif
    put_u32(out, kStored);
    put_u32(out, std::uint32_t(raw.size()));
    put_u32(out, std::uint32_t(raw.size()));
    out.insert(out.end(), raw.begin(), raw.end());
}

bool get_block(Cursor& c, Bytes& raw, size_t max_raw) {
    std::uint32_t method = c.u32(), raw_size = c.u32();
    Bytes data = c.blob(max_raw + 1024);
    if (!c.ok || raw_size > max_raw) return false;
    if (method == kStored) {
        if (data.size() != raw_size) return false;
        raw = std::move(data);
        return true;
    }
#if SUPERSAGA_HAVE_ZLIB
    if (method == kDeflate) {
        raw.resize(raw_size);
        uLongf len = raw_size;
        return uncompress(raw.data(), &len, data.data(), uLong(data.size())) == Z_OK && len == raw_size;
    }
#endif
    return false;
}

struct Parsed {
    std::string sha1, build, label;
    std::int64_t saved_at = 0;
    std::uint64_t playtime = 0;
    Bytes thumb_raw, state_raw;
};

// Parses a .ssst. With want_state false the (large) state block is not decompressed.
bool parse(const Bytes& file, Parsed& out, bool want_state, std::string* error) {
    auto fail = [&](const char* why) { if (error) *error = why; return false; };
    if (file.size() < 12 || std::memcmp(file.data(), kMagic, 8) != 0) return fail("Not a SuperSaga save state file.");
    Cursor c{file, 8};
    std::uint32_t version = c.u32();
    if (version != kFileVersion) return fail("This save state file is from a different version of SuperSaga.");
    out.sha1 = c.str(64);
    out.build = c.str(64);
    out.saved_at = std::int64_t(c.u64());
    out.playtime = c.u64();
    out.label = c.str(256);
    if (!c.ok) return fail("The save state file is damaged.");
    if (!get_block(c, out.thumb_raw, kW * kH * 2) || out.thumb_raw.size() != size_t(kW * kH * 2))
        return fail("The save state file is damaged.");
    if (want_state && !get_block(c, out.state_raw, 8u << 20)) return fail("The save state file is damaged.");
    return true;
}

Bytes build_file(const std::string& sha1, std::int64_t saved_at, std::uint64_t playtime, const std::string& label,
                 const Bytes& thumb_raw, const Bytes& state_raw) {
    Bytes out(kMagic, kMagic + 8);
    put_u32(out, kFileVersion);
    put_str(out, sha1);
    put_str(out, std::string("supersaga ") + SUPERSAGA_VERSION);
    put_u64(out, std::uint64_t(saved_at));
    put_u64(out, playtime);
    put_str(out, label.substr(0, 200));
    put_block(out, thumb_raw);
    put_block(out, state_raw);
    return out;
}

// Rewrites only the header fields of an existing file (keeps the blocks as they are).
Bytes relabel(const Bytes& file, const std::string& label) {
    Parsed p;
    Cursor c{file, 8};
    c.u32();
    p.sha1 = c.str(64);
    p.build = c.str(64);
    std::uint64_t at = c.u64(), play = c.u64();
    c.str(256);
    Bytes out(kMagic, kMagic + 8);
    put_u32(out, kFileVersion);
    put_str(out, p.sha1);
    put_str(out, p.build);
    put_u64(out, at);
    put_u64(out, play);
    put_str(out, label.substr(0, 200));
    out.insert(out.end(), file.begin() + long(c.p), file.end());
    return out;
}

Bytes thumb_bytes(const gba::Gba& g) {
    const std::uint16_t* px = g.framebuffer();
    Bytes b(size_t(kW * kH * 2));
    std::memcpy(b.data(), px, b.size());
    return b;
}

// Small PNG writer (RGB8) for the thumbnails shown in Finder; deflate if zlib is available.
std::uint32_t crc32_of(const std::uint8_t* d, size_t n, std::uint32_t c) {
    static std::uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t v = i;
            for (int k = 0; k < 8; ++k) v = (v & 1) ? 0xEDB88320 ^ (v >> 1) : v >> 1;
            table[i] = v;
        }
        init = true;
    }
    for (size_t i = 0; i < n; ++i) c = table[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return c;
}
void be32(Bytes& v, std::uint32_t x) { for (int i = 3; i >= 0; --i) v.push_back(std::uint8_t(x >> (8 * i))); }

bool write_png(const fs::path& path, const std::uint16_t* px) {
    Bytes raw;
    raw.reserve(size_t((kW * 3 + 1) * kH));
    for (int y = 0; y < kH; ++y) {
        raw.push_back(0);
        for (int x = 0; x < kW; ++x) {
            std::uint16_t c = px[y * kW + x];
            auto ex = [](std::uint16_t v) { return std::uint8_t((v << 3) | (v >> 2)); };
            raw.push_back(ex(c & 31));
            raw.push_back(ex((c >> 5) & 31));
            raw.push_back(ex((c >> 10) & 31));
        }
    }
    Bytes z;
#if SUPERSAGA_HAVE_ZLIB
    uLongf len = compressBound(uLong(raw.size()));
    z.resize(len);
    if (compress2(z.data(), &len, raw.data(), uLong(raw.size()), 6) != Z_OK) return false;
    z.resize(len);
#else
    z = {0x78, 0x01};
    std::uint32_t a = 1, b = 0;
    for (std::uint8_t ch : raw) { a = (a + ch) % 65521; b = (b + a) % 65521; }
    for (size_t off = 0; off < raw.size(); off += 65535) {
        size_t n = std::min<size_t>(65535, raw.size() - off);
        z.push_back(off + n >= raw.size() ? 1 : 0);
        z.push_back(std::uint8_t(n)); z.push_back(std::uint8_t(n >> 8));
        z.push_back(std::uint8_t(~n)); z.push_back(std::uint8_t(~n >> 8));
        z.insert(z.end(), raw.begin() + long(off), raw.begin() + long(off + n));
    }
    be32(z, (b << 16) | a);
#endif
    Bytes png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    auto chunk = [&](const char* type, const Bytes& data) {
        be32(png, std::uint32_t(data.size()));
        size_t start = png.size();
        png.insert(png.end(), type, type + 4);
        png.insert(png.end(), data.begin(), data.end());
        be32(png, ~crc32_of(&png[start], png.size() - start, 0xFFFFFFFF));
    };
    Bytes ihdr;
    be32(ihdr, kW);
    be32(ihdr, kH);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
    return write_file_atomic(path, png);
}

}  // namespace

std::string slot_name(int slot) { return "Slot " + std::to_string(slot); }

std::string format_playtime(std::uint64_t frames) {
    std::uint64_t secs = frames * 280896ull / 16777216ull;
    char buf[32];
    std::snprintf(buf, sizeof buf, "%llu:%02llu:%02llu", (unsigned long long)(secs / 3600),
                  (unsigned long long)(secs / 60 % 60), (unsigned long long)(secs % 60));
    return buf;
}

Store::Store(fs::path dir, std::string rom_sha1, fs::path sav_path)
    : dir_(std::move(dir)), sha1_(std::move(rom_sha1)), sav_(std::move(sav_path)) {
    std::error_code ec;
    fs::create_directories(dir_, ec);
    shifted_ = migrate_auto_slot(dir_, "ssst");
}

int migrate_auto_slot(const fs::path& dir, const std::string& ext) {
    std::error_code ec;
    const fs::path legacy = dir / ("auto." + ext);
    if (!fs::exists(legacy, ec)) return 0;
    auto path = [&](const std::string& stem, const char* e) { return dir / (stem + e); };
    auto slot_stem = [](int s) { return "slot-" + std::to_string(s); };
    const std::string main_ext = "." + ext;
    const char* exts[] = {main_ext.c_str(), ".png", ".bak"};
    // Moves one state and its thumbnail / backup; a companion file the source lacks is cleared at
    // the destination so it can't end up attached to the wrong state.
    auto move = [&](const std::string& from, const std::string& to) {
        for (const char* e : exts) {
            std::error_code mec;
            if (fs::exists(path(from, e), mec)) fs::rename(path(from, e), path(to, e), mec);
            else fs::remove(path(to, e), mec);
        }
    };
    int gap = 1;
    while (gap <= kSlots && fs::exists(path(slot_stem(gap), main_ext.c_str()), ec)) ++gap;
    if (gap > kSlots) {  // every slot is full: the last one makes way (recoverable from the Trash)
        gap = kSlots;
        for (const char* e : exts)
            if (fs::exists(path(slot_stem(gap), e), ec)) move_to_trash(path(slot_stem(gap), e));
    }
    for (int s = gap - 1; s >= 1; --s) move(slot_stem(s), slot_stem(s + 1));
    move("auto", slot_stem(1));
    return gap - 1;
}

int Store::latest() {
    int best = -1;
    for (int s = kFirstSlot; s <= kSlots; ++s) {
        const SlotInfo& i = info(s);
        if (i.exists && i.error.empty() && (best < 0 || i.saved_at > info(best).saved_at)) best = s;
    }
    return best;
}

fs::path Store::file(int slot) const {
    return dir_ / ("slot-" + std::to_string(slot) + ".ssst");
}

void Store::refresh() { cache_valid_ = false; }

const SlotInfo& Store::info(int slot) {
    if (!cache_valid_) {
        cache_.assign(kSlots + 1, SlotInfo{});
        for (int s = kFirstSlot; s <= kSlots; ++s) {
            SlotInfo& i = cache_[size_t(s)];
            i.slot = s;
            Bytes data;
            std::error_code ec;
            if (!fs::exists(file(s), ec) || !read_file(file(s), data)) continue;
            i.exists = true;
            Parsed p;
            std::string err;
            if (!parse(data, p, false, &err)) { i.error = err; continue; }
            if (p.sha1 != sha1_) { i.error = "Made with a different ROM."; continue; }
            i.saved_at = std::time_t(p.saved_at);
            i.playtime_frames = p.playtime;
            i.label = p.label;
            i.thumb.resize(size_t(kW * kH));
            std::memcpy(i.thumb.data(), p.thumb_raw.data(), p.thumb_raw.size());
        }
        cache_valid_ = true;
    }
    return cache_[size_t(std::clamp(slot, kFirstSlot, kSlots))];
}

bool Store::save(int slot, const gba::Gba& g, std::string* error, const std::string& label) {
    std::error_code ec;
    fs::path f = file(slot);
    std::string keep_label = label;
    if (keep_label.empty()) {
        const SlotInfo& old = info(slot);
        if (old.exists && old.error.empty()) keep_label = old.label;
    }
    Bytes data = build_file(sha1_, std::int64_t(std::time(nullptr)), g.frame_count(), keep_label, thumb_bytes(g),
                            g.save_state());
    if (fs::exists(f, ec)) {
        fs::path bak = f;
        bak.replace_extension(".bak");
        fs::copy_file(f, bak, fs::copy_options::overwrite_existing, ec);
    }
    if (!write_file_atomic(f, data)) {
        if (error) *error = "Could not write " + f.string();
        return false;
    }
    fs::path png = f;
    png.replace_extension(".png");
    write_png(png, g.framebuffer());
    refresh();
    return true;
}

bool Store::load(int slot, gba::Gba& g, std::string* error) {
    Bytes data;
    if (!read_file(file(slot), data)) {
        if (error) *error = slot_name(slot) + " is empty.";
        return false;
    }
    Parsed p;
    if (!parse(data, p, true, error)) return false;
    if (p.sha1 != sha1_) {
        if (error) *error = "That save state was made with a different ROM.";
        return false;
    }
    Bytes before = g.save_state();
    // Keep the battery save recoverable: the loaded state carries its own SRAM, which is written
    // to the .sav the next time the game saves.
    {
        Bytes cur, newest;
        auto list = list_backups(sav_.parent_path() / "backups", sav_.stem().string());
        bool same = read_file(sav_, cur) && !list.empty() && read_file(list.front(), newest) && cur == newest;
        if (!same) backup_save(sav_, sav_.parent_path() / "backups");
    }
    if (!g.load_state(p.state_raw, error)) return false;
    undo_ = std::move(before);
    return true;
}

bool Store::undo_load(gba::Gba& g, std::string* error) {
    if (!undo_) {
        if (error) *error = "There is nothing to undo.";
        return false;
    }
    if (!g.load_state(*undo_, error)) return false;
    undo_.reset();
    return true;
}

bool Store::remove(int slot, std::string* error) {
    std::error_code ec;
    for (const char* ext : {".ssst", ".png", ".bak"}) {
        fs::path f = file(slot);
        f.replace_extension(ext);
        if (!fs::exists(f, ec)) continue;
        if (!move_to_trash(f)) {
            if (error) *error = "Could not move " + f.string() + " to the Trash.";
            return false;
        }
    }
    refresh();
    return true;
}

bool Store::rename(int slot, const std::string& label, std::string* error) {
    Bytes data;
    Parsed p;
    if (!read_file(file(slot), data) || !parse(data, p, false, error)) return false;
    if (!write_file_atomic(file(slot), relabel(data, label))) {
        if (error) *error = "Could not write " + file(slot).string();
        return false;
    }
    refresh();
    return true;
}

bool Store::export_slot(int slot, const fs::path& to, std::string* error) {
    Bytes data;
    if (!read_file(file(slot), data)) {
        if (error) *error = slot_name(slot) + " is empty.";
        return false;
    }
    fs::path out = to;
    if (out.extension() != ".ssst") out += ".ssst";
    if (!write_file_atomic(out, data)) {
        if (error) *error = "Could not write " + out.string();
        return false;
    }
    return true;
}

bool Store::import_into(int slot, const fs::path& from, std::string* error) {
    Bytes data;
    if (!read_file(from, data)) {
        if (error) *error = "Could not read " + from.string();
        return false;
    }
    Parsed p;
    if (!parse(data, p, true, error)) return false;
    if (p.sha1 != sha1_) {
        if (error) *error = "That save state was made with a different ROM.";
        return false;
    }
    // Make sure this build can actually load it before replacing the slot.
    std::error_code ec;
    fs::path f = file(slot);
    if (fs::exists(f, ec)) {
        fs::path bak = f;
        bak.replace_extension(".bak");
        fs::copy_file(f, bak, fs::copy_options::overwrite_existing, ec);
    }
    if (!write_file_atomic(f, data)) {
        if (error) *error = "Could not write " + f.string();
        return false;
    }
    fs::path png = f;
    png.replace_extension(".png");
    std::vector<std::uint16_t> px(size_t(kW * kH));
    std::memcpy(px.data(), p.thumb_raw.data(), p.thumb_raw.size());
    write_png(png, px.data());
    refresh();
    return true;
}

void set_hotkeys(Hotkeys h) { g_hotkeys = std::move(h); }
void quick_save() { if (g_hotkeys.quick_save) g_hotkeys.quick_save(); }
void save_new_slot() { if (g_hotkeys.save_new_slot) g_hotkeys.save_new_slot(); }
void quick_load() { if (g_hotkeys.quick_load) g_hotkeys.quick_load(); }
void slot_step(int delta) { if (g_hotkeys.slot_step) g_hotkeys.slot_step(delta); }

}  // namespace supersaga::states
