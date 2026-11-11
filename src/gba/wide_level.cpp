// Widescreen hook for the game. The PPU asks wide_level_detect() once per frame what the extra
// columns at the sides of a widescreen view should show (see WideMode in gba.h). Superstar Saga
// support is not written yet, so no scene widens and the sides stay black; the settings menu
// hides the widescreen option until it is (kShowWidescreen in options_panel.cpp). A game port
// fills this in from the game's own level layout in RAM; only addresses and layout descriptions
// belong here, never game data.
#include "gba.h"

namespace supersaga::gba {

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

u16 Gba::wide_level_entry(int, s32, s32) const { return 0; }

void Gba::wide_level_detect() {
    for (auto& L : wide_level_) L = WideLevel{};
    wide_scene_ = false;
    wide_main_bg_ = -1;
    wide_hud_prio_ = -1;
}

}  // namespace supersaga::gba
