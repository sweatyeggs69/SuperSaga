// Top level: construction, reset, frame loop/scheduler, interrupts, DMA and timers.
#include "gba.h"

#include <algorithm>
#include <cstdio>
#include <iostream>

namespace supersaga::gba {

namespace {
inline u16 io16(const std::array<u8, 0x400>& io, u32 a) { return u16(io[a] | (io[a + 1] << 8)); }
inline u32 io32(const std::array<u8, 0x400>& io, u32 a) { return io16(io, a) | (u32(io16(io, a + 2)) << 16); }
}  // namespace

Gba::Gba(std::vector<u8> rom, std::filesystem::path save_path)
    : rom_(std::move(rom)), save_path_(std::move(save_path)) {
    // Nintendo's save libraries leave their name in the ROM; EEPROM_Vnnn means a serial EEPROM.
    static constexpr char kEeprom[] = "EEPROM_V";
    eeprom_ = std::search(rom_.begin(), rom_.end(), kEeprom, kEeprom + 8) != rom_.end();
    reset();
}

Gba::~Gba() { flush_save(); }

void Gba::use_bios(std::vector<u8> bios) {
    if (bios.size() != bios_.size()) return;
    std::copy(bios.begin(), bios.end(), bios_.begin());
    real_bios_ = true;
}

void Gba::reset() {
    ewram_.fill(0);
    iwram_.fill(0);
    io_.fill(0);
    pal_.fill(0);
    vram_.fill(0);
    oam_.fill(0);
    if (!real_bios_) bios_install();
    dma_ = {};
    timer_ = {};
    keys_pressed_ = 0;
    line_cycle_ = 0;
    vcount_ = 0;
    hblank_ = false;
    bg_ref_x_.fill(0);
    bg_ref_y_.fill(0);
    frame_.fill(0);
    // IO state left behind by the BIOS boot.
    io_[0x88] = 0x00;
    io_[0x89] = 0x02;  // SOUNDBIAS = 0x200
    io_[0x300] = 1;    // POSTFLG
    io_[0x00] = 0x80;  // DISPCNT: forced blank
    io_[0x20] = 0x00; io_[0x21] = 0x01;  // BG2PA = 0x100
    io_[0x26] = 0x00; io_[0x27] = 0x01;  // BG2PD
    io_[0x30] = 0x00; io_[0x31] = 0x01;  // BG3PA
    io_[0x36] = 0x00; io_[0x37] = 0x01;  // BG3PD
    update_waitstates();
    apu_reset();
    cpu_reset();
    load_save();
}

void Gba::set_keys(u16 pressed) {
    keys_pressed_ = pressed & 0x3FF;
    u16 cnt = io16(io_, 0x132);
    if (cnt & 0x4000) {
        u16 mask = cnt & 0x3FF;
        bool hit = (cnt & 0x8000) ? (keys_pressed_ & mask) == mask : (keys_pressed_ & mask) != 0;
        if (hit) request_irq(12);
    }
}

// ---- scheduler -----------------------------------------------------------------------------

void Gba::tick(u32 cycles) {
    timers_advance(cycles);
    apu_advance(cycles);
    ppu_advance(cycles);
}

u32 Gba::cycles_until_event() const {
    u32 c = hblank_ ? kCyclesPerLine - line_cycle_ : 960 - line_cycle_;
    c = std::min(c, timer_cycles_to_overflow());
    c = std::min(c, 512 - apu_sample_acc_);
    return std::max<u32>(c, 1);
}

void Gba::run_frame() {
    frame_done_ = false;
    while (!frame_done_) {
        if (halted_) {
            u16 ie = io16(io_, 0x200), iflag = io16(io_, 0x202);
            if (ie & iflag) {
                halted_ = false;
            } else {
                tick(cycles_until_event());
                continue;
            }
        }
        cpu_step();
        tick(std::max<u32>(cycles_, 1));
    }
    ++frame_count_;
    if (sram_dirty_ && ++sram_dirty_frames_ >= 60) flush_save();
}

std::string Gba::debug_state() const {
    char buf[512];
    std::snprintf(buf, sizeof buf,
                  "pc=%08X cpsr=%08X %s r0=%08X r1=%08X r2=%08X r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X "
                  "sp=%08X lr=%08X vcount=%u IE=%04X IF=%04X IME=%u halted=%d frame=%llu",
                  pc_, cpsr_, (cpsr_ & 0x20) ? "T" : "A", r_[0], r_[1], r_[2], r_[3], r_[4], r_[5], r_[6],
                  r_[7], r_[13], r_[14], vcount_, io16(io_, 0x200), io16(io_, 0x202), io_[0x208] & 1,
                  halted_ ? 1 : 0, (unsigned long long)frame_count_);
    return buf;
}

// ---- interrupts ----------------------------------------------------------------------------

void Gba::request_irq(int bit) {
    u16 v = io16(io_, 0x202) | u16(1u << bit);
    io_[0x202] = u8(v);
    io_[0x203] = u8(v >> 8);
}

// ---- DMA -----------------------------------------------------------------------------------

void Gba::dma_write_control(int ch, u16 v) {
    Dma& d = dma_[ch];
    bool was = d.control & 0x8000;
    d.control = v;
    if (!was && (v & 0x8000)) {
        u32 base = 0xB0 + 12 * ch;
        d.src = io32(io_, base) & (ch == 0 ? 0x07FFFFFF : 0x0FFFFFFF);
        d.dst = io32(io_, base + 4) & (ch == 3 ? 0x0FFFFFFF : 0x07FFFFFF);
        d.count = io16(io_, base + 8) & (ch == 3 ? 0xFFFF : 0x3FFF);
        if (d.count == 0) d.count = ch == 3 ? 0x10000 : 0x4000;
        if (((v >> 12) & 3) == 0) dma_run(ch);
    }
}

void Gba::dma_trigger(int timing) {
    for (int ch = 0; ch < 4; ++ch) {
        const Dma& d = dma_[ch];
        if ((d.control & 0x8000) && int((d.control >> 12) & 3) == timing) dma_run(ch);
    }
}

void Gba::dma_fifo_request(int fifo) {
    u32 dest = 0x040000A0 + 4 * u32(fifo);
    for (int ch = 1; ch <= 2; ++ch) {
        const Dma& d = dma_[ch];
        if ((d.control & 0x8000) && ((d.control >> 12) & 3) == 3 && d.dst == dest) dma_run(ch);
    }
}

void Gba::dma_run(int ch) {
    Dma& d = dma_[ch];
    u16 c = d.control;
    int timing = (c >> 12) & 3;
    bool fifo = timing == 3 && (ch == 1 || ch == 2);
    bool word = (c & 0x400) || fifo;
    u32 count = fifo ? 4 : d.count;
    int dctl = (c >> 5) & 3, sctl = (c >> 7) & 3;
    s32 step = word ? 4 : 2;
    s32 dstep = fifo ? 0 : (dctl == 0 || dctl == 3) ? step : dctl == 1 ? -step : 0;
    s32 sstep = sctl == 0 ? step : sctl == 1 ? -step : 0;
    u32 s = d.src, t = d.dst;
    u32 saved = cycles_;
    if (eeprom_at(t)) eeprom_in_count_ = 0;  // each EEPROM command is one transfer
    for (u32 i = 0; i < count; ++i) {
        if (word) write32(t, read32(s));
        else write16(t, read16(s));
        s += u32(sstep);
        t += u32(dstep);
    }
    if (eeprom_at(d.dst)) eeprom_command(count);
    cycles_ = saved + 2 + count * 2;
    d.src = s;
    if (!fifo) d.dst = t;
    if (c & 0x4000) request_irq(8 + ch);
    if ((c & 0x200) && timing != 0) {
        if (!fifo) {
            u32 base = 0xB0 + 12 * ch;
            d.count = io16(io_, base + 8) & (ch == 3 ? 0xFFFF : 0x3FFF);
            if (d.count == 0) d.count = ch == 3 ? 0x10000 : 0x4000;
            if (dctl == 3) d.dst = io32(io_, base + 4) & (ch == 3 ? 0x0FFFFFFF : 0x07FFFFFF);
        }
    } else {
        d.control &= 0x7FFF;
        u32 reg = 0xBA + 12 * ch;
        io_[reg] = u8(d.control);
        io_[reg + 1] = u8(d.control >> 8);
    }
}

// ---- timers --------------------------------------------------------------------------------

namespace {
constexpr u32 kPrescale[4] = {1, 64, 256, 1024};
}

void Gba::timer_write_control(int i, u16 v) {
    Timer& t = timer_[i];
    bool was = t.control & 0x80;
    t.control = v & 0xC7;
    if (!was && (v & 0x80)) {
        t.counter = t.reload;
        t.prescale_acc = 0;
    }
}

void Gba::timer_overflow(int i) {
    Timer& t = timer_[i];
    if (t.control & 0x40) request_irq(3 + i);
    if (i < 2) apu_timer_overflow(i);
    if (i < 3) {
        Timer& n = timer_[i + 1];
        if ((n.control & 0x84) == 0x84) {
            if (++n.counter == 0) {
                n.counter = n.reload;
                timer_overflow(i + 1);
            }
        }
    }
}

void Gba::timers_advance(u32 cycles) {
    for (int i = 0; i < 4; ++i) {
        Timer& t = timer_[i];
        if (!(t.control & 0x80) || (i > 0 && (t.control & 0x04))) continue;
        u32 period = kPrescale[t.control & 3];
        t.prescale_acc += cycles;
        u32 ticks = t.prescale_acc / period;
        t.prescale_acc %= period;
        while (ticks) {
            u32 left = 0x10000u - t.counter;
            if (ticks >= left) {
                ticks -= left;
                t.counter = t.reload;
                timer_overflow(i);
            } else {
                t.counter = u16(t.counter + ticks);
                ticks = 0;
            }
        }
    }
}

u32 Gba::timer_cycles_to_overflow() const {
    u32 best = 0xFFFFFFFF;
    for (int i = 0; i < 4; ++i) {
        const Timer& t = timer_[i];
        if (!(t.control & 0x80) || (i > 0 && (t.control & 0x04))) continue;
        u32 period = kPrescale[t.control & 3];
        u64 c = u64(0x10000u - t.counter) * period - t.prescale_acc;
        best = u32(std::min<u64>(best, c));
    }
    return best;
}

}  // namespace supersaga::gba
