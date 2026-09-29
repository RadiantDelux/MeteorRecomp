#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

namespace WiiCompiled::Disc {

// A payload may omit its final alignment bytes in an extracted disc tree.
// Never turn an arbitrary hole, an aligned EOF, or a neighbouring file into
// padding. A read starting inside the omitted tail is valid as well.
inline uint32_t AlignmentPaddingLength(uint64_t offset, uint64_t payloadEnd,
                                      uint64_t nextStart, uint32_t requested,
                                      uint32_t alignment) {
    if (alignment == 0 || (alignment & (alignment - 1u)) != 0 ||
        payloadEnd > std::numeric_limits<uint64_t>::max() - (alignment - 1u)) {
        return 0;
    }
    const uint64_t alignedEnd = (payloadEnd + alignment - 1u) & ~uint64_t(alignment - 1u);
    const uint64_t padEnd = std::min(alignedEnd, nextStart);
    if (offset < payloadEnd || offset >= padEnd) return 0;
    return static_cast<uint32_t>(std::min<uint64_t>(requested, padEnd - offset));
}

} // namespace WiiCompiled::Disc
