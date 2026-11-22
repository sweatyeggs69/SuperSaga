// Rumble: which changes in blank RAM images turn into controller rumble patterns. No ROM needed.
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
    rumble::Pattern p = w.step(iwram, ram);
    CHECK(p && p.peak_low() >= 0.45f && p.strength() < 1.0f);

    // A bigger hit is stronger.
    set16(ram, mario, 3);
    rumble::Pattern big = w.step(iwram, ram);
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

    // Player: steps through a pattern a frame at a time, and a lower priority pattern never cuts a
    // higher one short; it plays again once that is over.
    {
        rumble::Player pl;
        CHECK(pl.start(rumble::patterns::kDeath));
        auto s = pl.next();
        CHECK(s && s->low == 1.0f && s->high == 1.0f && s->ms > 250);  // first beat, with overlap
        CHECK(!pl.start(rumble::patterns::kMissile));
        int commands = 1, frames = 0;
        bool paused = false;
        while (pl.playing() && frames < 200) {
            if (auto c = pl.next()) {
                ++commands;
                if (c->low == 0 && c->high == 0 && pl.playing()) paused = true;
            }
            ++frames;
        }
        CHECK(paused);                          // the pauses between beats stop the motors
        CHECK(commands == 7);                   // six steps, then the final stop
        CHECK(frames >= 75 && frames <= 90);    // about 1.4 s of game frames
        CHECK(!pl.playing());
        CHECK(pl.start(rumble::patterns::kMissile));
        CHECK(pl.start(rumble::patterns::kDeath));  // a death interrupts anything
        CHECK(!pl.start({}));
    }

    // The pattern table: different events feel different.
    {
        namespace pt = rumble::patterns;
        CHECK(pt::kMissile.peak_low() == 0 && pt::kMissile.peak_high() > 0);  // small motor only
        CHECK(pt::kDeath.total_ms() > pt::kHeavyHit.total_ms());
        CHECK(pt::kPowerBomb.strength() == 1.0f && pt::kPowerBomb.steps[0].low < 0.5f);  // builds up
        const rumble::Pattern scratch = rumble::hit(1, 100), big = rumble::hit(40, 100);
        CHECK(scratch.count == 1 && big.count == 2 && big.strength() > scratch.strength());
        const rumble::Pattern sh = rumble::shake(600, 0.8f);
        CHECK(sh.count > 4 && sh.total_ms() == 600 && sh.priority == pt::kShakePriority);
        CHECK(rumble::shake(5000, 1.0f).total_ms() == 1200);
    }

    // Strength scaling.
    CHECK(rumble::motor(1.0f, 100) == 0xFFFF);
    CHECK(rumble::motor(1.0f, 50) == 0x8000);
    CHECK(rumble::motor(0.0f, 100) == 0);
    CHECK(rumble::motor(2.0f, 100) == 0xFFFF);

    if (failures) return 1;
    std::puts("rumble: ok");
    return 0;
}
