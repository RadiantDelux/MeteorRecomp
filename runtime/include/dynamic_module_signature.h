#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#endif

namespace DynamicModule {

// Full bit-for-bit invariant validation, not a hash or a sampled signature.
// Masked relocation bits are checked separately against the live linked layout.
// Loads never extend beyond length and require no alignment from the guest.
inline bool MatchMaskedImage(const uint8_t* image, const uint8_t* expected,
                             const uint8_t* mask, size_t length) noexcept {
    size_t offset = 0;
#if defined(__SSE2__) || defined(_M_X64)
    const auto zero = _mm_setzero_si128();
    for (; length - offset >= 16; offset += 16) {
        const auto actual = _mm_loadu_si128(reinterpret_cast<const __m128i*>(image + offset));
        const auto wanted = _mm_loadu_si128(reinterpret_cast<const __m128i*>(expected + offset));
        const auto bits = _mm_loadu_si128(reinterpret_cast<const __m128i*>(mask + offset));
        const auto mismatch = _mm_and_si128(_mm_xor_si128(actual, wanted), bits);
        if (_mm_movemask_epi8(_mm_cmpeq_epi8(mismatch, zero)) != 0xffff) return false;
    }
#else
    for (; length - offset >= sizeof(uint64_t); offset += sizeof(uint64_t)) {
        uint64_t actual, wanted, bits;
        std::memcpy(&actual, image + offset, sizeof(actual));
        std::memcpy(&wanted, expected + offset, sizeof(wanted));
        std::memcpy(&bits, mask + offset, sizeof(bits));
        if (((actual ^ wanted) & bits) != 0) return false;
    }
#endif
    for (; offset < length; ++offset)
        if (((image[offset] ^ expected[offset]) & mask[offset]) != 0) return false;
    return true;
}

// The registrar has already resolved and bounded the complete image. Reusing
// that view avoids repeating guest address translation for every linked field.
inline uint32_t ReadImage32(const uint8_t* image, uint32_t offset) noexcept {
    const auto* p = image + offset;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

inline uint16_t ReadImage16(const uint8_t* image, uint32_t offset) noexcept {
    const auto* p = image + offset;
    return static_cast<uint16_t>((uint16_t(p[0]) << 8) | p[1]);
}

} // namespace DynamicModule
