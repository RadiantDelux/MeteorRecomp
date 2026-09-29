#include "interpolation_cadence.h"
#include <cassert>
#include <iostream>

int main() {
    constexpr uint64_t vi = 16'683'000;
    InterpolationCadence cadence;
    for (unsigned i = 0; i < 600; ++i)
        assert(!cadence.Observe(vi * (i % 8 == 0 ? 2 : 1), vi));
    cadence.Reset();
    for (unsigned i = 0; i < 60; ++i) assert(!cadence.Observe(vi * 3 / 2, vi));
    cadence.Reset();
    for (unsigned i = 0; i < 60; ++i) {
        const bool eligible = cadence.Observe(vi * (i % 2 == 0 ? 1 : 3), vi);
        if (i >= 5) assert(eligible);
    }
    assert(!cadence.Observe(vi * 5, vi));
    assert(!cadence.Observe(vi * 2, vi));
    for (unsigned i = 0; i < 6; ++i) cadence.Observe(vi * 2, vi);
    assert(cadence.Observe(vi * 2, vi));
    cadence.Observe(vi, vi);
    assert(!cadence.Observe(vi, vi));
    assert(!cadence.Observe(40'000'000, 20'000'000)); // video-mode change resets history
    std::cout << "Source cadence: jitter, random native-60 hitches, 40 FPS and discontinuities passed\n";
}
