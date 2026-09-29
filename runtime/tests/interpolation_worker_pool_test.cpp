#include "../../aurora-main/lib/gx/interpolation_worker_pool.hpp"
#include <array>
#include <cassert>
#include <chrono>
#include <iostream>

int main() {
    for (unsigned helpers : {0u, 1u, 3u, 6u}) {
        aurora::gx::InterpolationWorkerPool pool(helpers);
        for (size_t generation = 1; generation <= 2000; ++generation) {
            const size_t count = generation % 2 ? 257 : 4096;
            std::array<std::atomic_uint, 4096> visits{};
            std::array<size_t, 4096> output{};
            pool.run(count, [&](size_t i) {
                visits[i].fetch_add(1, std::memory_order_relaxed);
                if (i % 256 == 0 && generation % 37 == 0) std::this_thread::yield();
                output[i] = i * generation;
            });
            for (size_t i = 0; i < count; ++i) {
                assert(visits[i].load() == 1);
                assert(output[i] == i * generation);
            }
            // Return destroys the borrowed callable and local storage; a late
            // helper must have acknowledged completion before either expires.
        }
        size_t serial = 0;
        pool.run(0, [&](size_t) { assert(false); });
        pool.run(63, [&](size_t) { ++serial; });
        assert(serial == 63);
    }
    std::cout << "Interpolation pool: 8000 dispatches, exactly-once work and lifetime barriers passed\n";
}
