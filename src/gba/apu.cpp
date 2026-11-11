// Sound: Direct Sound A/B FIFOs (driven by timers 0/1 and DMA1/2) and the four PSG channels.
// Mixed to stereo s16 at 32768 Hz.
#include "gba.h"

#include <algorithm>

namespace supersaga::gba {

namespace {
inline u16 rd16(const std::array<u8, 0x400>& io, u32 a) { return u16(io[a] | (io[a + 1] << 8)); }
constexpr u32 kDuty[4] = {1, 2, 4, 6};  // high steps out of 8
constexpr size_t kMaxBufferedFrames = kAudioRate / 2;
}  // namespace

void Gba::apu_reset() {
    sq_ = {};
    wave_ = {};
    noise_ = {};
    wave_ram_.fill(0);
    fifo_ = {};
    apu_sample_acc_ = 0;
    apu_seq_acc_ = 0;
    apu_seq_step_ = 0;
    audio_.clear();
}

size_t Gba::take_audio(std::vector<s16>& out) {
    size_t frames = audio_.size() / 2;
    out.insert(out.end(), audio_.begin(), audio_.end());
    audio_.clear();
    return frames;
}

void Gba::apu_fifo_push(int fifo, u32 data, int bytes) {
    Fifo& f = fifo_[fifo];
    for (int i = 0; i < bytes; ++i) {
        if (f.size < 32) {
            f.data[(f.read + f.size) % 32] = s8(data >> (8 * i));
            ++f.size;
        }
    }
}

void Gba::apu_timer_overflow(int timer) {
    u16 cnth = rd16(io_, 0x82);
    for (int f = 0; f < 2; ++f) {
        int sel = (cnth >> (f ? 14 : 10)) & 1;
        if (sel != timer) continue;
        Fifo& q = fifo_[f];
        if (q.size) {
            q.latch = q.data[q.read];
            q.read = (q.read + 1) % 32;
            --q.size;
        }
        if (q.size <= 16) dma_fifo_request(f);
    }
}

void Gba::apu_trigger(int ch) {
    switch (ch) {
        case 0:
        case 1: {
            Square& s = sq_[ch];
            u16 env = rd16(io_, ch == 0 ? 0x62 : 0x68);
            s.on = (env & 0xF800) != 0;  // DAC off when initial volume 0 and decreasing
            s.volume = env >> 12;
            s.env_counter = 0;
            if (s.length == 0) s.length = 64;
            if (ch == 0) {
                u16 sw = rd16(io_, 0x60);
                s.sweep_period = (sw >> 4) & 7;
                s.sweep_counter = 0;
            }
            break;
        }
        case 2:
            wave_.on = io_[0x70] & 0x80;
            wave_.phase = 0;
            if (wave_.length == 0) wave_.length = 256;
            break;
        case 3: {
            u16 env = rd16(io_, 0x78);
            noise_.on = (env & 0xF800) != 0;
            noise_.volume = env >> 12;
            noise_.env_counter = 0;
            noise_.lfsr = (io_[0x7C] & 0x08) ? 0x7F : 0x7FFF;
            if (noise_.length == 0) noise_.length = 64;
            break;
        }
    }
}

void Gba::apu_write(u32 reg, u16 v, u16 old) {
    (void)old;
    switch (reg) {
        // Envelope registers: initial volume 0 with a falling envelope switches the channel's
        // DAC off, which silences a playing channel at once (games use it to stop a sound).
        case 0x62:
            sq_[0].length = 64 - (v & 63);
            if (!(v & 0xF800)) sq_[0].on = false;
            break;
        case 0x68:
            sq_[1].length = 64 - (v & 63);
            if (!(v & 0xF800)) sq_[1].on = false;
            break;
        case 0x64:
        case 0x6C: {
            int ch = reg == 0x64 ? 0 : 1;
            sq_[ch].length_en = v & 0x4000;
            if (v & 0x8000) apu_trigger(ch);
            io_[reg + 1] &= 0x7F;
            break;
        }
        case 0x70:
            if (!(v & 0x80)) wave_.on = false;
            break;
        case 0x72: wave_.length = 256 - (v & 0xFF); break;
        case 0x74:
            wave_.length_en = v & 0x4000;
            if (v & 0x8000) apu_trigger(2);
            io_[reg + 1] &= 0x7F;
            break;
        case 0x78:
            noise_.length = 64 - (v & 63);
            if (!(v & 0xF800)) noise_.on = false;
            break;
        case 0x7C:
            noise_.length_en = v & 0x4000;
            if (v & 0x8000) apu_trigger(3);
            io_[reg + 1] &= 0x7F;
            break;
        case 0x82:
            if (v & 0x0800) fifo_[0] = {};
            if (v & 0x8000) fifo_[1] = {};
            io_[0x83] &= 0x77;
            break;
        case 0x84:
            if (!(v & 0x80)) { sq_[0].on = sq_[1].on = wave_.on = noise_.on = false; }
            break;
        default:
            if (reg >= 0x90 && reg < 0xA0) {
                // CPU writes go to the bank that is not being played.
                u32 bank = (io_[0x70] & 0x40) ? 0 : 16;
                wave_ram_[bank + (reg - 0x90)] = u8(v);
                wave_ram_[bank + (reg - 0x90) + 1] = u8(v >> 8);
            }
            break;
    }
}

void Gba::apu_frame_sequencer() {
    u32 step = apu_seq_step_++ & 7;
    if ((step & 1) == 0) {  // length, 256 Hz
        auto len = [](bool& on, u32& length, bool en) {
            if (en && length && --length == 0) on = false;
        };
        len(sq_[0].on, sq_[0].length, sq_[0].length_en);
        len(sq_[1].on, sq_[1].length, sq_[1].length_en);
        len(wave_.on, wave_.length, wave_.length_en);
        len(noise_.on, noise_.length, noise_.length_en);
    }
    if (step == 2 || step == 6) {  // sweep, 128 Hz
        Square& s = sq_[0];
        u16 sw = rd16(io_, 0x60);
        u32 period = (sw >> 4) & 7, shift = sw & 7;
        if (s.on && period && ++s.sweep_counter >= period) {
            s.sweep_counter = 0;
            u32 freq = rd16(io_, 0x64) & 0x7FF;
            u32 delta = freq >> shift;
            u32 nf = (sw & 8) ? freq - delta : freq + delta;
            if (nf > 2047) s.on = false;
            else if (shift) {
                io_[0x64] = u8(nf);
                io_[0x65] = u8((io_[0x65] & 0xF8) | (nf >> 8));
            }
        }
    }
    if (step == 7) {  // envelope, 64 Hz
        auto env = [](u32& vol, u32& counter, u16 reg) {
            u32 st = (reg >> 8) & 7;
            if (!st || ++counter < st) return;
            counter = 0;
            if (reg & 0x800) { if (vol < 15) ++vol; }
            else if (vol > 0) --vol;
        };
        env(sq_[0].volume, sq_[0].env_counter, rd16(io_, 0x62));
        env(sq_[1].volume, sq_[1].env_counter, rd16(io_, 0x68));
        env(noise_.volume, noise_.env_counter, rd16(io_, 0x78));
    }
}

void Gba::apu_sample() {
    s32 psg[4] = {0, 0, 0, 0};
    for (int ch = 0; ch < 2; ++ch) {
        Square& s = sq_[ch];
        u16 duty_reg = rd16(io_, ch == 0 ? 0x62 : 0x68);
        u32 freq = rd16(io_, ch == 0 ? 0x64 : 0x6C) & 0x7FF;
        s.phase = (s.phase + (2097152u / (2048 - freq))) % (8u << 16);
        if (s.on) {
            bool high = (s.phase >> 16) < kDuty[(duty_reg >> 6) & 3];
            psg[ch] = high ? s32(s.volume) : -s32(s.volume);
        }
    }
    {
        u16 cntl = rd16(io_, 0x70), cnth = rd16(io_, 0x72);
        u32 freq = rd16(io_, 0x74) & 0x7FF;
        bool two_banks = cntl & 0x20;
        u32 len = two_banks ? 64 : 32;
        wave_.phase = (wave_.phase + (4194304u / (2048 - freq))) % (len << 16);
        if (wave_.on) {
            u32 pos = wave_.phase >> 16;
            u32 bank = two_banks ? 0 : ((cntl & 0x40) ? 16 : 0);
            if (two_banks && (cntl & 0x40)) pos = (pos + 32) % 64;
            u8 byte = wave_ram_[(bank + pos / 2) % 32];
            s32 nib = (pos & 1) ? (byte & 15) : (byte >> 4);
            s32 val = nib * 2 - 15;
            if (cnth & 0x8000) val = val * 3 / 4;
            else switch ((cnth >> 13) & 3) {
                case 0: val = 0; break;
                case 1: break;
                case 2: val /= 2; break;
                case 3: val /= 4; break;
            }
            psg[2] = val;
        }
    }
    {
        u16 reg = rd16(io_, 0x7C);
        u32 r = reg & 7, s = (reg >> 4) & 15;
        u32 period = (r ? 32 * r : 16) << (s + 1);
        noise_.acc += 512;
        u32 steps = 0;
        while (noise_.acc >= period && steps < 512) {
            noise_.acc -= period;
            u32 bit = (noise_.lfsr ^ (noise_.lfsr >> 1)) & 1;
            noise_.lfsr = (noise_.lfsr >> 1) | (bit << 14);
            if (reg & 0x08) noise_.lfsr = (noise_.lfsr & ~0x40u) | (bit << 6);
            ++steps;
        }
        if (steps == 512) noise_.acc = 0;
        if (noise_.on) psg[3] = (noise_.lfsr & 1) ? -s32(noise_.volume) : s32(noise_.volume);
    }

    s32 left = 0, right = 0;
    if (io_[0x84] & 0x80) {
        u16 cntl = rd16(io_, 0x80), cnth = rd16(io_, 0x82);
        s32 pl = 0, pr = 0;
        for (int ch = 0; ch < 4; ++ch) {
            if (cntl & (0x1000 << ch)) pl += psg[ch];
            if (cntl & (0x0100 << ch)) pr += psg[ch];
        }
        pl *= s32(1 + ((cntl >> 4) & 7));
        pr *= s32(1 + (cntl & 7));
        u32 psg_shift = 2 - std::min<u32>(2, cnth & 3);
        left += pl >> psg_shift;
        right += pr >> psg_shift;
        s32 a = fifo_[0].latch * ((cnth & 4) ? 4 : 2);
        s32 b = fifo_[1].latch * ((cnth & 8) ? 4 : 2);
        if (cnth & 0x0100) right += a;
        if (cnth & 0x0200) left += a;
        if (cnth & 0x1000) right += b;
        if (cnth & 0x2000) left += b;
    }
    s32 bias = rd16(io_, 0x88) & 0x3FE;
    auto out = [&](s32 v) {
        s32 x = std::clamp(v + bias, 0, 0x3FF) - 0x200;
        return s16(std::clamp(x * 48, -32768, 32767));
    };
    if (audio_.size() >= kMaxBufferedFrames * 2) audio_.erase(audio_.begin(), audio_.begin() + 2);
    audio_.push_back(out(left));
    audio_.push_back(out(right));
}

void Gba::apu_advance(u32 cycles) {
    apu_seq_acc_ += cycles;
    while (apu_seq_acc_ >= 32768) {
        apu_seq_acc_ -= 32768;
        apu_frame_sequencer();
    }
    apu_sample_acc_ += cycles;
    while (apu_sample_acc_ >= 512) {
        apu_sample_acc_ -= 512;
        apu_sample();
    }
}

}  // namespace supersaga::gba
