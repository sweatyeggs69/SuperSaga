// Widescreen support specific to Mario & Luigi: Superstar Saga (USA, A88E).
//
// In the field (rooms and the overworld) the game draws three block layers with BG1-BG3,
// each a 512x256 tilemap in screen blocks 0, 2 and 4 that acts as a ring buffer around the
// 240x160 view. The whole room is kept decompressed in EWRAM, and the game's column/row
// writer (sub_0805905C) copies the part around the view into a shadow of those screen blocks
// that VBlank DMAs to VRAM. For the extra columns of a widescreen view we rebuild tilemap
// entries straight from RAM the same way:
//
//   field struct F (IWRAM, tagged "S_MP" at F + 0x08):
//     F + 0x2A + L*2   u16  row stride of layer L's block map (in 16x16 blocks)
//     F + 0x42 + L*2   u16  room width of layer L (in blocks)
//     F + 0x48 + L*2   u16  room height of layer L (in blocks)
//     F + 0x4E + L*2   u16  camera x of layer L (pixels)
//     F + 0x54 + L*2   u16  camera y of layer L (pixels)
//     F + 0x98         u32  block maps; layer L starts at entry L * 0x800
//     F + 0xA8         u32  block -> 4 tilemap entries table (top left, top right,
//                           bottom left, bottom right)
//   block = u16 at map + (L * 0x800 + by * stride + bx) * 2
//   entry = u16 at table + block * 8 + quarter * 2
//
// HOFS/VOFS only hold the camera modulo 512/256, so once per frame we check which absolute
// camera position matches what is actually in VRAM by comparing sample tiles of the visible
// area (the game's own camera is tried first). If nothing matches (title, menus, battles,
// cutscenes with other layouts) that layer is not rebuilt. Gameplay is the frames where at
// least one block layer matches; only those widen. A block layer that does not match, and
// every other background (text boxes, the field HUD), is hidden at the sides rather than
// showing stale or wrapped VRAM. Only addresses and layout descriptions belong here; never
// copy game data into the repo.
#include "gba.h"

namespace supersaga::gba {

namespace {
constexpr u32 kField = 0x030046F4;     // usual address of the field struct F
constexpr u32 kFieldTag = 0x504D5F53;  // "S_MP" at F + 0x08
constexpr u32 kStride = 0x2A, kWidth = 0x42, kHeight = 0x48, kCamX = 0x4E, kCamY = 0x54;
constexpr u32 kMaps = 0x98, kTable = 0xA8;
constexpr u32 kLayerEntries = 0x800;  // block map entries reserved per layer

bool in_ewram(u32 a) { return (a >> 24) == 0x02; }
}  // namespace

u16 Gba::wide_ram16(u32 a) const {
    if ((a >> 24) == 0x02) return u16(ewram_[a & 0x3FFFF] | (ewram_[(a + 1) & 0x3FFFF] << 8));
    if ((a >> 24) == 0x03) return u16(iwram_[a & 0x7FFF] | (iwram_[(a + 1) & 0x7FFF] << 8));
    if ((a >> 24) >= 0x08 && (a >> 24) <= 0x09) {
        const u32 off = a & 0x01FFFFFE;
        if (off + 1 < rom_.size()) return u16(rom_[off] | (rom_[off + 1] << 8));
    }
    return 0;
}

u32 Gba::wide_ram32(u32 a) const { return u32(wide_ram16(a)) | (u32(wide_ram16(a + 2)) << 16); }

u16 Gba::wide_level_entry(int bg, s32 tx, s32 ty) const {
    const WideLevel& L = wide_level_[bg];
    const u32 index = u32(L.layer) * kLayerEntries + u32((ty >> 1) * L.stride + (tx >> 1));
    const u16 block = wide_ram16(L.map + index * 2);
    const u32 quarter = u32(tx & 1) + u32(ty & 1) * 2;
    return wide_ram16(L.table + (u32(block) * 4 + quarter) * 2);
}

void Gba::wide_level_detect() {
    for (auto& L : wide_level_) L = WideLevel{};
    wide_scene_ = false;
    wide_main_bg_ = -1;
    wide_hud_prio_ = -1;
    if (!wide_margin_) return;
    const u16 dispcnt = u16(io_[0] | (io_[1] << 8));
    if ((dispcnt & 7) != 0) return;
    // The struct has sat at the same address in every field scene seen so far; if it is not
    // there, nothing widens.
    if (wide_ram32(kField + 0x08) != kFieldTag) return;
    const u32 maps = wide_ram32(kField + kMaps), table = wide_ram32(kField + kTable);
    if (!in_ewram(maps) || !in_ewram(table)) return;

    int bg_of_layer[3] = {-1, -1, -1};
    int first_bg = -1;

    for (int bg = 0; bg < 4; ++bg) {
        const u16 cnt = u16(io_[0x08 + 2 * bg] | (io_[0x09 + 2 * bg] << 8));
        const u32 sbb = (cnt >> 8) & 31;
        // A block layer sits at screen block 2 * layer and is 512x256.
        if (sbb > 4 || (sbb & 1) || ((cnt >> 14) & 3) != 1 || (cnt & 0x80)) continue;
        const int layer = int(sbb / 2);
        if (!(dispcnt & (0x100 << bg)) || bg_of_layer[layer] >= 0) continue;
        bg_of_layer[layer] = bg;
        WideLevel& L = wide_level_[bg];
        L.layer = layer;
        L.map = maps;
        L.table = table;
        L.stride = wide_ram16(kField + kStride + u32(layer) * 2);
        L.width = wide_ram16(kField + kWidth + u32(layer) * 2);
        L.height = wide_ram16(kField + kHeight + u32(layer) * 2);
        if (L.width <= 0 || L.height <= 0 || L.stride < L.width || L.stride * L.height > s32(kLayerEntries)) continue;
        if ((maps & 0x3FFFF) + (u32(layer) * kLayerEntries + u32(L.stride * L.height)) * 2 > 0x40000) continue;
        const s32 tiles_w = L.width * 2, tiles_h = L.height * 2;

        const u32 hofs = u32(io_[0x10 + 4 * bg] | (io_[0x11 + 4 * bg] << 8)) & 0x1FF;
        const u32 vofs = u32(io_[0x12 + 4 * bg] | (io_[0x13 + 4 * bg] << 8)) & 0x1FF;
        const u32 screen_base = sbb * 0x800;
        const s32 level_w = tiles_w * 8, level_h = tiles_h * 8;
        // Sample tiles spread over the visible area. The best-matching camera wins if at least
        // 15 of every 16 samples agree (animated tiles and doors can differ for a frame).
        static constexpr int kSx[] = {4, 36, 68, 100, 132, 164, 196, 228};
        static constexpr int kSy[] = {12, 60, 108, 148};
        auto score = [&](s32 cx, s32 cy) {
            int checked = 0, match = 0;
            for (int sy : kSy)
                for (int sx : kSx) {
                    const s32 ax = cx + sx, ay = cy + sy;
                    if (ax < 0 || ay < 0 || ax >= level_w || ay >= level_h) continue;
                    const u32 vx = u32(ax >> 3) & 63, vy = u32(ay >> 3) & 31;
                    const u32 off = screen_base + (vx >> 5) * 0x800 + vy * 64 + (vx & 31) * 2;
                    const u16 vram_e = u16(vram_[off] | (vram_[off + 1] << 8));
                    ++checked;
                    if (vram_e == wide_level_entry(bg, ax >> 3, ay >> 3)) ++match;
                }
            return checked >= 16 && match * 16 >= checked * 15 ? match : -1;
        };
        // Try the game's own camera for this layer first (it wins ties, which matters for a
        // mostly empty layer that matches in many places), then every other candidate.
        const s32 hint_x = s32(wide_ram16(kField + kCamX + u32(layer) * 2));
        const s32 hint_y = s32(wide_ram16(kField + kCamY + u32(layer) * 2));
        int best = -1;
        s32 best_x = 0, best_y = 0;
        if (u32(hint_x & 511) == hofs && u32(hint_y & 255) == (vofs & 255)) {
            best = score(hint_x, hint_y);
            best_x = hint_x;
            best_y = hint_y;
        }
        for (s32 cy = s32(vofs & 0xFF); cy < level_h; cy += 256) {
            for (s32 cx = s32(hofs); cx < level_w; cx += 512) {
                const int match = score(cx, cy);
                if (match > best) {
                    best = match;
                    best_x = cx;
                    best_y = cy;
                }
            }
        }
        if (best < 0) continue;
        L.tiles_w = tiles_w;
        L.tiles_h = tiles_h;
        L.cam_x = best_x - s32(hofs);
        L.cam_y = best_y - s32(vofs);
        L.mode = WideMode::Level;
        if (first_bg < 0 || layer == 1) first_bg = bg;
    }

    if (first_bg < 0) {
        for (auto& L : wide_level_) L.mode = WideMode::Vram;
        return;
    }
    wide_scene_ = true;
    // Side columns past the room's edge are black; the main layer (1) decides, else the
    // first layer that matched.
    wide_main_bg_ = first_bg;
    // Everything that is not a matched block layer (text boxes and the field HUD on BG0, a
    // block layer whose VRAM did not match) would show stale or wrapped VRAM: hide its sides.
    for (int bg = 0; bg < 4; ++bg)
        if (wide_level_[bg].mode != WideMode::Level) wide_level_[bg].mode = WideMode::Hide;
}

}  // namespace supersaga::gba
