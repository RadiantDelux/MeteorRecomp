#pragma once

#include <cstdint>

namespace DataCacheWriteback {
inline constexpr uint32_t kLineBytes = 32;

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
