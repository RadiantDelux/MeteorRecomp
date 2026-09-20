#pragma once

#include "isa/ppc_isa_context.h"
#include "memory.h"
#include "recomp_mod_loader.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace GxMatrixProvenance {

inline uint32_t Read32(const uint8_t* p) noexcept {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) | p[3];
}

// Diagnostics may inspect backed RAM but must never read an MMIO register.
inline const uint8_t* Ram(uint32_t address, uint32_t size) {
    const bool mainRam =
        (address >= Memory::kMem1CachedBase &&
         uint64_t(address) + size <= uint64_t(Memory::kMem1CachedBase) + Memory::kMem1Size) ||
        (address >= Memory::kMem2CachedBase &&
         uint64_t(address) + size <= Memory::kMem2CachedEnd);
    return mainRam && Memory::Contains(address, size) ? Memory::GetPointer(address, size) : nullptr;
}

// Opt-in provenance for complete XF matrix writes. This observes packets before
// submission and never changes guest bytes, GX state or command ordering.
inline void ObserveWords(uint32_t address, const uint8_t* words, uint32_t wordCount,
                         const char* sourceKind, uint32_t sourceAddress = 0) {
#if defined(MKW_GENERIC_DOL_BOOT)
    if (words == nullptr || wordCount != 12) return;
    if (address >= 0x400 || address % 12 != 0) return;
    struct State {
        uint32_t poll = 0, remaining = 0, id = 0;
        FILE* output = nullptr;
        bool positionOnly = false;
    };
    static thread_local State state;
    if (state.output == nullptr && (++state.poll & 1023u) == 0) {
        if (FILE* request = std::fopen("gx_matrix_capture.request", "r")) {
            unsigned count = 0;
            char mode[16]{};
            const bool valid = std::fscanf(request, "%u %15s", &count, mode) >= 1 && count > 0 && count <= 256;
            std::fclose(request);
            std::remove("gx_matrix_capture.request");
            if (valid) {
                state.output = std::fopen("gx_matrix_capture.txt", "w");
                state.remaining = count;
                state.id = 0;
                state.positionOnly = std::strcmp(mode, "pos") == 0;
                if (state.output) {
                    std::fprintf(state.output, "bounded thin-X/Y raw XF matrix provenance count=%u positionOnly=%u\n",
                                 count, unsigned(state.positionOnly));
                    std::fflush(state.output);
                }
            }
        }
    }
    if (state.output == nullptr || state.remaining == 0) return;
    // Slot0..9 are the position bank. Texture-matrix writes share XF memory
    // but must not consume a position-only request before a PN0 load is seen.
    if (state.positionOnly && address >= 0x78) return;
    float matrix[12];
    for (unsigned i = 0; i < 12; ++i) {
        const uint32_t bits = Read32(words + i * 4);
        std::memcpy(&matrix[i], &bits, sizeof(bits));
        if (!std::isfinite(matrix[i])) return;
    }
    const double x2 = double(matrix[0])*matrix[0] + double(matrix[4])*matrix[4] + double(matrix[8])*matrix[8];
    const double y2 = double(matrix[1])*matrix[1] + double(matrix[5])*matrix[5] + double(matrix[9])*matrix[9];
    if (y2 <= 1e-12 || x2 >= y2 * 1e-6) return;

    const CpuContext* cpu = TryGetCpuContext();
    std::fprintf(state.output, "event=%u xf=%04X x2=%.12g y2=%.12g dispatch=%08X lr=%08X source=%s address=%08X\n matrix",
                 state.id++, address, x2, y2, RecompMod::CurrentTranslatedExecutionAddress(), cpu ? cpu->lr : 0,
                 sourceKind, sourceAddress);
    for (float value : matrix) std::fprintf(state.output, " %.9g", double(value));
    std::fprintf(state.output, "\n");
    if (cpu) {
        std::fprintf(state.output, " module runtime=%08X canonical=%08X size=%08X bss=%08X generation=%llu\n",
                     cpu->dynamicModuleRuntimeBase, cpu->dynamicModuleCanonicalBase,
                     cpu->dynamicModuleImageSize, cpu->dynamicModuleRuntimeBssBase,
                     static_cast<unsigned long long>(cpu->dynamicModuleGeneration));
        std::fprintf(state.output, " gpr");
        for (uint32_t value : cpu->gpr) std::fprintf(state.output, " %08X", value);
        std::fprintf(state.output, "\n stack");
        uint32_t stack = cpu->gpr[1];
        for (unsigned depth = 0; depth < 32; ++depth) {
            const uint8_t* bytes = Ram(stack, 8);
            if (!bytes) break;
            std::fprintf(state.output, " %08X:%08X", stack, Read32(bytes + 4));
            const uint32_t next = Read32(bytes);
            if (next <= stack || next - stack > 0x100000u) break;
            stack = next;
        }
        std::fprintf(state.output, "\n");
        // Helpers often replace volatile r3-r7 before emitting XF. Preserve
        // backed-RAM nonvolatile node pointers too, with the same fixed160B
        // limit per register. This remains read-only and never follows MMIO.
        for (unsigned reg = 3; reg <= 31; ++reg) {
            const uint8_t* bytes = Ram(cpu->gpr[reg], 160);
            if (!bytes) continue;
            std::fprintf(state.output, " memr%u=%08X", reg, cpu->gpr[reg]);
            for (unsigned i = 0; i < 160; ++i) std::fprintf(state.output, "%s%02X", i % 4 ? "" : " ", bytes[i]);
            std::fprintf(state.output, "\n");
        }
    }
    std::fflush(state.output);
    if (--state.remaining == 0) {
        std::fclose(state.output);
        state.output = nullptr;
    }
#else
    (void)address;
    (void)words;
    (void)wordCount;
    (void)sourceKind;
    (void)sourceAddress;
#endif
}

inline void Observe(const uint8_t* packet, uint32_t packetBytes) {
#if defined(MKW_GENERIC_DOL_BOOT)
    if (packet == nullptr || packetBytes != 53 || packet[0] != 0x10 || Read32(packet + 1) >> 16 != 11) return;
    ObserveWords(Read32(packet + 1) & 0xffffu, packet + 5, 12, "raw");
#else
    (void)packet;
    (void)packetBytes;
#endif
}
} // namespace GxMatrixProvenance
