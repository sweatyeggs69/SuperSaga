// Save state serialization. Members are written in a fixed order as raw bytes; a fingerprint of
// the struct sizes guards against loading a state from a build with a different layout.
#include "gba.h"

#include <cstring>
#include <type_traits>

namespace supersaga::gba {

namespace {
constexpr char kMagic[8] = {'M', 'F', 'U', 'S', 'G', 'B', 'A', 'S'};
constexpr u32 kVersion = 1;

class Writer {
public:
    explicit Writer(std::vector<u8>& out) : out_(out) {}
    template <class T>
    void put(const T& v) {
        static_assert(std::is_trivially_copyable_v<T>);
        const u8* p = reinterpret_cast<const u8*>(&v);
        out_.insert(out_.end(), p, p + sizeof(T));
    }
private:
    std::vector<u8>& out_;
};

class Reader {
public:
    explicit Reader(const std::vector<u8>& in) : in_(in) {}
    template <class T>
    bool get(T& v) {
        static_assert(std::is_trivially_copyable_v<T>);
        if (pos_ + sizeof(T) > in_.size()) return false;
        std::memcpy(&v, in_.data() + pos_, sizeof(T));
        pos_ += sizeof(T);
        return true;
    }
    bool done() const { return pos_ == in_.size(); }
private:
    const std::vector<u8>& in_;
    size_t pos_ = 0;
};
}  // namespace

// The order of fields here is the format. Bump kVersion when it changes.
#define SUPERSAGA_STATE_FIELDS(X)                                                                    \
    X(ewram_) X(iwram_) X(io_) X(pal_) X(vram_) X(oam_) X(sram_) X(sram_dirty_)                  \
    X(r_) X(cpsr_) X(bank_r13_) X(bank_r14_) X(bank_spsr_) X(bank_fiq_r8_) X(bank_usr_r8_)        \
    X(pc_) X(halted_) X(intr_wait_) X(bios_latch_) X(last_fetch_) X(wait_n_) X(wait_s_)          \
    X(keys_pressed_) X(dma_) X(timer_)                                                          \
    X(line_cycle_) X(vcount_) X(hblank_) X(bg_ref_x_) X(bg_ref_y_) X(frame_) X(frame_count_)     \
    X(sq_) X(wave_) X(wave_ram_) X(noise_) X(fifo_) X(apu_sample_acc_) X(apu_seq_acc_)          \
    X(apu_seq_step_) X(eeprom_size_) X(eeprom_in_) X(eeprom_in_count_) X(eeprom_read_addr_)          \
    X(eeprom_read_pos_)

// A state saved under one BIOS mode (real BIOS vs. HLE) can be loaded under the other only at a
// point where no BIOS code is mid-way: the game itself running, or the game waiting in Halt,
// IntrWait or VBlankIntrWait, which is then backed out to its SWI instruction and simply executed
// again under the new mode.
namespace {
// The real BIOS SWI handler pushes {r11, r12, lr} and then the caller's CPSR on the SVC stack,
// switches to System mode, pushes {r2, lr} on the caller's stack and calls the SWI with
// lr = kSwiReturn. The waits are leaves that halt with lr untouched, using r0, r1, r3, r11 and
// r12 as scratch.
constexpr u32 kSwiReturn = 0x170;
// IntrWait's halt loop in the real BIOS (strb to HALTCNT, then the flag check).
constexpr u32 kIntrWaitLoop = 0x344;
}  // namespace

bool Gba::convert_bios_mode(Gba& t, bool from_real) {
    const u32 mode = t.cpsr_ & 0x1F;
    if (mode == 0x12) return false;  // inside an IRQ (the dispatcher differs between modes)
    if (!from_real) {
        // HLE -> real BIOS. Only our IRQ stub lives in the BIOS region under HLE.
        if (t.pc_ < 0x4000) return false;
        if (t.intr_wait_) {  // re-execute the SWI at pc_ through the real BIOS
            t.intr_wait_ = false;
            t.halted_ = false;
        }
        return true;
    }
    // Real BIOS -> HLE.
    if (t.pc_ >= 0x4000) return true;  // game code (not inside a SWI)
    auto rd8 = [&](u32 a) -> int {
        switch (a >> 24) {
            case 0x02: return t.ewram_[a & 0x3FFFF];
            case 0x03: return t.iwram_[a & 0x7FFF];
            case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D:
                return (a & 0x01FFFFFF) < rom_.size() ? rom_[a & 0x01FFFFFF] : -1;
            default: return -1;
        }
    };
    auto rd32 = [&](u32 a, u32& v) {
        if ((a >> 24) != 0x03 || (a & 3)) return false;  // the BIOS keeps its stacks in IWRAM
        v = 0;
        for (int i = 3; i >= 0; --i) v = (v << 8) | u32(rd8(a + u32(i)));
        return true;
    };
    // Registers of the caller as they will be once the SWI has been backed out.
    u32 caller, ret, sp_svc, r11 = 0, r12 = 0;
    bool from_sys_wait = false;
    u32 sp_sys = 0, r2 = 0, lr_sys = 0, r4 = 0;
    bool in_intr_wait = false;
    if (mode == 0x13) {
        // Still in SVC mode: the SWI entry has not switched to System mode yet.
        caller = t.bank_spsr_[3];
        ret = t.r_[14];
        sp_svc = t.r_[13];
    } else if (mode == 0x1F && t.halted_) {
        // Halted inside a BIOS wait in System mode, where some games spend most of every frame.
        // Halt and VBlankIntrWait halt with lr = kSwiReturn. IntrWait's loop (0x344-0x34C) first
        // pushes {r4, lr = kSwiReturn} and keeps the mask in r1 throughout.
        sp_sys = t.r_[13];
        if (t.pc_ >= kIntrWaitLoop && t.pc_ <= kIntrWaitLoop + 8) {
            u32 lr_wait = 0;
            if (!rd32(sp_sys, r4) || !rd32(sp_sys + 4, lr_wait) || lr_wait != kSwiReturn) return false;
            sp_sys += 8;
            in_intr_wait = true;
        } else if (t.r_[14] != kSwiReturn) {
            return false;
        }
        if (!rd32(sp_sys, r2) || !rd32(sp_sys + 4, lr_sys)) return false;
        sp_svc = t.bank_r13_[3];
        if (!rd32(sp_svc, caller) || !rd32(sp_svc + 4, r11) || !rd32(sp_svc + 8, r12) ||
            !rd32(sp_svc + 12, ret))
            return false;
        if (caller != t.bank_spsr_[3]) return false;  // the SVC SPSR still holds the caller's CPSR
        from_sys_wait = true;
    } else {
        return false;
    }
    const u32 caller_mode = caller & 0x1F;
    if (caller_mode == 0x12 || caller_mode == 0x13) return false;
    if (from_sys_wait && caller_mode != 0x1F && caller_mode != 0x10) return false;
    const bool thumb = caller & 0x20;
    const u32 swi_at = ret - (thumb ? 2 : 4);
    int b0 = rd8(swi_at), b1 = rd8(swi_at + 1), b2 = rd8(swi_at + 2), b3 = thumb ? 0 : rd8(swi_at + 3);
    if (b0 < 0 || b1 < 0 || b2 < 0 || b3 < 0) return false;
    u32 number;
    if (thumb) {
        if (b1 != 0xDF) return false;
        number = u32(b0);
    } else {
        if ((b3 & 0x0F) != 0x0F) return false;
        number = u32(b2);
    }
    // Only waits, which change nothing when run again from the start. Halted in System mode
    // outside IntrWait's loop, IntrWait's mask in r1 may already be overwritten, so there only
    // Halt and VBlankIntrWait (which sets its own arguments) qualify.
    // Inside IntrWait's own loop r1 still holds the mask, so IntrWait (and VBlankIntrWait, which
    // runs the same loop) qualify there too, run again with r0 = 1: wait for the next of those
    // interrupts, as it was doing.
    const bool ok = in_intr_wait ? ((number == 4 && (t.r_[1] & 0x3FFF) != 0) || number == 5)
                    : from_sys_wait ? (number == 2 || number == 5)
                                    : (number == 4 || number == 5);
    if (!ok) return false;
    if (in_intr_wait) {
        t.r_[0] = 1;
        t.r_[4] = r4;
    }
    // Leave the BIOS: back to the caller's mode with both stacks popped, then run the SWI again.
    if (from_sys_wait) {
        // Pop what the BIOS pushed: {r2, lr} on the System stack and {spsr, r11, r12, lr} on the
        // SVC stack. r0, r1 and r3 are scratch for these waits.
        t.r_[2] = r2;
        t.r_[11] = r11;
        t.r_[12] = r12;
        t.r_[13] = sp_sys + 8;
        t.r_[14] = lr_sys;
        t.bank_r13_[3] = sp_svc + 16;
        t.bank_r14_[3] = ret;
        t.cpsr_ = caller;  // usr/sys share one register bank, so no bank switch is needed
    } else {
        t.r_[13] = 0x03007FE0;
        t.set_cpsr(caller);
    }
    t.pc_ = swi_at;
    t.halted_ = false;
    t.intr_wait_ = false;
    return true;
}

std::vector<u8> Gba::save_state() const {
    std::vector<u8> out;
    out.reserve(0x60000);
    Writer w(out);
    for (char c : kMagic) w.put(c);
    w.put(kVersion);
    u32 fingerprint = 0;
#define SUPERSAGA_FP(m) fingerprint = fingerprint * 131 + u32(sizeof(m));
    SUPERSAGA_STATE_FIELDS(SUPERSAGA_FP)
#undef SUPERSAGA_FP
    w.put(fingerprint);
    w.put(real_bios_);
#define SUPERSAGA_PUT(m) w.put(m);
    SUPERSAGA_STATE_FIELDS(SUPERSAGA_PUT)
#undef SUPERSAGA_PUT
    return out;
}

bool Gba::load_state(const std::vector<u8>& data, std::string* error) {
    auto fail = [&](const char* why) {
        if (error) *error = why;
        return false;
    };
    Reader r(data);
    char magic[8];
    for (char& c : magic)
        if (!r.get(c)) return fail("The save state is truncated.");
    if (std::memcmp(magic, kMagic, sizeof magic) != 0) return fail("This is not a SuperSaga save state.");
    u32 version = 0, fingerprint = 0, expected = 0;
    bool state_real_bios = false;
    if (!r.get(version) || version != kVersion)
        return fail("This save state was made by a different version of SuperSaga and can't be loaded.");
#define SUPERSAGA_FP(m) expected = expected * 131 + u32(sizeof(m));
    SUPERSAGA_STATE_FIELDS(SUPERSAGA_FP)
#undef SUPERSAGA_FP
    if (!r.get(fingerprint) || fingerprint != expected)
        return fail("This save state was made by a different build of SuperSaga and can't be loaded.");
    if (!r.get(state_real_bios)) return fail("The save state is truncated.");

    // Decode into a copy first so a bad file never leaves the machine half-loaded.
    auto tmp = std::make_unique<Gba>(std::vector<u8>{}, std::filesystem::path{});
    Gba& t = *tmp;
#define SUPERSAGA_GET(m) if (!r.get(t.m)) return fail("The save state is truncated.");
    SUPERSAGA_STATE_FIELDS(SUPERSAGA_GET)
#undef SUPERSAGA_GET
    if (!r.done()) return fail("The save state has unexpected extra data.");
    if (state_real_bios != real_bios_ && !convert_bios_mode(t, state_real_bios))
        return fail("This save state was made with a different BIOS setting at a moment that can't be "
                    "converted (the BIOS or an interrupt was running). Switch the BIOS setting back to load it.");

#define SUPERSAGA_COPY(m) m = t.m;
    SUPERSAGA_STATE_FIELDS(SUPERSAGA_COPY)
#undef SUPERSAGA_COPY
    branched_ = false;
    cycles_ = 0;
    frame_done_ = false;
    audio_.clear();
    sram_dirty_ = false;  // the save file is updated the next time the game writes SRAM
    sram_dirty_frames_ = 0;
    wide_level_ = {};
    wide_scene_ = false;
    wide_main_bg_ = -1;
    wide_hud_prio_ = -1;
    return true;
}

#undef SUPERSAGA_STATE_FIELDS

}  // namespace supersaga::gba
