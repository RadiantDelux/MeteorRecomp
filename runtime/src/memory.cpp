#include "memory.h"
#include "gamecube_memory_card.h"
#include "gamecube_audio_dma_timing.h"
#include "audio_backend.h"
#include "platform/host_platform.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <optional>
#include "ppc_runtime.h"
#include "hle_stubs.h"
#include "runtime_log.h"
#include "runtime_config.h"
#include "system_bridge.h"
#include "wiicompiled_target.h"
#include <dolphin/pad.h>
#include <dolphin/si.h>
#include <mutex>
#include <sstream>
#include <unordered_map>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#endif

MemoryInline::PageEntry MemoryInline::g_pageTable[MemoryInline::kPageCount]{};
uintptr_t MemoryInline::g_fullPageBias[MemoryInline::kPageCount]{};
uintptr_t MemoryInline::g_fullReadablePageBias[MemoryInline::kPageCount]{};
uintptr_t MemoryInline::g_fullWritablePageBias[MemoryInline::kPageCount]{};
const MemoryInline::SparseWritablePageTable*
    MemoryInline::g_sparseWritablePageTables[MemoryInline::kPageCount]{};
uint8_t MemoryInline::g_deferredReadCoveredPages[MemoryInline::kPageCount]{};
template <typename T>
T MemoryInline::ReadResolvedFallback(uint32_t addr) {
    if constexpr (sizeof(T) == 1) return Memory::Read8(addr);
    if constexpr (sizeof(T) == 2) return Memory::Read16(addr);
    if constexpr (sizeof(T) == 4) return Memory::Read32(addr);
    return Memory::Read64(addr);
}
template uint8_t MemoryInline::ReadResolvedFallback<uint8_t>(uint32_t);
template uint16_t MemoryInline::ReadResolvedFallback<uint16_t>(uint32_t);
template uint32_t MemoryInline::ReadResolvedFallback<uint32_t>(uint32_t);
template uint64_t MemoryInline::ReadResolvedFallback<uint64_t>(uint32_t);
double MemoryInline::ReadResolvedFallbackFloat32(uint32_t addr) { return Memory::ReadFloat32(addr); }
double MemoryInline::ReadResolvedFallbackFloat64(uint32_t addr) { return Memory::ReadFloat64(addr); }
template <typename T>
void MemoryInline::WriteResolvedFallback(uint32_t addr, T value) {
    if constexpr (sizeof(T) == 1) Memory::Write8(addr, value);
    else if constexpr (sizeof(T) == 2) Memory::Write16(addr, value);
    else if constexpr (sizeof(T) == 4) Memory::Write32(addr, value);
    else Memory::Write64(addr, value);
}
template void MemoryInline::WriteResolvedFallback<uint8_t>(uint32_t, uint8_t);
template void MemoryInline::WriteResolvedFallback<uint16_t>(uint32_t, uint16_t);
template void MemoryInline::WriteResolvedFallback<uint32_t>(uint32_t, uint32_t);
template void MemoryInline::WriteResolvedFallback<uint64_t>(uint32_t, uint64_t);
void MemoryInline::WriteResolvedFallbackFloat32(uint32_t addr, double val) { Memory::WriteFloat32(addr, val); }
void MemoryInline::WriteResolvedFallbackFloat64(uint32_t addr, double val) { Memory::WriteFloat64(addr, val); }

// Shared extracted-disc backend implemented by hle/storage/dvd.cpp. Raw GameCube
// DI commands still use the same FST-backed byte source as the higher-level SDK HLE.
extern bool DVD_HLE_ReadWordsToGuest(uint32_t buffer, uint32_t length, uint32_t wordOffset);

namespace {
struct DeferredRead {
    uint64_t token = 0;
    uint32_t start = 0;
    size_t length = 0;
    Memory::DeferredReadCallback callback = nullptr;
    void* user = nullptr;
};

std::mutex& DeferredReadMutex() {
    static std::mutex mutex;
    return mutex;
}

std::vector<DeferredRead>& DeferredReads() {
    static std::vector<DeferredRead> reads;
    return reads;
}

uint64_t& NextDeferredReadToken() {
    static uint64_t token = 1;
    return token;
}

bool RangesOverlap(uint32_t firstStart, size_t firstLength,
                   uint32_t secondStart, size_t secondLength) {
    const uint64_t firstEnd = static_cast<uint64_t>(firstStart) + firstLength;
    const uint64_t secondEnd = static_cast<uint64_t>(secondStart) + secondLength;
    return static_cast<uint64_t>(firstStart) < secondEnd &&
           static_cast<uint64_t>(secondStart) < firstEnd;
}

void RefreshDeferredReadPage(uint32_t page) {
    const uint32_t pageStart = page << MemoryInline::kPageShift;
    const auto& reads = DeferredReads();
    const bool covered = std::any_of(reads.begin(), reads.end(), [pageStart](const DeferredRead& read) {
        return RangesOverlap(pageStart, MemoryInline::kPageSize, read.start, read.length);
    });
    MemoryInline::g_fullReadablePageBias[page] =
        !covered ? MemoryInline::g_fullPageBias[page] : 0;
    MemoryInline::g_deferredReadCoveredPages[page] = covered ? 1 : 0;
}

struct Region {
    Memory::RegionConfig config;
    uint8_t* storagePtr = nullptr;
    size_t storageSize = 0;
};

std::vector<Region>& Regions() {
    static std::vector<Region> regions;
    return regions;
}

std::mutex& RegionMutex() {
    static std::mutex mutex;
    return mutex;
}

std::array<std::unique_ptr<MemoryInline::SparseWritablePageTable>,
           MemoryInline::kPageCount>& SparseWritablePageTableStorage() {
    static std::array<std::unique_ptr<MemoryInline::SparseWritablePageTable>,
                      MemoryInline::kPageCount> storage;
    return storage;
}

void ClearPageTable() {
    auto* table = MemoryInline::g_pageTable;
    for (uint32_t i = 0; i < MemoryInline::kPageCount; ++i) {
        auto& entry = table[i];
        entry.base = nullptr;
        entry.limit = 0;
        MemoryInline::g_fullPageBias[i] = 0;
        MemoryInline::g_fullReadablePageBias[i] = 0;
        MemoryInline::g_fullWritablePageBias[i] = 0;
        MemoryInline::g_deferredReadCoveredPages[i] = 0;
        MemoryInline::g_sparseWritablePageTables[i] = nullptr;
        SparseWritablePageTableStorage()[i].reset();
    }
}

} // namespace

uint64_t Memory::RegisterDeferredRead(uint32_t addr, size_t length,
                                      DeferredReadCallback callback, void* user) {
    if (length == 0 || callback == nullptr ||
        static_cast<uint64_t>(addr) + length > (uint64_t{1} << 32)) {
        return 0;
    }

    std::lock_guard lock{DeferredReadMutex()};
    uint64_t token = NextDeferredReadToken()++;
    if (token == 0) token = NextDeferredReadToken()++;
    DeferredReads().push_back({token, addr, length, callback, user});
    const uint32_t firstPage = addr >> MemoryInline::kPageShift;
    const uint32_t lastPage = static_cast<uint32_t>(
        (static_cast<uint64_t>(addr) + length - 1) >> MemoryInline::kPageShift);
    for (uint32_t page = firstPage; page <= lastPage; ++page) {
        MemoryInline::g_fullReadablePageBias[page] = 0;
        MemoryInline::g_deferredReadCoveredPages[page] = 1;
    }
    // Clearing the readable bias only intercepts the checked path. A flat read
    // needs the host pages themselves to trap, which is what PAGE_NOACCESS on
    // the guest view does; the vectored handler materializes the copy, restores
    // the protection and re-runs the access.
    GuestFlat::ProtectDeferredRange(addr, length);
    return token;
}

void Memory::ClearDeferredReads() {
    std::lock_guard lock{DeferredReadMutex()};
    auto& reads = DeferredReads();
    std::vector<uint32_t> pages;
    for (const auto& read : reads) {
        const uint32_t firstPage = read.start >> MemoryInline::kPageShift;
        const uint32_t lastPage = static_cast<uint32_t>(
            (static_cast<uint64_t>(read.start) + read.length - 1) >> MemoryInline::kPageShift);
        for (uint32_t page = firstPage; page <= lastPage; ++page) pages.push_back(page);
        GuestFlat::UnprotectDeferredRange(read.start, read.length);
    }
    reads.clear();
    std::sort(pages.begin(), pages.end());
    pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
    for (uint32_t page : pages) RefreshDeferredReadPage(page);
}

bool MemoryInline::ResolveDeferredReads(uint32_t addr, size_t length) {
    std::vector<DeferredRead> matches;
    std::vector<uint32_t> affectedPages;
    {
        std::lock_guard lock{DeferredReadMutex()};
        auto& reads = DeferredReads();
        for (auto it = reads.begin(); it != reads.end();) {
            if (!RangesOverlap(addr, length, it->start, it->length)) {
                ++it;
                continue;
            }
            const uint32_t firstPage = it->start >> kPageShift;
            const uint32_t lastPage = static_cast<uint32_t>(
                (static_cast<uint64_t>(it->start) + it->length - 1) >> kPageShift);
            for (uint32_t page = firstPage; page <= lastPage; ++page) affectedPages.push_back(page);
            matches.push_back(*it);
            // Idempotent: the vectored handler already dropped the protection
            // for the range whose trap brought us here.
            GuestFlat::UnprotectDeferredRange(it->start, it->length);
            it = reads.erase(it);
        }
        std::sort(affectedPages.begin(), affectedPages.end());
        affectedPages.erase(std::unique(affectedPages.begin(), affectedPages.end()), affectedPages.end());
        for (uint32_t page : affectedPages) RefreshDeferredReadPage(page);
    }

    // The range is removed before entering renderer code so any memory reads
    // made while submitting the copy cannot recursively trigger it.
    for (const auto& match : matches) {
        if (!match.callback(match.user)) {
            throw Memory::AccessViolation(addr, length, "deferred read materialization failed");
        }
    }
    return true;
}

namespace {

void BuildPageTable() {
    auto* table = MemoryInline::g_pageTable;
    for (const auto& region : Regions()) {
        const uint64_t base = region.config.baseAddress;
        const uint64_t size = region.storageSize;
        const uint64_t end = base + size;
        if (size == 0) {
            continue;
        }

        const uint32_t startPage = static_cast<uint32_t>(base >> MemoryInline::kPageShift);
        const uint32_t endPage = static_cast<uint32_t>((end - 1) >> MemoryInline::kPageShift);
        for (uint32_t page = startPage; page <= endPage; ++page) {
            const uint64_t pageBase = static_cast<uint64_t>(page) << MemoryInline::kPageShift;
            const uint64_t offset = pageBase - base;
            if (offset >= size) {
                continue;
            }
            const uint32_t limit = static_cast<uint32_t>(std::min<uint64_t>(MemoryInline::kPageSize, size - offset));
            table[page].base = region.storagePtr + static_cast<size_t>(offset);
            table[page].limit = limit;
        }
    }

    // The last page of each contiguous mapping remains on the checked path so
    // an access straddling its end cannot escape the mapped region. Every
    // preceding full page can safely service native accesses up to 8 bytes,
    // including a cross-page access into its contiguous successor.
    for (uint32_t page = 0; page + 1 < MemoryInline::kPageCount; ++page) {
        const auto& current = table[page];
        const auto& next = table[page + 1];
        if (!current.base || current.limit != MemoryInline::kPageSize ||
            !next.base || next.limit < MemoryInline::kMaxFastScalarSize - 1u ||
            next.base != current.base + MemoryInline::kPageSize)
            continue;
        const uintptr_t guestPageBase = static_cast<uintptr_t>(page) << MemoryInline::kPageShift;
        const uintptr_t bias = reinterpret_cast<uintptr_t>(current.base) - guestPageBase;
        MemoryInline::g_fullPageBias[page] = bias + 1u;
    }
}

void RefreshFastPathTables() {
    auto& sparseStorage = SparseWritablePageTableStorage();
    for (uint32_t page = 0; page < MemoryInline::kPageCount; ++page) {
        const uintptr_t bias = MemoryInline::g_fullPageBias[page];
        MemoryInline::g_fullReadablePageBias[page] = bias;
        MemoryInline::g_deferredReadCoveredPages[page] = 0;
        MemoryInline::g_sparseWritablePageTables[page] = nullptr;
        sparseStorage[page].reset();

        const bool coarseExecutable =
            RecompMod::g_executableWriteGuardCoarsePages[page].load(
                std::memory_order_relaxed) != 0;
        MemoryInline::g_fullWritablePageBias[page] = coarseExecutable ? 0 : bias;
        if (!coarseExecutable || bias == 0)
            continue;

        auto sparse = std::make_unique<MemoryInline::SparseWritablePageTable>();
        bool hasExecutableSubPage = false;
        bool hasWritableSubPage = false;
        const uint32_t firstExactPage =
            page * MemoryInline::kWritableSubPagesPerPage;
        for (uint32_t subPage = 0;
             subPage < MemoryInline::kWritableSubPagesPerPage; ++subPage) {
            const bool executable =
                RecompMod::g_executableWriteGuardPages[firstExactPage + subPage].load(
                    std::memory_order_relaxed) != 0;
            hasExecutableSubPage |= executable;
            hasWritableSubPage |= !executable;
            sparse->encodedBias[subPage] = executable ? 0 : bias;
        }

        // A homogeneous executable page has no writable fast path; a
        // homogeneous data page already uses g_fullWritablePageBias. Retain an
        // allocation only for the intended mixed case.
        if (hasExecutableSubPage && hasWritableSubPage) {
            MemoryInline::g_sparseWritablePageTables[page] = sparse.get();
            sparseStorage[page] = std::move(sparse);
        }
    }
    std::lock_guard lock{DeferredReadMutex()};
    for (const auto& read : DeferredReads()) {
        const uint32_t firstPage = read.start >> MemoryInline::kPageShift;
        const uint32_t lastPage = static_cast<uint32_t>(
            (static_cast<uint64_t>(read.start) + read.length - 1) >> MemoryInline::kPageShift);
        for (uint32_t page = firstPage; page <= lastPage; ++page) {
            MemoryInline::g_fullReadablePageBias[page] = 0;
            MemoryInline::g_deferredReadCoveredPages[page] = 1;
        }
    }
}

Region* TryResolveRegion(uint32_t address, size_t length) {
    auto& regions = Regions();
    for (auto& region : regions) {
        const uint64_t base = region.config.baseAddress;
        const uint64_t limit = base + region.storageSize;
        const uint64_t addr = address;
        const uint64_t end = addr + length;
        if (addr >= base && end <= limit) {
            return &region;
        }
    }
    return nullptr;
}

Region& ResolveRegion(uint32_t address, size_t length) {
    if (Region* region = TryResolveRegion(address, length)) {
        return *region;
    }
    // Emit extra context to help diagnose early-boot accesses that miss the map.
    if (auto* cpu = TryGetCpuContext()) {
        const uint32_t active = RecompMod::CurrentTranslatedExecutionAddress();
        RT_LOG(RT_TAG_MEMORY) << "AccessViolation ctx=" << cpu
                  << " pc=0x" << std::hex << cpu->pc
                  << " active=0x" << active
                  << " lr=0x" << cpu->lr
                  << " r1=0x" << cpu->gpr[1]
                  << " r8=0x" << cpu->gpr[8]
                  << " r9=0x" << cpu->gpr[9]
                  << " r12=0x" << cpu->gpr[12]
                  << " r30=0x" << cpu->gpr[30]
                  << " r31=0x" << cpu->gpr[31]
                  << std::dec
                  << " addr=0x" << std::hex << address
                  << " len=" << length << std::dec
                  << " reason=no mapped region" << std::endl;
    } else {
        RT_LOG(RT_TAG_MEMORY) << "AccessViolation ctx=(null) addr=0x" << std::hex << address
                  << " len=" << length << std::dec << " reason=no mapped region" << std::endl;
    }
    RT_LOG(RT_TAG_MEMORY) << "===== DUMPING CPU STATE =====" << std::endl;
    SystemBridge::DumpCpuState(TryGetCpuContext());
    // Hexdump guest memory around pointer-carrying registers so a corrupted
    // structure's surroundings (e.g. ASCII sprayed over a link pointer) are
    // visible in the report without a debugger attached.
    if (auto* cpu = TryGetCpuContext()) {
        for (const int reg : {4, 5, 6, 7, 8, 26, 27, 28, 29, 30, 31}) {
            const uint32_t base = cpu->gpr[reg];
            if (base < 0x80000000u || base >= 0x94000000u)
                continue;
            const uint32_t start = (base - 0x40u) & ~0xFu;
            RT_LOG(RT_TAG_MEMORY) << "hexdump around r" << reg << "=0x" << std::hex << base << ":" << std::endl;
            for (uint32_t row = 0; row < 16; ++row) {
                const uint32_t rowAddr = start + row * 16u;
                RT_LOG(RT_TAG_MEMORY) << "  0x" << std::hex << rowAddr << ":";
                char ascii[17] = {};
                for (uint32_t i = 0; i < 16; ++i) {
                    uint8_t byte = 0;
                    if (!MemoryInline::TryReadGuestScalar(rowAddr + i, byte)) {
                        std::cerr << " ??";
                        ascii[i] = '?';
                        continue;
                    }
                    std::cerr << " " << std::setw(2) << std::setfill('0') << static_cast<uint32_t>(byte);
                    ascii[i] = (byte >= 0x20 && byte < 0x7F) ? static_cast<char>(byte) : '.';
                }
                std::cerr << "  |" << ascii << "|" << std::dec << std::setfill(' ') << std::endl;
            }
        }
    }
#if defined(_WIN32)
    void* frames[32]{};
    const USHORT captured = CaptureStackBackTrace(0, static_cast<DWORD>(std::size(frames)), frames, nullptr);
    const auto imageBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    HANDLE process = GetCurrentProcess();
    static const bool symbolsReady = [] {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
        if (SymInitialize(GetCurrentProcess(), nullptr, TRUE) != FALSE)
            return true;
        // The runtime crash reporter may already own the process-wide DbgHelp
        // session. Reuse it rather than treating ERROR_INVALID_PARAMETER as
        // symbol unavailability.
        return GetLastError() == ERROR_INVALID_PARAMETER;
    }();
    RT_LOG(RT_TAG_MEMORY) << "host stack at first invalid guest access:" << std::endl;
    for (USHORT index = 0; index < captured; ++index) {
        const auto addressValue = reinterpret_cast<uintptr_t>(frames[index]);
        RT_LOG(RT_TAG_MEMORY) << "  #" << index << " absolute=0x" << std::hex << addressValue;
        if (imageBase != 0 && addressValue >= imageBase)
            std::cerr << " image+0x" << (addressValue - imageBase);
        std::array<char, sizeof(SYMBOL_INFO) + MAX_SYM_NAME> symbolBuffer{};
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbolBuffer.data());
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = MAX_SYM_NAME;
        DWORD64 displacement = 0;
        if (symbolsReady && SymFromAddr(process, addressValue, &displacement, symbol))
            std::cerr << " " << symbol->Name << "+0x" << displacement;
        std::cerr << std::dec << std::endl;
    }
#endif
    std::cerr.flush();
    throw Memory::AccessViolation(address, length, "no mapped region");
}

GuestFlat::Backing ClassifyBacking(uint32_t baseAddress) {
    // MEM1: physical (0x00000000), cached (0x80000000), uncached (0xC0000000)
    if ((baseAddress >= Memory::kMem1PhysicalBase &&
         baseAddress < Memory::kMem1PhysicalBase + Memory::kMem1Size) ||
        (baseAddress >= Memory::kMem1CachedBase &&
         baseAddress < Memory::kMem1CachedBase + Memory::kMem1Size) ||
        (baseAddress >= Memory::kMem1UncachedBase &&
         baseAddress < Memory::kMem1UncachedBase + Memory::kMem1Size)) {
        return GuestFlat::Backing::Mem1;
    }

    // NDEV-sized MEM2: physical (0x10000000), cached (0x90000000),
    // uncached (0xD0000000). Mario Kart Wii detects this configuration and
    // creates its original EGGRootDebug expansion heap.
    if ((baseAddress >= Memory::kMem2PhysicalBase &&
         baseAddress < Memory::kMem2PhysicalEnd) ||
        (baseAddress >= Memory::kMem2CachedBase &&
         baseAddress < Memory::kMem2CachedEnd) ||
        (baseAddress >= Memory::kMem2UncachedBase &&
         baseAddress < Memory::kMem2UncachedEnd)) {
        return GuestFlat::Backing::Mem2;
    }

    return GuestFlat::Backing::Owned;
}


template <typename T>
T ReadScalar(uint32_t address) {
    // This is the cold *Slow path; Memory::GetPointer already tries the fast
    // probe first, so repeating it here only guaranteed a second miss.
    auto* ptr = Memory::GetPointer(address, sizeof(T));
    if constexpr (sizeof(T) == 1) {
        return *ptr;
    } else {
        T value = 0;
        std::memcpy(&value, ptr, sizeof(T));
        return MemoryInline::MaybeByteSwap(value);
    }
}

template <typename T>
void WriteScalar(uint32_t address, T value) {
    if (RecompMod::HandleExecutableWrite(address, sizeof(T), static_cast<uint64_t>(value))) {
        return;
    }

    auto* ptr = Memory::GetPointer(address, sizeof(T));
    if constexpr (sizeof(T) == 1) {
        *ptr = static_cast<uint8_t>(value);
    } else {
        const T swapped = MemoryInline::MaybeByteSwap(value);
        std::memcpy(ptr, &swapped, sizeof(T));
    }
}

} // namespace

Memory::AccessViolation::AccessViolation(uint32_t address, size_t length, std::string_view reason)
    : std::runtime_error([&]() {
          std::ostringstream oss;
          oss << "Memory access violation at 0x" << std::hex << std::uppercase << address
              << " (+0x" << length << ") :: " << reason;
          return oss.str();
      }()),
      address_(address),
      length_(length),
      reason_(reason) {}

void Memory::RefreshWritableFastPathsForExecutableRanges() {
    std::lock_guard<std::mutex> lock(RegionMutex());
    RefreshFastPathTables();
}

Memory::Config Memory::Config::WiiDefaults() {
    // Physical / cached / uncached views of MEM1 and MEM2, then the locked cache.
    Config config;

    config.regions.push_back(RegionConfig{
        .name = "MEM1_PHYS",
        .baseAddress = Memory::kMem1PhysicalBase,
        .sizeBytes = Memory::kMem1Size,
    });

    config.regions.push_back(RegionConfig{
        .name = "MEM1",
        .baseAddress = Memory::kMem1CachedBase,
        .sizeBytes = Memory::kMem1Size,
    });

    config.regions.push_back(RegionConfig{
        .name = "MEM1_UNCACHED",
        .baseAddress = Memory::kMem1UncachedBase,
        .sizeBytes = Memory::kMem1Size,
    });

    config.regions.push_back(RegionConfig{
        .name = "MEM2_PHYS",
        .baseAddress = Memory::kMem2PhysicalBase,
        .sizeBytes = Memory::kMem2Size,
    });

    config.regions.push_back(RegionConfig{
        .name = "MEM2",
        .baseAddress = Memory::kMem2CachedBase,
        .sizeBytes = Memory::kMem2Size,
    });

    config.regions.push_back(RegionConfig{
        .name = "MEM2_UNCACHED",
        .baseAddress = Memory::kMem2UncachedBase,
        .sizeBytes = Memory::kMem2Size,
    });

    // Kamek module overlay: 2 MiB above MEM1, which the game believes ends at 0x81800000, so this
    // costs no arena space (unlike the old in-arena reservation that shrank the race scene heaps).
    // Base must stay within +/-32 MiB of every DOL/StaticR hook site for Kamek Rel24 branches to encode.
    config.regions.push_back(RegionConfig{
        .name = "MEM1_KAMEK_OVERLAY",
        .baseAddress = 0x81800000,
        .sizeBytes = 0x200000,
    });

    // Locked cache (THP decoder fast RAM) is really 16KB at 0xE0000000, but a sub-page mapping can
    // never enter the coarse 1MiB bias tables, forcing every THP load/store through the checked
    // fallback (the dominant cost of THP-heavy screens). Back a full 1MiB page plus the successor
    // page the bias builder needs, so locked-cache access stays on the two-instruction native path.
    constexpr size_t lcSize = MemoryInline::kPageSize + 4096u;
    config.regions.push_back(RegionConfig{
        .name = "LOCKED_CACHE",
        .baseAddress = 0xE0000000,
        .sizeBytes = lcSize,
    });

    return config;
}

Memory::Config Memory::Config::GameCubeDefaults() {
    // GameCube has the same 24 MiB MEM1 physical/cached/uncached views as Wii,
    // but no CPU-addressable MEM2. ARAM is a separate DSP-owned memory and must
    // not be exposed here as 0x90000000 RAM.
    Config config;
    config.regions.push_back(RegionConfig{
        .name = "MEM1_PHYS",
        .baseAddress = Memory::kMem1PhysicalBase,
        .sizeBytes = Memory::kMem1Size,
    });
    config.regions.push_back(RegionConfig{
        .name = "MEM1",
        .baseAddress = Memory::kMem1CachedBase,
        .sizeBytes = Memory::kMem1Size,
    });
    config.regions.push_back(RegionConfig{
        .name = "MEM1_UNCACHED",
        .baseAddress = Memory::kMem1UncachedBase,
        .sizeBytes = Memory::kMem1Size,
    });

    // Gekko supports locked L1 just like Broadway. Keep the existing enlarged
    // host backing so translated locked-cache accesses retain their fast path.
    constexpr size_t lcSize = MemoryInline::kPageSize + 4096u;
    config.regions.push_back(RegionConfig{
        .name = "LOCKED_CACHE",
        .baseAddress = 0xE0000000,
        .sizeBytes = lcSize,
    });
    return config;
}

void Memory::Init(size_t mem1Size) {
    Config config;
    config.regions.push_back(RegionConfig{.name = "MEM1", .baseAddress = 0x80000000, .sizeBytes = mem1Size});
    Init(config);
}

void Memory::Init(const Config& config) {
    std::lock_guard<std::mutex> lock(RegionMutex());
    auto& regions = Regions();
    regions.clear();
    ClearPageTable();
    regions.reserve(config.regions.size());

    // The flat reservation backs every guest view from one shared section object, so cached/uncached/
    // physical mirrors alias as before. GetPointer still hands out the unprotected host alias, so
    // native code (image loading, DVD reads, HLE) is unaffected by the guest view's protections.
    {
        std::vector<GuestFlat::RegionRequest> flatRegions;
        flatRegions.reserve(config.regions.size());
        for (const auto& regionConfig : config.regions) {
            flatRegions.push_back(GuestFlat::RegionRequest{
                regionConfig.baseAddress, regionConfig.sizeBytes,
                ClassifyBacking(regionConfig.baseAddress)});
        }
        GuestFlat::Initialize(flatRegions);
    }

    // Map virtual regions onto that backing store, with mirroring.
    for (const auto& regionConfig : config.regions) {
        Region instance;
        instance.config = regionConfig;

        instance.storagePtr = GuestFlat::HostPointer(regionConfig.baseAddress);
        if (instance.storagePtr == nullptr && regionConfig.sizeBytes != 0) {
            throw std::runtime_error("Flat guest mapping is missing region '" + regionConfig.name + "'");
        }
        instance.storageSize = regionConfig.sizeBytes;

        regions.emplace_back(std::move(instance));
    }

    BuildPageTable();
    RefreshFastPathTables();
}

void Memory::Reset() {
    ClearDeferredReads();
    std::lock_guard<std::mutex> lock(RegionMutex());
    Regions().clear();
    ClearPageTable();
}

namespace {
constexpr uint32_t kPiBase = 0xCC003000u;
constexpr uint32_t kPiInterruptCause = kPiBase + 0x00u;
constexpr uint32_t kPiInterruptMask = kPiBase + 0x04u;
constexpr uint32_t kPiFifoBase = kPiBase + 0x0Cu;
constexpr uint32_t kPiFifoEnd = kPiBase + 0x10u;
constexpr uint32_t kPiFifoWritePointer = kPiBase + 0x14u;
constexpr uint32_t kPiFifoReset = kPiBase + 0x18u;
constexpr uint32_t kPiErrorCause = kPiBase + 0x1Cu;
constexpr uint32_t kPiErrorAddress = kPiBase + 0x20u;
constexpr uint32_t kPiResetCode = kPiBase + 0x24u;
constexpr uint32_t kPiUnknown = kPiBase + 0x28u;
constexpr uint32_t kPiFlipperRevision = kPiBase + 0x2Cu;
constexpr uint32_t kPiFlipperBusStrength = kPiBase + 0x30u;
constexpr uint32_t kFlipperRevisionC = 0x246500B1u;
constexpr uint32_t kPiInitialInterruptCause = 0x00010100u; // reset button state | VI
#if WIICOMPILED_GUEST_IS_GAMECUBE
constexpr uint32_t kPiFifoAddressMask = 0x03FFFFE0u;
#else
constexpr uint32_t kPiFifoAddressMask = 0x1FFFFFE0u;
#endif
// Hollywood exposes the same Broadway-owned GPIO bank through the ordinary
// PPC window and the AHB "trusted" mirror. Retail SDK AVE/I2C code uses the
// trusted 0xCD8000xx mirror even when running on Broadway, while other SDK
// paths (for example sensor-bar power) use 0xCD0000xx.
constexpr uint32_t kHollywoodGpioBOut = 0xCD0000C0u;
constexpr uint32_t kHollywoodGpioBOutTrusted = 0xCD8000C0u;
constexpr uint32_t kHollywoodGpioBDirTrusted = 0xCD8000C4u;
constexpr uint32_t kHollywoodGpioBInTrusted = 0xCD8000C8u;
constexpr uint32_t kHollywoodGpioSensorBar = 0x00000100u;
constexpr uint32_t kHollywoodCompat = 0xCD800180u;
constexpr uint32_t kHollywoodPllAi = 0xCD8001CCu;
constexpr uint32_t kHollywoodPllAiExt = 0xCD8001D0u;
constexpr uint32_t kMiBase = 0xCC004000u;
constexpr uint32_t kMiLast = kMiBase + 0x5Au;
constexpr uint32_t kDspBase = 0xCC005000u;
constexpr uint32_t kDspMailboxToHi = kDspBase + 0x00u;
constexpr uint32_t kDspMailboxToLo = kDspBase + 0x02u;
constexpr uint32_t kDspMailboxFromHi = kDspBase + 0x04u;
constexpr uint32_t kDspMailboxFromLo = kDspBase + 0x06u;
constexpr uint32_t kDspControl = kDspBase + 0x0Au;
constexpr uint32_t kArInfo = kDspBase + 0x12u;
constexpr uint32_t kArMode = kDspBase + 0x16u;
constexpr uint32_t kArRefresh = kDspBase + 0x1Au;
constexpr uint32_t kArDmaMmAddrHi = kDspBase + 0x20u;
constexpr uint32_t kArDmaMmAddrLo = kDspBase + 0x22u;
constexpr uint32_t kArDmaArAddrHi = kDspBase + 0x24u;
constexpr uint32_t kArDmaArAddrLo = kDspBase + 0x26u;
constexpr uint32_t kArDmaCountHi = kDspBase + 0x28u;
constexpr uint32_t kArDmaCountLo = kDspBase + 0x2Au;
constexpr uint32_t kAudioDmaStartHi = kDspBase + 0x30u;
constexpr uint32_t kAudioDmaStartLo = kDspBase + 0x32u;
constexpr uint32_t kAudioDmaBlocksLength = kDspBase + 0x34u;
constexpr uint32_t kAudioDmaControlLength = kDspBase + 0x36u;
constexpr uint32_t kAudioDmaBlocksLeft = kDspBase + 0x3Au;
#if WIICOMPILED_GUEST_IS_GAMECUBE
constexpr uint32_t kAiBase = 0xCC006C00u;
#else
// Broadway keeps the Flipper-compatible audio interface at the Wii MMIO alias.
constexpr uint32_t kAiBase = 0xCD006C00u;
#endif
constexpr uint32_t kAiControl = kAiBase + 0x00u;
constexpr uint32_t kAiVolume = kAiBase + 0x04u;
constexpr uint32_t kAiSampleCounter = kAiBase + 0x08u;
constexpr uint32_t kAiInterruptTiming = kAiBase + 0x0Cu;
constexpr uint32_t kAiPersistentControlMask = 0x00000057u; // PSTAT,AISFR,AIINTMSK,AIINTVLD,AIDFR
constexpr uint32_t kAiInterruptBit = 0x00000008u;
constexpr uint32_t kAiSampleCounterResetBit = 0x00000020u;
#if WIICOMPILED_GUEST_IS_GAMECUBE
constexpr uint32_t kExiBase = 0xCC006800u;
#else
// Broadway maps the same three EXI channel register blocks through the Wii
// hardware window at 0xCD006800.  Keep one register/device model and select the
// physical base from the guest platform rather than duplicating EXI semantics.
constexpr uint32_t kExiBase = 0xCD006800u;
#endif
constexpr uint32_t kExiChannelStride = 0x14u;
constexpr uint32_t kExiChannelCount = 3u;
constexpr uint32_t kExiLastRegister = kExiBase + (kExiChannelCount - 1u) * kExiChannelStride + 0x10u;
constexpr uint32_t kExiStatusPersistentMask = 0x000007F5u;
constexpr uint32_t kExiStatusFlagMask = 0x0000080Au; // EXIINT, TCINT, EXTINT are W1C
constexpr uint32_t kExiStatusExtBit = 0x00001000u;
constexpr uint32_t kExiStatusRomDisableBit = 0x00002000u;
constexpr uint32_t kExiStatusChipSelectMask = 0x00000380u;
constexpr uint32_t kExiStatusTransferCompleteBit = 0x00000008u;
constexpr uint32_t kExiControlStartBit = 0x00000001u;
constexpr uint32_t kExiControlDmaBit = 0x00000002u;
constexpr uint32_t kExiIplChipSelect = 2u;
constexpr uint32_t kIplRomSize = 0x00200000u;
constexpr uint32_t kIplSramBase = 0x00800000u;
constexpr uint32_t kIplSramSize = 0x44u;
#if WIICOMPILED_GUEST_IS_GAMECUBE
constexpr uint32_t kDiBase = 0xCC006000u;
#else
// The Wii exposes the same DI register layout through 0xCD006000.  PPC-side
// SDK code reads DICFG directly even though ordinary Wii disc I/O is normally
// mediated by IOS /dev/di.
constexpr uint32_t kDiBase = 0xCD006000u;
#endif
constexpr uint32_t kDiStatus = kDiBase + 0x00u;
constexpr uint32_t kDiCover = kDiBase + 0x04u;
constexpr uint32_t kDiCommand0 = kDiBase + 0x08u;
constexpr uint32_t kDiCommand1 = kDiBase + 0x0Cu;
constexpr uint32_t kDiCommand2 = kDiBase + 0x10u;
constexpr uint32_t kDiDmaAddress = kDiBase + 0x14u;
constexpr uint32_t kDiDmaLength = kDiBase + 0x18u;
constexpr uint32_t kDiDmaControl = kDiBase + 0x1Cu;
constexpr uint32_t kDiImmediate = kDiBase + 0x20u;
constexpr uint32_t kDiConfig = kDiBase + 0x24u;
constexpr uint32_t kDiStatusErrorInterrupt = 0x00000004u;
constexpr uint32_t kDiStatusTransferComplete = 0x00000010u;
constexpr uint32_t kDiStatusWritableStateMask = 0x0000002Bu; // BREAK + interrupt masks
constexpr uint32_t kDiStatusW1cMask = 0x00000054u; // DEINT, TCINT, BRKINT
constexpr uint32_t kDiCoverMaskBit = 0x00000002u;
constexpr uint32_t kDiCoverW1cBit = 0x00000004u;
constexpr uint32_t kViBase = 0xCC002000u;
constexpr uint32_t kViLast = kViBase + 0x74u;
constexpr uint32_t kViDisplayControl = kViBase + 0x02u;
constexpr uint16_t kViDisplayControlFormatMask = 0x0300u;
constexpr uint32_t kViVerticalBeamPosition = kViBase + 0x2Cu;
constexpr uint32_t kViHorizontalBeamPosition = kViBase + 0x2Eu;
constexpr uint32_t kViPreRetraceHi = kViBase + 0x30u;
constexpr uint32_t kViPostRetraceHi = kViBase + 0x34u;
constexpr uint16_t kViInterruptMaskBit = 0x1000u;
constexpr uint16_t kViInterruptStatusBit = 0x8000u;
constexpr uint32_t kViPiCauseBit = 0x00000100u;
constexpr std::array<uint32_t, 4> kViInterruptHighOffsets{0x30u, 0x34u, 0x38u, 0x3Cu};
constexpr uint32_t kViClock = kViBase + 0x6Cu;
constexpr uint32_t kViDtvStatus = kViBase + 0x6Eu;
constexpr uint32_t kViRegisterCount = 0x76u / 2u;
#if WIICOMPILED_GUEST_IS_GAMECUBE
constexpr uint32_t kSiBase = 0xCC006400u;
#else
// RVL keeps the Flipper-compatible SI block but maps it through 0xCD006400.
constexpr uint32_t kSiBase = 0xCD006400u;
#endif
constexpr uint32_t kSiPoll = kSiBase + 0x30u;
constexpr uint32_t kSiComCsr = kSiBase + 0x34u;
constexpr uint32_t kSiStatus = kSiBase + 0x38u;
constexpr uint32_t kSiExiClock = kSiBase + 0x3Cu;
constexpr uint32_t kSiBufferBase = kSiBase + 0x80u;
constexpr uint32_t kSiBufferEnd = kSiBufferBase + 0x7Fu;
constexpr uint32_t kSiComPersistentMask = 0x4F7F7FC6u;
constexpr uint32_t kSiComRdstInterrupt = 0x10000000u;
constexpr uint32_t kSiComRdstMask = 0x08000000u;
constexpr uint32_t kSiComError = 0x20000000u;
constexpr uint32_t kSiComTcMask = 0x40000000u;
constexpr uint32_t kSiComTcInterrupt = 0x80000000u;
constexpr uint32_t kSiStatusWriteClearMask = 0x0F0F0F0Fu;

// Broadway's GPIOB output register is guest-visible hardware state. The host has no physical Wii
// sensor-bar power rail, so preserve the register value and consume the hardware-facing side effect
// here. This is deliberately one exact implemented register; every other unknown Hollywood MMIO
// address remains a strict fault below.
std::atomic<uint32_t> g_hollywoodGpioBOut{0};
std::atomic<uint32_t> g_hollywoodGpioBDir{0};
std::atomic<uint32_t> g_hollywoodCompat{0};
std::atomic<uint32_t> g_hollywoodPllAi{0};
std::atomic<uint32_t> g_hollywoodPllAiExt{0};
std::atomic<bool> g_loggedPiColdBoot{false};
std::atomic<uint32_t> g_piInterruptCause{kPiInitialInterruptCause};
std::atomic<uint32_t> g_piInterruptMask{0};
std::atomic<uint32_t> g_piFifoBase{0};
std::atomic<uint32_t> g_piFifoEnd{0};
std::atomic<uint32_t> g_piFifoWritePointer{0};
std::atomic<uint32_t> g_piErrorCause{0};
std::atomic<uint32_t> g_piErrorAddress{0};
std::atomic<uint32_t> g_piResetCode{0};
std::atomic<uint32_t> g_piUnknown{0x000001FFu};
std::atomic<uint32_t> g_piFlipperBusStrength{0x02492492u};
std::array<std::atomic<uint16_t>, 0x2Eu> g_miRegisters{};
constexpr uint32_t kGameCubeAramSize = 16u * 1024u * 1024u;
constexpr uint32_t kGameCubeAramAddressMask = 0x03FFFFFFu;
constexpr uint32_t kDspMailboxFullBit = 0x8000u;
constexpr uint32_t kDspBootMail = 0x00543448u;
std::vector<uint8_t> g_gameCubeAram(kGameCubeAramSize, 0);
std::mutex g_dspMutex;
uint16_t g_dspToMailboxHi = 0;
uint16_t g_dspToMailboxLo = 0;
bool g_dspToMailboxFull = false;
uint32_t g_dspFromMailbox = 0;
bool g_dspFromMailboxFull = false;
bool g_dspBootstrapMailPending = false;
std::atomic<uint16_t> g_dspControl{0x0004u}; // DSP starts halted
std::atomic<bool> g_gameCubeAudioDmaResyncAfterDelivery{false};
std::atomic<bool> g_gameCubeAramInterruptDelivered{false};
std::atomic<bool> g_gameCubeAudioDmaInterruptDelivered{false};
std::atomic<uint32_t> g_audioDmaLatchedStart{0};
std::atomic<uint16_t> g_audioDmaLatchedBlocks{0};
std::atomic<bool> g_gameCubeDspInterruptDelivered{false};
std::atomic<uint16_t> g_arInfo{0};
std::atomic<uint16_t> g_arRefresh{156u};
std::atomic<uint16_t> g_arDmaMmAddrHi{0};
std::atomic<uint16_t> g_arDmaMmAddrLo{0};
std::atomic<uint16_t> g_arDmaArAddrHi{0};
std::atomic<uint16_t> g_arDmaArAddrLo{0};
std::atomic<uint16_t> g_arDmaCountHi{0};
std::atomic<uint16_t> g_arDmaCountLo{0};
std::atomic<uint16_t> g_audioDmaStartHi{0};
std::atomic<uint16_t> g_audioDmaStartLo{0};
std::atomic<uint16_t> g_audioDmaBlocksLength{0};
std::atomic<uint16_t> g_audioDmaControlLength{0};
std::atomic<uint16_t> g_audioDmaBlocksLeft{0};
std::atomic<uint64_t> g_audioDmaEpochMicros{0};
std::atomic<uint32_t> g_aiControl{0x00000042u}; // AIS 48 kHz, AID 32 kHz
std::atomic<uint32_t> g_aiVolume{0};
std::atomic<uint32_t> g_aiSampleCounterBase{0};
std::atomic<uint64_t> g_aiSampleCounterEpochMicros{0};
std::atomic<uint32_t> g_aiInterruptTiming{0};

inline void TraceGameCubePostAramMmioRead(uint32_t, uint8_t, uint32_t) {}

struct ExiChannelState {
    std::atomic<uint32_t> status{0};
    std::atomic<uint32_t> dmaAddress{0};
    std::atomic<uint32_t> dmaLength{0};
    std::atomic<uint32_t> control{0};
    std::atomic<uint32_t> immediateData{0};
};

std::array<ExiChannelState, kExiChannelCount> g_exiChannels{};
std::once_flag g_exiInitOnce;
std::mutex g_exiDeviceMutex;
uint32_t g_iplCommand = 0;
uint32_t g_iplCommandBytesReceived = 0;
uint32_t g_iplCursor = 0;
std::array<uint8_t, kIplRomSize> g_iplRom{};
std::array<uint8_t, kIplSramSize> g_iplSram{};
std::atomic<uint32_t> g_diStatus{0};
std::atomic<uint32_t> g_diCover{0}; // disc inserted, lid closed
std::array<std::atomic<uint32_t>, 3> g_diCommand{};
std::atomic<uint32_t> g_diDmaAddress{0};
std::atomic<uint32_t> g_diDmaLength{0};
std::atomic<uint32_t> g_diDmaControl{0};
std::atomic<uint32_t> g_diImmediate{0};
std::atomic<uint32_t> g_diConfig{1}; // boot-ROM descrambler disabled
std::atomic<bool> g_gameCubeDiInterruptDelivered{false};
std::atomic<bool> g_gameCubeSiInterruptDelivered{false};
std::atomic<bool> g_gameCubeViInterruptDelivered{false};
std::array<std::atomic<uint16_t>, kViRegisterCount> g_viRegisters{};
std::once_flag g_viInitOnce;
std::atomic<uint64_t> g_viEpochMicros{0};
std::atomic<uint64_t> g_viLastHalfLineAbsolute{0};
std::array<std::atomic<uint32_t>, 4> g_siOut{};
std::array<std::atomic<uint32_t>, 4> g_siInHi{};
std::array<std::atomic<uint32_t>, 4> g_siInLo{};
std::array<std::atomic<uint8_t>, 4> g_siPadMode{{3u, 3u, 3u, 3u}};
std::atomic<uint32_t> g_siPoll{492u << 16};
std::atomic<uint32_t> g_siComCsr{0};
std::atomic<uint32_t> g_siStatus{0};
std::atomic<uint32_t> g_siExiClock{0};
std::array<uint8_t, 128> g_siBuffer{};
std::mutex g_siMutex;

uint64_t HostSteadyMicros();

uint32_t SiRdstBit(uint32_t channel) {
    return 0x20000000u >> (channel * 8u);
}

uint32_t SiNoResponseBit(uint32_t channel) {
    return 0x08000000u >> (channel * 8u);
}

uint8_t PadAxisToRaw(int8_t value) {
    return static_cast<uint8_t>(std::clamp<int>(static_cast<int>(value) + 128, 0, 255));
}

uint32_t PackSiPadLow(const PADStatus& pad, uint8_t mode) {
    const uint32_t sx = PadAxisToRaw(pad.substickX);
    const uint32_t sy = PadAxisToRaw(pad.substickY);
    const uint32_t l = pad.triggerLeft;
    const uint32_t r = pad.triggerRight;
    const uint32_t a = pad.analogA;
    const uint32_t b = pad.analogB;
    switch (mode) {
    case 1:
        return (b >> 4) | ((a >> 4) << 4) | (r << 8) | (l << 16) |
               ((sy >> 4) << 24) | ((sx >> 4) << 28);
    case 2:
        return b | (a << 8) | ((r >> 4) << 16) | ((l >> 4) << 20) |
               ((sy >> 4) << 24) | ((sx >> 4) << 28);
    case 3:
        return r | (l << 8) | (sy << 16) | (sx << 24);
    case 4:
        return b | (a << 8) | (sy << 16) | (sx << 24);
    default:
        return (b >> 4) | ((a >> 4) << 4) | ((r >> 4) << 8) |
               ((l >> 4) << 12) | (sy << 16) | (sx << 24);
    }
}

std::array<PADStatus, 4> ReadHostPads() {
    std::array<PADStatus, 4> pads{};
    PADInit();
    PADRead(pads.data());
    return pads;
}

void UpdateSiPiInterrupt() {
    uint32_t csr = g_siComCsr.load(std::memory_order_relaxed);
    const uint32_t status = g_siStatus.load(std::memory_order_relaxed);
    const bool anyReady = (status & 0x20202020u) != 0;
    if (anyReady) {
        csr |= kSiComRdstInterrupt;
    } else {
        csr &= ~kSiComRdstInterrupt;
    }
    g_siComCsr.store(csr, std::memory_order_relaxed);
    const bool asserted = ((csr & kSiComRdstInterrupt) && (csr & kSiComRdstMask)) ||
                          ((csr & kSiComTcInterrupt) && (csr & kSiComTcMask));
    if (asserted) {
        g_piInterruptCause.fetch_or(0x00000008u, std::memory_order_relaxed);
    } else {
        g_piInterruptCause.fetch_and(~0x00000008u, std::memory_order_relaxed);
        g_gameCubeSiInterruptDelivered.store(false, std::memory_order_relaxed);
    }
}

void RefreshSiPadInputs() {
    const auto pads = ReadHostPads();
    static const bool tracePad = std::getenv("WIICOMPILED_INPUT_TRACE") != nullptr;
    static uint16_t lastPad0Buttons = 0xFFFFu;
    uint32_t status = g_siStatus.load(std::memory_order_relaxed);
    for (uint32_t channel = 0; channel < 4; ++channel) {
        const auto& pad = pads[channel];
        const uint32_t rdst = SiRdstBit(channel);
        const uint32_t noResponse = SiNoResponseBit(channel);
        if (pad.err == PAD_ERR_NONE) {
            const uint32_t hi = static_cast<uint32_t>(PadAxisToRaw(pad.stickY)) |
                                (static_cast<uint32_t>(PadAxisToRaw(pad.stickX)) << 8) |
                                (static_cast<uint32_t>(pad.button | 0x0080u) << 16);
            const uint32_t lo = PackSiPadLow(pad, g_siPadMode[channel].load(std::memory_order_relaxed));
            if (tracePad && channel == 0u && pad.button != lastPad0Buttons) {
                RT_LOG(RT_TAG_MEMORY) << "[gc-si-pad0] buttons=0x" << std::hex
                                      << static_cast<uint32_t>(pad.button)
                                      << " hi=0x" << hi << " lo=0x" << lo
                                      << std::dec << std::endl;
                lastPad0Buttons = pad.button;
            }
            g_siInHi[channel].store(hi, std::memory_order_relaxed);
            g_siInLo[channel].store(lo, std::memory_order_relaxed);
            status |= rdst;
            status &= ~noResponse;
        } else {
            g_siInHi[channel].store(0xC0000000u, std::memory_order_relaxed); // ERRSTAT | ERRLATCH
            g_siInLo[channel].store(0, std::memory_order_relaxed);
            status &= ~rdst;
            status |= noResponse;
        }
    }
    g_siStatus.store(status, std::memory_order_relaxed);
    UpdateSiPiInterrupt();
}

void ApplySiDirectCommands() {
    const uint32_t poll = g_siPoll.load(std::memory_order_relaxed);
    for (uint32_t channel = 0; channel < 4; ++channel) {
        const uint32_t command = g_siOut[channel].load(std::memory_order_relaxed);
        const uint8_t opcode = static_cast<uint8_t>((command >> 16) & 0xFFu);
        if (opcode != 0x40u) {
            continue;
        }
        const uint8_t motor = static_cast<uint8_t>(command & 0xFFu);
        PADControlMotor(channel, motor == 1u ? PAD_MOTOR_RUMBLE : PAD_MOTOR_STOP);
        const bool pollingEnabled = ((poll >> (7u - channel)) & 1u) != 0;
        if (!pollingEnabled) {
            g_siPadMode[channel].store(static_cast<uint8_t>((command >> 8) & 0x7u),
                                       std::memory_order_relaxed);
        }
    }
}

void WriteSiBufferResponseWord(uint32_t wordOffset, uint32_t value) {
    if (wordOffset + 4u > g_siBuffer.size()) {
        return;
    }
    g_siBuffer[wordOffset + 0u] = static_cast<uint8_t>(value >> 24);
    g_siBuffer[wordOffset + 1u] = static_cast<uint8_t>(value >> 16);
    g_siBuffer[wordOffset + 2u] = static_cast<uint8_t>(value >> 8);
    g_siBuffer[wordOffset + 3u] = static_cast<uint8_t>(value);
}

bool RunSiBufferTransfer(uint32_t channel) {
    const auto pads = ReadHostPads();
    if (channel >= pads.size() || pads[channel].err != PAD_ERR_NONE) {
        g_siStatus.fetch_or(SiNoResponseBit(channel), std::memory_order_relaxed);
        return false;
    }
    const uint8_t command = g_siBuffer[0];
    switch (command) {
    case 0x00u: // status
    case 0xFFu: // reset
        g_siBuffer[0] = 0x09u;
        g_siBuffer[1] = 0x00u;
        g_siBuffer[2] = 0x00u;
        return true;
    case 0x40u: { // direct pad data
        const auto& pad = pads[channel];
        const uint32_t hi = static_cast<uint32_t>(PadAxisToRaw(pad.stickY)) |
                            (static_cast<uint32_t>(PadAxisToRaw(pad.stickX)) << 8) |
                            (static_cast<uint32_t>(pad.button | 0x0080u) << 16);
        const uint32_t lo = PackSiPadLow(pad, g_siPadMode[channel].load(std::memory_order_relaxed));
        WriteSiBufferResponseWord(0, hi);
        WriteSiBufferResponseWord(4, lo);
        return true;
    }
    case 0x41u: // origin
    case 0x42u: // recalibrate
        g_siBuffer[0] = 0;
        g_siBuffer[1] = 0;
        g_siBuffer[2] = 128;
        g_siBuffer[3] = 128;
        g_siBuffer[4] = 128;
        g_siBuffer[5] = 128;
        g_siBuffer[6] = 0;
        g_siBuffer[7] = 0;
        g_siBuffer[8] = 0;
        g_siBuffer[9] = 0;
        return true;
    case 0x1Du: // set game ID, no reply required
        return true;
    default:
        RT_LOG(RT_TAG_MEMORY) << "SI: unsupported buffer command 0x" << std::hex
                              << static_cast<uint32_t>(command) << std::dec << std::endl;
        return false;
    }
}

bool TryReadModeledSi32(uint32_t addr, uint32_t& value) {
    if (addr >= kSiBase && addr < kSiBase + 0x30u && (addr & 3u) == 0) {
        const uint32_t channel = (addr - kSiBase) / 0x0Cu;
        const uint32_t offset = (addr - kSiBase) % 0x0Cu;
        if (channel >= 4u) {
            return false;
        }
        if (offset == 0u) {
            value = g_siOut[channel].load(std::memory_order_relaxed);
            return true;
        }
        RefreshSiPadInputs();
        if (offset == 4u || offset == 8u) {
            value = offset == 4u ? g_siInHi[channel].load(std::memory_order_relaxed)
                                 : g_siInLo[channel].load(std::memory_order_relaxed);
            g_siStatus.fetch_and(~SiRdstBit(channel), std::memory_order_relaxed);
            UpdateSiPiInterrupt();
            return true;
        }
    }
    switch (addr) {
    case kSiPoll: value = g_siPoll.load(std::memory_order_relaxed); return true;
    case kSiComCsr: value = g_siComCsr.load(std::memory_order_relaxed); return true;
    case kSiStatus:
        RefreshSiPadInputs();
        value = g_siStatus.load(std::memory_order_relaxed);
        return true;
    case kSiExiClock: value = g_siExiClock.load(std::memory_order_relaxed); return true;
    default:
        break;
    }
    if (addr >= kSiBufferBase && addr + 3u <= kSiBufferEnd && (addr & 3u) == 0) {
        std::lock_guard<std::mutex> lock(g_siMutex);
        const uint32_t offset = addr - kSiBufferBase;
        value = (static_cast<uint32_t>(g_siBuffer[offset + 0u]) << 24) |
                (static_cast<uint32_t>(g_siBuffer[offset + 1u]) << 16) |
                (static_cast<uint32_t>(g_siBuffer[offset + 2u]) << 8) |
                static_cast<uint32_t>(g_siBuffer[offset + 3u]);
        return true;
    }
    return false;
}

bool TryWriteModeledSi32(uint32_t addr, uint32_t value) {
    if (addr >= kSiBase && addr < kSiBase + 0x30u && (addr & 3u) == 0) {
        const uint32_t channel = (addr - kSiBase) / 0x0Cu;
        const uint32_t offset = (addr - kSiBase) % 0x0Cu;
        if (channel < 4u) {
            if (offset == 0u) g_siOut[channel].store(value, std::memory_order_relaxed);
            else if (offset == 4u) g_siInHi[channel].store(value, std::memory_order_relaxed);
            else if (offset == 8u) g_siInLo[channel].store(value, std::memory_order_relaxed);
            else return false;
            return true;
        }
    }
    switch (addr) {
    case kSiPoll:
        g_siPoll.store(value, std::memory_order_relaxed);
        return true;
    case kSiComCsr: {
        uint32_t old = g_siComCsr.load(std::memory_order_relaxed);
        uint32_t next = value & kSiComPersistentMask;
        next |= old & kSiComRdstInterrupt & ~value;
        next |= old & kSiComTcInterrupt & ~value;
        next &= ~kSiComError;
        g_siComCsr.store(next, std::memory_order_relaxed);
        if ((value & 1u) != 0) {
            const uint32_t channel = (value >> 1) & 0x3u;
            const bool success = RunSiBufferTransfer(channel);
            uint32_t done = g_siComCsr.load(std::memory_order_relaxed) & ~1u;
            done |= kSiComTcInterrupt;
            if (!success) done |= kSiComError;
            g_siComCsr.store(done, std::memory_order_relaxed);
        }
        UpdateSiPiInterrupt();
        return true;
    }
    case kSiStatus: {
        uint32_t old = g_siStatus.load(std::memory_order_relaxed);
        old &= ~(value & kSiStatusWriteClearMask);
        if ((value & 0x80000000u) != 0) {
            ApplySiDirectCommands();
            old &= ~0x90909090u; // WRST bits plus WR complete immediately
        }
        g_siStatus.store(old, std::memory_order_relaxed);
        UpdateSiPiInterrupt();
        return true;
    }
    case kSiExiClock:
        g_siExiClock.store(value, std::memory_order_relaxed);
        return true;
    default:
        break;
    }
    if (addr >= kSiBufferBase && addr + 3u <= kSiBufferEnd && (addr & 3u) == 0) {
        std::lock_guard<std::mutex> lock(g_siMutex);
        const uint32_t offset = addr - kSiBufferBase;
        g_siBuffer[offset + 0u] = static_cast<uint8_t>(value >> 24);
        g_siBuffer[offset + 1u] = static_cast<uint8_t>(value >> 16);
        g_siBuffer[offset + 2u] = static_cast<uint8_t>(value >> 8);
        g_siBuffer[offset + 3u] = static_cast<uint8_t>(value);
        return true;
    }
    return false;
}

void InitializeViState() {
    std::call_once(g_viInitOnce, [] {
        for (auto& reg : g_viRegisters) {
            reg.store(0, std::memory_order_relaxed);
        }
        // GameCube NTSC preset. Only the fields the SDK can observe before it
        // programs its own mode need reset values here.
        g_viRegisters[(0x00u) / 2u].store(0x0006u, std::memory_order_relaxed); // EQU=6
        g_viRegisters[(0x02u) / 2u].store(0x0001u, std::memory_order_relaxed); // timing enabled, NTSC
        // Flipper's boot VI has both standard retrace comparators armed before
        // the title calls VIConfigure: pre-retrace VCT=263/HCT=430 and
        // post-retrace VCT=1/HCT=1.  VIWaitForRetrace may legally run while
        // these boot values are still active.
        g_viRegisters[(0x30u) / 2u].store(
            static_cast<uint16_t>(kViInterruptMaskBit | 263u), std::memory_order_relaxed);
        g_viRegisters[(0x32u) / 2u].store(430u, std::memory_order_relaxed);
        g_viRegisters[(0x34u) / 2u].store(
            static_cast<uint16_t>(kViInterruptMaskBit | 1u), std::memory_order_relaxed);
        g_viRegisters[(0x36u) / 2u].store(1u, std::memory_order_relaxed);
        g_viRegisters[(0x6Cu) / 2u].store(0x0001u, std::memory_order_relaxed); // 54 MHz clock
        g_viRegisters[(0x6Eu) / 2u].store(0x0000u, std::memory_order_relaxed); // no component cable
        g_viEpochMicros.store(HostSteadyMicros(), std::memory_order_relaxed);
        g_viLastHalfLineAbsolute.store(0u, std::memory_order_relaxed);
    });
}

bool IsModeledVi16(uint32_t addr) {
    return addr >= kViBase && addr <= kViLast && (addr & 1u) == 0;
}

struct RawViTiming {
    uint64_t frameMicrosNumerator;
    uint64_t frameMicrosDenominator;
    uint32_t linesPerFrame;
    uint64_t halfLinesPerFrame;
    uint16_t halfLineWidth;
};

constexpr RawViTiming RawViTimingForDisplayControl(uint16_t displayControl) {
    const uint16_t format = static_cast<uint16_t>(
        (displayControl & kViDisplayControlFormatMask) >> 8u);
    if (format == 1u) {
        // PAL50: 625 lines / 1250 half-lines, 25 full frames per second.
        return {1'000'000ull, 25ull, 625u, 1250ull, 432u};
    }

    // NTSC and EURGB60 use the 525-line timing family. Keep the existing
    // NTSC 1001/30000 s full-frame cadence for every non-PAL50 DCR format.
    return {1'000'000ull * 1001ull, 30'000ull, 525u, 1050ull, 429u};
}

constexpr bool IsPal50RawViTimingFamily(uint16_t displayControl) {
    return ((displayControl & kViDisplayControlFormatMask) >> 8u) == 1u;
}

// Keep the raw beam/comparator model on the same cadence as VI_HLE when a
// PAL title is promoted to EURGB60. The guest may still write PAL50 to the
// register, but raw reads and timing calculations observe the effective mode.
uint16_t EffectiveRawViDisplayControl(uint16_t displayControl) {
    if (!VI_HLE_IsPal60Forced() || !IsPal50RawViTimingFamily(displayControl)) {
        return displayControl;
    }
    return static_cast<uint16_t>((displayControl & ~kViDisplayControlFormatMask) | 0x0500u);
}

void CurrentViBeam(uint16_t& vertical, uint16_t& horizontal) {
    InitializeViState();
    // The DCR FMT field selects the raw VI timing family. G2XP8P programs
    // FMT=1 for PAL50; EURGB60 remains in the 525-line timing family.
    const uint16_t displayControl = EffectiveRawViDisplayControl(
        g_viRegisters[(0x02u) / 2u].load(std::memory_order_relaxed));
    const RawViTiming timing = RawViTimingForDisplayControl(displayControl);
    const uint64_t frameMicros = timing.frameMicrosNumerator / timing.frameMicrosDenominator;
    const uint64_t now = HostSteadyMicros();
    const uint64_t epoch = g_viEpochMicros.load(std::memory_order_relaxed);
    const uint64_t elapsed = now >= epoch ? now - epoch : 0;
    const uint64_t withinFrame = frameMicros != 0 ? elapsed % frameMicros : 0;
    const uint64_t scaledLine = withinFrame * timing.linesPerFrame;
    const uint32_t line = frameMicros != 0 ? static_cast<uint32_t>(scaledLine / frameMicros) : 0u;
    const uint64_t lineStart = frameMicros != 0 ? (static_cast<uint64_t>(line) * frameMicros) / timing.linesPerFrame : 0u;
    const uint64_t lineEnd = frameMicros != 0 ? (static_cast<uint64_t>(line + 1u) * frameMicros) / timing.linesPerFrame : 1u;
    const uint64_t lineSpan = std::max<uint64_t>(1u, lineEnd - lineStart);
    const uint64_t withinLine = withinFrame >= lineStart ? withinFrame - lineStart : 0;
    vertical = static_cast<uint16_t>(1u + line);
    horizontal = static_cast<uint16_t>(1u + std::min<uint64_t>(timing.halfLineWidth * 2u - 1u,
        withinLine * (timing.halfLineWidth * 2u) / lineSpan));
}

void UpdateViPiInterruptFromRegisters() {
    bool asserted = false;
    for (const uint32_t offset : kViInterruptHighOffsets) {
        const uint16_t high = g_viRegisters[offset / 2u].load(std::memory_order_relaxed);
        asserted |= (high & (kViInterruptStatusBit | kViInterruptMaskBit)) ==
                    (kViInterruptStatusBit | kViInterruptMaskBit);
    }
    if (asserted) {
        g_piInterruptCause.fetch_or(kViPiCauseBit, std::memory_order_relaxed);
    } else {
        g_piInterruptCause.fetch_and(~kViPiCauseBit, std::memory_order_relaxed);
        g_gameCubeViInterruptDelivered.store(false, std::memory_order_relaxed);
    }
}

void UpdateViDisplayInterruptTiming() {
    InitializeViState();

    // Keep comparator timing in exactly the same DCR-selected time domain as
    // CurrentViBeam; the guest can replace the comparator positions later.
    const uint16_t displayControl = EffectiveRawViDisplayControl(
        g_viRegisters[(0x02u) / 2u].load(std::memory_order_relaxed));
    const RawViTiming timing = RawViTimingForDisplayControl(displayControl);
    const uint64_t frameMicros = timing.frameMicrosNumerator / timing.frameMicrosDenominator;

    const uint64_t now = HostSteadyMicros();
    const uint64_t epoch = g_viEpochMicros.load(std::memory_order_relaxed);
    const uint64_t elapsed = now >= epoch ? now - epoch : 0u;
    const uint64_t currentHalfLine = frameMicros != 0u
        ? (elapsed * timing.halfLinesPerFrame) / frameMicros
        : 0u;
    const uint64_t previousHalfLine =
        g_viLastHalfLineAbsolute.exchange(currentHalfLine, std::memory_order_relaxed);
    if (currentHalfLine <= previousHalfLine) {
        return;
    }

    bool raised = false;
    for (const uint32_t offset : kViInterruptHighOffsets) {
        uint16_t high = g_viRegisters[offset / 2u].load(std::memory_order_relaxed);
        if ((high & kViInterruptMaskBit) == 0u) {
            continue;
        }
        const uint16_t low =
            g_viRegisters[(offset + 2u) / 2u].load(std::memory_order_relaxed);
        const uint32_t vertical = high & 0x07FFu;
        const uint32_t horizontal = low & 0x07FFu;
        if (vertical == 0u) {
            continue;
        }

        const uint64_t targetInFrame =
            2ull * static_cast<uint64_t>(vertical - 1u) +
            (horizontal > timing.halfLineWidth ? 1ull : 0ull);
        uint64_t nextTarget = targetInFrame;
        if (nextTarget <= previousHalfLine) {
            nextTarget +=
                ((previousHalfLine - nextTarget) / timing.halfLinesPerFrame + 1ull) *
                timing.halfLinesPerFrame;
        }
        if (nextTarget <= currentHalfLine) {
            high = static_cast<uint16_t>(high | kViInterruptStatusBit);
            g_viRegisters[offset / 2u].store(high, std::memory_order_relaxed);
            raised = true;
        }
    }

    if (raised) {
        g_gameCubeViInterruptDelivered.store(false, std::memory_order_relaxed);
        UpdateViPiInterruptFromRegisters();
    }
}

bool TryReadModeledVi16(uint32_t addr, uint16_t& value) {
    if (!IsModeledVi16(addr)) {
        return false;
    }
    InitializeViState();
    if (addr == kViVerticalBeamPosition || addr == kViHorizontalBeamPosition) {
        uint16_t vertical = 0;
        uint16_t horizontal = 0;
        CurrentViBeam(vertical, horizontal);
        value = addr == kViVerticalBeamPosition ? vertical : horizontal;
        return true;
    }
    value = g_viRegisters[(addr - kViBase) / 2u].load(std::memory_order_relaxed);
    if (addr == kViDisplayControl) {
        value = EffectiveRawViDisplayControl(value);
    }
    return true;
}

bool TryWriteModeledVi16(uint32_t addr, uint16_t value) {
    if (!IsModeledVi16(addr)) {
        return false;
    }
    InitializeViState();
    if (addr == kViVerticalBeamPosition || addr == kViHorizontalBeamPosition) {
        return true; // hardware treats these writes as undocumented/no-op for our purposes
    }
    if (addr == kViClock) {
        g_viRegisters[(addr - kViBase) / 2u].store(value & 1u, std::memory_order_relaxed);
        return true;
    }
    if (addr == kViDtvStatus) {
        g_viRegisters[(addr - kViBase) / 2u].store(value, std::memory_order_relaxed);
        return true;
    }
    // VI control bit 1 is reset/self-clearing. Preserve all other programmed
    // fields because games use the SDK to fully replace the preset timings.
    if (addr == kViDisplayControl) {
        const uint16_t oldValue =
            g_viRegisters[(addr - kViBase) / 2u].load(std::memory_order_relaxed);
        value = static_cast<uint16_t>(value & ~0x0002u);
        g_viRegisters[(addr - kViBase) / 2u].store(value, std::memory_order_relaxed);
        const uint16_t oldEffectiveValue = EffectiveRawViDisplayControl(oldValue);
        const uint16_t newEffectiveValue = EffectiveRawViDisplayControl(value);
        if (IsPal50RawViTimingFamily(oldEffectiveValue) != IsPal50RawViTimingFamily(newEffectiveValue)) {
            // Do not invent a fresh beam phase from host wall time here. The HLE
            // retrace domain supplies the authoritative phase at its next timing
            // adoption boundary. Until then, move only the comparator traversal
            // cursor into the new family's units so a format switch itself cannot
            // look like a giant comparator crossing. Comparator/status/PI state
            // remains entirely guest-owned.
            const RawViTiming timing = RawViTimingForDisplayControl(newEffectiveValue);
            const uint64_t frameMicros =
                timing.frameMicrosNumerator / timing.frameMicrosDenominator;
            const uint64_t now = HostSteadyMicros();
            const uint64_t epoch = g_viEpochMicros.load(std::memory_order_relaxed);
            const uint64_t elapsed = now >= epoch ? now - epoch : 0u;
            const uint64_t currentHalfLine = frameMicros != 0u
                ? (elapsed * timing.halfLinesPerFrame) / frameMicros
                : 0u;
            g_viLastHalfLineAbsolute.store(currentHalfLine, std::memory_order_relaxed);
        }
        return true;
    }
    g_viRegisters[(addr - kViBase) / 2u].store(value, std::memory_order_relaxed);
    if (addr == kViPreRetraceHi || addr == kViPostRetraceHi ||
        addr == kViBase + 0x38u || addr == kViBase + 0x3Cu) {
        UpdateViPiInterruptFromRegisters();
    }
    return true;
}

bool TryReadModeledVi32(uint32_t addr, uint32_t& value) {
    if ((addr & 3u) != 0 || addr < kViBase || addr + 2u > kViLast) {
        return false;
    }
    uint16_t hi = 0;
    uint16_t lo = 0;
    if (!TryReadModeledVi16(addr, hi) || !TryReadModeledVi16(addr + 2u, lo)) {
        return false;
    }
    value = (static_cast<uint32_t>(hi) << 16) | lo;
    return true;
}

bool TryWriteModeledVi32(uint32_t addr, uint32_t value) {
    if ((addr & 3u) != 0 || addr < kViBase || addr + 2u > kViLast) {
        return false;
    }
    return TryWriteModeledVi16(addr, static_cast<uint16_t>(value >> 16)) &&
           TryWriteModeledVi16(addr + 2u, static_cast<uint16_t>(value));
}

void UpdateDiPiInterrupt() {
    const uint32_t status = g_diStatus.load(std::memory_order_relaxed);
    const uint32_t cover = g_diCover.load(std::memory_order_relaxed);
    const bool asserted = ((status & 0x04u) && (status & 0x02u)) ||
                          ((status & 0x10u) && (status & 0x08u)) ||
                          ((status & 0x40u) && (status & 0x20u)) ||
                          ((cover & 0x04u) && (cover & 0x02u));
    if (asserted) {
        g_piInterruptCause.fetch_or(0x00000004u, std::memory_order_relaxed);
    } else {
        g_piInterruptCause.fetch_and(~0x00000004u, std::memory_order_relaxed);
        g_gameCubeDiInterruptDelivered.store(false, std::memory_order_relaxed);
    }
}

bool ExecuteGameCubeDiCommand() {
    const uint32_t command0 = g_diCommand[0].load(std::memory_order_relaxed);
    const uint32_t command1 = g_diCommand[1].load(std::memory_order_relaxed);
    const uint32_t command2 = g_diCommand[2].load(std::memory_order_relaxed);
    const uint8_t opcode = static_cast<uint8_t>(command0 >> 24);
    const uint32_t dmaAddress = g_diDmaAddress.load(std::memory_order_relaxed);
    const uint32_t dmaLength = g_diDmaLength.load(std::memory_order_relaxed);
    bool success = true;
    uint32_t transferred = 0;

    switch (opcode) {
    case 0x12u: // Inquiry
        if (dmaLength >= 12u && Memory::Contains(dmaAddress, dmaLength)) {
            Memory::Write32(dmaAddress + 0u, 0x00000002u);
            Memory::Write32(dmaAddress + 4u, 0x20060526u);
            Memory::Write32(dmaAddress + 8u, 0x41000000u);
            // The drive only returns meaningful identification fields in the
            // first words, but the DI transaction consumes the entire DMA
            // length programmed by DVDLowInquiry (32 bytes in Nintendo's SDK).
            // Advancing only the 12 bytes we explicitly populate leaves
            // DILENGTH non-zero and makes the SDK retry Inquiry indefinitely.
            transferred = dmaLength;
        } else {
            success = false;
        }
        break;
    case 0xA8u: { // Read / Read Disc ID
        const uint8_t subcommand = static_cast<uint8_t>(command0);
        uint32_t readLength = 0;
        uint32_t wordOffset = 0;
        if (subcommand == 0x40u) {
            readLength = std::min<uint32_t>(0x20u, dmaLength);
            wordOffset = 0;
        } else if (subcommand == 0x00u) {
            readLength = std::min(command2, dmaLength);
            wordOffset = command1;
        } else {
            success = false;
            break;
        }
        success = readLength != 0u && DVD_HLE_ReadWordsToGuest(dmaAddress, readLength, wordOffset);
        if (success) {
            transferred = readLength;
        }
        break;
    }
    case 0xABu: // Seek
        break;
    case 0xE0u: // Request Error
        g_diImmediate.store(0, std::memory_order_relaxed);
        break;
    case 0xE1u: // Audio stream configuration; DTK data path is added when exercised.
        break;
    case 0xE2u: // Request audio status
        g_diImmediate.store(0, std::memory_order_relaxed);
        break;
    case 0xE3u: // Stop motor
        break;
    case 0xE4u: // Audio buffer config
        break;
    default:
        RT_LOG(RT_TAG_MEMORY) << "DI: unsupported command 0x" << std::hex
                              << static_cast<uint32_t>(opcode) << " cmd0=0x" << command0
                              << std::dec << std::endl;
        success = false;
        break;
    }

    if (transferred != 0u) {
        g_diDmaAddress.store((dmaAddress + transferred) & 0x03FFFFE0u, std::memory_order_relaxed);
        g_diDmaLength.store(dmaLength - std::min(dmaLength, transferred), std::memory_order_relaxed);
    }
    g_diDmaControl.fetch_and(~1u, std::memory_order_relaxed);
    if (success) {
        g_diStatus.fetch_or(kDiStatusTransferComplete, std::memory_order_relaxed);
    } else {
        g_diStatus.fetch_or(kDiStatusErrorInterrupt, std::memory_order_relaxed);
    }
    UpdateDiPiInterrupt();
    return success;
}

bool TryReadModeledDi32(uint32_t addr, uint32_t& value) {
    switch (addr) {
    case kDiStatus: value = g_diStatus.load(std::memory_order_relaxed); return true;
    case kDiCover: value = g_diCover.load(std::memory_order_relaxed); return true;
    case kDiCommand0: value = g_diCommand[0].load(std::memory_order_relaxed); return true;
    case kDiCommand1: value = g_diCommand[1].load(std::memory_order_relaxed); return true;
    case kDiCommand2: value = g_diCommand[2].load(std::memory_order_relaxed); return true;
    case kDiDmaAddress: value = g_diDmaAddress.load(std::memory_order_relaxed); return true;
    case kDiDmaLength: value = g_diDmaLength.load(std::memory_order_relaxed); return true;
    case kDiDmaControl: value = g_diDmaControl.load(std::memory_order_relaxed); return true;
    case kDiImmediate: value = g_diImmediate.load(std::memory_order_relaxed); return true;
    case kDiConfig: value = g_diConfig.load(std::memory_order_relaxed); return true;
    default: return false;
    }
}

bool TryWriteModeledDi32(uint32_t addr, uint32_t value) {
    switch (addr) {
    case kDiStatus: {
        const uint32_t old = g_diStatus.load(std::memory_order_relaxed);
        uint32_t next = value & kDiStatusWritableStateMask;
        next |= old & kDiStatusW1cMask & ~value;
        g_diStatus.store(next, std::memory_order_relaxed);
        UpdateDiPiInterrupt();
        return true;
    }
    case kDiCover: {
        const uint32_t old = g_diCover.load(std::memory_order_relaxed);
        uint32_t next = old & 0x1u; // cover state is read-only
        next |= value & kDiCoverMaskBit;
        next |= old & kDiCoverW1cBit & ~value;
        g_diCover.store(next, std::memory_order_relaxed);
        UpdateDiPiInterrupt();
        return true;
    }
    case kDiCommand0: g_diCommand[0].store(value, std::memory_order_relaxed); return true;
    case kDiCommand1: g_diCommand[1].store(value, std::memory_order_relaxed); return true;
    case kDiCommand2: g_diCommand[2].store(value, std::memory_order_relaxed); return true;
    case kDiDmaAddress:
        g_diDmaAddress.store(value & 0x03FFFFE0u, std::memory_order_relaxed);
        return true;
    case kDiDmaLength:
        g_diDmaLength.store(value & ~0x1Fu, std::memory_order_relaxed);
        return true;
    case kDiDmaControl:
        g_diDmaControl.store(value & 0x7u, std::memory_order_relaxed);
        if ((value & 1u) != 0) {
            ExecuteGameCubeDiCommand();
        }
        return true;
    case kDiImmediate:
        g_diImmediate.store(value, std::memory_order_relaxed);
        return true;
    case kDiConfig:
        return true; // read-only on hardware; ignore translated store defensively
    default:
        return false;
    }
}

void UpdateIplRtc() {
    constexpr int64_t kGameCubeEpochUnixSeconds = 946684800ll;
    const int64_t unixSeconds = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const uint32_t rtc = static_cast<uint32_t>(std::max<int64_t>(0, unixSeconds - kGameCubeEpochUnixSeconds));
    g_iplSram[0] = static_cast<uint8_t>(rtc >> 24);
    g_iplSram[1] = static_cast<uint8_t>(rtc >> 16);
    g_iplSram[2] = static_cast<uint8_t>(rtc >> 8);
    g_iplSram[3] = static_cast<uint8_t>(rtc);
}

GameCubeMemoryCard g_slotACard;
std::once_flag g_cardInitOnce;
std::array<uint32_t, 3> g_exiDelivered{};

constexpr uint32_t kIplJapaneseFontOffset = 0x001AFF00u;
constexpr uint32_t kIplWesternFontOffset = 0x001FCF00u;

std::optional<std::filesystem::path> FindIplFontAsset(const char* filename) {
    if (const auto executableDirectory = RuntimeConfigFile::ExecutableDirectory()) {
        const auto adjacent = *executableDirectory / filename;
        if (std::filesystem::is_regular_file(adjacent)) {
            return adjacent;
        }
    }

    for (auto base = std::filesystem::current_path(); !base.empty();) {
        const auto sourceTreeAsset = base / "runtime" / "assets" / "ipl" / filename;
        if (std::filesystem::is_regular_file(sourceTreeAsset)) {
            return sourceTreeAsset;
        }
        const auto parent = base.parent_path();
        if (parent == base) {
            break;
        }
        base = parent;
    }
    return std::nullopt;
}

bool LoadIplFontAsset(const char* filename, uint32_t offset) {
    const auto path = FindIplFontAsset(filename);
    if (!path || offset >= g_iplRom.size()) {
        RT_LOG(RT_TAG_MEMORY) << "IPL font asset missing: " << filename << std::endl;
        return false;
    }

    std::ifstream stream(*path, std::ios::binary | std::ios::ate);
    if (!stream) {
        RT_LOG(RT_TAG_MEMORY) << "IPL font asset could not be opened: " << path->string() << std::endl;
        return false;
    }
    const std::streamsize size = stream.tellg();
    if (size <= 0 || static_cast<uint64_t>(size) > g_iplRom.size() - offset) {
        RT_LOG(RT_TAG_MEMORY) << "IPL font asset has invalid size: " << path->string()
                              << " size=" << size << std::endl;
        return false;
    }
    stream.seekg(0, std::ios::beg);
    stream.read(reinterpret_cast<char*>(g_iplRom.data() + offset), size);
    if (!stream) {
        RT_LOG(RT_TAG_MEMORY) << "IPL font asset read failed: " << path->string() << std::endl;
        return false;
    }
    RT_LOG(RT_TAG_MEMORY) << "Loaded free IPL font asset " << filename
                          << " (" << size << " bytes) @ 0x" << std::hex << offset
                          << std::dec << std::endl;
    return true;
}

void UpdateExiInterrupts() {
    if (g_slotACard.InterruptPending())
        g_exiChannels[0].status.fetch_or(2u, std::memory_order_relaxed);
    bool pending = false;
    for (uint32_t ch = 0; ch < 3; ++ch) {
        const uint32_t status = g_exiChannels[ch].status.load(std::memory_order_relaxed);
        const uint32_t enabled = ((status & 1u) ? status & 2u : 0u) |
                                 ((status & 4u) ? status & 8u : 0u) |
                                 ((ch < 2 && (status & 0x400u)) ? status & 0x800u : 0u);
        g_exiDelivered[ch] &= enabled;
        pending |= enabled != 0;
    }
    if (pending) g_piInterruptCause.fetch_or(0x10u, std::memory_order_relaxed);
    else g_piInterruptCause.fetch_and(~0x10u, std::memory_order_relaxed);
}

void InitializeExiState() {
    std::call_once(g_exiInitOnce, [] {
        // Dolphin/retail reset state: channels 0 and 1 start with EXTINT asserted;
        // channel 1 also starts with device 0 selected.
        g_exiChannels[0].status.store(0x00000800u, std::memory_order_relaxed);
        g_exiChannels[1].status.store(0x00000880u, std::memory_order_relaxed);
        g_exiChannels[2].status.store(0, std::memory_order_relaxed);

        // Wii hardware does not contain the GameCube IPL program, but it still
        // exposes the IPL font data through EXI. Dolphin ships clean-room/free
        // replacement font blobs for exactly this use case; package those blobs
        // with WiiCompiled and map them at the retail IPL font offsets.
        g_iplRom.fill(0);
        static constexpr char kNtscIplHeader[] =
            "(C) 1999-2001 Nintendo.  All rights reserved."
            "(C) 1999 ArtX Inc.  All rights reserved.";
        std::memcpy(g_iplRom.data(), kNtscIplHeader, sizeof(kNtscIplHeader) - 1u);
        LoadIplFontAsset("font_japanese.bin", kIplJapaneseFontOffset);
        LoadIplFontAsset("font_western.bin", kIplWesternFontOffset);

        // GameCube SRAM template used by Dolphin: valid settings checksum,
        // English, OOBE complete, stereo, and the canonical slot flash IDs.
        g_iplSram.fill(0);
        g_iplSram[4] = 0x00;
        g_iplSram[5] = 0x2C;
        g_iplSram[6] = 0xFF;
        g_iplSram[7] = 0xD0;
        g_iplSram[22] = 0;    // English
        g_iplSram[23] = 0x2C; // stereo + OOBE complete + retail default bit 5
        constexpr char kSlotA[] = "DOLPHINSLOTA";
        constexpr char kSlotB[] = "DOLPHINSLOTB";
        std::memcpy(g_iplSram.data() + 24, kSlotA, 12);
        std::memcpy(g_iplSram.data() + 36, kSlotB, 12);
        g_iplSram[62] = 0x6E;
        g_iplSram[63] = 0x6D;
        UpdateIplRtc();
    });
}

void ResetIplTransfer() {
    std::lock_guard<std::mutex> lock(g_exiDeviceMutex);
    g_iplCommand = 0;
    g_iplCommandBytesReceived = 0;
    g_iplCursor = 0;
}

void TransferIplByte(uint8_t& data) {
    std::lock_guard<std::mutex> lock(g_exiDeviceMutex);
    if (g_iplCommandBytesReceived < 4u) {
        g_iplCommand = (g_iplCommand << 8) | data;
        data = 0xFFu;
        ++g_iplCommandBytesReceived;
        if (g_iplCommandBytesReceived == 4u) {
            UpdateIplRtc();
        }
        return;
    }

    const bool isWrite = (g_iplCommand & 0x80000000u) != 0;
    const uint32_t address = (g_iplCommand >> 6) & 0x01FFFFFFu;
    if (address < kIplRomSize) {
        if (!isWrite) {
            const uint32_t offset = (address + g_iplCursor++) % kIplRomSize;
            data = g_iplRom[offset];
        }
        return;
    }
    if (address >= kIplSramBase && address < kIplSramBase + kIplSramSize) {
        const uint32_t offset = (address - kIplSramBase + g_iplCursor++) % kIplSramSize;
        if (isWrite) {
            g_iplSram[offset] = data;
        } else {
            data = g_iplSram[offset];
        }
        return;
    }

    // The IPL UART is not connected to host serial hardware. Reads report an
    // empty FIFO, matching Dolphin's immediate HLE behavior.
    if (!isWrite) {
        data = 0;
    }
}

bool DecodeExiAddress(uint32_t addr, uint32_t& channel, uint32_t& regOffset) {
    if (addr < kExiBase || addr > kExiLastRegister) {
        return false;
    }
    const uint32_t rel = addr - kExiBase;
    channel = rel / kExiChannelStride;
    regOffset = rel % kExiChannelStride;
    return channel < kExiChannelCount && regOffset <= 0x10u && (regOffset & 3u) == 0;
}

uint32_t ExiChipSelect(uint32_t status) {
    return (status & kExiStatusChipSelectMask) >> 7;
}

bool TryReadModeledExi32(uint32_t addr, uint32_t& value) {
    InitializeExiState();
    uint32_t channel = 0;
    uint32_t offset = 0;
    if (!DecodeExiAddress(addr, channel, offset)) {
        return false;
    }
    auto& state = g_exiChannels[channel];
    switch (offset) {
    case 0x00u: {
        uint32_t status = state.status.load(std::memory_order_relaxed);
        // EXT reports physical presence; channel 2 has no external slot.
        status &= ~kExiStatusExtBit;
        if (channel == 0 && g_slotACard.Present()) status |= kExiStatusExtBit;
        state.status.store(status, std::memory_order_relaxed);
        value = status;
        return true;
    }
    case 0x04u: value = state.dmaAddress.load(std::memory_order_relaxed); return true;
    case 0x08u: value = state.dmaLength.load(std::memory_order_relaxed); return true;
    case 0x0Cu: value = state.control.load(std::memory_order_relaxed); return true;
    case 0x10u: value = state.immediateData.load(std::memory_order_relaxed); return true;
    default: return false;
    }
}

void ExiTransferImmediate(uint32_t channel, uint32_t control) {
    auto& state = g_exiChannels[channel];
    const uint32_t size = ((control >> 4) & 0x3u) + 1u;
    const uint32_t rw = (control >> 2) & 0x3u;
    const uint32_t chipSelect = ExiChipSelect(state.status.load(std::memory_order_relaxed));
    uint32_t data = state.immediateData.load(std::memory_order_relaxed);
    uint32_t result = 0;

    for (uint32_t i = 0; i < size; ++i) {
        uint8_t byte = (rw == 0u) ? 0u : static_cast<uint8_t>(data >> (24u - i * 8u));
        if (channel == 0u && chipSelect == kExiIplChipSelect) {
            TransferIplByte(byte);
        } else if (channel == 0 && chipSelect == 1 && g_slotACard.Present()) {
            byte = g_slotACard.Transfer(byte);
        } else if (rw == 0u) {
            // A transfer to a configured "None" device completes and returns zero.
            byte = 0u;
        }
        if (rw == 0u || rw == 2u) {
            result |= static_cast<uint32_t>(byte) << (24u - i * 8u);
        }
    }
    if (rw == 0u || rw == 2u) {
        state.immediateData.store(result, std::memory_order_relaxed);
    }
}

void ExiTransferDma(uint32_t channel, uint32_t control) {
    auto& state = g_exiChannels[channel];
    const uint32_t rw = (control >> 2) & 0x3u;
    const uint32_t chipSelect = ExiChipSelect(state.status.load(std::memory_order_relaxed));
    uint32_t address = state.dmaAddress.load(std::memory_order_relaxed);
    const uint32_t length = state.dmaLength.load(std::memory_order_relaxed);
    if (channel == 0 && chipSelect == 1 && g_slotACard.Present()) {
        if (length > 16u * 1024u * 1024u) throw std::runtime_error("Invalid EXI DMA length");
        std::vector<uint8_t> bytes(length);
        if (rw == 0) {
            g_slotACard.DmaRead(bytes.data(), bytes.size());
            for (uint32_t i = 0; i < length; ++i) Memory::Write8(address + i, bytes[i]);
        } else if (rw == 1) {
            for (uint32_t i = 0; i < length; ++i) bytes[i] = Memory::Read8(address + i);
            g_slotACard.DmaWrite(bytes.data(), bytes.size());
        }
        return;
    }
    for (uint32_t i = 0; i < length; ++i) {
        uint8_t byte = rw == 0u ? 0u : Memory::Read8(address + i);
        if (channel == 0u && chipSelect == kExiIplChipSelect) {
            TransferIplByte(byte);
        } else if (channel == 0 && chipSelect == 1 && g_slotACard.Present()) {
            byte = g_slotACard.Transfer(byte);
        } else if (rw == 0u) {
            byte = 0u;
        }
        if (rw == 0u) {
            Memory::Write8(address + i, byte);
        }
    }
}

bool TryWriteModeledExi32(uint32_t addr, uint32_t value) {
    InitializeExiState();
    uint32_t channel = 0;
    uint32_t offset = 0;
    if (!DecodeExiAddress(addr, channel, offset)) {
        return false;
    }
    auto& state = g_exiChannels[channel];
    switch (offset) {
    case 0x00u: {
        const uint32_t old = state.status.load(std::memory_order_relaxed);
        const uint32_t oldChipSelect = ExiChipSelect(old);
        uint32_t persistentMask = kExiStatusPersistentMask;
        if (channel == 0u) {
            persistentMask |= kExiStatusRomDisableBit;
        }
        uint32_t next = value & persistentMask;
        next |= old & kExiStatusExtBit;
        next |= old & kExiStatusFlagMask & ~value;
        state.status.store(next, std::memory_order_relaxed);
        const uint32_t newChipSelect = ExiChipSelect(next);
        g_exiDelivered[channel] &= ~(value & kExiStatusFlagMask);
        if (channel == 0 && g_slotACard.Present() && newChipSelect != oldChipSelect) {
            if (oldChipSelect == 1) g_slotACard.Select(false);
            if (newChipSelect == 1) g_slotACard.Select(true);
        }
        if (channel == 0u && newChipSelect == kExiIplChipSelect && newChipSelect != oldChipSelect) {
            ResetIplTransfer();
        }
        UpdateExiInterrupts();
        return true;
    }
    case 0x04u:
        state.dmaAddress.store(value, std::memory_order_relaxed);
        return true;
    case 0x08u:
        state.dmaLength.store(value, std::memory_order_relaxed);
        return true;
    case 0x0Cu: {
        state.control.store(value & 0x3Fu, std::memory_order_relaxed);
        if ((value & kExiControlStartBit) != 0) {
            if ((value & kExiControlDmaBit) != 0) {
                ExiTransferDma(channel, value);
            } else {
                ExiTransferImmediate(channel, value);
            }
            state.control.fetch_and(~kExiControlStartBit, std::memory_order_relaxed);
            state.status.fetch_or(kExiStatusTransferCompleteBit, std::memory_order_relaxed);
            UpdateExiInterrupts();
        }
        return true;
    }
    case 0x10u:
        state.immediateData.store(value, std::memory_order_relaxed);
        return true;
    default:
        return false;
    }
}

uint64_t HostSteadyMicros() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

uint32_t AiSampleRate(uint32_t control) {
    return (control & 0x2u) != 0 ? 48000u : 32000u;
}

uint32_t ReadAiSampleCounterAt(uint64_t nowMicros) {
    const uint32_t control = g_aiControl.load(std::memory_order_relaxed);
    const uint32_t base = g_aiSampleCounterBase.load(std::memory_order_relaxed);
    if ((control & 0x1u) == 0) {
        return base;
    }
    const uint64_t epoch = g_aiSampleCounterEpochMicros.load(std::memory_order_relaxed);
    const uint64_t elapsed = nowMicros >= epoch ? nowMicros - epoch : 0;
    const uint64_t advanced = elapsed * AiSampleRate(control) / 1'000'000u;
    return base + static_cast<uint32_t>(advanced);
}

void LatchAiSampleCounter(uint64_t nowMicros) {
    g_aiSampleCounterBase.store(ReadAiSampleCounterAt(nowMicros), std::memory_order_relaxed);
    g_aiSampleCounterEpochMicros.store(nowMicros, std::memory_order_relaxed);
}

void ServiceGameCubeAudioDma(bool updateProgress = true) {
    constexpr uint16_t kAudioDmaEnable = 0x8000u;
    constexpr uint16_t kAudioDmaLengthMask = 0x7FFFu;
    constexpr uint16_t kDspAidInterruptStatus = 0x0008u;
    constexpr uint32_t kBytesPerDmaBlock = 32u;

    const uint16_t controlLength = g_audioDmaControlLength.load(std::memory_order_relaxed);
    const uint32_t blocks = g_audioDmaLatchedBlocks.load(std::memory_order_relaxed);
    const uint64_t epoch = g_audioDmaEpochMicros.load(std::memory_order_relaxed);
    if ((controlLength & kAudioDmaEnable) == 0 || blocks == 0 || epoch == 0) {
        return;
    }

    // AI_CR.AIDFR is inverted by the SDK: set selects the 32 kHz DSP/AID
    // clock, clear selects 48 kHz. AID DMA consumes interleaved stereo s16
    // samples, and the length register is expressed in 32-byte blocks.
    const uint32_t sampleRate =
        (g_aiControl.load(std::memory_order_relaxed) & 0x00000040u) != 0 ? 32000u : 48000u;
    const uint64_t totalBytes = static_cast<uint64_t>(blocks) * kBytesPerDmaBlock;
    const uint64_t durationMicros =
        GameCubeAudioDmaTiming::DurationMicros(blocks, sampleRate);
    const uint64_t now = HostSteadyMicros();
    const uint64_t elapsed = now >= epoch ? now - epoch : 0;

    if (elapsed < durationMicros) {
        // Polling the interrupt only needs the deadline. Compute and publish
        // the countdown when the guest actually reads its register.
        if (updateProgress) {
            g_audioDmaBlocksLeft.store(GameCubeAudioDmaTiming::RemainingBlocks(blocks, sampleRate, elapsed),
                                      std::memory_order_relaxed);
        }
        return;
    }

    // DMA automatically reloads while enabled. Register writes from the ISR
    // describe the NEXT transfer; they must not restart the active transfer.
    uint64_t expectedEpoch = epoch;
    // Keep the fractional block time instead of losing it on every late
    // poll. Catch up at most four blocks after a long guest/render stall.
    const uint64_t nextEpoch = std::max(epoch + durationMicros,
        now > 3 * durationMicros ? now - 3 * durationMicros : uint64_t{0});
    if (!g_audioDmaEpochMicros.compare_exchange_strong(
            expectedEpoch, nextEpoch, std::memory_order_acq_rel, std::memory_order_relaxed)) {
        return;
    }
    const uint32_t completedStart = g_audioDmaLatchedStart.load(std::memory_order_relaxed);
    if (Memory::Contains(completedStart, static_cast<size_t>(totalBytes))) {
        auto& backend = AudioBackend::Instance();
        if (backend.Init(sampleRate, 2))
            backend.PushWiiAiSamplesBE16(Memory::GetPointer(completedStart, totalBytes), totalBytes);
    }
    const uint16_t nextBlocks = controlLength & kAudioDmaLengthMask;
    g_audioDmaLatchedStart.store((g_audioDmaStartHi.load() << 16) | g_audioDmaStartLo.load());
    g_audioDmaLatchedBlocks.store(nextBlocks);
    g_audioDmaBlocksLeft.store(nextBlocks, std::memory_order_relaxed);
    g_dspControl.fetch_or(kDspAidInterruptStatus, std::memory_order_relaxed);
    g_gameCubeAudioDmaInterruptDelivered.store(false, std::memory_order_relaxed);
}

bool TryReadModeledAi32(uint32_t addr, uint32_t& value) {
    switch (addr) {
    case kAiControl:
        value = g_aiControl.load(std::memory_order_relaxed);
        return true;
    case kAiVolume:
        value = g_aiVolume.load(std::memory_order_relaxed);
        return true;
    case kAiSampleCounter:
        value = ReadAiSampleCounterAt(HostSteadyMicros());
        return true;
    case kAiInterruptTiming:
        value = g_aiInterruptTiming.load(std::memory_order_relaxed);
        return true;
    default:
        return false;
    }
}

bool TryWriteModeledAi32(uint32_t addr, uint32_t value) {
    switch (addr) {
    case kAiControl: {
        const uint64_t now = HostSteadyMicros();
        LatchAiSampleCounter(now);
        const uint32_t old = g_aiControl.load(std::memory_order_relaxed);
        uint32_t next = value & kAiPersistentControlMask;
        // AIINT is write-one-to-clear; writing zero preserves its current state.
        next = (next & ~kAiInterruptBit) |
               ((old & kAiInterruptBit) & ((value & kAiInterruptBit) == 0 ? kAiInterruptBit : 0u));
        g_aiControl.store(next, std::memory_order_relaxed);
        if ((value & kAiSampleCounterResetBit) != 0) {
            g_aiSampleCounterBase.store(0, std::memory_order_relaxed);
        }
        g_aiSampleCounterEpochMicros.store(now, std::memory_order_relaxed);
        return true;
    }
    case kAiVolume:
        g_aiVolume.store(value & 0x0000FFFFu, std::memory_order_relaxed);
        return true;
    case kAiSampleCounter:
        g_aiSampleCounterBase.store(value, std::memory_order_relaxed);
        g_aiSampleCounterEpochMicros.store(HostSteadyMicros(), std::memory_order_relaxed);
        return true;
    case kAiInterruptTiming:
        g_aiInterruptTiming.store(value, std::memory_order_relaxed);
        return true;
    default:
        return false;
    }
}

bool TryReadModeledDsp16(uint32_t addr, uint16_t& value) {
    if (addr == kDspMailboxToHi || addr == kDspMailboxToLo ||
        addr == kDspMailboxFromHi || addr == kDspMailboxFromLo) {
        std::lock_guard<std::mutex> lock(g_dspMutex);
        if ((addr == kDspMailboxFromHi || addr == kDspMailboxFromLo) && !g_dspFromMailboxFull) {
            if (g_dspBootstrapMailPending) {
                g_dspFromMailbox = kDspBootMail;
                g_dspFromMailboxFull = true;
                g_dspBootstrapMailPending = false;
            } else if (DSP_HLE_CheckMailFromDSP() != 0) {
                g_dspFromMailbox = DSP_HLE_ReadMailFromDSP() & 0x7FFFFFFFu;
                g_dspFromMailboxFull = true;
            }
        }
        switch (addr) {
        case kDspMailboxToHi:
            value = static_cast<uint16_t>((g_dspToMailboxHi & 0x7FFFu) |
                                          (g_dspToMailboxFull ? kDspMailboxFullBit : 0u));
            return true;
        case kDspMailboxToLo:
            value = g_dspToMailboxLo;
            return true;
        case kDspMailboxFromHi:
            value = static_cast<uint16_t>(((g_dspFromMailbox >> 16) & 0x7FFFu) |
                                          (g_dspFromMailboxFull ? kDspMailboxFullBit : 0u));
            return true;
        case kDspMailboxFromLo:
            value = static_cast<uint16_t>(g_dspFromMailbox);
            if (g_dspFromMailboxFull) {
                g_dspFromMailboxFull = false;
                // Reading the low half consumes the hardware mailbox.  If the
                // HLE has another interrupting task mail queued behind it, this
                // is the point where real DSP hardware can assert a fresh DIRQ.
                if (DSP_HLE_CheckMailFromDSP() != 0) {
                    Memory::RaiseGameCubeDspInterrupt();
                }
            }
            return true;
        default:
            break;
        }
    }
    switch (addr) {
    case kDspControl: {
        value = g_dspControl.load(std::memory_order_relaxed);
        return true;
    }
    case kArInfo: value = g_arInfo.load(std::memory_order_relaxed); return true;
    case kArMode: value = 1u; return true; // ARAM controller initialized
    case kArRefresh: value = g_arRefresh.load(std::memory_order_relaxed); return true;
    case kArDmaMmAddrHi: value = g_arDmaMmAddrHi.load(std::memory_order_relaxed); return true;
    case kArDmaMmAddrLo: value = g_arDmaMmAddrLo.load(std::memory_order_relaxed); return true;
    case kArDmaArAddrHi: value = g_arDmaArAddrHi.load(std::memory_order_relaxed); return true;
    case kArDmaArAddrLo: value = g_arDmaArAddrLo.load(std::memory_order_relaxed); return true;
    case kArDmaCountHi: value = g_arDmaCountHi.load(std::memory_order_relaxed); return true;
    case kArDmaCountLo: value = g_arDmaCountLo.load(std::memory_order_relaxed); return true;
    case kAudioDmaStartHi: value = g_audioDmaStartHi.load(std::memory_order_relaxed); return true;
    case kAudioDmaStartLo: value = g_audioDmaStartLo.load(std::memory_order_relaxed); return true;
    case kAudioDmaBlocksLength: value = g_audioDmaBlocksLength.load(std::memory_order_relaxed); return true;
    case kAudioDmaControlLength: value = g_audioDmaControlLength.load(std::memory_order_relaxed); return true;
    case kAudioDmaBlocksLeft:
        ServiceGameCubeAudioDma();
        value = g_audioDmaBlocksLeft.load(std::memory_order_relaxed);
        return true;
    default: return false;
    }
}

bool TryWriteModeledDsp16(uint32_t addr, uint16_t value) {
    if (addr == kDspMailboxToHi || addr == kDspMailboxToLo) {
        std::lock_guard<std::mutex> lock(g_dspMutex);
        if (addr == kDspMailboxToHi) {
            g_dspToMailboxHi = value;
            return true;
        }

        g_dspToMailboxLo = value;
        g_dspToMailboxFull = true;
        const uint32_t mail = (static_cast<uint32_t>(g_dspToMailboxHi) << 16) |
                              g_dspToMailboxLo;
        // Deliver all 32 bits before clearing mailbox-full. AX command/task
        // tags (BABE/CDD1) include bit 31; clearing it here loses the command.
        // The shared AX/DSP HLE consumes host->DSP mail synchronously. This is
        // the raw-MMIO bridge for generic GameCube DOLs; it keeps the hardware
        // mailbox visible to the SDK while reusing the existing DSP protocol
        // implementation rather than creating a second audio core.
        DSP_HLE_SendMailToDSP(mail);
        g_dspToMailboxFull = false;
        return true;
    }
    if (addr == kDspMailboxFromHi || addr == kDspMailboxFromLo) {
        // CPU writes to the DSP->CPU mailbox do not create mail. SDK startup
        // writes zero to the high half while the DSP is held/reset; accept that
        // hardware-style clear without manufacturing a response.
        if (addr == kDspMailboxFromHi && value == 0) {
            std::lock_guard<std::mutex> lock(g_dspMutex);
            g_dspFromMailbox = 0;
            g_dspFromMailboxFull = false;
            g_dspBootstrapMailPending = false;
            return true;
        }
        return true;
    }
    switch (addr) {
    case kDspControl: {
        // Match the hardware-facing bits used by Dolphin's DSP HLE. Reset is
        // write-one/self-clearing; AID/ARAM/DSP interrupt flags are W1C while
        // their adjacent mask bits and halt/init state are ordinary state.
        const uint16_t old = g_dspControl.load(std::memory_order_relaxed);
        constexpr uint16_t kInterruptFlags = 0x00A8u;
        uint16_t next = value;
        next &= static_cast<uint16_t>(~0x0001u); // DSPReset self-clears
        next = static_cast<uint16_t>((next & ~kInterruptFlags) |
                                     (old & kInterruptFlags & ~value));
        g_dspControl.store(next, std::memory_order_relaxed);
        if ((next & 0x0020u) == 0) {
            g_gameCubeAramInterruptDelivered.store(false, std::memory_order_relaxed);
        }
        if ((next & 0x0018u) != 0x0018u) {
            // AIDINT is bit 3 and AIDINTMSK is bit 4. ACKing the status or
            // masking the source deasserts the CPU-visible condition; a later
            // completion/unmask must therefore be claimable again.
            g_gameCubeAudioDmaInterruptDelivered.store(false, std::memory_order_relaxed);
        }
        if ((next & 0x0180u) != 0x0180u) {
            // DSPINT is bit 7 and DSPINTMSK is bit 8.  ACKing the current
            // request or masking it rearms host delivery; a queued subsequent
            // task mail reasserts DSPINT when the current mailbox is consumed.
            g_gameCubeDspInterruptDelivered.store(false, std::memory_order_relaxed);
        }
        // The SDK bootstrap program copied to 0x01000000 emits 0x00543448 after
        // HALT is released. We model that documented boot-ROM handoff at the
        // hardware boundary; later task/AX mails still flow through AxDspHle.
        if ((old & 0x0004u) != 0 && (next & 0x0004u) == 0) {
            std::lock_guard<std::mutex> lock(g_dspMutex);
            g_dspBootstrapMailPending = true;
        }
        return true;
    }
    case kArInfo: g_arInfo.store(value & 0x007Fu, std::memory_order_relaxed); return true;
    case kArRefresh: g_arRefresh.store(value & 0x07FFu, std::memory_order_relaxed); return true;
    case kArDmaMmAddrHi: g_arDmaMmAddrHi.store(value & 0x03FFu, std::memory_order_relaxed); return true;
    case kArDmaMmAddrLo: g_arDmaMmAddrLo.store(value & 0xFFE0u, std::memory_order_relaxed); return true;
    case kArDmaArAddrHi: g_arDmaArAddrHi.store(value & 0x03FFu, std::memory_order_relaxed); return true;
    case kArDmaArAddrLo: g_arDmaArAddrLo.store(value & 0xFFE0u, std::memory_order_relaxed); return true;
    case kArDmaCountHi: g_arDmaCountHi.store(value & 0x83FFu, std::memory_order_relaxed); return true;
    case kArDmaCountLo: {
        g_arDmaCountLo.store(value & 0xFFE0u, std::memory_order_relaxed);
        const uint32_t mainAddress =
            (static_cast<uint32_t>(g_arDmaMmAddrHi.load(std::memory_order_relaxed) & 0x03FFu) << 16) |
            g_arDmaMmAddrLo.load(std::memory_order_relaxed);
        const uint32_t aramAddress =
            (static_cast<uint32_t>(g_arDmaArAddrHi.load(std::memory_order_relaxed) & 0x03FFu) << 16) |
            g_arDmaArAddrLo.load(std::memory_order_relaxed);
        const uint16_t countHi = g_arDmaCountHi.load(std::memory_order_relaxed);
        const uint32_t length =
            (static_cast<uint32_t>(countHi & 0x03FFu) << 16) |
            g_arDmaCountLo.load(std::memory_order_relaxed);
        const bool aramToMain = (countHi & 0x8000u) != 0;

        if (length != 0) {
            // GameCube ARAM DMA exposes a 26-bit hardware address space. Retail
            // consoles contain 16 MiB of internal ARAM; addresses above that are
            // routed to the High Speed Port used by optional expansion devices.
            // Nintendo's __ARChecksize deliberately probes 0x01000000 and higher,
            // so treating those probes as invalid memory breaks the SDK's size
            // detection. With no HSP expansion attached, reads/writes transfer no
            // data. Keep the real 16 MiB backing store and model that bus routing
            // instead of manufacturing extra ARAM or zero-filling probe reads.
            const uint32_t hardwareAramAddress = aramAddress & kGameCubeAramAddressMask;
            if (hardwareAramAddress < kGameCubeAramSize) {
                uint8_t* mainMemory = Memory::GetPointer(mainAddress, length);
                uint32_t remaining = length;
                uint32_t currentAramAddress = hardwareAramAddress;
                uint8_t* currentMainMemory = mainMemory;
                while (remaining != 0) {
                    const uint32_t physicalAramAddress = currentAramAddress & (kGameCubeAramSize - 1u);
                    const uint32_t bytesToEnd = kGameCubeAramSize - physicalAramAddress;
                    const uint32_t chunk = std::min(remaining, bytesToEnd);
                    uint8_t* aram = g_gameCubeAram.data() + physicalAramAddress;
                    if (aramToMain) {
                        std::memcpy(currentMainMemory, aram, chunk);
                    } else {
                        std::memcpy(aram, currentMainMemory, chunk);
                    }
                    currentAramAddress += chunk;
                    currentMainMemory += chunk;
                    remaining -= chunk;
                }
            } else {
            }
        }

        // ARAM DMA completion raises DSPCR.AIDINT/ARINT bit 0x20. The SDK polls
        // this bit and acknowledges it with the existing W1C control path.
        g_dspControl.fetch_or(0x0020u, std::memory_order_relaxed);
        g_gameCubeAramInterruptDelivered.store(false, std::memory_order_relaxed);
        return true;
    }
    case kAudioDmaStartHi: g_audioDmaStartHi.store(value & 0x03FFu, std::memory_order_relaxed); return true;
    case kAudioDmaStartLo: g_audioDmaStartLo.store(value & 0xFFE0u, std::memory_order_relaxed); return true;
    case kAudioDmaBlocksLength: g_audioDmaBlocksLength.store(value, std::memory_order_relaxed); return true;
    case kAudioDmaControlLength: {
        constexpr uint16_t kAudioDmaEnable = 0x8000u;
        constexpr uint16_t kAudioDmaLengthMask = 0x7FFFu;
        const uint16_t previous = g_audioDmaControlLength.exchange(value, std::memory_order_relaxed);
        const uint16_t blocks = value & kAudioDmaLengthMask;
        if ((value & kAudioDmaEnable) != 0 && blocks != 0) {
            if ((previous & kAudioDmaEnable) == 0 || g_audioDmaEpochMicros.load() == 0) {
                g_audioDmaLatchedStart.store((g_audioDmaStartHi.load() << 16) | g_audioDmaStartLo.load());
                g_audioDmaLatchedBlocks.store(blocks);
                g_audioDmaBlocksLeft.store(blocks, std::memory_order_relaxed);
                g_audioDmaEpochMicros.store(HostSteadyMicros(), std::memory_order_relaxed);
                // AIDINT is raised when the transfer latches, including start.
                g_dspControl.fetch_or(0x0008u);
                g_gameCubeAudioDmaInterruptDelivered.store(false);
            }
        } else {
            g_audioDmaBlocksLeft.store(0u, std::memory_order_relaxed);
            g_audioDmaEpochMicros.store(0u, std::memory_order_relaxed);
        }
        return true;
    }
    default: return false;
    }
}

bool IsModeledMi16(uint32_t addr) {
    if (addr < kMiBase || addr > kMiLast || (addr & 1u) != 0) {
        return false;
    }
    const uint32_t offset = addr - kMiBase;
    return offset <= 0x10u ||
           (offset >= 0x1Cu && offset <= 0x24u) ||
           (offset >= 0x32u && offset <= 0x5Au);
}

bool TryReadModeledMi16(uint32_t addr, uint16_t& value) {
    if (!IsModeledMi16(addr)) {
        return false;
    }
    value = g_miRegisters[(addr - kMiBase) / 2u].load(std::memory_order_relaxed);
    return true;
}

bool TryWriteModeledMi16(uint32_t addr, uint16_t value) {
    if (!IsModeledMi16(addr)) {
        return false;
    }
    g_miRegisters[(addr - kMiBase) / 2u].store(value, std::memory_order_relaxed);
    return true;
}

bool TryReadModeledMmio32(uint32_t addr, uint32_t& value) {
    // EXI exists on both GameCube and Wii.  The register layout is shared; only
    // the physical base differs (0xCC006800 vs 0xCD006800).
    if (TryReadModeledExi32(addr, value)) {
        return true;
    }
    // DI register reads are shared as well.  Wii software in particular probes
    // DICFG at 0xCD006024 during SDK EXI initialization.
    if (TryReadModeledDi32(addr, value)) {
        return true;
    }
    if (TryReadModeledVi32(addr, value)) {
        return true;
    }
    if (TryReadModeledSi32(addr, value)) {
        return true;
    }
    if (TryReadModeledAi32(addr, value)) {
        return true;
    }
    if (addr >= kDspBase && addr <= kAudioDmaBlocksLeft && (addr & 3u) == 0) {
        uint16_t hi = 0;
        uint16_t lo = 0;
        if (TryReadModeledDsp16(addr, hi) && TryReadModeledDsp16(addr + 2u, lo)) {
            value = (static_cast<uint32_t>(hi) << 16) | lo;
            return true;
        }
    }
    if (addr >= kMiBase && addr < kMiLast && (addr & 3u) == 0) {
        uint16_t hi = 0;
        uint16_t lo = 0;
        if (TryReadModeledMi16(addr, hi) && TryReadModeledMi16(addr + 2u, lo)) {
            value = (static_cast<uint32_t>(hi) << 16) | lo;
            return true;
        }
    }
    switch (addr) {
    case kPiInterruptCause:
        value = g_piInterruptCause.load(std::memory_order_relaxed);
        return true;
    case kPiInterruptMask:
        value = g_piInterruptMask.load(std::memory_order_relaxed);
        return true;
    case kPiFifoBase:
        value = g_piFifoBase.load(std::memory_order_relaxed);
        return true;
    case kPiFifoEnd:
        value = g_piFifoEnd.load(std::memory_order_relaxed);
        return true;
    case kPiFifoWritePointer:
        value = g_piFifoWritePointer.load(std::memory_order_relaxed);
        return true;
    case kPiErrorCause:
        value = g_piErrorCause.load(std::memory_order_relaxed);
        return true;
    case kPiErrorAddress:
        value = g_piErrorAddress.load(std::memory_order_relaxed);
        return true;
    case kPiResetCode:
        value = g_piResetCode.load(std::memory_order_relaxed);
        if (!g_loggedPiColdBoot.exchange(true, std::memory_order_relaxed)) {
            RT_LOG(RT_TAG_MEMORY) << "PI_RESET_CODE: cold boot (raw=0x" << std::hex << value
                                  << std::dec << ')' << std::endl;
        }
        return true;
#if WIICOMPILED_GUEST_IS_GAMECUBE
    case kPiUnknown:
        value = g_piUnknown.load(std::memory_order_relaxed);
        return true;
    case kPiFlipperRevision:
        value = kFlipperRevisionC;
        return true;
    case kPiFlipperBusStrength:
        value = g_piFlipperBusStrength.load(std::memory_order_relaxed);
        return true;
#endif
    default:
        break;
    }
    if (addr == kHollywoodGpioBOut || addr == kHollywoodGpioBOutTrusted) {
        value = g_hollywoodGpioBOut.load(std::memory_order_relaxed);
        return true;
    }
    if (addr == kHollywoodGpioBDirTrusted) {
        value = g_hollywoodGpioBDir.load(std::memory_order_relaxed);
        return true;
    }
    if (addr == kHollywoodGpioBInTrusted) {
        // The only external Broadway-visible input modeled by Dolphin is the
        // disc-slot presence line. AVE_SDA is driven by the external encoder;
        // for the write-only SDK initialization sequence used here, a low SDA
        // is the device ACK. Leave all unmodeled input lines low rather than
        // inventing host hardware state.
        value = 0;
        return true;
    }
    switch (addr) {
    case kHollywoodCompat:
        value = g_hollywoodCompat.load(std::memory_order_relaxed);
        return true;
    case kHollywoodPllAi:
        value = g_hollywoodPllAi.load(std::memory_order_relaxed);
        return true;
    case kHollywoodPllAiExt:
        value = g_hollywoodPllAiExt.load(std::memory_order_relaxed);
        return true;
    default:
        break;
    }
    return false;
}

bool TryWriteModeledMmio32(uint32_t addr, uint32_t value) {
    if (TryWriteModeledExi32(addr, value)) {
        return true;
    }
    if (TryWriteModeledVi32(addr, value)) {
        return true;
    }
    if (TryWriteModeledSi32(addr, value)) {
        return true;
    }
    if (TryWriteModeledAi32(addr, value)) {
        return true;
    }
    if (addr >= kDspBase && addr <= kAudioDmaBlocksLeft && (addr & 3u) == 0) {
        if (TryWriteModeledDsp16(addr, static_cast<uint16_t>(value >> 16)) &&
            TryWriteModeledDsp16(addr + 2u, static_cast<uint16_t>(value))) {
            return true;
        }
    }
#if WIICOMPILED_GUEST_IS_GAMECUBE
    if (TryWriteModeledDi32(addr, value)) {
        return true;
    }
#endif
    if (addr >= kMiBase && addr < kMiLast && (addr & 3u) == 0) {
        if (IsModeledMi16(addr) && IsModeledMi16(addr + 2u)) {
            TryWriteModeledMi16(addr, static_cast<uint16_t>(value >> 16));
            TryWriteModeledMi16(addr + 2u, static_cast<uint16_t>(value));
            return true;
        }
    }
    switch (addr) {
    case kPiInterruptCause:
        g_piInterruptCause.fetch_and(~value, std::memory_order_relaxed);
        return true;
    case kPiInterruptMask:
        g_piInterruptMask.store(value, std::memory_order_relaxed);
#if WIICOMPILED_GUEST_IS_GAMECUBE
        if ((value & 0x00000004u) == 0) {
            // If software masks DI while its source remains asserted, the CPU
            // interrupt line deasserts. A later unmask must be able to deliver
            // that still-pending source as a fresh assertion.
            g_gameCubeDiInterruptDelivered.store(false, std::memory_order_relaxed);
        }
        if ((value & 0x00000008u) == 0) {
            g_gameCubeSiInterruptDelivered.store(false, std::memory_order_relaxed);
        }
#endif
        return true;
    case kPiFifoBase:
        g_piFifoBase.store(value & kPiFifoAddressMask, std::memory_order_relaxed);
        return true;
    case kPiFifoEnd:
        g_piFifoEnd.store(value & kPiFifoAddressMask, std::memory_order_relaxed);
        return true;
    case kPiFifoWritePointer:
        g_piFifoWritePointer.store(value & kPiFifoAddressMask, std::memory_order_relaxed);
        return true;
    case kPiFifoReset:
        // GXAbortFrame writes bit 0 to reset the hardware gather/CP FIFO. Aurora owns
        // the host-side FIFO lifetime, so acknowledge the PI command here; a title that
        // actually depends on an in-flight abort needs a dedicated Aurora reset bridge.
        if ((value & 1u) != 0) {
            RT_LOG(RT_TAG_MEMORY) << "PI_FIFO_RESET requested" << std::endl;
        }
        return true;
    case kPiErrorCause:
        g_piErrorCause.store(value & 0x7u, std::memory_order_relaxed);
        return true;
    case kPiResetCode:
        g_piResetCode.store(value, std::memory_order_relaxed);
        return true;
#if WIICOMPILED_GUEST_IS_GAMECUBE
    case kPiUnknown:
        g_piUnknown.store(value & 0x3FFu, std::memory_order_relaxed);
        return true;
    case kPiFlipperBusStrength:
        g_piFlipperBusStrength.store(value & 0x07FFFFFFu, std::memory_order_relaxed);
        return true;
#endif
    default:
        break;
    }
    if (addr == kHollywoodGpioBOut || addr == kHollywoodGpioBOutTrusted) {
        const uint32_t previous = g_hollywoodGpioBOut.exchange(value, std::memory_order_relaxed);
        if ((previous ^ value) & kHollywoodGpioSensorBar) {
            RT_LOG(RT_TAG_MEMORY) << "Hollywood GPIOB_OUT: sensor bar power "
                                  << ((value & kHollywoodGpioSensorBar) ? "enabled" : "disabled")
                                  << " (value=0x" << std::hex << value << std::dec << ')' << std::endl;
        }
        return true;
    }
    if (addr == kHollywoodGpioBDirTrusted) {
        g_hollywoodGpioBDir.store(value, std::memory_order_relaxed);
        return true;
    }
    switch (addr) {
    case kHollywoodCompat:
        g_hollywoodCompat.store(value, std::memory_order_relaxed);
        return true;
    case kHollywoodPllAi:
        g_hollywoodPllAi.store(value, std::memory_order_relaxed);
        return true;
    case kHollywoodPllAiExt:
        g_hollywoodPllAiExt.store(value, std::memory_order_relaxed);
        return true;
    default:
        break;
    }
    return false;
}

// MMIO reads have no backing store or generic fallback. Answering zero (the old sparse fallback)
// turned unimplemented devices into silent hangs, so any register not explicitly modeled above
// still throws exactly like an unknown MMIO write.
[[noreturn]] void ThrowMmioReadBlocked(uint32_t addr, size_t length) {
    std::ostringstream reason;
    reason << (MemoryInline::IsGpuFifoAddress(addr)
                   ? "GPU FIFO read blocked (the gather pipe is write-only)"
                   : "MMIO read blocked (non-GPU)")
           << "; add HLE for this device instead of answering zero"
           << " (active=0x" << std::hex << std::uppercase
           << RecompMod::CurrentTranslatedExecutionAddress() << std::dec << std::nouppercase << ")";
    throw Memory::AccessViolation(addr, length, reason.str());
}
} // namespace

void Memory::RebaseViBeamToRetrace(uint64_t retraceMicros, bool secondHalf) {
    InitializeViState();

    const uint16_t displayControl =
        g_viRegisters[(0x02u) / 2u].load(std::memory_order_relaxed);
    const RawViTiming timing = RawViTimingForDisplayControl(displayControl);
    const uint64_t frameMicros =
        timing.frameMicrosNumerator / timing.frameMicrosDenominator;
    const uint64_t phaseMicros = secondHalf ? frameMicros / 2u : 0u;
    const uint64_t phaseHalfLines = secondHalf ? timing.halfLinesPerFrame / 2u : 0u;

    // retraceMicros and HostSteadyMicros() share std::chrono::steady_clock's
    // epoch. Backdating the raw epoch by the physical half-frame phase makes the
    // beam read exactly that field at the scheduled retrace boundary without
    // resetting any guest-visible interrupt/comparator state.
    const uint64_t epochMicros =
        retraceMicros >= phaseMicros ? retraceMicros - phaseMicros : 0u;
    g_viEpochMicros.store(epochMicros, std::memory_order_relaxed);
    g_viLastHalfLineAbsolute.store(phaseHalfLines, std::memory_order_relaxed);
}

uint8_t MemoryInline::Read8Slow(uint32_t addr) {
    if (IsMmioAddress(addr)) {
        ThrowMmioReadBlocked(addr, sizeof(uint8_t));
    }
    ResolveDeferredReads(addr, sizeof(uint8_t));
    return ReadScalar<uint8_t>(addr);
}

uint16_t MemoryInline::Read16Slow(uint32_t addr) {
    if (IsMmioAddress(addr)) {
        uint16_t value = 0;
        if (GX_HLE_TryCpRegisterRead16(addr, &value)) {
            TraceGameCubePostAramMmioRead(addr, sizeof(uint16_t), value);
            return value;
        }
        if (GX_HLE_TryPeRegisterRead16(addr, &value)) {
            TraceGameCubePostAramMmioRead(addr, sizeof(uint16_t), value);
            return value;
        }
        if (TryReadModeledDsp16(addr, value)) {
            TraceGameCubePostAramMmioRead(addr, sizeof(uint16_t), value);
            return value;
        }
        if (TryReadModeledVi16(addr, value)) {
            TraceGameCubePostAramMmioRead(addr, sizeof(uint16_t), value);
            return value;
        }
        if (TryReadModeledMi16(addr, value)) {
            TraceGameCubePostAramMmioRead(addr, sizeof(uint16_t), value);
            return value;
        }
#if WIICOMPILED_GUEST_IS_GAMECUBE
        if (addr >= kAiBase && addr <= kAiInterruptTiming + 2u) {
            const uint32_t aligned = addr & ~3u;
            uint32_t word = 0;
            if (TryReadModeledAi32(aligned, word)) {
                value = static_cast<uint16_t>((addr & 2u) != 0 ? word : (word >> 16));
                TraceGameCubePostAramMmioRead(addr, sizeof(uint16_t), value);
                return value;
            }
        }
        if (addr >= kPiBase && addr <= kPiFlipperBusStrength + 2u) {
            const uint32_t aligned = addr & ~3u;
            uint32_t word = 0;
            if (TryReadModeledMmio32(aligned, word)) {
                value = static_cast<uint16_t>((addr & 2u) != 0 ? word : (word >> 16));
                TraceGameCubePostAramMmioRead(addr, sizeof(uint16_t), value);
                return value;
            }
        }
#endif
        ThrowMmioReadBlocked(addr, sizeof(uint16_t));
    }
    ResolveDeferredReads(addr, sizeof(uint16_t));
    return ReadScalar<uint16_t>(addr);
}

uint32_t MemoryInline::Read32Slow(uint32_t addr) {
    if (IsMmioAddress(addr)) {
        uint32_t value = 0;
        if (TryReadModeledMmio32(addr, value)) {
            TraceGameCubePostAramMmioRead(addr, sizeof(uint32_t), value);
            return value;
        }
        ThrowMmioReadBlocked(addr, sizeof(uint32_t));
    }
    ResolveDeferredReads(addr, sizeof(uint32_t));
    const uint32_t value = ReadScalar<uint32_t>(addr);
    return value;
}

uint64_t MemoryInline::Read64Slow(uint32_t addr) {
    if (IsMmioAddress(addr)) {
        ThrowMmioReadBlocked(addr, sizeof(uint64_t));
    }
    ResolveDeferredReads(addr, sizeof(uint64_t));
    return ReadScalar<uint64_t>(addr);
}

double MemoryInline::ReadFloat32Slow(uint32_t addr) {
    if (IsMmioAddress(addr)) {
        ThrowMmioReadBlocked(addr, sizeof(float));
    }
    ResolveDeferredReads(addr, sizeof(uint32_t));
    const auto bits = ReadScalar<uint32_t>(addr);
    return PpcLoadFloat32BitsInline(bits);
}

double MemoryInline::ReadFloat64Slow(uint32_t addr) {
    if (IsMmioAddress(addr)) {
        ThrowMmioReadBlocked(addr, sizeof(double));
    }
    ResolveDeferredReads(addr, sizeof(uint64_t));
    const auto bits = ReadScalar<uint64_t>(addr);
    double value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void MemoryInline::Write8Slow(uint32_t addr, uint8_t val) {
    if (IsGpuFifoAddress(addr)) {
        GX_HLE_FIFO_Write8(val);
        return;
    }
    if (IsMmioAddress(addr)) {
        throw Memory::AccessViolation(addr, sizeof(val), "MMIO write blocked (non-GPU)");
    }
    WriteScalar(addr, val);
}

void MemoryInline::Write16Slow(uint32_t addr, uint16_t val) {
    if (IsGpuFifoAddress(addr)) {
        GX_HLE_FIFO_Write16(val);
        return;
    }
    if (GX_HLE_TryCpRegisterWrite16(addr, val)) {
        return;
    }
    if (GX_HLE_TryPeRegisterWrite16(addr, val)) {
        return;
    }
    if (TryWriteModeledDsp16(addr, val)) {
        return;
    }
    if (TryWriteModeledVi16(addr, val)) {
        return;
    }
    if (TryWriteModeledMi16(addr, val)) {
        return;
    }
#if WIICOMPILED_GUEST_IS_GAMECUBE
    if (addr >= kAiBase && addr <= kAiInterruptTiming + 2u) {
        const uint32_t aligned = addr & ~3u;
        uint32_t word = 0;
        if (TryReadModeledAi32(aligned, word)) {
            const uint32_t merged = (addr & 2u) != 0
                ? ((word & 0xFFFF0000u) | val)
                : ((word & 0x0000FFFFu) | (static_cast<uint32_t>(val) << 16));
            if (TryWriteModeledAi32(aligned, merged)) {
                return;
            }
        }
    }
#endif
    if (IsMmioAddress(addr)) {
        throw Memory::AccessViolation(addr, sizeof(val), "MMIO write blocked (non-GPU)");
    }
    WriteScalar(addr, val);
}

void MemoryInline::Write32Slow(uint32_t addr, uint32_t val) {
    if (IsGpuFifoAddress(addr)) {
        GX_HLE_FIFO_Write32(val);
        return;
    }
    if (TryWriteModeledMmio32(addr, val)) {
        return;
    }
    if (IsMmioAddress(addr)) {
        std::ostringstream reason;
        reason << "MMIO write blocked (non-GPU); add HLE for this device"
               << " (active=0x" << std::hex << std::uppercase
               << RecompMod::CurrentTranslatedExecutionAddress() << std::dec << std::nouppercase << ')';
        throw Memory::AccessViolation(addr, sizeof(val), reason.str());
    }
    WriteScalar(addr, val);
}

void MemoryInline::Write64Slow(uint32_t addr, uint64_t val) {
    if (IsGpuFifoAddress(addr)) {
        // The gather pipe is a byte-stream FIFO; a 64-bit store is two big-endian
        // 32-bit pushes, high word first. Without this arm the write fell through
        // to WriteScalar, which found no mapped region behind 0xCC008000 and threw
        // "no mapped region" instead of reaching GX.
        GX_HLE_FIFO_Write32(static_cast<uint32_t>(val >> 32));
        GX_HLE_FIFO_Write32(static_cast<uint32_t>(val));
        return;
    }
    if (IsMmioAddress(addr)) {
        throw Memory::AccessViolation(addr, sizeof(val), "MMIO write blocked (non-GPU)");
    }
    WriteScalar(addr, val);
}

void MemoryInline::WriteFloat32Slow(uint32_t addr, double val) {
    // stfs stores the IEEE-754 bit pattern of the single-precision value, never a
    // truncated integer.
    const uint32_t bits = ConvertPpcDoubleToSingleBits(val);
    if (IsGpuFifoAddress(addr)) {
        GX_HLE_FIFO_WriteFloat(PpcSingleBitsToFloat(bits));
        return;
    }
    if (IsMmioAddress(addr)) {
        throw Memory::AccessViolation(addr, sizeof(float), "MMIO write blocked (non-GPU)");
    }

    WriteScalar(addr, bits);
}

void MemoryInline::WriteFloat64Slow(uint32_t addr, double val) {
    // stfd stores the full 64-bit FPR bit pattern - load-bearing for the
    // fctiwz->stfd->lwz idiom, where the integer result lives in the low word.
    uint64_t bits;
    std::memcpy(&bits, &val, sizeof(bits));

    if (IsGpuFifoAddress(addr)) {
        GX_HLE_FIFO_WriteFloat(static_cast<float>(val));
        return;
    }
    if (IsMmioAddress(addr)) {
        throw Memory::AccessViolation(addr, sizeof(double), "MMIO write blocked (non-GPU)");
    }

    WriteScalar(addr, bits);
}

uint8_t* Memory::GetPointer(uint32_t addr) {
    return GetPointer(addr, 1);
}

uint8_t* Memory::GetPointer(uint32_t addr, size_t length) {
    if (auto* ptr = MemoryInline::GetPointerFast(addr, length)) {
        return ptr;
    }
    auto& region = ResolveRegion(addr, length);
    auto offset = static_cast<size_t>(addr - region.config.baseAddress);
    return region.storagePtr + offset;
}

uint8_t* Memory::GetGameCubeAramPointer() { return g_gameCubeAram.data(); }

bool Memory::ClaimGameCubeAramInterrupt()
{
    // DSPCR bit 5 is the ARAM interrupt status and bit 6 is its mask/enable.
    // Leave the status asserted until translated SDK code acknowledges it by
    // writing DSPCR; this latch is only about one host delivery per assertion.
    const uint16_t control = g_dspControl.load(std::memory_order_relaxed);
    if ((control & 0x0060u) != 0x0060u) {
        return false;
    }
    bool expected = false;
    return g_gameCubeAramInterruptDelivered.compare_exchange_strong(
        expected, true, std::memory_order_acq_rel, std::memory_order_relaxed);
}

bool Memory::ClaimGameCubeAudioDmaInterrupt()
{
    // Advance the raw AID device at the same hardware boundary used to poll
    // asynchronous external interrupts. DSPCR bit 3 is AIDINT status and bit 4
    // is its mask/enable; translated SDK code owns both fields and their ACK.
    ServiceGameCubeAudioDma(false);
    const uint16_t control = g_dspControl.load(std::memory_order_relaxed);
    if ((control & 0x0018u) != 0x0018u) {
        return false;
    }
    bool expected = false;
    return g_gameCubeAudioDmaInterruptDelivered.compare_exchange_strong(
        expected, true, std::memory_order_acq_rel, std::memory_order_relaxed);
}

void Memory::SetGameCubeAudioDmaResyncAfterDelivery(bool enabled)
{
    g_gameCubeAudioDmaResyncAfterDelivery.store(enabled, std::memory_order_relaxed);
}

void Memory::CompleteGameCubeAudioDmaInterruptDelivery()
{
    if (!g_gameCubeAudioDmaResyncAfterDelivery.load(std::memory_order_relaxed)) {
        return;
    }

    const uint16_t controlLength = g_audioDmaControlLength.load(std::memory_order_relaxed);
    const uint32_t blocks = g_audioDmaLatchedBlocks.load(std::memory_order_relaxed);
    if ((controlLength & 0x8000u) == 0u || blocks == 0u) {
        return;
    }

    // Do this after the translated ISR has fully returned.  STF's AID callback
    // performs a sizeable CPU-side AX update and can take longer on the host
    // than one short DMA period.  Re-anchoring at the ACK near the start of the
    // callback would still leave the next completion overdue on return.
    g_audioDmaEpochMicros.store(HostSteadyMicros(), std::memory_order_relaxed);
    g_audioDmaBlocksLeft.store(static_cast<uint16_t>(blocks), std::memory_order_relaxed);
}

void Memory::RaiseGameCubeDspInterrupt()
{
#if WIICOMPILED_GUEST_IS_GAMECUBE
    // Keep this atomic-only: AxDspHle::PushMail calls us while holding its own
    // mutex, whereas raw mailbox reads take the DSP MMIO mutex before entering
    // AxDspHle.  Taking either lock here would invert that established order.
    g_dspControl.fetch_or(0x0080u, std::memory_order_release);
#endif
}

bool Memory::ClaimGameCubeDspInterrupt()
{
#if WIICOMPILED_GUEST_IS_GAMECUBE
    // DSPCR bit 7 is DSPINT status and bit 8 is its CPU mask/enable.  Status is
    // persistent until the translated SDK W1C-acknowledges it.
    const uint16_t control = g_dspControl.load(std::memory_order_acquire);
    if ((control & 0x0180u) != 0x0180u) {
        return false;
    }
    bool expected = false;
    return g_gameCubeDspInterruptDelivered.compare_exchange_strong(
        expected, true, std::memory_order_acq_rel, std::memory_order_relaxed);
#else
    return false;
#endif
}

void Memory::CompleteGameCubeDspHleBootHandshake()
{
#if WIICOMPILED_GUEST_IS_GAMECUBE
    std::lock_guard<std::mutex> lock(g_dspMutex);
    g_dspBootstrapMailPending = false;
    if (g_dspFromMailboxFull && g_dspFromMailbox == kDspBootMail) {
        g_dspFromMailbox = 0;
        g_dspFromMailboxFull = false;
    }
#endif
}

bool Memory::Contains(uint32_t addr, size_t length) {
    if (MemoryInline::GetPointerFast(addr, length)) {
        return true;
    }
    // Contains is a membership predicate: callers probe arbitrary guest values
    // (e.g. gfx-node slots that may hold floats), so a miss must stay silent
    // rather than take ResolveRegion's crash-dump path.
    return TryResolveRegion(addr, length) != nullptr;
}

bool Memory::ClaimGameCubeDiInterrupt()
{
    // PI cause bit 2 is the DI source. The CPU only sees it as an external
    // interrupt while the matching PI mask bit is enabled. Keep DI_STATUS and
    // DI_COVER untouched here: translated DVDLow/OS code owns the hardware ACK
    // through their W1C fields, which rearms our delivery latch via
    // UpdateDiPiInterrupt() once the source deasserts.
    const uint32_t cause = g_piInterruptCause.load(std::memory_order_relaxed);
    const uint32_t mask = g_piInterruptMask.load(std::memory_order_relaxed);
    if ((cause & mask & 0x00000004u) == 0) {
        return false;
    }
    bool expected = false;
    return g_gameCubeDiInterruptDelivered.compare_exchange_strong(
        expected, true, std::memory_order_acq_rel, std::memory_order_relaxed);
}

// The generic raw-VI model starts with the hardware's boot-time VI cause bit
// asserted (kPiInitialInterruptCause). Until the guest programs DI0..DI3 there
// is no VI register status bit for the retail handler to ACK, so claiming that
// one boot assertion also deasserts the synthetic PI source. Later programmed
// VI events re-arm through the raw-VI status path.
bool GameCube_ClaimViInterrupt()
{
    UpdateViDisplayInterruptTiming();
    const uint32_t cause = g_piInterruptCause.load(std::memory_order_relaxed);
    const uint32_t mask = g_piInterruptMask.load(std::memory_order_relaxed);
    if ((cause & mask & kViPiCauseBit) == 0u) {
        return false;
    }

    bool expected = false;
    if (!g_gameCubeViInterruptDelivered.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel, std::memory_order_relaxed)) {
        return false;
    }

    bool hasRegisterStatus = false;
    for (const uint32_t offset : kViInterruptHighOffsets) {
        const uint16_t high = g_viRegisters[offset / 2u].load(std::memory_order_relaxed);
        hasRegisterStatus |= (high & kViInterruptStatusBit) != 0u;
    }
    if (!hasRegisterStatus) {
        g_piInterruptCause.fetch_and(~kViPiCauseBit, std::memory_order_relaxed);
    }
    return true;
}

void Memory::EnableGameCubeMemoryCard() {
    std::call_once(g_cardInitOnce, [] {
        InitializeExiState();
        const auto path = RuntimeConfigFile::ApplicationDataDirectory() /
                          "MemoryCards" / "MemoryCardA.USA.raw";
        g_slotACard.Open(path);
        const auto flash = g_slotACard.FlashId();
        std::copy(flash.begin(), flash.end(), g_iplSram.begin() + 24);
        uint8_t checksum = 0;
        for (const uint8_t byte : flash) checksum += byte;
        g_iplSram[62] = static_cast<uint8_t>(checksum ^ 0xFF);
        g_exiChannels[0].status.fetch_or(kExiStatusExtBit, std::memory_order_relaxed);
        RT_LOG(RT_TAG_MEMORY) << "[gc-card] slot A: " << path.string()
                              << " (" << g_slotACard.Data().size() << " bytes)" << std::endl;
    });
}

int Memory::ClaimGameCubeExiInterrupt() {
    UpdateExiInterrupts();
    if ((g_piInterruptMask.load(std::memory_order_relaxed) & 0x10u) == 0) return -1;
    for (uint32_t ch = 0; ch < 3; ++ch) {
        const uint32_t status = g_exiChannels[ch].status.load(std::memory_order_relaxed);
        constexpr uint32_t flags[] = {2u, 8u, 0x800u};
        constexpr uint32_t masks[] = {1u, 4u, 0x400u};
        for (uint32_t kind = 0; kind < (ch < 2 ? 3u : 2u); ++kind) {
            if ((status & flags[kind]) && (status & masks[kind]) &&
                !(g_exiDelivered[ch] & flags[kind])) {
                g_exiDelivered[ch] |= flags[kind];
                return static_cast<int>(9 + ch * 3 + kind);
            }
        }
    }
    return -1;
}

bool Memory::ClaimGameCubeSiInterrupt()
{
    // PI cause bit 3 is the Serial Interface source. The translated SDK owns
    // SI_COMCSR/SI_STATUS and PI masking; this latch only prevents duplicate
    // host injection while one hardware assertion remains pending.
    const uint32_t cause = g_piInterruptCause.load(std::memory_order_relaxed);
    const uint32_t mask = g_piInterruptMask.load(std::memory_order_relaxed);
    if ((cause & mask & 0x00000008u) == 0) {
        return false;
    }
    bool expected = false;
    return g_gameCubeSiInterruptDelivered.compare_exchange_strong(
        expected, true, std::memory_order_acq_rel, std::memory_order_relaxed);
}

std::vector<Memory::RegionConfig> Memory::DescribeRegions() {
    std::lock_guard<std::mutex> lock(RegionMutex());
    std::vector<RegionConfig> info;
    info.reserve(Regions().size());
    for (const auto& region : Regions()) {
        info.push_back(RegionConfig{region.config.name, region.config.baseAddress, region.storageSize});
    }
    return info;
}
