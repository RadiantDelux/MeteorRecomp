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

int32_t Meteor_Strcmp(uint32_t lhs, uint32_t rhs) {
    for (;;) {
        const uint8_t a = MemoryInline::FlatRead8(lhs++);
        const uint8_t b = MemoryInline::FlatRead8(rhs++);
        if (a != b) {
            return static_cast<int32_t>(a) - static_cast<int32_t>(b);
        }
        if (a == 0u) {
            return 0;
        }
    }
}

uint32_t Meteor_ReadEncodedLowMemOption(uint32_t key, uint32_t output, uint32_t outputLength) {
    // Exact native form of RDSPAF 0x80257810. The retail helper scans the
    // 256-byte low-memory option blob at 0x80003800, XOR-decodes each byte with
    // a rotating 0x73B5DBFA key, performs the same ASCII case-folded key match,
    // then copies the value after '=' until NUL/CR/LF or the caller's limit.
    // Keeping this as a title-native helper removes translated loop/checkpoint
    // overhead only; all guest reads/writes and return semantics stay identical.
    uint32_t rollingKey = 0x73B5DBFAu;
    uint32_t matched = 0u;
    uint32_t index = 0u;

    for (; index < 256u; ++index) {
        const uint8_t encoded = MemoryInline::FlatRead8(0x80003800u + index);
        const uint8_t decoded = static_cast<uint8_t>(encoded ^ rollingKey);
        if (encoded != 0u) {
            const uint8_t keyByte = MemoryInline::FlatRead8(key + matched);
            if (keyByte == 0u && decoded == static_cast<uint8_t>('=')) {
                break;
            } else {
                const uint32_t signedKeyByte = static_cast<uint32_t>(
                    static_cast<int32_t>(static_cast<int8_t>(keyByte)));
                const bool sameFoldedByte =
                    ((static_cast<uint32_t>(decoded) ^ signedKeyByte) & 0xDFu) == 0u;
                matched = sameFoldedByte ? (matched + 1u) : 0u;
            }
        }
        rollingKey = (rollingKey << 1u) | (rollingKey >> 31u);
    }

    if (index >= 256u) {
        return 0u;
    }

    ++index; // Skip '=' exactly like loc_80257904.
    uint32_t written = 0u;
    while (index < 256u && written < outputLength) {
        const uint8_t encoded = MemoryInline::FlatRead8(0x80003800u + index);
        rollingKey = (rollingKey << 1u) | (rollingKey >> 31u);
        uint8_t decoded = static_cast<uint8_t>(encoded ^ rollingKey);
        if (decoded == static_cast<uint8_t>('\r') || decoded == static_cast<uint8_t>('\n')) {
            decoded = 0u;
        }
        MemoryInline::FlatWrite8(output++, decoded);
        ++written;
        if (decoded == 0u) {
            return 1u;
        }
        ++index;
    }
    return 0u;
}

} // namespace

REGISTER_TITLE_NATIVE_FUNCTION_AS(0x80004338, Meteor_Memmove, "Meteor_Memmove");
REGISTER_TITLE_NATIVE_FUNCTION_AS(0x80004388, Meteor_Memset, "Meteor_Memset");
REGISTER_TITLE_NATIVE_FUNCTION_AS(0x8023C3DC, Meteor_Strcmp, "Meteor_Strcmp");
REGISTER_TITLE_NATIVE_FUNCTION_AS(0x80257810, Meteor_ReadEncodedLowMemOption,
                                  "Meteor_ReadEncodedLowMemOption");
