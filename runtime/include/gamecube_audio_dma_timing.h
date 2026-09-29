#pragma once
#include <cstdint>

namespace GameCubeAudioDmaTiming {
// One DMA block is 32 bytes: eight stereo s16 sample frames. These exact
// integer ratios avoid a variable 64-bit division on every interrupt poll.
inline constexpr std::uint64_t DurationMicros(std::uint32_t blocks, std::uint32_t sampleRate) {
    return sampleRate == 32000 ? std::uint64_t(blocks) * 250
                             : (std::uint64_t(blocks) * 500 + 2) / 3;
}
inline constexpr std::uint16_t RemainingBlocks(std::uint32_t blocks,
                                              std::uint32_t sampleRate,
                                              std::uint64_t elapsed) {
    if (elapsed >= DurationMicros(blocks, sampleRate)) return 0;
    const auto consumed = sampleRate == 32000 ? elapsed / 250 : elapsed * 3 / 500;
    return static_cast<std::uint16_t>(blocks - consumed);
}
} // namespace GameCubeAudioDmaTiming
