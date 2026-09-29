#pragma once

#include <cstdint>

namespace DataCacheWriteback {
inline constexpr uint32_t kLineBytes = 32;

// SDK range loops add the leading misalignment and round the length in 32-bit
// registers before loading CTR. Preserve that arithmetic at the title boundary.
constexpr uint32_t RangeLineCount(uint32_t address, uint32_t length) {
    return (length + (address & (kLineBytes - 1u)) + (kLineBytes - 1u)) / kLineBytes;
}

// A fully backed range can publish all its lines together. False requests the
// original per-line fallback (holes, wrapping ranges or an overflowing count).
template <class Contains, class Publish>
bool PublishRange(uint32_t address, uint32_t length, Contains&& contains, Publish&& publish) {
    if (length == 0) return true;
    const uint32_t count = RangeLineCount(address, length);
    const uint32_t start = address & ~(kLineBytes - 1u);
    const uint32_t bytes = count * kLineBytes;
    if (count == 0 || uint64_t(start) + bytes > 0x1'0000'0000ull || !contains(start, bytes)) return false;
    publish(start, bytes);
    return true;
}

// dcbst/dcbf publish the line containing EA, not the next aligned line.
// Host RAM already contains the bytes, so notify its cached consumers without
// reading or rewriting the data. contains must reject non-backed ranges.
template <class Contains, class Publish>
bool PublishLine(uint32_t address, Contains&& contains, Publish&& publish) {
    const uint32_t line = address & ~(kLineBytes - 1u);
    if (!contains(line, kLineBytes)) return false;
    publish(line, kLineBytes);
    return true;
}
} // namespace DataCacheWriteback
