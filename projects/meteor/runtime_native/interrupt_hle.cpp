#include "abi_bridge.h"
#include "memory.h"
#include "ppc_runtime.h"
#include "runtime_log.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

constexpr uint32_t kInterruptTablePtr = 0x8062ADA0u; // r13 - 0x6580
constexpr uint32_t kInterruptTable = 0x80003040u;
constexpr size_t kInterruptTableBytes = 0x80u;
constexpr uint32_t kInterruptMaskLo = 0x800000C4u;
constexpr uint32_t kInterruptMaskHi = 0x800000C8u;

constexpr uint32_t kExceptionTablePtr = 0x8062AD08u; // r13 - 0x6618
constexpr uint32_t kExternalInterruptException = 4u;
constexpr uint32_t kExternalInterruptHandler = 0x8020E4ACu;
constexpr uint32_t kMsrExternalInterruptEnable = 0x00008000u;

void ClearGuestRange(uint32_t address, size_t size) {
    if (auto* ptr = Memory::GetPointer(address, size)) {
        std::memset(ptr, 0, size);
        return;
    }
    for (size_t offset = 0; offset < size; offset += sizeof(uint32_t)) {
        Memory::Write32(address + static_cast<uint32_t>(offset), 0);
    }
}

} // namespace

extern "C" void meteor_OSInterruptInit(CpuContext* cpu) {
    if (!cpu) {
        return;
    }

    // RDSPAF 0x8020DDE8, validated against the DOL in Ghidra headless:
    // initialize the software-visible interrupt state exactly as the SDK does,
    // but do not program the physical PI/Hollywood registers at
    // 0xCC003004/0xCD000034 or the per-device MMIO masks.
    const uint32_t previousMsr = cpu->msr;
    cpu->msr &= ~kMsrExternalInterruptEnable;

    Memory::Write32(kInterruptTablePtr, kInterruptTable);
    ClearGuestRange(kInterruptTable, kInterruptTableBytes);
    Memory::Write32(kInterruptMaskLo, 0u);
    Memory::Write32(kInterruptMaskHi, 0u);

    uint32_t previousHandler = 0;
    const uint32_t exceptionTable = Memory::Read32(kExceptionTablePtr);
    if (exceptionTable != 0) {
        const uint32_t slot = exceptionTable + kExternalInterruptException * sizeof(uint32_t);
        previousHandler = Memory::Read32(slot);
        Memory::Write32(slot, kExternalInterruptHandler);
    } else {
        RT_LOG(RT_TAG_OS) << "RDSPAF OSInterruptInit: exception table pointer is zero; "
                          << "external interrupt handler not installed" << std::endl;
    }

    cpu->msr = previousMsr;
    cpu->gpr[3] = previousHandler;

    RT_LOG(RT_TAG_OS) << "RDSPAF OSInterruptInit: initialized guest tables; skipped PI/Hollywood MMIO"
                      << std::endl;
}

REGISTER_TITLE_NATIVE_FUNCTION(0x8020DDE8, meteor_OSInterruptInit);
