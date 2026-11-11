#include "app/licenses.h"

#include <cstdio>

extern const unsigned char supersaga_text_license[];
extern const unsigned supersaga_text_license_size;
extern const unsigned char supersaga_text_notices[];
extern const unsigned supersaga_text_notices_size;
extern const unsigned char supersaga_text_dmsans_ofl[];
extern const unsigned supersaga_text_dmsans_ofl_size;

namespace supersaga::licenses {

namespace {
std::string_view view(const unsigned char* p, unsigned n) { return {reinterpret_cast<const char*>(p), n}; }
}  // namespace

std::string_view supersaga() { return view(supersaga_text_license, supersaga_text_license_size); }
std::string_view notices() { return view(supersaga_text_notices, supersaga_text_notices_size); }
std::string_view font_ofl() { return view(supersaga_text_dmsans_ofl, supersaga_text_dmsans_ofl_size); }

void print_all() {
    for (std::string_view t : {supersaga(), notices(), font_ofl()}) {
        std::fwrite(t.data(), 1, t.size(), stdout);
        std::fputs("\n\n", stdout);
    }
}

}  // namespace supersaga::licenses
