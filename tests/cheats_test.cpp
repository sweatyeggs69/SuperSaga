// Cheats: refills the brothers' HP and BP and sets coins in a blank EWRAM image. No ROM needed.
#include <cstdint>
#include <cstdio>
#include <vector>

#include "app/cheats.h"

namespace {
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)
constexpr std::size_t kMario = 0x4866, kLuigi = 0x48A2, kCoins = 0x48E0;
std::uint16_t get16(const std::vector<std::uint8_t>& m, std::size_t at) { return std::uint16_t(m[at] | (m[at + 1] << 8)); }
void set16(std::vector<std::uint8_t>& m, std::size_t at, std::uint16_t v) { m[at] = std::uint8_t(v); m[at + 1] = std::uint8_t(v >> 8); }
void stats(std::vector<std::uint8_t>& m, std::size_t base, std::uint16_t hp, std::uint16_t max_hp, std::uint16_t bp,
           std::uint16_t max_bp) {
    set16(m, base + 0, hp);
    set16(m, base + 2, max_hp);
    set16(m, base + 6, bp);
    set16(m, base + 8, max_bp);
}
}  // namespace

int main() {
    namespace cheats = supersaga::cheats;
    using cheats::Flags;
    std::vector<std::uint8_t> ram(0x40000, 0);

    // Nothing enabled, or no file loaded (maximums 0): no writes.
    CHECK(!cheats::apply(ram, Flags{}));
    CHECK(!cheats::apply(ram, Flags{true, true, true}));
    CHECK(get16(ram, kCoins) == 0);
    // Out-of-range maximums (other data in that RAM) are left alone too.
    stats(ram, kMario, 5, 0xFFFF, 5, 10);
    stats(ram, kLuigi, 5, 12, 5, 10);
    CHECK(!cheats::apply(ram, Flags{true, true, true}));

    // Mario 4/10 HP 3/10 BP, Luigi 7/12 HP 0/10 BP, 25 coins.
    stats(ram, kMario, 4, 10, 3, 10);
    stats(ram, kLuigi, 7, 12, 0, 10);
    set16(ram, kCoins, 25);

    CHECK(cheats::apply(ram, Flags{true, false, false}));
    CHECK(get16(ram, kMario) == 10 && get16(ram, kLuigi) == 12);
    CHECK(get16(ram, kMario + 6) == 3 && get16(ram, kLuigi + 6) == 0);  // BP untouched
    CHECK(get16(ram, kCoins) == 25);
    CHECK(!cheats::apply(ram, Flags{true, false, false}));  // already full

    CHECK(cheats::apply(ram, Flags{false, true, false}));
    CHECK(get16(ram, kMario + 6) == 10 && get16(ram, kLuigi + 6) == 10);
    CHECK(get16(ram, kMario + 2) == 10 && get16(ram, kMario + 8) == 10);  // maximums never change

    CHECK(cheats::apply(ram, Flags{false, false, true}));
    CHECK(get16(ram, kCoins) == 9999);

    if (failures) std::fprintf(stderr, "%d check(s) failed\n", failures);
    else std::printf("cheats tests passed\n");
    return failures ? 1 : 0;
}
