#include "hle_stubs.h"
#include "memory.h"
#include "abi_bridge.h"
#include "guest_interrupt_context.h"
#include "ppc_runtime.h"
#include "aurora_events.h"
#include "settings_overlay.h"
#include "fiber_manager.h"
#include "platform/host_platform.h"
#include "runtime_log.h"
#include "recomp_mod_loader.h"
#include "practice_sweeper.h"

#include <dolphin/vi.h>
#include <dolphin/gx/GXAurora.h>
#include "gx_guest_write.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <thread>

#include <aurora/aurora.h>
#include <aurora/gfx.h>

// Forward declaration for OSWakeupThread - used to wake threads on VI retrace queue
extern "C" void OSWakeupThread_HLE_801aaaa4(CpuContext* ctx);

// Forward declaration for OSSleepThread - used by VIWaitForRetrace HLE
extern "C" void OSSleepThread_HLE_801aa9b8(CpuContext* cpu);
extern "C" int g_gxFrameCount;
extern "C" int32_t OS__DisableInterrupts_801a65ac();
extern "C" int32_t OS__RestoreInterrupts_801a65d4(int32_t level);

// Aurora frame cycle tracking - needs external linkage for the GX HLE
// (declared in gx_internal.h, consumed by gx_frame.cpp). We need to call
// aurora_begin_frame() before GX commands and aurora_end_frame() after.
std::atomic_bool g_auroraFrameActive{false};
std::atomic_bool g_auroraFrameHadWork{false};

static bool TryPrepareRamXfbPresentSource(uint32_t xfbAddr, uint32_t width, uint32_t height);

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

constexpr uint32_t kGuestGxRenderModeCopyBytes = 0x39;

bool ReadGuestRenderModeObj(uint32_t guestPtr, GXRenderModeObj& out) {
    if (guestPtr == 0 || !Memory::Contains(guestPtr, kGuestGxRenderModeCopyBytes)) {
        return false;
    }

    try {
        out.viTVmode = static_cast<VITVMode>(Memory::Read32(guestPtr + 0x00));
        out.fbWidth = Memory::Read16(guestPtr + 0x04);
        out.efbHeight = Memory::Read16(guestPtr + 0x06);
        out.xfbHeight = Memory::Read16(guestPtr + 0x08);
        out.viXOrigin = Memory::Read16(guestPtr + 0x0a);
        out.viYOrigin = Memory::Read16(guestPtr + 0x0c);
        out.viWidth = Memory::Read16(guestPtr + 0x0e);
        out.viHeight = Memory::Read16(guestPtr + 0x10);
        out.xFBmode = static_cast<VIXFBMode>(Memory::Read32(guestPtr + 0x14));
        out.field_rendering = Memory::Read8(guestPtr + 0x18);
        out.aa = Memory::Read8(guestPtr + 0x19);

        for (size_t i = 0; i < 12; ++i) {
            out.sample_pattern[i][0] = Memory::Read8(guestPtr + 0x1a + static_cast<uint32_t>(i * 2));
            out.sample_pattern[i][1] = Memory::Read8(guestPtr + 0x1a + static_cast<uint32_t>(i * 2) + 1);
        }
        for (size_t i = 0; i < 7; ++i) {
            out.vfilter[i] = Memory::Read8(guestPtr + 0x32 + static_cast<uint32_t>(i));
        }
    } catch (const Memory::AccessViolation&) {
        return false;
    }

    return true;
}

struct ViState {
    bool initialized = false;
    uint32_t tvFormat = 0; // VIGetTvFormat returns VI_NTSC before VIInit
    uint32_t nextFrameBuffer = 0;
    uint32_t currentFrameBuffer = 0;
    uint32_t retraceCount = 0;
    bool black = false;
    uint32_t preRetraceCallback = 0;
    uint32_t postRetraceCallback = 0;
    uint32_t renderWidth = 640;
    uint32_t renderHeight = 480;
    uint32_t viXOrigin = 0;
    uint32_t viYOrigin = 0;
    uint32_t xfbWidth = 640;
    uint32_t xfbHeight = 480;
    bool fieldOdd = false;
    Clock::time_point lastRetrace = Clock::now();
    std::chrono::microseconds retraceInterval{16666us}; // ~60 Hz
    bool hasValidXfb = false; // True once we've received at least one GXCopyDisp
    uint32_t readyXfb = 0;    // XFB address from the most recent GXCopyDisp

    // VIConfigure/VISetNextFrameBuffer/VISetBlack only write pending values below; VIFlush arms them but
    // the commit happens at the next retrace, matching real VI hardware. A VIFlush called from a
    // pre-retrace callback therefore misses the imminent field and lands one field late.

    // Pending values (written by VISetNextFrameBuffer, VISetBlack, VIConfigure)
    uint32_t pendingNextFrameBuffer = 0;
    bool pendingBlack = false;
    // Matches the active-state default (NTSC before VIInit) and the pending
    // 16666us interval below; a flush before any VIConfigure must not commit
    // PAL timing onto an NTSC-interval state.
    uint32_t pendingTvFormat = 0;
    uint32_t pendingRenderWidth = 640;
    uint32_t pendingRenderHeight = 480;
    uint32_t pendingViXOrigin = 0;
    uint32_t pendingViYOrigin = 0;
    uint32_t pendingXfbWidth = 640;
    uint32_t pendingXfbHeight = 480;
    std::chrono::microseconds pendingRetraceInterval{16666us};
    // Raw VI MMIO and high-level retrace callbacks must share one phase domain.
    // Consume this only on authoritative timing/polarity adoption, never on each
    // retrace, so comparator traversal remains continuous hardware state.
    bool beamPhaseRebasePending = true;
    
    // Set by VIFlush(); cleared after commit in AdvanceRetrace
    bool flushArmed = false;
};

std::mutex g_viMutex;
ViState g_vi;
std::atomic<bool> g_seenGxCopyDisp{false};
std::atomic<bool> g_presentAtRetrace{false};
std::atomic<bool> g_forcePal60Raw{false};


// MKW's guest-side VI globals remain the default layout for the normal title.
// Generic-DOL projects can replace this with their verified SDK-linked addresses.
ViGuestStateLayout g_viGuestLayout{
    .initializedFlag = 0x80386b38,
    .tvFormat = 0x80386ba8,
    .renderWidth = 0x80350864,
    .renderHeight = 0x80350866,
    .xfbWidth = 0x80350872,
    .xfbHeight = 0x8035087c,
    .retraceCount = 0x80386be4,
    .timingGuard = 0x80386b44,
    .preRetraceCallback = 0x80386bb8,
    .postRetraceCallback = 0x80386bb4,
    .nextFrameBuffer = 0x80386ba0,
    .nextFrameBufferHw = 0x80350890,
    .retraceQueue = 0x80386bc0,
    .eggSystem = 0x80386F60,
};

std::chrono::microseconds IntervalForFormat(uint32_t tvFormat) {
    // PAL50 is exactly 50 Hz. NTSC/EURGB60 use the 1000/1001 timing family
    // (~59.94 Hz), matching the raw VI model and the translated SDK/THP code
    // that uses 5994 (hundredths of Hz) for non-PAL50 cadence arithmetic.
    return tvFormat == 1 ? 20000us : 16683us;
}

bool IsPal50TimingFamily(uint32_t tvFormat) {
    return tvFormat == 1u;
}

uint32_t EffectiveGuestTvFormat(uint32_t tvFormat) {
    if (g_viGuestLayout.forcePal60 && IsPal50TimingFamily(tvFormat)) {
        return g_viGuestLayout.forcedTvFormat != 0u ? g_viGuestLayout.forcedTvFormat : 5u;
    }
    return tvFormat;
}

// Shared busy-wait budget for deadline-precise sleeps (matches Aurora's
// presenter spin window). Larger windows burn a core for no visible gain.
constexpr std::chrono::microseconds kFinalSpinWindow{500};

void SleepPreciselyUntil(Clock::time_point deadline, bool finishWithSpin = false,
                         std::chrono::microseconds spinWindow = 750us) {
    const auto now = Clock::now();
    if (now >= deadline) {
        return;
    }
    const auto timerDeadline =
        finishWithSpin && deadline - now > spinWindow ? deadline - spinWindow : deadline;
#if defined(_WIN32)
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
    struct HighResolutionTimer {
        HANDLE handle = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                               TIMER_MODIFY_STATE | SYNCHRONIZE);
        ~HighResolutionTimer() {
            if (handle != nullptr) {
                CloseHandle(handle);
            }
        }
    };
    static thread_local HighResolutionTimer timer;
    if (timer.handle != nullptr) {
        const auto remaining100ns =
            std::chrono::duration_cast<std::chrono::duration<int64_t, std::ratio<1, 10000000>>>(timerDeadline - now);
        LARGE_INTEGER due{};
        due.QuadPart = -std::max<int64_t>(remaining100ns.count(), 1);
        if (SetWaitableTimerEx(timer.handle, &due, 0, nullptr, nullptr, nullptr, 0) != FALSE) {
            WaitForSingleObject(timer.handle, INFINITE);
            if (!finishWithSpin) {
                return;
            }
        }
    }
#endif
    if (!finishWithSpin) {
        std::this_thread::sleep_until(deadline);
        return;
    }
    if (Clock::now() < timerDeadline) {
        std::this_thread::sleep_until(timerDeadline);
    }
    // This runs only for the final fraction of a VI interval. Do not yield the
    // host thread here: a scheduler quantum is larger than the remaining
    // budget and would recreate the 17-18 ms sawtooth this path removes.
    while (Clock::now() < deadline) {
#if defined(_WIN32)
        YieldProcessor();
#endif
    }
}

void ViSetR3(CpuContext* ctx, uint32_t value)
{
    if (ctx) {
        ctx->gpr[3] = value;
    }
}

void WriteGuestStateLocked() {
    try {
        if (g_viGuestLayout.initializedFlag) Memory::Write8(g_viGuestLayout.initializedFlag, 1);
        if (g_viGuestLayout.timingGuard) Memory::Write8(g_viGuestLayout.timingGuard, 1);
        if (g_viGuestLayout.tvFormat && !g_viGuestLayout.tvFormatGuestAuthoritative) {
            Memory::Write32(g_viGuestLayout.tvFormat, g_vi.tvFormat);
        }
        if (g_viGuestLayout.renderWidth) Memory::Write16(g_viGuestLayout.renderWidth, static_cast<uint16_t>(g_vi.renderWidth));
        if (g_viGuestLayout.renderHeight) Memory::Write16(g_viGuestLayout.renderHeight, static_cast<uint16_t>(g_vi.renderHeight));
        if (g_viGuestLayout.xfbWidth) Memory::Write16(g_viGuestLayout.xfbWidth, static_cast<uint16_t>(g_vi.xfbWidth));
        if (g_viGuestLayout.xfbHeight) Memory::Write16(g_viGuestLayout.xfbHeight, static_cast<uint16_t>(g_vi.xfbHeight));
        if (g_viGuestLayout.retraceCount) Memory::Write32(g_viGuestLayout.retraceCount, g_vi.retraceCount);
        if (g_viGuestLayout.preRetraceCallback) Memory::Write32(g_viGuestLayout.preRetraceCallback, g_vi.preRetraceCallback);
        if (g_viGuestLayout.postRetraceCallback) Memory::Write32(g_viGuestLayout.postRetraceCallback, g_vi.postRetraceCallback);
        // Write PENDING frame buffer to guest memory so SDK code sees the queued value
        if (g_viGuestLayout.nextFrameBuffer && !g_viGuestLayout.nextFrameBufferGuestAuthoritative) {
            Memory::Write32(g_viGuestLayout.nextFrameBuffer, g_vi.pendingNextFrameBuffer);
        }
        if (g_viGuestLayout.nextFrameBufferHw && !g_viGuestLayout.nextFrameBufferGuestAuthoritative) {
            Memory::Write32(g_viGuestLayout.nextFrameBufferHw, g_vi.pendingNextFrameBuffer);
        }
    } catch (const ::Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_VI, "WriteGuestStateLocked", e);
    }
}

void RefreshGuestRetraceCallbacksLocked() {
    try {
        // A translated VIConfigure updates the title's linked SDK state without
        // crossing the shared HLE VIConfigure entry. Titles that opt into this
        // mirror therefore make that guest word authoritative for host retrace
        // pacing too. This keeps PAL50/EURGB60 changes causal and title-driven.
        if (g_viGuestLayout.tvFormat && g_viGuestLayout.tvFormatGuestAuthoritative) {
            const uint32_t guestTvFormat = Memory::Read32(g_viGuestLayout.tvFormat);
            const uint32_t effectiveTvFormat = EffectiveGuestTvFormat(guestTvFormat);
            if (effectiveTvFormat != g_vi.tvFormat) {
                if (IsPal50TimingFamily(effectiveTvFormat) != IsPal50TimingFamily(g_vi.tvFormat)) {
                    g_vi.beamPhaseRebasePending = true;
                }
                g_vi.tvFormat = effectiveTvFormat;
                g_vi.pendingTvFormat = effectiveTvFormat;
                g_vi.retraceInterval = IntervalForFormat(effectiveTvFormat);
                g_vi.pendingRetraceInterval = g_vi.retraceInterval;
                if (effectiveTvFormat != guestTvFormat) {
                    Memory::Write32(g_viGuestLayout.tvFormat, effectiveTvFormat);
                }
                RT_LOG(RT_TAG_VI) << "guest VI tvFormat mirror changed to " << guestTvFormat
                                  << "; retrace interval="
                                  << g_vi.retraceInterval.count() << " us\n";
            }
        }


        if (g_viGuestLayout.viYOrigin && g_viGuestLayout.viYOriginGuestAuthoritative) {
            const uint32_t guestViYOrigin = Memory::Read16(g_viGuestLayout.viYOrigin);
            if (guestViYOrigin != g_vi.viYOrigin) {
                if (((guestViYOrigin ^ g_vi.viYOrigin) & 1u) != 0u) {
                    g_vi.beamPhaseRebasePending = true;
                }
                g_vi.viYOrigin = guestViYOrigin;
                g_vi.pendingViYOrigin = guestViYOrigin;
                RT_LOG(RT_TAG_VI) << "guest VI viYOrigin mirror changed to "
                                  << guestViYOrigin << "\n";
            }
        }

        if (g_viGuestLayout.nextFrameBuffer && g_viGuestLayout.nextFrameBufferGuestAuthoritative) {
            const uint32_t guestNextFb = Memory::Read32(g_viGuestLayout.nextFrameBuffer);
            if (guestNextFb != 0 && guestNextFb != g_vi.nextFrameBuffer) {
                g_vi.pendingNextFrameBuffer = guestNextFb;
                g_vi.nextFrameBuffer = guestNextFb;
                // A translated VISetNextFrameBuffer + VIFlush has already
                // published this SDK mirror. Commit it on this retrace just as
                // VI hardware would latch the next XFB.
                g_vi.currentFrameBuffer = guestNextFb;
            }
        }

        // Generic-DOL titles may execute their translated VISet*RetraceCallback
        // directly (static translated calls bypass the runtime registry/native
        // replacement). In that case the SDK guest globals are the authoritative
        // callback state. Pull them into the host VI mirror before each retrace so
        // the callback is dispatched instead of having WriteGuestStateLocked()
        // overwrite a retail-installed callback with a stale host value.
        if (g_viGuestLayout.preRetraceCallback) {
            g_vi.preRetraceCallback = Memory::Read32(g_viGuestLayout.preRetraceCallback);
        }
        if (g_viGuestLayout.postRetraceCallback) {
            g_vi.postRetraceCallback = Memory::Read32(g_viGuestLayout.postRetraceCallback);
        }
    } catch (const ::Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_VI, "RefreshGuestRetraceCallbacksLocked", e);
    }
}

void EnsureInitializedLocked() {
    if (g_vi.initialized) {
        return;
    }
    g_vi.initialized = true;
    g_vi.retraceInterval = IntervalForFormat(g_vi.tvFormat);
    g_vi.lastRetrace = Clock::now();
    WriteGuestStateLocked();
}

// GXRenderModeObj::viTVmode encodes the output family (0 NTSC, 1 PAL, 2 MPAL,
// 5 EURGB60) in bits [4:2].
uint32_t ExtractTvFormat(uint32_t tvMode) {
    return (tvMode >> 2) & 0x7;
}

// Re-entry guard to prevent AdvanceRetrace calling itself via OSWakeupThread -> SelectThread
static std::atomic<bool> s_inAdvanceRetrace{false};

// Set while VI_HLE_PresentFrame runs its seal/pace/pre-warm sequence. Guest callbacks
// serviced during that window (pace-loop alarms, GX timing polls) still see the stale
// hasValidXfb/g_auroraFrameActive flags; a retrace-context present fired from them would
// end the freshly pre-warmed empty frame and show it as a black frame group.
static std::atomic<bool> s_presentSequenceActive{false};
// Retrace number that was current when the most recent paced GX producer frame
// was sealed. This is host presentation bookkeeping only; it never changes the
// guest VI counter or pacing decisions.
static std::atomic<uint32_t> s_lastProducerRetraceCount{~0u};
// Last VI retrace slot already covered by a queued 30 -> 60 interpolation
// group. When a 30 Hz producer seals at retrace N, Aurora schedules its
// midpoint/native pair for N+1/N+2, so the repeat-scanout fallback must not
// enqueue a duplicate old latch for N+2. Host presentation bookkeeping only.
static std::atomic<uint32_t> s_interpolationCoveredThroughRetrace{0u};
// A retrace delivered from the middle of native GX work must not re-enter
// Aurora/window code. Coalesce its host scanout request here and consume it at
// the next safe presentation boundary after the interrupt-style callback path
// has unwound. Zero means no deferred scanout is pending.
void RequestViScanout(Clock::time_point retraceStamp, std::chrono::microseconds retraceInterval) {
    // Do not replay obsolete catch-up fields as a burst. Once the next VI period
    // has already started, the only hardware-faithful host action is to keep the
    // newest pending scanout slot.
    const auto now = Clock::now();
    if (now >= retraceStamp + retraceInterval) {
        return;
    }
    constexpr auto kNativePresentOffset = 6500us;
    const auto presentAt = retraceStamp + kNativePresentOffset;
    const uint64_t presentAtNanos = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(presentAt.time_since_epoch()).count());
    // This API is intentionally narrower than the rest of Aurora: it only
    // snapshots the already-immutable VI latch under a mutex and queues one
    // PresentationJob to the dedicated presenter thread. It performs no GX
    // work, frame begin/end, window event service, or guest callback. That makes
    // it safe to invoke from the deferred retrace path and preserves every VI
    // scanout slot instead of coalescing two PAL fields into the next GX frame.
    aurora_present_vi_scanout(presentAtNanos);
}

void RequestViRepeatScanout(Clock::time_point retraceStamp,
                            std::chrono::microseconds retraceInterval) {
    // Repeat at the retrace edge. Producer-owned jobs retain their existing
    // presentation phase; this path only fills a missing scanout interval and
    // never changes the producer schedule itself.
    const auto now = Clock::now();
    if (now >= retraceStamp + retraceInterval) {
        return;
    }
    const uint64_t presentAtNanos = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            retraceStamp.time_since_epoch()).count());
    aurora_present_vi_scanout(presentAtNanos);
}

void AdvanceRetrace(CpuContext* ctx, Clock::time_point retraceStamp, bool serviceAurora) {
    // Prevent re-entry - this can happen if OSWakeupThread triggers SelectThread
    // which goes idle and calls ProcessTimerEvents again
    if (s_inAdvanceRetrace.exchange(true)) {
        return;
    }
    
    uint32_t preCb = 0;
    uint32_t postCb = 0;
    uint32_t retraceValue = 0;
    bool hasXfbReady = false;
    uint32_t readyXfb = 0;
    uint32_t currentFb = 0;
    uint32_t xfbWidth = 0;
    uint32_t xfbHeight = 0;
    bool isBlack = false;
    bool producerUpdatedPreviousInterval = false;
    std::chrono::microseconds retraceInterval{};
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();

        // Callback setters are ordinary SDK software state, not VI hardware.
        // Keep the host dispatch mirror coherent with translated guest setters.
        RefreshGuestRetraceCallbacksLocked();

        // A translated VIConfigure/VIFlush pair can own a staged TV-format
        // word while this HLE replaces the retail VI interrupt that normally
        // publishes staged -> committed. Mirror only that missing publication
        // here, before retrace callbacks. Do not route it through flushArmed:
        // flushArmed also commits host-owned XFB/black/geometry state which the
        // translated guest path did not populate through this HLE.
        if (g_viGuestLayout.tvFormat && g_viGuestLayout.tvFormatGuestAuthoritative &&
            g_viGuestLayout.pendingTvFormat && g_viGuestLayout.tvFormatCommitArm) {
            try {
                const uint32_t commitArm = Memory::Read32(g_viGuestLayout.tvFormatCommitArm);
                if (commitArm != 0u) {
                    const uint32_t requestedTvFormat = Memory::Read32(g_viGuestLayout.pendingTvFormat);
                    const uint32_t pendingTvFormat = EffectiveGuestTvFormat(requestedTvFormat);
                    const uint32_t pendingTiming = g_viGuestLayout.pendingTiming
                        ? Memory::Read32(g_viGuestLayout.pendingTiming)
                        : 0u;
                    const uint32_t activeTiming = g_viGuestLayout.activeTiming
                        ? Memory::Read32(g_viGuestLayout.activeTiming)
                        : 0u;
                    const bool committedStateChanged =
                        pendingTvFormat != g_vi.tvFormat ||
                        (g_viGuestLayout.activeTiming && g_viGuestLayout.pendingTiming &&
                         pendingTiming != activeTiming);
                    if (IsPal50TimingFamily(pendingTvFormat) != IsPal50TimingFamily(g_vi.tvFormat)) {
                        g_vi.beamPhaseRebasePending = true;
                    }
                    g_vi.tvFormat = pendingTvFormat;
                    g_vi.pendingTvFormat = pendingTvFormat;
                    g_vi.retraceInterval = IntervalForFormat(pendingTvFormat);
                    g_vi.pendingRetraceInterval = g_vi.retraceInterval;

                    // Retail exposes the new family through VIGetTvFormat only
                    // after this commit and publishes the matching timing table
                    // in the same interrupt transaction, then clears the arm.
                    if (g_viGuestLayout.activeTiming && g_viGuestLayout.pendingTiming) {
                        Memory::Write32(g_viGuestLayout.activeTiming, pendingTiming);
                    }
                    Memory::Write32(g_viGuestLayout.tvFormat, pendingTvFormat);
                    Memory::Write32(g_viGuestLayout.tvFormatCommitArm, 0u);
                    if (committedStateChanged) {
                        RT_LOG(RT_TAG_VI) << "guest VI committed pending tvFormat="
                                          << pendingTvFormat << " (requested " << requestedTvFormat
                                          << "); retrace interval="
                                          << g_vi.retraceInterval.count()
                                          << " us; timing=0x" << std::hex << pendingTiming
                                          << std::dec << "\n";
                    }
                }
            } catch (const ::Memory::AccessViolation& e) {
                LogMemoryError(RT_TAG_VI, "AdvanceRetrace guest tvFormat commit", e);
            }
        }
        
        // Commit pending state if VIFlush armed it (see ViState).
        if (g_vi.flushArmed) {
            if (IsPal50TimingFamily(g_vi.pendingTvFormat) != IsPal50TimingFamily(g_vi.tvFormat) ||
                (((g_vi.pendingViYOrigin ^ g_vi.viYOrigin) & 1u) != 0u)) {
                g_vi.beamPhaseRebasePending = true;
            }
            // Commit pending -> active
            g_vi.nextFrameBuffer = g_vi.pendingNextFrameBuffer;
            g_vi.black = g_vi.pendingBlack;
            g_vi.tvFormat = g_vi.pendingTvFormat;
            g_vi.renderWidth = g_vi.pendingRenderWidth;
            g_vi.renderHeight = g_vi.pendingRenderHeight;
            g_vi.viXOrigin = g_vi.pendingViXOrigin;
            g_vi.viYOrigin = g_vi.pendingViYOrigin;
            g_vi.xfbWidth = g_vi.pendingXfbWidth;
            g_vi.xfbHeight = g_vi.pendingXfbHeight;
            g_vi.retraceInterval = g_vi.pendingRetraceInterval;
            
            // Clear flush armed flag
            g_vi.flushArmed = false;
            
        }
        
        g_vi.retraceCount++;
        g_vi.fieldOdd = !g_vi.fieldOdd;
        g_vi.currentFrameBuffer = g_vi.nextFrameBuffer;
        currentFb = g_vi.currentFrameBuffer;
        g_vi.lastRetrace = retraceStamp;
        if (g_vi.beamPhaseRebasePending) {
            const bool yOriginOdd = (g_vi.viYOrigin & 1u) != 0u;
            // HEADLESS guest semantics: VIGetNextField's semantic result equals
            // fieldOdd, while physical second-half selection XORs adjusted
            // display-Y parity. Keep that policy in VI, not generic Memory.
            const bool secondHalf = g_vi.fieldOdd != yOriginOdd;
            const uint64_t retraceMicros = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    retraceStamp.time_since_epoch()).count());
            Memory::RebaseViBeamToRetrace(retraceMicros, secondHalf);
            RT_LOG(RT_TAG_VI) << "raw beam phase rebase tvFormat=" << g_vi.tvFormat
                              << " viYOrigin=" << g_vi.viYOrigin
                              << " fieldOdd=" << (g_vi.fieldOdd ? 1 : 0)
                              << " secondHalf=" << (secondHalf ? 1 : 0)
                              << " retraceUs=" << retraceMicros << "\n";
            g_vi.beamPhaseRebasePending = false;
        }
        retraceValue = g_vi.retraceCount;
        producerUpdatedPreviousInterval =
            s_lastProducerRetraceCount.load(std::memory_order_acquire) ==
            (retraceValue - 1u);
        preCb = g_vi.preRetraceCallback;
        postCb = g_vi.postRetraceCallback;
        hasXfbReady = g_vi.hasValidXfb;
        readyXfb = g_vi.readyXfb;
        xfbWidth = g_vi.xfbWidth;
        xfbHeight = g_vi.xfbHeight;
        isBlack = g_vi.black;
        retraceInterval = g_vi.retraceInterval;
        WriteGuestStateLocked();
    }

    // Diagnostic product only. Publish the scripted sample immediately after
    // the guest-visible retrace count is committed, before any retrace waiter or
    // callback can sample KPAD/WPAD/BT input for this retrace.
    PracticeSweeper::OnRetrace(retraceValue);

    // Wake up threads sleeping on the VI retrace queue (VIWaitForRetrace).
    // The retrace count has been incremented and written to guest memory.
    if (ctx && g_viGuestLayout.retraceQueue) {
        ctx->gpr[3] = g_viGuestLayout.retraceQueue;
        OSWakeupThread_HLE_801aaaa4(ctx);
    }

    if (serviceAurora) {
        // Process window events (but don't present - that happens in GXCopyDisp)
        UpdateAuroraAndProcessEvents();

        // Start a new Aurora frame if one isn't already active
        if (!g_auroraFrameActive.load(std::memory_order_acquire)) {
            if (BeginAuroraFrame()) {
                g_auroraFrameActive.store(true, std::memory_order_release);
            }
        }
    }

    if (ctx) {
        ctx->gpr[3] = retraceValue;
        if (preCb) {
            InvokeIndirectCpu(preCb, ctx);
        }
        if (postCb) {
            // Guard: only invoke callback if sSystem is initialized
            // The callback dereferences sSystem which must be non-null
            const uint32_t sSystemPtr = g_viGuestLayout.eggSystem
                ? Memory::Read32(g_viGuestLayout.eggSystem)
                : 1;
            if (sSystemPtr != 0) {
                InvokeIndirectCpu(postCb, ctx);
            }
        }
    }

    // VISetBlack(TRUE) keeps frame submission running but shows only the clear color: GX render work is
    // skipped and Aurora's end_frame() clears to black, matching the hardware manual's "signal continues,
    // pixels go black" behavior. When not black, submission waits for hasXfbReady (GXCopyDisp done).
    const bool viOwnsScanout = g_presentAtRetrace.load(std::memory_order_acquire);
    if (serviceAurora && !s_presentSequenceActive.load(std::memory_order_acquire)) {
        const bool frameActive = g_auroraFrameActive.load(std::memory_order_acquire);
        const bool xfbMatches = (readyXfb != 0 && readyXfb == currentFb);
        const bool shouldPresentXfb = hasXfbReady && !isBlack && xfbMatches;
        const bool shouldPresentRamXfb = !g_seenGxCopyDisp.load(std::memory_order_acquire) &&
            !hasXfbReady && !isBlack && currentFb != 0 &&
            TryPrepareRamXfbPresentSource(currentFb,
                xfbWidth != 0 ? xfbWidth : 640u,
                xfbHeight != 0 ? xfbHeight : 480u);
        const bool shouldPresentBlack = isBlack && frameActive;
        const bool shouldSubmit = frameActive &&
            (viOwnsScanout ? (shouldPresentRamXfb || shouldPresentBlack)
                           : (shouldPresentXfb || shouldPresentRamXfb || shouldPresentBlack));
        if (shouldSubmit) {
            if (!isBlack || settings_overlay::StartupScreenVisible()) {
                // Normal presentation: draw overlay on top of GX content
                settings_overlay::Draw();
            }
            // Outside startup, VI black remains a pure black presentation.
            // Unpaced: this present already runs in retrace context.
            VI_HLE_PresentFrame(viOwnsScanout ? false : shouldPresentXfb, false);
        } else if (g_auroraFrameHadWork.load(std::memory_order_acquire) && !shouldPresentXfb && !isBlack) {
            // GX work is in progress but frame not complete - just poll window events
            // Don't call aurora_end_frame() as that would present incomplete work
            UpdateAuroraAndProcessEvents();
        }
    }

    // Real VI scans the currently latched XFB every retrace, regardless of how
    // often GX produces a new one. Aurora's scanout snapshot is immutable, so
    // presenting it again cannot race the producer or fabricate GX work. Even
    // a deferred guest interrupt may queue this narrow presenter-only job; it
    // still never enters GX, frame begin/end, window service, or guest callbacks.
    if (viOwnsScanout && (!isBlack || serviceAurora)) {
        RequestViScanout(retraceStamp, retraceInterval);
    } else if (!viOwnsScanout && !isBlack && !producerUpdatedPreviousInterval &&
               retraceValue > s_interpolationCoveredThroughRetrace.load(std::memory_order_acquire)) {
        // Real VI keeps scanning the last latched XFB even when the CPU spends a
        // retrace (or several) in a retail polling loop and GX produces nothing.
        // Repeat only the immutable host snapshot: no GX work, guest callback,
        // guest counter, scheduler state, or game-time state is advanced here.
        RequestViRepeatScanout(retraceStamp, retraceInterval);
    }

    // Clear re-entry guard
    s_inAdvanceRetrace.store(false);
}

bool AdvanceDueRetraces(CpuContext* ctx, int maxToProcess, bool serviceAurora)
{
    bool advancedAny = false;

    for (int catchUpCount = 0; catchUpCount < maxToProcess; ++catchUpCount) {
        Clock::time_point target;
        auto now = Clock::now();
        {
            std::lock_guard<std::mutex> lock(g_viMutex);
            if (!g_vi.initialized) {
                return advancedAny;
            }
            target = g_vi.lastRetrace + g_vi.retraceInterval;
            if (now < target) {
                return advancedAny;
            }
        }


        AdvanceRetrace(ctx, target, serviceAurora);
        advancedAny = true;
    }

    return advancedAny;
}

} // namespace

void VI_HLE_SetGuestStateLayout(const ViGuestStateLayout& layout) {
    std::lock_guard<std::mutex> lock(g_viMutex);
    g_viGuestLayout = layout;
    g_forcePal60Raw.store(layout.forcePal60, std::memory_order_release);
    g_presentAtRetrace.store(layout.presentAtRetrace, std::memory_order_release);
    aurora_set_vi_scanout_repeat_mode(!layout.presentAtRetrace);
    aurora_set_vi_scanout_mode(layout.presentAtRetrace);
    if (g_viGuestLayout.tvFormat && g_viGuestLayout.tvFormatGuestAuthoritative) {
        try {
            const uint32_t guestTvFormat = Memory::Read32(g_viGuestLayout.tvFormat);
            const uint32_t effectiveTvFormat = EffectiveGuestTvFormat(guestTvFormat);
            if (IsPal50TimingFamily(effectiveTvFormat) != IsPal50TimingFamily(g_vi.tvFormat)) {
                g_vi.beamPhaseRebasePending = true;
            }
            g_vi.tvFormat = effectiveTvFormat;
            g_vi.pendingTvFormat = effectiveTvFormat;
            g_vi.retraceInterval = IntervalForFormat(effectiveTvFormat);
            g_vi.pendingRetraceInterval = g_vi.retraceInterval;
            if (effectiveTvFormat != guestTvFormat) {
                Memory::Write32(g_viGuestLayout.tvFormat, effectiveTvFormat);
            }
        } catch (const ::Memory::AccessViolation& e) {
            LogMemoryError(RT_TAG_VI, "VI_HLE_SetGuestStateLayout tvFormat", e);
        }
    }
    if (g_viGuestLayout.pendingTvFormat) {
        try {
            const uint32_t guestPendingTvFormat = Memory::Read32(g_viGuestLayout.pendingTvFormat);
            g_vi.pendingTvFormat = EffectiveGuestTvFormat(guestPendingTvFormat);
            g_vi.pendingRetraceInterval = IntervalForFormat(g_vi.pendingTvFormat);
        } catch (const ::Memory::AccessViolation& e) {
            LogMemoryError(RT_TAG_VI, "VI_HLE_SetGuestStateLayout pendingTvFormat", e);
        }
    }
    if (g_viGuestLayout.viYOrigin && g_viGuestLayout.viYOriginGuestAuthoritative) {
        try {
            const uint32_t guestViYOrigin = Memory::Read16(g_viGuestLayout.viYOrigin);
            if (((guestViYOrigin ^ g_vi.viYOrigin) & 1u) != 0u) {
                g_vi.beamPhaseRebasePending = true;
            }
            g_vi.viYOrigin = guestViYOrigin;
            g_vi.pendingViYOrigin = guestViYOrigin;
        } catch (const ::Memory::AccessViolation& e) {
            LogMemoryError(RT_TAG_VI, "VI_HLE_SetGuestStateLayout viYOrigin", e);
        }
    }
    if ((g_viGuestLayout.tvFormat && g_viGuestLayout.tvFormatGuestAuthoritative) ||
        (g_viGuestLayout.viYOrigin && g_viGuestLayout.viYOriginGuestAuthoritative)) {
        // A newly bound translated SDK mirror is an authoritative phase-domain
        // adoption even when its current values happen to match our defaults.
        g_vi.beamPhaseRebasePending = true;
    }
    if (g_vi.initialized) {
        WriteGuestStateLocked();
    }
}

bool VI_HLE_IsPal60Forced() noexcept {
    return g_forcePal60Raw.load(std::memory_order_acquire);
}

void VI_HLE_GetTvFormat(CpuContext* ctx) {
    uint32_t tvFormat = 0;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        tvFormat = g_vi.tvFormat;
    }
    ViSetR3(ctx, tvFormat);
}

// Force one retrace boundary to pass, whether or not its wall-clock deadline
// has arrived, so the guest's retrace callbacks (AsyncDisplay's counters and
// friends) run. VI_HLE_PollRetrace below is the time-driven counterpart.
void VI_HLE_ForceRetrace(CpuContext* ctx) {
    Clock::time_point target;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        target = g_vi.lastRetrace + g_vi.retraceInterval;
    }
    AdvanceRetrace(ctx, target, true);
}

bool VI_HLE_IsAdvancingRetrace() {
    return s_inAdvanceRetrace.load(std::memory_order_acquire);
}

// Advance every retrace whose interval has already elapsed. Safe to call from
// busy loops (GX drawing, the scheduler's idle spin) to keep VBlank ticking.
void VI_HLE_PollRetrace(CpuContext* ctx) {
    AdvanceDueRetraces(ctx, 8, true);
}

void VI_HLE_ProcessRetracesDeferred(int maxToProcess) {
    if (maxToProcess <= 0 || !OS_HLE_InterruptsEnabled()) {
        return;
    }

    // This entry point runs from the middle of an arbitrary translated function
    // (GX__Begin's timing service, the host frame loop). Retrace callbacks are
    // an interrupt from that function's point of view, so they get a private
    // copy of its register file exactly as the hardware interrupt would.
    GuestInterruptCallbackContext interrupt;
    CpuContext* cpu = interrupt.get();

    // Host renderer ownership is not a guest critical section. Deliver due VI
    // callbacks at wall-clock cadence while suppressing thread switches and
    // all recursive Aurora/event work until the native GX call has unwound.
    OS_HLE_BeginDeferredGuestCallbacks();
    try {
        AdvanceDueRetraces(cpu, maxToProcess, false);
    } catch (...) {
        OS_HLE_EndDeferredGuestCallbacks();
        throw;
    }
    OS_HLE_EndDeferredGuestCallbacks();
}

void VI_HLE_ServiceDeferredPresentation() {
    // Deferred retraces deliberately run guest callbacks without Aurora/window
    // re-entry. Once that interrupt-style path has fully unwound, submit the
    // XFB selected by the retrace without advancing VI or invoking callbacks a
    // second time.
    if (s_inAdvanceRetrace.load(std::memory_order_acquire) ||
        s_presentSequenceActive.load(std::memory_order_acquire)) {
        return;
    }


    bool hasXfbReady = false;
    uint32_t readyXfb = 0;
    uint32_t currentFb = 0;
    bool isBlack = false;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        if (!g_vi.initialized) {
            return;
        }
        hasXfbReady = g_vi.hasValidXfb;
        readyXfb = g_vi.readyXfb;
        currentFb = g_vi.currentFrameBuffer;
        isBlack = g_vi.black;
    }

    const bool frameActive = g_auroraFrameActive.load(std::memory_order_acquire);
    const bool xfbMatches = readyXfb != 0 && readyXfb == currentFb;
    const bool shouldPresentXfb = hasXfbReady && !isBlack && xfbMatches;
    const bool shouldPresentBlack = isBlack && frameActive;
    if (!frameActive || (!shouldPresentXfb && !shouldPresentBlack)) {
        return;
    }

    if (!isBlack || settings_overlay::StartupScreenVisible()) {
        settings_overlay::Draw();
    }
    VI_HLE_PresentFrame(shouldPresentXfb, false);
}
void VI_HLE_WaitForNextRetracePoll() {
    Clock::time_point retraceDeadline;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        retraceDeadline = g_vi.lastRetrace + g_vi.retraceInterval;
    }

    const auto now = Clock::now();
    if (now >= retraceDeadline) {
        return;
    }
    // Audio DMA and alarm queues still need regular service even when the VI
    // deadline is farther away. The high-resolution wait removes the 1 ms
    // scheduler overshoot when retrace is the next event.
    const bool retraceIsNext = retraceDeadline <= now + 1ms;
    SleepPreciselyUntil(retraceIsNext ? retraceDeadline : now + 1ms, retraceIsNext,
                        kFinalSpinWindow);
}

namespace {

// Retrace count consumed by the most recent paced present. This is a slot memo
// against the VI timeline, not a second clock: it only answers "did a retrace
// already elapse while this frame was being produced?". Producer-thread only.
uint32_t s_lastPacedRetraceCount = ~0u;

// Wall-clock spacing between producer presents. Retrace deltas quantize a
// healthy ~33.3 ms producer to 1/2/3 depending on phase; this auxiliary host
// measurement lets target60 recognize the 30 Hz cadence without altering the
// guest VI clock or callbacks. Producer-thread only.
Clock::time_point s_lastProducerPresentTime{};
// Small hysteresis for target60 source-cadence classification. BT3 battle
// simulation is 30 Hz, but host delivery occasionally quantizes one source step
// short/long around the VI grid. A single jittery sample must not tear down the
// interpolation history; sustained native-60 delivery still exits immediately.
uint32_t s_target60CadenceConfidence = 0;
uint32_t s_target60NativeLikeStreak = 0;

// Last presentation anchor handed to Aurora, in nanoseconds on the VI retrace
// grid. Guarantees consecutive sealed frames never share an anchor (see the
// comment at the stamping site). Producer-thread only, like the memo above.
uint64_t s_lastPresentAnchorNanos = 0;
bool s_lastPresentAnchorWasThirtyToSixty = false;

// Sleeps to the same VI retrace boundary VIWaitForRetrace targets, servicing alarms every 1 ms so audio
// DMA and timers keep running, then delivers that retrace so guest logic starts exactly on the grid.
void PaceToRetraceBoundary(Clock::time_point deadline) {
    constexpr auto kServiceSlice = 1ms;
    for (;;) {
        const auto now = Clock::now();
        if (now >= deadline) {
            break;
        }
        if (deadline - now > kServiceSlice + kFinalSpinWindow) {
            SleepPreciselyUntil(now + kServiceSlice);
            OS_HLE_ProcessAlarmsDeferred(8);
            // Audio DMA is a 3 ms cadence and this wait is up to a full display
            // period long. Without a pump here the blocks that came due during
            // the wait are all delivered at once when the guest next reaches
            // idle, which the guest observes as AI slack jitter.
            Audio_HLE_PollDeferred();
            continue;
        }
        SleepPreciselyUntil(deadline, true, kFinalSpinWindow);
        break;
    }
    VI_HLE_ProcessRetracesDeferred(1);
}

} // namespace

// Single owner of the Aurora frame presentation sequence: seals the active frame, optionally paces the
// producer to the VI retrace boundary, and pre-warms the next frame. Paced from GXCopyDisp; unpaced for
// the retrace-context black/boot present path in AdvanceRetrace.
void VI_HLE_PresentFrame(bool presentedXfb, bool paceToRetrace) {
    if (s_presentSequenceActive.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    struct SequenceGuard {
        ~SequenceGuard() { s_presentSequenceActive.store(false, std::memory_order_release); }
    } sequenceGuard;
    Clock::time_point paceDeadline{};
    bool paceThisFrame = false;
    if (paceToRetrace) {
        const Clock::time_point producerPresentNow = Clock::now();
        uint64_t baseNanos = 0;
        uint64_t intervalNanos = 0;
        uint32_t retraceCount = 0;
        uint32_t retracesElapsed = 0;
        uint32_t interpolationFps = 0;
        bool thirtyToSixtyMode = false;
        bool interpolationCadenceEligible = false;
        {
            std::lock_guard<std::mutex> lock(g_viMutex);
            EnsureInitializedLocked();
            paceDeadline = g_vi.lastRetrace + g_vi.retraceInterval;
            retraceCount = g_vi.retraceCount;
            retracesElapsed = retraceCount - s_lastPacedRetraceCount;
            interpolationFps = aurora_get_frame_interpolation_fps();
            thirtyToSixtyMode = interpolationFps == 60;
            bool wallClockThirtyHz = false;
            bool native60Like = false;
            bool hardCadenceStall = false;
            if (thirtyToSixtyMode && s_lastProducerPresentTime != Clock::time_point{}) {
                const uint64_t producerDeltaNanos = static_cast<uint64_t>(
                    std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::nanoseconds>(
                                              producerPresentNow - s_lastProducerPresentTime)
                                              .count()));
                const uint64_t viNanos = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(g_vi.retraceInterval).count());
                // Healthy 30 Hz sits near 2*VI, but host scheduling can deliver
                // one source step anywhere from roughly 1.25 to 3.25 VI without
                // the guest simulation cadence changing. Native-60 UI remains
                // near 1 VI, while >=3.5 VI is treated as a real discontinuity.
                wallClockThirtyHz = viNanos != 0 &&
                                     producerDeltaNanos >= (viNanos * 5u) / 4u &&
                                     producerDeltaNanos < (viNanos * 13u) / 4u;
                native60Like = viNanos != 0 && producerDeltaNanos < (viNanos * 5u) / 4u;
                hardCadenceStall = viNanos != 0 && producerDeltaNanos >= (viNanos * 7u) / 2u;
            }
            if (thirtyToSixtyMode) {
                const bool directThirtyHz = retracesElapsed == 2 || wallClockThirtyHz;
                if (directThirtyHz) {
                    s_target60CadenceConfidence = std::min<uint32_t>(4u, s_target60CadenceConfidence + 2u);
                    s_target60NativeLikeStreak = 0;
                } else if (hardCadenceStall) {
                    s_target60CadenceConfidence = 0;
                    s_target60NativeLikeStreak = 0;
                } else if (native60Like) {
                    ++s_target60NativeLikeStreak;
                    if (s_target60NativeLikeStreak >= 2u) {
                        s_target60CadenceConfidence = 0;
                    } else if (s_target60CadenceConfidence != 0) {
                        --s_target60CadenceConfidence;
                    }
                } else if (s_target60CadenceConfidence != 0) {
                    --s_target60CadenceConfidence;
                }
                interpolationCadenceEligible =
                    directThirtyHz ||
                    (!hardCadenceStall && s_target60CadenceConfidence >= 2u &&
                     s_target60NativeLikeStreak < 2u);
            } else {
                s_target60CadenceConfidence = 0;
                s_target60NativeLikeStreak = 0;
                interpolationCadenceEligible = retracesElapsed <= 1;
            }
            // Publish the producer/retrace relationship under the same lock as
            // the count read. Otherwise AdvanceRetrace can increment the count
            // between the read and the atomic store and enqueue an unnecessary
            // repeat for a frame that was just produced.
            s_lastProducerRetraceCount.store(retraceCount, std::memory_order_release);
            // The cadence-gated 30 -> 60 group owns its midpoint/native slots,
            // plus one physical-VI grace slot. Aurora can slide a late group by
            // one VI; without the grace an old-frame repeat can be queued for
            // that same future slot before the delayed midpoint reaches the
            // presenter. Holding the last scanout for one extra retrace is
            // visually equivalent to that repeat and avoids the stale present.
            const uint32_t coveredThrough =
                s_interpolationCoveredThroughRetrace.load(std::memory_order_acquire);
            if (thirtyToSixtyMode && interpolationCadenceEligible) {
                s_interpolationCoveredThroughRetrace.store(
                    std::max(coveredThrough, retraceCount + 3u), std::memory_order_release);
            } else if (hardCadenceStall) {
                // A genuine long source gap has no pending midpoint group worth
                // protecting. Release stale grace immediately so VI can repeat
                // the last actually presented image rather than leaving a hole.
                s_interpolationCoveredThroughRetrace.store(0u, std::memory_order_release);
            } else if (retraceCount > coveredThrough) {
                // Expire old coverage only after its final scheduled retrace has
                // passed; an intervening ineligible producer frame must not reopen
                // a repeat slot already owned by an earlier interpolation group.
                s_interpolationCoveredThroughRetrace.store(0u, std::memory_order_release);
            }
            baseNanos = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    g_vi.lastRetrace.time_since_epoch())
                    .count());
            intervalNanos = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(g_vi.retraceInterval)
                    .count());
        }
        // Hold the frame to its boundary only when no retrace elapsed during
        // its production. A frame that missed its boundary presents
        // immediately: hardware would quantize down to the next vblank here,
        // but free-running late frames matches the previous pacer and keeps a
        // heavy scene at e.g. 50 fps instead of hard 30.
        paceThisFrame = retracesElapsed == 0;
        s_lastPacedRetraceCount = retraceCount;
        s_lastProducerPresentTime = producerPresentNow;
        // aurora_report_producer_paced needs a different signal than the pace-wait above. Normal
        // high-rate interpolation assumes a 60 Hz producer, where <=1 retrace elapsed is healthy.
        // The explicit 60 FPS mode is 30 -> 60: two retraces is the direct signal, while the
        // producer wall-clock window absorbs 1/3-retrace phase quantization around the same ~33 ms
        // cadence. Native 60 Hz UI and real stalls remain ineligible. This changes presentation
        // sampling only; guest simulation/VI callback timing is untouched.
        aurora_report_producer_paced(interpolationCadenceEligible);
        // Stamp the sealed frame's presentation schedule so Aurora paces interpolated slots against
        // this same VI timeline. Anchor to the NEXT retrace boundary, not the period just produced,
        // since slots anchored to the current period would already be expired by seal time. Encoding
        // overruns are corrected by sliding the whole slot group forward onto a later boundary of this
        // same grid, so this stays the single cadence authority. Anchors must also be strictly
        // monotonic. A target60 group owns two physical VI slots, so consecutive target60 first
        // anchors also stay two VI periods apart; otherwise a wall-clock-qualified 1-retrace sample
        // could place its midpoint on the previous group's native slot and burst-present.
        const uint64_t retraceIntervalNanos = intervalNanos;
        uint64_t anchorNanos = baseNanos + retraceIntervalNanos;
        if (s_lastPresentAnchorNanos != 0) {
            const bool currentThirtyToSixty = thirtyToSixtyMode && interpolationCadenceEligible;
            const uint64_t minimumSpacing =
                currentThirtyToSixty && s_lastPresentAnchorWasThirtyToSixty
                    ? retraceIntervalNanos * 2u
                    : retraceIntervalNanos;
            const uint64_t minimumAnchor = s_lastPresentAnchorNanos + minimumSpacing;
            if (anchorNanos < minimumAnchor) {
                anchorNanos = minimumAnchor;
            }
        }
        s_lastPresentAnchorNanos = anchorNanos;
        s_lastPresentAnchorWasThirtyToSixty =
            thirtyToSixtyMode && interpolationCadenceEligible;
        // A 30 Hz source frame spans two VI periods. Keep the first interpolated slot on the next
        // VI boundary, but subdivide the full two-retrace source interval so the midpoint and native
        // frame land 16.7 ms apart at 60 Hz. Native 60 Hz/menu frames keep the ordinary one-period
        // schedule and are cadence-gated out of interpolation above.
        const uint64_t presentationIntervalNanos =
            thirtyToSixtyMode && interpolationCadenceEligible
                ? retraceIntervalNanos * 2u
                : retraceIntervalNanos;
        aurora_set_present_schedule(anchorNanos, presentationIntervalNanos);
    } else {
        // Retrace-context presents (VI black, boot) have no display period of
        // their own to subdivide; present as soon as the frame is ready. The
        // schedule grid is gone, so the anchor cursor must not constrain the
        // next paced frame.
        s_lastPresentAnchorNanos = 0;
        s_lastPresentAnchorWasThirtyToSixty = false;
        s_lastProducerPresentTime = {};
        s_target60CadenceConfidence = 0;
        s_target60NativeLikeStreak = 0;
        s_interpolationCoveredThroughRetrace.store(0u, std::memory_order_release);
        aurora_set_present_schedule(0, 0);
    }

    aurora_end_frame();
    // Do not join the asynchronous frame worker here. Its cycle deliberately
    // waits for the producer to authorize preparation of the next frame before
    // reaching DONE, so a wait between end_frame() and that next begin would
    // deadlock. In VI-owned scanout mode the freshly sealed snapshot may become
    // the latch after this retrace; hardware likewise keeps scanning the prior
    // XFB until the new copy is ready, so the next retrace will pick it up.
    if (paceThisFrame) {
        PaceToRetraceBoundary(paceDeadline);
        std::lock_guard<std::mutex> lock(g_viMutex);
        s_lastPacedRetraceCount = g_vi.retraceCount;
    }
    settings_overlay::AdvancePresentedFrame();
    g_auroraFrameActive.store(false, std::memory_order_release);
    g_auroraFrameHadWork.store(false, std::memory_order_release);
    if (presentedXfb) {
        std::lock_guard<std::mutex> lock(g_viMutex);
        g_vi.hasValidXfb = false;
        g_vi.readyXfb = 0;
    }
    // Pre-warm the next frame so subsequent GX work has a valid frame context.
    {
        UpdateAuroraAndProcessEvents();
        if (BeginAuroraFrame()) {
            g_auroraFrameActive.store(true, std::memory_order_release);
        }
    }
}

static bool TryPrepareRamXfbPresentSource(uint32_t xfbAddr, uint32_t width, uint32_t height) {
    if (xfbAddr == 0 || width == 0 || height == 0 || (width & 1u) != 0) {
        return false;
    }
    const uint64_t byteSize64 = static_cast<uint64_t>(width) * height * 2u;
    if (byteSize64 > UINT32_MAX) {
        return false;
    }
    const uint32_t byteSize = static_cast<uint32_t>(byteSize64);

    static uint32_t s_lastRamXfbAddr = 0;
    static uint32_t s_lastRamXfbSize = 0;
    static uint64_t s_lastRamXfbGeneration = GxGuestWrite::kUntracked;
    const uint64_t generation = GxGuestWrite::GenerationForRange(xfbAddr, byteSize);
    const bool changed = xfbAddr != s_lastRamXfbAddr || byteSize != s_lastRamXfbSize ||
                         generation == GxGuestWrite::kUntracked || generation != s_lastRamXfbGeneration;
    if (!changed) {
        return false;
    }

    try {
        const void* data = Memory::GetPointer(xfbAddr, byteSize);
        if (!data) {
            return false;
        }
        AuroraSetRamXfbPresentSource(data, width, height, width);
        s_lastRamXfbAddr = xfbAddr;
        s_lastRamXfbSize = byteSize;
        s_lastRamXfbGeneration = generation;
        RT_LOGF(RT_TAG_VI, "presenting guest RAM XFB 0x%08X %ux%u generation=%llu\n",
                xfbAddr, width, height, static_cast<unsigned long long>(generation));
        return true;
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_VI, "RAM XFB present", e);
        return false;
    }
}

// -----------------------------------------------------------------------------
// VI_HLE_SetXfbReady - Called by GXCopyDisp to signal EFB->XFB copy completed.
// This marks that we now have valid framebuffer data to present.
// -----------------------------------------------------------------------------
void VI_HLE_SetXfbReady(uint32_t xfbAddr) {
    g_seenGxCopyDisp.store(true, std::memory_order_release);
    std::lock_guard<std::mutex> lock(g_viMutex);
    g_vi.hasValidXfb = true;
    g_vi.readyXfb = xfbAddr;
    if (g_vi.currentFrameBuffer == 0 && g_vi.nextFrameBuffer == 0) {
        g_vi.currentFrameBuffer = xfbAddr;
        g_vi.nextFrameBuffer = xfbAddr;
        g_vi.pendingNextFrameBuffer = xfbAddr;
        WriteGuestStateLocked();
    } else if (g_vi.nextFrameBuffer != xfbAddr) {
        g_vi.nextFrameBuffer = xfbAddr;
        g_vi.pendingNextFrameBuffer = xfbAddr;
    }
}

// VIInit (0x801B94A4) and its lower-level helper __VIInit (0x801B9294) both
// program MMIO at 0xCC0020xx on hardware. We skip all hardware access and seed
// the same defaults instead, so the two entry points share one body.
static void SeedViStateForInit(CpuContext* ctx, const char* who)
{
    RT_LOG(RT_TAG_VI) << who << " called: seeding VI state (HLE)" << std::endl;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
    }
    ViSetR3(ctx, 0);
}

extern "C" void VIInit_HLE_801b94a4(CpuContext* ctx)
{
    SeedViStateForInit(ctx, "VIInit_801b94a4");
}
PPC_NATIVE_OVERRIDE_VOID(801B94A4, VIInit_HLE_801b94a4, (CpuContext* ctx), (ctx));

extern "C" void __VIInit_HLE_801b9294(CpuContext* ctx)
{
    SeedViStateForInit(ctx, "__VIInit_801b9294");
}
PPC_NATIVE_OVERRIDE_VOID(801B9294, __VIInit_HLE_801b9294, (CpuContext* ctx), (ctx));

// -----------------------------------------------------------------------------
// Helper stubs referenced by VIInit switch cases (case D variants).
// These are hardware-specific; treat as no-ops to keep control flow intact.
// -----------------------------------------------------------------------------
extern "C" void VIInit_caseD_0_HLE_801b9934(CpuContext* ctx)
{
    (void)ctx;
    RT_LOG(RT_TAG_VI) << "VIInit_caseD_0_801b9934 stubbed" << std::endl;
}
PPC_NATIVE_OVERRIDE_VOID(801B9934, VIInit_caseD_0_HLE_801b9934, (CpuContext* ctx), (ctx));

extern "C" void VIInit_caseD_1_HLE_801b993c(CpuContext* ctx)
{
    (void)ctx;
    RT_LOG(RT_TAG_VI) << "VIInit_caseD_1_801b993c stubbed" << std::endl;
}
PPC_NATIVE_OVERRIDE_VOID(801B993C, VIInit_caseD_1_HLE_801b993c, (CpuContext* ctx), (ctx));

extern "C" void VIInit_caseD_2_HLE_801b9944(CpuContext* ctx)
{
    (void)ctx;
    RT_LOG(RT_TAG_VI) << "VIInit_caseD_2_801b9944 stubbed" << std::endl;
}
PPC_NATIVE_OVERRIDE_VOID(801B9944, VIInit_caseD_2_HLE_801b9944, (CpuContext* ctx), (ctx));

// -----------------------------------------------------------------------------
// VISetPreRetraceCallback (0x801B90F4)
// -----------------------------------------------------------------------------
extern "C" void VISetPreRetraceCallback_HLE_801b90f4(CpuContext* ctx)
{
    const uint32_t newCb = ctx ? ctx->gpr[3] : 0;
    uint32_t prev = 0;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        prev = g_vi.preRetraceCallback;
        g_vi.preRetraceCallback = newCb;
        WriteGuestStateLocked();
    }
    ViSetR3(ctx, prev);
}
PPC_NATIVE_OVERRIDE_VOID(801B90F4, VISetPreRetraceCallback_HLE_801b90f4, (CpuContext* ctx), (ctx));

// -----------------------------------------------------------------------------
// VISetPostRetraceCallback (0x801B9138)
// -----------------------------------------------------------------------------
extern "C" void VISetPostRetraceCallback_HLE_801b9138(CpuContext* ctx)
{
    const uint32_t newCb = ctx ? ctx->gpr[3] : 0;
    uint32_t prev = 0;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        prev = g_vi.postRetraceCallback;
        g_vi.postRetraceCallback = newCb;
        WriteGuestStateLocked();
    }
    ViSetR3(ctx, prev);
}
PPC_NATIVE_OVERRIDE_VOID(801B9138, VISetPostRetraceCallback_HLE_801b9138, (CpuContext* ctx), (ctx));

// -----------------------------------------------------------------------------
// VIGetDTVStatus (0x801BAD38)
// Reads DTV status from VI hardware (MMIO 0xCC00206E). Stub to "not ready".
// -----------------------------------------------------------------------------
extern "C" void VIGetDTVStatus_HLE_801bad38(CpuContext* ctx)
{
    ViSetR3(ctx, 0); // return 0 -> not ready / disabled
}
PPC_NATIVE_OVERRIDE_VOID(801BAD38, VIGetDTVStatus_HLE_801bad38, (CpuContext* ctx), (ctx));

// -----------------------------------------------------------------------------
// VIConfigure & related helpers: translate GXRenderModeObj into guest globals.
// -----------------------------------------------------------------------------
extern "C" void VIConfigure_HLE_801b9f6c(CpuContext* ctx)
{
    // One read of the guest GXRenderModeObj serves both the VI pending state and
    // aurora, rather than unpacking the same 0x39 bytes twice.
    const uint32_t renderModePtr = ctx ? ctx->gpr[3] : 0;
    GXRenderModeObj renderMode{};
    if (!ReadGuestRenderModeObj(renderModePtr, renderMode)) {
        RT_LOG(RT_TAG_VI) << "VIConfigure: invalid GXRenderModeObj pointer 0x"
                  << std::hex << renderModePtr << std::dec << std::endl;
        ViSetR3(ctx, 0);
        return;
    }

    uint32_t decodedTvFormat =
        ExtractTvFormat(static_cast<uint32_t>(renderMode.viTVmode));
    // PAL60 is a complete render-mode change. Converting only the retrace
    // clock leaves the PAL50 528-line geometry paired with a 60 Hz VI and can
    // make the scanout black. Use the title's EURGB60 family and its 480-line
    // framebuffer geometry together before staging the VI state and host copy.
    if (g_viGuestLayout.forcePal60 && IsPal50TimingFamily(decodedTvFormat)) {
        renderMode.viTVmode = static_cast<VITVMode>(
            (static_cast<uint32_t>(renderMode.viTVmode) & ~(0x7u << 2)) | (5u << 2));
        renderMode.efbHeight = 480;
        renderMode.xfbHeight = 480;
        renderMode.viYOrigin = 0;
        renderMode.viHeight = 480;
        decodedTvFormat = 5u;
        RT_LOG(RT_TAG_VI) << "VIConfigure: PAL50 mode promoted to EURGB60 640x480\n";
    }
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();

        // Write to PENDING state - will be committed on next retrace after VIFlush
        g_vi.pendingTvFormat = EffectiveGuestTvFormat(decodedTvFormat);
        g_vi.pendingRetraceInterval = IntervalForFormat(g_vi.pendingTvFormat);
        g_vi.pendingRenderWidth =
            renderMode.viWidth != 0 ? renderMode.viWidth : renderMode.fbWidth;
        g_vi.pendingRenderHeight =
            renderMode.viHeight != 0 ? renderMode.viHeight : renderMode.xfbHeight;
        g_vi.pendingViXOrigin = renderMode.viXOrigin;
        g_vi.pendingViYOrigin = renderMode.viYOrigin;
        g_vi.pendingXfbWidth = renderMode.fbWidth;
        g_vi.pendingXfbHeight =
            renderMode.xfbHeight != 0 ? renderMode.xfbHeight : renderMode.efbHeight;
    }

    ::VIConfigure(&renderMode);

    ViSetR3(ctx, 0);
}
PPC_NATIVE_OVERRIDE_VOID(801B9F6C, VIConfigure_HLE_801b9f6c, (CpuContext* ctx), (ctx));

extern "C" void VIFlush_HLE_801ba9a4(CpuContext* ctx)
{
    uint32_t guestNextFb = 0;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();

        if (g_vi.pendingNextFrameBuffer == 0) {
            try {
                guestNextFb = g_viGuestLayout.nextFrameBuffer
                    ? Memory::Read32(g_viGuestLayout.nextFrameBuffer)
                    : 0;
            } catch (const Memory::AccessViolation&) {
                guestNextFb = 0;
            }
            if (guestNextFb == 0) {
                try {
                    guestNextFb = g_viGuestLayout.nextFrameBufferHw
                        ? Memory::Read32(g_viGuestLayout.nextFrameBufferHw)
                        : 0;
                } catch (const Memory::AccessViolation&) {
                    guestNextFb = 0;
                }
            }
            if (guestNextFb != 0) {
                g_vi.pendingNextFrameBuffer = guestNextFb;
            }
        }
        
        // Arm only; the commit happens at the next retrace (see ViState).
        g_vi.flushArmed = true;
    }

    // NOTE: We do NOT set hasValidXfb here. Frame readiness is signaled ONLY by
    // GXCopyDisp (which sets hasValidXfb = true), as that's when the EFB->XFB
    // copy is complete and we have a valid frame to present.

    ViSetR3(ctx, 0);
}
PPC_NATIVE_OVERRIDE_VOID(801BA9A4, VIFlush_HLE_801ba9a4, (CpuContext* ctx), (ctx));

extern "C" void VISetNextFrameBuffer_HLE_801baab8(CpuContext* ctx)
{
    const uint32_t fbPtr = ctx ? ctx->gpr[3] : 0;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        // Write to PENDING state - will be committed on next retrace after VIFlush
        g_vi.pendingNextFrameBuffer = fbPtr;
        // Also update guest memory for SDK code that reads this directly
        WriteGuestStateLocked();
    }
    ViSetR3(ctx, 0);
}
PPC_NATIVE_OVERRIDE_VOID(801BAAB8, VISetNextFrameBuffer_HLE_801baab8, (CpuContext* ctx), (ctx));

extern "C" void VIGetNextFrameBuffer_HLE_801bab24(CpuContext* ctx)
{
    uint32_t fb = 0;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        // Return PENDING value - what was set by VISetNextFrameBuffer
        fb = g_vi.pendingNextFrameBuffer;
    }
    ViSetR3(ctx, fb);
    VI_HLE_PollRetrace(ctx);
}
PPC_NATIVE_OVERRIDE_VOID(801BAB24, VIGetNextFrameBuffer_HLE_801bab24, (CpuContext* ctx), (ctx));

extern "C" void VISetBlack_HLE_801bab2c(CpuContext* ctx)
{
    const bool makeBlack = ctx ? (ctx->gpr[3] != 0) : false;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        // Write to PENDING state - will be committed on next retrace after VIFlush
        g_vi.pendingBlack = makeBlack;
    }
    ViSetR3(ctx, 0);
}
PPC_NATIVE_OVERRIDE_VOID(801BAB2C, VISetBlack_HLE_801bab2c, (CpuContext* ctx), (ctx));

extern "C" void VIGetRetraceCount_HLE_801baba4(CpuContext* ctx)
{
    uint32_t count = 0;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        count = g_vi.retraceCount;
    }
    ViSetR3(ctx, count);
    VI_HLE_PollRetrace(ctx);
}
PPC_NATIVE_OVERRIDE_VOID(801BABA4, VIGetRetraceCount_HLE_801baba4, (CpuContext* ctx), (ctx));

extern "C" void VIGetNextField_HLE_801babac(CpuContext* ctx)
{
    bool fieldOdd = false;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        fieldOdd = g_vi.fieldOdd;
    }
    ViSetR3(ctx, fieldOdd ? 1 : 0);
    VI_HLE_PollRetrace(ctx);
}
PPC_NATIVE_OVERRIDE_VOID(801BABAC, VIGetNextField_HLE_801babac, (CpuContext* ctx), (ctx));

extern "C" void VIGetCurrentLine_HLE_801bac48(CpuContext* ctx)
{
    uint32_t height = 480;
    std::chrono::microseconds interval{16666us};
    Clock::time_point last;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        height = g_vi.xfbHeight;
        interval = g_vi.retraceInterval;
        last = g_vi.lastRetrace;
    }
    const auto now = Clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - last);
    uint32_t line = 0;
    if (interval.count() > 0 && height > 0) {
        const uint64_t scaled = static_cast<uint64_t>(elapsed.count()) * height;
        line = static_cast<uint32_t>(std::min<uint64_t>(height - 1, scaled / interval.count()));
    }
    ViSetR3(ctx, line);
}
PPC_NATIVE_OVERRIDE_VOID(801BAC48, VIGetCurrentLine_HLE_801bac48, (CpuContext* ctx), (ctx));

extern "C" void VIWaitForRetrace_HLE_801b99ec(CpuContext* ctx)
{
    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    
    if (Fiber::GuestFiberManager::IsInitialized()) {
        const int32_t irqState = OS__DisableInterrupts_801a65ac();
        uint32_t retraceCount = 0;
        {
            std::lock_guard<std::mutex> lock(g_viMutex);
            EnsureInitializedLocked();
            retraceCount = g_vi.retraceCount;
        }

        do {
            if (g_viGuestLayout.retraceQueue) {
                cpu->gpr[3] = g_viGuestLayout.retraceQueue;
                OSSleepThread_HLE_801aa9b8(cpu);
            } else {
                VI_HLE_WaitForNextRetracePoll();
                VI_HLE_PollRetrace(cpu);
            }

            {
                std::lock_guard<std::mutex> lock(g_viMutex);
                EnsureInitializedLocked();
                if (g_vi.retraceCount != retraceCount) {
                    break;
                }
            }
        } while (true);

        OS__RestoreInterrupts_801a65d4(irqState);
    } else {
        std::chrono::microseconds interval{16666us};
        Clock::time_point target;
        {
            std::lock_guard<std::mutex> lock(g_viMutex);
            EnsureInitializedLocked();
            interval = g_vi.retraceInterval;
            target = g_vi.lastRetrace + interval;
        }

        const auto now = Clock::now();
        if (now < target) {
            SleepPreciselyUntil(target, true);
        }
        AdvanceRetrace(cpu, target, true);
    }
    ViSetR3(cpu, 0);
}
PPC_NATIVE_OVERRIDE_VOID(801B99EC, VIWaitForRetrace_HLE_801b99ec, (CpuContext* ctx), (ctx));
