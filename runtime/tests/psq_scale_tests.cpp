#include "isa/ppc_isa_quantized.h"

#include <cstdio>

template <typename T>
bool CheckScale(uint32_t scale)
{
    const int exponent = scale < 32 ? int(scale) : int(scale) - 64;
    const float loadFactor = std::ldexp(1.0f, -exponent);
    const float storeFactor = std::ldexp(1.0f, exponent);
    // Exhaust every integer input to psq_l, including both signed formats.
    for (int32_t v = std::numeric_limits<T>::min(); v <= std::numeric_limits<T>::max(); ++v) {
        const float expected = float(T(v)) * loadFactor;
        const float actual = PpcDequantizePsqInline(T(v), scale);
        if (std::memcmp(&expected, &actual, sizeof(float)) != 0) return false;
    }
    const auto checkStore = [&](float v) {
        // NaN-to-integer is not defined by C++; leave that pre-existing policy
        // out of this factor-equivalence test. Infinities clamp normally.
        if (std::isnan(v)) return true;
        const T expected = static_cast<T>(std::clamp(v * storeFactor,
            float(std::numeric_limits<T>::min()), float(std::numeric_limits<T>::max())));
        return PpcScaleAndClampPsqInline<T>(v, scale) == expected;
    };
    const float edges[] = {-INFINITY, INFINITY, -0.0f, 0.0f,
        std::numeric_limits<float>::denorm_min(), std::numeric_limits<float>::max(),
        -std::numeric_limits<float>::max()};
    for (float v : edges) if (!checkStore(v)) return false;
    // Check rounding at every integer threshold and both saturation limits.
    for (int32_t v = std::numeric_limits<T>::min(); v <= std::numeric_limits<T>::max(); ++v) {
        const float threshold = float(v) * loadFactor;
        if (!checkStore(threshold) || !checkStore(std::nextafter(threshold, -INFINITY)) ||
            !checkStore(std::nextafter(threshold, INFINITY))) return false;
    }
    uint32_t random = 0x95390123u;
    for (unsigned i = 0; i < 10000; ++i) {
        random = random * 1664525u + 1013904223u;
        if (!checkStore(PpcBitCastToFloatInline(random))) return false;
    }
    return true;
}

int main()
{
    for (uint32_t scale = 0; scale < 64; ++scale) {
        if (!CheckScale<uint8_t>(scale) || !CheckScale<int8_t>(scale) ||
            !CheckScale<uint16_t>(scale) || !CheckScale<int16_t>(scale)) {
            std::fprintf(stderr, "PSQ scale %u differs from ldexp reference\n", scale);
            return 1;
        }
    }
    std::puts("PASS: all 64 PSQ scales, all integer loads, store thresholds and saturation");
}
