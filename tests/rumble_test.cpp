// Rumble: which changes in blank RAM images turn into controller pulses. No ROM needed.
#include <array>
#include <cstdint>
#include <cstdio>

#include "app/rumble.h"

namespace {
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)
using Iwram = std::array<std::uint8_t, 0x8000>;
using Ewram = std::array<std::uint8_t, 0x40000>;
template <typename M> void set16(M& m, std::size_t at, std::uint16_t v) { m[at] = std::uint8_t(v); m[at + 1] = std::uint8_t(v >> 8); }
constexpr std::size_t ew(unsigned a) { return a - 0x02000000u; }
constexpr std::size_t iw(unsigned a) { return a - 0x03000000u; }
}  // namespace

int main() {
    namespace rumble = supersaga::rumble;
    namespace game = supersaga::game;
    Iwram iwram{};
    Ewram ram{};
    rumble::Watcher w;
    const std::size_t mario = ew(game::kMarioStatsAddress), luigi = ew(game::kLuigiStatsAddress);

    // No file loaded: nothing.
    CHECK(!w.step(iwram, ram));

    // Mario 10/10, Luigi 12/12.
    set16(ram, mario, 10);
    set16(ram, mario + 2, 10);
    set16(ram, luigi, 12);
    set16(ram, luigi + 2, 12);
    CHECK(!w.step(iwram, ram));  // the maximum just changed (file loaded)
    CHECK(!w.step(iwram, ram));

    // Luigi takes 3.
    set16(ram, luigi, 9);
    rumble::Pulse p = w.step(iwram, ram);
    CHECK(p && p.low >= 0.45f && p.strength() < 1.0f);

    // A bigger hit is stronger.
    set16(ram, mario, 3);
    rumble::Pulse big = w.step(iwram, ram);
    CHECK(big && big.strength() > p.strength());

    // Knocked out.
    set16(ram, mario, 0);
    p = w.step(iwram, ram);
    CHECK(p && p.strength() == 1.0f);

    // Healing (a Mushroom) never rumbles.
    set16(ram, mario, 10);
    CHECK(!w.step(iwram, ram));

    // Level up raises the maximum and HP: nothing.
    set16(ram, mario + 2, 14);
    set16(ram, mario, 14);
    CHECK(!w.step(iwram, ram));

    // reset(): a loaded state with less HP is not a hit.
    w.reset();
    set16(ram, luigi, 1);
    CHECK(!w.step(iwram, ram));

    // Mixer: a weaker pulse never cuts a stronger one short; it plays again once that is over.
    rumble::Mixer mix;
    CHECK(mix.take({1.0f, 1.0f, 100}));
    mix.step();
    CHECK(!mix.take({0.0f, 0.35f, 70}));
    for (int i = 0; i < 10; ++i) mix.step();
    CHECK(!mix.playing());
    CHECK(mix.take({0.0f, 0.35f, 70}));
    CHECK(!mix.take({}));

    // Strength scaling.
    CHECK(rumble::motor(1.0f, 100) == 0xFFFF);
    CHECK(rumble::motor(1.0f, 50) == 0x8000);
    CHECK(rumble::motor(0.0f, 100) == 0);
    CHECK(rumble::motor(2.0f, 100) == 0xFFFF);

    if (failures) return 1;
    std::puts("rumble: ok");
    return 0;
}
