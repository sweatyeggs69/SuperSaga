// ARM7TDMI interpreter (ARMv4T: ARM and Thumb).
#include "gba.h"

#include <bit>

namespace supersaga::gba {

namespace {

constexpr u32 kFlagN = 1u << 31, kFlagZ = 1u << 30, kFlagC = 1u << 29, kFlagV = 1u << 28;
constexpr u32 kFlagI = 1u << 7, kFlagT = 1u << 5;
constexpr u32 kModeUsr = 0x10, kModeIrq = 0x12, kModeSvc = 0x13, kModeSys = 0x1F;

int bank_of(u32 mode) {
    switch (mode & 0x1F) {
        case 0x11: return 1;
        case 0x12: return 2;
        case 0x13: return 3;
        case 0x17: return 4;
        case 0x1B: return 5;
        default: return 0;  // usr, sys
    }
}

u32 ror32(u32 v, u32 r) { return std::rotr(v, int(r & 31)); }

// Barrel shifter with an immediate amount (ARM encoding rules for amount 0).
u32 shift_imm(u32 v, u32 type, u32 amt, u32& carry) {
    switch (type) {
        case 0:  // LSL
            if (amt == 0) return v;
            carry = (v >> (32 - amt)) & 1;
            return v << amt;
        case 1:  // LSR (0 means 32)
            if (amt == 0) { carry = v >> 31; return 0; }
            carry = (v >> (amt - 1)) & 1;
            return v >> amt;
        case 2:  // ASR (0 means 32)
            if (amt == 0) { carry = v >> 31; return u32(s32(v) >> 31); }
            carry = (v >> (amt - 1)) & 1;
            return u32(s32(v) >> amt);
        default:  // ROR (0 means RRX)
            if (amt == 0) {
                u32 out = (carry << 31) | (v >> 1);
                carry = v & 1;
                return out;
            }
            carry = (v >> (amt - 1)) & 1;
            return ror32(v, amt);
    }
}

// Barrel shifter with a register amount (bottom byte of Rs).
u32 shift_reg(u32 v, u32 type, u32 amt, u32& carry) {
    if (amt == 0) return v;
    switch (type) {
        case 0:
            if (amt < 32) { carry = (v >> (32 - amt)) & 1; return v << amt; }
            carry = amt == 32 ? (v & 1) : 0;
            return 0;
        case 1:
            if (amt < 32) { carry = (v >> (amt - 1)) & 1; return v >> amt; }
            carry = amt == 32 ? (v >> 31) : 0;
            return 0;
        case 2:
            if (amt < 32) { carry = (v >> (amt - 1)) & 1; return u32(s32(v) >> amt); }
            carry = v >> 31;
            return u32(s32(v) >> 31);
        default: {
            u32 r = amt & 31;
            if (r == 0) { carry = v >> 31; return v; }
            carry = (v >> (r - 1)) & 1;
            return ror32(v, r);
        }
    }
}

}  // namespace

// ---- state helpers ------------------------------------------------------------------------

void Gba::switch_mode(u32 mode) {
    int from = bank_of(cpsr_), to = bank_of(mode);
    if (from != to) {
        bank_r13_[from] = r_[13];
        bank_r14_[from] = r_[14];
        if (from == 1)
            for (int i = 0; i < 5; ++i) { bank_fiq_r8_[i] = r_[8 + i]; r_[8 + i] = bank_usr_r8_[i]; }
        if (to == 1)
            for (int i = 0; i < 5; ++i) { bank_usr_r8_[i] = r_[8 + i]; r_[8 + i] = bank_fiq_r8_[i]; }
        r_[13] = bank_r13_[to];
        r_[14] = bank_r14_[to];
    }
    cpsr_ = (cpsr_ & ~0x1Fu) | (mode & 0x1F);
}

bool Gba::has_spsr() const { return bank_of(cpsr_) != 0; }
u32 Gba::get_spsr() const { return has_spsr() ? bank_spsr_[bank_of(cpsr_)] : cpsr_; }
void Gba::set_spsr(u32 v) {
    if (has_spsr()) bank_spsr_[bank_of(cpsr_)] = v;
}
void Gba::set_cpsr(u32 v) {
    switch_mode(v & 0x1F);
    cpsr_ = v;
}

u32 Gba::user_reg(int i) const {
    int b = bank_of(cpsr_);
    if (i >= 8 && i <= 12 && b == 1) return bank_usr_r8_[i - 8];
    if ((i == 13 || i == 14) && b != 0) return i == 13 ? bank_r13_[0] : bank_r14_[0];
    return r_[i];
}
void Gba::set_user_reg(int i, u32 v) {
    int b = bank_of(cpsr_);
    if (i >= 8 && i <= 12 && b == 1) { bank_usr_r8_[i - 8] = v; return; }
    if ((i == 13 || i == 14) && b != 0) { (i == 13 ? bank_r13_[0] : bank_r14_[0]) = v; return; }
    r_[i] = v;
}

void Gba::set_pc(u32 v) {
    bool thumb = cpsr_ & kFlagT;
    pc_ = thumb ? (v & ~1u) : (v & ~3u);
    branched_ = true;
    u32 region = (pc_ >> 24) & 15;
    cycles_ += wait_n_[region] + (thumb ? 0 : wait_s_[region]);
}

void Gba::set_nz(u32 v) {
    cpsr_ = (cpsr_ & ~(kFlagN | kFlagZ)) | (v & kFlagN) | (v == 0 ? kFlagZ : 0);
}

u32 Gba::alu_add(u32 a, u32 b, u32 carry_in, bool s) {
    u64 r = u64(a) + u64(b) + carry_in;
    u32 res = u32(r);
    if (s) {
        set_nz(res);
        cpsr_ = (cpsr_ & ~(kFlagC | kFlagV)) | ((r >> 32) ? kFlagC : 0) |
                ((~(a ^ b) & (a ^ res)) >> 31 ? kFlagV : 0);
    }
    return res;
}
// a - b - (1 - carry_in) == a + ~b + carry_in
u32 Gba::alu_sub(u32 a, u32 b, u32 carry_in, bool s) { return alu_add(a, ~b, carry_in, s); }

bool Gba::cond_passed(u32 cond) const {
    bool n = cpsr_ & kFlagN, z = cpsr_ & kFlagZ, c = cpsr_ & kFlagC, v = cpsr_ & kFlagV;
    switch (cond) {
        case 0x0: return z;
        case 0x1: return !z;
        case 0x2: return c;
        case 0x3: return !c;
        case 0x4: return n;
        case 0x5: return !n;
        case 0x6: return v;
        case 0x7: return !v;
        case 0x8: return c && !z;
        case 0x9: return !c || z;
        case 0xA: return n == v;
        case 0xB: return n != v;
        case 0xC: return !z && n == v;
        case 0xD: return z || n != v;
        case 0xE: return true;
        default: return false;  // NV
    }
}

u32 Gba::load32_rot(u32 addr) { return ror32(read32(addr & ~3u), (addr & 3) * 8); }
u32 Gba::load16_rot(u32 addr) {
    u32 v = read16(addr & ~1u);
    return (addr & 1) ? ror32(v, 8) : v;
}
u32 Gba::load16_signed(u32 addr) {
    if (addr & 1) return u32(s32(s8(read8(addr))));
    return u32(s32(s16(read16(addr))));
}

// ---- reset, exceptions, step ---------------------------------------------------------------

void Gba::cpu_reset() {
    r_.fill(0);
    bank_r13_.fill(0);
    bank_r14_.fill(0);
    bank_spsr_.fill(0);
    bank_fiq_r8_.fill(0);
    bank_usr_r8_.fill(0);
    halted_ = false;
    intr_wait_ = false;
    if (real_bios_ && bios_boot_) {
        // Power-on state: the real BIOS runs its intro, then jumps to the cartridge itself.
        cpsr_ = kModeSvc | kFlagI | (1u << 6);
        pc_ = 0;
        r_[15] = 8;
        io_[0x300] = 0;  // POSTFLG = 0: first boot, so the BIOS shows the logo
        bios_latch_ = 0;
        return;
    }
    // State after the BIOS boot sequence hands over to the cartridge.
    cpsr_ = kModeSvc | kFlagI | (1u << 6);
    r_[13] = 0x03007FE0;
    switch_mode(kModeIrq);
    r_[13] = 0x03007FA0;
    switch_mode(kModeSys);
    r_[13] = 0x03007F00;
    cpsr_ = kModeSys;
    pc_ = 0x08000000;
    r_[15] = pc_ + 8;
    halted_ = false;
    intr_wait_ = false;
    bios_latch_ = 0xE129F000;
}

void Gba::enter_irq() {
    u32 old = cpsr_;
    switch_mode(kModeIrq);
    set_spsr(old);
    r_[14] = pc_ + 4;  // the handler returns with "subs pc, lr, #4" to pc_
    cpsr_ = (cpsr_ & ~kFlagT) | kFlagI;
    pc_ = 0x18;
    halted_ = false;
}

void Gba::raise_swi() {
    u32 old = cpsr_;
    u32 next = pc_ + ((old & kFlagT) ? 2 : 4);
    switch_mode(kModeSvc);
    set_spsr(old);
    r_[14] = next;
    cpsr_ = (cpsr_ & ~kFlagT) | kFlagI;
    set_pc(0x08);
}

void Gba::cpu_step() {
    u16 ie = u16(io_[0x200] | (io_[0x201] << 8));
    u16 iflag = u16(io_[0x202] | (io_[0x203] << 8));
    if ((io_[0x208] & 1) && (ie & iflag) && !(cpsr_ & kFlagI)) enter_irq();

    cycles_ = 0;
    branched_ = false;
    u32 pc = pc_;
    if (cpsr_ & kFlagT) {
        u16 op = fetch16(pc);
        r_[15] = pc + 4;
        exec_thumb(op);
        if (!branched_) pc_ = pc + 2;
    } else {
        u32 op = fetch32(pc);
        r_[15] = pc + 8;
        if (cond_passed(op >> 28)) exec_arm(op);
        if (!branched_) pc_ = pc + 4;
    }
}

// ---- ARM -----------------------------------------------------------------------------------

void Gba::exec_arm(u32 op) {
    if ((op & 0x0FFFFFF0) == 0x012FFF10) {  // BX
        u32 v = r_[op & 15];
        if (v & 1) cpsr_ |= kFlagT; else cpsr_ &= ~kFlagT;
        set_pc(v);
        return;
    }
    if ((op & 0x0E000000) == 0x0A000000) {  // B, BL
        s32 off = s32(op << 8) >> 6;
        if (op & (1u << 24)) r_[14] = pc_ + 4;
        set_pc(r_[15] + u32(off));
        return;
    }
    if ((op & 0x0F000000) == 0x0F000000) {  // SWI
        if (real_bios_) {
            raise_swi();
        } else if (!hle_swi((op >> 16) & 0xFF)) {
            branched_ = true;  // re-execute (IntrWait)
        }
        return;
    }
    if ((op & 0x0FC000F0) == 0x00000090) {  // MUL, MLA
        u32 rd = (op >> 16) & 15, rn = (op >> 12) & 15, rs = (op >> 8) & 15, rm = op & 15;
        u32 res = r_[rm] * r_[rs];
        if (op & (1u << 21)) res += r_[rn];
        r_[rd] = res;
        if (op & (1u << 20)) set_nz(res);
        cycles_ += 2;
        return;
    }
    if ((op & 0x0F8000F0) == 0x00800090) {  // UMULL, UMLAL, SMULL, SMLAL
        u32 hi = (op >> 16) & 15, lo = (op >> 12) & 15, rs = (op >> 8) & 15, rm = op & 15;
        u64 res = (op & (1u << 22)) ? u64(s64(s32(r_[rm])) * s64(s32(r_[rs]))) : u64(r_[rm]) * r_[rs];
        if (op & (1u << 21)) res += (u64(r_[hi]) << 32) | r_[lo];
        r_[lo] = u32(res);
        r_[hi] = u32(res >> 32);
        if (op & (1u << 20))
            cpsr_ = (cpsr_ & ~(kFlagN | kFlagZ)) | ((res >> 63) ? kFlagN : 0) | (res == 0 ? kFlagZ : 0);
        cycles_ += 3;
        return;
    }
    if ((op & 0x0FB00FF0) == 0x01000090) {  // SWP, SWPB
        u32 rn = (op >> 16) & 15, rd = (op >> 12) & 15, rm = op & 15;
        u32 addr = r_[rn], src = r_[rm];
        if (op & (1u << 22)) {
            u32 v = read8(addr);
            write8(addr, u8(src));
            r_[rd] = v;
        } else {
            u32 v = load32_rot(addr);
            write32(addr & ~3u, src);
            r_[rd] = v;
        }
        cycles_ += 2;
        return;
    }
    if ((op & 0x0E000090) == 0x00000090 && (op & 0x60)) {
        arm_halfword_transfer(op);
        return;
    }
    if ((op & 0x0FBF0FFF) == 0x010F0000) {  // MRS
        r_[(op >> 12) & 15] = (op & (1u << 22)) ? get_spsr() : cpsr_;
        return;
    }
    if ((op & 0x0DB0F000) == 0x0120F000) {  // MSR
        u32 v = (op & (1u << 25)) ? ror32(op & 0xFF, ((op >> 8) & 15) * 2) : r_[op & 15];
        u32 mask = 0;
        if (op & (1u << 16)) mask |= 0x000000FF;
        if (op & (1u << 17)) mask |= 0x0000FF00;
        if (op & (1u << 18)) mask |= 0x00FF0000;
        if (op & (1u << 19)) mask |= 0xFF000000;
        if (op & (1u << 22)) {
            set_spsr((get_spsr() & ~mask) | (v & mask));
        } else {
            if ((cpsr_ & 0x1F) == kModeUsr) mask &= 0xFF000000;
            mask &= ~kFlagT;
            set_cpsr((cpsr_ & ~mask) | (v & mask));
        }
        return;
    }
    if ((op & 0x0C000000) == 0) {
        arm_data_processing(op);
        return;
    }
    if ((op & 0x0C000000) == 0x04000000) {
        if ((op & 0x02000010) == 0x02000010) return;  // undefined
        arm_single_transfer(op);
        return;
    }
    if ((op & 0x0E000000) == 0x08000000) {
        arm_block_transfer(op);
        return;
    }
    // Coprocessor / undefined: ignored (the GBA has no coprocessors).
}

void Gba::arm_data_processing(u32 op) {
    u32 opc = (op >> 21) & 15;
    bool s = op & (1u << 20);
    u32 rn = (op >> 16) & 15, rd = (op >> 12) & 15;
    u32 carry = (cpsr_ >> 29) & 1;
    u32 sc = carry;
    u32 op2;
    bool regshift = !(op & (1u << 25)) && (op & 0x10);
    if (op & (1u << 25)) {
        u32 rot = ((op >> 8) & 15) * 2;
        op2 = ror32(op & 0xFF, rot);
        if (rot) sc = op2 >> 31;
    } else {
        u32 rm = op & 15;
        u32 v = r_[rm];
        if (regshift && rm == 15) v += 4;
        u32 type = (op >> 5) & 3;
        if (regshift) {
            op2 = shift_reg(v, type, r_[(op >> 8) & 15] & 0xFF, sc);
            cycles_ += 1;
        } else {
            op2 = shift_imm(v, type, (op >> 7) & 31, sc);
        }
    }
    u32 a = r_[rn];
    if (regshift && rn == 15) a += 4;

    bool flags = s && rd != 15;
    u32 res = 0;
    bool write = true, logical = false;
    switch (opc) {
        case 0x0: res = a & op2; logical = true; break;
        case 0x1: res = a ^ op2; logical = true; break;
        case 0x2: res = alu_sub(a, op2, 1, flags); break;
        case 0x3: res = alu_sub(op2, a, 1, flags); break;
        case 0x4: res = alu_add(a, op2, 0, flags); break;
        case 0x5: res = alu_add(a, op2, carry, flags); break;
        case 0x6: res = alu_sub(a, op2, carry, flags); break;
        case 0x7: res = alu_sub(op2, a, carry, flags); break;
        case 0x8: res = a & op2; logical = true; write = false; flags = s; break;
        case 0x9: res = a ^ op2; logical = true; write = false; flags = s; break;
        case 0xA: alu_sub(a, op2, 1, s); write = false; break;
        case 0xB: alu_add(a, op2, 0, s); write = false; break;
        case 0xC: res = a | op2; logical = true; break;
        case 0xD: res = op2; logical = true; break;
        case 0xE: res = a & ~op2; logical = true; break;
        case 0xF: res = ~op2; logical = true; break;
    }
    if (logical && flags) {
        set_nz(res);
        cpsr_ = (cpsr_ & ~kFlagC) | (sc ? kFlagC : 0);
    }
    if (!write) return;
    if (rd == 15) {
        if (s) set_cpsr(get_spsr());
        set_pc(res);
    } else {
        r_[rd] = res;
    }
}

void Gba::arm_single_transfer(u32 op) {
    bool pre = op & (1u << 24), up = op & (1u << 23), byte = op & (1u << 22);
    bool wb = op & (1u << 21), load = op & (1u << 20);
    u32 rn = (op >> 16) & 15, rd = (op >> 12) & 15;
    u32 off;
    if (op & (1u << 25)) {
        u32 c = (cpsr_ >> 29) & 1;
        off = shift_imm(r_[op & 15], (op >> 5) & 3, (op >> 7) & 31, c);
    } else {
        off = op & 0xFFF;
    }
    u32 base = r_[rn];
    u32 moved = up ? base + off : base - off;
    u32 addr = pre ? moved : base;
    if (load) {
        u32 v = byte ? read8(addr) : load32_rot(addr);
        if (!pre || wb) r_[rn] = moved;
        if (rd == 15) set_pc(v); else r_[rd] = v;
        cycles_ += 1;
    } else {
        u32 v = r_[rd];
        if (rd == 15) v += 4;
        if (byte) write8(addr, u8(v)); else write32(addr & ~3u, v);
        if (!pre || wb) r_[rn] = moved;
    }
}

void Gba::arm_halfword_transfer(u32 op) {
    bool pre = op & (1u << 24), up = op & (1u << 23), imm = op & (1u << 22);
    bool wb = op & (1u << 21), load = op & (1u << 20);
    u32 rn = (op >> 16) & 15, rd = (op >> 12) & 15, sh = (op >> 5) & 3;
    u32 off = imm ? (((op >> 4) & 0xF0) | (op & 0xF)) : r_[op & 15];
    u32 base = r_[rn];
    u32 moved = up ? base + off : base - off;
    u32 addr = pre ? moved : base;
    if (load) {
        u32 v = sh == 1 ? load16_rot(addr) : sh == 2 ? u32(s32(s8(read8(addr)))) : load16_signed(addr);
        if (!pre || wb) r_[rn] = moved;
        if (rd == 15) set_pc(v); else r_[rd] = v;
        cycles_ += 1;
    } else {
        if (sh == 1) {
            u32 v = r_[rd];
            if (rd == 15) v += 4;
            write16(addr & ~1u, u16(v));
        }
        if (!pre || wb) r_[rn] = moved;
    }
}

void Gba::arm_block_transfer(u32 op) {
    bool pre = op & (1u << 24), up = op & (1u << 23), s = op & (1u << 22);
    bool wb = op & (1u << 21), load = op & (1u << 20);
    u32 rn = (op >> 16) & 15;
    u32 list = op & 0xFFFF;
    u32 base = r_[rn];
    u32 bytes;
    if (list == 0) {  // ARM7 quirk: empty list transfers r15 and moves the base by 0x40
        list = 0x8000;
        bytes = 0x40;
    } else {
        bytes = u32(std::popcount(list)) * 4;
    }
    u32 addr = up ? (pre ? base + 4 : base) : (pre ? base - bytes : base - bytes + 4);
    u32 new_base = up ? base + bytes : base - bytes;
    bool user = s && !(load && (list & 0x8000));
    cycles_ += u32(std::popcount(list));

    if (load) {
        if (wb) r_[rn] = new_base;
        for (int i = 0; i < 16; ++i) {
            if (!(list & (1u << i))) continue;
            u32 v = read32(addr & ~3u);
            addr += 4;
            if (user) {
                set_user_reg(i, v);
            } else if (i == 15) {
                if (s) set_cpsr(get_spsr());
                set_pc(v);
            } else {
                r_[i] = v;
            }
        }
    } else {
        bool first = true;
        for (int i = 0; i < 16; ++i) {
            if (!(list & (1u << i))) continue;
            u32 v = user ? user_reg(i) : r_[i];
            if (i == 15) v += 4;
            if (u32(i) == rn && wb) v = first ? base : new_base;
            write32(addr & ~3u, v);
            addr += 4;
            first = false;
        }
        if (wb) r_[rn] = new_base;
    }
}

// ---- Thumb ---------------------------------------------------------------------------------

void Gba::exec_thumb(u16 op) {
    switch (op >> 13) {
        case 0: {
            if ((op & 0x1800) == 0x1800) {  // add/sub register or imm3
                u32 rd = op & 7, rs = (op >> 3) & 7;
                u32 v = (op & 0x400) ? u32((op >> 6) & 7) : r_[(op >> 6) & 7];
                r_[rd] = (op & 0x200) ? alu_sub(r_[rs], v, 1, true) : alu_add(r_[rs], v, 0, true);
            } else {  // shift by immediate
                u32 rd = op & 7, rs = (op >> 3) & 7;
                u32 c = (cpsr_ >> 29) & 1;
                u32 v = shift_imm(r_[rs], (op >> 11) & 3, (op >> 6) & 31, c);
                r_[rd] = v;
                set_nz(v);
                cpsr_ = (cpsr_ & ~kFlagC) | (c ? kFlagC : 0);
            }
            return;
        }
        case 1: {  // mov/cmp/add/sub imm8
            u32 rd = (op >> 8) & 7, imm = op & 0xFF;
            switch ((op >> 11) & 3) {
                case 0: r_[rd] = imm; set_nz(imm); break;
                case 1: alu_sub(r_[rd], imm, 1, true); break;
                case 2: r_[rd] = alu_add(r_[rd], imm, 0, true); break;
                case 3: r_[rd] = alu_sub(r_[rd], imm, 1, true); break;
            }
            return;
        }
        case 2: {
            if ((op & 0xFC00) == 0x4000) {  // ALU operations
                u32 rd = op & 7, rs = (op >> 3) & 7;
                u32 a = r_[rd], b = r_[rs];
                u32 carry = (cpsr_ >> 29) & 1;
                auto shift = [&](u32 type) {
                    u32 c = carry;
                    u32 v = shift_reg(a, type, b & 0xFF, c);
                    r_[rd] = v;
                    set_nz(v);
                    cpsr_ = (cpsr_ & ~kFlagC) | (c ? kFlagC : 0);
                    cycles_ += 1;
                };
                switch ((op >> 6) & 15) {
                    case 0x0: r_[rd] = a & b; set_nz(r_[rd]); break;
                    case 0x1: r_[rd] = a ^ b; set_nz(r_[rd]); break;
                    case 0x2: shift(0); break;
                    case 0x3: shift(1); break;
                    case 0x4: shift(2); break;
                    case 0x5: r_[rd] = alu_add(a, b, carry, true); break;
                    case 0x6: r_[rd] = alu_sub(a, b, carry, true); break;
                    case 0x7: shift(3); break;
                    case 0x8: set_nz(a & b); break;
                    case 0x9: r_[rd] = alu_sub(0, b, 1, true); break;
                    case 0xA: alu_sub(a, b, 1, true); break;
                    case 0xB: alu_add(a, b, 0, true); break;
                    case 0xC: r_[rd] = a | b; set_nz(r_[rd]); break;
                    case 0xD: r_[rd] = a * b; set_nz(r_[rd]); cycles_ += 2; break;
                    case 0xE: r_[rd] = a & ~b; set_nz(r_[rd]); break;
                    case 0xF: r_[rd] = ~b; set_nz(r_[rd]); break;
                }
                return;
            }
            if ((op & 0xFC00) == 0x4400) {  // hi register ops / BX
                u32 rd = (op & 7) | ((op >> 4) & 8), rs = (op >> 3) & 15;
                switch ((op >> 8) & 3) {
                    case 0: {
                        u32 v = r_[rd] + r_[rs];
                        if (rd == 15) set_pc(v); else r_[rd] = v;
                        break;
                    }
                    case 1: alu_sub(r_[rd], r_[rs], 1, true); break;
                    case 2:
                        if (rd == 15) set_pc(r_[rs]); else r_[rd] = r_[rs];
                        break;
                    case 3: {
                        u32 v = r_[rs];
                        if (!(v & 1)) cpsr_ &= ~kFlagT;
                        set_pc(v);
                        break;
                    }
                }
                return;
            }
            if ((op & 0xF800) == 0x4800) {  // ldr rd, [pc, #imm]
                r_[(op >> 8) & 7] = read32((r_[15] & ~3u) + (op & 0xFF) * 4);
                cycles_ += 1;
                return;
            }
            u32 rd = op & 7, rb = (op >> 3) & 7, ro = (op >> 6) & 7;
            u32 addr = r_[rb] + r_[ro];
            if (!(op & 0x200)) {  // ldr/str word/byte, register offset
                switch ((op >> 10) & 3) {
                    case 0: write32(addr & ~3u, r_[rd]); break;
                    case 1: write8(addr, u8(r_[rd])); break;
                    case 2: r_[rd] = load32_rot(addr); cycles_ += 1; break;
                    case 3: r_[rd] = read8(addr); cycles_ += 1; break;
                }
            } else {  // halfword / signed, register offset
                switch ((op >> 10) & 3) {
                    case 0: write16(addr & ~1u, u16(r_[rd])); break;
                    case 1: r_[rd] = u32(s32(s8(read8(addr)))); cycles_ += 1; break;
                    case 2: r_[rd] = load16_rot(addr); cycles_ += 1; break;
                    case 3: r_[rd] = load16_signed(addr); cycles_ += 1; break;
                }
            }
            return;
        }
        case 3: {  // ldr/str immediate offset
            u32 rd = op & 7, rb = (op >> 3) & 7, off = (op >> 6) & 31;
            bool byte = op & 0x1000, load = op & 0x800;
            u32 addr = r_[rb] + (byte ? off : off * 4);
            if (load) {
                r_[rd] = byte ? read8(addr) : load32_rot(addr);
                cycles_ += 1;
            } else if (byte) {
                write8(addr, u8(r_[rd]));
            } else {
                write32(addr & ~3u, r_[rd]);
            }
            return;
        }
        case 4: {
            if (!(op & 0x1000)) {  // ldrh/strh immediate
                u32 rd = op & 7, rb = (op >> 3) & 7;
                u32 addr = r_[rb] + ((op >> 6) & 31) * 2;
                if (op & 0x800) { r_[rd] = load16_rot(addr); cycles_ += 1; }
                else write16(addr & ~1u, u16(r_[rd]));
            } else {  // sp-relative ldr/str
                u32 rd = (op >> 8) & 7;
                u32 addr = r_[13] + (op & 0xFF) * 4;
                if (op & 0x800) { r_[rd] = load32_rot(addr); cycles_ += 1; }
                else write32(addr & ~3u, r_[rd]);
            }
            return;
        }
        case 5: {
            if (!(op & 0x1000)) {  // add rd, pc/sp, #imm
                u32 base = (op & 0x800) ? r_[13] : (r_[15] & ~3u);
                r_[(op >> 8) & 7] = base + (op & 0xFF) * 4;
                return;
            }
            if ((op & 0x0F00) == 0x0000) {  // add sp, #+/-imm
                u32 imm = (op & 0x7F) * 4;
                r_[13] = (op & 0x80) ? r_[13] - imm : r_[13] + imm;
                return;
            }
            if ((op & 0x0600) == 0x0400) {  // push/pop
                bool pop = op & 0x800, extra = op & 0x100;
                u32 list = op & 0xFF;
                u32 n = u32(std::popcount(list)) + (extra ? 1 : 0);
                cycles_ += n;
                if (!pop) {
                    u32 addr = r_[13] - n * 4;
                    r_[13] = addr;
                    for (int i = 0; i < 8; ++i)
                        if (list & (1u << i)) { write32(addr, r_[i]); addr += 4; }
                    if (extra) write32(addr, r_[14]);
                } else {
                    u32 addr = r_[13];
                    for (int i = 0; i < 8; ++i)
                        if (list & (1u << i)) { r_[i] = read32(addr); addr += 4; }
                    if (extra) { u32 v = read32(addr); addr += 4; r_[13] = addr; set_pc(v); return; }
                    r_[13] = addr;
                }
                return;
            }
            return;  // BKPT and other non-v4T encodings
        }
        case 6: {
            if (!(op & 0x1000)) {  // ldmia/stmia
                u32 rb = (op >> 8) & 7, list = op & 0xFF;
                u32 addr = r_[rb];
                if (list == 0) {  // ARM7 quirk
                    if (op & 0x800) set_pc(read32(addr & ~3u));
                    else write32(addr & ~3u, r_[15] + 2);
                    r_[rb] = addr + 0x40;
                    return;
                }
                u32 end = addr + u32(std::popcount(list)) * 4;
                cycles_ += u32(std::popcount(list));
                if (op & 0x800) {
                    for (int i = 0; i < 8; ++i)
                        if (list & (1u << i)) { r_[i] = read32(addr & ~3u); addr += 4; }
                    if (!(list & (1u << rb))) r_[rb] = end;
                } else {
                    bool first = true;
                    for (int i = 0; i < 8; ++i) {
                        if (!(list & (1u << i))) continue;
                        u32 v = (u32(i) == rb && !first) ? end : r_[i];
                        write32(addr & ~3u, v);
                        addr += 4;
                        first = false;
                    }
                    r_[rb] = end;
                }
                return;
            }
            u32 cond = (op >> 8) & 15;
            if (cond == 15) {
                if (real_bios_) raise_swi();
                else if (!hle_swi(op & 0xFF)) branched_ = true;
                return;
            }
            if (cond == 14) return;
            if (cond_passed(cond)) set_pc(r_[15] + u32(s32(s8(op & 0xFF)) * 2));
            return;
        }
        case 7: {
            switch ((op >> 11) & 3) {
                case 0: set_pc(r_[15] + u32(s32(u32(op & 0x7FF) << 21) >> 20)); return;
                case 2: r_[14] = r_[15] + u32(s32(u32(op & 0x7FF) << 21) >> 9); return;
                case 3: {
                    u32 target = r_[14] + ((op & 0x7FF) << 1);
                    r_[14] = (pc_ + 2) | 1;
                    set_pc(target);
                    return;
                }
                default: return;  // BLX suffix (ARMv5)
            }
        }
    }
}

}  // namespace supersaga::gba
