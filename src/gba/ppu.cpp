// Scanline renderer: modes 0-5, text/affine/bitmap BGs, sprites, windows, blending, mosaic.
#include "gba.h"

#include <algorithm>

namespace supersaga::gba {

namespace {
inline u16 rd16(const u8* p) { return u16(p[0] | (p[1] << 8)); }

constexpr int kObjSize[3][4][2] = {
    {{8, 8}, {16, 16}, {32, 32}, {64, 64}},  // square
    {{16, 8}, {32, 8}, {32, 16}, {64, 32}},  // horizontal
    {{8, 16}, {8, 32}, {16, 32}, {32, 64}},  // vertical
};

inline u16 blend(u16 a, u16 b, u32 eva, u32 evb) {
    u32 r = std::min<u32>(31, ((a & 31) * eva + (b & 31) * evb) >> 4);
    u32 g = std::min<u32>(31, (((a >> 5) & 31) * eva + ((b >> 5) & 31) * evb) >> 4);
    u32 bl = std::min<u32>(31, (((a >> 10) & 31) * eva + ((b >> 10) & 31) * evb) >> 4);
    return u16(r | (g << 5) | (bl << 10));
}
inline u16 brighten(u16 c, u32 evy) {
    u32 r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    r += ((31 - r) * evy) >> 4;
    g += ((31 - g) * evy) >> 4;
    b += ((31 - b) * evy) >> 4;
    return u16(r | (g << 5) | (b << 10));
}
inline u16 darken(u16 c, u32 evy) {
    u32 r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    r -= (r * evy) >> 4;
    g -= (g * evy) >> 4;
    b -= (b * evy) >> 4;
    return u16(r | (g << 5) | (b << 10));
}
}  // namespace

void Gba::ppu_write_bg_ref(int i) {
    u32 base = 0x28 + 16 * u32(i);
    u32 x = rd16(&io_[base]) | (u32(rd16(&io_[base + 2])) << 16);
    u32 y = rd16(&io_[base + 4]) | (u32(rd16(&io_[base + 6])) << 16);
    bg_ref_x_[i] = s32(x << 4) >> 4;  // 28-bit signed 20.8 fixed point
    bg_ref_y_[i] = s32(y << 4) >> 4;
}

void Gba::ppu_advance(u32 cycles) {
    line_cycle_ += cycles;
    for (;;) {
        if (!hblank_ && line_cycle_ >= 960) {
            hblank_ = true;
            if (vcount_ < 160) {
                ppu_render_line(int(vcount_));
                dma_trigger(2);
            }
            if (io_[4] & 0x10) request_irq(1);
            continue;
        }
        if (line_cycle_ >= kCyclesPerLine) {
            line_cycle_ -= kCyclesPerLine;
            hblank_ = false;
            if (++vcount_ == kLinesPerFrame) vcount_ = 0;
            if (vcount_ == 160) {
                frame_done_ = true;
                if (io_[4] & 0x08) request_irq(0);
                dma_trigger(1);
                ppu_write_bg_ref(0);
                ppu_write_bg_ref(1);
            }
            if (vcount_ == io_[5] && (io_[4] & 0x20)) request_irq(2);
            continue;
        }
        break;
    }
}

void Gba::ppu_render_text_bg(int bg, int y, u16* color, u8* opaque) {
    u16 cnt = rd16(&io_[0x08 + 2 * bg]);
    u32 hofs = rd16(&io_[0x10 + 4 * bg]) & 0x1FF;
    u32 vofs = rd16(&io_[0x12 + 4 * bg]) & 0x1FF;
    u32 char_base = ((cnt >> 2) & 3) * 0x4000;
    u32 screen_base = ((cnt >> 8) & 31) * 0x800;
    bool c256 = cnt & 0x80;
    u32 size = (cnt >> 14) & 3;
    u32 wmask = (size & 1) ? 511 : 255, hmask = (size & 2) ? 511 : 255;
    bool mosaic = cnt & 0x40;
    u32 mh = (io_[0x4C] & 15) + 1, mv = (io_[0x4C] >> 4) + 1;
    u32 yy = mosaic ? u32(y) - u32(y) % mv : u32(y);
    u32 sy = (yy + vofs) & hmask;
    const int m = wide_margin_, width = kScreenWidth + 2 * m;
    for (int x = 0; x < width; ++x) {
        const s32 px_screen = x - m;
        s32 xx = px_screen;
        if (mosaic) xx -= ((px_screen % s32(mh)) + s32(mh)) % s32(mh);
        u32 sx = (u32(xx) + hofs) & wmask;
        u32 block = (sx >> 8) + (sy >> 8) * ((size & 1) ? 2 : 1);
        u32 map = screen_base + block * 0x800 + ((sy & 255) >> 3) * 64 + ((sx & 255) >> 3) * 2;
        if (map >= 0x10000) { opaque[x] = 0; continue; }
        u16 e = rd16(&vram_[map]);
        if (m && (x < m || x >= m + kScreenWidth) && wide_level_[bg].mode != WideMode::Vram) {
            // Outside the original view the VRAM ring holds stale columns: rebuild the
            // entry from the level layout in RAM instead (wide_level.cpp), or hide it.
            const WideLevel& L = wide_level_[bg];
            const s32 ax = s32(xx) + s32(hofs) + L.cam_x;
            const s32 ay = s32(yy) + s32(vofs) + L.cam_y;
            if (L.mode == WideMode::Level && ax >= 0 && ay >= 0 && ax < L.tiles_w * 8 && ay < L.tiles_h * 8) {
                e = wide_level_entry(bg, ax >> 3, ay >> 3);
            } else {
                opaque[x] = 0;  // hidden, or beyond the level's edge
                continue;
            }
        }
        u32 tile = e & 0x3FF, px = sx & 7, py = sy & 7;
        if (e & 0x400) px = 7 - px;
        if (e & 0x800) py = 7 - py;
        u32 idx;
        if (c256) {
            u32 addr = char_base + tile * 64 + py * 8 + px;
            idx = addr < 0x10000 ? vram_[addr] : 0;
            if (idx) color[x] = rd16(&pal_[idx * 2]);
        } else {
            u32 addr = char_base + tile * 32 + py * 4 + px / 2;
            u32 b = addr < 0x10000 ? vram_[addr] : 0;
            idx = (px & 1) ? b >> 4 : b & 15;
            if (idx) color[x] = rd16(&pal_[((e >> 12) * 16 + idx) * 2]);
        }
        opaque[x] = idx != 0;
    }
}

void Gba::ppu_render_affine_bg(int bg, u16* color, u8* opaque) {
    u16 cnt = rd16(&io_[0x08 + 2 * bg]);
    int i = bg - 2;
    s32 pa = s16(rd16(&io_[0x20 + 16 * i])), pc = s16(rd16(&io_[0x24 + 16 * i]));
    u32 char_base = ((cnt >> 2) & 3) * 0x4000;
    u32 screen_base = ((cnt >> 8) & 31) * 0x800;
    s32 size = 128 << ((cnt >> 14) & 3);
    bool wrap = cnt & 0x2000;
    s32 rx = bg_ref_x_[i], ry = bg_ref_y_[i];
    const int m = wide_margin_, width = kScreenWidth + 2 * m;
    for (int x = 0; x < width; ++x) {
        s32 tx = (rx + pa * (x - m)) >> 8, ty = (ry + pc * (x - m)) >> 8;
        if (wrap) {
            tx &= size - 1;
            ty &= size - 1;
        } else if (tx < 0 || ty < 0 || tx >= size || ty >= size) {
            opaque[x] = 0;
            continue;
        }
        u32 tile = vram_[(screen_base + u32(ty >> 3) * u32(size / 8) + u32(tx >> 3)) & 0xFFFF];
        u32 idx = vram_[(char_base + tile * 64 + u32(ty & 7) * 8 + u32(tx & 7)) & 0xFFFF];
        opaque[x] = idx != 0;
        if (idx) color[x] = rd16(&pal_[idx * 2]);
    }
}

void Gba::ppu_render_bitmap(int mode, u16* color, u8* opaque) {
    s32 pa = s16(rd16(&io_[0x20])), pc = s16(rd16(&io_[0x24]));
    s32 rx = bg_ref_x_[0], ry = bg_ref_y_[0];
    u32 frame = (io_[0] & 0x10) ? 0xA000 : 0;
    s32 w = mode == 5 ? 160 : 240, h = mode == 5 ? 128 : 160;
    const int m = wide_margin_, width = kScreenWidth + 2 * m;
    for (int x = 0; x < width; ++x) {
        s32 tx = (rx + pa * (x - m)) >> 8, ty = (ry + pc * (x - m)) >> 8;
        if (tx < 0 || ty < 0 || tx >= w || ty >= h) { opaque[x] = 0; continue; }
        if (mode == 3) {
            color[x] = rd16(&vram_[u32(ty * 240 + tx) * 2]);
            opaque[x] = 1;
        } else if (mode == 4) {
            u32 idx = vram_[frame + u32(ty * 240 + tx)];
            opaque[x] = idx != 0;
            if (idx) color[x] = rd16(&pal_[idx * 2]);
        } else {
            color[x] = rd16(&vram_[frame + u32(ty * 160 + tx) * 2]);
            opaque[x] = 1;
        }
    }
}

void Gba::ppu_render_objects(int y) {
    obj_prio_.fill(4);
    obj_semi_.fill(0);
    obj_window_.fill(0);
    u16 dispcnt = rd16(&io_[0]);
    bool map1d = dispcnt & 0x40;
    bool bitmap = (dispcnt & 7) >= 3;
    u32 mh = (io_[0x4D] & 15) + 1, mv = (io_[0x4D] >> 4) + 1;
    for (int i = 0; i < 128; ++i) {
        u16 a0 = rd16(&oam_[i * 8]), a1 = rd16(&oam_[i * 8 + 2]), a2 = rd16(&oam_[i * 8 + 4]);
        bool affine = a0 & 0x100;
        if (!affine && (a0 & 0x200)) continue;
        int mode = (a0 >> 10) & 3;
        if (mode == 3) continue;
        int shape = a0 >> 14, sz = a1 >> 14;
        if (shape == 3) continue;
        int w = kObjSize[shape][sz][0], h = kObjSize[shape][sz][1];
        bool dbl = affine && (a0 & 0x200);
        int bw = dbl ? w * 2 : w, bh = dbl ? h * 2 : h;
        int oy = a0 & 0xFF;
        if (oy >= 160) oy -= 256;
        int line = y - oy;
        if (line < 0 || line >= bh) continue;
        bool mosaic = a0 & 0x1000;
        if (mosaic) line -= line % int(mv);
        int ox = a1 & 0x1FF;
        if (ox >= 240 + wide_margin_) ox -= 512;  // 9-bit X wraps; wide mode keeps more on the right
        bool c256 = a0 & 0x2000;
        u32 tile = a2 & 0x3FF, prio = (a2 >> 10) & 3, palbank = a2 >> 12;
        s32 pa = 256, pb = 0, pc = 0, pd = 256;
        if (affine) {
            u32 p = ((a1 >> 9) & 31) * 32;
            pa = s16(rd16(&oam_[p + 6]));
            pb = s16(rd16(&oam_[p + 14]));
            pc = s16(rd16(&oam_[p + 22]));
            pd = s16(rd16(&oam_[p + 30]));
        }
        // HUD objects (and objects the game parks just past the right edge) stay in the
        // original view, which is all the game expects to be seen.
        const bool clip240 = wide_scene_ && int(prio) == wide_hud_prio_;
        for (int bx = 0; bx < bw; ++bx) {
            int sx = ox + bx + wide_margin_;  // index into the (possibly wide) line
            if (sx < 0 || sx >= kScreenWidth + 2 * wide_margin_) continue;
            if (clip240 && (sx < wide_margin_ || sx >= wide_margin_ + kScreenWidth)) continue;
            int tx, ty;
            int mbx = mosaic ? bx - bx % int(mh) : bx;
            if (affine) {
                int cx = mbx - bw / 2, cy = line - bh / 2;
                tx = ((pa * cx + pb * cy) >> 8) + w / 2;
                ty = ((pc * cx + pd * cy) >> 8) + h / 2;
                if (tx < 0 || ty < 0 || tx >= w || ty >= h) continue;
            } else {
                tx = mbx;
                ty = line;
                if (a1 & 0x1000) tx = w - 1 - tx;
                if (a1 & 0x2000) ty = h - 1 - ty;
            }
            u32 tx8 = u32(tx >> 3), ty8 = u32(ty >> 3);
            u32 t, idx;
            u16 col;
            if (c256) {
                t = map1d ? tile + (ty8 * u32(w / 8) + tx8) * 2 : (tile & ~1u) + ty8 * 32 + tx8 * 2;
                t &= 0x3FF;
                if (bitmap && t < 512) continue;
                idx = vram_[0x10000 + t * 32 + u32(ty & 7) * 8 + u32(tx & 7)];
                col = rd16(&pal_[0x200 + idx * 2]);
            } else {
                t = map1d ? tile + ty8 * u32(w / 8) + tx8 : tile + ty8 * 32 + tx8;
                t &= 0x3FF;
                if (bitmap && t < 512) continue;
                u32 b = vram_[0x10000 + t * 32 + u32(ty & 7) * 4 + u32(tx & 7) / 2];
                idx = (tx & 1) ? b >> 4 : b & 15;
                col = rd16(&pal_[0x200 + (palbank * 16 + idx) * 2]);
            }
            if (!idx) continue;
            if (mode == 2) { obj_window_[sx] = 1; continue; }
            if (prio < obj_prio_[sx]) {
                obj_color_[sx] = col;
                obj_prio_[sx] = u8(prio);
                obj_semi_[sx] = mode == 1;
            }
        }
    }
}

void Gba::ppu_render_line(int y) {
    if (y == 0) wide_level_detect();
    u16 dispcnt = rd16(&io_[0]);
    const int m = wide_margin_, width = kScreenWidth + 2 * m;
    u16* out = m ? &wide_frame_[std::size_t(y) * std::size_t(width)] : &frame_[y * kScreenWidth];
    auto finish = [&] {
        if (m) std::copy(out + m, out + m + kScreenWidth, &frame_[y * kScreenWidth]);
    };
    auto advance_refs = [&] {
        for (int i = 0; i < 2; ++i) {
            bg_ref_x_[i] += s16(rd16(&io_[0x22 + 16 * i]));  // PB
            bg_ref_y_[i] += s16(rd16(&io_[0x26 + 16 * i]));  // PD
        }
    };
    if (dispcnt & 0x80) {
        std::fill(out, out + width, 0x7FFF);
        advance_refs();
        finish();
        return;
    }
    int mode = dispcnt & 7;
    bool active[4] = {false, false, false, false};
    for (int bg = 0; bg < 4; ++bg) {
        bool on = dispcnt & (0x100 << bg);
        switch (mode) {
            case 0: active[bg] = on; break;
            case 1: active[bg] = on && bg <= 2; break;
            case 2: active[bg] = on && bg >= 2; break;
            default: active[bg] = on && bg == 2; break;
        }
    }
    for (int bg = 0; bg < 4; ++bg) {
        if (!active[bg]) continue;
        u16* col = bg_color_[bg].data();
        u8* opq = bg_opaque_[bg].data();
        if (mode >= 3) ppu_render_bitmap(mode, col, opq);
        else if (mode == 0 || (mode == 1 && bg < 2)) ppu_render_text_bg(bg, y, col, opq);
        else ppu_render_affine_bg(bg, col, opq);
    }
    if (dispcnt & 0x1000) ppu_render_objects(y);
    else { obj_prio_.fill(4); obj_window_.fill(0); }
    advance_refs();

    // Widescreen policy (wide_level.cpp decides it per frame). Only gameplay widens; every
    // other screen (title, menus, cutscenes) keeps black sides. In gameplay each background
    // fills the sides as wide_level_[bg].mode says; affine layers are left out.
    const bool wide_scene = m && wide_scene_;
    if (m) {
        auto is_text = [&](int bg) { return mode == 0 || (mode == 1 && bg < 2); };
        for (int bg = 0; bg < 4; ++bg) {
            if (!active[bg]) continue;
            if (wide_scene && is_text(bg)) continue;
            std::fill(bg_opaque_[bg].begin(), bg_opaque_[bg].begin() + m, u8(0));
            std::fill(bg_opaque_[bg].begin() + m + kScreenWidth, bg_opaque_[bg].begin() + width, u8(0));
        }
    }

    // Side columns past the edge of the room (main level layer) stay black.
    auto outside_level = [&](int x) {
        if (wide_main_bg_ < 0) return false;
        const WideLevel& L = wide_level_[wide_main_bg_];
        const s32 ax = s32(x - m) + s32(rd16(&io_[0x10 + 4 * wide_main_bg_]) & 0x1FF) + L.cam_x;
        const s32 ay = s32(y) + s32(rd16(&io_[0x12 + 4 * wide_main_bg_]) & 0x1FF) + L.cam_y;
        return ax < 0 || ay < 0 || ax >= L.tiles_w * 8 || ay >= L.tiles_h * 8;
    };

    u32 bgprio[4];
    for (int bg = 0; bg < 4; ++bg) bgprio[bg] = io_[0x08 + 2 * bg] & 3;

    bool win0 = dispcnt & 0x2000, win1 = dispcnt & 0x4000, objwin = dispcnt & 0x8000;
    bool any_win = win0 || win1 || objwin;
    auto inside = [&](int w, int x) {
        u16 h = rd16(&io_[0x40 + 2 * w]), v = rd16(&io_[0x44 + 2 * w]);
        int x1 = h >> 8, x2 = h & 0xFF, y1 = v >> 8, y2 = v & 0xFF;
        if (x2 > 240) x2 = 240;
        if (y2 > 228) y2 = 228;
        bool ix = x1 <= x2 ? (x >= x1 && x < x2) : (x >= x1 || x < x2);
        bool iy = y1 <= y2 ? (y >= y1 && y < y2) : (y >= y1 || y < y2);
        return ix && iy;
    };
    u8 m_win0 = io_[0x48] & 0x3F, m_win1 = io_[0x49] & 0x3F, m_out = io_[0x4A] & 0x3F, m_obj = io_[0x4B] & 0x3F;

    u16 bld = rd16(&io_[0x50]);
    u32 bmode = (bld >> 6) & 3;
    u32 eva = std::min<u32>(16, io_[0x52] & 31), evb = std::min<u32>(16, io_[0x53] & 31);
    u32 evy = std::min<u32>(16, io_[0x54] & 31);
    u16 backdrop = rd16(&pal_[0]);

    for (int x = 0; x < width; ++x) {
        if (m && (x < m || x >= m + kScreenWidth)) {
            if (!wide_scene || outside_level(x)) {
                out[x] = 0;
                continue;
            }
        }
        u8 mask = 0x3F;
        if (any_win) {
            // Window edges are given in 0..240; the extra columns reuse the nearest edge.
            const int wx = std::clamp(x - m, 0, kScreenWidth - 1);
            if (win0 && inside(0, wx)) mask = m_win0;
            else if (win1 && inside(1, wx)) mask = m_win1;
            else if (objwin && obj_window_[x]) mask = m_obj;
            else mask = m_out;
        }
        int layer[2] = {5, 5};
        u16 col[2] = {backdrop, backdrop};
        int found = 0;
        for (u32 p = 0; p < 4 && found < 2; ++p) {
            if ((mask & 0x10) && obj_prio_[x] == p) {
                layer[found] = 4;
                col[found] = obj_color_[x];
                ++found;
            }
            for (int bg = 0; bg < 4 && found < 2; ++bg) {
                if (active[bg] && (mask & (1 << bg)) && bgprio[bg] == p && bg_opaque_[bg][x]) {
                    layer[found] = bg;
                    col[found] = bg_color_[bg][x];
                    ++found;
                }
            }
        }
        u16 c = col[0];
        if (mask & 0x20) {
            bool second_ok = bld & (0x100 << layer[1]);
            if (layer[0] == 4 && obj_semi_[x] && second_ok) {
                c = blend(col[0], col[1], eva, evb);
            } else if (bld & (1 << layer[0])) {
                if (bmode == 1 && second_ok) c = blend(col[0], col[1], eva, evb);
                else if (bmode == 2) c = brighten(col[0], evy);
                else if (bmode == 3) c = darken(col[0], evy);
            }
        }
        out[x] = c;
    }
    finish();
}

void Gba::set_wide_margin(int margin) {
    margin = std::clamp(margin, 0, kMaxWideMargin);
    if (margin == wide_margin_) return;
    wide_margin_ = margin;
    if (margin) wide_frame_.assign(std::size_t(kScreenWidth + 2 * margin) * kScreenHeight, 0);
    else wide_frame_.clear();
}

}  // namespace supersaga::gba
