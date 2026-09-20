#include "memory.h"
#include "isa/big_endian.h"
#include "isa/ppc_isa_fpenv.h"

#include <array>
#include <cstdio>
#include <type_traits>

// Exercise the real resolved load without linking the full game runtime.
double MemoryInline::ReadResolvedFallbackFloat32(uint32_t) { return -1.0; }

static_assert(std::is_same_v<decltype(Memory::ReadFloat32(0)), double>);
static_assert(std::is_same_v<decltype(MemoryInline::FlatReadFloat32(0)), double>);

int main() {
    struct Sample { uint32_t single; uint64_t widened; };
    constexpr Sample samples[] = {
        {0x00000000, 0x0000000000000000ULL},
        {0x80000000, 0x8000000000000000ULL},
        {0x00000001, 0x36A0000000000000ULL}, // Title projection selector
        {0x80000001, 0xB6A0000000000000ULL},
        {0x007FFFFF, 0x380FFFFFC0000000ULL},
        {0x00800000, 0x3810000000000000ULL},
        {0x3F800000, 0x3FF0000000000000ULL},
        {0xC2500000, 0xC04A000000000000ULL}, // Camera depth -52
        {0x7F7FFFFF, 0x47EFFFFFE0000000ULL},
        {0x7F800000, 0x7FF0000000000000ULL},
        {0xFF800000, 0xFFF0000000000000ULL},
        {0x7FC12345, 0x7FF82468A0000000ULL},
        {0x7F800001, 0x7FF0000020000000ULL}, // Preserve signaling NaN bits
    };
    const uint32_t savedControl = MkwGetHostFpControl();
    bool passed = true;
    for (const uint32_t fpscr : {0u, 4u}) {
        MkwApplyHostNiMode(fpscr);
        for (const auto& sample : samples) {
            std::array<uint8_t, 8> memory{};
            BigEndian::Write32(memory.data() + 4, sample.single);
            const double loaded = MemoryInline::ReadResolvedFloat32(memory.data(), 4, 0);
            uint64_t actual;
            std::memcpy(&actual, &loaded, sizeof(actual));
            if (actual != sample.widened) {
                std::fprintf(stderr, "NI=%u input=%08X got=%016llX expected=%016llX\n",
                             fpscr, sample.single, (unsigned long long)actual,
                             (unsigned long long)sample.widened);
                passed = false;
            }
            if (sample.single == 1 && loaded == 0.0) {
                std::fputs("Projection selector must compare unequal to zero\n", stderr);
                passed = false;
            }
        }
    }
    MkwRestoreHostMxcsr(savedControl);
    if (passed) std::puts("Float loads preserve bits with NI off and on");
    return passed ? 0 : 1;
}
