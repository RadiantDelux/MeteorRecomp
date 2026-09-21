#include "abi_bridge.h"
#include "guest_interrupt_context.h"
#include "ppc_runtime.h"
#include "hle_stubs.h"
#include "memory.h"
#include "memory_access.h"
#include "runtime_log.h"
#include "loop_service_cadence.h"
#include "host_stall_trace.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <thread>

extern "C" void Meteor_BtUsbPump(CpuContext* cpu);
extern "C" void Meteor_BtUsbObserveGuestHeapFree(uint32_t pointer, uint32_t callerLr);
extern "C" void Meteor_BtUsbObserveCallBoundary(uint32_t target, uint32_t callerLr,
                                                  uint32_t previousTarget, uint32_t previousLr);
extern "C" void Meteor_BtUsbObservePostHeapFree(uint32_t callerLr);
extern "C" void Meteor_BtUsbObserveMemset(uint32_t destination, uint32_t value,
                                            uint32_t length, uint32_t callerLr);
extern "C" void Meteor_BtUsbObserveHeapAllocCall(uint32_t heapIndex, uint32_t size,
                                                  uint32_t alignment, uint32_t callerLr);
extern "C" void Meteor_BtUsbObserveHeapAllocReturn(uint32_t result, uint32_t callerLr);
extern "C" int g_gxFrameCount;

namespace {

bool MeteorRuntimeTraceEnabledLocal() noexcept {
    static const bool enabled = std::getenv("METEOR_TRACE_RUNTIME") != nullptr;
    return enabled;
}

std::atomic<bool> g_meteorPeFinishDelivered{false};
std::atomic<uint32_t> g_meteorLastCallTarget{0u};
std::atomic<uint32_t> g_meteorLastCallLr{0u};
std::atomic<uint64_t> g_meteorCallSerial{0u};
std::atomic<uint32_t> g_meteorLastLoopPc{0u};
std::atomic<uint32_t> g_meteorLastLoopLr{0u};
std::atomic<uint64_t> g_meteorLoopSerial{0u};
// Translated guest OSThreads are cooperative host fibers and execute on one host
// thread at a time.  Keep the hot backedge cadence counter on that execution
// thread so millions of decoder-loop checkpoints do not perform a contended
// atomic RMW merely for host diagnostics.  The public atomics above are sparse
// watchdog snapshots only; every local tick still reaches the cadence gate.
thread_local uint64_t g_meteorLocalLoopSerial = 0u;
thread_local uint64_t g_meteorArmedLoopSerial = 0u;
thread_local uint32_t g_meteorArmedLoopPc = 0u;
thread_local meteor::LoopServiceCadence g_meteorLoopServiceCadence;
std::atomic<uint64_t> g_meteorCount8015A800{0u};
std::atomic<uint64_t> g_meteorCount8015A06C{0u};
std::atomic<uint64_t> g_meteorCount80257520{0u};
std::atomic<uint64_t> g_meteorCount8025797C{0u};
std::atomic<uint64_t> g_meteorCount80257810{0u};
std::atomic<uint64_t> g_meteorCount802AB688{0u};
std::atomic<uint64_t> g_meteorCount80210990{0u};
std::atomic<uint64_t> g_meteorCount8020635C{0u};
std::atomic<uint64_t> g_meteorCount802061C8{0u};
std::atomic<uint64_t> g_meteorCount802065CC{0u};
std::atomic<uint64_t> g_meteorCount8029EFA8{0u};
std::atomic<uint64_t> g_meteorCount8029EFD0{0u};
std::atomic<uint64_t> g_meteorCount802A1914{0u};
std::atomic<uint64_t> g_meteorCount802A15DC{0u};
std::atomic<uint64_t> g_meteorCount802AA640{0u};
std::atomic<uint64_t> g_meteorCount802AA4E4{0u};
std::atomic<uint32_t> g_meteorSchedulerCurrentThread{0u};
std::atomic<uint64_t> g_meteorSchedulerThreadChanges{0u};
std::array<std::atomic<uint64_t>, 10> g_meteor80257520ByLr{};
std::array<std::atomic<uint64_t>, 3> g_meteor80206840ByLr{};
std::array<std::atomic<uint64_t>, 14> g_meteor802065CCByLr{};
std::once_flag g_meteorCallWatchdogOnce;

// THP producer latency telemetry.  The opening movie has 2342 frames; a 4096
// slot table keeps one steady-clock timeline per guest tag without allocation,
// locks, queue mutation, or per-frame logging.  Ghidra HEADLESS proves the tag
// is written to work+4 before the stream worker publishes the same work object
// through the p8 -> p12 -> p20 decoder pipeline.
constexpr uint32_t kThpLatencyInvalidTag = 0xFFFFFFFFu;
constexpr size_t kThpLatencySlotCount = 4096u;

struct ThpLatencySlot {
    std::atomic<uint32_t> tag{kThpLatencyInvalidTag};
    std::atomic<uint64_t> streamPublishUs{0u};
    std::atomic<uint64_t> decoderInputPublishUs{0u};
    std::atomic<uint64_t> decodeBeginUs{0u};
    std::atomic<uint64_t> decodeEndUs{0u};
    std::atomic<uint64_t> readyPublishUs{0u};
};

struct ThpLatencyAggregate {
    std::atomic<uint64_t> count{0u};
    std::atomic<uint64_t> sumUs{0u};
    std::atomic<uint64_t> maxUs{0u};
};

struct ThpLatencySnapshot {
    uint64_t streamPublishUs = 0u;
    uint64_t decoderInputPublishUs = 0u;
    uint64_t decodeBeginUs = 0u;
    uint64_t decodeEndUs = 0u;
    uint64_t readyPublishUs = 0u;
};

// Low-overhead causal profiling for the translated THP decoder.  This is kept
// entirely in host thread-local state so profiling itself does not add atomics
// to the ~10k translated helper calls made by every movie frame.  Timings are
// inclusive of the title call/return hooks for each tracked translated helper;
// asynchronous service/reschedule timings are also accumulated separately so
// wall time can be split into decoder work vs runtime scheduling/service.
enum class ThpPerfBucket : uint8_t {
    None = 0,
    Transform,
    Entropy,
    LockedCacheDma,
    LockedCacheBusy,
};

constexpr bool kEnableThpPerfProfile = false;

struct ThpPerfFrame {
    bool active = false;
    uint64_t wallBeginNs = 0u;
    ThpPerfBucket activeBucket = ThpPerfBucket::None;
    uint64_t bucketBeginNs = 0u;
    uint64_t transformNs = 0u;
    uint64_t entropyNs = 0u;
    uint64_t lockedCacheDmaNs = 0u;
    uint64_t lockedCacheBusyNs = 0u;
    uint64_t peServiceNs = 0u;
    uint64_t asyncServiceNs = 0u;
    uint64_t rescheduleNs = 0u;
    uint64_t rescheduleTransformNs = 0u;
    uint64_t rescheduleEntropyNs = 0u;
    uint64_t rescheduleLockedCacheNs = 0u;
    uint32_t transformCalls = 0u;
    uint32_t entropyCalls = 0u;
    uint32_t lockedCacheDmaCalls = 0u;
    uint32_t lockedCacheBusyCalls = 0u;
    uint32_t rescheduleCalls = 0u;
};

struct ThpPerfAggregate {
    uint64_t frames = 0u;
    uint64_t wallNs = 0u;
    uint64_t wallMaxNs = 0u;
    uint64_t transformNs = 0u;
    uint64_t entropyNs = 0u;
    uint64_t lockedCacheDmaNs = 0u;
    uint64_t lockedCacheBusyNs = 0u;
    uint64_t peServiceNs = 0u;
    uint64_t asyncServiceNs = 0u;
    uint64_t rescheduleNs = 0u;
    uint64_t rescheduleTransformNs = 0u;
    uint64_t rescheduleEntropyNs = 0u;
    uint64_t rescheduleLockedCacheNs = 0u;
    uint64_t transformCalls = 0u;
    uint64_t entropyCalls = 0u;
    uint64_t lockedCacheDmaCalls = 0u;
    uint64_t lockedCacheBusyCalls = 0u;
    uint64_t rescheduleCalls = 0u;
};

thread_local ThpPerfFrame g_thpPerfFrame{};
thread_local ThpPerfAggregate g_thpPerfAggregate{};

std::array<ThpLatencySlot, kThpLatencySlotCount> g_thpLatencySlots{};
ThpLatencyAggregate g_thpStreamToP12{};
ThpLatencyAggregate g_thpP12ToDecode{};
ThpLatencyAggregate g_thpStreamToDecode{};
ThpLatencyAggregate g_thpDecodeWork{};
ThpLatencyAggregate g_thpDecodeTail{};
ThpLatencyAggregate g_thpStreamToReady{};
std::atomic<uint64_t> g_thpLatencyReadyCount{0u};

uint64_t MeteorSteadyMicros() noexcept {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

uint64_t MeteorSteadyNanos() noexcept {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

ThpPerfBucket ThpPerfBucketForTarget(uint32_t target) noexcept {
    switch (target) {
    case 0x80308618u:
    case 0x80308AA4u:
        return ThpPerfBucket::Transform;
    case 0x803093E4u:
    case 0x80309A40u:
    case 0x8030A0C8u:
        return ThpPerfBucket::Entropy;
    case 0x802097B4u:
        return ThpPerfBucket::LockedCacheDma;
    case 0x80209854u:
        return ThpPerfBucket::LockedCacheBusy;
    default:
        return ThpPerfBucket::None;
    }
}

void ThpPerfBeginTrackedCall(uint32_t target) noexcept {
    ThpPerfFrame& frame = g_thpPerfFrame;
    if (!frame.active) {
        return;
    }
    const ThpPerfBucket bucket = ThpPerfBucketForTarget(target);
    if (bucket == ThpPerfBucket::None) {
        return;
    }
    // The tracked decoder helpers do not recursively call one another.  If that
    // ever changes, leave the outer interval intact rather than corrupting it.
    if (frame.activeBucket != ThpPerfBucket::None) {
        return;
    }
    frame.activeBucket = bucket;
    frame.bucketBeginNs = MeteorSteadyNanos();
    switch (bucket) {
    case ThpPerfBucket::Transform: ++frame.transformCalls; break;
    case ThpPerfBucket::Entropy: ++frame.entropyCalls; break;
    case ThpPerfBucket::LockedCacheDma: ++frame.lockedCacheDmaCalls; break;
    case ThpPerfBucket::LockedCacheBusy: ++frame.lockedCacheBusyCalls; break;
    default: break;
    }
}

void ThpPerfEndTrackedCall(uint32_t target) noexcept {
    ThpPerfFrame& frame = g_thpPerfFrame;
    const ThpPerfBucket bucket = ThpPerfBucketForTarget(target);
    if (!frame.active || bucket == ThpPerfBucket::None || frame.activeBucket != bucket ||
        frame.bucketBeginNs == 0u) {
        return;
    }
    const uint64_t elapsedNs = MeteorSteadyNanos() - frame.bucketBeginNs;
    switch (bucket) {
    case ThpPerfBucket::Transform: frame.transformNs += elapsedNs; break;
    case ThpPerfBucket::Entropy: frame.entropyNs += elapsedNs; break;
    case ThpPerfBucket::LockedCacheDma: frame.lockedCacheDmaNs += elapsedNs; break;
    case ThpPerfBucket::LockedCacheBusy: frame.lockedCacheBusyNs += elapsedNs; break;
    default: break;
    }
    frame.activeBucket = ThpPerfBucket::None;
    frame.bucketBeginNs = 0u;
}

void ThpPerfRecordReschedule(uint64_t elapsedNs) noexcept {
    ThpPerfFrame& frame = g_thpPerfFrame;
    if (!frame.active || elapsedNs == 0u) {
        return;
    }
    frame.rescheduleNs += elapsedNs;
    ++frame.rescheduleCalls;
    switch (frame.activeBucket) {
    case ThpPerfBucket::Transform: frame.rescheduleTransformNs += elapsedNs; break;
    case ThpPerfBucket::Entropy: frame.rescheduleEntropyNs += elapsedNs; break;
    case ThpPerfBucket::LockedCacheDma:
    case ThpPerfBucket::LockedCacheBusy:
        frame.rescheduleLockedCacheNs += elapsedNs;
        break;
    default: break;
    }
}

void ThpPerfFinishFrame() noexcept {
    ThpPerfFrame& frame = g_thpPerfFrame;
    if (!frame.active || frame.wallBeginNs == 0u) {
        frame = {};
        return;
    }

    const uint64_t wallNs = MeteorSteadyNanos() - frame.wallBeginNs;
    ThpPerfAggregate& aggregate = g_thpPerfAggregate;
    ++aggregate.frames;
    aggregate.wallNs += wallNs;
    aggregate.wallMaxNs = std::max(aggregate.wallMaxNs, wallNs);
    aggregate.transformNs += frame.transformNs;
    aggregate.entropyNs += frame.entropyNs;
    aggregate.lockedCacheDmaNs += frame.lockedCacheDmaNs;
    aggregate.lockedCacheBusyNs += frame.lockedCacheBusyNs;
    aggregate.peServiceNs += frame.peServiceNs;
    aggregate.asyncServiceNs += frame.asyncServiceNs;
    aggregate.rescheduleNs += frame.rescheduleNs;
    aggregate.rescheduleTransformNs += frame.rescheduleTransformNs;
    aggregate.rescheduleEntropyNs += frame.rescheduleEntropyNs;
    aggregate.rescheduleLockedCacheNs += frame.rescheduleLockedCacheNs;
    aggregate.transformCalls += frame.transformCalls;
    aggregate.entropyCalls += frame.entropyCalls;
    aggregate.lockedCacheDmaCalls += frame.lockedCacheDmaCalls;
    aggregate.lockedCacheBusyCalls += frame.lockedCacheBusyCalls;
    aggregate.rescheduleCalls += frame.rescheduleCalls;

    if ((aggregate.frames & 0x7Fu) == 0u) {
        const uint64_t trackedNs = aggregate.transformNs + aggregate.entropyNs +
                                   aggregate.lockedCacheDmaNs + aggregate.lockedCacheBusyNs;
        const uint64_t residualNs = aggregate.wallNs > trackedNs ? aggregate.wallNs - trackedNs : 0u;
        const auto avgUs = [&](uint64_t ns) { return ns / aggregate.frames / 1000u; };
        auto& os = RT_LOG("meteor-thp-perf");
        os << "summary frames=" << aggregate.frames
           << " wallAvgUs=" << avgUs(aggregate.wallNs)
           << " wallMaxUs=" << aggregate.wallMaxNs / 1000u
           << " transformAvgUs=" << avgUs(aggregate.transformNs)
           << " entropyAvgUs=" << avgUs(aggregate.entropyNs)
           << " lcDmaAvgUs=" << avgUs(aggregate.lockedCacheDmaNs)
           << " lcBusyAvgUs=" << avgUs(aggregate.lockedCacheBusyNs)
           << " residualAvgUs=" << avgUs(residualNs)
           << " peServiceAvgUs=" << avgUs(aggregate.peServiceNs)
           << " asyncServiceAvgUs=" << avgUs(aggregate.asyncServiceNs)
           << " reschedAvgUs=" << avgUs(aggregate.rescheduleNs)
           << " reschedTransformAvgUs=" << avgUs(aggregate.rescheduleTransformNs)
           << " reschedEntropyAvgUs=" << avgUs(aggregate.rescheduleEntropyNs)
           << " reschedLcAvgUs=" << avgUs(aggregate.rescheduleLockedCacheNs)
           << " transformCallsPerFrame=" << aggregate.transformCalls / aggregate.frames
           << " entropyCallsPerFrame=" << aggregate.entropyCalls / aggregate.frames
           << " lcDmaCallsPerFrame=" << aggregate.lockedCacheDmaCalls / aggregate.frames
           << " lcBusyCallsPerFrame=" << aggregate.lockedCacheBusyCalls / aggregate.frames
           << " reschedCallsPerFrame=" << aggregate.rescheduleCalls / aggregate.frames
           << std::endl;
    }

    frame = {};
}

ThpLatencySlot& ThpLatencySlotForTag(uint32_t tag) noexcept {
    return g_thpLatencySlots[tag & (kThpLatencySlotCount - 1u)];
}

void ThpLatencyResetForStreamPublish(uint32_t tag, uint64_t nowUs) noexcept {
    ThpLatencySlot& slot = ThpLatencySlotForTag(tag);
    slot.tag.store(kThpLatencyInvalidTag, std::memory_order_relaxed);
    slot.decoderInputPublishUs.store(0u, std::memory_order_relaxed);
    slot.decodeBeginUs.store(0u, std::memory_order_relaxed);
    slot.decodeEndUs.store(0u, std::memory_order_relaxed);
    slot.readyPublishUs.store(0u, std::memory_order_relaxed);
    slot.streamPublishUs.store(nowUs, std::memory_order_relaxed);
    slot.tag.store(tag, std::memory_order_release);
}

ThpLatencySlot& ThpLatencyEnsureTag(uint32_t tag) noexcept {
    ThpLatencySlot& slot = ThpLatencySlotForTag(tag);
    if (slot.tag.load(std::memory_order_acquire) != tag) {
        slot.tag.store(kThpLatencyInvalidTag, std::memory_order_relaxed);
        slot.streamPublishUs.store(0u, std::memory_order_relaxed);
        slot.decoderInputPublishUs.store(0u, std::memory_order_relaxed);
        slot.decodeBeginUs.store(0u, std::memory_order_relaxed);
        slot.decodeEndUs.store(0u, std::memory_order_relaxed);
        slot.readyPublishUs.store(0u, std::memory_order_relaxed);
        slot.tag.store(tag, std::memory_order_release);
    }
    return slot;
}

ThpLatencySnapshot ThpLatencyRead(uint32_t tag) noexcept {
    ThpLatencySnapshot result{};
    ThpLatencySlot& slot = ThpLatencySlotForTag(tag);
    if (slot.tag.load(std::memory_order_acquire) != tag) {
        return result;
    }
    result.streamPublishUs = slot.streamPublishUs.load(std::memory_order_relaxed);
    result.decoderInputPublishUs = slot.decoderInputPublishUs.load(std::memory_order_relaxed);
    result.decodeBeginUs = slot.decodeBeginUs.load(std::memory_order_relaxed);
    result.decodeEndUs = slot.decodeEndUs.load(std::memory_order_relaxed);
    result.readyPublishUs = slot.readyPublishUs.load(std::memory_order_relaxed);
    return result;
}

uint64_t ThpLatencyDelta(uint64_t beginUs, uint64_t endUs) noexcept {
    return beginUs != 0u && endUs >= beginUs ? endUs - beginUs : 0u;
}

void ThpLatencyRecord(ThpLatencyAggregate& aggregate, uint64_t deltaUs) noexcept {
    if (deltaUs == 0u) {
        return;
    }
    aggregate.count.fetch_add(1u, std::memory_order_relaxed);
    aggregate.sumUs.fetch_add(deltaUs, std::memory_order_relaxed);
    uint64_t previousMax = aggregate.maxUs.load(std::memory_order_relaxed);
    while (deltaUs > previousMax &&
           !aggregate.maxUs.compare_exchange_weak(previousMax, deltaUs,
                                                   std::memory_order_relaxed,
                                                   std::memory_order_relaxed)) {
    }
}

void LogThpLatencyMetric(std::ostream& os, const char* name,
                         const ThpLatencyAggregate& aggregate) {
    const uint64_t count = aggregate.count.load(std::memory_order_relaxed);
    const uint64_t sumUs = aggregate.sumUs.load(std::memory_order_relaxed);
    const uint64_t maxUs = aggregate.maxUs.load(std::memory_order_relaxed);
    os << ' ' << name << "AvgUs=" << (count != 0u ? sumUs / count : 0u)
       << ' ' << name << "MaxUs=" << maxUs
       << ' ' << name << "N=" << count;
}

void StartMeteorCallWatchdog() {
    std::call_once(g_meteorCallWatchdogOnce, [] {
        std::thread([] {
            uint64_t previousSerial = 0u;
            uint64_t previousLoopSerial = 0u;
            std::array<uint64_t, 16> previousCounts{};
            std::array<uint64_t, 10> previous57520ByLr{};
            std::array<uint64_t, 3> previous06840ByLr{};
            std::array<uint64_t, 14> previous065CCByLr{};
            uint64_t previousSchedulerThreadChanges = 0u;
            uint32_t stableSamples = 0u;
            int previousGxFrameCount = -1;
            uint32_t stableGxFrameSamples = 0u;
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                if (!g_meteorPeFinishDelivered.load(std::memory_order_acquire)) {
                    continue;
                }

                const uint64_t serial = g_meteorCallSerial.load(std::memory_order_acquire);
                const uint32_t target = g_meteorLastCallTarget.load(std::memory_order_relaxed);
                const uint32_t lr = g_meteorLastCallLr.load(std::memory_order_relaxed);
                const uint64_t callDelta = serial - previousSerial;
                const uint64_t loopSerial = g_meteorLoopSerial.load(std::memory_order_acquire);
                const uint32_t loopPc = g_meteorLastLoopPc.load(std::memory_order_relaxed);
                const uint32_t loopLr = g_meteorLastLoopLr.load(std::memory_order_relaxed);
                const uint64_t loopDelta = loopSerial - previousLoopSerial;
                previousLoopSerial = loopSerial;

                if (loopDelta != 0u) {
                    RT_LOG("meteor-loop-watch") << "pc=0x" << std::hex << loopPc
                                                << " lr=0x" << loopLr
                                                << std::dec << " loopsPerSecond~" << loopDelta
                                                << " serial=" << loopSerial << std::endl;
                }
                const int gxFrameCount = g_gxFrameCount;
                if (gxFrameCount == previousGxFrameCount) {
                    ++stableGxFrameSamples;
                } else {
                    previousGxFrameCount = gxFrameCount;
                    stableGxFrameSamples = 0u;
                }
                if (stableGxFrameSamples == 3u || stableGxFrameSamples == 30u) {
                    try {
                        constexpr uint32_t kPeControl = 0xCC00100Au;
                        constexpr uint32_t kMainThread = 0x80559D98u;
                        uint16_t peControl = 0u;
                        const bool peReadable = GX_HLE_TryPeRegisterRead16(kPeControl, &peControl);
                        RT_LOG("meteor-gx-stall")
                            << "gxFrames=" << gxFrameCount
                            << " stableSeconds=" << stableGxFrameSamples
                            << " peReadable=" << peReadable
                            << " peControl=0x" << std::hex << peControl
                            << " mainState=" << std::dec << Memory::Read16(kMainThread + 0x2C8u)
                            << " mainSuspend=" << static_cast<int32_t>(Memory::Read32(kMainThread + 0x2CCu))
                            << " mainQueue=0x" << std::hex << Memory::Read32(kMainThread + 0x2DCu)
                            << " current=0x" << Memory::Read32(0x800000D4u)
                            << " running=0x" << Memory::Read32(0x800000E4u)
                            << " pending=0x" << Memory::Read32(0x8062ADC8u)
                            << std::dec << std::endl;
                    } catch (const Memory::AccessViolation&) {
                    }
                }
                if (callDelta != 0u) {
                    RT_LOG("meteor-call-watch") << "target=0x" << std::hex << target
                                                << " lr=0x" << lr
                                                << std::dec << " callsPerSecond~" << callDelta
                                                << " serial=" << serial << std::endl;
                }
                const std::array<uint64_t, 16> counts{
                    g_meteorCount8015A800.load(std::memory_order_relaxed),
                    g_meteorCount8015A06C.load(std::memory_order_relaxed),
                    g_meteorCount80257520.load(std::memory_order_relaxed),
                    g_meteorCount8025797C.load(std::memory_order_relaxed),
                    g_meteorCount80257810.load(std::memory_order_relaxed),
                    g_meteorCount802AB688.load(std::memory_order_relaxed),
                    g_meteorCount80210990.load(std::memory_order_relaxed),
                    g_meteorCount8020635C.load(std::memory_order_relaxed),
                    g_meteorCount802061C8.load(std::memory_order_relaxed),
                    g_meteorCount802065CC.load(std::memory_order_relaxed),
                    g_meteorCount8029EFA8.load(std::memory_order_relaxed),
                    g_meteorCount8029EFD0.load(std::memory_order_relaxed),
                    g_meteorCount802A1914.load(std::memory_order_relaxed),
                    g_meteorCount802A15DC.load(std::memory_order_relaxed),
                    g_meteorCount802AA640.load(std::memory_order_relaxed),
                    g_meteorCount802AA4E4.load(std::memory_order_relaxed),
                };
                bool anyTrackedDelta = false;
                for (size_t i = 0; i < counts.size(); ++i) {
                    anyTrackedDelta = anyTrackedDelta || counts[i] != previousCounts[i];
                }
                if (anyTrackedDelta) {
                    RT_LOG("meteor-call-counts")
                        << "d8015A800=" << (counts[0] - previousCounts[0])
                        << " d8015A06C=" << (counts[1] - previousCounts[1])
                        << " d80257520=" << (counts[2] - previousCounts[2])
                        << " d8025797C=" << (counts[3] - previousCounts[3])
                        << " d80257810=" << (counts[4] - previousCounts[4])
                        << " d802AB688=" << (counts[5] - previousCounts[5])
                        << " d80210990=" << (counts[6] - previousCounts[6])
                        << " d8020635C=" << (counts[7] - previousCounts[7])
                        << " d802061C8=" << (counts[8] - previousCounts[8])
                        << " d802065CC=" << (counts[9] - previousCounts[9])
                        << " d8029EFA8=" << (counts[10] - previousCounts[10])
                        << " d8029EFD0=" << (counts[11] - previousCounts[11])
                        << " d802A1914=" << (counts[12] - previousCounts[12])
                        << " d802A15DC=" << (counts[13] - previousCounts[13])
                        << " d802AA640=" << (counts[14] - previousCounts[14])
                        << " d802AA4E4=" << (counts[15] - previousCounts[15])
                        << std::endl;
                    previousCounts = counts;
                }
                const uint64_t schedulerThreadChanges = g_meteorSchedulerThreadChanges.load(std::memory_order_relaxed);
                const uint32_t schedulerCurrentThread = g_meteorSchedulerCurrentThread.load(std::memory_order_relaxed);
                if (schedulerThreadChanges != previousSchedulerThreadChanges) {
                    RT_LOG("meteor-scheduler-thread")
                        << "current=0x" << std::hex << schedulerCurrentThread
                        << std::dec << " changes=" << schedulerThreadChanges
                        << " delta=" << (schedulerThreadChanges - previousSchedulerThreadChanges)
                        << std::endl;
                    previousSchedulerThreadChanges = schedulerThreadChanges;
                }
                std::array<uint64_t, 10> byLr{};
                bool any57520Delta = false;
                for (size_t i = 0; i < byLr.size(); ++i) {
                    byLr[i] = g_meteor80257520ByLr[i].load(std::memory_order_relaxed);
                    any57520Delta = any57520Delta || byLr[i] != previous57520ByLr[i];
                }
                if (any57520Delta) {
                    RT_LOG("meteor-57520-lr")
                        << "80027798=" << (byLr[0] - previous57520ByLr[0])
                        << " 8015A0F8=" << (byLr[1] - previous57520ByLr[1])
                        << " 801A031C=" << (byLr[2] - previous57520ByLr[2])
                        << " 8020672C=" << (byLr[3] - previous57520ByLr[3])
                        << " 80206860=" << (byLr[4] - previous57520ByLr[4])
                        << " 80219F24=" << (byLr[5] - previous57520ByLr[5])
                        << " 80219F44=" << (byLr[6] - previous57520ByLr[6])
                        << " 80219F64=" << (byLr[7] - previous57520ByLr[7])
                        << " 802D847C=" << (byLr[8] - previous57520ByLr[8])
                        << " other=" << (byLr[9] - previous57520ByLr[9])
                        << std::endl;
                    previous57520ByLr = byLr;
                }
                std::array<uint64_t, 3> by06840Lr{};
                bool any06840Delta = false;
                for (size_t i = 0; i < by06840Lr.size(); ++i) {
                    by06840Lr[i] = g_meteor80206840ByLr[i].load(std::memory_order_relaxed);
                    any06840Delta = any06840Delta || by06840Lr[i] != previous06840ByLr[i];
                }
                if (any06840Delta) {
                    RT_LOG("meteor-06840-lr")
                        << "802061DC=" << (by06840Lr[0] - previous06840ByLr[0])
                        << " 802065EC=" << (by06840Lr[1] - previous06840ByLr[1])
                        << " other=" << (by06840Lr[2] - previous06840ByLr[2])
                        << std::endl;
                    previous06840ByLr = by06840Lr;
                }
                std::array<uint64_t, 14> by065CCLr{};
                bool any065CCDelta = false;
                for (size_t i = 0; i < by065CCLr.size(); ++i) {
                    by065CCLr[i] = g_meteor802065CCByLr[i].load(std::memory_order_relaxed);
                    any065CCDelta = any065CCDelta || by065CCLr[i] != previous065CCByLr[i];
                }
                if (any065CCDelta) {
                    RT_LOG("meteor-065cc-lr")
                        << "80027DF4=" << (by065CCLr[0] - previous065CCByLr[0])
                        << " 80027E48=" << (by065CCLr[1] - previous065CCByLr[1])
                        << " 80027E50=" << (by065CCLr[2] - previous065CCByLr[2])
                        << " 80028590=" << (by065CCLr[3] - previous065CCByLr[3])
                        << " 80028D40=" << (by065CCLr[4] - previous065CCByLr[4])
                        << " 80028E80=" << (by065CCLr[5] - previous065CCByLr[5])
                        << " 8002D398=" << (by065CCLr[6] - previous065CCByLr[6])
                        << " 80159FBC=" << (by065CCLr[7] - previous065CCByLr[7])
                        << " 8015A030=" << (by065CCLr[8] - previous065CCByLr[8])
                        << " 801D3798=" << (by065CCLr[9] - previous065CCByLr[9])
                        << " 801D3850=" << (by065CCLr[10] - previous065CCByLr[10])
                        << " 801D3900=" << (by065CCLr[11] - previous065CCByLr[11])
                        << " 801D3964=" << (by065CCLr[12] - previous065CCByLr[12])
                        << " other=" << (by065CCLr[13] - previous065CCByLr[13])
                        << std::endl;
                    previous065CCByLr = by065CCLr;
                }
                if (serial == previousSerial) {
                    ++stableSamples;
                } else {
                    stableSamples = 0u;
                    previousSerial = serial;
                }

                if (stableSamples == 2u || stableSamples == 5u || stableSamples == 10u) {
                    RT_LOG("meteor-call-watch") << "stable call boundary target=0x"
                                                << std::hex << target
                                                << " lr=0x" << lr
                                                << std::dec << " serial=" << serial
                                                << " stableSeconds=" << stableSamples
                                                << std::endl;
                }
            }
        }).detach();
    });
}

bool DeliverMeteorPeFinishInterrupt(CpuContext* interruptedCpu) noexcept {
    if (interruptedCpu == nullptr || (interruptedCpu->msr & 0x00008000u) == 0u) {
        return false;
    }

    try {
        // Ghidra headless validates RDSPAF 0x8022DD04 as the GX interrupt init
        // routine: interrupt 0x13 is installed at 0x8022DC84. GXDrawDone at
        // 0x8022D9A0 emits BP 0x45 FINISH, clears r13-0x6118 and sleeps on
        // r13-0x6120 until this handler sets the flag and wakes the queue.
        if (!GX_HLE_ClaimPeFinishInterrupt()) {
            return false;
        }

        GuestInterruptCallbackContext interrupt;
        CpuContext* interruptCpu = interrupt.get();
        interruptCpu->msr &= ~0x00008000u;

        // RDSPAF GXDrawDone uses r13-0x6118 as its completion byte and sleeps on
        // r13-0x6120.  The retail PE FINISH handler at 0x8022DC84 acknowledges
        // PE CSR bit 3, sets that byte, optionally runs the user draw-done
        // callback at r13-0x6114, then OSWakeupThread(r13-0x6120).  At the current
        // boot path no callback is installed.  Waking through the shared HLE is
        // important here: invoking the translated OSWakeupThread from inside this
        // translated scheduler checkpoint re-enters SelectThread before the
        // interrupted scheduler frame can consume its newly-published pending bit.
        constexpr uint32_t kPeControl = 0xCC00100Au;
        constexpr uint32_t kDrawDoneFlag = 0x8062B208u;     // r13 - 0x6118
        constexpr uint32_t kDrawDoneCallback = 0x8062B20Cu; // r13 - 0x6114
        constexpr uint32_t kDrawDoneQueue = 0x8062B200u;    // r13 - 0x6120

        const uint32_t callback = Memory::Read32(kDrawDoneCallback);
        if (callback == 0u) {
            const uint16_t peControl = Memory::Read16(kPeControl);
            Memory::Write16(kPeControl, static_cast<uint16_t>(peControl | 0x0008u));
            Memory::Write8(kDrawDoneFlag, 1u);
            OS_HLE_WakeupThreadNoReschedule(interruptCpu, kDrawDoneQueue);
        } else {
            // Preserve retail callback semantics if the title installs one later.
            const uint32_t interruptedContext = Memory::Read32(0x800000D4u);
            interruptCpu->gpr[3] = 0x13u;
            interruptCpu->gpr[4] = interruptedContext;
            OS_HLE_BeginDeferredGuestCallbacks();
            try {
                InvokeIndirectCpu(0x8022DC84u, interruptCpu);
            } catch (...) {
                OS_HLE_EndDeferredGuestCallbacks();
                throw;
            }
            OS_HLE_EndDeferredGuestCallbacks();
        }

        g_meteorPeFinishDelivered.store(true, std::memory_order_release);
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed)) {
            RT_LOG("meteor-scheduler") << "delivered RDSPAF PE FINISH interrupt"
                                        << (callback == 0u ? " via direct draw-done wake" : " via retail handler 0x8022DC84")
                                        << std::endl;
        }
        return true;
    } catch (...) {
        // Keep the title loop hook noexcept. A failed speculative delivery must
        // leave the normal VI idle service available for the next checkpoint.
        return false;
    }
}

void TryMeteorInterruptReturnReschedule(CpuContext* interruptedCpu) noexcept {
    if (interruptedCpu == nullptr || (interruptedCpu->msr & 0x00008000u) == 0u ||
        VI_HLE_IsAdvancingRetrace()) {
        return;
    }

    try {
        const auto scheduler = OS_HLE_GetSchedulerGuestStateLayout();
        if (scheduler.schedulerDisableCount == 0u || scheduler.schedulerReschedFlag == 0u) {
            return;
        }
        if (Memory::Read32(scheduler.schedulerDisableCount) != 0u ||
            Memory::Read32(scheduler.schedulerReschedFlag) == 0u) {
            return;
        }

        // A hardware IRQ may wake a higher-priority thread while the interrupted
        // translated function is sitting in a hot backedge.  On Broadway the IRQ
        // return path consumes that reschedule request before resuming the old
        // thread, but the interrupted architectural register file itself is not a
        // SelectThread call frame and therefore must survive intact.
        //
        // Preserve the complete context, not only r3.  SelectThread invokes the
        // retail switch callback before changing host fibers; that callback uses
        // r3/r4 for (oldContext,newContext).  When this helper was entered from a
        // generated loop checkpoint, r4 could therefore leak the selected OSThread
        // address back into the interrupted routine.  RDSPAF exposed this exactly
        // as 0x8002D414's live output buffer changing from 0x90CCAF80 to thread
        // 0x805D5A68, after which resource relocation overwrote that OSThread.
        const CpuContext interruptedSnapshot = *interruptedCpu;
        const uint64_t thpRescheduleBeginNs =
            g_thpPerfFrame.active ? MeteorSteadyNanos() : 0u;
        interruptedCpu->gpr[3] = 0u;
        OS_HLE_SelectThread(interruptedCpu);
        if (thpRescheduleBeginNs != 0u) {
            ThpPerfRecordReschedule(MeteorSteadyNanos() - thpRescheduleBeginNs);
        }
        *interruptedCpu = interruptedSnapshot;
        OS_HLE_ApplyInterruptStateFromMsr(interruptedCpu->msr);
    } catch (const Memory::AccessViolation&) {
    } catch (...) {
        // This is title checkpoint service; never turn a speculative asynchronous
        // reschedule into a host exception.  The pending bit remains available to
        // the next retail/native scheduler boundary.
    }
}

void ConfigureMeteorSchedulerWaitLayouts() noexcept {
    static std::once_flag once;
    std::call_once(once, [] {
        // RDSPAF scheduler globals, validated in Ghidra headless from
        // __OSThreadInit/SelectThread (0x802103A4 / 0x80210990). The title's
        // SDA1 base is 0x80631320; these are the exact linked SDK objects that
        // the translated idle loop at 0x80210ADC is polling.
        OsSchedulerGuestStateLayout scheduler{};
        scheduler.defaultThreadContext = 0x80559D98u;
        scheduler.idleThreadContext = 0x8055A1B0u;
        scheduler.threadQueueArray = 0x8055A0B0u;
        scheduler.switchThreadCallback = 0x80629420u; // r13 - 0x7F00
        scheduler.schedulerDisableCount = 0x8062ADC0u; // r13 - 0x6560
        scheduler.schedulerReschedFlag = 0x8062ADC4u; // r13 - 0x655C
        scheduler.schedulerPendingMask = 0x8062ADC8u; // r13 - 0x6558
        OS_HLE_SetSchedulerGuestStateLayout(scheduler);

        // RDSPAF OSCreateThread (0x80210BD0) calls OSInitContext at 0x80209F88,
        // stores OSExitThread (0x80210E3C) into the new context LR, gates the
        // extended context initialization on 0x805492F0, and derives the context
        // attribute word from r13-0x7F28 (0x806293F8). Keep those linked guest
        // addresses in the title adapter instead of teaching the shared Wii HLE
        // about Meteor's SDK placement.
        OsThreadGuestConfig thread{};
        thread.initContextFunction = 0x80209F88u;
        thread.exitThreadFunction = 0x80210E3Cu;
        thread.schedulerInitFlag = 0x805492F0u;
        thread.threadAttrSource = 0x806293F8u; // r13 - 0x7F28
        OS_HLE_SetThreadGuestConfig(thread);

        // The retail VI interrupt handler at 0x8021BBA8 increments
        // r13-0x636C and wakes r13-0x6390 after every retrace; VIWaitForRetrace
        // at 0x8021CCA0 sleeps on that same queue. The translated callback setters
        // at 0x8021C3B8/0x8021C3FC store the pre/post callbacks at r13-0x6398 and
        // r13-0x639C respectively. Bind those software mirrors too so the shared
        // VI retrace service can pull the retail-installed callback immediately
        // before dispatch instead of keeping a stale host-side zero callback.
        //
        // This is required by the retail video state machine: live RDSPAF installs
        // post-retrace callback 0x801D4048 in r13-0x639C, and that callback is the
        // natural producer of the state transition consumed by the frame loop.
        ViGuestStateLayout vi{};
        // RDSPAF keeps VIConfigure's requested TV family staged at 0x8055FAD8,
        // while VIGetTvFormat reads the committed word at r13-0x63A8. VIFlush
        // arms r13-0x6370 and the retail VI interrupt publishes staged -> active.
        // Because this HLE replaces that interrupt, bind all three so the commit
        // happens on the same retrace boundary instead of creating either hybrid
        // (50-Hz host + stale EURGB60 guest, or a host stuck at EURGB60 after a
        // PAL50 request).
        vi.tvFormat = 0x8062AF78u; // r13 - 0x63A8, committed VIGetTvFormat source
        vi.tvFormatGuestAuthoritative = true;
          // Promote the complete PAL render mode in VIConfigure: EURGB60
          // timing plus 640x480 framebuffer geometry.
          vi.forcePal60 = true;
          vi.forcedTvFormat = 5u;
        vi.pendingTvFormat = 0x8055FAD8u; // translated VIConfigure staged TV family
        vi.tvFormatCommitArm = 0x8062AFB0u; // r13 - 0x6370, VIFlush commit arm
        vi.activeTiming = 0x8062AF7Cu; // r13 - 0x63A4, committed timing-table pointer
        vi.pendingTiming = 0x8055FB04u; // VIConfigure staged timing-table pointer
        // Ghidra HEADLESS: RDSPAF VI state is based at 0x8055FAB0 and the
        // adjusted display-Y halfword consumed by 0x8021DE2C lives at +0x0A.
        // Its parity is part of VIGetNextField polarity, so translated VIConfigure
        // remains authoritative for this value too.
        vi.viYOrigin = 0x8055FABAu;
        vi.viYOriginGuestAuthoritative = true;
        // VISetNextFrameBuffer at 0x8021DD38 stores the retail-selected XFB in
        // VI state base 0x8055FAB0 + 0x30. Boot/logo screens use this direct RAM
        // path before the normal GXCopyDisp producer becomes active.
        vi.nextFrameBuffer = 0x8055FAE0u;
        vi.nextFrameBufferGuestAuthoritative = true;
        vi.retraceCount = 0x8062AFB4u; // r13 - 0x636C
        vi.retraceQueue = 0x8062AF90u; // r13 - 0x6390
        vi.preRetraceCallback = 0x8062AF88u;  // r13 - 0x6398
        vi.postRetraceCallback = 0x8062AF84u; // r13 - 0x639C
        // Keep VI as the guest timing authority, but do not quantize host scanout
        // through the asynchronous VI latch. GXCopyDisp already seals/paces each
        // completed frame to the same retrace grid and deliberately free-runs a
        // frame that finished late; forcing presentation back through the VI latch
        // makes a worker completion that misses one boundary wait a full extra
        // retrace, which turns mild lateness into 60->40 / 30->20 cadence drops.
        vi.presentAtRetrace = false;
        VI_HLE_SetGuestStateLayout(vi);

        // RDSPAF's RVL OS alarm block is linked at a different SDA1 offset than
        // MKW. Ghidra headless identifies __InitAlarm at 0x802084B4 clearing
        // r13-0x65E0, CreateAlarm at 0x8020850C, and the real InsertAlarm entry
        // at 0x8020851C. The host alarm pump must reinsert periodic alarms through
        // those title-local objects; using the MKW defaults makes it parse VI
        // globals as an alarm queue and dispatch the unrelated interior address
        // 0x801A0620.
        OsAlarmGuestStateLayout alarms{};
        alarms.queueOffsetFromR13 = 0x65E0u;
        alarms.insertAlarmFunction = 0x8020851Cu;
        OS_HLE_SetAlarmGuestStateLayout(alarms);
    });
}

} // namespace

void Meteor_ConfigureOsHleGuestState() noexcept {
    ConfigureMeteorSchedulerWaitLayouts();
}

uint64_t NandIosCurrentCallbackDispatchId() noexcept;

void Meteor_RuntimeCallCheckpoint(uint32_t target, CpuContext* ctx) noexcept {
    // This hook is wired into every translated call boundary.  All work in the
    // call/return checkpoints is bring-up telemetry; functional scheduler/VI,
    // alarm/audio/BT servicing lives in Meteor_RuntimeLoopCheckpoint instead.
    // Keep the historical probes available behind the explicit trace switch,
    // but make normal gameplay pay a single predictable branch rather than the
    // full diagnostic decision tree on every guest call.
    if (!MeteorRuntimeTraceEnabledLocal()) {
        return;
    }

    if constexpr (kEnableThpPerfProfile) {
        if (ctx && target == 0x8030741Cu && ctx->lr == 0x801D508Cu) {
            g_thpPerfFrame = {};
            g_thpPerfFrame.active = true;
            g_thpPerfFrame.wallBeginNs = MeteorSteadyNanos();
        }
        ThpPerfBeginTrackedCall(target);
    }

    // End-to-end THP producer timing. HEADLESS proves three distinct publication
    // boundaries before the consumer-visible ready queue:
    //   p8 stream  0x801D4D60 -> OSSendMessage(queue 0x80547A40), LR 0x801D4D64
    //   p12 worker tail-send -> queue 0x80547A88, target LR remains 0x801D2CAC
    //   p20 decode 0x801D5088 -> 0x8030741C, LR 0x801D508C
    // followed by ready publication at LR 0x801D50D0.  Record host wall time at
    // those already-existing call boundaries only; do not yield, wait, retry, or
    // touch guest queues/state.
    if (ctx && target == 0x8020E574u) {
        try {
            const uint32_t queue = ctx->gpr[3];
            const uint32_t message = ctx->gpr[4];
            const uint64_t nowUs = MeteorSteadyMicros();

            if (ctx->lr == 0x801D4D64u && queue == 0x80547A40u && message != 0u &&
                Memory::Contains(message + 4u, sizeof(uint32_t))) {
                const uint32_t tag = Memory::Read32(message + 4u);
                ThpLatencyResetForStreamPublish(tag, nowUs);
            } else if (ctx->lr == 0x801D2CACu && queue == 0x80547A88u && message != 0u &&
                       Memory::Contains(message + 4u, sizeof(uint32_t))) {
                const uint32_t tag = Memory::Read32(message + 4u);
                ThpLatencyEnsureTag(tag).decoderInputPublishUs.store(nowUs, std::memory_order_relaxed);
            } else if (ctx->lr == 0x801D50D0u && queue == 0x80548E18u && message != 0u &&
                       Memory::Contains(message + 0x0Cu, sizeof(uint32_t))) {
                const uint32_t tag = Memory::Read32(message + 0x0Cu);
                ThpLatencySlot& slot = ThpLatencyEnsureTag(tag);
                slot.readyPublishUs.store(nowUs, std::memory_order_relaxed);
                const ThpLatencySnapshot latency = ThpLatencyRead(tag);

                ThpLatencyRecord(g_thpStreamToP12,
                                 ThpLatencyDelta(latency.streamPublishUs,
                                                 latency.decoderInputPublishUs));
                ThpLatencyRecord(g_thpP12ToDecode,
                                 ThpLatencyDelta(latency.decoderInputPublishUs,
                                                 latency.decodeBeginUs));
                ThpLatencyRecord(g_thpStreamToDecode,
                                 ThpLatencyDelta(latency.streamPublishUs,
                                                 latency.decodeBeginUs));
                ThpLatencyRecord(g_thpDecodeWork,
                                 ThpLatencyDelta(latency.decodeBeginUs,
                                                 latency.decodeEndUs));
                ThpLatencyRecord(g_thpDecodeTail,
                                 ThpLatencyDelta(latency.decodeEndUs,
                                                 latency.readyPublishUs));
                ThpLatencyRecord(g_thpStreamToReady,
                                 ThpLatencyDelta(latency.streamPublishUs,
                                                 latency.readyPublishUs));

                const uint64_t readyCount =
                    g_thpLatencyReadyCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if ((readyCount & 0x7Fu) == 0u) {
                    auto& os = RT_LOG("meteor-thp-latency");
                    os << "summary ready=" << readyCount;
                    LogThpLatencyMetric(os, "streamP12", g_thpStreamToP12);
                    LogThpLatencyMetric(os, "p12Decode", g_thpP12ToDecode);
                    LogThpLatencyMetric(os, "streamDecode", g_thpStreamToDecode);
                    LogThpLatencyMetric(os, "decode", g_thpDecodeWork);
                    LogThpLatencyMetric(os, "decodeTail", g_thpDecodeTail);
                    LogThpLatencyMetric(os, "streamReady", g_thpStreamToReady);
                    os << std::endl;
                }
            }
        } catch (const Memory::AccessViolation&) {
            RT_LOG("meteor-thp-latency") << "call-snapshot=invalid" << std::endl;
        }
    }

    if (MeteorRuntimeTraceEnabledLocal() && ctx && target == 0x8030741Cu && ctx->lr == 0x801D508Cu) {
        try {
            const uint32_t work = ctx->gpr[23];
            if (work != 0u && Memory::Contains(work + 4u, sizeof(uint32_t))) {
                const uint32_t tag = Memory::Read32(work + 4u);
                ThpLatencyEnsureTag(tag).decodeBeginUs.store(MeteorSteadyMicros(),
                                                             std::memory_order_relaxed);
            }
        } catch (const Memory::AccessViolation&) {
            RT_LOG("meteor-thp-latency") << "decode-begin-snapshot=invalid" << std::endl;
        }
    }

    // BTA system task fallback dispatch. 0x80271BF8 is the indirect call used
    // after mailbox-0 messages are matched against the six registered BTA
    // subsystem entries; LR is therefore 0x80271C00 at the callee.  Trace the
    // selected handler and the original message without changing guest state so
    // the successful/failing 0x1708 HID-send cases can be compared directly.
    if (ctx && ctx->lr == 0x80271C00u) {
        static std::atomic<uint32_t> btaDispatchTraceCount{0u};
        const uint32_t n = btaDispatchTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (n < 128u) {
            const uint32_t msg = ctx->gpr[3];
            auto& os = RT_LOG("meteor-bta-dispatch");
            os << "handler=0x" << std::hex << target
               << " msg=0x" << msg;
            try {
                if (msg != 0u && Memory::Contains(msg, 0x20u)) {
                    os << " event=0x" << Memory::Read16(msg)
                       << " bytes=";
                    for (uint32_t i = 0u; i < 0x20u; ++i) {
                        if (i != 0u) os << ':';
                        os << static_cast<uint32_t>(Memory::Read8(msg + i));
                    }
                }
            } catch (const Memory::AccessViolation&) {
                os << " snapshot=invalid";
            }
            os << std::dec << std::endl;
        }
    }
    if (ctx && target == 0x8027B960u) {
        auto& os = RT_LOG("meteor-bt-stored-key");
        os << "btm_return_link_keys_evt lr=0x" << std::hex << ctx->lr
           << " data=0x" << ctx->gpr[3];
        try {
            const uint32_t p = ctx->gpr[3];
            if (p != 0u && Memory::Contains(p, 24u)) {
                os << " type=0x" << static_cast<uint32_t>(Memory::Read8(p))
                   << " count=0x" << static_cast<uint32_t>(Memory::Read8(p + 1u))
                   << " bdaddr=";
                for (uint32_t i = 0u; i < 6u; ++i) {
                    if (i != 0u) os << ':';
                    os << static_cast<uint32_t>(Memory::Read8(p + 2u + i));
                }
            }
        } catch (const Memory::AccessViolation&) {
            os << " snapshot=invalid";
        }
        os << std::dec << std::endl;
    }
    if (ctx && target == 0x8026C3B0u) {
        auto& os = RT_LOG("meteor-bt-stored-key");
        os << "WUDStoredLinkKeyCallback lr=0x" << std::hex << ctx->lr
           << " data=0x" << ctx->gpr[3];
        try {
            const uint32_t p = ctx->gpr[3];
            if (p != 0u && Memory::Contains(p, 24u)) {
                os << " event=0x" << static_cast<uint32_t>(Memory::Read8(p))
                   << " count=0x" << static_cast<uint32_t>(Memory::Read8(p + 1u))
                   << " bdaddr=";
                for (uint32_t i = 0u; i < 6u; ++i) {
                    if (i != 0u) os << ':';
                    os << static_cast<uint32_t>(Memory::Read8(p + 2u + i));
                }
            }
        } catch (const Memory::AccessViolation&) {
            os << " snapshot=invalid";
        }
        os << std::dec << std::endl;
    }
    if (ctx && target == 0x8026B8E8u) {
        const uint32_t event = ctx->gpr[3];
        const uint32_t data = ctx->gpr[4];
        auto& os = RT_LOG("meteor-wud-security");
        os << "WUDSecurityCallback event=0x" << std::hex << event
           << " lr=0x" << ctx->lr
           << " data=0x" << data;
        try {
            constexpr uint32_t kWudBase = 0x8058A160u;
            os << " syncState=0x" << static_cast<uint32_t>(Memory::Read8(kWudBase + 1765u));
            if (data != 0u && Memory::Contains(data, 6u)) {
                os << " bdaddr=";
                for (uint32_t i = 0u; i < 6u; ++i) {
                    if (i != 0u) os << ':';
                    os << static_cast<uint32_t>(Memory::Read8(data + i));
                }

                int foundTable = -1;
                int foundIndex = -1;
                uint32_t foundRecord = 0u;
                for (uint32_t i = 0u; i < 10u && foundIndex < 0; ++i) {
                    const uint32_t record = kWudBase + 228u + i * 96u;
                    bool same = true;
                    for (uint32_t j = 0u; j < 6u; ++j) {
                        if (Memory::Read8(record + 64u + j) != Memory::Read8(data + j)) {
                            same = false;
                            break;
                        }
                    }
                    if (same) {
                        foundTable = 0;
                        foundIndex = static_cast<int>(i);
                        foundRecord = record;
                    }
                }
                for (uint32_t i = 0u; i < 6u && foundIndex < 0; ++i) {
                    const uint32_t record = kWudBase + 1188u + i * 96u;
                    bool same = true;
                    for (uint32_t j = 0u; j < 6u; ++j) {
                        if (Memory::Read8(record + 64u + j) != Memory::Read8(data + j)) {
                            same = false;
                            break;
                        }
                    }
                    if (same) {
                        foundTable = 1;
                        foundIndex = static_cast<int>(i);
                        foundRecord = record;
                    }
                }
                os << " lookupTable=" << std::dec << foundTable
                   << " lookupIndex=" << foundIndex
                   << " record=0x" << std::hex << foundRecord;
                if (foundRecord != 0u) {
                    os << " recordState=0x"
                       << static_cast<uint32_t>(Memory::Read8(foundRecord + 89u));
                }

                const uint32_t special = kWudBase + 3304u;
                bool specialMatch = true;
                for (uint32_t j = 0u; j < 6u; ++j) {
                    if (Memory::Read8(special + 64u + j) != Memory::Read8(data + j)) {
                        specialMatch = false;
                        break;
                    }
                }
                os << " specialMatch=" << (specialMatch ? 1 : 0)
                   << " specialState=0x"
                   << static_cast<uint32_t>(Memory::Read8(special + 89u));
            }
        } catch (const Memory::AccessViolation&) {
            os << " snapshot=invalid";
        }
        os << std::dec << std::endl;
    }
    if (ctx && target == 0x80268B0Cu) {
        auto& os = RT_LOG("meteor-wud-state");
        os << "func_80268B0C entry lr=0x" << std::hex << ctx->lr
           << " r3=0x" << ctx->gpr[3]
           << " r4=0x" << ctx->gpr[4];
        try {
            os << " state=0x" << static_cast<uint32_t>(Memory::Read8(0x8058A16Cu))
               << " flag1765=0x" << static_cast<uint32_t>(Memory::Read8(0x8058A845u))
               << " flag1767=0x" << static_cast<uint32_t>(Memory::Read8(0x8058A847u))
               << " flag1800=0x" << static_cast<uint32_t>(Memory::Read8(0x8058A868u));
        } catch (const Memory::AccessViolation&) {
            os << " snapshot=invalid";
        }
        os << std::dec << std::endl;
    }
    if (ctx && target == 0x80274B28u) {
        RT_LOG("meteor-wud-state")
            << "BTA_DmSearch entry lr=0x" << std::hex << ctx->lr
            << " params=0x" << ctx->gpr[3]
            << " services=0x" << ctx->gpr[4]
            << " callback=0x" << ctx->gpr[5]
            << std::dec << std::endl;
    }
    if (ctx && target == 0x8027D3F0u) {
        RT_LOG("meteor-bt-remote-name")
            << "btm_initiate_rem_name lr=0x" << std::hex << ctx->lr
            << " bdaddr=0x" << ctx->gpr[3]
            << " db=0x" << ctx->gpr[4]
            << " mask=0x" << ctx->gpr[5]
            << " timeout=0x" << ctx->gpr[6]
            << " callback=0x" << ctx->gpr[7]
            << std::dec << std::endl;
    }
    // Narrow Bluetooth security diagnostic. Retail L2CAP has already parsed the
    // virtual remote's HID-control Connection Request and is rejecting it with
    // result 0x0003. Trace the exact BTM access request and the two procedure
    // helpers that can turn that request into an Authentication Requested HCI
    // command without altering any guest state.
    if (ctx && target == 0x8027FC60u) {
        RT_LOG("meteor-bt-security")
            << "l2cap-access entry lr=0x" << std::hex << ctx->lr
            << " bdaddr=0x" << ctx->gpr[3]
            << " psm=0x" << ctx->gpr[4]
            << " handle=0x" << ctx->gpr[5]
            << " originator=0x" << ctx->gpr[6]
            << " cb=0x" << ctx->gpr[7]
            << std::dec << std::endl;
    }
    if (ctx && target == 0x80281BB4u) {
        const uint32_t dev = ctx->gpr[3];
        auto& os = RT_LOG("meteor-bt-security");
        os << "execute-procedure entry lr=0x" << std::hex << ctx->lr
           << " dev=0x" << dev;
        try {
            if (dev != 0u && Memory::Contains(dev, 132u)) {
                os << " hciHandle=0x" << Memory::Read16(dev + 24u)
                   << " secFlags=0x" << static_cast<uint32_t>(Memory::Read8(dev + 118u))
                   << " secState=0x" << static_cast<uint32_t>(Memory::Read8(dev + 127u))
                   << " role=0x" << static_cast<uint32_t>(Memory::Read8(dev + 128u))
                   << " required=0x" << static_cast<uint32_t>(Memory::Read8(dev + 129u));
            }
        } catch (const Memory::AccessViolation&) {
            os << " snapshot=invalid";
        }
        os << std::dec << std::endl;
    }
    if (ctx && target == 0x802850FCu) {
        RT_LOG("meteor-bt-security")
            << "auth-request entry lr=0x" << std::hex << ctx->lr
            << " handle=0x" << ctx->gpr[3]
            << std::dec << std::endl;
    }
    if (ctx && target == 0x8028C5ACu) {
        RT_LOG("meteor-bt-security")
            << "l2cap-sec-callback entry lr=0x" << std::hex << ctx->lr
            << " bdaddr=0x" << ctx->gpr[3]
            << " ref=0x" << ctx->gpr[4]
            << " status=0x" << ctx->gpr[5]
            << std::dec << std::endl;
    }
    if (ctx && (target == 0x802771C8u || target == 0x8027730Cu)) {
        const uint32_t bdaddr = ctx->gpr[3];
        auto& os = RT_LOG("meteor-bt-hid-open");
        os << (target == 0x802771C8u ? "BTA_HhOpen" : "BTA_HhAddDev")
           << " entry lr=0x" << std::hex << ctx->lr
           << " bdaddr=0x" << bdaddr;
        try {
            if (bdaddr != 0u && Memory::Contains(bdaddr, 6u)) {
                os << " bytes=";
                for (uint32_t i = 0; i < 6u; ++i) {
                    if (i != 0u) os << ':';
                    os << static_cast<uint32_t>(Memory::Read8(bdaddr + i));
                }
            }
        } catch (const Memory::AccessViolation&) {
            os << " bytes=invalid";
        }
        os << " r4=0x" << ctx->gpr[4]
           << " r5=0x" << ctx->gpr[5]
           << " r6=0x" << ctx->gpr[6]
           << " r7=0x" << ctx->gpr[7]
           << std::dec << std::endl;
    }
    if (ctx && target == 0x8027D540u) {
        const uint32_t name = ctx->gpr[4];
        const uint32_t length = ctx->gpr[5];
        const uint32_t status = ctx->gpr[6];
        auto& os = RT_LOG("meteor-bt-remote-name");
        os << "btm_process_remote_name lr=0x" << std::hex << ctx->lr
           << " name=0x" << name
           << " length=0x" << length
           << " status=0x" << status
           << " text=";
        try {
            const uint32_t n = std::min<uint32_t>(length, 32u);
            if (name != 0u && Memory::Contains(name, n)) {
                for (uint32_t i = 0; i < n; ++i) {
                    const uint8_t c = Memory::Read8(name + i);
                    if (c == 0u) break;
                    os << (c >= 32u && c < 127u ? static_cast<char>(c) : '.');
                }
            } else {
                os << "<invalid>";
            }
        } catch (const Memory::AccessViolation&) {
            os << "<fault>";
        }
        try {
            os << " registeredCb=0x" << std::hex << Memory::Read32(0x805B9E74u);
        } catch (const Memory::AccessViolation&) {
            os << " registeredCb=invalid";
        }
        os << std::dec << std::endl;
    }
    if (ctx && target == 0x80273A28u) {
        RT_LOG("meteor-bt-search-flow")
            << "bta_dm_remname_cback lr=0x" << std::hex << ctx->lr
            << " result=0x" << ctx->gpr[3]
            << std::dec << std::endl;
    }
    if (ctx && target == 0x80272F00u) {
        const uint32_t msg = ctx->gpr[3];
        auto& os = RT_LOG("meteor-bt-search-flow");
        os << "bta_dm_rmt_name lr=0x" << std::hex << ctx->lr
           << " msg=0x" << msg;
        try {
            if (msg != 0u && Memory::Contains(msg, 0x110u)) {
                os << " event=0x" << Memory::Read16(msg)
                   << " bdaddr=";
                for (uint32_t i = 0u; i < 6u; ++i) {
                    if (i != 0u) os << ':';
                    os << static_cast<uint32_t>(Memory::Read8(msg + 8u + i));
                }
                os << " name=";
                for (uint32_t i = 0u; i < 64u; ++i) {
                    const uint8_t c = Memory::Read8(msg + 0x0Eu + i);
                    if (c == 0u) break;
                    os << (c >= 32u && c < 127u ? static_cast<char>(c) : '.');
                }
            }
        } catch (const Memory::AccessViolation&) {
            os << " snapshot=invalid";
        }
        os << std::dec << std::endl;
    }
    if (ctx && target == 0x8026BE3Cu) {
        const bool isWudSearchCallback = true;
        const uint32_t event = ctx->gpr[3];
        const uint32_t data = ctx->gpr[4];
        auto& os = RT_LOG("meteor-bt-search-flow");
        os << "WUD-search-callback"
           << " lr=0x" << std::hex << ctx->lr
           << " event=0x" << event
           << " data=0x" << data;
        try {
            os << " wudState=0x" << static_cast<uint32_t>(Memory::Read8(0x8058A16Cu))
               << " syncMode=0x" << static_cast<uint32_t>(Memory::Read8(0x8058A845u))
               << " searchMode=0x" << static_cast<uint32_t>(Memory::Read8(0x8058A847u));
            if (data != 0u && Memory::Contains(data, 16u)) {
                os << " bytes=";
                for (uint32_t i = 0u; i < 16u; ++i) {
                    if (i != 0u) os << ':';
                    os << static_cast<uint32_t>(Memory::Read8(data + i));
                }
            }
            if (isWudSearchCallback && event == 2u && data != 0u &&
                Memory::Contains(data, 0x104u)) {
                os << " bdaddr=";
                for (uint32_t i = 0u; i < 6u; ++i) {
                    if (i != 0u) os << ':';
                    os << static_cast<uint32_t>(Memory::Read8(data + i));
                }
                os << " name=";
                for (uint32_t i = 0u; i < 64u; ++i) {
                    const uint8_t c = Memory::Read8(data + 6u + i);
                    if (c == 0u) break;
                    os << (c >= 32u && c < 127u ? static_cast<char>(c) : '.');
                }
                os << " services=0x" << Memory::Read32(data + 0x100u);
            }
        } catch (const Memory::AccessViolation&) {
            os << " snapshot=invalid";
        }
        os << std::dec << std::endl;
    }

    if (ctx && target == 0x802797DCu) {
        try {
            auto& os = RT_LOG("meteor-bt-disconnect-by-address");
            os << "callerLr=0x" << std::hex << ctx->lr
               << " addrPtr=0x" << ctx->gpr[3];
            if (ctx->gpr[3] != 0u && Memory::Contains(ctx->gpr[3], 6u)) {
                os << " bdaddr=";
                for (uint32_t i = 0u; i < 6u; ++i) {
                    if (i != 0u) {
                        os << ':';
                    }
                    os << static_cast<uint32_t>(Memory::Read8(ctx->gpr[3] + i));
                }
            }
            os << " state=0x" << static_cast<uint32_t>(Memory::Read8(0x805B8E46u))
               << " retry=0x" << static_cast<uint32_t>(Memory::Read8(0x805B8E47u))
               << std::dec << std::endl;
        } catch (const Memory::AccessViolation&) {
        }
    }

    if (ctx && target == 0x802849D8u && ctx->gpr[4] == 19u) {
        try {
            RT_LOG("meteor-bt-disconnect-request")
                << "lr=0x" << std::hex << ctx->lr
                << " handle=0x" << ctx->gpr[3]
                << " reason=0x" << ctx->gpr[4]
                << " state=0x" << static_cast<uint32_t>(Memory::Read8(0x805B8E46u))
                << " retry=0x" << static_cast<uint32_t>(Memory::Read8(0x805B8E47u))
                << std::dec << std::endl;
        } catch (const Memory::AccessViolation&) {
        }
    }

    if (ctx && (target == 0x80263EE8u || target == 0x80264278u || target == 0x80264794u)) {
        static std::atomic<uint32_t> wpadReportTraceCount{0u};
        const uint32_t n = wpadReportTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (n < 96u) {
            try {
                const uint32_t chan = ctx->gpr[3] & 3u;
                const uint32_t control = Memory::Read32(0x805869F0u + chan * 4u);
                auto& os = RT_LOG("meteor-wpad-report");
                os << "target=0x" << std::hex << target
                   << " lr=0x" << ctx->lr
                   << " chan=0x" << chan
                   << " data=0x" << ctx->gpr[4]
                   << " control=0x" << control;
                if (control != 0u && Memory::Contains(control, 0x98Cu)) {
                    os << " probe=0x" << Memory::Read32(control + 0x8BCu)
                       << " probeDev=0x" << static_cast<uint32_t>(Memory::Read8(control + 0x8C1u))
                       << " pendingCb=0x" << Memory::Read32(control + 0x89Cu)
                       << " pendingReport=0x" << static_cast<uint32_t>(Memory::Read8(control + 0x987u));
                }
                if (ctx->gpr[4] != 0u && Memory::Contains(ctx->gpr[4], 8u)) {
                    os << " bytes=";
                    for (uint32_t i = 0u; i < 8u; ++i) {
                        if (i != 0u) os << ':';
                        os << static_cast<uint32_t>(Memory::Read8(ctx->gpr[4] + i));
                    }
                }
                os << std::dec << std::endl;
            } catch (const Memory::AccessViolation&) {
                RT_LOG("meteor-wpad-report") << "snapshot read failed" << std::endl;
            }
        }
    }

    if (ctx && (target == 0x80262000u || target == 0x8027727Cu)) {
        static std::atomic<uint32_t> wpadSendTraceCount{0u};
        const uint32_t n = wpadSendTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (n < 96u) {
            try {
                auto& os = RT_LOG("meteor-wpad-send");
                os << "target=0x" << std::hex << target
                   << " lr=0x" << ctx->lr
                   << " r3=0x" << ctx->gpr[3]
                   << " r4=0x" << ctx->gpr[4];
                if (target == 0x80262000u && ctx->gpr[4] != 0u && Memory::Contains(ctx->gpr[4], 0x30u)) {
                    const uint32_t d = ctx->gpr[4];
                    os << " descReport=0x" << static_cast<uint32_t>(Memory::Read8(d))
                       << " descLen=0x" << Memory::Read16(d + 0x1Au)
                       << " cb=0x" << Memory::Read32(d + 0x2Cu);
                } else if (target == 0x8027727Cu && ctx->gpr[4] != 0u && Memory::Contains(ctx->gpr[4], 0x20u)) {
                    const uint32_t p = ctx->gpr[4];
                    os << " packet=";
                    for (uint32_t i = 0u; i < 16u; ++i) {
                        if (i != 0u) os << ':';
                        os << static_cast<uint32_t>(Memory::Read8(p + i));
                    }
                }
                os << std::dec << std::endl;
            } catch (const Memory::AccessViolation&) {
                RT_LOG("meteor-wpad-send") << "snapshot read failed" << std::endl;
            }
        }
    }
    if (ctx && target == 0x80277474u) {
        static std::atomic<uint32_t> hhStateTraceCount{0u};
        const uint32_t n = hhStateTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (n < 128u) {
            try {
                const uint32_t dev = ctx->gpr[3];
                const uint32_t event = ctx->gpr[4];
                const uint32_t msg = ctx->gpr[5];
                auto& os = RT_LOG("meteor-bta-hh-state");
                os << "event=0x" << std::hex << event
                   << " dev=0x" << dev
                   << " msg=0x" << msg;
                if (dev != 0u && Memory::Contains(dev, 0x20u)) {
                    const uint32_t oldState = Memory::Read8(dev + 0x1Cu);
                    os << " oldState=0x" << oldState;
                    const uint32_t eventIndex = event & 0xFFu;
                    if (oldState >= 1u && oldState <= 4u && eventIndex < 0x40u) {
                        const uint32_t transitionTable = Memory::Read32(0x80366D68u + (oldState - 1u) * 4u);
                        if (transitionTable != 0u && Memory::Contains(transitionTable + eventIndex * 2u, 2u)) {
                            const uint32_t action = Memory::Read8(transitionTable + eventIndex * 2u);
                            const uint32_t newState = Memory::Read8(transitionTable + eventIndex * 2u + 1u);
                            os << " trans=0x" << transitionTable
                               << " action=0x" << action
                               << " newState=0x" << newState;
                            if (action < 0x20u && Memory::Contains(0x80366CF0u + action * 4u, 4u)) {
                                os << " actionTarget=0x" << Memory::Read32(0x80366CF0u + action * 4u);
                            }
                        }
                    }
                }
                if (msg != 0u && Memory::Contains(msg, 0x14u)) {
                    os << " msgEvent=0x" << Memory::Read16(msg)
                       << " msgDev=0x" << Memory::Read16(msg + 6u)
                       << " msgType=0x" << static_cast<uint32_t>(Memory::Read8(msg + 8u))
                       << " payload=0x" << Memory::Read32(msg + 0x10u);
                }
                os << std::dec << std::endl;
            } catch (const Memory::AccessViolation&) {
                RT_LOG("meteor-bta-hh-state") << "snapshot read failed" << std::endl;
            }
        }
    }
    if (MeteorRuntimeTraceEnabledLocal() && ctx && target == 0x8026D0C8u) {
        RT_LOG("meteor-bt-callback-lifetime")
            << "guest-entry id=" << std::dec << NandIosCurrentCallbackDispatchId()
            << " arg=0x" << std::hex << ctx->gpr[4]
            << " result=0x" << ctx->gpr[3]
            << " lr=0x" << ctx->lr
            << std::dec << std::endl;
    }

    if (ctx && MeteorRuntimeTraceEnabledLocal()) {
        Meteor_BtUsbObserveCallBoundary(
            target,
            ctx->lr,
            g_meteorLastCallTarget.load(std::memory_order_relaxed),
            g_meteorLastCallLr.load(std::memory_order_relaxed));
    }

    if (MeteorRuntimeTraceEnabledLocal() && ctx && target == 0x80252C14u) {
        if (ctx->lr == 0x8026D1E8u) {
            RT_LOG("meteor-bt-callback-lifetime")
                << "guest-final-free id=" << std::dec << NandIosCurrentCallbackDispatchId()
                << " ptr=0x" << std::hex << ctx->gpr[4]
                << " heap=0x" << ctx->gpr[3]
                << std::dec << std::endl;
        }
        if (MeteorRuntimeTraceEnabledLocal()) {
            Meteor_BtUsbObserveGuestHeapFree(ctx->gpr[4], ctx->lr);
        }
        // Narrow bring-up diagnostic for the BTE USB wrapper lifetime. The live
        // ep81 callback wrappers are allocated from the IPC arena near
        // 0x97FC1E00..0x97FC2400. Record the exact guest caller that frees one of
        // those blocks; the later 0x20202020 payload pattern is recycled memory,
        // but 0x80252C14 itself does not fill the payload with spaces.
        const uint32_t ptr = ctx->gpr[4];
        if (ptr >= 0x97FC1E00u && ptr < 0x97FC2400u) {
            static std::atomic<uint32_t> ipcArenaFreeTraceCount{0u};
            const uint32_t n = ipcArenaFreeTraceCount.fetch_add(1u, std::memory_order_relaxed);
            if (n < 256u) {
                try {
                    RT_LOG("meteor-ipc-arena-free")
                        << "call#" << std::dec << (n + 1u)
                        << " lr=0x" << std::hex << ctx->lr
                        << " heap=0x" << ctx->gpr[3]
                        << " ptr=0x" << ptr
                        << " word0=0x" << (Memory::Contains(ptr, 4u) ? Memory::Read32(ptr) : 0u)
                        << " word4=0x" << (Memory::Contains(ptr, 8u) ? Memory::Read32(ptr + 4u) : 0u)
                        << std::dec << std::endl;
                } catch (const Memory::AccessViolation&) {
                    RT_LOG("meteor-ipc-arena-free") << "snapshot read failed" << std::endl;
                }
            }
        }
    }

    if (ctx && (target == 0x80004388u || target == 0x8000443Cu)) {
        if (MeteorRuntimeTraceEnabledLocal()) {
            Meteor_BtUsbObserveMemset(ctx->gpr[3], ctx->gpr[4], ctx->gpr[5], ctx->lr);
        }
    }
    if (ctx && (target == 0x80252A0Cu || target == 0x80252C10u)) {
        if (MeteorRuntimeTraceEnabledLocal()) {
            Meteor_BtUsbObserveHeapAllocCall(ctx->gpr[3], ctx->gpr[4], ctx->gpr[5], ctx->lr);
        }
    }

    if (MeteorRuntimeTraceEnabledLocal() && ctx && target == 0x8027A524u) {
        static std::atomic<uint32_t> btStateEventTraceCount{0u};
        const uint32_t n = btStateEventTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (n < 16u) {
            try {
                const uint32_t msg = ctx->gpr[3];
                auto& os = RT_LOG("meteor-bt-state-event");
                os << "call#" << std::dec << (n + 1u)
                   << " lr=0x" << std::hex << ctx->lr
                   << " msg=0x" << msg;
                if (msg != 0u && Memory::Contains(msg, 0x20u)) {
                    os << " word10=0x" << Memory::Read32(msg + 0x10u)
                       << " type14=0x" << Memory::Read16(msg + 0x14u)
                       << " word16=0x" << Memory::Read16(msg + 0x16u)
                       << " word18=0x" << Memory::Read32(msg + 0x18u)
                       << " state=0x" << static_cast<uint32_t>(Memory::Read8(0x805B8E46u))
                       << " retry=0x" << static_cast<uint32_t>(Memory::Read8(0x805B8E47u));
                }
                os << std::dec << std::endl;
            } catch (const Memory::AccessViolation&) {
                RT_LOG("meteor-bt-state-event") << "snapshot read failed" << std::endl;
            }
        }
    }

    if (ctx && MeteorRuntimeTraceEnabledLocal()) {
        static std::atomic<uint32_t> serviceInitTraceMask{0u};
        uint32_t traceBit = 0u;
        const char* traceName = nullptr;
        switch (target) {
        case 0x802B1D00u: traceBit = 1u << 0; traceName = "B1D00"; break;
        case 0x802A915Cu: traceBit = 1u << 1; traceName = "A915C"; break;
        case 0x802A924Cu: traceBit = 1u << 2; traceName = "A924C"; break;
        case 0x802AA478u: traceBit = 1u << 3; traceName = "AA478"; break;
        case 0x802A9410u: traceBit = 1u << 4; traceName = "A9410"; break;
        case 0x802B1C58u: traceBit = 1u << 5; traceName = "B1C58"; break;
        case 0x8029F11Cu: traceBit = 1u << 6; traceName = "9F11C"; break;
        default: break;
        }
        if (traceBit != 0u &&
            (serviceInitTraceMask.fetch_or(traceBit, std::memory_order_relaxed) & traceBit) == 0u) {
            try {
                auto& os = RT_LOG("meteor-service-init");
                os << traceName
                   << " target=0x" << std::hex << target
                   << " lr=0x" << ctx->lr
                   << " r3=0x" << ctx->gpr[3]
                   << " r4=0x" << ctx->gpr[4]
                   << " r5=0x" << ctx->gpr[5];
                constexpr uint32_t kServiceSlots = 0x805C7240u;
                for (uint32_t i = 0; i < 4u; ++i) {
                    const uint32_t slot = kServiceSlots + i * 0x10u;
                    os << " s" << std::dec << i << "=0x" << std::hex << Memory::Read32(slot);
                }
                os << std::dec << std::endl;
            } catch (const Memory::AccessViolation&) {
                RT_LOG("meteor-service-init") << traceName << " snapshot read failed" << std::endl;
            }
        }
    }

    if (MeteorRuntimeTraceEnabledLocal() && ctx && target >= 0x80290000u && target < 0x802B0000u) {
        try {
            const uint32_t globalObject = Memory::Read32(0x805C136Cu);
            if (globalObject != 0u && ctx->gpr[3] == globalObject) {
                static std::array<std::atomic<uint64_t>, 32> seenObjectCallKeys{};
                const uint64_t key = (static_cast<uint64_t>(target) << 32u) | ctx->lr;
                bool seen = false;
                for (auto& slot : seenObjectCallKeys) {
                    if (slot.load(std::memory_order_relaxed) == key) {
                        seen = true;
                        break;
                    }
                }
                if (!seen) {
                    for (auto& slot : seenObjectCallKeys) {
                        uint64_t expected = 0u;
                        if (slot.compare_exchange_strong(expected, key, std::memory_order_relaxed)) {
                            RT_LOG("meteor-async-object-call")
                                << "target=0x" << std::hex << target
                                << " lr=0x" << ctx->lr
                                << " obj=0x" << globalObject
                                << " state0=" << std::dec << static_cast<uint32_t>(Memory::Read8(globalObject))
                                << " state1=" << static_cast<uint32_t>(Memory::Read8(globalObject + 1u))
                                << " r4=0x" << std::hex << ctx->gpr[4]
                                << " r5=0x" << ctx->gpr[5]
                                << " r6=0x" << ctx->gpr[6]
                                << std::dec << std::endl;
                            break;
                        }
                    }
                }
            }
        } catch (const Memory::AccessViolation&) {
        }
    }

    if (MeteorRuntimeTraceEnabledLocal()) {
    switch (target) {
    case 0x8015A800u: g_meteorCount8015A800.fetch_add(1u, std::memory_order_relaxed); break;
    case 0x8015A06Cu: g_meteorCount8015A06C.fetch_add(1u, std::memory_order_relaxed); break;
    case 0x80257520u: {
        g_meteorCount80257520.fetch_add(1u, std::memory_order_relaxed);
        size_t bucket = 9u;
        switch (ctx ? ctx->lr : 0u) {
        case 0x80027798u: bucket = 0u; break;
        case 0x8015A0F8u: bucket = 1u; break;
        case 0x801A031Cu: bucket = 2u; break;
        case 0x8020672Cu: bucket = 3u; break;
        case 0x80206860u: bucket = 4u; break;
        case 0x80219F24u: bucket = 5u; break;
        case 0x80219F44u: bucket = 6u; break;
        case 0x80219F64u: bucket = 7u; break;
        case 0x802D847Cu: bucket = 8u; break;
        default: break;
        }
        g_meteor80257520ByLr[bucket].fetch_add(1u, std::memory_order_relaxed);
        break;
    }
    case 0x8025797Cu: g_meteorCount8025797C.fetch_add(1u, std::memory_order_relaxed); break;
    case 0x80257810u: g_meteorCount80257810.fetch_add(1u, std::memory_order_relaxed); break;
    case 0x802AB688u: {
        g_meteorCount802AB688.fetch_add(1u, std::memory_order_relaxed);
        if (ctx && g_meteorPeFinishDelivered.load(std::memory_order_acquire)) {
            static std::array<std::atomic<uint32_t>, 8> seenAb688Objects{};
            const uint32_t object = ctx->gpr[3];
            bool seen = false;
            for (auto& slot : seenAb688Objects) {
                if (slot.load(std::memory_order_relaxed) == object) {
                    seen = true;
                    break;
                }
            }
            if (!seen) {
                for (auto& slot : seenAb688Objects) {
                    uint32_t expected = 0u;
                    if (slot.compare_exchange_strong(expected, object, std::memory_order_relaxed)) {
                        try {
                            const uint32_t inner = Memory::Read32(object + 40u);
                            auto& os = RT_LOG("meteor-ab688-object");
                            os << "obj=0x" << std::hex << object
                               << " state1=" << std::dec << static_cast<uint32_t>(Memory::Read8(object + 1u))
                               << " state4=" << static_cast<uint32_t>(Memory::Read8(object + 4u))
                               << " index=0x" << std::hex << Memory::Read32(object + 32u)
                               << " gate=0x" << Memory::Read32(object + 36u)
                               << " inner=0x" << inner
                               << " aux=0x" << Memory::Read32(object + 44u);
                            if (inner != 0u) {
                                os << " innerState1=" << std::dec
                                   << static_cast<uint32_t>(Memory::Read8(inner + 1u));
                            }
                            os << std::endl;
                        } catch (const Memory::AccessViolation&) {
                            RT_LOG("meteor-ab688-object") << "read failed obj=0x" << std::hex << object
                                                         << std::dec << std::endl;
                        }
                        break;
                    }
                }
            }
        }
        break;
    }
    case 0x80210990u: {
        g_meteorCount80210990.fetch_add(1u, std::memory_order_relaxed);
        try {
            const uint32_t currentThread = Memory::Read32(0x800000E4u);
            const uint32_t previousThread = g_meteorSchedulerCurrentThread.exchange(currentThread, std::memory_order_relaxed);
            if (previousThread != 0u && previousThread != currentThread) {
                g_meteorSchedulerThreadChanges.fetch_add(1u, std::memory_order_relaxed);
            }
        } catch (const Memory::AccessViolation&) {
        }
        break;
    }
    case 0x8020635Cu: g_meteorCount8020635C.fetch_add(1u, std::memory_order_relaxed); break;
    case 0x802061C8u: g_meteorCount802061C8.fetch_add(1u, std::memory_order_relaxed); break;
    case 0x802065CCu: {
        g_meteorCount802065CC.fetch_add(1u, std::memory_order_relaxed);
        size_t bucket = 13u;
        switch (ctx ? ctx->lr : 0u) {
        case 0x80027DF4u: bucket = 0u; break;
        case 0x80027E48u: bucket = 1u; break;
        case 0x80027E50u: bucket = 2u; break;
        case 0x80028590u: bucket = 3u; break;
        case 0x80028D40u: bucket = 4u; break;
        case 0x80028E80u: bucket = 5u; break;
        case 0x8002D398u: bucket = 6u; break;
        case 0x80159FBCu: bucket = 7u; break;
        case 0x8015A030u: bucket = 8u; break;
        case 0x801D3798u: bucket = 9u; break;
        case 0x801D3850u: bucket = 10u; break;
        case 0x801D3900u: bucket = 11u; break;
        case 0x801D3964u: bucket = 12u; break;
        default: break;
        }
        g_meteor802065CCByLr[bucket].fetch_add(1u, std::memory_order_relaxed);
        break;
    }
    case 0x80206840u: {
        size_t bucket = 2u;
        switch (ctx ? ctx->lr : 0u) {
        case 0x802061DCu: bucket = 0u; break;
        case 0x802065ECu: bucket = 1u; break;
        default: break;
        }
        g_meteor80206840ByLr[bucket].fetch_add(1u, std::memory_order_relaxed);
        break;
    }
    case 0x8029EFA8u: {
        g_meteorCount8029EFA8.fetch_add(1u, std::memory_order_relaxed);
        static std::atomic<bool> loggedFirstPumpContext{false};
        if (ctx && !loggedFirstPumpContext.exchange(true, std::memory_order_relaxed)) {
            try {
                RT_LOG("meteor-pump-context")
                    << "target=0x8029efa8 lr=0x" << std::hex << ctx->lr
                    << " currentThread=0x" << Memory::Read32(0x800000E4u)
                    << " runningContext=0x" << Memory::Read32(0x800000D4u)
                    << " r1=0x" << ctx->gpr[1]
                    << " r3=0x" << ctx->gpr[3]
                    << std::dec << std::endl;
            } catch (const Memory::AccessViolation&) {
            }
        }
        break;
    }
    case 0x8029EFD0u: g_meteorCount8029EFD0.fetch_add(1u, std::memory_order_relaxed); break;
    case 0x802A1914u: g_meteorCount802A1914.fetch_add(1u, std::memory_order_relaxed); break;
    case 0x802A15DCu: g_meteorCount802A15DC.fetch_add(1u, std::memory_order_relaxed); break;
    case 0x802AA640u: {
        g_meteorCount802AA640.fetch_add(1u, std::memory_order_relaxed);
        static std::atomic<bool> loggedFirstGcdService{false};
        if (ctx && !loggedFirstGcdService.exchange(true, std::memory_order_relaxed)) {
            RT_LOG("meteor-gcd-service") << "target=0x802aa640 lr=0x" << std::hex << ctx->lr
                                         << " r1=0x" << ctx->gpr[1]
                                         << std::dec << std::endl;
        }
        break;
    }
    case 0x802AA4E4u: {
        g_meteorCount802AA4E4.fetch_add(1u, std::memory_order_relaxed);
        static std::atomic<bool> loggedFirstGcdPoll{false};
        if (ctx && !loggedFirstGcdPoll.exchange(true, std::memory_order_relaxed)) {
            RT_LOG("meteor-gcd-poll") << "target=0x802aa4e4 lr=0x" << std::hex << ctx->lr
                                      << " object=0x" << ctx->gpr[3]
                                      << std::dec << std::endl;
        }
        break;
    }
    case 0x80211238u:
    case 0x802114D0u: {
        if (!ctx) {
            break;
        }
        const uint32_t thread = ctx->gpr[3];
        constexpr uint32_t kPhase2Thread = 0x805D5A68u;
        constexpr uint32_t kPhase4Thread = 0x805D7D80u;
        constexpr uint32_t kPhase6Thread = 0x805D4408u;
        const bool initResumeCall =
            target == 0x80211238u &&
            (ctx->lr == 0x802B1C10u || ctx->lr == 0x802B1C18u || ctx->lr == 0x802B1C20u);
        if (!initResumeCall && thread != kPhase2Thread && thread != kPhase4Thread && thread != kPhase6Thread) {
            break;
        }
        if (initResumeCall) {
            static std::atomic<uint32_t> loggedInitResumeSites{0u};
            const uint32_t siteBit = ctx->lr == 0x802B1C10u ? 1u : (ctx->lr == 0x802B1C18u ? 2u : 4u);
            if ((loggedInitResumeSites.fetch_or(siteBit, std::memory_order_relaxed) & siteBit) == 0u) {
                RT_LOG("meteor-init-resume")
                    << "lr=0x" << std::hex << ctx->lr
                    << " r3=0x" << thread
                    << " r31=0x" << ctx->gpr[31]
                    << " sp=0x" << ctx->gpr[1]
                    << " currentThread=0x" << Memory::Read32(0x800000E4u)
                    << std::dec << std::endl;
            }
        }
        static std::atomic<uint32_t> loggedThreadCtlMask{0u};
        uint32_t workerBit = thread == kPhase2Thread ? 0u : (thread == kPhase4Thread ? 1u : 2u);
        uint32_t opBit = target == 0x80211238u ? 0u : 3u;
        const uint32_t bit = 1u << (workerBit + opBit);
        if ((loggedThreadCtlMask.fetch_or(bit, std::memory_order_relaxed) & bit) == 0u) {
            try {
                RT_LOG("meteor-worker-threadctl")
                    << (target == 0x80211238u ? "resume" : "suspend")
                    << " target=0x" << std::hex << target
                    << " lr=0x" << ctx->lr
                    << " thread=0x" << thread
                    << " currentThread=0x" << Memory::Read32(0x800000E4u)
                    << std::dec
                    << " state=" << Memory::Read16(thread + 0x2C8u)
                    << " suspendBefore=" << static_cast<int32_t>(Memory::Read32(thread + 0x2CCu))
                    << " prio=" << static_cast<int32_t>(Memory::Read32(thread + 0x2D0u))
                    << " basePrio=" << static_cast<int32_t>(Memory::Read32(thread + 0x2D4u))
                    << std::endl;
            } catch (const Memory::AccessViolation&) {
            }
        }
        break;
    }
    case 0x802B1898u:
    case 0x802AE81Cu:
    case 0x8029F0F8u: {
        static std::atomic<uint32_t> loggedRetailPumpThreadMask{0u};
        uint32_t bit = 0u;
        const char* name = nullptr;
        if (target == 0x802B1898u) {
            bit = 1u;
            name = "thread-entry";
        } else if (target == 0x802AE81Cu) {
            bit = 2u;
            name = "phase4-dispatch";
        } else {
            bit = 4u;
            name = "pump-callback";
        }
        if (ctx && (loggedRetailPumpThreadMask.fetch_or(bit, std::memory_order_relaxed) & bit) == 0u) {
            try {
                RT_LOG("meteor-retail-pump-thread")
                    << name
                    << " target=0x" << std::hex << target
                    << " lr=0x" << ctx->lr
                    << " currentThread=0x" << Memory::Read32(0x800000E4u)
                    << " runningContext=0x" << Memory::Read32(0x800000D4u)
                    << " r1=0x" << ctx->gpr[1]
                    << std::dec << std::endl;
            } catch (const Memory::AccessViolation&) {
            }
        }
        break;
    }
    default: break;
    }
    }
    if (MeteorRuntimeTraceEnabledLocal()) {
        g_meteorLastCallTarget.store(target, std::memory_order_relaxed);
        g_meteorLastCallLr.store(ctx ? ctx->lr : 0u, std::memory_order_relaxed);
        g_meteorCallSerial.fetch_add(1u, std::memory_order_release);
        StartMeteorCallWatchdog();
    }
}

void Meteor_RuntimeReturnCheckpoint(uint32_t target, CpuContext* ctx) noexcept {
    if (!MeteorRuntimeTraceEnabledLocal()) {
        return;
    }

    if (MeteorRuntimeTraceEnabledLocal() && ctx && target == 0x8030741Cu && ctx->lr == 0x801D508Cu) {
        try {
            const uint32_t work = ctx->gpr[23];
            if (work != 0u && Memory::Contains(work + 4u, sizeof(uint32_t))) {
                const uint32_t tag = Memory::Read32(work + 4u);
                ThpLatencyEnsureTag(tag).decodeEndUs.store(MeteorSteadyMicros(),
                                                           std::memory_order_relaxed);
            }
        } catch (const Memory::AccessViolation&) {
            RT_LOG("meteor-thp-latency") << "decode-end-snapshot=invalid" << std::endl;
        }
    }
    if (MeteorRuntimeTraceEnabledLocal() && ctx && (target == 0x8027FC60u || target == 0x80281BB4u || target == 0x802850FCu ||
                target == 0x802771C8u || target == 0x8027730Cu)) {
        RT_LOG("meteor-bt-security")
            << "return target=0x" << std::hex << target
            << " lr=0x" << ctx->lr
            << " r3=0x" << ctx->gpr[3]
            << std::dec << std::endl;
    }
    if (ctx && target == 0x80252C14u) {
        if (MeteorRuntimeTraceEnabledLocal()) {
            Meteor_BtUsbObservePostHeapFree(ctx->lr);
        }
    }
    if (ctx && (target == 0x80252A0Cu || target == 0x80252C10u)) {
        if (MeteorRuntimeTraceEnabledLocal()) {
            Meteor_BtUsbObserveHeapAllocReturn(ctx->gpr[3], ctx->lr);
        }
    }
    if constexpr (kEnableThpPerfProfile) {
        ThpPerfEndTrackedCall(target);
        if (ctx && target == 0x8030741Cu && ctx->lr == 0x801D508Cu) {
            ThpPerfFinishFrame();
        }
    }
}

bool Meteor_RuntimeLoopCheckpointRequired(uint32_t guestPc) noexcept {
    const uint64_t loopSerial = ++g_meteorLocalLoopSerial;
    // The full hook below has functional work only on service boundaries or
    // in the two scheduler idle loops. Avoid register spills/reloads for empty
    // checkpoints throughout gameplay, not just in the movie decoder. Queue
    // polls still reach every original PE/audio/VI service boundary.
    const bool required = meteor::NeedsLoopCheckpoint(
        loopSerial, guestPc, MeteorRuntimeTraceEnabledLocal());
    if (required) {
        g_meteorArmedLoopSerial = loopSerial;
        g_meteorArmedLoopPc = guestPc;
    }
    return required;
}

void Meteor_RuntimeLoopCheckpoint(uint32_t guestPc, CpuContext* ctx) noexcept {
    if (!ctx) {
        return;
    }

    uint64_t loopSerial = 0u;
    if (g_meteorArmedLoopSerial != 0u && g_meteorArmedLoopPc == guestPc) {
        loopSerial = g_meteorArmedLoopSerial;
        g_meteorArmedLoopSerial = 0u;
        g_meteorArmedLoopPc = 0u;
    } else {
        // Native/runtime callers can invoke ApplyRuntimeLoopOptions directly and
        // therefore do not pass through the generated pre-spill gate.
        loopSerial = ++g_meteorLocalLoopSerial;
    }

    meteor::LoopServiceCadence::Due serviceDue{};
    if (!VI_HLE_IsAdvancingRetrace() &&
        meteor::NeedsLoopCheckpoint(loopSerial, guestPc, false)) {
        serviceDue = g_meteorLoopServiceCadence.Poll(loopSerial, MeteorSteadyNanos());
    }

    // The watchdog needs only a recent progress sample, not a globally atomic
    // write for every CFG backedge.  Publish at the same 0x4000 cadence already
    // used by the heavy asynchronous-hardware service.  `loopSerial` itself is
    // still incremented on every backedge. Original instruction-count service
    // boundaries remain available alongside the elapsed-time polls.
    if ((loopSerial & 0x3FFFu) == 0u) {
        g_meteorLastLoopPc.store(guestPc, std::memory_order_relaxed);
        g_meteorLastLoopLr.store(ctx->lr, std::memory_order_relaxed);
        g_meteorLoopSerial.store(loopSerial, std::memory_order_release);
    }

    // PE FINISH is asynchronous hardware too, but unlike VI/alarm service it is
    // commonly a per-frame completion edge: GXDrawDone sleeps the render thread
    // until the BP 0x45 FINISH condition interrupts it.  Once normal worker/BTE
    // threads are live RDSPAF may never revisit SelectThread's idle backedge, so
    // polling FINISH only from that idle hook can leave the condition pending
    // forever while the main thread sleeps on r13-0x6120.  Loop checkpoints are
    // interrupt-safe translated backedges; sample FINISH at a lightweight cadence
    // independent from the heavier VI/alarm/IOS pump below.  Delivery itself uses
    // a private interrupt context and WakeupThreadNoReschedule.  The interrupt
    // return service below consumes any newly-published reschedule request before
    // returning to the interrupted translated backedge.
    if ((loopSerial & 0xFFu) == 0u && !VI_HLE_IsAdvancingRetrace() &&
        (ctx->msr & 0x00008000u) != 0u) {
        const uint64_t thpPeBeginNs = g_thpPerfFrame.active ? MeteorSteadyNanos() : 0u;
        ConfigureMeteorSchedulerWaitLayouts();
        OS_HLE_ApplyInterruptStateFromMsr(ctx->msr);
        DeliverMeteorPeFinishInterrupt(ctx);
        TryMeteorInterruptReturnReschedule(ctx);
        if (thpPeBeginNs != 0u) {
            g_thpPerfFrame.peServiceNs += MeteorSteadyNanos() - thpPeBeginNs;
        }
    }

    // AID DMA runs every 3 ms. Keep the 0x2000 backedge opportunities and
    // also poll after 1 ms of host time, including while the scheduler is idle.
    // The device itself delivers only interrupts whose deadline has elapsed.
    if (serviceDue.audio && !VI_HLE_IsAdvancingRetrace()) {
        OS_HLE_ApplyInterruptStateFromMsr(ctx->msr);
        Audio_HLE_PollDeferred();
        TryMeteorInterruptReturnReschedule(ctx);
    }

    // VI retrace is asynchronous hardware. Once the guest leaves the scheduler
    // idle loop it can spend long stretches inside translated hot loops while a
    // runnable main thread exists. Poll by time as well as backedge count so
    // different menu/combat workloads cannot delay the same hardware timers.
    // Deliver only *due* retraces on an interrupt copy. This
    // wakes VIWaitForRetrace queues exactly like the hardware IRQ without pumping
    // any title service (GCD/MFS) directly. The real phase worker remains the code
    // that calls the retail service dispatcher after it is woken.
    if (serviceDue.async && !VI_HLE_IsAdvancingRetrace()) {
        const uint64_t thpAsyncBeginNs = g_thpPerfFrame.active ? MeteorSteadyNanos() : 0u;
        ConfigureMeteorSchedulerWaitLayouts();
        // RDSPAF's OSDisable/Enable/RestoreInterrupts bodies are still translated,
        // so they update the guest MSR directly while the shared HLE interrupt
        // latch is otherwise only synchronized by HLE context/fiber restores.
        // At this asynchronous hardware checkpoint, the resident CpuContext is
        // authoritative: mirror EE before asking asynchronous hardware whether
        // an IRQ may fire. Decrementer/OSAlarm delivery belongs here for the
        // same reason as VI: RDSPAF can sit in translated polling loops while
        // periodic alarms are supposed to interrupt it (notably the BTE tick
        // installed by 0x80271918 via OSSetPeriodicAlarm 0x802087DC).
        OS_HLE_ApplyInterruptStateFromMsr(ctx->msr);
        VI_HLE_ProcessRetracesDeferred(4);
        OS_HLE_ProcessAlarmsDeferred(8);
        // IOS/USB completions are interrupt-driven on hardware. Never enter a
        // guest IOS callback while EE is clear: RDSPAF's IPC heap allocator uses
        // OSDisableInterrupts around its free-list mutations, and re-entering a
        // BT completion from this loop checkpoint in that critical section can
        // make the allocator coalesce a live callback wrapper into a free block.
        if (OS_HLE_InterruptsEnabled()) {
            // This checkpoint can sit in the middle of any translated routine.
            // The generated backedge spills volatile guest registers into `ctx`
            // before calling us and reloads them afterwards. Running the IOS/USB
            // completion on that same context lets the callback/free path clobber
            // the interrupted routine's r0/r3-r7/CR state. That was observed as
            // a freshly rearmed ep81 wrapper being overwritten immediately after
            // the completion returned. Model an external interrupt instead: run
            // the asynchronous side work on a private register file seeded from
            // the interrupted thread, while deferring scheduler switches until
            // the side-call has completely unwound.
            GuestInterruptCallbackContext interrupt;
            CpuContext* interruptCpu = interrupt.get();
            OS_HLE_BeginDeferredGuestCallbacks();
            try {
                const HostStallTrace trace("ios-service");
                Meteor_BtUsbPump(interruptCpu);
            } catch (...) {
                OS_HLE_EndDeferredGuestCallbacks();
                throw;
            }
            OS_HLE_EndDeferredGuestCallbacks();
            // Diagnostic: callbacks may wake the BTA task while scheduler
            // switching is intentionally suppressed above. Observe whether a
            // reschedule request survives the safe unwind boundary.
            if (MeteorRuntimeTraceEnabledLocal()) try {
                const auto scheduler = OS_HLE_GetSchedulerGuestStateLayout();
                if (scheduler.schedulerReschedFlag != 0u &&
                    Memory::Read32(scheduler.schedulerReschedFlag) != 0u) {
                    static std::atomic<uint32_t> deferredReschedTraceCount{0u};
                    const uint32_t n = deferredReschedTraceCount.fetch_add(1u, std::memory_order_relaxed);
                    if (n < 64u) {
                        RT_LOG("meteor-bt-deferred-resched")
                            << "guestPc=0x" << std::hex << guestPc
                            << " disable=0x" << Memory::Read32(scheduler.schedulerDisableCount)
                            << " resched=0x" << Memory::Read32(scheduler.schedulerReschedFlag)
                            << " pending=0x" << Memory::Read32(scheduler.schedulerPendingMask)
                            << " current=0x" << Memory::Read32(0x800000D4u)
                            << " running=0x" << Memory::Read32(0x800000D8u)
                            << std::dec << std::endl;
                    }
                }
            } catch (const Memory::AccessViolation&) {
            }
        }

        // VI/alarm/IOS completions above intentionally suppress nested scheduler
        // switches while their interrupt-style callbacks are on the stack.  Once
        // they have fully unwound, honor the same deferred reschedule that retail
        // interrupt return would perform.  Without this, a CPU-bound polling loop
        // can leave higher-priority READY workers stranded indefinitely even with
        // schedulerDisableCount==0 and a non-zero pending mask.
        TryMeteorInterruptReturnReschedule(ctx);
        if (thpAsyncBeginNs != 0u) {
            g_thpPerfFrame.asyncServiceNs += MeteorSteadyNanos() - thpAsyncBeginNs;
        }
    }

    // Everything through the generic "first unknown loop" tracker below is
    // historical bring-up telemetry.  This loop hook runs on millions of guest
    // backedges per second, so even the bounded/unseen trackers become a hot path
    // once their fixed arrays fill (32/64 atomic loads on every checkpoint).
    // Preserve the diagnostics for explicit trace runs, but keep normal gameplay
    // on the functional PE/audio/VI/alarm/IOS path above plus idle service below.
    if (MeteorRuntimeTraceEnabledLocal()) {
    if (guestPc == 0x80028E7Cu && g_meteorPeFinishDelivered.load(std::memory_order_acquire)) {
        static std::atomic<bool> loggedAsyncWaitState{false};
        if (!loggedAsyncWaitState.exchange(true, std::memory_order_relaxed)) {
            try {
                const uint32_t object = Memory::Read32(0x805C136Cu);
                auto& os = RT_LOG("meteor-28e7c-state");
                os << "return=0x" << std::hex << ctx->gpr[3]
                   << " globalObj=0x" << object
                   << " currentId=0x" << Memory::Read32(0x805C1370u)
                   << " cachedStatus=0x" << Memory::Read32(0x805C1374u);
                if (object != 0u) {
                    const uint32_t control = Memory::Read32(object + 4u);
                    os << " objState0=" << std::dec << static_cast<uint32_t>(Memory::Read8(object))
                       << " objState1=" << static_cast<uint32_t>(Memory::Read8(object + 1u))
                       << " word4=0x" << std::hex << Memory::Read32(object + 4u)
                       << " word8=0x" << Memory::Read32(object + 8u)
                       << " word12=0x" << Memory::Read32(object + 12u);
                    if (control != 0u) {
                        const uint32_t ioObject = Memory::Read32(control + 4u);
                        const uint32_t low = Memory::Read32(control + 8u);
                        os << " ctrlState1=" << std::dec << static_cast<uint32_t>(Memory::Read8(control + 1u))
                           << " ctrl2=" << static_cast<uint32_t>(Memory::Read8(control + 2u))
                           << " ctrl49=" << static_cast<uint32_t>(Memory::Read8(control + 73u))
                           << " ctrl4A=" << static_cast<uint32_t>(Memory::Read8(control + 74u))
                           << " ctrl4B=" << static_cast<uint32_t>(Memory::Read8(control + 75u))
                           << " ctrl4C=" << static_cast<uint32_t>(Memory::Read8(control + 76u))
                           << " ctrl4D=" << static_cast<uint32_t>(Memory::Read8(control + 77u))
                           << " ctrlIo=0x" << std::hex << ioObject
                           << " ctrlLow=0x" << low
                           << " ctrlSrc=0x" << Memory::Read32(control + 84u)
                           << " ctrlArg=0x" << Memory::Read32(control + 88u)
                           << " ctrlRemain=0x" << Memory::Read32(control + 96u);
                        if (ioObject != 0u) {
                            const uint32_t vtable = Memory::Read32(ioObject);
                            os << " ioVt=0x" << vtable;
                            if (vtable != 0u) {
                                os << " ioM1C=0x" << Memory::Read32(vtable + 0x1Cu)
                                   << " ioM20=0x" << Memory::Read32(vtable + 0x20u)
                                   << " ioM24=0x" << Memory::Read32(vtable + 0x24u);
                            }
                        }
                        if (low != 0u) {
                            const uint32_t vtable = Memory::Read32(low);
                            const uint32_t implementation = Memory::Read32(low + 4u);
                            os << " lowVt=0x" << vtable
                               << " lowImpl=0x" << implementation;
                            if (vtable != 0u) {
                                os << " lowM14=0x" << Memory::Read32(vtable + 0x14u)
                                   << " lowM1C=0x" << Memory::Read32(vtable + 0x1Cu)
                                   << " lowM20=0x" << Memory::Read32(vtable + 0x20u)
                                   << " lowM24=0x" << Memory::Read32(vtable + 0x24u)
                                   << " lowM2C=0x" << Memory::Read32(vtable + 0x2Cu);
                            }
                            if (implementation != 0u) {
                                os << " impl0=0x" << Memory::Read32(implementation)
                                   << " impl4=0x" << Memory::Read32(implementation + 4u)
                                   << " impl8=0x" << Memory::Read32(implementation + 8u)
                                   << " implC=0x" << Memory::Read32(implementation + 0xCu)
                                   << " impl10=0x" << Memory::Read32(implementation + 0x10u);
                            }
                        }
                    }
                }
                os << std::dec << std::endl;

                // 0x802A9FA0 is the retail 32-entry service dispatcher. Ghidra
                // headless validates this exact table at 0x805C7240, stride 0x10:
                // slot+0 is an interface whose first word is the service method,
                // slot+4 is the upper-case service name. Dump it at the actual
                // AFS busy-wait (not at an earlier boot-time DI read) so we can
                // distinguish missing GCD registration from a registered service
                // that is simply not getting scheduled.
                constexpr uint32_t kServiceSlots = 0x805C7240u;
                uint32_t populatedServices = 0u;
                for (uint32_t i = 0; i < 32u; ++i) {
                    const uint32_t slot = kServiceSlots + i * 0x10u;
                    if (!Memory::Contains(slot, 0x10u)) {
                        continue;
                    }
                    const uint32_t iface = Memory::Read32(slot);
                    if (iface == 0u) {
                        continue;
                    }
                    ++populatedServices;
                    uint32_t fn0 = 0u;
                    if (Memory::Contains(iface, 4u)) {
                        fn0 = Memory::Read32(iface);
                    }
                    char name[13]{};
                    for (uint32_t j = 0; j < 12u; ++j) {
                        const uint8_t ch = Memory::Read8(slot + 4u + j);
                        if (ch == 0u) {
                            break;
                        }
                        name[j] = (ch >= 0x20u && ch <= 0x7Eu) ? static_cast<char>(ch) : '?';
                    }
                    RT_LOG("meteor-service-table")
                        << "index=" << i
                        << " name=" << name
                        << " iface=0x" << std::hex << iface
                        << " fn0=0x" << fn0
                        << std::dec << std::endl;
                }
                RT_LOG("meteor-service-table") << "populated=" << populatedServices << std::endl;

                // Retail startup (validated headlessly) creates the phase-2
                // worker at 0x805D5A68 with entry 0x802B17F0 and the phase-4
                // worker at 0x805D7D80 with entry 0x802B1898, then resumes both
                // with 0x80211238.  Phase 4 dispatches 0x802AE81C; its registered
                // 0x8029F0F8 callback is the normal path to 0x8029EFA8.  Snapshot
                // both OSThreads at the stuck phase-2 callback so a scheduler bug
                // can be distinguished from missing callback registration.
                constexpr uint32_t kPhase2Thread = 0x805D5A68u;
                constexpr uint32_t kPhase4Thread = 0x805D7D80u;
                for (const auto thread : {kPhase2Thread, kPhase4Thread}) {
                    if (!Memory::Contains(thread, 0x2E8u)) {
                        continue;
                    }
                    RT_LOG("meteor-worker-thread")
                        << "thread=0x" << std::hex << thread
                        << " state=" << std::dec << Memory::Read16(thread + 0x2C8u)
                        << " suspend=" << static_cast<int32_t>(Memory::Read32(thread + 0x2CCu))
                        << " prio=" << static_cast<int32_t>(Memory::Read32(thread + 0x2D0u))
                        << " basePrio=" << static_cast<int32_t>(Memory::Read32(thread + 0x2D4u))
                        << " queue=0x" << std::hex << Memory::Read32(thread + 0x2DCu)
                        << " next=0x" << Memory::Read32(thread + 0x2E0u)
                        << " prev=0x" << Memory::Read32(thread + 0x2E4u)
                        << std::dec << std::endl;
                }
            } catch (const Memory::AccessViolation&) {
                RT_LOG("meteor-28e7c-state") << "async wait state read failed return=0x"
                                             << std::hex << ctx->gpr[3] << std::dec << std::endl;
            }
        }
    }

    if (guestPc == 0x8023C36Cu && g_meteorPeFinishDelivered.load(std::memory_order_acquire)) {
        static std::atomic<bool> loggedBadStringScan{false};
        if (!loggedBadStringScan.exchange(true, std::memory_order_relaxed)) {
            try {
                auto& os = RT_LOG("meteor-string-spin");
                os << "strcat scan dst=0x" << std::hex << ctx->gpr[3]
                   << " srcMinus1=0x" << ctx->gpr[4]
                   << " scan=0x" << ctx->gpr[5]
                   << " bytes=";
                for (uint32_t i = 0; i < 32u; ++i) {
                    const uint32_t value = Memory::Read8(ctx->gpr[3] + i);
                    if (i != 0u) {
                        os << ',';
                    }
                    os << std::hex << value;
                }
                os << std::dec << std::endl;
            } catch (const Memory::AccessViolation&) {
                RT_LOG("meteor-string-spin") << "failed to read strcat destination" << std::endl;
            }
        }

        const uint32_t scanDistance = ctx->gpr[5] - ctx->gpr[3];
        if (scanDistance >= 0x100u) {
            static std::atomic<bool> loggedRunawayStringScan{false};
            if (!loggedRunawayStringScan.exchange(true, std::memory_order_relaxed)) {
                try {
                    auto& os = RT_LOG("meteor-string-spin");
                    os << "RUNAWAY strcat scan dst=0x" << std::hex << ctx->gpr[3]
                       << " srcMinus1=0x" << ctx->gpr[4]
                       << " scan=0x" << ctx->gpr[5]
                       << " distance=0x" << scanDistance
                       << " lr=0x" << ctx->lr
                       << " bytes=";
                    for (uint32_t i = 0; i < 32u; ++i) {
                        const uint32_t value = Memory::Read8(ctx->gpr[3] + i);
                        if (i != 0u) {
                            os << ',';
                        }
                        os << std::hex << value;
                    }
                    os << std::dec << std::endl;
                } catch (const Memory::AccessViolation&) {
                    RT_LOG("meteor-string-spin") << "RUNAWAY strcat scan read failed dst=0x"
                                                  << std::hex << ctx->gpr[3]
                                                  << " scan=0x" << ctx->gpr[5]
                                                  << " distance=0x" << scanDistance
                                                  << std::dec << std::endl;
                }
            }
        }
    }

    if (guestPc != 0u && guestPc != 0x80210ADCu && guestPc != 0x80210AD8u) {
        if (g_meteorPeFinishDelivered.load(std::memory_order_acquire)) {
            static std::array<std::atomic<uint32_t>, 32> seenPostPeLoops{};
            bool postSeen = false;
            for (auto& slot : seenPostPeLoops) {
                if (slot.load(std::memory_order_relaxed) == guestPc) {
                    postSeen = true;
                    break;
                }
            }
            if (!postSeen) {
                for (auto& slot : seenPostPeLoops) {
                    uint32_t empty = 0u;
                    if (slot.compare_exchange_strong(empty, guestPc, std::memory_order_relaxed)) {
                        RT_LOG("meteor-scheduler") << "first post-PE loop checkpoint pc=0x"
                                                    << std::hex << guestPc
                                                    << " lr=0x" << ctx->lr
                                                    << " r3=0x" << ctx->gpr[3]
                                                    << " r13=0x" << ctx->gpr[13]
                                                    << std::dec << std::endl;
                        break;
                    }
                    if (empty == guestPc) {
                        break;
                    }
                }
            }
        }

        // Diagnostic only: generated translations already call this hook at
        // selected loop backedges. Record each previously unseen PC once so a
        // post-scheduler busy loop can be identified without modifying generated
        // code or flooding the process log.
        static std::array<std::atomic<uint32_t>, 64> seenUnknownLoops{};
        bool seen = false;
        for (auto& slot : seenUnknownLoops) {
            if (slot.load(std::memory_order_relaxed) == guestPc) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            for (auto& slot : seenUnknownLoops) {
                uint32_t empty = 0u;
                if (slot.compare_exchange_strong(empty, guestPc, std::memory_order_relaxed)) {
                    RT_LOG("meteor-scheduler") << "first unknown loop checkpoint pc=0x"
                                                << std::hex << guestPc
                                                << " lr=0x" << ctx->lr
                                                << " r3=0x" << ctx->gpr[3]
                                                << " r13=0x" << ctx->gpr[13]
                                                << std::dec << std::endl;
                    break;
                }
                if (empty == guestPc) {
                    break;
                }
            }
        }
    }
    }

    // Ghidra headless validated RDSPAF 0x80210990 as the retail scheduler
    // selection routine. 0x80210ADC is its EE-enabled idle spin on the run-queue
    // pending mask; 0x80210AD8 is the companion backedge after restoring IRQ
    // state. No title-specific hardware semantics live here: delegate the wait
    // to the shared Wii scheduler service used by native SelectThread.
    if (meteor::IsSchedulerIdleLoop(guestPc)) {
        const bool traceScheduler = MeteorRuntimeTraceEnabledLocal();
        uint32_t count = 0u;
        if (traceScheduler) {
            static std::atomic<uint32_t> diagnosticCount{0u};
            count = diagnosticCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
        }
        if (traceScheduler && (count <= 8u || (count & (count - 1u)) == 0u)) {
            try {
                const uint32_t pending = Memory::Read32(0x8062ADC8u);
                const uint32_t retrace = Memory::Read32(0x8062AFB4u);
                const uint32_t retraceHead = Memory::Read32(0x8062AF90u);
                const uint32_t retraceTail = Memory::Read32(0x8062AF94u);
                const uint32_t alarmHead = Memory::Read32(0x8062AD40u); // r13 - 0x65E0
                const uint32_t alarmTail = Memory::Read32(0x8062AD44u);
                RT_LOG("meteor-scheduler") << "checkpoint #" << count
                                            << " pc=0x" << std::hex << guestPc
                                            << " lr=0x" << ctx->lr
                                            << " r13=0x" << ctx->gpr[13]
                                            << " pending=0x" << pending
                                            << " viq=[0x" << retraceHead << ",0x" << retraceTail << "]"
                                            << " alarmq=[0x" << alarmHead << ",0x" << alarmTail << "]"
                                            << std::dec << " retrace=" << retrace
                                            << std::endl;
                if (retraceHead != 0u) {
                    RT_LOG("meteor-scheduler") << "vi-wait-thread=0x" << std::hex << retraceHead
                                                << std::dec
                                                << " state=" << Memory::Read16(retraceHead + 0x2C8u)
                                                << " suspend=" << static_cast<int32_t>(Memory::Read32(retraceHead + 0x2CCu))
                                                << " prio=" << static_cast<int32_t>(Memory::Read32(retraceHead + 0x2D0u))
                                                << " basePrio=" << static_cast<int32_t>(Memory::Read32(retraceHead + 0x2D4u))
                                                << " queue=0x" << std::hex << Memory::Read32(retraceHead + 0x2DCu)
                                                << " next=0x" << Memory::Read32(retraceHead + 0x2E0u)
                                                << " prev=0x" << Memory::Read32(retraceHead + 0x2E4u)
                                                << std::dec << std::endl;
                }
            } catch (const Memory::AccessViolation&) {
            }
        }
        ConfigureMeteorSchedulerWaitLayouts();
        // PE FINISH is an asynchronous Hollywood/Pixel-Engine source just like
        // VI retrace. Deliver it before waiting for VI so GXDrawDone can wake the
        // render thread immediately; then return to the translated scheduler so
        // it consumes the newly-published run-queue bit itself.
        if (DeliverMeteorPeFinishInterrupt(ctx)) {
            return;
        }
        OS_HLE_ServiceIdleWait(ctx);
        if (DeliverMeteorPeFinishInterrupt(ctx)) {
            return;
        }
        if (traceScheduler && (count <= 8u || (count & (count - 1u)) == 0u)) {
            try {
                const uint32_t pendingAfter = Memory::Read32(0x8062ADC8u);
                const uint32_t thread = 0x80559D98u;
                RT_LOG("meteor-scheduler") << "after-service #" << count
                                            << " pending=0x" << std::hex << pendingAfter
                                            << " viq=[0x" << Memory::Read32(0x8062AF90u)
                                            << ",0x" << Memory::Read32(0x8062AF94u) << "]"
                                            << " runq16=[0x" << Memory::Read32(0x8055A130u)
                                            << ",0x" << Memory::Read32(0x8055A134u) << "]"
                                            << std::dec
                                            << " state=" << Memory::Read16(thread + 0x2C8u)
                                            << " queue=0x" << std::hex << Memory::Read32(thread + 0x2DCu)
                                            << std::dec << std::endl;
            } catch (const Memory::AccessViolation&) {
            }
        }
        return;
    }
}
