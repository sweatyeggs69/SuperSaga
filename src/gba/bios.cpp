// High-level emulation of the GBA BIOS. No Nintendo code is used: the only code placed in the
// BIOS region is a six-instruction IRQ dispatcher written here, and every SWI is implemented in C++.
#include "gba.h"

#include <cmath>
#include <cstdio>
#include <iostream>

namespace supersaga::gba {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr u32 kIntrCheck = 0x03007FF8;  // BIOS_IF: handlers OR acknowledged IRQ bits in here

void put32(std::array<u8, 0x4000>& b, u32 a, u32 v) {
    b[a] = u8(v); b[a + 1] = u8(v >> 8); b[a + 2] = u8(v >> 16); b[a + 3] = u8(v >> 24);
}

s32 arctan(s32 i) {
    s32 a = -((i * i) >> 14);
    s32 b = ((0xA9 * a) >> 14) + 0x390;
    b = ((b * a) >> 14) + 0x91C;
    b = ((b * a) >> 14) + 0xFB6;
    b = ((b * a) >> 14) + 0x16AA;
    b = ((b * a) >> 14) + 0x2081;
    b = ((b * a) >> 14) + 0x3651;
    b = ((b * a) >> 14) + 0xA2F9;
    return (i * b) >> 16;
}

u32 arctan2(s32 x, s32 y) {
    if (y == 0) return x >= 0 ? 0 : 0x8000;
    if (x == 0) return y >= 0 ? 0x4000 : 0xC000;
    if (y >= 0) {
        if (x >= 0) {
            if (x >= y) return u32(arctan((y << 14) / x));
        } else if (-x >= y) {
            return u32(arctan((y << 14) / x) + 0x8000);
        }
        return u32(0x4000 - arctan((x << 14) / y));
    }
    if (x <= 0) {
        if (-x > -y) return u32(arctan((y << 14) / x) + 0x8000);
    } else if (x >= -y) {
        return u32(arctan((y << 14) / x) + 0x10000);
    }
    return u32(0xC000 - arctan((x << 14) / y));
}
}  // namespace

void Gba::bios_install() {
    bios_.fill(0);
    for (u32 v = 0; v < 0x20; v += 4) put32(bios_, v, 0xEAFFFFFE);  // b . (unused vectors)
    // IRQ vector (0x18): save scratch regs, call [0x03007FFC] with lr = 0x28, restore, return.
    put32(bios_, 0x18, 0xE92D500F);  // stmfd sp!, {r0-r3, r12, lr}
    put32(bios_, 0x1C, 0xE3A00301);  // mov   r0, #0x04000000
    put32(bios_, 0x20, 0xE28FE000);  // add   lr, pc, #0
    put32(bios_, 0x24, 0xE510F004);  // ldr   pc, [r0, #-4]
    put32(bios_, 0x28, 0xE8BD500F);  // ldmfd sp!, {r0-r3, r12, lr}
    put32(bios_, 0x2C, 0xE25EF004);  // subs  pc, lr, #4
}

bool Gba::hle_swi(u32 n) {
    // Value the real BIOS leaves on the bus after returning from a SWI.
    bios_latch_ = 0xE3A02004;
    switch (n) {
        case 0x00: hle_soft_reset(); return true;
        case 0x01: hle_register_ram_reset(r_[0]); return true;
        case 0x02:
        case 0x03: halted_ = true; return true;
        case 0x04:
        case 0x05: {
            if (n == 5) { r_[0] = 1; r_[1] = 1; }
            if (!intr_wait_ && r_[0]) write16(kIntrCheck, u16(read16(kIntrCheck) & ~r_[1]));
            io_[0x208] = 1;  // IME
            u16 flags = read16(kIntrCheck);
            if (flags & r_[1]) {
                write16(kIntrCheck, u16(flags & ~r_[1]));
                intr_wait_ = false;
                return true;
            }
            intr_wait_ = true;
            halted_ = true;
            return false;  // run the IRQ, then execute this SWI again
        }
        case 0x06:
        case 0x07: {
            s32 num = s32(r_[0]), den = s32(r_[1]);
            if (n == 7) std::swap(num, den);
            if (den == 0) {
                r_[0] = num < 0 ? u32(-1) : 1;
                r_[1] = u32(num);
                r_[3] = 1;
            } else if (num == INT32_MIN && den == -1) {
                r_[0] = u32(INT32_MIN);
                r_[1] = 0;
                r_[3] = u32(INT32_MIN);
            } else {
                s32 q = num / den;
                r_[0] = u32(q);
                r_[1] = u32(num % den);
                r_[3] = u32(q < 0 ? -q : q);
            }
            return true;
        }
        case 0x08: {
            u32 v = r_[0], res = 0, bit = 1u << 30;
            while (bit > v) bit >>= 2;
            while (bit) {
                if (v >= res + bit) { v -= res + bit; res = (res >> 1) + bit; }
                else res >>= 1;
                bit >>= 2;
            }
            r_[0] = res;
            return true;
        }
        case 0x09: r_[0] = u32(arctan(s32(s16(r_[0])))); return true;
        case 0x0A: r_[0] = arctan2(s32(s16(r_[0])), s32(s16(r_[1]))) & 0xFFFF; return true;
        case 0x0B: hle_cpu_set(false); return true;
        case 0x0C: hle_cpu_set(true); return true;
        case 0x0D: r_[0] = 0xBAAE187F; return true;
        case 0x0E: hle_bg_affine_set(); return true;
        case 0x0F: hle_obj_affine_set(); return true;
        case 0x10: hle_bit_unpack(); return true;
        case 0x11: hle_lz77(false); return true;
        case 0x12: hle_lz77(true); return true;
        case 0x13: hle_huffman(); return true;
        case 0x14: hle_rl(false); return true;
        case 0x15: hle_rl(true); return true;
        case 0x16: hle_diff_unfilter(false, false); return true;
        case 0x17: hle_diff_unfilter(false, true); return true;
        case 0x18: hle_diff_unfilter(true, false); return true;
        case 0x19: {
            u16 bias = u16((io_[0x88] | (io_[0x89] << 8)) & ~0x3FE) | (r_[0] ? 0x200 : 0);
            io_[0x88] = u8(bias);
            io_[0x89] = u8(bias >> 8);
            return true;
        }
        case 0x1F: {
            double freq = read32(r_[0] + 4) / std::pow(2.0, (180.0 - r_[1] - r_[2] / 256.0) / 12.0);
            r_[0] = u32(freq);
            return true;
        }
        case 0x25: r_[0] = 1; return true;  // MultiBoot: report failure (no link cable)
        case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E:
        case 0x20: case 0x21: case 0x22: case 0x23: case 0x24:
        case 0x28: case 0x29:
            return true;  // BIOS sound driver: the game uses its own (m4a) driver
        default:
            if (verbose) std::fprintf(stderr, "supersaga_gba: unimplemented SWI 0x%02X at %08X\n", n, pc_);
            return true;
    }
}

void Gba::hle_soft_reset() {
    u8 flag = iwram_[0x7FFA];
    for (u32 a = 0x7E00; a < 0x8000; ++a) iwram_[a] = 0;
    switch_mode(0x13);
    r_[13] = 0x03007FE0;
    r_[14] = 0;
    set_spsr(0);
    switch_mode(0x12);
    r_[13] = 0x03007FA0;
    r_[14] = 0;
    set_spsr(0);
    switch_mode(0x1F);
    r_[13] = 0x03007F00;
    for (int i = 0; i < 13; ++i) r_[i] = 0;
    r_[14] = 0;
    cpsr_ = 0x1F;
    intr_wait_ = false;
    set_pc(flag ? 0x02000000 : 0x08000000);
}

void Gba::hle_register_ram_reset(u32 flags) {
    io_[0] = 0x80;
    io_[1] = 0;
    if (flags & 0x01) ewram_.fill(0);
    if (flags & 0x02) std::fill(iwram_.begin(), iwram_.begin() + 0x7E00, 0);
    if (flags & 0x04) pal_.fill(0);
    if (flags & 0x08) vram_.fill(0);
    if (flags & 0x10) oam_.fill(0);
    if (flags & 0x20) std::fill(io_.begin() + 0x120, io_.begin() + 0x160, 0);
    if (flags & 0x40) {
        std::fill(io_.begin() + 0x60, io_.begin() + 0xA8, 0);
        io_[0x88] = 0;
        io_[0x89] = 0x02;
        apu_reset();
    }
    if (flags & 0x80) {
        std::fill(io_.begin() + 0x02, io_.begin() + 0x60, 0);
        std::fill(io_.begin() + 0xB0, io_.begin() + 0x120, 0);
        std::fill(io_.begin() + 0x200, io_.begin() + 0x20A, 0);
        io_[0x20] = io_[0x30] = 0;
        io_[0x21] = io_[0x31] = 1;
        io_[0x26] = io_[0x36] = 0;
        io_[0x27] = io_[0x37] = 1;
        dma_ = {};
        timer_ = {};
        update_waitstates();
    }
}

void Gba::hle_cpu_set(bool fast) {
    u32 src = r_[0], dst = r_[1], ctl = r_[2];
    u32 count = ctl & 0x1FFFFF;
    bool fill = ctl & (1u << 24);
    bool word = fast || (ctl & (1u << 26));
    if (fast) count = (count + 7) & ~7u;
    if ((src >> 24) == 0) return;  // the BIOS refuses to read its own region
    if (word) {
        src &= ~3u;
        dst &= ~3u;
        u32 v = read32(src);
        for (u32 i = 0; i < count; ++i) {
            if (!fill) v = read32(src + 4 * i);
            write32(dst + 4 * i, v);
        }
    } else {
        src &= ~1u;
        dst &= ~1u;
        u16 v = read16(src);
        for (u32 i = 0; i < count; ++i) {
            if (!fill) v = read16(src + 2 * i);
            write16(dst + 2 * i, v);
        }
    }
}

void Gba::write_decompressed(u32 dst, const std::vector<u8>& data, bool vram) {
    if (vram) {
        for (size_t i = 0; i < data.size(); i += 2) {
            u16 v = data[i] | (i + 1 < data.size() ? u16(data[i + 1] << 8) : 0);
            write16(dst + u32(i), v);
        }
    } else {
        for (size_t i = 0; i < data.size(); ++i) write8(dst + u32(i), data[i]);
    }
}

void Gba::hle_lz77(bool vram) {
    u32 src = r_[0];
    u32 size = read32(src) >> 8;
    src += 4;
    std::vector<u8> out;
    out.reserve(size);
    while (out.size() < size) {
        u8 flags = read8(src++);
        for (int b = 0; b < 8 && out.size() < size; ++b, flags <<= 1) {
            if (flags & 0x80) {
                u8 b0 = read8(src++), b1 = read8(src++);
                size_t disp = (size_t((b0 & 0xF) << 8) | b1) + 1;
                u32 len = (b0 >> 4) + 3;
                for (u32 k = 0; k < len && out.size() < size; ++k)
                    out.push_back(disp <= out.size() ? out[out.size() - disp] : 0);
            } else {
                out.push_back(read8(src++));
            }
        }
    }
    write_decompressed(r_[1], out, vram);
}

void Gba::hle_rl(bool vram) {
    u32 src = r_[0];
    u32 size = read32(src) >> 8;
    src += 4;
    std::vector<u8> out;
    out.reserve(size);
    while (out.size() < size) {
        u8 flag = read8(src++);
        if (flag & 0x80) {
            u32 len = (flag & 0x7F) + 3;
            u8 v = read8(src++);
            for (u32 k = 0; k < len && out.size() < size; ++k) out.push_back(v);
        } else {
            u32 len = (flag & 0x7F) + 1;
            for (u32 k = 0; k < len && out.size() < size; ++k) out.push_back(read8(src++));
        }
    }
    write_decompressed(r_[1], out, vram);
}

void Gba::hle_huffman() {
    u32 src = r_[0];
    u32 header = read32(src);
    u32 size = header >> 8;
    u32 bits = header & 0xF;
    if (bits != 4 && bits != 8) bits = 8;
    u32 tree = src + 4;
    u32 root = tree + 1;
    u32 data = tree + (u32(read8(tree)) + 1) * 2;
    std::vector<u8> out;
    out.reserve(size + 4);
    u32 word_out = 0, nbits = 0;
    u32 node = root;
    while (out.size() < size) {
        u32 w = read32(data);
        data += 4;
        for (int b = 31; b >= 0 && out.size() < size; --b) {
            u32 bit = (w >> b) & 1;
            u8 nv = read8(node);
            u32 next = (node & ~1u) + (u32(nv & 0x3F) + 1) * 2 + bit;
            bool leaf = bit ? (nv & 0x40) : (nv & 0x80);
            if (leaf) {
                word_out |= (u32(read8(next)) & ((1u << bits) - 1)) << nbits;
                nbits += bits;
                if (nbits == 32) {
                    for (int k = 0; k < 4; ++k) out.push_back(u8(word_out >> (8 * k)));
                    word_out = 0;
                    nbits = 0;
                }
                node = root;
            } else {
                node = next;
            }
        }
    }
    out.resize(size);
    for (size_t i = 0; i + 3 < out.size() + 3 && i < out.size(); i += 4) {
        u32 v = 0;
        for (int k = 0; k < 4 && i + k < out.size(); ++k) v |= u32(out[i + k]) << (8 * k);
        write32(r_[1] + u32(i), v);
    }
}

void Gba::hle_bit_unpack() {
    u32 src = r_[0], dst = r_[1], info = r_[2];
    u32 len = read16(info), sw = read8(info + 2), dw = read8(info + 3);
    u32 off = read32(info + 4);
    bool zero = off >> 31;
    off &= 0x7FFFFFFF;
    if (!sw || !dw || sw > 8 || dw > 32) return;
    u32 out = 0, nbits = 0;
    for (u32 i = 0; i < len; ++i) {
        u8 b = read8(src + i);
        for (u32 bit = 0; bit < 8; bit += sw) {
            u32 v = (b >> bit) & ((1u << sw) - 1);
            if (v || zero) v += off;
            out |= v << nbits;
            nbits += dw;
            if (nbits >= 32) {
                write32(dst, out);
                dst += 4;
                out = 0;
                nbits = 0;
            }
        }
    }
}

void Gba::hle_diff_unfilter(bool bits16, bool vram) {
    u32 src = r_[0];
    u32 size = read32(src) >> 8;
    src += 4;
    std::vector<u8> out;
    out.reserve(size);
    if (bits16) {
        u16 acc = 0;
        for (u32 i = 0; i < size; i += 2) {
            acc = u16(acc + read16(src + i));
            out.push_back(u8(acc));
            out.push_back(u8(acc >> 8));
        }
        out.resize(size);
        write_decompressed(r_[1], out, true);
    } else {
        u8 acc = 0;
        for (u32 i = 0; i < size; ++i) {
            acc = u8(acc + read8(src + i));
            out.push_back(acc);
        }
        write_decompressed(r_[1], out, vram);
    }
}

void Gba::hle_bg_affine_set() {
    u32 src = r_[0], dst = r_[1], n = r_[2];
    for (u32 i = 0; i < n; ++i, src += 20, dst += 16) {
        double ox = s32(read32(src)) / 256.0, oy = s32(read32(src + 4)) / 256.0;
        double cx = s16(read16(src + 8)), cy = s16(read16(src + 10));
        double sx = s16(read16(src + 12)) / 256.0, sy = s16(read16(src + 14)) / 256.0;
        double theta = (read16(src + 16) >> 8) / 128.0 * kPi;
        double a = std::cos(theta), b = std::sin(theta);
        double pa = a * sx, pb = -b * sx, pc = b * sy, pd = a * sy;
        double rx = ox - (pa * cx + pb * cy), ry = oy - (pc * cx + pd * cy);
        write16(dst, u16(s16(pa * 256)));
        write16(dst + 2, u16(s16(pb * 256)));
        write16(dst + 4, u16(s16(pc * 256)));
        write16(dst + 6, u16(s16(pd * 256)));
        write32(dst + 8, u32(s32(rx * 256)));
        write32(dst + 12, u32(s32(ry * 256)));
    }
}

void Gba::hle_obj_affine_set() {
    u32 src = r_[0], dst = r_[1], n = r_[2], stride = r_[3];
    for (u32 i = 0; i < n; ++i, src += 8, dst += stride * 4) {
        double sx = s16(read16(src)) / 256.0, sy = s16(read16(src + 2)) / 256.0;
        double theta = (read16(src + 4) >> 8) / 128.0 * kPi;
        double a = std::cos(theta), b = std::sin(theta);
        write16(dst, u16(s16(a * sx * 256)));
        write16(dst + stride, u16(s16(-b * sx * 256)));
        write16(dst + stride * 2, u16(s16(b * sy * 256)));
        write16(dst + stride * 3, u16(s16(a * sy * 256)));
    }
}

}  // namespace supersaga::gba
