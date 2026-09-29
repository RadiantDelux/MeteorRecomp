#pragma once

#include <cstdint>

// Broadway/Gekko DMA_U (SPR 922) and DMA_L (SPR 923) wire contract.
// The two registers split a seven-bit count of 32-byte blocks; zero denotes
// 128 blocks. DMA_L bit 1 submits, bit 4 selects RAM -> locked-cache loading.
// DMA completion is synchronous in this runtime, like the SDK LC HLE path.
namespace LockedCacheDma {

constexpr uint32_t kUpperSpr = 922;
constexpr uint32_t kLowerSpr = 923;
constexpr uint32_t kTrigger = 0x2;
constexpr uint32_t kLoad = 0x10;
constexpr uint32_t kBlockBytes = 32;

struct Transfer {
    uint32_t source;
    uint32_t destination;
    uint32_t bytes;
    bool load;
};

constexpr Transfer Decode(uint32_t upper, uint32_t lower) noexcept {
    const uint32_t encodedBlocks = ((upper & 0x1fu) << 2) | ((lower >> 2) & 3u);
    const uint32_t blocks = encodedBlocks == 0 ? 128u : encodedBlocks;
    const uint32_t memory = upper & ~0x1fu;
    const uint32_t cache = lower & ~0x1fu;
    const bool load = (lower & kLoad) != 0;
    return {load ? memory : cache, load ? cache : memory, blocks * kBlockBytes, load};
}

// copy must complete (or throw) before the trigger is acknowledged. A register
// write without DMA_T only latches configuration; DMA_F has no pending queue
// to discard when every earlier transfer has already completed synchronously.
template<class Copy>
bool WriteLower(uint32_t upper, uint32_t& lower, uint32_t value, Copy&& copy) {
    lower = value;
    if ((value & kTrigger) == 0) return false;
    copy(Decode(upper, value));
    lower &= ~kTrigger;
    return true;
}

} // namespace LockedCacheDma
