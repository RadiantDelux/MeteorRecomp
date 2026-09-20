#include "gamecube_external_interrupts.h"

#include <atomic>
#include <cstdint>
#include <stdexcept>

#include "abi_bridge.h"
#include "hle_stubs.h"
#include "memory.h"
#include "ppc_runtime.h"
#include "runtime_log.h"
#include "wiicompiled_target.h"

#if WIICOMPILED_GUEST_IS_GAMECUBE

bool GameCube_ClaimViInterrupt();

namespace {

constexpr uint32_t kInterruptHandlerTable = 0x80003040u;
constexpr uint32_t kCurrentThreadPointer = 0x800000D4u;
constexpr uint32_t kMsrExternalInterruptEnable = 0x00008000u;

constexpr uint32_t kInterruptAid = 5u;
constexpr uint32_t kInterruptAram = 6u;
constexpr uint32_t kInterruptDsp = 7u;
constexpr uint32_t kInterruptPeToken = 18u;
constexpr uint32_t kInterruptPeFinish = 19u;
constexpr uint32_t kInterruptSi = 20u;
constexpr uint32_t kInterruptDi = 21u;
constexpr uint32_t kInterruptVi = 24u;

std::atomic<uint32_t> g_loggedInterrupts{0};

uint32_t InstalledHandler(uint32_t id)
{
    return Memory::Read32(kInterruptHandlerTable + id * 4u);
}

void LogFirstDelivery(uint32_t id, uint32_t handler, uint32_t boundaryPc)
{
    if (id >= 32u) {
        return;
    }
    const uint32_t bit = 1u << id;
    const uint32_t previous = g_loggedInterrupts.fetch_or(bit, std::memory_order_relaxed);
    if ((previous & bit) == 0u) {
        RT_LOG(RT_TAG_OS) << "[gc-irq] delivering interrupt " << id
                          << " to 0x" << std::hex << handler
                          << " at guest boundary 0x" << boundaryPc
                          << std::dec << std::endl;
    }
}

void DeliverInstalledInterrupt(uint32_t id, uint32_t boundaryPc,
                               CpuContext* interruptedCpu, uint32_t interruptedContext)
{
    const uint32_t handler = InstalledHandler(id);
    if (handler == 0u) {
        throw std::runtime_error("GameCube external interrupt has no installed guest handler");
    }

    CpuContext interruptCpu = *interruptedCpu;
    CpuContextScope scope(&interruptCpu);
    interruptCpu.msr &= ~kMsrExternalInterruptEnable;
    interruptCpu.gpr[3] = id;
    interruptCpu.gpr[4] = interruptedContext;

    LogFirstDelivery(id, handler, boundaryPc);
    // External interrupts can arrive at any translated call/backedge boundary.
    // Run the guest handler on the private interrupt register file, but defer
    // any scheduler-driven thread switch until the interrupted translated
    // function has unwound to a safe boundary.  Without this guard an IRQ can
    // reschedule while a constructor owns stack-resident state, letting a
    // different guest thread reuse/overwrite that stack frame.
    OS_HLE_BeginDeferredGuestCallbacks();
    try {
        InvokeIndirectCpu(handler, &interruptCpu);
    } catch (...) {
        OS_HLE_EndDeferredGuestCallbacks();
        throw;
    }
    OS_HLE_EndDeferredGuestCallbacks();
}

template <typename Claim>
bool TryDeliver(uint32_t id, uint32_t boundaryPc, CpuContext* interruptedCpu,
                uint32_t interruptedContext, Claim&& claim)
{
    // Do not consume the device latch before the linked SDK has installed the
    // matching handler. This keeps a pending source retryable during early boot.
    if (InstalledHandler(id) == 0u || !claim()) {
        return false;
    }
    DeliverInstalledInterrupt(id, boundaryPc, interruptedCpu, interruptedContext);
    return true;
}

} // namespace

void GameCube_ServiceExternalInterrupts(uint32_t boundaryPc, CpuContext* ctx)
{
    if (ctx == nullptr || (ctx->msr & kMsrExternalInterruptEnable) == 0u) {
        return;
    }


    uint32_t interruptedContext = 0u;
    try {
        interruptedContext = Memory::Read32(kCurrentThreadPointer);
    } catch (const Memory::AccessViolation&) {
        return;
    }

    // Preserve the hardware/source priority used by the existing GameCube
    // bring-up path. Each source owns a delivery latch until guest code ACKs it.
    TryDeliver(kInterruptAram, boundaryPc, ctx, interruptedContext,
               [] { return Memory::ClaimGameCubeAramInterrupt(); });
    TryDeliver(kInterruptDsp, boundaryPc, ctx, interruptedContext,
               [] { return Memory::ClaimGameCubeDspInterrupt(); });

    const int exiId = Memory::ClaimGameCubeExiInterrupt();
    if (exiId >= 0) {
        const uint32_t id = static_cast<uint32_t>(exiId);
        if (InstalledHandler(id) == 0u) {
            throw std::runtime_error("GameCube EXI interrupt has no installed guest handler");
        }
        DeliverInstalledInterrupt(id, boundaryPc, ctx, interruptedContext);
    }

    TryDeliver(kInterruptDi, boundaryPc, ctx, interruptedContext,
               [] { return Memory::ClaimGameCubeDiInterrupt(); });
    TryDeliver(kInterruptSi, boundaryPc, ctx, interruptedContext,
               [] { return Memory::ClaimGameCubeSiInterrupt(); });
    if (TryDeliver(kInterruptAid, boundaryPc, ctx, interruptedContext,
                   [] { return Memory::ClaimGameCubeAudioDmaInterrupt(); })) {
        // A title may opt into re-anchoring the next raw AID period only after
        // the translated ISR has fully returned.  This leaves DSPCR/status
        // ownership with guest code while preventing host callback latency from
        // turning one completion into an immediate IRQ storm.
        Memory::CompleteGameCubeAudioDmaInterruptDelivery();
    }
    TryDeliver(kInterruptVi, boundaryPc, ctx, interruptedContext,
               [] { return GameCube_ClaimViInterrupt(); });

    uint16_t token = 0u;
    if (InstalledHandler(kInterruptPeToken) != 0u && GX_HLE_ClaimPeTokenInterrupt(&token)) {
        (void)token;
        DeliverInstalledInterrupt(kInterruptPeToken, boundaryPc, ctx, interruptedContext);
    }
    if (InstalledHandler(kInterruptPeFinish) != 0u && GX_HLE_ClaimPeFinishInterrupt()) {
        DeliverInstalledInterrupt(kInterruptPeFinish, boundaryPc, ctx, interruptedContext);
    }
}

#else

void GameCube_ServiceExternalInterrupts(uint32_t, CpuContext*) {}

#endif
