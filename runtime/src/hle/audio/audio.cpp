#include "memory.h"
#include "guest_interrupt_context.h"
#include "hle_stubs.h"
#include "ppc_runtime.h"
#include "audio_backend.h"
#include "audio_playback_continuity.h"
#include "ax_dsp.h"
#include "music_attenuation.h"
#include "runtime_log.h"
#include "host_stall_trace.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <mutex>
#include <vector>

namespace {
constexpr uint32_t kDefaultSampleRate = 32000u;
constexpr uint32_t kAudioChannels = 2u;
constexpr uint32_t kBytesPerSample = 2u;
constexpr uint32_t kMaxAiCompletionsPerService = 1024u;

thread_local bool g_replayingHistoricalAidCatchup = false;

bool AudioAidTraceEnabled()
{
    static const bool enabled = std::getenv("METEOR_TRACE_AUDIO") != nullptr;
    return enabled;
}

std::atomic<uint64_t> g_audioAidPrograms{0};
std::atomic<uint64_t> g_audioAidPhysicalBlocks{0};
std::atomic<uint64_t> g_audioAidStaleBlocks{0};
std::atomic<uint64_t> g_audioAidCallbackDispatches{0};
std::atomic<uint64_t> g_audioAidBusySuppressions{0};
std::atomic<uint64_t> g_audioAidMaskedPhysicalBlocks{0};

void TraceAudioAidState(const char* reason)
{
    if (!AudioAidTraceEnabled()) {
        return;
    }
    RT_LOG(RT_TAG_AUDIO) << "AID trace: reason=" << reason
                         << " programs=" << g_audioAidPrograms.load(std::memory_order_relaxed)
                         << " physical=" << g_audioAidPhysicalBlocks.load(std::memory_order_relaxed)
                         << " stale=" << g_audioAidStaleBlocks.load(std::memory_order_relaxed)
                         << " callbacks=" << g_audioAidCallbackDispatches.load(std::memory_order_relaxed)
                         << " busySuppressed=" << g_audioAidBusySuppressions.load(std::memory_order_relaxed)
                         << " maskedPhysical=" << g_audioAidMaskedPhysicalBlocks.load(std::memory_order_relaxed)
                         << std::endl;
}
AiGuestStateLayout g_aiGuestStateLayout{
    0x80386448u, // initializedFlag
    0x8038644Cu, // callbackBusy
    0x8038647Cu, // callbackStackSwitch
    0x80386480u, // dmaCallback
};

struct AIDmaState {
    std::mutex mutex;
    uint32_t programmedStartAddr = 0;
    uint32_t programmedLength = 0;
    uint32_t activeStartAddr = 0;
    uint32_t activeLength = 0;
    uint32_t registerStartAddr = 0;
    uint32_t callback = 0;
    bool enabled = false;
    uint32_t sampleRate = kDefaultSampleRate;
    uint32_t bytesLeft = 0;
    double accumulatorSeconds = 0.0;
    uint64_t lastAdvanceMicros = 0;
    bool aidPending = false;
    bool tickActive = false;
    bool loggedBackendFailure = false;
    bool loggedMissingCallback = false;
    bool loggedAccessFailure = false;
    AudioDmaPlaybackCursor playback;
};

AIDmaState g_ai{};

bool TryReadGuestAiWord(uint32_t address, uint32_t& value)
{
    if (address == 0) {
        value = 0;
        return false;
    }
    return Memory::TryRead32(address, value);
}

void TryWriteGuestAiWord(uint32_t address, uint32_t value)
{
    if (address != 0) {
        Memory::TryWrite32(address, value);
    }
}

// Audio degradation is invisible to the player except as silence, so every
// notice below reaches stderr unconditionally. The ones that sit on the
// per-DMA-frame path keep their one-shot latch in g_ai.
void ReportAudioProblem(const char* who, const char* what) {
    RT_LOGF(RT_TAG_AUDIO, "%s: %s\n", who, what);
    std::fflush(stderr);
}

bool EnsureAudioBackend(uint32_t sampleRate) {
    return AudioBackend::Instance().Init(sampleRate, kAudioChannels);
}

uint32_t EncodeAIDmaStartRegister(uint32_t startAddr) {
    return startAddr & 0x1fffffe0u;
}

uint32_t EncodeAIDmaLengthRegister(uint32_t length) {
    return length & 0x000fffe0u;
}

bool PushAudioBlock(uint32_t startAddr, uint32_t length) {
    if (startAddr == 0 || length == 0) {
        return false;
    }
    const uint32_t bytes = length;
    const uint8_t* src = nullptr;
    try {
        src = static_cast<const uint8_t*>(Memory::GetPointer(startAddr, bytes));
    } catch (const Memory::AccessViolation&) {
        src = nullptr;
    }

    if (src) {
        return AudioBackend::Instance().PushWiiAiSamplesBE16(src, bytes);
    }

    const uint32_t sampleCount = bytes / kBytesPerSample;
    if (sampleCount == 0) {
        return false;
    }
    std::vector<int16_t> samples(sampleCount);
    try {
        for (uint32_t i = 0; i < sampleCount; ++i) {
            const uint32_t addr = startAddr + i * kBytesPerSample;
            samples[i] = static_cast<int16_t>(Memory::Read16(addr));
        }
    } catch (const Memory::AccessViolation&) {
        return false;
    }
    // Memory::Read16 has converted endianness, but the Wii AI frame order is
    // still right, left. Convert it to the host's left, right convention.
    for (uint32_t i = 0; i + 1 < sampleCount; i += 2) {
        std::swap(samples[i], samples[i + 1]);
    }
    return AudioBackend::Instance().PushSamplesLE16(samples.data(), samples.size());
}

uint64_t AudioSteadyMicros()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

double AiDmaBlockDurationSeconds(uint32_t length, uint32_t sampleRate)
{
    if (length == 0u || sampleRate == 0u) {
        return 0.0;
    }
    const double bytesPerSecond =
        static_cast<double>(sampleRate) * kAudioChannels * kBytesPerSample;
    return static_cast<double>(length) / bytesPerSecond;
}

struct CompletedAiDmaBlock {
    uint32_t startAddr = 0;
    uint32_t length = 0;
    uint32_t callback = 0;
    uint32_t sampleRate = kDefaultSampleRate;
    bool fresh = true;
};

// Advance the physical AID engine from a single monotonic wall-clock cursor.
// This is intentionally independent of guest callback dispatch: retail DMA keeps
// consuming/reloading buffers while the AID handler or THP mixer is executing.
// Register writes only affect the next reload through programmedStart/Length.
uint32_t AdvanceAiPhysicalTo(uint64_t nowMicros,
                             uint32_t maxCompletions = UINT32_MAX)
{
    // No guest callbacks run until this batch has been published. Reuse the
    // storage instead of allocating/freeing it on every 3 ms audio interrupt.
    thread_local std::vector<CompletedAiDmaBlock> completed;
    // Masked or busy IRQs may accumulate beyond the catch-up window.
    if (completed.capacity() < 1024u) {
        completed.reserve(1024u);
    }
    completed.clear();

    {
        std::lock_guard<std::mutex> lock(g_ai.mutex);
        if (g_ai.lastAdvanceMicros == 0u) {
            g_ai.lastAdvanceMicros = nowMicros;
        }

        if (!g_ai.enabled || g_ai.activeStartAddr == 0u ||
            g_ai.activeLength == 0u || g_ai.sampleRate == 0u) {
            g_ai.lastAdvanceMicros = nowMicros;
            return 0u;
        }

        uint64_t elapsedMicros = nowMicros >= g_ai.lastAdvanceMicros
            ? nowMicros - g_ai.lastAdvanceMicros
            : 0u;
        g_ai.lastAdvanceMicros = nowMicros;

        // Retain excess elapsed time for later service points.
        g_ai.accumulatorSeconds += static_cast<double>(elapsedMicros) / 1'000'000.0;

        while (completed.size() < maxCompletions) {
            const double blockDuration =
                AiDmaBlockDurationSeconds(g_ai.activeLength, g_ai.sampleRate);
            if (blockDuration <= 0.0 || g_ai.accumulatorSeconds < blockDuration) {
                break;
            }

            g_ai.accumulatorSeconds -= blockDuration;
            completed.push_back({g_ai.activeStartAddr, g_ai.activeLength,
                                 g_ai.callback, g_ai.sampleRate,
                                 g_ai.playback.Complete(g_aiGuestStateLayout.suppressStaleDmaAudio &&
                                                       g_ai.callback != 0u)});

            // Physical autoreload happens before AIDINT. The callback that this
            // completion wakes may program a later buffer, never the one that is
            // already becoming active here.
            g_ai.activeStartAddr = g_ai.programmedStartAddr;
            g_ai.activeLength = g_ai.programmedLength;
            g_ai.bytesLeft = g_ai.activeLength;
            g_ai.aidPending = true;

            if (g_ai.activeStartAddr == 0u || g_ai.activeLength == 0u) {
                g_ai.accumulatorSeconds = 0.0;
                break;
            }
        }

        if (g_ai.activeLength != 0u && g_ai.sampleRate != 0u) {
            const double blockDuration =
                AiDmaBlockDurationSeconds(g_ai.activeLength, g_ai.sampleRate);
            if (blockDuration > 0.0) {
                const double progress = std::clamp(
                    g_ai.accumulatorSeconds / blockDuration, 0.0, 1.0);
                const uint32_t consumed = static_cast<uint32_t>(
                    static_cast<double>(g_ai.activeLength) * progress);
                g_ai.bytesLeft = consumed < g_ai.activeLength
                    ? g_ai.activeLength - consumed
                    : 0u;
            }
        }
    }

    if (completed.empty()) {
        return 0u;
    }

    // Publish any host-side AX mix before reading the completed guest buffers.
    // One join covers a catch-up batch because no guest callback can run between
    // these physical completions until Audio_HLE_Tick services the latched AIDINT.
    AxDspHle::JoinMixWorker();

    // The batch uses one captured sample rate and one host backend check.
    if (!EnsureAudioBackend(completed.front().sampleRate)) {
        std::lock_guard<std::mutex> lock(g_ai.mutex);
        if (!g_ai.loggedBackendFailure) {
            g_ai.loggedBackendFailure = true;
            ReportAudioProblem("Audio", "audio backend unavailable; dropping samples");
        }
        return static_cast<uint32_t>(completed.size());
    }

    size_t staleFramesTotal = 0;
    size_t staleBlocks = 0;
    for (const CompletedAiDmaBlock& block : completed) {
        if (!block.fresh) {
            ++staleBlocks;
            staleFramesTotal += block.length / (kAudioChannels * kBytesPerSample);
        }
    }
    if (AudioAidTraceEnabled()) {
        g_audioAidPhysicalBlocks.fetch_add(completed.size(), std::memory_order_relaxed);
        if (staleBlocks != 0) {
            const uint64_t before = g_audioAidStaleBlocks.fetch_add(staleBlocks, std::memory_order_relaxed);
            const uint64_t after = before + staleBlocks;
            if ((before / 32u) != (after / 32u)) {
                TraceAudioAidState("stale-completion");
            }
        }
    }
    AudioBackend& audioBackend = AudioBackend::Instance();
    AudioHostGapResolution staleResolution{};
    if (staleFramesTotal != 0 || !g_replayingHistoricalAidCatchup) {
        staleResolution = audioBackend.ResolveStaleDmaFrames(staleFramesTotal);
    }
    size_t coveredStaleFrames = staleResolution.coveredFrames;
    size_t staleFramesPending = 0;
    auto flushStaleFrames = [&]() {
        if (staleFramesPending == 0) {
            return true;
        }
        const size_t frames = staleFramesPending;
        staleFramesPending = 0;
        return audioBackend.PushGapFrames(frames);
    };

    for (size_t i = 0; i < completed.size(); ++i) {
        const CompletedAiDmaBlock& block = completed[i];
        if (!block.fresh) {
            size_t frames = block.length / (kAudioChannels * kBytesPerSample);
            const size_t covered = std::min(frames, coveredStaleFrames);
            coveredStaleFrames -= covered;
            frames -= covered;
            staleFramesPending += frames;
            continue;
        }

        const bool pushed = flushStaleFrames() && PushAudioBlock(block.startAddr, block.length);
        if (!pushed) {
            std::lock_guard<std::mutex> lock(g_ai.mutex);
            if (!g_ai.loggedAccessFailure) {
                g_ai.loggedAccessFailure = true;
                ReportAudioProblem("Audio", "failed to read DMA buffer; disabling audio DMA");
            }
            g_ai.enabled = false;
            g_ai.aidPending = false;
            break;
        }
    }

    if (!flushStaleFrames()) {
        std::lock_guard<std::mutex> lock(g_ai.mutex);
        if (!g_ai.loggedAccessFailure) {
            g_ai.loggedAccessFailure = true;
            ReportAudioProblem("Audio", "failed to publish stale DMA state; disabling audio DMA");
        }
        g_ai.enabled = false;
        g_ai.aidPending = false;
    }
    return static_cast<uint32_t>(completed.size());
}

bool AiHistoricalReplayRequired()
{
    std::lock_guard<std::mutex> lock(g_ai.mutex);
    if (!g_aiGuestStateLayout.suppressStaleDmaAudio || g_ai.callback == 0u ||
        !g_ai.enabled || g_ai.activeStartAddr == 0u ||
        g_ai.activeLength == 0u || g_ai.sampleRate == 0u) {
        return false;
    }
    const double blockDuration =
        AiDmaBlockDurationSeconds(g_ai.activeLength, g_ai.sampleRate);
    if (blockDuration <= 0.0) {
        return false;
    }

    // Deliver latched IRQs before consuming historical boundaries; the reload
    // length may differ from the active block length.
    if (g_ai.aidPending) {
        return g_ai.accumulatorSeconds >= blockDuration;
    }
    if (g_ai.programmedStartAddr == 0u || g_ai.programmedLength == 0u) {
        return false;
    }
    const double reloadDuration =
        AiDmaBlockDurationSeconds(g_ai.programmedLength, g_ai.sampleRate);
    return reloadDuration > 0.0 &&
           g_ai.accumulatorSeconds >= blockDuration + reloadDuration;
}

} // namespace

void AI_HLE_SetGuestStateLayout(const AiGuestStateLayout& layout)
{
    g_aiGuestStateLayout = layout;
}

extern "C" void AIClockInit_801A1138(uint32_t clock_mode)
{
    // clock_mode is unused: AID/DSP rate is controlled separately by AI state, and
    // treating it as a sample-rate switch would break Wii AX's normal 32 kHz cadence.
    (void)clock_mode;
    uint32_t rate = kDefaultSampleRate;
    {
        std::lock_guard<std::mutex> lock(g_ai.mutex);
        rate = g_ai.sampleRate;
    }
    if (!EnsureAudioBackend(rate)) {
        ReportAudioProblem("__AIClockInit", "audio backend init failed");
    }
}

PPC_NATIVE_OVERRIDE_VOID(801A1138, AIClockInit_801A1138, (uint32_t clock_mode), (clock_mode));

extern "C" void OSInitAudioSystem_801A1358()
{
    AIClockInit_801A1138(1);
    AxDspHle::InitAram();
    AxDspHle::Init();
    if (!EnsureAudioBackend(kDefaultSampleRate)) {
        ReportAudioProblem("__OSInitAudioSystem", "audio backend init failed");
    }
}

PPC_NATIVE_OVERRIDE_VOID(801A1358, OSInitAudioSystem_801A1358, (), ());

extern "C" void OSStopAudioSystem_801A1520()
{
    {
        std::lock_guard<std::mutex> lock(g_ai.mutex);
        g_ai.enabled = false;
        g_ai.programmedStartAddr = 0;
        g_ai.programmedLength = 0;
        g_ai.activeStartAddr = 0;
        g_ai.activeLength = 0;
        g_ai.registerStartAddr = 0;
        g_ai.bytesLeft = 0;
        g_ai.accumulatorSeconds = 0.0;
        g_ai.lastAdvanceMicros = 0;
        g_ai.aidPending = false;
    }
    AxDspHle::Stop();
}

PPC_NATIVE_OVERRIDE_VOID(801A1520, OSStopAudioSystem_801A1520, (), ());



// Do NOT stub Audio__Manager__Init_80717150 / Audio__Manager__InitSelf_8071724c: they must
// run translated to init AudioHandleHolder::sInstance, or createSceneSoundManager NULL-vtable crashes.


void AI_HLE_Init(uint32_t callback_stack_switch)
{
    const uint32_t rate = kDefaultSampleRate;
    uint32_t initialized = 0;
    const bool alreadyInitialized =
        TryReadGuestAiWord(g_aiGuestStateLayout.initializedFlag, initialized) && initialized == 1u;
    {
        std::lock_guard<std::mutex> lock(g_ai.mutex);
        g_ai.sampleRate = rate;
        if (!alreadyInitialized) {
            g_ai.callback = 0;
            g_ai.loggedMissingCallback = false;
            g_ai.loggedAccessFailure = false;
        }
    }
    if (!alreadyInitialized) {
        TryWriteGuestAiWord(g_aiGuestStateLayout.dmaCallback, 0);
        TryWriteGuestAiWord(g_aiGuestStateLayout.callbackBusy, 0);
        TryWriteGuestAiWord(g_aiGuestStateLayout.callbackStackSwitch, callback_stack_switch);
        TryWriteGuestAiWord(g_aiGuestStateLayout.initializedFlag, 1);
    }
    if (!EnsureAudioBackend(rate)) {
        ReportAudioProblem("AIInit", "audio backend init failed");
    } else {
        RT_LOG(RT_TAG_AUDIO) << "AIInit_801240b0 called: Audio subsystem initialized (HLE)" << std::endl;
    }
}

extern "C" void AIInit_801240b0(uint32_t callback_stack_switch)
{
    AI_HLE_Init(callback_stack_switch);
}

PPC_NATIVE_OVERRIDE_VOID(801240b0, AIInit_801240b0, (uint32_t callback_stack_switch), (callback_stack_switch));

extern "C" uint32_t AICheckInit_80124094()
{
    return AI_HLE_CheckInit();
}
REGISTER_NATIVE_FUNCTION(0x80124094, AICheckInit_80124094);

uint32_t AI_HLE_CheckInit()
{
    uint32_t initialized = 0;
    TryReadGuestAiWord(g_aiGuestStateLayout.initializedFlag, initialized);
    return initialized;
}



void DSP_HLE_SetGuestStateLayout(const DspGuestStateLayout& layout)
{
    AxDspHle::SetGuestStateLayout(layout.initializedFlag,
                                  layout.assertPending,
                                  layout.assertTask,
                                  layout.reservedState,
                                  layout.currentTask,
                                  layout.firstTask,
                                  layout.runningTask);
}

void DSP_HLE_Init()
{
    // Nintendo's DSPInit is idempotent. Preserve an already-live task graph on
    // repeated calls rather than resetting it a second time.
    if (AxDspHle::CheckInit() == 0) {
        AxDspHle::Init();
    }
}

uint32_t DSP_HLE_CheckInit() { return AxDspHle::CheckInit(); }
uint32_t DSP_HLE_AddTask(uint32_t taskPtr) { return AxDspHle::AddTask(taskPtr); }
void DSP_HLE_SendMailToDSP(uint32_t mail) { AxDspHle::SendMailToDSP(mail); }
uint32_t DSP_HLE_CheckMailToDSP() { return AxDspHle::CheckMailToDSP(); }
uint32_t DSP_HLE_CheckMailFromDSP() { return AxDspHle::CheckMailFromDSP(); }
uint32_t DSP_HLE_ReadMailFromDSP() { return AxDspHle::ReadMailFromDSP(); }
uint32_t DSP_HLE_AssertTask(uint32_t taskPtr) { return AxDspHle::AssertTask(taskPtr); }

extern "C" void DSPInit_8015d444()
{
    DSP_HLE_Init();
    RT_LOG(RT_TAG_AUDIO) << "DSPInit_8015d444 called: DSP hardware boundary initialized (HLE)" << std::endl;
}

PPC_NATIVE_OVERRIDE_VOID(8015d444, DSPInit_8015d444, (), ());

extern "C" uint32_t DSPCheckInit_8015d504()
{
    return DSP_HLE_CheckInit();
}
REGISTER_NATIVE_FUNCTION(0x8015D504, DSPCheckInit_8015d504);

extern "C" uint32_t DSPAddTask_8015d50c(uint32_t task_ptr)
{
    return DSP_HLE_AddTask(task_ptr);
}
REGISTER_NATIVE_FUNCTION(0x8015D50C, DSPAddTask_8015d50c);

extern "C" void __DSP_boot_task_8015dc60(uint32_t task_ptr)
{
    AxDspHle::AssertTask(task_ptr);
    RT_LOG(RT_TAG_AUDIO) << "__DSP_boot_task called: booted DSP task at 0x"
              << std::hex << task_ptr << std::dec << std::endl;
}

PPC_NATIVE_OVERRIDE_VOID(8015dc60, __DSP_boot_task_8015dc60, (uint32_t task_ptr), (task_ptr));


extern "C" void __AXOutInitDSP_801269bc(CpuContext* ctx)
{
    AxDspHle::InitForAXOut(ctx);
    RT_LOG(RT_TAG_AUDIO) << "__AXOutInitDSP called: native AX/DSP HLE initialized." << std::endl;
}

PPC_NATIVE_OVERRIDE_VOID(801269bc, __AXOutInitDSP_801269bc, (CpuContext* ctx), (ctx));



void AI_HLE_InitDMA(uint32_t start_addr, uint32_t length)
{
    // If DMA is already running, close physical time under the OLD programmed
    // registers before this write becomes visible as the next reload.
    // Only the outer service drains overdue boundaries between guest rearms.
    if (!g_replayingHistoricalAidCatchup) {
        AdvanceAiPhysicalTo(AudioSteadyMicros());
    }
    std::lock_guard<std::mutex> lock(g_ai.mutex);
    g_ai.programmedStartAddr = start_addr;
    g_ai.registerStartAddr = EncodeAIDmaStartRegister(start_addr);
    g_ai.programmedLength = EncodeAIDmaLengthRegister(length);
    g_ai.playback.Program();
    if (AudioAidTraceEnabled()) {
        g_audioAidPrograms.fetch_add(1, std::memory_order_relaxed);
    }
    if (!g_ai.enabled) {
        g_ai.bytesLeft = g_ai.programmedLength;
    }
}

extern "C" void AIInitDMA_80123fcc(uint32_t start_addr, uint32_t length)
{
    AI_HLE_InitDMA(start_addr, length);
}

PPC_NATIVE_OVERRIDE_VOID(80123fcc, AIInitDMA_80123fcc, (uint32_t start_addr, uint32_t length), (start_addr, length));



// AIRegisterDMACallback stores the callback in the guest global at 0x80386480.
// Returns the old callback pointer.
uint32_t AI_HLE_RegisterDMACallback(uint32_t callback)
{
    uint32_t old_callback = 0;
    TryReadGuestAiWord(g_aiGuestStateLayout.dmaCallback, old_callback);
    TryWriteGuestAiWord(g_aiGuestStateLayout.dmaCallback, callback);
    {
        std::lock_guard<std::mutex> lock(g_ai.mutex);
        g_ai.callback = callback;
        g_ai.loggedMissingCallback = false;
    }
    return old_callback;
}

extern "C" uint32_t AIRegisterDMACallback_80123f88(uint32_t callback)
{
    return AI_HLE_RegisterDMACallback(callback);
}
PPC_NATIVE_OVERRIDE(80123f88, AIRegisterDMACallback_80123f88, uint32_t, (uint32_t callback), (callback));

// AIStartDMA toggles the AI DMA control register on hardware. Keep the guest-visible
// DMA state here and let the VI tick advance the hardware boundary.
void AI_HLE_StartDMA()
{
    const uint32_t rate = kDefaultSampleRate;
    const uint64_t nowMicros = AudioSteadyMicros();
    bool started = false;
    {
        std::lock_guard<std::mutex> lock(g_ai.mutex);
        g_ai.sampleRate = rate;
        if (!g_ai.enabled) {
            g_ai.activeStartAddr = g_ai.programmedStartAddr;
            g_ai.activeLength = g_ai.programmedLength;
            g_ai.playback.Start();
            g_ai.bytesLeft = g_ai.activeLength;
            g_ai.accumulatorSeconds = 0.0;
            g_ai.lastAdvanceMicros = nowMicros;
            // Retail AID latches an interrupt when DMA is started. This lets the
            // guest prepare/program the following buffer while the first one is
            // physically in flight instead of starting one callback phase late.
            g_ai.aidPending = true;
            g_ai.enabled = true;
            started = true;
        }
    }
    if (started) {
        // Silence credit does not carry into a new DMA run.
        AudioBackend::Instance().ResetGapAccounting();
    }
    if (!EnsureAudioBackend(rate)) {
        ReportAudioProblem("AIStartDMA", "audio backend init failed");
    }
}

extern "C" void AIStartDMA_80124048()
{
    AI_HLE_StartDMA();
}

PPC_NATIVE_OVERRIDE_VOID(80124048, AIStartDMA_80124048, (), ());

extern "C" uint32_t AIGetDMABytesLeft_8012405c()
{
    return AI_HLE_GetDMABytesLeft();
}
PPC_NATIVE_OVERRIDE(8012405C, AIGetDMABytesLeft_8012405c, uint32_t, (), ());

extern "C" uint32_t AIGetDMAStartAddr_8012406c()
{
    return AI_HLE_GetDMAStartAddr();
}
PPC_NATIVE_OVERRIDE(8012406C, AIGetDMAStartAddr_8012406c, uint32_t, (), ());

extern "C" uint32_t AIGetDMALength_80124084()
{
    return AI_HLE_GetDMALength();
}
PPC_NATIVE_OVERRIDE(80124084, AIGetDMALength_80124084, uint32_t, (), ());

extern "C" uint32_t AIGetDSPSampleRate_8012409c()
{
    std::lock_guard<std::mutex> lock(g_ai.mutex);
    // SDK AIGetDSPSampleRate returns AIDFR^1: 0 for 32 kHz, 1 for 48 kHz.
    return (g_ai.sampleRate == 48000u) ? 1u : 0u;
}
PPC_NATIVE_OVERRIDE(8012409C, AIGetDSPSampleRate_8012409c, uint32_t, (), ());

void AI_HLE_StopDMA()
{
    if (!g_replayingHistoricalAidCatchup) {
        AdvanceAiPhysicalTo(AudioSteadyMicros());
    }
    std::lock_guard<std::mutex> lock(g_ai.mutex);
    g_ai.enabled = false;
    g_ai.activeStartAddr = 0;
    g_ai.activeLength = 0;
    g_ai.bytesLeft = g_ai.programmedLength;
    g_ai.accumulatorSeconds = 0.0;
    g_ai.lastAdvanceMicros = 0;
    // Disabling DMA stops further physical completions but does not ACK an AIDINT
    // that was already latched. The interrupt handler/guest callback owns that.
}

uint32_t AI_HLE_GetDMAEnableFlag()
{
    std::lock_guard<std::mutex> lock(g_ai.mutex);
    return g_ai.enabled ? 1u : 0u;
}

uint32_t AI_HLE_GetDMABytesLeft()
{
    if (!g_replayingHistoricalAidCatchup) {
        AdvanceAiPhysicalTo(AudioSteadyMicros());
    }
    std::lock_guard<std::mutex> lock(g_ai.mutex);
    return g_ai.bytesLeft;
}

uint32_t AI_HLE_GetDMAStartAddr()
{
    std::lock_guard<std::mutex> lock(g_ai.mutex);
    return g_ai.registerStartAddr;
}

uint32_t AI_HLE_GetDMALength()
{
    std::lock_guard<std::mutex> lock(g_ai.mutex);
    return g_ai.programmedLength;
}

extern "C" void DSPSendMailToDSP_8015d430(uint32_t mail)
{
    DSP_HLE_SendMailToDSP(mail);
}
PPC_NATIVE_OVERRIDE_VOID(8015D430, DSPSendMailToDSP_8015d430, (uint32_t mail), (mail));

extern "C" void SoundPlayerSetVolume_800a35e0(uint32_t soundPlayer, float volume)
{
    MusicAttenuation::SetSoundPlayerVolume(soundPlayer, volume);
}

PPC_NATIVE_OVERRIDE_VOID(800A35E0, SoundPlayerSetVolume_800a35e0,
              (uint32_t soundPlayer, float volume), (soundPlayer, volume));

extern "C" uint32_t DSPCheckMailToDSP_8015d3fc()
{
    return DSP_HLE_CheckMailToDSP();
}
PPC_NATIVE_OVERRIDE(8015D3FC, DSPCheckMailToDSP_8015d3fc, uint32_t, (), ());

extern "C" uint32_t DSPCheckMailFromDSP_8015d40c()
{
    return DSP_HLE_CheckMailFromDSP();
}
REGISTER_NATIVE_FUNCTION(0x8015D40C, DSPCheckMailFromDSP_8015d40c);

extern "C" uint32_t DSPReadMailFromDSP_8015d41c()
{
    return DSP_HLE_ReadMailFromDSP();
}
REGISTER_NATIVE_FUNCTION(0x8015D41C, DSPReadMailFromDSP_8015d41c);

extern "C" uint32_t DSPAssertTask_8015d57c(uint32_t taskPtr)
{
    return DSP_HLE_AssertTask(taskPtr);
}
PPC_NATIVE_OVERRIDE(8015D57C, DSPAssertTask_8015d57c, uint32_t, (uint32_t taskPtr), (taskPtr));

bool ReplayHistoricalAiCatchup(CpuContext* ctx, uint64_t targetMicros)
{
    {
        std::lock_guard<std::mutex> lock(g_ai.mutex);
        if (g_ai.tickActive) {
            return false;
        }
        g_ai.tickActive = true;
    }

    struct ReplayReset {
        ~ReplayReset()
        {
            // Preserve silence credit across pieces of the same historical batch.
            (void)AudioBackend::Instance().ResolveStaleDmaFrames(0);
            g_replayingHistoricalAidCatchup = false;
            std::lock_guard<std::mutex> lock(g_ai.mutex);
            g_ai.tickActive = false;
        }
    } reset;

    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    CpuContextScope scope(cpu);
    g_replayingHistoricalAidCatchup = true;

    for (uint32_t replayed = 0; replayed < kMaxAiCompletionsPerService; ++replayed) {
        if (!OS_HLE_InterruptsEnabled()) {
            break;
        }
        uint32_t callback = 0;
        bool pending = false;
        {
            std::lock_guard<std::mutex> lock(g_ai.mutex);
            callback = g_ai.callback;
            pending = g_ai.aidPending;
            if (pending) {
                // Retail __AIDHandler ACKs before entering the registered callback.
                g_ai.aidPending = false;
            }
        }

        if (!pending) {
            if (AdvanceAiPhysicalTo(targetMicros, 1u) == 0u) {
                break;
            }
            std::lock_guard<std::mutex> lock(g_ai.mutex);
            callback = g_ai.callback;
            pending = g_ai.aidPending;
            if (pending) {
                g_ai.aidPending = false;
            }
        }

        if (!pending || callback == 0u) {
            break;
        }

        uint32_t guestBusy = 0u;
        TryReadGuestAiWord(g_aiGuestStateLayout.callbackBusy, guestBusy);
        if (guestBusy != 0u) {
            if (AudioAidTraceEnabled()) {
                g_audioAidBusySuppressions.fetch_add(1, std::memory_order_relaxed);
                TraceAudioAidState("replay-guest-busy");
            }
            break;
        }

        if (!CanDispatchGuestTarget(callback)) {
            std::lock_guard<std::mutex> lock(g_ai.mutex);
            if (!g_ai.loggedMissingCallback) {
                g_ai.loggedMissingCallback = true;
                ReportAudioProblem("Audio", "AI DMA callback not registered; skipping");
            }
            break;
        }

        if (AudioAidTraceEnabled()) {
            g_audioAidCallbackDispatches.fetch_add(1, std::memory_order_relaxed);
        }
        TryWriteGuestAiWord(g_aiGuestStateLayout.callbackBusy, 1u);
        try {
            // Keep a fixed elapsed-time target while callbacks rearm each block.
            InvokeIndirectCpu(callback, cpu);
            TryWriteGuestAiWord(g_aiGuestStateLayout.callbackBusy, 0u);
            AxDspHle::ServiceDeferredCallbacks();
        } catch (...) {
            TryWriteGuestAiWord(g_aiGuestStateLayout.callbackBusy, 0u);
            throw;
        }
    }

    return true;
}

void Audio_HLE_Tick(CpuContext* ctx, uint32_t deltaMicros)
{
    const HostStallTrace trace("audio-service");
    if (g_replayingHistoricalAidCatchup) {
        // Nested checkpoints must not drain the outer service's backlog.
        return;
    }
    // The physical AID clock is absolute now; caller poll deltas are deliberately
    // ignored because nested polls used to consume callback wall time and could
    // either double-count or permanently drop it.
    (void)deltaMicros;
    bool streamingBoundaryPrepared = false;

    // Replay multiple overdue periods one boundary and callback at a time.
    if (g_aiGuestStateLayout.suppressStaleDmaAudio && OS_HLE_InterruptsEnabled()) {
        bool serviceActive = false;
        {
            std::lock_guard<std::mutex> lock(g_ai.mutex);
            serviceActive = g_ai.tickActive;
        }
        if (!serviceActive) {
            uint32_t guestBusy = 0u;
            TryReadGuestAiWord(g_aiGuestStateLayout.callbackBusy, guestBusy);
            if (guestBusy == 0u) {
                const uint64_t targetMicros = AudioSteadyMicros();
                AdvanceAiPhysicalTo(targetMicros, 0u);
                if (AiHistoricalReplayRequired() && ReplayHistoricalAiCatchup(ctx, targetMicros)) {
                    return;
                }
                // Use the snapshot so a single due IRQ cannot become a batch.
                AdvanceAiPhysicalTo(targetMicros, 1u);
                streamingBoundaryPrepared = true;
            }
        }
    }

    if (!streamingBoundaryPrepared) {
        AdvanceAiPhysicalTo(AudioSteadyMicros());
    }

    uint32_t callback = 0;
    bool pending = false;
    bool outerServiceActive = false;

    {
        std::lock_guard<std::mutex> lock(g_ai.mutex);
        callback = g_ai.callback;
        if (g_ai.tickActive) {
            outerServiceActive = true;
        } else {
            pending = g_ai.aidPending;
            if (!pending) {
                return;
            }
            // Retail __AIDHandler ACKs the interrupt status before testing the busy
            // latch, so a suppressed interrupt never becomes a queued callback.
            g_ai.aidPending = false;
            g_ai.tickActive = true;
        }
    }

    if (outerServiceActive) {
        // tickActive covers the whole host service, including deferred DSP work.
        // Only suppress a nested AIDINT if the *guest* callbackBusy latch is still
        // set. After the registered callback returns callbackBusy is clear and a
        // new completion must stay pending for the next service boundary.
        uint32_t nestedGuestBusy = 0u;
        TryReadGuestAiWord(g_aiGuestStateLayout.callbackBusy, nestedGuestBusy);
        if (nestedGuestBusy != 0u) {
            std::lock_guard<std::mutex> lock(g_ai.mutex);
            if (g_ai.aidPending) {
                g_ai.aidPending = false;
            }
        }
        return;
    }

    struct ActiveTickReset {
        ~ActiveTickReset()
        {
            std::lock_guard<std::mutex> lock(g_ai.mutex);
            g_ai.tickActive = false;
        }
    } activeTickReset;

    if (!pending || callback == 0u) {
        return;
    }

    uint32_t guestBusy = 0u;
    TryReadGuestAiWord(g_aiGuestStateLayout.callbackBusy, guestBusy);
    if (guestBusy != 0u) {
        if (AudioAidTraceEnabled()) {
            const uint64_t before = g_audioAidBusySuppressions.fetch_add(1, std::memory_order_relaxed);
            if ((before & 31u) == 31u) {
                TraceAudioAidState("guest-busy");
            }
        }
        return;
    }

    if (!CanDispatchGuestTarget(callback)) {
        std::lock_guard<std::mutex> lock(g_ai.mutex);
        if (!g_ai.loggedMissingCallback) {
            g_ai.loggedMissingCallback = true;
            ReportAudioProblem("Audio", "AI DMA callback not registered; skipping");
        }
        return;
    }

    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    CpuContextScope scope(cpu);
    if (AudioAidTraceEnabled()) {
        g_audioAidCallbackDispatches.fetch_add(1, std::memory_order_relaxed);
    }
    TryWriteGuestAiWord(g_aiGuestStateLayout.callbackBusy, 1u);
    try {
        InvokeIndirectCpu(callback, cpu);
        // DMA never pauses for the registered callback. Account for all physical
        // time spent inside translated mixer work before clearing callbackBusy.
        // If a nested service point actually runs while callbackBusy is set, the
        // tickActive path above ACKs/drops the latched AID exactly there. Do not
        // manufacture that ACK merely because wall time crossed a DMA boundary:
        // retail leaves an unserviced AIDINT pending after callbackBusy clears.
        AdvanceAiPhysicalTo(AudioSteadyMicros());
        // Retail __AIDHandler clears callbackBusy immediately after the registered
        // callback returns. Host-deferred DSP work is outside that busy window.
        TryWriteGuestAiWord(g_aiGuestStateLayout.callbackBusy, 0u);

        AxDspHle::ServiceDeferredCallbacks();
        // Physical DMA also runs during deferred host/DSP service, but a completion
        // here occurs after callbackBusy was cleared and must remain pending for the
        // next interrupt service boundary rather than being suppressed.
        AdvanceAiPhysicalTo(AudioSteadyMicros());
    } catch (...) {
        TryWriteGuestAiWord(g_aiGuestStateLayout.callbackBusy, 0u);
        throw;
    }

}

void Audio_HLE_Poll(CpuContext* ctx)
{
    MusicAttenuation::TickGuest();
    Audio_HLE_Tick(ctx, 0u);
}

void Audio_HLE_PollDeferred()
{
    const HostStallTrace trace("audio-deferred");
    if (g_replayingHistoricalAidCatchup) {
        return;
    }
    if (!OS_HLE_InterruptsEnabled()) {
        // Physical AID transport continues while EE is masked.
        const uint32_t physicalBlocks = AdvanceAiPhysicalTo(AudioSteadyMicros());
        if (physicalBlocks != 0u && AudioAidTraceEnabled()) {
            const uint64_t before = g_audioAidMaskedPhysicalBlocks.fetch_add(
                physicalBlocks, std::memory_order_relaxed);
            const uint64_t after = before + physicalBlocks;
            if ((before / 32u) != (after / 32u)) {
                TraceAudioAidState("masked-physical");
            }
        }
        return;
    }

    // Audio_HLE_Tick owns boundary replay when IRQ delivery is enabled.
    GuestInterruptCallbackContext interrupt;
    CpuContext* cpu = interrupt.get();

    OS_HLE_BeginDeferredGuestCallbacks();
    try {
        Audio_HLE_Tick(cpu, 0u);
    } catch (...) {
        OS_HLE_EndDeferredGuestCallbacks();
        throw;
    }
    OS_HLE_EndDeferredGuestCallbacks();
}
