#pragma once
// supersaga_gba: Game Boy Advance runtime used to run the game while the static recompiler is built.
// ARM7TDMI interpreter, memory bus, PPU, APU, DMA, timers and a high-level emulation of the
// BIOS calls (no Nintendo BIOS is needed or shipped). No SDL dependency.

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace supersaga::gba {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8 = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

inline constexpr int kScreenWidth = 240;
inline constexpr int kScreenHeight = 160;
// Optional widescreen rendering: the PPU can draw extra columns on each side of the 240-pixel
// view (whatever the game left in VRAM/OAM there). 22 per side gives a 284-pixel (16:9) line;
// wider is not useful because the game stops drawing objects about 32 pixels past the view.
inline constexpr int kMaxWideMargin = 22;
inline constexpr int kMaxLineWidth = kScreenWidth + 2 * kMaxWideMargin;
inline constexpr int kAudioRate = 32768;
inline constexpr u32 kCpuHz = 16777216;
inline constexpr u32 kCyclesPerLine = 1232;
inline constexpr u32 kLinesPerFrame = 228;
inline constexpr u32 kCyclesPerFrame = kCyclesPerLine * kLinesPerFrame;  // 280896, ~59.73 Hz

// KEYINPUT bit order.
enum Key : u16 {
    kKeyA = 1 << 0,
    kKeyB = 1 << 1,
    kKeySelect = 1 << 2,
    kKeyStart = 1 << 3,
    kKeyRight = 1 << 4,
    kKeyLeft = 1 << 5,
    kKeyUp = 1 << 6,
    kKeyDown = 1 << 7,
    kKeyR = 1 << 8,
    kKeyL = 1 << 9,
};

class Gba {
public:
    // save_path: battery save file. Loaded if it exists, written by flush_save(). The save chip
    // (SRAM or serial EEPROM) is picked from the library string in the ROM, as on the cartridge.
    Gba(std::vector<u8> rom, std::filesystem::path save_path);
    ~Gba();
    Gba(const Gba&) = delete;
    Gba& operator=(const Gba&) = delete;

    // Optional real BIOS image (16 KiB) used instead of the BIOS HLE, for reference/debugging.
    // Call before reset(). The port never requires it.
    void use_bios(std::vector<u8> bios);
    bool using_real_bios() const { return real_bios_; }
    // With a real BIOS: start at 0x00000000 in the power-on state so the BIOS intro (logo and
    // chime) plays before the game. Takes effect on the next reset(). Ignored without a BIOS.
    void set_bios_boot(bool boot) { bios_boot_ = boot; }

    void reset();
    // Runs until the start of the next VBlank, so framebuffer() holds a complete frame.
    void run_frame();
    // 240x160 pixels, BGR555 (GBA native, bit 15 unused).
    const u16* framebuffer() const { return frame_.data(); }
    // Widescreen: extra columns per side to render (0 = off, the default). framebuffer() is
    // always the plain 240x160 view; wide_framebuffer() is wide_width() x 160 with the same
    // view in the middle.
    void set_wide_margin(int margin);
    int wide_margin() const { return wide_margin_; }
    int wide_width() const { return kScreenWidth + 2 * wide_margin_; }
    const u16* wide_framebuffer() const { return wide_margin_ ? wide_frame_.data() : frame_.data(); }
    // Bitmask of Key values currently held.
    void set_keys(u16 pressed);
    // Moves generated audio (interleaved stereo s16 at kAudioRate) into out; returns frame count.
    size_t take_audio(std::vector<s16>& out);
    // Writes the save memory to disk if it changed since the last flush.
    void flush_save();

    // Save states. Call between frames (after run_frame). The data is versioned and carries a
    // layout fingerprint; load_state() refuses data from an incompatible build. Display-only
    // state (widescreen margin/output, per-line scratch buffers) is not included.
    std::vector<u8> save_state() const;
    bool load_state(const std::vector<u8>& data, std::string* error = nullptr);

    u64 frame_count() const { return frame_count_; }
    // Read-only views of game memory (for achievements and tools).
    std::span<const u8> iwram() const { return iwram_; }
    std::span<const u8> ewram() const { return ewram_; }
    // Battery save contents: 32 KiB SRAM, or the EEPROM (8 KiB, or 512 bytes for the small chip).
    std::span<const u8> sram() const { return std::span<const u8>(sram_).first(save_size()); }
    bool has_eeprom() const { return eeprom_; }
    std::size_t save_size() const { return eeprom_ ? eeprom_size_ : sram_.size(); }
    // Writable IWRAM and EWRAM, for game tweaks (cheats). Call between frames.
    std::span<u8> iwram_mut() { return iwram_; }
    std::span<u8> ewram_mut() { return ewram_; }
    // Writable ROM image (this Gba's own copy), for in-memory code tweaks. Call between frames.
    std::span<u8> rom_mut() { return rom_; }
    std::string debug_state() const;
    // Logs unimplemented BIOS calls / unusual events to stderr when true.
    bool verbose = false;

private:
    // ---- scheduler -------------------------------------------------------------------------
    void tick(u32 cycles);
    u32 cycles_until_event() const;

    // ---- CPU (cpu.cpp) ---------------------------------------------------------------------
    void cpu_reset();
    void cpu_step();
    void exec_arm(u32 op);
    void exec_thumb(u16 op);
    bool cond_passed(u32 cond) const;
    void set_pc(u32 v);
    void switch_mode(u32 mode);
    u32 get_spsr() const;
    void set_spsr(u32 v);
    bool has_spsr() const;
    void set_cpsr(u32 v);
    void enter_irq();
    void raise_swi();
    u32 user_reg(int i) const;
    void set_user_reg(int i, u32 v);
    void arm_data_processing(u32 op);
    void arm_single_transfer(u32 op);
    void arm_halfword_transfer(u32 op);
    void arm_block_transfer(u32 op);
    u32 alu_add(u32 a, u32 b, u32 carry_in, bool s);
    u32 alu_sub(u32 a, u32 b, u32 carry_in, bool s);
    void set_nz(u32 v);
    u32 load32_rot(u32 addr);
    u32 load16_rot(u32 addr);
    u32 load16_signed(u32 addr);

    // ---- bus (bus.cpp) ---------------------------------------------------------------------
    u8 read8(u32 a);
    u16 read16(u32 a);
    u32 read32(u32 a);
    void write8(u32 a, u8 v);
    void write16(u32 a, u16 v);
    void write32(u32 a, u32 v);
    u16 fetch16(u32 a);
    u32 fetch32(u32 a);
    u32 open_bus(u32 a) const;
    u32 vram_offset(u32 a) const;
    u16 io_read16(u32 a);
    void io_write16(u32 a, u16 v);
    void io_write8(u32 a, u8 v);
    void update_waitstates();
    void load_save();
    // Serial EEPROM: the game streams commands one bit per halfword through DMA3.
    bool eeprom_at(u32 a) const;
    u16 eeprom_read_bit();
    void eeprom_write_bit(u16 v);
    void eeprom_command(u32 bits);

    // ---- DMA / timers / IRQ (system.cpp) ---------------------------------------------------
    void request_irq(int bit);
    void dma_write_control(int ch, u16 v);
    void dma_trigger(int timing);  // 1 = VBlank, 2 = HBlank
    void dma_run(int ch);
    void dma_fifo_request(int fifo);
    void timers_advance(u32 cycles);
    void timer_overflow(int i);
    void timer_write_control(int i, u16 v);
    u32 timer_cycles_to_overflow() const;

    // ---- PPU (ppu.cpp) ---------------------------------------------------------------------
    void ppu_advance(u32 cycles);
    void ppu_render_line(int y);
    void ppu_render_text_bg(int bg, int y, u16* color, u8* opaque);
    // Widescreen level reconstruction (wide_level.cpp, specific to this game).
    void wide_level_detect();
    u16 wide_level_entry(int bg, s32 tx, s32 ty) const;  // entry of wide_level_[bg] at level tile (tx, ty)
    u16 wide_ram16(u32 a) const;
    u32 wide_ram32(u32 a) const;
    void ppu_render_affine_bg(int bg, u16* color, u8* opaque);
    void ppu_render_bitmap(int mode, u16* color, u8* opaque);
    void ppu_render_objects(int y);
    void ppu_write_bg_ref(int index);

    // ---- APU (apu.cpp) ---------------------------------------------------------------------
    void apu_reset();
    void apu_advance(u32 cycles);
    void apu_write(u32 reg, u16 v, u16 old);
    void apu_fifo_push(int fifo, u32 data, int bytes);
    void apu_timer_overflow(int timer);
    void apu_frame_sequencer();
    void apu_sample();
    s32 fifo_out16(int f) const;
    s32 fifo_filtered16(int f);
    void apu_trigger(int ch);

    // ---- BIOS HLE (bios.cpp) ---------------------------------------------------------------
    void bios_install();
    // Returns false if the SWI should not advance the PC (IntrWait waiting for an interrupt).
    bool hle_swi(u32 number);
    void hle_cpu_set(bool fast);
    void hle_lz77(bool vram);
    void hle_rl(bool vram);
    void hle_huffman();
    void hle_bit_unpack();
    void hle_diff_unfilter(bool bits16, bool vram);
    void hle_bg_affine_set();
    void hle_obj_affine_set();
    void hle_register_ram_reset(u32 flags);
    void hle_soft_reset();
    void write_decompressed(u32 dst, const std::vector<u8>& data, bool vram);
    bool convert_bios_mode(Gba& snapshot, bool from_real_bios);

    // ---- state -----------------------------------------------------------------------------
    std::vector<u8> rom_;
    std::filesystem::path save_path_;
    std::array<u8, 0x4000> bios_{};
    std::array<u8, 0x40000> ewram_{};
    std::array<u8, 0x8000> iwram_{};
    std::array<u8, 0x400> io_{};
    std::array<u8, 0x400> pal_{};
    std::array<u8, 0x18000> vram_{};
    std::array<u8, 0x400> oam_{};
    std::array<u8, 0x8000> sram_{};  // SRAM, or the EEPROM contents (first save_size() bytes)
    bool eeprom_ = false;  // from the ROM (EEPROM_V library string), not part of save states
    u32 eeprom_size_ = 0x2000;
    std::array<u8, 81> eeprom_in_{};  // bits written since the last command
    u32 eeprom_in_count_ = 0;
    u32 eeprom_read_addr_ = 0;       // byte offset of the block being read
    u32 eeprom_read_pos_ = 68;       // bit of the 68-bit read reply (68 = none pending)
    bool real_bios_ = false;
    bool bios_boot_ = false;
    bool sram_dirty_ = false;
    u32 sram_dirty_frames_ = 0;

    // CPU
    std::array<u32, 16> r_{};
    u32 cpsr_ = 0;
    std::array<u32, 6> bank_r13_{}, bank_r14_{}, bank_spsr_{};
    std::array<u32, 5> bank_fiq_r8_{}, bank_usr_r8_{};
    u32 pc_ = 0;  // address of the instruction about to execute
    bool branched_ = false;
    u32 cycles_ = 0;  // cycles used by the current instruction
    bool halted_ = false;
    bool intr_wait_ = false;  // an IntrWait SWI is waiting and will be re-executed
    u32 bios_latch_ = 0xE129F000;
    u32 last_fetch_ = 0;
    std::array<u32, 16> wait_n_{}, wait_s_{};  // per region (addr >> 24): 16-bit access cycles

    // System
    u16 keys_pressed_ = 0;
    struct Dma {
        u32 src = 0, dst = 0, count = 0;  // internal (latched) registers
        u16 control = 0;
        bool active = false;
    };
    std::array<Dma, 4> dma_{};
    struct Timer {
        u16 counter = 0, reload = 0, control = 0;
        u32 prescale_acc = 0;
    };
    std::array<Timer, 4> timer_{};

    // PPU
    u32 line_cycle_ = 0;
    u32 vcount_ = 0;
    bool hblank_ = false;
    std::array<s32, 2> bg_ref_x_{}, bg_ref_y_{};  // internal affine reference points (BG2, BG3)
    std::array<u16, kScreenWidth * kScreenHeight> frame_{};
    // Per-line layer buffers, indexed by screen x + wide_margin_.
    std::array<std::array<u16, kMaxLineWidth>, 4> bg_color_{};
    std::array<std::array<u8, kMaxLineWidth>, 4> bg_opaque_{};
    std::array<u16, kMaxLineWidth> obj_color_{};
    std::array<u8, kMaxLineWidth> obj_prio_{};  // 4 = none
    std::array<u8, kMaxLineWidth> obj_semi_{};
    std::array<u8, kMaxLineWidth> obj_window_{};
    int wide_margin_ = 0;
    std::vector<u16> wide_frame_;  // wide_width() x kScreenHeight when wide_margin_ > 0
    // Not emulation state: rebuilt from RAM/VRAM/IO at line 0 of every frame.
    // What the extra widescreen columns of each background show (decided in wide_level.cpp):
    // Vram = the BG's own map wrapped as on hardware, Level = entries rebuilt from the level
    // layout in RAM, Hide = transparent (the VRAM there would be stale).
    enum class WideMode : u8 { Vram, Level, Hide };
    struct WideLevel {
        WideMode mode = WideMode::Vram;
        int layer = 0;                 // the game's own layer index for this BG
        u32 map = 0;
        s32 width = 0, height = 0;     // in the layout's own units
        s32 tiles_w = 0, tiles_h = 0;  // 8x8 tiles
        s32 cam_x = 0, cam_y = 0;      // offset to add to HOFS/VOFS for absolute level pixels
    };
    std::array<WideLevel, 4> wide_level_{};  // per BG
    bool wide_scene_ = false;  // widen this frame at all (otherwise the sides stay black)
    int wide_main_bg_ = -1;    // in a wide scene: side columns outside this BG's level are black
    int wide_hud_prio_ = -1;   // in a wide scene: objects of this priority stay in the 240 view
    bool frame_done_ = false;
    u64 frame_count_ = 0;

    // APU
    struct Square {
        bool on = false;
        u32 freq = 0, duty = 0, phase = 0;  // phase: 16.16 position in the 8-step duty cycle
        u32 length = 0;
        bool length_en = false;
        u32 volume = 0, env_step = 0, env_counter = 0;
        bool env_up = false;
        u32 sweep_period = 0, sweep_shift = 0, sweep_counter = 0;
        bool sweep_down = false;
    };
    std::array<Square, 2> sq_{};
    struct Wave {
        bool on = false;
        u32 freq = 0, phase = 0, length = 0;
        bool length_en = false;
    } wave_;
    std::array<u8, 32> wave_ram_{};  // two 16-byte banks
    struct Noise {
        bool on = false;
        u32 lfsr = 0x7FFF, length = 0, volume = 0, env_step = 0, env_counter = 0;
        bool length_en = false, env_up = false, narrow = false;
        u32 period = 0, acc = 0;  // in 2^24 Hz cycles
    } noise_;
    struct Fifo {
        std::array<s8, 32> data{};
        u32 read = 0, size = 0;
        s8 latch = 0;
        // For output: the sample before latch, and how far playback has moved from it toward
        // latch (cycles since the timer took latch, out of the timer period). Output samples
        // interpolate between the two instead of holding each one, which keeps the images of
        // low-rate DMA sound (~15.7 kHz here) from showing up as hiss in the 32 kHz output.
        s8 prev = 0;
        u32 since = 0, period = 1;
        // Low-pass (Butterworth biquad, cutoff 0.45 x the DMA sound's rate) for what is left of
        // those images; coefficients follow the timer period they were made for.
        float x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        float b0 = 1, b1 = 0, a1 = 0, a2 = 0;
        u32 coef_period = 0;
    };
    std::array<Fifo, 2> fifo_{};
    u32 apu_sample_acc_ = 0;
    u32 apu_seq_acc_ = 0;
    u32 apu_seq_step_ = 0;
    std::vector<s16> audio_;
};

}  // namespace supersaga::gba
