#pragma once
// License texts compiled into the program, so they travel with every build (the Windows release
// is a single .exe with no folder of text files next to it).

#include <string_view>

namespace supersaga::licenses {

std::string_view supersaga();        // LICENSE (MIT)
std::string_view notices();     // THIRD_PARTY_NOTICES.md
std::string_view font_ofl();    // SIL Open Font License for the DM Sans font
// All of the above, for --licenses.
void print_all();

}  // namespace supersaga::licenses
