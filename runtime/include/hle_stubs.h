#pragma once

#include "abi_bridge.h"

// hle/gx/gx_fatal_stubs.cpp includes nothing but this header and reaches
// std::fprintf / std::snprintf / std::abort through it.
#include <cstdio>
#include <cstdlib>

// VI Utils
struct ViGuestStateLayout {
    uint32_t initializedFlag = 0;
    uint32_t tvFormat = 0;
    // Some generic-DOL titles execute VIConfigure translated instead of through
    // the shared HLE entry. In that case the guest SDK's tvFormat word is the
    // authoritative source and the host VI clock must pull it before retraces
    // rather than overwrite it with the host's default format.
    bool tvFormatGuestAuthoritative = false;
    // Titles that leave VIConfigure/VIFlush translated can keep a staged TV
    // family separate from VIGetTvFormat's committed word. The translated
    // VIFlush arms publication; the shared HLE mirrors the missing interrupt-
    // time commit at the next retrace when these addresses are provided.
    uint32_t pendingTvFormat = 0;
    uint32_t tvFormatCommitArm = 0;
    // Title policy for PAL software that should run on the 60 Hz timing
    // family (30/60 presentation). The guest's PAL50 request remains intact
    // in its staged word; the committed VI family is promoted at the same
    // staged->active boundary as real PAL60/RGB60 hardware.
    bool forcePal60 = false;
    uint32_t forcedTvFormat = 5;
    // VI's committed timing-table pointer is published atomically with the TV
    // family by the retail interrupt. Keep the guest-visible pair coherent even
    // though the host derives its wall-clock interval from the TV family.
    uint32_t activeTiming = 0;
    uint32_t pendingTiming = 0;
    uint32_t viYOrigin = 0;
    // Translated VIConfigure paths can also own the SDK's adjusted display-Y.
    // Its low bit participates in VIGetNextField polarity, so the host needs the
    // same value when coupling semantic field state to the raw VI beam phase.
    bool viYOriginGuestAuthoritative = false;
    uint32_t renderWidth = 0;
    uint32_t renderHeight = 0;
    uint32_t xfbWidth = 0;
    uint32_t xfbHeight = 0;
    uint32_t retraceCount = 0;
    uint32_t timingGuard = 0;
    uint32_t preRetraceCallback = 0;
    uint32_t postRetraceCallback = 0;
    uint32_t nextFrameBuffer = 0;
    uint32_t nextFrameBufferHw = 0;
    bool nextFrameBufferGuestAuthoritative = false;
    uint32_t retraceQueue = 0;
    uint32_t eggSystem = 0;
    // Generic-DOL titles may opt into hardware-style VI-owned scanout. GX frame
    // completion then latches an immutable image, while each VI retrace presents
    // the currently latched image even when the producer has no new frame.
    bool presentAtRetrace = false;
};

struct OsSchedulerGuestStateLayout {
    uint32_t defaultThreadContext = 0;
    uint32_t idleThreadContext = 0;
    uint32_t threadQueueArray = 0;
    uint32_t switchThreadCallback = 0;
    uint32_t schedulerDisableCount = 0;
    uint32_t schedulerReschedFlag = 0;
    uint32_t schedulerPendingMask = 0;
};

struct OsThreadGuestConfig {
    uint32_t initContextFunction = 0;
    uint32_t exitThreadFunction = 0;
    uint32_t schedulerInitFlag = 0;
    uint32_t threadAttrSource = 0;
};

struct OsAlarmGuestStateLayout {
    // Offset from SDA1/r13 to the two-word OSAlarm queue head/tail pair.
    uint32_t queueOffsetFromR13 = 0;
    // Title's internal InsertAlarm entry. Periodic alarms are reinserted through
    // the exact guest routine so title-specific timebase/queue semantics remain
    // authoritative while the host supplies the missing decrementer interrupt.
    uint32_t insertAlarmFunction = 0;
};

struct AiGuestStateLayout {
    // SDK software-visible AI globals.  The hardware register block itself is
    // virtualized by the host audio backend; these four words are the state
    // that guest code can legitimately observe around AIInit/callback dispatch.
    uint32_t initializedFlag = 0;
    uint32_t callbackBusy = 0;
    uint32_t callbackStackSwitch = 0;
    uint32_t dmaCallback = 0;
    // Streaming titles re-arm every produced block. Keep hardware autoreload,
    // but render an unrefreshed submission as a host playback gap after a stall.
    bool suppressStaleDmaAudio = false;
};

struct DspGuestStateLayout {
    uint32_t initializedFlag = 0;
    uint32_t assertPending = 0;
    uint32_t assertTask = 0;
    uint32_t reservedState = 0;
    uint32_t currentTask = 0;
    uint32_t firstTask = 0;
    uint32_t runningTask = 0;
};

// VI's host timeline/render state is title-neutral, but the SDK's software-visible
// globals are linked at title-specific addresses. Generic-DOL title wrappers set
// this layout before invoking the shared VI HLE. A zero field means that title does
// not expose/use that mirror and therefore must not be written speculatively.
void VI_HLE_SetGuestStateLayout(const ViGuestStateLayout& layout);
// Reports whether the active title's PAL50 request is promoted to EURGB60.
// The raw VI register model uses the same promotion for beam/comparator timing.
bool VI_HLE_IsPal60Forced() noexcept;
void VI_HLE_GetTvFormat(CpuContext* ctx);
void VI_HLE_ForceRetrace(CpuContext* ctx);
void VI_HLE_PollRetrace(CpuContext* ctx);
void VI_HLE_ProcessRetracesDeferred(int maxToProcess);
void VI_HLE_WaitForNextRetracePoll();
// Service the asynchronous sources that can wake a retail SDK scheduler idle
// loop, then yield until the next VI polling deadline. Generic-DOL titles bind
// their translated idle-loop address to this helper in the title adapter.
void OS_HLE_ServiceIdleWait(CpuContext* ctx);
// Single owner of the Aurora frame presentation sequence (seal, optional pace
// to the VI retrace boundary, pre-warm the next frame). paceToRetrace is true
// for the GXCopyDisp producer path and false for retrace-context presents.
void VI_HLE_PresentFrame(bool presentedXfb, bool paceToRetrace);
bool VI_HLE_IsAdvancingRetrace();
// RVL OS thread/scheduler structures have stable field layouts but their linked
// globals are title-specific. Generic-DOL title wrappers configure these host
// HLE addresses before any scheduling operation; the built-in defaults remain
// the original MKW addresses for the non-generic MKW executable.
void OS_HLE_SetSchedulerGuestStateLayout(const OsSchedulerGuestStateLayout& layout);
OsSchedulerGuestStateLayout OS_HLE_GetSchedulerGuestStateLayout();
void OS_HLE_SetThreadGuestConfig(const OsThreadGuestConfig& config);
void OS_HLE_CreateThread(CpuContext* ctx);
void OS_HLE_ResumeThread(CpuContext* ctx);
void OS_HLE_SelectThread(CpuContext* ctx);
void OS_HLE_SleepThread(CpuContext* ctx);
void OS_HLE_WakeupThread(CpuContext* ctx);
void OS_HLE_SetAlarmGuestStateLayout(const OsAlarmGuestStateLayout& layout);
void AI_HLE_SetGuestStateLayout(const AiGuestStateLayout& layout);
void AI_HLE_Init(uint32_t callbackStackSwitch);
uint32_t AI_HLE_CheckInit();
uint32_t AI_HLE_RegisterDMACallback(uint32_t callback);
void AI_HLE_InitDMA(uint32_t startAddr, uint32_t length);
void AI_HLE_StartDMA();
void AI_HLE_StopDMA();
uint32_t AI_HLE_GetDMAEnableFlag();
uint32_t AI_HLE_GetDMABytesLeft();
uint32_t AI_HLE_GetDMAStartAddr();
uint32_t AI_HLE_GetDMALength();
void DSP_HLE_SetGuestStateLayout(const DspGuestStateLayout& layout);
void DSP_HLE_Init();
uint32_t DSP_HLE_CheckInit();
uint32_t DSP_HLE_AddTask(uint32_t taskPtr);
void DSP_HLE_SendMailToDSP(uint32_t mail);
uint32_t DSP_HLE_CheckMailToDSP();
uint32_t DSP_HLE_CheckMailFromDSP();
uint32_t DSP_HLE_ReadMailFromDSP();
uint32_t DSP_HLE_AssertTask(uint32_t taskPtr);
void VI_HLE_SetXfbReady(uint32_t xfbAddr); // Called by GXCopyDisp to signal EFB-to-XFB copy
// Reset the host-side GX command/FIFO decoder without running a title's SDK
// GXInit implementation. Generic-DOL title shims use this when they keep the
// guest SDK's software state but virtualize the CP/PI register programming.
void GX_HLE_ResetFifoParser();
// Start/finish FIFO recording against a title-linked GXFifoObj. This is used
// by generic-DOL title adapters whose translated GXBegin/EndDisplayList live at
// different SDK addresses than the shared native replacements.
void GX_HLE_BeginDisplayListRecordingForFifo(uint32_t listAddr, uint32_t sizeBytes,
                                             uint32_t fifoObjAddr);
void GX_HLE_EndDisplayListRecordingToFifo(uint32_t fifoObjAddr);
// Some titles issue very large indexed display-list workloads whose array snapshots
// exceed the host staging slice many times per retail frame. Opt them into the
// correctness-first interpreter, which expands indexed attributes to direct vertices.
void GX_HLE_SetPreferDeindexedDisplayLists(bool enabled);
// Enables the conservative sparse-index remap for titles whose retail renderer
// submits many small indexed display lists against large shared arrays. The
// optimization stays opt-in so other titles retain the established GX path.
void GX_HLE_SetSparseIndexedDisplayLists(bool enabled);
// Abort the host-side GX frame/FIFO work that corresponds to a hardware
// GXAbortFrame.  Unlike ResetFifoParser this preserves decoded VCD/VAT/array
// state and only discards the current frame plus transient FIFO packet state.
void GX_HLE_AbortFrameBackend();
// Reports whether the modeled Command Processor has reached an armed
// GXSetBreakPt boundary. This may drain raw FIFO commands into the host CP but
// does not wait for GPU/PE rendering completion.
bool GX_HLE_CpBreakpointHit();
// Claims one enabled, not-yet-delivered CP breakpoint interrupt. The hit stays
// asserted until the guest disables or re-arms the breakpoint; claiming only
// prevents duplicate host delivery of the same hardware event.
bool GX_HLE_ClaimCpBreakpointInterrupt();
// Claims one enabled, not-yet-delivered PE TOKEN interrupt raised by a
// PETOKENINT BP command. The hardware condition remains pending until the guest
// acknowledges PE control/status bit 2; claiming only prevents duplicate host
// delivery before that W1C acknowledgement.
bool GX_HLE_ClaimPeTokenInterrupt(uint16_t* token);
// Claims one enabled, not-yet-delivered PE FINISH interrupt raised by BP 0x45.
// The PE CSR pending bit remains asserted until the guest handler acknowledges
// status bit 3; claiming only prevents duplicate delivery of the same event.
bool GX_HLE_ClaimPeFinishInterrupt();
// Title-neutral DVD host services used by generic-DOL adapters. These build the
// extracted-disc index/published FST without touching any title's SDK globals,
// and service a DI-style read whose offset is expressed in four-byte words.
void DVD_HLE_EnsureDiscIndex();
bool DVD_HLE_ReadWordsToGuest(uint32_t buffer, uint32_t length, uint32_t wordOffset);
void Audio_HLE_Tick(CpuContext* ctx, uint32_t deltaMicros);
void Audio_HLE_Poll(CpuContext* ctx);
// Deferred twin of Audio_HLE_Poll for the long host waits that already service
// retraces and alarms (the VI retrace pacing loop, the Aurora frame-worker wait
// callback). Runs the AI DMA tick on an isolated register file the way
// OS_HLE_ProcessAlarmsDeferred does, so it is safe to call from the middle of an
// arbitrary translated function.
void Audio_HLE_PollDeferred();
bool OS_HLE_InterruptsEnabled() noexcept;
void OS_HLE_ApplyInterruptStateFromMsr(uint32_t msr) noexcept;
extern "C" void OS_HLE_ProcessAlarmsDeferred(int maxToProcess);
extern "C" void OS_HLE_BeginDeferredGuestCallbacks();
extern "C" void OS_HLE_EndDeferredGuestCallbacks();


// Defines and registers a faithful native reimplementation that REPLACES the translated function
// at a PPC address (not a stub; genuine not-yet-implemented entries live in hle/gx/gx_fatal_stubs.cpp
// and abort). The translator regex-parses these macro names to skip that address at build time, so
// renaming requires updating Translator.Cli/Program.cs, RuntimeNativeGuestEffectAnalyzer.cs,
// TranslatedBuildShardEmitter.cs and RuntimeNativeFunctionAbiProvider.cs together.

#if defined(MKW_GENERIC_DOL_BOOT)
// Do not emit the MKW-addressed `func_XXXXXXXX` replacement symbol either: the
// generic translated DOL owns that symbol when an address happens to collide.
#define PPC_NATIVE_OVERRIDE(addr_hex, name, ret_type, arg_list, call_list)
#define PPC_NATIVE_OVERRIDE_VOID(addr_hex, name, arg_list, call_list)
#else
#define PPC_NATIVE_OVERRIDE(addr_hex, name, ret_type, arg_list, call_list) \
    extern "C" ret_type func_##addr_hex arg_list { return name call_list; } \
    REGISTER_NATIVE_FUNCTION(0x##addr_hex, name)

#define PPC_NATIVE_OVERRIDE_VOID(addr_hex, name, arg_list, call_list) \
    extern "C" void func_##addr_hex arg_list { name call_list; } \
    REGISTER_NATIVE_FUNCTION(0x##addr_hex, name)
#endif
