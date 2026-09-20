#include "locked_cache_dma.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace {
void Require(bool result, const char* message) {
    if (!result) throw std::runtime_error(message);
}

uint32_t Upper(uint32_t memory, uint32_t blocks) {
    return (memory & ~31u) | ((blocks >> 2) & 31u);
}
uint32_t Lower(uint32_t cache, uint32_t blocks, bool load, bool trigger = true) {
    return (cache & ~31u) | ((blocks & 3u) << 2) | (load ? 0x10u : 0u) | (trigger ? 2u : 0u);
}
}

int main() {
    try {
        std::array<unsigned char, 16384> cache{};
        std::array<unsigned char, 16384> ram{};
        constexpr uint32_t cacheBase = 0xE0000000u;
        constexpr uint32_t ramBase = 0x11234000u;
        uint32_t lastDestination = 0, lastSize = 0, copies = 0;
        const auto copy = [&](const LockedCacheDma::Transfer& t) {
            const uint32_t sourceBase = t.load ? ramBase : cacheBase;
            const uint32_t destinationBase = t.load ? cacheBase : ramBase;
            const auto& source = t.load ? ram : cache;
            auto& destination = t.load ? cache : ram;
            Require(t.source >= sourceBase && t.destination >= destinationBase, "wrong direction");
            const uint32_t so = t.source - sourceBase, d = t.destination - destinationBase;
            Require(so + t.bytes <= source.size() && d + t.bytes <= destination.size(), "bad extent");
            std::memmove(destination.data() + d, source.data() + so, t.bytes);
            lastDestination = t.destination;
            lastSize = t.bytes;
            ++copies;
        };
        for (uint32_t i = 0; i < cache.size(); ++i) cache[i] = static_cast<unsigned char>(i * 37u + 11u);
        uint32_t lower = 0;
        Require(!LockedCacheDma::WriteLower(Upper(ramBase, 7), lower,
                                           Lower(cacheBase, 7, false, false), copy), "non-trigger copied");
        Require(copies == 0 && ram[0] == 0, "non-trigger changed RAM");
        Require(LockedCacheDma::WriteLower(Upper(ramBase + 64, 7), lower,
                                          Lower(cacheBase + 32, 7, false), copy), "store not submitted");
        Require(lastDestination == ramBase + 64 && lastSize == 224, "split length/address decode");
        Require(std::memcmp(ram.data() + 64, cache.data() + 32, 224) == 0, "store bytes differ");
        Require(ram[63] == 0 && ram[288] == 0, "store crossed requested extent");
        Require((lower & LockedCacheDma::kTrigger) == 0, "trigger not acknowledged");

        std::fill(cache.begin(), cache.end(), 0);
        LockedCacheDma::WriteLower(Upper(ramBase + 64, 7), lower, Lower(cacheBase + 96, 7, true), copy);
        Require(std::memcmp(cache.data() + 96, ram.data() + 64, 224) == 0, "load bytes differ");
        Require(lastDestination == cacheBase + 96, "load destination wrong");
        Require((lower & LockedCacheDma::kLoad) != 0 && (lower & 2u) == 0, "load latch lost");

        for (uint32_t blocks : {0u, 1u, 3u, 4u, 31u, 64u, 127u}) {
            const auto transfer = LockedCacheDma::Decode(Upper(ramBase, blocks), Lower(cacheBase, blocks, false));
            Require(transfer.bytes == (blocks == 0 ? 128u : blocks) * 32u, "block count decode");
        }
        std::fill(cache.begin(), cache.end(), 0xD5);
        LockedCacheDma::WriteLower(Upper(ramBase, 0), lower, Lower(cacheBase, 0, false), copy);
        Require(lastSize == 4096 && ram[4095] == 0xD5 && ram[4096] == 0, "zero count is not 128 blocks");
        const auto beforeFlush = copies;
        LockedCacheDma::WriteLower(0, lower, 1, copy);
        Require(copies == beforeFlush && lower == 1, "flush-only command transferred data");

        std::puts("PASS: locked-cache DMA direction, split count, zero=128, byte extent, trigger, flush");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
