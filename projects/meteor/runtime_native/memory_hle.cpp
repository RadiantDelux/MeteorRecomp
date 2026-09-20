#include "abi_bridge.h"
#include "memory.h"
#include "memory_access.h"
#include "recomp_mod_loader.h"

#include <cstdint>
#include <cstring>
#include <limits>

namespace {

bool RangeWraps(uint32_t address, uint32_t length) noexcept {
    return length != 0u &&
           static_cast<uint64_t>(address) + static_cast<uint64_t>(length) >
               static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) + 1ull;
}

bool RangeTouchesMmio(uint32_t address, uint32_t length) noexcept {
    if (length == 0u || RangeWraps(address, length)) {
        return false;
    }
    constexpr uint64_t kMmioBegin = 0xCC000000ull;
    constexpr uint64_t kMmioEnd = 0xCE000000ull;
    const uint64_t begin = address;
    const uint64_t end = begin + length;
    return begin < kMmioEnd && end > kMmioBegin;
}

void MemmoveScalar(uint32_t destination, uint32_t source, uint32_t length) {
    if (length == 0u || destination == source) {
        return;
    }
    const uint64_t dstBegin = destination;
    const uint64_t srcBegin = source;
    const uint64_t srcEnd = srcBegin + length;
    if (dstBegin < srcBegin || dstBegin >= srcEnd) {
        for (uint32_t i = 0; i < length; ++i) {
            MemoryInline::FlatWrite8(destination + i, MemoryInline::FlatRead8(source + i));
        }
        return;
    }
    for (uint32_t i = length; i != 0u; --i) {
        const uint32_t offset = i - 1u;
        MemoryInline::FlatWrite8(destination + offset, MemoryInline::FlatRead8(source + offset));
    }
}

void MemsetScalar(uint32_t destination, uint8_t value, uint32_t length) {
    for (uint32_t i = 0; i < length; ++i) {
        MemoryInline::FlatWrite8(destination + i, value);
    }
}

bool CanUseBulkWrite(uint32_t destination, uint32_t length) noexcept {
    return !RangeWraps(destination, length) &&
           !RangeTouchesMmio(destination, length) &&
           !RecompMod::ExecutableWriteGuardMayHit(destination, length) &&
           Memory::Contains(destination, length);
}

uint32_t Meteor_Memmove(uint32_t destination, uint32_t source, uint32_t length) {
    if (length == 0u || destination == source) {
        return destination;
    }
    const bool bulkSafe =
        !RangeWraps(source, length) && !RangeTouchesMmio(source, length) &&
        Memory::Contains(source, length) && CanUseBulkWrite(destination, length);
    if (!bulkSafe) {
        MemmoveScalar(destination, source, length);
        return destination;
    }
    MemoryInline::ResolveDeferredReads(source, length);
    MemoryInline::ResolveDeferredReads(destination, length);
    std::memmove(Memory::GetPointer(destination, length), Memory::GetPointer(source, length), length);
    return destination;
}

uint32_t Meteor_Memset(uint32_t destination, uint32_t value, uint32_t length) {
    if (length == 0u) {
        return destination;
    }
    if (!CanUseBulkWrite(destination, length)) {
        MemsetScalar(destination, static_cast<uint8_t>(value), length);
        return destination;
    }
    MemoryInline::ResolveDeferredReads(destination, length);
    std::memset(Memory::GetPointer(destination, length), static_cast<int>(value & 0xFFu), length);
    return destination;
}

} // namespace

REGISTER_TITLE_NATIVE_FUNCTION_AS(0x80004338, Meteor_Memmove, "Meteor_Memmove");
REGISTER_TITLE_NATIVE_FUNCTION_AS(0x80004388, Meteor_Memset, "Meteor_Memset");
