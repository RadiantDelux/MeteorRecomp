#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

class Memory {
public:
    using DeferredReadCallback = bool (*)(void* user);

    static constexpr size_t kMem1Size = 24u * 1024u * 1024u;
    static constexpr size_t kMem2Size = 128u * 1024u * 1024u;
    static constexpr uint32_t kMem1PhysicalBase = 0x00000000u;
    static constexpr uint32_t kMem1CachedBase = 0x80000000u;
    static constexpr uint32_t kMem1UncachedBase = 0xC0000000u;
    static constexpr uint32_t kMem2PhysicalBase = 0x10000000u;
    static constexpr uint32_t kMem2CachedBase = 0x90000000u;
    static constexpr uint32_t kMem2UncachedBase = 0xD0000000u;
    static constexpr uint32_t kMem2PhysicalEnd =
        kMem2PhysicalBase + static_cast<uint32_t>(kMem2Size);
    static constexpr uint32_t kMem2CachedEnd =
        kMem2CachedBase + static_cast<uint32_t>(kMem2Size);
    static constexpr uint32_t kMem2UncachedEnd =
        kMem2UncachedBase + static_cast<uint32_t>(kMem2Size);

    struct RegionConfig {
        std::string name;
        uint32_t baseAddress = 0;
        size_t sizeBytes = 0;
    };

    struct Config {
        std::vector<RegionConfig> regions;
        static Config WiiDefaults();
        static Config GameCubeDefaults();
    };

    class AccessViolation : public std::runtime_error {
    public:
        AccessViolation(uint32_t address, size_t length, std::string_view reason);

        uint32_t address() const noexcept { return address_; }
        size_t length() const noexcept { return length_; }
        std::string_view reason() const noexcept { return reason_; }

    private:
        uint32_t address_ = 0;
        size_t length_ = 0;
        std::string reason_;
    };

    static void Init(const Config& config);
    // Single flat MEM1 region. Not used by the shipped runtime, but the
    // translator integration-test harnesses emit calls to it.
    static void Init(size_t mem1Size);
    static void Reset();
    // Executable ranges are normally registered during startup. Rebuild the
    // writable fast-path classification after each registration so a page
    // previously classified as ordinary data cannot retain a stale direct
    // write bias.
    static void RefreshWritableFastPathsForExecutableRanges();

    static uint8_t Read8(uint32_t addr);
    static uint16_t Read16(uint32_t addr);
    static uint32_t Read32(uint32_t addr);
    static uint64_t Read64(uint32_t addr);
    static double ReadFloat32(uint32_t addr);
    static double ReadFloat64(uint32_t addr);
    static void Write8(uint32_t addr, uint8_t val);
    static void Write16(uint32_t addr, uint16_t val);
    static void Write32(uint32_t addr, uint32_t val);
    static void Write64(uint32_t addr, uint64_t val);
    static void WriteFloat32(uint32_t addr, double val);
    static void WriteFloat64(uint32_t addr, double val);

    // Couple the raw VI beam phase to an authoritative HLE retrace boundary.
    // This only rebases the beam epoch/traversal cursor; guest comparator words,
    // VI status and PI interrupt state remain untouched.
    static void RebaseViBeamToRetrace(uint64_t retraceMicros, bool secondHalf);

    // Exception-safe scalar access for HLE code. These keep Read32/Write32's
    // full mapping behavior and only convert an unmapped address into a failure
    // result; they are deliberately not MemoryInline::Try*GuestScalar, which is
    // the translated-code fast path over the page table.
    static bool TryRead32(uint32_t addr, uint32_t& value) noexcept {
        try {
            value = Read32(addr);
            return true;
        } catch (const AccessViolation&) {
            return false;
        }
    }

    static bool TryWrite32(uint32_t addr, uint32_t value) noexcept {
        try {
            Write32(addr, value);
            return true;
        } catch (const AccessViolation&) {
            return false;
        }
    }

    static uint8_t* GetPointer(uint32_t addr);
    static uint8_t* GetPointer(uint32_t addr, size_t length);
    static bool Contains(uint32_t addr, size_t length = 1);

    static uint64_t RegisterDeferredRead(uint32_t addr, size_t length,
                                         DeferredReadCallback callback, void* user);
    static void ClearDeferredReads();

    // Host-side delivery latch for the GameCube ARAM DMA interrupt. The DSP
    // status/mask bits remain guest-visible MMIO state; this only prevents the
    // same asserted hardware condition from being injected repeatedly before
    // the guest handler acknowledges it through DSPCR W1C semantics.
    static bool ClaimGameCubeAramInterrupt();
    // Raw GameCube ARAM shared by the DMA device and DSP sample decoder.
    static uint8_t* GetGameCubeAramPointer();

    // Claims one delivery of the currently asserted GameCube DI interrupt
    // line. DI/PI status and mask bits remain guest-owned MMIO state; the
    // host latch is rearmed when the guest acknowledges/deasserts the source.
    static bool ClaimGameCubeDiInterrupt();

    // Claims one delivery of the currently asserted GameCube SI interrupt
    // line. SI/PI status and mask bits remain guest-owned MMIO state; the
    // host latch is rearmed when the SI source deasserts.
    static bool ClaimGameCubeSiInterrupt();

    // Title opt-in: persistent raw card in slot A, retaining guest CARD code.
    static void EnableGameCubeMemoryCard();
    // Returns the enabled SDK EXI interrupt id (9..16), or -1.
    static int ClaimGameCubeExiInterrupt();

    // Claims one delivery of the GameCube audio-interface DMA interrupt.
    // The raw AID DMA registers and DSPCR status/mask bits remain guest-owned;
    // this only supplies the missing passage of host time to DMA completion and
    // prevents duplicate injection until the translated SDK acknowledges AIDINT.
    static bool ClaimGameCubeAudioDmaInterrupt();
    // Optional title policy for raw GameCube AID timing.  When enabled, the
    // next DMA period is re-anchored after an injected IRQ handler returns.
    static void SetGameCubeAudioDmaResyncAfterDelivery(bool enabled);
    static void CompleteGameCubeAudioDmaInterruptDelivery();

    // DSP task mail is a distinct interrupt source from mailbox-full.  The HLE
    // task core raises this when it posts an interrupting DSP->CPU protocol mail;
    // the translated SDK owns DSPCR.DSPINT/DSPINTMSK and its W1C acknowledgement.
    static void RaiseGameCubeDspInterrupt();
    static bool ClaimGameCubeDspInterrupt();
    // When a title hands first-task DSP boot to the shared HLE instead of running
    // the translated SDK boot handshake, retire the boot-ROM handoff mail that
    // handshake would otherwise consume. This does not touch later task mails.
    static void CompleteGameCubeDspHleBootHandshake();

    // sizeBytes reports the storage actually allocated, which is not always the
    // configured size (aliased MEM1/MEM2 windows are clamped to what is behind
    // them). Used by the crash dump.
    static std::vector<RegionConfig> DescribeRegions();
};

// The translated-code access layer is kept separately from the public memory API.
#include "memory_access.h"
