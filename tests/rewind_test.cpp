// Rewind history: snapshots stored as differences step back to exactly what was pushed.
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#include "app/rewind.h"

using supersaga::rewind::History;

static int failures = 0;
#define CHECK(x)                                                              \
    do {                                                                      \
        if (!(x)) {                                                           \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

int main() {
    std::mt19937 rng(1234);
    // Mostly unchanged frames with a few scattered and clustered edits, like real save states.
    std::vector<std::vector<std::uint8_t>> frames;
    std::vector<std::uint8_t> s(400000);
    for (auto& b : s) b = std::uint8_t(rng());
    for (int f = 0; f < 60; ++f) {
        for (int k = 0; k < 50; ++k) s[rng() % s.size()] ^= std::uint8_t(1 + rng() % 255);
        const std::size_t at = rng() % (s.size() - 5000);
        for (std::size_t k = 0; k < 3000; ++k) s[at + k] = std::uint8_t(rng());
        if (f % 7 == 0) s.back() ^= 0x5A;   // edits at the very end
        if (f % 5 == 0) s.front() ^= 0xA5;  // and the very start
        frames.push_back(s);
    }

    {
        History h;
        for (const auto& f : frames) h.push(f);
        CHECK(h.depth() == frames.size() - 1);
        CHECK(h.bytes() < frames.size() * 8000);  // differences, not whole states
        for (int i = int(frames.size()) - 2; i >= 0; --i) {
            const auto* back = h.step_back();
            CHECK(back && *back == frames[i]);
        }
        CHECK(h.step_back() == nullptr);  // nothing older than the first snapshot
        // Pushing again after stepping back continues from there.
        h.push(frames[10]);
        const auto* back = h.step_back();
        CHECK(back && *back == frames[0]);
    }
    {
        History h;
        h.set_limits(10, std::size_t(1) << 30);
        for (const auto& f : frames) h.push(f);
        CHECK(h.depth() == 10);
        for (int i = 0; i < 10; ++i) {
            const auto* back = h.step_back();
            CHECK(back && *back == frames[frames.size() - 2 - i]);
        }
        CHECK(h.step_back() == nullptr);
    }
    {
        History h;
        h.set_limits(1000, 20000);  // byte budget smaller than the history
        for (const auto& f : frames) h.push(f);
        CHECK(h.bytes() <= 20000);
        CHECK(h.depth() > 0 && h.depth() < frames.size() - 1);
    }
    {
        History h;  // a different size starts over
        h.push(frames[0]);
        h.push(frames[1]);
        h.push(std::vector<std::uint8_t>(10, 1));
        CHECK(h.depth() == 0);
        CHECK(h.step_back() == nullptr);
        h.push(std::vector<std::uint8_t>(10, 1));  // identical snapshot: empty difference
        const auto* back = h.step_back();
        CHECK(back && *back == std::vector<std::uint8_t>(10, 1));
    }
    if (failures) return 1;
    std::puts("rewind: ok");
    return 0;
}
