// Memory map, IO registers, wait states and the battery save (SRAM or serial EEPROM).
#include "gba.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>

namespace supersaga::gba {

namespace {
inline u16 ld16(const u8* p) { return u16(p[0] | (p[1] << 8)); }
inline u32 ld32(const u8* p) { return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24); }
inline void st16(u8* p, u16 v) { p[0] = u8(v); p[1] = u8(v >> 8); }
inline void st32(u8* p, u32 v) { p[0] = u8(v); p[1] = u8(v >> 8); p[2] = u8(v >> 16); p[3] = u8(v >> 24); }
// Regions with a 16-bit (or 8-bit) bus need two accesses for a 32-bit transfer.
inline bool narrow_bus(u32 region) { return region == 2 || region == 5 || region == 6 || region >= 8; }
}  // namespace

u32 Gba::vram_offset(u32 a) const {
    u32 off = a & 0x1FFFF;
    if (off >= 0x18000) off -= 0x8000;
    return off;
}

u32 Gba::open_bus(u32 a) const {
    (void)a;
    return last_fetch_;
}

void Gba::update_waitstates() {
    u16 w = u16(io_[0x204] | (io_[0x205] << 8));
    static constexpr u32 kN[4] = {4, 3, 2, 8};
    u32 sram = 1 + kN[w & 3];
    u32 n0 = 1 + kN[(w >> 2) & 3], s0 = 1 + ((w & 0x10) ? 1 : 2);
    u32 n1 = 1 + kN[(w >> 5) & 3], s1 = 1 + ((w & 0x80) ? 1 : 4);
    u32 n2 = 1 + kN[(w >> 8) & 3], s2 = 1 + ((w & 0x400) ? 1 : 8);
    bool prefetch = w & 0x4000;
    wait_n_.fill(1);
    wait_s_.fill(1);
    wait_n_[2] = wait_s_[2] = 3;  // EWRAM
    wait_n_[8] = wait_n_[9] = n0;
    wait_s_[8] = wait_s_[9] = prefetch ? 1 : s0;
    wait_n_[0xA] = wait_n_[0xB] = n1;
    wait_s_[0xA] = wait_s_[0xB] = prefetch ? 1 : s1;
    wait_n_[0xC] = wait_n_[0xD] = n2;
    wait_s_[0xC] = wait_s_[0xD] = prefetch ? 1 : s2;
    wait_n_[0xE] = wait_n_[0xF] = wait_s_[0xE] = wait_s_[0xF] = sram;
}

// ---- reads ---------------------------------------------------------------------------------

u8 Gba::read8(u32 a) {
    u32 region = (a >> 24) & 15;
    cycles_ += wait_n_[region];
    switch (a >> 24) {
        case 0x00:
            if (a < 0x4000) return pc_ < 0x4000 ? bios_[a] : u8(bios_latch_ >> ((a & 3) * 8));
            return u8(open_bus(a) >> ((a & 3) * 8));
        case 0x02: return ewram_[a & 0x3FFFF];
        case 0x03: return iwram_[a & 0x7FFF];
        case 0x04: {
            if ((a & 0xFFFFFF) >= 0x400) return u8(open_bus(a) >> ((a & 3) * 8));
            u16 v = io_read16(a & 0x3FE);
            return u8(v >> ((a & 1) * 8));
        }
        case 0x05: return pal_[a & 0x3FF];
        case 0x06: return vram_[vram_offset(a)];
        case 0x07: return oam_[a & 0x3FF];
        case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: {
            u32 off = a & 0x01FFFFFF;
            if (off < rom_.size()) return rom_[off];
            return u8(((a >> 1) & 0xFFFF) >> ((a & 1) * 8));
        }
        case 0x0E: case 0x0F: return sram_[a & 0x7FFF];
        default: return u8(open_bus(a) >> ((a & 3) * 8));
    }
}

u16 Gba::read16(u32 a) {
    a &= ~1u;
    u32 region = (a >> 24) & 15;
    cycles_ += wait_n_[region];
    switch (a >> 24) {
        case 0x00:
            if (a < 0x4000) return pc_ < 0x4000 ? ld16(&bios_[a]) : u16(bios_latch_ >> ((a & 2) * 8));
            return u16(open_bus(a) >> ((a & 2) * 8));
        case 0x02: return ld16(&ewram_[a & 0x3FFFF]);
        case 0x03: return ld16(&iwram_[a & 0x7FFF]);
        case 0x04:
            if ((a & 0xFFFFFF) >= 0x400) return u16(open_bus(a) >> ((a & 2) * 8));
            return io_read16(a & 0x3FE);
        case 0x05: return ld16(&pal_[a & 0x3FF]);
        case 0x06: return ld16(&vram_[vram_offset(a)]);
        case 0x07: return ld16(&oam_[a & 0x3FF]);
        case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: {
            if (eeprom_at(a)) return eeprom_read_bit();
            u32 off = a & 0x01FFFFFF;
            if (off + 1 < rom_.size()) return ld16(&rom_[off]);
            return u16(a >> 1);
        }
        case 0x0E: case 0x0F: return u16(sram_[a & 0x7FFF] * 0x0101);
        default: return u16(open_bus(a) >> ((a & 2) * 8));
    }
}

u32 Gba::read32(u32 a) {
    a &= ~3u;
    u32 region = (a >> 24) & 15;
    cycles_ += wait_n_[region] + (narrow_bus(region) ? wait_s_[region] : 0);
    switch (a >> 24) {
        case 0x00:
            if (a < 0x4000) return pc_ < 0x4000 ? ld32(&bios_[a]) : bios_latch_;
            return open_bus(a);
        case 0x02: return ld32(&ewram_[a & 0x3FFFF]);
        case 0x03: return ld32(&iwram_[a & 0x7FFF]);
        case 0x04:
            if ((a & 0xFFFFFF) >= 0x400) return open_bus(a);
            return io_read16(a & 0x3FC) | (u32(io_read16((a & 0x3FC) + 2)) << 16);
        case 0x05: return ld32(&pal_[a & 0x3FF]);
        case 0x06: return ld32(&vram_[vram_offset(a)]);
        case 0x07: return ld32(&oam_[a & 0x3FF]);
        case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: {
            u32 off = a & 0x01FFFFFF;
            if (off + 3 < rom_.size()) return ld32(&rom_[off]);
            return ((a >> 1) & 0xFFFF) | ((((a + 2) >> 1) & 0xFFFF) << 16);
        }
        case 0x0E: case 0x0F: return sram_[a & 0x7FFF] * 0x01010101u;
        default: return open_bus(a);
    }
}

u16 Gba::fetch16(u32 a) {
    u32 region = (a >> 24) & 15;
    u32 saved = cycles_;
    u16 v = read16(a);
    cycles_ = saved + wait_s_[region];
    if (a < 0x4000) bios_latch_ = ld32(&bios_[a & 0x3FFC]);
    last_fetch_ = v | (u32(v) << 16);
    return v;
}

u32 Gba::fetch32(u32 a) {
    u32 region = (a >> 24) & 15;
    u32 saved = cycles_;
    u32 v = read32(a);
    cycles_ = saved + wait_s_[region] * (narrow_bus(region) ? 2 : 1);
    if (a < 0x4000) bios_latch_ = v;
    last_fetch_ = v;
    return v;
}

// ---- writes --------------------------------------------------------------------------------

void Gba::write8(u32 a, u8 v) {
    u32 region = (a >> 24) & 15;
    cycles_ += wait_n_[region];
    switch (a >> 24) {
        case 0x02: ewram_[a & 0x3FFFF] = v; return;
        case 0x03: iwram_[a & 0x7FFF] = v; return;
        case 0x04:
            if ((a & 0xFFFFFF) < 0x400) io_write8(a & 0x3FF, v);
            return;
        case 0x05: st16(&pal_[a & 0x3FE], u16(v * 0x0101)); return;
        case 0x06: {
            u32 off = vram_offset(a);
            bool bitmap = (io_[0] & 7) >= 3;
            if (off < (bitmap ? 0x14000u : 0x10000u)) st16(&vram_[off & ~1u], u16(v * 0x0101));
            return;
        }
        case 0x07: return;  // byte writes to OAM are ignored
        case 0x0E: case 0x0F:
            if (sram_[a & 0x7FFF] != v) { sram_[a & 0x7FFF] = v; sram_dirty_ = true; }
            return;
        default: return;
    }
}

void Gba::write16(u32 a, u16 v) {
    a &= ~1u;
    u32 region = (a >> 24) & 15;
    cycles_ += wait_n_[region];
    switch (a >> 24) {
        case 0x02: st16(&ewram_[a & 0x3FFFF], v); return;
        case 0x03: st16(&iwram_[a & 0x7FFF], v); return;
        case 0x04:
            if ((a & 0xFFFFFF) >= 0x400) return;
            if ((a & 0x3FE) >= 0xA0 && (a & 0x3FE) < 0xA8) { apu_fifo_push((a & 4) ? 1 : 0, v, 2); return; }
            io_write16(a & 0x3FE, v);
            return;
        case 0x05: st16(&pal_[a & 0x3FF], v); return;
        case 0x06: st16(&vram_[vram_offset(a)], v); return;
        case 0x07: st16(&oam_[a & 0x3FF], v); return;
        case 0x0D:
            if (eeprom_at(a)) eeprom_write_bit(v);
            return;
        case 0x0E: case 0x0F: write8(a, u8(v)); return;
        default: return;
    }
}

void Gba::write32(u32 a, u32 v) {
    a &= ~3u;
    u32 region = (a >> 24) & 15;
    cycles_ += wait_n_[region] + (narrow_bus(region) ? wait_s_[region] : 0);
    switch (a >> 24) {
        case 0x02: st32(&ewram_[a & 0x3FFFF], v); return;
        case 0x03: st32(&iwram_[a & 0x7FFF], v); return;
        case 0x04:
            if ((a & 0xFFFFFF) >= 0x400) return;
            if ((a & 0x3FC) == 0xA0 || (a & 0x3FC) == 0xA4) { apu_fifo_push((a & 4) ? 1 : 0, v, 4); return; }
            io_write16(a & 0x3FC, u16(v));
            io_write16((a & 0x3FC) + 2, u16(v >> 16));
            return;
        case 0x05: st32(&pal_[a & 0x3FF], v); return;
        case 0x06: st32(&vram_[vram_offset(a)], v); return;
        case 0x07: st32(&oam_[a & 0x3FF], v); return;
        case 0x0E: case 0x0F: write8(a, u8(v)); return;
        default: return;
    }
}

// ---- IO registers --------------------------------------------------------------------------

u16 Gba::io_read16(u32 a) {
    switch (a) {
        case 0x004: {
            u16 v = u16((io_[4] | (io_[5] << 8)) & 0xFF38);
            if (vcount_ >= 160 && vcount_ != 227) v |= 1;
            if (hblank_) v |= 2;
            if (vcount_ == u32(io_[5])) v |= 4;
            return v;
        }
        case 0x006: return u16(vcount_);
        case 0x100: case 0x104: case 0x108: case 0x10C: return timer_[(a - 0x100) / 4].counter;
        case 0x130: return u16(~keys_pressed_ & 0x3FF);
        case 0x084: {
            u16 v = io_[0x84] & 0x80;
            if (sq_[0].on) v |= 1;
            if (sq_[1].on) v |= 2;
            if (wave_.on) v |= 4;
            if (noise_.on) v |= 8;
            return v;
        }
        // Write-only DMA address/count registers.
        case 0x0B0: case 0x0B2: case 0x0B4: case 0x0B6: case 0x0B8:
        case 0x0BC: case 0x0BE: case 0x0C0: case 0x0C2: case 0x0C4:
        case 0x0C8: case 0x0CA: case 0x0CC: case 0x0CE: case 0x0D0:
        case 0x0D4: case 0x0D6: case 0x0D8: case 0x0DA: case 0x0DC:
            return 0;
        case 0x0BA: return dma_[0].control;
        case 0x0C6: return dma_[1].control;
        case 0x0D2: return dma_[2].control;
        case 0x0DE: return dma_[3].control;
        default: return ld16(&io_[a]);
    }
}

void Gba::io_write16(u32 a, u16 v) {
    u16 old = ld16(&io_[a]);
    switch (a) {
        case 0x004: st16(&io_[a], u16((old & 0x0007) | (v & 0xFF38))); return;
        case 0x006: return;
        case 0x028: case 0x02A: case 0x02C: case 0x02E:
            st16(&io_[a], v);
            ppu_write_bg_ref(0);
            return;
        case 0x038: case 0x03A: case 0x03C: case 0x03E:
            st16(&io_[a], v);
            ppu_write_bg_ref(1);
            return;
        case 0x0BA: st16(&io_[a], v); dma_write_control(0, v); return;
        case 0x0C6: st16(&io_[a], v); dma_write_control(1, v); return;
        case 0x0D2: st16(&io_[a], v); dma_write_control(2, v); return;
        case 0x0DE: st16(&io_[a], v); dma_write_control(3, v); return;
        case 0x100: case 0x104: case 0x108: case 0x10C:
            timer_[(a - 0x100) / 4].reload = v;
            st16(&io_[a], v);
            return;
        case 0x102: case 0x106: case 0x10A: case 0x10E:
            st16(&io_[a], v & 0xC7);
            timer_write_control((a - 0x102) / 4, v);
            return;
        case 0x130: return;
        case 0x202: st16(&io_[a], u16(old & ~v)); return;
        case 0x204: st16(&io_[a], v & 0x5FFF); update_waitstates(); return;
        case 0x208: st16(&io_[a], v & 1); return;
        case 0x300:
            io_[0x300] = u8(v);
            halted_ = true;  // the high byte is HALTCNT
            return;
        default: break;
    }
    if (a >= 0x060 && a < 0x0A0) {
        st16(&io_[a], v);
        apu_write(a, v, old);
        return;
    }
    st16(&io_[a], v);
}

void Gba::io_write8(u32 a, u8 v) {
    if (a == 0x202 || a == 0x203) {
        io_[a] &= u8(~v);
        return;
    }
    if (a == 0x301) { halted_ = true; return; }
    if (a == 0x300) { io_[a] = v; return; }
    if (a >= 0x0A0 && a < 0x0A8) { apu_fifo_push(a >= 0xA4 ? 1 : 0, v, 1); return; }
    u32 h = a & ~1u;
    u32 sh = (a & 1) * 8;
    u16 cur = ld16(&io_[h]);
    if (h == 0x100 || h == 0x104 || h == 0x108 || h == 0x10C) cur = timer_[(h - 0x100) / 4].reload;
    if (h == 0x0BA || h == 0x0C6 || h == 0x0D2 || h == 0x0DE) cur = dma_[(h - 0x0BA) / 12].control;
    io_write16(h, u16((cur & ~(0xFF << sh)) | (v << sh)));
}

// ---- save ----------------------------------------------------------------------------------

void Gba::load_save() {
    sram_.fill(0xFF);
    eeprom_in_count_ = 0;
    eeprom_read_pos_ = 68;
    std::ifstream f(save_path_, std::ios::binary);
    if (f) {
        f.read(reinterpret_cast<char*>(sram_.data()), sram_.size());
        if (eeprom_ && f.gcount() == 0x200) eeprom_size_ = 0x200;
    }
    sram_dirty_ = false;
}

void Gba::flush_save() {
    if (!sram_dirty_ || save_path_.empty()) return;
    std::ofstream f(save_path_, std::ios::binary | std::ios::trunc);
    if (!f) {
        std::cerr << "warning: could not write save file " << save_path_ << "\n";
        return;
    }
    f.write(reinterpret_cast<const char*>(sram_.data()), std::streamsize(save_size()));
    sram_dirty_ = false;
    sram_dirty_frames_ = 0;
}

// ---- serial EEPROM ---------------------------------------------------------------------------
// 512 bytes (6-bit block address) or 8 KiB (14-bit, of which 10 are used), in 64-bit blocks.
// Commands arrive one bit per halfword, MSB first, as one DMA3 transfer each:
//   read request  "11" + address + "0"            (9 or 17 bits), then 68 bits are read back:
//                 4 dummy bits and the block's 64 bits
//   write         "10" + address + 64 bits + "0"  (73 or 81 bits)
// The DMA length gives the address width. Writes finish at once, so the ready bit is always 1.
// Blocks are stored in file order: bit 7 of byte 0 is the first data bit (the usual .sav layout).

bool Gba::eeprom_at(u32 a) const {
    return eeprom_ && (a >> 24) == 0x0D && (rom_.size() <= 0x1000000 || (a & 0xFFFFFF) >= 0xFFFF00);
}

u16 Gba::eeprom_read_bit() {
    if (eeprom_read_pos_ >= 68) return 1;
    u32 pos = eeprom_read_pos_++;
    if (pos < 4) return 0;
    pos -= 4;
    return u16((sram_[(eeprom_read_addr_ + pos / 8) & 0x1FFF] >> (7 - pos % 8)) & 1);
}

void Gba::eeprom_write_bit(u16 v) {
    if (eeprom_in_count_ < eeprom_in_.size()) eeprom_in_[eeprom_in_count_] = u8(v & 1);
    ++eeprom_in_count_;
}

void Gba::eeprom_command(u32 bits) {
    const u32 n = std::min<u32>(eeprom_in_count_, u32(eeprom_in_.size()));
    eeprom_in_count_ = 0;
    if (bits != n || n < 9 || eeprom_in_[0] != 1) return;
    auto field = [&](u32 from, u32 width) {
        u32 v = 0;
        for (u32 i = 0; i < width; ++i) v = (v << 1) | eeprom_in_[from + i];
        return v;
    };
    const bool read = eeprom_in_[1] == 1;
    const u32 abits = read ? n - 3 : n - 67;
    if (abits != 6 && abits != 14) return;
    eeprom_size_ = abits == 6 ? 0x200 : 0x2000;
    const u32 addr = (field(2, abits) & (abits == 6 ? 0x3F : 0x3FF)) * 8;
    if (read) {
        eeprom_read_addr_ = addr;
        eeprom_read_pos_ = 0;
        return;
    }
    for (u32 i = 0; i < 8; ++i) {
        const u8 byte = u8(field(2 + abits + i * 8, 8));
        if (sram_[addr + i] != byte) { sram_[addr + i] = byte; sram_dirty_ = true; }
    }
}

}  // namespace supersaga::gba
