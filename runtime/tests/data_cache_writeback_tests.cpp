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
        std::puts("PASS: DC writeback alignment, source generation, aliases, invalid ranges, byte preservation");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
