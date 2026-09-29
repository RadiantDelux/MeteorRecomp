#include "gamecube_audio_dma_timing.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>

int main() {
    std::uint64_t checks = 0, random = 1234567;
    for (const std::uint32_t rate : {32000, 48000}) {
        for (std::uint32_t blocks = 0; blocks <= 0x7fff; ++blocks) {
            const std::uint64_t bytes = std::uint64_t(blocks) * 32;
            const std::uint64_t bytesPerSecond = rate * 4;
            const auto duration = (bytes * 1'000'000 + bytesPerSecond - 1) / bytesPerSecond;
            if (GameCubeAudioDmaTiming::DurationMicros(blocks, rate) != duration)
                throw std::runtime_error("DMA deadline changed");
            random = random * 6364136223846793005ULL + 1;
            for (auto elapsed : {std::uint64_t(0), duration ? duration - 1 : 0,
                                 duration, duration + 1, random % (duration + 1)}) {
                const auto consumed = (elapsed * bytesPerSecond / 1'000'000) / 32;
                const auto remaining = blocks - (std::min<std::uint64_t>)(blocks, consumed);
                if (GameCubeAudioDmaTiming::RemainingBlocks(blocks, rate, elapsed) != remaining)
                    throw std::runtime_error("DMA countdown changed");
                ++checks;
            }
        }
    }
    std::cout << "Audio DMA: all 32768 lengths at both rates, " << checks
              << " countdown/deadline boundary comparisons passed\n";
}
