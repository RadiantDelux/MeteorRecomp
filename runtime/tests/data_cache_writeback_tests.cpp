#include "data_cache_writeback.h"
#include "gx_guest_write.h"

#include <array>
#include <cstdio>
#include <stdexcept>

namespace {
void Require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
}

int main() {
    try {
        constexpr uint32_t base = 0x80120000u;
        std::array<uint8_t, 64> bytes{};
        bytes[17] = 0x5a;
        const auto before = bytes;
        const auto generation = GxGuestWrite::GenerationForRange(base, 32);
        uint32_t notifications = 0, last = 0, length = 0;
        const auto contains = [](uint32_t start, uint32_t size) {
            const auto physical = CanonicalizeGxMainRamAddress(start);
            return size == 32 && physical >= 0x120000u && physical < 0x120040u;
        };
        const auto publish = [&](uint32_t start, uint32_t size) {
            ++notifications;
            last = start;
            length = size;
            GxGuestWrite::NotifyWrite(start, size);
        };
        Require(DataCacheWriteback::PublishLine(base + 31, contains, publish), "writeback rejected");
        Require(last == base && length == 32 && notifications == 1, "unaligned EA extent");
        Require(!GxGuestWrite::CanSkipDigest(generation,
            GxGuestWrite::GenerationForRange(base, 32)), "stale texture generation survived writeback");
        Require(bytes == before, "publication changed guest bytes");
        const auto afterFirst = GxGuestWrite::GenerationForRange(base, 32);
        Require(DataCacheWriteback::PublishLine(0xc012003f, contains, publish), "uncached alias rejected");
        Require(last == 0xc0120020u, "wrong alias line");
        Require(!GxGuestWrite::CanSkipDigest(afterFirst,
            GxGuestWrite::GenerationForRange(base, 64)), "alias failed to invalidate same physical generation");
        Require(DataCacheWriteback::PublishLine(0x00120001, contains, publish), "physical alias rejected");
        Require(!DataCacheWriteback::PublishLine(0xcc008000, contains, publish), "MMIO was published");
        Require(!DataCacheWriteback::PublishLine(0xffffffff, contains, publish), "invalid address was published");
        Require(notifications == 3, "invalid extent had a side effect");
        const auto containsRange = [](uint32_t start, uint32_t size) {
            const uint64_t physical = CanonicalizeGxMainRamAddress(start);
            return physical >= 0x120000u && physical + size <= 0x140000u;
        };
        // Compare batched publication with the original sequence of 32-byte
        // writebacks for every possible leading alignment and small extent.
        for (uint32_t offset = 0; offset < 32; ++offset) {
            for (uint32_t size = 1; size <= 512; ++size) {
                uint32_t first = ~0u, end = 0, lines = 0;
                for (uint32_t cursor = (base + offset) & ~31u;
                     cursor < base + offset + size; cursor += 32) {
                    first = std::min(first, cursor);
                    end = cursor + 32;
                    ++lines;
                }
                const auto prior = notifications;
                Require(DataCacheWriteback::PublishRange(base + offset, size, containsRange, publish),
                        "valid range rejected");
                Require(last == first && length == end - first && notifications == prior + 1,
                        "batched range differs from individual lines");
                Require(DataCacheWriteback::RangeLineCount(base + offset, size) == lines,
                        "wrong SDK count/return register extent");
            }
        }
        const auto prior = notifications;
        Require(DataCacheWriteback::PublishRange(base, 65536, containsRange, publish), "large range rejected");
        Require(notifications == prior + 1 && last == base && length == 65536,
                "large range still published per line");
        Require(DataCacheWriteback::PublishRange(base, 0, containsRange, publish), "empty range rejected");
        Require(!DataCacheWriteback::PublishRange(base, 0x30000, containsRange, publish), "hole needs fallback");
        Require(!DataCacheWriteback::PublishRange(0xFFFFFFF0u, 64, containsRange, publish), "wrap needs fallback");
        Require(!DataCacheWriteback::PublishRange(base + 1, 0xFFFFFFFFu, containsRange, publish),
                "overflowing SDK count needs fallback");
        Require(notifications == prior + 1, "empty/invalid range published data");
        std::puts("PASS: DC writeback alignment, source generation, aliases, invalid ranges, byte preservation");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
