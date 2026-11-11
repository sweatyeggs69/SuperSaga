// supersaga_disasm: recursive-descent ARM/Thumb tracer for the SuperSaga ROM (ARMv4T, ARM7TDMI).
//
// Everything this tool writes is derived from the ROM. It goes to generated/, which is
// gitignored, and must never be committed or published.
//
// Approach
//   * Capstone decodes and prints instructions; anything outside ARMv4T is rejected so data
//     is not mistaken for newer-architecture code.
//   * Control flow and constant tracking are read from the raw ARMv4T encodings, which are
//     fixed and simple, rather than from Capstone operand details.
//   * Tracing starts at the reset branch at 0x08000000 (ARM). BL targets and resolvable BX
//     targets become functions. Values are tracked per basic block only (registers are
//     unknown at each block start), so only locally provable targets are followed.
//   * Indirect jumps that cannot be proven (jump tables, bx/mov pc on unknown registers) are
//     logged to unresolved.txt, never guessed.

#include <capstone.h>

#include "rom.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using opt = std::optional<u32>;

constexpr u32 kRomBase = 0x08000000;
constexpr u32 kIrqVector = 0x03007FFC;  // BIOS reads the user IRQ handler pointer from here
constexpr u32 kDmaBase = 0x040000B0;    // DMA0..3: SAD, DAD, CNT (12 bytes each)

std::string hex(u32 v) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%08X", v);
    return b;
}
std::string hexopt(opt v) { return v ? hex(*v) : std::string("?"); }
u64 key(u32 a, bool thumb) { return (u64(a) << 1) | (thumb ? 1 : 0); }
u32 key_addr(u64 k) { return u32(k >> 1); }
bool key_thumb(u64 k) { return k & 1; }
bool is_ram(u32 v) { return (v >> 24) == 0x02 || (v >> 24) == 0x03; }
u32 rotr(u32 v, unsigned r) { return r ? (v >> r) | (v << (32 - r)) : v; }

struct Decoded {
    bool valid = false;
    u8 size = 0;
    u32 raw = 0;          // ARM: word. Thumb: halfword, or (hi << 16) | lo for a BL pair.
    u16 written = 0;      // bitmask of r0..r15 written, from Capstone
    bool regs_ok = false;
    std::string text;
    std::string why;      // reason when !valid
};

class Decoder {
public:
    explicit Decoder(const std::vector<u8>& rom) : rom_(rom) {
        if (cs_open(CS_ARCH_ARM, CS_MODE_ARM, &arm_) != CS_ERR_OK ||
            cs_open(CS_ARCH_ARM, CS_MODE_THUMB, &thumb_) != CS_ERR_OK) {
            throw std::runtime_error("cs_open failed");
        }
        cs_option(arm_, CS_OPT_DETAIL, CS_OPT_ON);
        cs_option(thumb_, CS_OPT_DETAIL, CS_OPT_ON);
        arm_insn_ = cs_malloc(arm_);
        thumb_insn_ = cs_malloc(thumb_);
    }
    ~Decoder() {
        cs_free(arm_insn_, 1);
        cs_free(thumb_insn_, 1);
        cs_close(&arm_);
        cs_close(&thumb_);
    }
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    bool in_rom(u32 a, u32 n = 1) const {
        return a >= kRomBase && u64(a - kRomBase) + n <= rom_.size();
    }
    u32 rd32(u32 a) const {
        const u8* p = &rom_[a - kRomBase];
        return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24);
    }
    u16 rd16(u32 a) const {
        const u8* p = &rom_[a - kRomBase];
        return u16(p[0] | (p[1] << 8));
    }

    const Decoded& get(u32 addr, bool thumb) {
        auto [it, fresh] = cache_.try_emplace(key(addr, thumb));
        if (fresh) it->second = decode(addr, thumb);
        return it->second;
    }

private:
    Decoded decode(u32 addr, bool thumb) {
        Decoded d;
        if (addr & (thumb ? 1u : 3u)) { d.why = "misaligned"; return d; }
        if (!in_rom(addr, thumb ? 2 : 4)) { d.why = "outside ROM"; return d; }
        unsigned n = 0;
        if (thumb) {
            u16 h = rd16(addr);
            if ((h & 0xF800) == 0xF000) {
                if (!in_rom(addr + 2, 2)) { d.why = "BL prefix at ROM end"; return d; }
                u16 l = rd16(addr + 2);
                if ((l & 0xF800) != 0xF800) { d.why = "BL prefix without BL suffix"; return d; }
                n = 4;
                d.raw = (u32(h) << 16) | l;
            } else if ((h & 0xE000) == 0xE000 && (h & 0x1800) != 0) {
                d.why = "Thumb-2 / BLX suffix / lone BL suffix (not ARMv4T)";
                return d;
            } else {
                n = 2;
                d.raw = h;
                u8 top = u8(h >> 8);
                if ((top & 0xF0) == 0xB0 && top != 0xB0 && top != 0xB4 && top != 0xB5 &&
                    top != 0xBC && top != 0xBD) {
                    d.why = "misc Thumb op outside ARMv4T (CBZ/IT/BKPT/REV/...)";
                    return d;
                }
                if ((h & 0xFF87) == 0x4780) { d.why = "BLX register (ARMv5)"; return d; }
                if (top == 0xDE) { d.why = "undefined Thumb encoding"; return d; }
            }
        } else {
            d.raw = rd32(addr);
            n = 4;
            if ((d.raw >> 28) == 0xF) { d.why = "ARM cond=NV (ARMv5+ only)"; return d; }
        }

        csh h = thumb ? thumb_ : arm_;
        cs_insn* in = thumb ? thumb_insn_ : arm_insn_;
        const u8* p = &rom_[addr - kRomBase];
        size_t sz = n;
        u64 a = addr;
        if (!cs_disasm_iter(h, &p, &sz, &a, in) || in->size != n) {
            d.why = "Capstone could not decode";
            return d;
        }
        static constexpr u8 kRejected[] = {
            ARM_GRP_CRYPTO, ARM_GRP_DATABARRIER, ARM_GRP_DIVIDE, ARM_GRP_FPARMV8, ARM_GRP_NEON,
            ARM_GRP_T2EXTRACTPACK, ARM_GRP_THUMB2DSP, ARM_GRP_TRUSTZONE, ARM_GRP_V5T,
            ARM_GRP_V5TE, ARM_GRP_V6, ARM_GRP_V6T2, ARM_GRP_V7, ARM_GRP_V8, ARM_GRP_VFP2,
            ARM_GRP_VFP3, ARM_GRP_VFP4, ARM_GRP_MCLASS, ARM_GRP_THUMB2, ARM_GRP_CRC,
            ARM_GRP_DPVFP, ARM_GRP_V6M, ARM_GRP_VIRTUALIZATION};
        // Capstone tags the ARMv4 multiply family as "v6"; the ARM7TDMI has all of these.
        bool v4_multiply = in->id == ARM_INS_MUL || in->id == ARM_INS_MLA || in->id == ARM_INS_UMULL ||
                           in->id == ARM_INS_UMLAL || in->id == ARM_INS_SMULL || in->id == ARM_INS_SMLAL;
        for (u8 g = 0; g < in->detail->groups_count && !v4_multiply; ++g) {
            if (std::find(std::begin(kRejected), std::end(kRejected), in->detail->groups[g]) !=
                std::end(kRejected)) {
                d.why = std::string("not ARMv4T: ") + in->mnemonic + " " + in->op_str;
                return d;
            }
        }

        cs_regs rr, ww;
        u8 nr = 0, nw = 0;
        if (cs_regs_access(h, in, rr, &nr, ww, &nw) == CS_ERR_OK) {
            d.regs_ok = true;
            for (u8 i = 0; i < nw; ++i) {
                int r = reg_index(ww[i]);
                if (r >= 0) d.written |= u16(1u << r);
            }
        }
        d.valid = true;
        d.size = u8(n);
        d.text = in->mnemonic;
        if (in->op_str[0]) d.text += std::string(" ") + in->op_str;
        return d;
    }

    static int reg_index(u16 r) {
        if (r >= ARM_REG_R0 && r <= ARM_REG_R12) return r - ARM_REG_R0;
        if (r == ARM_REG_SP) return 13;
        if (r == ARM_REG_LR) return 14;
        if (r == ARM_REG_PC) return 15;
        return -1;
    }

    const std::vector<u8>& rom_;
    csh arm_ = 0, thumb_ = 0;
    cs_insn* arm_insn_ = nullptr;
    cs_insn* thumb_insn_ = nullptr;
    std::unordered_map<u64, Decoded> cache_;
};

struct CallSite { u32 site; u32 target; bool thumb; bool tail; std::string how; };
struct Unresolved { u32 addr; bool thumb; std::string kind; std::string text; };
struct Stop { u32 addr; bool thumb; std::string why; };
struct IrqWrite { u32 site; opt value; };
struct Copy { u32 site; std::string how; u32 src; u32 dst; opt count; };
struct Table { u32 site; u32 base; u32 n; };

struct Explored {
    u32 start = 0;
    bool thumb = false;
    std::vector<u32> insns;
    std::set<u32> literals;
    std::set<u32> labels;
    u32 lo = 0, hi = 0, code_bytes = 0;
    std::vector<CallSite> calls;
    std::vector<Unresolved> unresolved;
    std::vector<Stop> stops;
    std::vector<IrqWrite> irq;
    std::vector<Copy> copies;
    std::vector<Table> tables;
    std::vector<std::pair<u64, std::string>> new_entries;
};

class Tracer {
public:
    Tracer(Decoder& dec, const std::unordered_set<u64>& entries, bool pointer_literals)
        : dec_(dec), entries_(entries), pointer_literals_(pointer_literals) {}

    Explored explore(u32 start, bool thumb) {
        Explored out;
        out.start = start;
        out.thumb = thumb;
        std::unordered_set<u32> visited;
        std::deque<u32> work{start};

        while (!work.empty()) {
            u32 a = work.front();
            work.pop_front();
            regs_.fill(std::nullopt);
            prev_pop_ = -1;
            jt_.fill(Jt{});
            bound_.fill(std::nullopt);
            pending_cmp_.reset();
            dma_sad_.fill(std::nullopt);
            dma_dad_.fill(std::nullopt);
            dma_copy_idx_.fill(-1);
            for (;;) {
                if (visited.count(a)) break;
                if (a != start && entries_.count(key(a, thumb))) {
                    out.calls.push_back({a, a, thumb, true, "falls through into function"});
                    break;
                }
                const Decoded& d = dec_.get(a, thumb);
                if (!d.valid) {
                    out.stops.push_back({a, thumb, d.why});
                    break;
                }
                visited.insert(a);
                out.insns.push_back(a);
                bool end = thumb ? step_thumb(out, work, a, d) : step_arm(out, work, a, d);
                if (end) break;
                a += d.size;
            }
        }

        std::sort(out.insns.begin(), out.insns.end());
        if (!out.insns.empty()) {
            out.lo = out.insns.front();
            for (u32 i : out.insns) {
                const Decoded& d = dec_.get(i, thumb);
                out.hi = std::max(out.hi, i + d.size);
                out.code_bytes += d.size;
            }
        }
        return out;
    }

private:
    // ---- shared helpers -------------------------------------------------------------------
    void add_entry(Explored& out, u32 target, bool thumb, const std::string& why) {
        out.new_entries.emplace_back(key(target, thumb), why);
    }

    // Records a call or tail jump to a target. Returns false if the target is not usable.
    bool call(Explored& out, u32 site, u32 target, bool thumb, bool tail, const std::string& how,
              const std::string& text) {
        if (!dec_.in_rom(target, 2) || (target & (thumb ? 1u : 3u)) || target < kRomBase + 0xC0) {
            out.unresolved.push_back(
                {site, out.thumb,
                 std::string(tail ? "jump" : "call") + " to non-code/out-of-ROM address " +
                     hex(target) + (thumb ? " (Thumb)" : " (ARM)") + " [" + how + "]",
                 text});
            return false;
        }
        out.calls.push_back({site, target, thumb, tail, how});
        add_entry(out, target, thumb, how);
        return true;
    }

    void jump(Explored& out, std::deque<u32>& work, u32 site, u32 target, const std::string& text) {
        if (target == out.start || !entries_.count(key(target, out.thumb))) {
            if (!dec_.in_rom(target, 2)) {
                out.unresolved.push_back({site, out.thumb, "branch outside ROM " + hex(target), text});
                return;
            }
            out.labels.insert(target);
            work.push_back(target);
        } else {
            out.calls.push_back({site, target, out.thumb, true, "tail branch"});
        }
    }

    // A BIOS call wrapper: "swi #n; bx lr" (Thumb) or "swi #n<<16; bx lr" (ARM).
    std::optional<unsigned> swi_wrapper(u32 t, bool thumb) {
        if (!dec_.in_rom(t, 8)) return std::nullopt;
        if (thumb) {
            u16 a = dec_.rd16(t), b = dec_.rd16(t + 2);
            if ((a & 0xFF00) == 0xDF00 && b == 0x4770) return a & 0xFF;
        } else {
            u32 a = dec_.rd32(t), b = dec_.rd32(t + 4);
            if ((a & 0xFF000000) == 0xEF000000 && b == 0xE12FFF1E) return (a >> 16) & 0xFF;
        }
        return std::nullopt;
    }

    // Literal-pool word that looks like a code pointer: an odd ROM address whose target starts
    // with "push {..., lr}", or an aligned one whose target starts with "stmfd sp!, {..., lr}".
    void literal_pointer(Explored& out, u32 v) {
        if (!pointer_literals_ || !dec_.in_rom(v & ~1u, 4) || (v & ~1u) < kRomBase + 0xC0) return;
        if ((v & 1) && (dec_.rd16(v & ~1u) & 0xFF00) == 0xB500)
            add_entry(out, v & ~1u, true, "literal pointer (Thumb push lr)");
        else if ((v & 3) == 0 && (dec_.rd32(v) & 0xFFFF4000) == 0xE92D4000)
            add_entry(out, v, false, "literal pointer (ARM stmfd lr)");
    }

    // Resolves a bounded jump table: every entry must be a halfword-aligned ROM code address.
    bool jump_table(Explored& out, std::deque<u32>& work, u32 site, u32 jbase, u32 jn, const std::string& text) {
        struct { u32 base, n; } j{jbase, jn};
        if (!dec_.in_rom(j.base, j.n * 4) || (j.base & 3)) return false;
        for (u32 i = 0; i < j.n; ++i) {
            u32 t = dec_.rd32(j.base + 4 * i) & ~1u;
            if (!dec_.in_rom(t, 2) || t < kRomBase + 0xC0) return false;
        }
        for (u32 i = 0; i < j.n; ++i) {
            out.literals.insert(j.base + 4 * i);
            jump(out, work, site, dec_.rd32(j.base + 4 * i) & ~1u, text);
        }
        out.tables.push_back({site, j.base, j.n});
        return true;
    }

    void clobber_after_call() {
        for (int r : {0, 1, 2, 3, 12, 14}) {
            regs_[r].reset();
            jt_[r] = Jt{};
            bound_[r].reset();
        }
    }

    void store(Explored& out, u32 site, opt ea, opt val) {
        if (!ea) return;
        u32 e = *ea;
        if (e == kIrqVector) {
            out.irq.push_back({site, val});
            if (val && dec_.in_rom(*val)) add_entry(out, *val & ~1u, *val & 1, "IRQ vector write");
        }
        if (e >= kDmaBase && e < kDmaBase + 48) {
            unsigned ch = (e - kDmaBase) / 12, off = (e - kDmaBase) % 12;
            if (off == 0) dma_sad_[ch] = val;
            if (off == 4) {
                dma_dad_[ch] = val;
                if (dma_sad_[ch] && val && dec_.in_rom(*dma_sad_[ch]) && is_ram(*val)) {
                    dma_copy_idx_[ch] = int(out.copies.size());
                    out.copies.push_back({site, "DMA" + std::to_string(ch), *dma_sad_[ch], *val,
                                          std::nullopt});
                }
            }
            if (off == 8 && dma_copy_idx_[ch] >= 0) out.copies[dma_copy_idx_[ch]].count = val;
        }
    }

    void swi(Explored& out, u32 site, unsigned n) {
        if ((n == 0x0B || n == 0x0C) && regs_[0] && regs_[1] && dec_.in_rom(*regs_[0]) &&
            is_ram(*regs_[1])) {
            out.copies.push_back({site, n == 0x0B ? "SWI CpuSet" : "SWI CpuFastSet", *regs_[0],
                                  *regs_[1], regs_[2]});
        }
        for (int r : {0, 1, 2, 3}) regs_[r].reset();
    }

    // Handles bx/known register branches. Returns true if the path ends.
    bool bx(Explored& out, u32 site, unsigned size, unsigned rm, opt val, bool always,
            const std::string& text, int popped) {
        if (rm == 14 || int(rm) == popped) return always;  // return (bx lr, or pop {rN}; bx rN)
        if (!val) {
            out.unresolved.push_back({site, out.thumb, "bx r" + std::to_string(rm) + " (unknown target)", text});
            return always;
        }
        u32 t = *val & ~1u;
        bool tm = *val & 1;
        bool linked = regs_[14] && ((*regs_[14] & ~1u) == site + size);
        if (!dec_.in_rom(t)) {
            out.unresolved.push_back({site, out.thumb,
                                      std::string(linked ? "call" : "jump") + " into non-ROM " + hex(*val) +
                                          (is_ram(t) ? " (RAM-resident code)" : ""),
                                      text});
        } else {
            call(out, site, t, tm, !linked, linked ? "bx call (literal)" : "bx tail (literal)", text);
        }
        if (linked) {
            clobber_after_call();
            return false;
        }
        return always;
    }

    void invalidate(const Decoded& d) {
        if (!d.regs_ok) { regs_.fill(std::nullopt); return; }
        for (int r = 0; r < 16; ++r)
            if (d.written & (1u << r)) regs_[r].reset();
    }

    // ---- Thumb ----------------------------------------------------------------------------
    bool step_thumb(Explored& out, std::deque<u32>& work, u32 a, const Decoded& d) {
        int prev = prev_pop_;
        prev_pop_ = -1;
        auto cmp = pending_cmp_;
        pending_cmp_.reset();
        const auto jt0 = jt_;
        const auto bound0 = bound_;
        for (int r = 0; r < 16; ++r)
            if (!d.regs_ok || (d.written & (1u << r))) { jt_[r] = Jt{}; bound_[r].reset(); }
        auto scaled_plus = [&](unsigned x, unsigned y) -> std::optional<Jt> {
            if (jt0[x].kind == Jt::Scaled && regs_[y]) return Jt{Jt::Ptr, *regs_[y], jt0[x].n};
            if (jt0[y].kind == Jt::Scaled && regs_[x]) return Jt{Jt::Ptr, *regs_[x], jt0[y].n};
            return std::nullopt;
        };
        auto rv = [&](unsigned r) -> opt { return r == 15 ? opt(a + 4) : regs_[r]; };
        auto add = [](opt x, opt y) -> opt { return x && y ? opt(*x + *y) : std::nullopt; };
        auto sub = [](opt x, opt y) -> opt { return x && y ? opt(*x - *y) : std::nullopt; };

        if (d.size == 4) {  // BL pair
            u32 hi = d.raw >> 16, lo = d.raw & 0x7FF;
            int32_t off = int32_t((hi & 0x7FF) << 21) >> 9;
            u32 t = a + 4 + u32(off) + (lo << 1);
            call(out, a, t, true, false, "bl", d.text);
            if (auto n = swi_wrapper(t, true)) swi(out, a, *n);
            // GCC's _call_via_rN veneers are a lone "bx rN": resolve through the caller's register.
            if (dec_.in_rom(t, 2)) {
                const Decoded& v = dec_.get(t, true);
                if (v.valid && v.size == 2 && (v.raw & 0xFF87) == 0x4700) {
                    unsigned rm = (v.raw >> 3) & 15;
                    if (rm != 14 && rm != 15) {
                        if (regs_[rm])
                            call(out, a, *regs_[rm] & ~1u, *regs_[rm] & 1, false,
                                 "bl _call_via_r" + std::to_string(rm), d.text);
                        else
                            out.unresolved.push_back({a, true, "bl _call_via_r" + std::to_string(rm) + " (unknown target)", d.text});
                    }
                }
            }
            clobber_after_call();
            return false;
        }

        u16 h = u16(d.raw);
        if ((h & 0xF000) == 0xD000) {
            unsigned cond = (h >> 8) & 15;
            if (cond == 0xF) { swi(out, a, h & 0xFF); return false; }
            int32_t off = int32_t(int8_t(h & 0xFF)) * 2;
            jump(out, work, a, a + 4 + u32(off), d.text);
            if (cond == 8 && cmp) bound_[cmp->first] = cmp->second;  // bhi: fallthrough has idx <= N
            return false;
        }
        if ((h & 0xF800) == 0xE000) {
            int32_t off = int32_t(u32(h & 0x7FF) << 21) >> 20;
            jump(out, work, a, a + 4 + u32(off), d.text);
            return true;
        }
        if ((h & 0xFF87) == 0x4700) {
            unsigned rm = (h >> 3) & 15;
            return bx(out, a, 2, rm, rv(rm), true, d.text, prev);
        }
        if ((h & 0xFC00) == 0x4400) {  // hi register ADD/CMP/MOV
            unsigned op = (h >> 8) & 3, rd = (h & 7) | ((h >> 4) & 8), rs = (h >> 3) & 15;
            if (op == 1) return false;
            if (rd == 15) {
                if (op == 2 && rs == 14) return true;  // mov pc, lr
                if (op == 2 && jt0[rs].kind == Jt::Val && jump_table(out, work, a, jt0[rs].base, jt0[rs].n, d.text))
                    return true;
                out.unresolved.push_back({a, true,
                                          op == 2 ? "mov pc, r" + std::to_string(rs) + " (jump table / indirect)"
                                                  : "add pc, r" + std::to_string(rs) + " (jump table / indirect)",
                                          d.text});
                return true;
            }
            if (op == 0) {
                if (auto j = scaled_plus(rd, rs)) jt_[rd] = *j;
            } else {
                jt_[rd] = jt0[rs];
                bound_[rd] = bound0[rs];
            }
            regs_[rd] = op == 2 ? rv(rs) : add(rv(rd), rv(rs));
            return false;
        }
        if ((h & 0xFE00) == 0xBC00) {  // pop
            if (h & 0x100) return true;
            for (int r = 0; r < 8; ++r)
                if (h & (1 << r)) { regs_[r].reset(); prev_pop_ = r; }
            return false;
        }
        if ((h & 0xF800) == 0x4800) {  // ldr rd, [pc, #imm]
            unsigned rd = (h >> 8) & 7;
            u32 la = ((a + 4) & ~3u) + (h & 0xFF) * 4;
            if (dec_.in_rom(la, 4)) {
                out.literals.insert(la);
                regs_[rd] = dec_.rd32(la);
                literal_pointer(out, *regs_[rd]);
            } else {
                regs_[rd].reset();
            }
            return false;
        }
        if ((h & 0xF800) == 0xA000) {  // add rd, pc, #imm (adr)
            regs_[(h >> 8) & 7] = ((a + 4) & ~3u) + (h & 0xFF) * 4;
            return false;
        }
        if ((h & 0xE000) == 0x2000) {  // mov/cmp/add/sub rd, #imm8
            unsigned op = (h >> 11) & 3, rd = (h >> 8) & 7;
            u32 imm = h & 0xFF;
            if (op == 0) regs_[rd] = imm;
            if (op == 1) pending_cmp_ = std::make_pair(rd, imm);
            if (op == 2) regs_[rd] = add(regs_[rd], imm);
            if (op == 3) regs_[rd] = sub(regs_[rd], imm);
            return false;
        }
        if ((h & 0xF800) == 0x0000) {  // lsl rd, rs, #imm
            unsigned rd = h & 7, rs = (h >> 3) & 7, sh = (h >> 6) & 31;
            regs_[rd] = regs_[rs] ? opt(*regs_[rs] << sh) : std::nullopt;
            if (sh == 2 && bound0[rs] && *bound0[rs] < 1024) jt_[rd] = Jt{Jt::Scaled, 0, *bound0[rs] + 1};
            return false;
        }
        if ((h & 0xF800) == 0x1800) {  // add/sub rd, rs, rn|#imm3
            unsigned rd = h & 7, rs = (h >> 3) & 7, rn = (h >> 6) & 7;
            opt v = (h & 0x400) ? opt(rn) : regs_[rn];
            if (!(h & 0x600)) {
                if (auto j = scaled_plus(rs, rn)) jt_[rd] = *j;
            } else if ((h & 0x400) && rn == 0) {  // adds rd, rs, #0 is a register move
                jt_[rd] = jt0[rs];
                bound_[rd] = bound0[rs];
            }
            regs_[rd] = (h & 0x200) ? sub(regs_[rs], v) : add(regs_[rs], v);
            return false;
        }
        if ((h & 0xFFC0) == 0x6800 && jt0[(h >> 3) & 7].kind == Jt::Ptr) {  // ldr rd, [rb] from table
            regs_[h & 7].reset();
            jt_[h & 7] = Jt{Jt::Val, jt0[(h >> 3) & 7].base, jt0[(h >> 3) & 7].n};
            return false;
        }
        if ((h & 0xFE00) == 0x5800) {  // ldr rd, [rb, ro]
            regs_[h & 7].reset();
            if (auto j = scaled_plus((h >> 3) & 7, (h >> 6) & 7)) jt_[h & 7] = Jt{Jt::Val, j->base, j->n};
            return false;
        }
        if ((h & 0xF800) == 0x6000) {  // str rd, [rb, #imm]
            unsigned rd = h & 7, rb = (h >> 3) & 7;
            store(out, a, add(regs_[rb], ((h >> 6) & 31) * 4), regs_[rd]);
            return false;
        }
        if ((h & 0xFE00) == 0x5000) {  // str rd, [rb, ro]
            store(out, a, add(regs_[(h >> 3) & 7], regs_[(h >> 6) & 7]), regs_[h & 7]);
            return false;
        }
        if ((h & 0xF800) == 0xC000) {  // stmia rb!, {rlist}
            unsigned rb = (h >> 8) & 7;
            opt ea = regs_[rb];
            for (int r = 0; r < 8; ++r) {
                if (!(h & (1 << r))) continue;
                store(out, a, ea, regs_[r]);
                if (ea) *ea += 4;
            }
            regs_[rb] = ea;
            return false;
        }
        invalidate(d);
        return false;
    }

    // ---- ARM ------------------------------------------------------------------------------
    bool step_arm(Explored& out, std::deque<u32>& work, u32 a, const Decoded& d) {
        int prev = prev_pop_;
        prev_pop_ = -1;
        u32 w = d.raw;
        bool al = (w >> 28) == 0xE;
        auto rv = [&](unsigned r) -> opt { return r == 15 ? opt(a + 8) : regs_[r]; };
        auto set = [&](unsigned r, opt v) { regs_[r] = al ? v : std::nullopt; };

        if ((w & 0x0E000000) == 0x0A000000) {  // b / bl
            int32_t off = int32_t(w << 8) >> 6;
            u32 t = a + 8 + u32(off);
            if (w & (1u << 24)) {
                call(out, a, t, false, false, "bl", d.text);
                if (auto n = swi_wrapper(t, false)) swi(out, a, *n);
                clobber_after_call();
                return false;
            }
            jump(out, work, a, t, d.text);
            return al;
        }
        if ((w & 0x0FFFFFF0) == 0x012FFF10) {  // bx rm
            unsigned rm = w & 15;
            return bx(out, a, 4, rm, rv(rm), al, d.text, prev);
        }
        if ((w & 0x0F000000) == 0x0F000000) {  // swi
            swi(out, a, (w >> 16) & 0xFF);
            return false;
        }
        if ((w & 0x0C100000) == 0x04100000) {  // ldr / ldrb
            unsigned rn = (w >> 16) & 15, rd = (w >> 12) & 15;
            bool literal = (w & 0x0F7F0000) == 0x051F0000;  // ldr rd, [pc, #+/-imm], word, no wb
            opt v;
            if (literal) {
                u32 off = w & 0xFFF;
                u32 la = (w & (1u << 23)) ? a + 8 + off : a + 8 - off;
                if (dec_.in_rom(la, 4)) {
                    out.literals.insert(la);
                    v = dec_.rd32(la);
                    if (rd != 15) literal_pointer(out, *v);
                }
            }
            if (rd == 15) {
                if (literal && v)
                    call(out, a, *v & ~3u, false, true, "ldr pc, =literal", d.text);  // no interworking on v4T
                else if (rn != 13)
                    out.unresolved.push_back({a, false, "ldr pc, [r" + std::to_string(rn) + ", ...] (indirect)", d.text});
                return al;
            }
            if (literal) {
                set(rd, v);
                return false;
            }
            invalidate(d);
            return false;
        }
        if ((w & 0x0E000000) == 0x08000000) {  // ldm / stm
            unsigned rn = (w >> 16) & 15;
            bool load = w & (1u << 20);
            if (load) {
                if (w & 0x8000) {
                    if (rn != 13)
                        out.unresolved.push_back({a, false, "ldm with pc from r" + std::to_string(rn), d.text});
                    return al;
                }
                invalidate(d);
                if (rn == 13)
                    for (int r = 0; r < 15; ++r)
                        if (w & (1u << r)) prev_pop_ = r;
                return false;
            }
            bool p = w & (1u << 24), u = w & (1u << 23), wb = w & (1u << 21);
            if (al && u) {  // stmia / stmib
                opt ea = regs_[rn];
                for (int r = 0; r < 16; ++r) {
                    if (!(w & (1u << r))) continue;
                    if (ea && p) *ea += 4;
                    store(out, a, ea, rv(r));
                    if (ea && !p) *ea += 4;
                }
                if (wb) regs_[rn] = ea;
                return false;
            }
            invalidate(d);
            return false;
        }
        if ((w & 0x0E500000) == 0x04000000) {  // str rd, [rn, #imm]
            unsigned rn = (w >> 16) & 15, rd = (w >> 12) & 15;
            bool p = w & (1u << 24), u = w & (1u << 23), wb = w & (1u << 21);
            u32 off = w & 0xFFF;
            opt base = rv(rn);
            opt moved = base ? opt(u ? *base + off : *base - off) : std::nullopt;
            if (al) store(out, a, p ? moved : base, rv(rd));
            if (wb || !p) set(rn, moved);
            return false;
        }
        if ((w & 0x0C000000) == 0) {  // data processing
            bool imm = w & (1u << 25);
            bool dp = imm || (w & 0x90) != 0x90;
            unsigned op = (w >> 21) & 15, rn = (w >> 16) & 15, rd = (w >> 12) & 15;
            bool s = w & (1u << 20);
            bool test = op >= 8 && op <= 11;
            if (dp && !(test && !s)) {
                if (test) return false;
                opt op2;
                if (imm) op2 = rotr(w & 0xFF, ((w >> 8) & 15) * 2);
                else if (((w >> 4) & 0xFF) == 0) op2 = rv(w & 15);
                if (rd == 15) {
                    if (op == 13 && !imm && ((w >> 4) & 0xFF) == 0 && (w & 15) == 14) return al;  // mov pc, lr
                    if (op == 4 && rn == 15 && !imm)
                        out.unresolved.push_back({a, false, "add pc, pc, rX (jump table)", d.text});
                    else
                        out.unresolved.push_back({a, false, "data-processing write to pc (indirect)", d.text});
                    return al;
                }
                opt x = rv(rn), r;
                bool both = x && op2;
                switch (op) {
                    case 13: r = op2; break;
                    case 15: if (op2) r = ~*op2; break;
                    case 4: if (both) r = *x + *op2; break;
                    case 2: if (both) r = *x - *op2; break;
                    case 3: if (both) r = *op2 - *x; break;
                    case 12: if (both) r = *x | *op2; break;
                    case 0: if (both) r = *x & *op2; break;
                    case 1: if (both) r = *x ^ *op2; break;
                    case 14: if (both) r = *x & ~*op2; break;
                    default: break;
                }
                set(rd, r);
                return false;
            }
        }
        invalidate(d);
        return false;
    }

    Decoder& dec_;
    const std::unordered_set<u64>& entries_;
    bool pointer_literals_ = true;
    std::array<opt, 16> regs_{};
    // GCC Thumb switch: cmp rI, #N; bhi default; lsls rS, rI, #2; ldr rT, =table; adds rS, rT;
    // ldr rS, [rS]; mov pc, rS. Tracked per register; anything else clears it.
    struct Jt { enum Kind { None, Scaled, Ptr, Val } kind = None; u32 base = 0, n = 0; };
    std::array<Jt, 16> jt_{};
    std::array<opt, 16> bound_{};
    std::optional<std::pair<unsigned, u32>> pending_cmp_;
    int prev_pop_ = -1;  // highest register popped by the previous instruction (return idiom)
    std::array<opt, 4> dma_sad_{}, dma_dad_{};
    std::array<int, 4> dma_copy_idx_{};
};

const char* mode_name(bool thumb) { return thumb ? "thumb" : "arm"; }

}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path rom_path, out_dir = "generated";
    bool pointer_literals = true;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if ((a == "-o" || a == "--out") && i + 1 < argc) out_dir = argv[++i];
        else if (a == "--strict") pointer_literals = false;
        else if (a == "-h" || a == "--help") {
            std::cout << "usage: supersaga_disasm <rom.gba> [-o generated] [--strict]\n";
            return 0;
        } else rom_path = a;
    }
    if (rom_path.empty()) {
        std::cerr << "usage: supersaga_disasm <rom.gba> [-o generated] [--strict]\n";
        return 2;
    }

    auto loaded = supersaga::load_rom(rom_path);
    if (!loaded.ok) {
        std::cerr << "error: " << loaded.error << "\n";
        return 1;
    }
    if (!loaded.rom.hash_matches) {
        std::cerr << "error: ROM SHA-1 " << loaded.rom.sha1 << " does not match the expected dump "
                  << (*supersaga::kExpectedSha1 ? supersaga::kExpectedSha1 : supersaga::game::kGameCode) << "\n";
        return 1;
    }
    const auto& rom = loaded.rom.data;

    Decoder dec(rom);
    std::unordered_set<u64> entry_set;
    std::map<u64, std::string> entries;  // key -> why it is a function
    auto add_entry = [&](u64 k, const std::string& why) {
        if (entry_set.insert(k).second) {
            entries.emplace(k, why);
            return true;
        }
        return false;
    };
    add_entry(key(kRomBase, false), "reset vector");

    // Phase 1 discovers entries with a worklist; phase 2 re-traces every function against the
    // final entry set so bodies stop at other functions' starts. Repeat until nothing new.
    std::map<u64, Explored> results;
    int rounds = 0;
    for (;;) {
        ++rounds;
        Tracer tracer(dec, entry_set, pointer_literals);
        std::deque<u64> work;
        for (auto& [k, _] : entries)
            if (!results.count(k)) work.push_back(k);
        while (!work.empty()) {
            u64 k = work.front();
            work.pop_front();
            if (results.count(k)) continue;
            Explored ex = tracer.explore(key_addr(k), key_thumb(k));
            for (auto& [nk, why] : ex.new_entries)
                if (add_entry(nk, why)) work.push_back(nk);
            results[k] = std::move(ex);
        }
        size_t before = entries.size();
        std::map<u64, Explored> again;
        for (auto& [k, _] : entries) {
            Explored ex = tracer.explore(key_addr(k), key_thumb(k));
            for (auto& [nk, why] : ex.new_entries) add_entry(nk, why);
            again[k] = std::move(ex);
        }
        results = std::move(again);
        if (entries.size() == before) break;
    }

    // ---- aggregate ----------------------------------------------------------------------------
    struct Refs { u32 call_sites = 0, tail_refs = 0; std::set<u64> callers; };
    std::map<u64, Refs> refs;
    std::vector<std::pair<u64, Unresolved>> unresolved;
    std::vector<std::pair<u64, Stop>> stops;
    std::vector<std::pair<u64, IrqWrite>> irq;
    std::vector<std::pair<u64, Copy>> copies;
    std::set<u32> labels;
    std::vector<u8> cover(rom.size(), 0);  // bit0 code, bit1 literal, bit2 code seen twice
    for (auto& [k, ex] : results) {
        for (auto& c : ex.calls) {
            auto& r = refs[key(c.target, c.thumb)];
            if (c.tail) ++r.tail_refs; else ++r.call_sites;
            r.callers.insert(k);
        }
        for (auto& u : ex.unresolved) unresolved.emplace_back(k, u);
        for (auto& s : ex.stops) stops.emplace_back(k, s);
        for (auto& i : ex.irq) irq.emplace_back(k, i);
        for (auto& c : ex.copies) copies.emplace_back(k, c);
        labels.insert(ex.labels.begin(), ex.labels.end());
        for (u32 i : ex.insns) {
            const Decoded& d = dec.get(i, ex.thumb);
            for (u32 b = 0; b < d.size; ++b) {
                u8& c = cover[i - kRomBase + b];
                c |= (c & 1) ? 4 : 1;
            }
        }
        for (u32 l : ex.literals)
            for (u32 b = 0; b < 4; ++b) cover[l - kRomBase + b] |= 2;
    }
    // Different functions legitimately share blocks (tail merges), so dedupe by address.
    auto dedupe = [](auto& v) {
        std::sort(v.begin(), v.end(), [](auto& x, auto& y) { return x.second.addr < y.second.addr; });
    };
    {
        std::sort(unresolved.begin(), unresolved.end(),
                  [](auto& x, auto& y) { return x.second.addr < y.second.addr; });
        unresolved.erase(std::unique(unresolved.begin(), unresolved.end(),
                                     [](auto& x, auto& y) {
                                         return x.second.addr == y.second.addr && x.second.kind == y.second.kind;
                                     }),
                         unresolved.end());
        dedupe(stops);
        stops.erase(std::unique(stops.begin(), stops.end(),
                                [](auto& x, auto& y) { return x.second.addr == y.second.addr; }),
                    stops.end());
    }
    std::sort(irq.begin(), irq.end(), [](auto& x, auto& y) { return x.second.site < y.second.site; });
    irq.erase(std::unique(irq.begin(), irq.end(), [](auto& x, auto& y) { return x.second.site == y.second.site; }), irq.end());
    std::sort(copies.begin(), copies.end(), [](auto& x, auto& y) { return x.second.site < y.second.site; });
    copies.erase(std::unique(copies.begin(), copies.end(), [](auto& x, auto& y) { return x.second.site == y.second.site; }), copies.end());

    size_t n_tables = 0, n_table_entries = 0;
    {
        std::set<u32> seen;
        for (auto& [k, ex] : results)
            for (auto& t : ex.tables)
                if (seen.insert(t.site).second) { ++n_tables; n_table_entries += t.n; }
    }
    u64 code_bytes = 0, lit_bytes = 0, shared_bytes = 0;
    for (u8 c : cover) {
        if (c & 1) ++code_bytes;
        else if (c & 2) ++lit_bytes;
        if (c & 4) ++shared_bytes;
    }
    size_t n_arm = 0, n_thumb = 0;
    for (auto& [k, _] : entries) (key_thumb(k) ? n_thumb : n_arm)++;
    std::map<std::string, size_t> unres_kinds;
    for (auto& [_, u] : unresolved) {
        std::string k = u.kind;
        if (auto p = k.find(" 0x"); p != std::string::npos) k = k.substr(0, p);
        if (auto p = k.find(" ("); p != std::string::npos && k.rfind("bx r", 0) == 0) k = "bx rN (unknown target)";
        if (k.rfind("bl _call_via_r", 0) == 0) k = "bl _call_via_rN (unknown target)";
        ++unres_kinds[k];
    }
    std::map<std::string, size_t> why_kinds;
    for (auto& [_, why] : entries) ++why_kinds[why.rfind("bl _call_via_r", 0) == 0 ? "bl _call_via_rN" : why];

    // ---- write output -------------------------------------------------------------------------
    std::filesystem::create_directories(out_dir);
    {
        std::ofstream f(out_dir / "functions.csv");
        f << "address,mode,size,end,code_bytes,insns,call_sites,callers,tail_refs,found_by\n";
        for (auto& [k, ex] : results) {
            auto& r = refs[k];
            f << hex(ex.start) << ',' << mode_name(ex.thumb) << ',' << (ex.hi - ex.lo) << ','
              << hex(ex.hi) << ',' << ex.code_bytes << ',' << ex.insns.size() << ',' << r.call_sites
              << ',' << r.callers.size() << ',' << r.tail_refs << ",\"" << entries[k] << "\"\n";
        }
    }
    {
        std::ofstream f(out_dir / "unresolved.txt");
        f << "# Indirect control flow that was not resolved (address, mode, function, kind, insn)\n";
        for (auto& [k, u] : unresolved)
            f << hex(u.addr) << ' ' << mode_name(u.thumb) << " in " << hex(key_addr(k)) << "  " << u.kind
              << "  |  " << u.text << '\n';
        f << "\n# Paths stopped on bytes that are not valid ARMv4T code (often data after a noreturn call)\n";
        for (auto& [k, s] : stops)
            f << hex(s.addr) << ' ' << mode_name(s.thumb) << " in " << hex(key_addr(k)) << "  " << s.why << '\n';
    }
    {
        std::ofstream f(out_dir / "notes.txt");
        f << "# Writes to the IRQ vector " << hex(kIrqVector) << "\n";
        for (auto& [k, i] : irq) f << hex(i.site) << " in " << hex(key_addr(k)) << "  value " << hexopt(i.value) << '\n';
        f << "\n# Copies from ROM into RAM (possible RAM-resident code or data)\n";
        for (auto& [k, c] : copies)
            f << hex(c.site) << " in " << hex(key_addr(k)) << "  " << c.how << "  " << hex(c.src) << " -> "
              << hex(c.dst) << "  count/ctrl " << hexopt(c.count) << '\n';
    }
    {
        // Full listing in address order, gaps marked as unanalyzed.
        struct Item { bool literal; bool thumb; };
        std::map<u32, Item> items;
        for (auto& [k, ex] : results) {
            for (u32 i : ex.insns) items.emplace(i, Item{false, ex.thumb});
            for (u32 l : ex.literals) items.emplace(l, Item{true, false});
        }
        std::map<u32, std::vector<u64>> func_at;
        for (auto& [k, _] : entries) func_at[key_addr(k)].push_back(k);

        std::ofstream f(out_dir / "supersaga.s");
        f << "@ Generated by supersaga_disasm from the user's own ROM. Do not commit or distribute.\n"
          << "@ ROM " << loaded.rom.header.title << " [" << loaded.rom.header.game_code << "] sha1 "
          << loaded.rom.sha1 << "\n\n";
        u32 next = kRomBase;
        int mode = -1;
        char line[256];
        for (auto& [addr, it] : items) {
            if (addr < next) continue;  // overlapping decode (e.g. literal inside code range)
            if (addr > next)
                f << "\n@ ---- unanalyzed " << hex(next) << " - " << hex(addr) << " (" << (addr - next)
                  << " bytes)\n\n";
            if (auto fa = func_at.find(addr); fa != func_at.end()) {
                for (u64 k : fa->second) {
                    auto& r = refs[k];
                    f << "\n@ function " << mode_name(key_thumb(k)) << ", " << r.call_sites << " call sites, "
                      << r.tail_refs << " tail refs; found by: " << entries[k] << '\n';
                }
                f << (key_thumb(fa->second.front()) ? "\t.thumb_func\n" : "") << "sub_" << std::hex
                  << std::uppercase << addr << std::dec << ":\n";
            } else if (labels.count(addr)) {
                f << "_" << std::hex << std::uppercase << addr << std::dec << ":\n";
            }
            if (it.literal) {
                std::snprintf(line, sizeof line, "\t.word 0x%08X\t\t\t\t@ %08X\n", dec.rd32(addr), addr);
                f << line;
                next = addr + 4;
                continue;
            }
            if (mode != int(it.thumb)) {
                f << (it.thumb ? "\t.thumb\n" : "\t.arm\n");
                mode = it.thumb;
            }
            const Decoded& d = dec.get(addr, it.thumb);
            if (d.size == 4 && it.thumb)
                std::snprintf(line, sizeof line, "\t%-40s @ %08X: %04X %04X\n", d.text.c_str(), addr,
                              d.raw >> 16, d.raw & 0xFFFF);
            else
                std::snprintf(line, sizeof line, it.thumb ? "\t%-40s @ %08X: %04X\n" : "\t%-40s @ %08X: %08X\n",
                              d.text.c_str(), addr, d.raw);
            f << line;
            next = addr + d.size;
        }
        if (next < kRomBase + rom.size())
            f << "\n@ ---- unanalyzed " << hex(next) << " - " << hex(u32(kRomBase + rom.size())) << '\n';
    }

    // ---- summary ------------------------------------------------------------------------------
    auto pct = [&](u64 n) { return 100.0 * double(n) / double(rom.size()); };
    std::printf("supersaga_disasm: %s [%s] sha1 verified, %zu-round fixpoint\n", loaded.rom.header.title.c_str(),
                loaded.rom.header.game_code.c_str(), size_t(rounds));
    std::printf("functions:        %zu (ARM %zu, Thumb %zu)\n", entries.size(), n_arm, n_thumb);
    for (auto& [w, n] : why_kinds) std::printf("  found by %-28s %zu\n", w.c_str(), n);
    std::printf("code bytes:       %llu (%.2f%% of %zu)\n", (unsigned long long)code_bytes, pct(code_bytes), rom.size());
    std::printf("literal bytes:    %llu (%.2f%%)\n", (unsigned long long)lit_bytes, pct(lit_bytes));
    std::printf("shared code:      %llu bytes reached from more than one function\n", (unsigned long long)shared_bytes);
    std::printf("jump tables:      %zu resolved (%zu entries, bound proven by cmp/bhi)\n", n_tables, n_table_entries);
    std::printf("unresolved:       %zu indirect branches/calls\n", unresolved.size());
    for (auto& [k, n] : unres_kinds) std::printf("  %-44s %zu\n", k.c_str(), n);
    std::printf("decode stops:     %zu (paths that ran into non-ARMv4T bytes)\n", stops.size());
    std::printf("IRQ vector writes:%zu\n", irq.size());
    for (auto& [k, i] : irq)
        std::printf("  %s in sub_%08X writes %s\n", hex(i.site).c_str(), key_addr(k), hexopt(i.value).c_str());
    std::printf("ROM->RAM copies:  %zu\n", copies.size());
    for (auto& [k, c] : copies)
        std::printf("  %s in sub_%08X %s %s -> %s count/ctrl %s\n", hex(c.site).c_str(), key_addr(k),
                    c.how.c_str(), hex(c.src).c_str(), hex(c.dst).c_str(), hexopt(c.count).c_str());
    std::printf("output:           %s/{functions.csv,unresolved.txt,notes.txt,supersaga.s}\n", out_dir.string().c_str());
    return 0;
}
