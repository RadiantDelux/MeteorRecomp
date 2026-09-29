#include "../../aurora-main/lib/presentation_schedule.hpp"
#include <cassert>
#include <iostream>

int main() {
    using namespace aurora;
    for (const uint64_t vi : {16'683'000u, 20'000'000u}) {
        uint64_t retrace = 1'000'000'000;
        // Slow 2D frames, normal frames and mode switches cannot build debt.
        for (unsigned frame = 0; frame < 2000; ++frame) {
            retrace += vi * (frame % 9 == 0 ? 3u : 1u);
            const auto base = retrace + vi;
            const auto native = presentation_deadline(base, presentation_span(vi, false), 0, 0);
            assert(native == retrace + 6'500'000);
            const auto span = presentation_span(vi, true);
            const auto midpoint = presentation_deadline(base, span, 1, 0);
            const auto final = presentation_deadline(base, span, 1, 1);
            assert(midpoint == base && final - midpoint == vi);
            // 120/180 FPS retain their one-VI subdivision.
            assert(presentation_deadline(base, vi, 2, 2) - base == vi * 2 / 3);
        }
    }
    assert(presentation_deadline(0, 0, 0, 0) == 0);
    std::cout << "Presentation: native 2D, 30->60, high-rate slots and jitter passed\n";
}
