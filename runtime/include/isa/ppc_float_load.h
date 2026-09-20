#pragma once

#include <cstdint>
#include <cstring>

// lfs widens the memory bit pattern independently of FPSCR[NI]. In particular,
// a host float-to-double conversion with DAZ/FZ enabled would turn 0x00000001
// into zero. Games can use this nonzero value as GXSetProjectionv's type flag.
// Keep the conversion in the integer domain, including subnormals and sNaNs.
inline uint64_t PpcConvertToDoubleBitsInline(uint32_t value)
{
    uint64_t x = value;
    uint64_t exp = (x >> 23) & 0xFFu;
    uint64_t frac = x & 0x007FFFFFu;

    if (exp > 0 && exp < 255)
    {
        const uint64_t y = !(exp >> 7);
        const uint64_t z = (y << 61) | (y << 60) | (y << 59);
        return ((x & 0xC0000000ULL) << 32) | z | ((x & 0x3FFFFFFFULL) << 29);
    }

    if (exp == 0 && frac != 0)
    {
        exp = 1023 - 126;
        do
        {
            frac <<= 1;
            --exp;
        } while ((frac & 0x00800000u) == 0);

        return ((x & 0x80000000ULL) << 32) | (exp << 52) | ((frac & 0x007FFFFFULL) << 29);
    }

    const uint64_t y = exp >> 7;
    const uint64_t z = (y << 61) | (y << 60) | (y << 59);
    return ((x & 0xC0000000ULL) << 32) | z | ((x & 0x3FFFFFFFULL) << 29);
}

inline double PpcLoadFloat32BitsInline(uint32_t bits)
{
    const uint64_t widened = PpcConvertToDoubleBitsInline(bits);
    double value;
    std::memcpy(&value, &widened, sizeof(value));
    return value;
}
