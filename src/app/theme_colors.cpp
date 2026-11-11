// SuperSaga's colors for the shared launcher theme (theme.h): warm dark brown-red surfaces, a
// flame orange accent and a gold focus ring, after the SuperSaga logo's orange-to-gold letters.
// This is the only theme file that differs between ports.
#include "app/theme.h"

namespace supersaga::theme {

namespace {
ImVec4 rgb(int r, int g, int b, float a = 1.0f) { return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a); }
}  // namespace

const Palette& palette() {
    static const Palette p{
        rgb(28, 15, 12),            // surface
        rgb(38, 21, 17),            // card
        rgb(51, 29, 23),            // raised
        rgb(66, 39, 31),            // high
        rgb(240, 180, 150, 0.15f),  // line
        rgb(255, 246, 238),         // ink
        rgb(214, 184, 166),         // ink_muted
        rgb(158, 128, 112),         // ink_faint
        rgb(240, 110, 4),           // accent
        rgb(247, 150, 60),          // accent_hover
        rgb(214, 80, 6),            // accent_active
        rgb(240, 110, 4, 0.16f),    // accent_soft
        rgb(30, 12, 4),             // on_accent
        rgb(247, 190, 40),          // highlight
        rgb(52, 211, 153),          // good
        rgb(248, 113, 113),         // bad
        rgb(150, 40, 20),           // glow
    };
    return p;
}

}  // namespace supersaga::theme
