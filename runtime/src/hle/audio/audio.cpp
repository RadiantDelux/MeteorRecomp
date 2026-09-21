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
#include <cstdint>
#include <chrono>
#include <cmath>
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

struct ThpAudioDiagState {
    bool active = false;
    uint64_t blocks = 0;
    uint64_t alternatingAddr = 0;
    uint64_t sameAddr = 0;
    uint64_t unexpectedAddr = 0;
    uint64_t sameHash = 0;
    uint64_t sameHashZero = 0;
    uint64_t sameHashNonZero = 0;
    uint64_t lateTicks = 0;
    uint64_t extraPeriods = 0;
    uint64_t boundaryCount = 0;
    uint64_t boundaryAbsSum = 0;
    uint32_t boundaryMax = 0;
    uint32_t lastAddr = 0;
    uint32_t lastHash = 0;
    int32_t lastLeft = 0;
    int32_t lastRight = 0;
    uint64_t callbackMaxUs = 0;
    uint64_t callbackOver3ms = 0;
    uint64_t busySuppressed = 0;
    uint64_t maskedPhysicalPolls = 0;
    uint64_t maskedPhysicalBlocks = 0;
};

ThpAudioDiagState g_thpAudioDiag{};

uint16_t ReadBE16(const uint8_t* p)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) |
                                 static_cast<uint16_t>(p[1]));
}

void ObserveThpAudioBlock(uint32_t startAddr, uint32_t length, uint32_t overduePeriods)
{
    auto& d = g_thpAudioDiag;
    if (!d.active) {
        d = {};
        d.active = true;
    }

    ++d.blocks;
    if (overduePeriods > 1u) {
        ++d.lateTicks;
        d.extraPeriods += overduePeriods - 1u;
    }

    if (d.lastAddr != 0u) {
        if (startAddr == d.lastAddr) {
            ++d.sameAddr;
        } else if ((d.lastAddr == 0x80546380u && startAddr == 0x80546500u) ||
                   (d.lastAddr == 0x80546500u && startAddr == 0x80546380u)) {
            ++d.alternatingAddr;
        } else {
            ++d.unexpectedAddr;
        }
    }

    try {
        const auto* pcm = static_cast<const uint8_t*>(Memory::GetPointer(startAddr, length));
        uint32_t hash = 2166136261u;
        bool allZero = true;
        for (uint32_t i = 0; i < length; ++i) {
            allZero = allZero && pcm[i] == 0u;
            hash ^= pcm[i];
            hash *= 16777619u;
        }
        if (d.blocks > 1u && hash == d.lastHash) {
            ++d.sameHash;
            if (allZero) {
                ++d.sameHashZero;
            } else {
                ++d.sameHashNonZero;
            }
        }

        if (length >= 4u) {
            // Wii AI memory order is right,left. Compare host-logical L/R sample
            // continuity across DMA boundaries without modifying the PCM stream.
            const int32_t firstRight = static_cast<int16_t>(ReadBE16(pcm));
            const int32_t firstLeft = static_cast<int16_t>(ReadBE16(pcm + 2));
            const uint32_t tail = length - 4u;
            const int32_t lastRight = static_cast<int16_t>(ReadBE16(pcm + tail));
            const int32_t lastLeft = static_cast<int16_t>(ReadBE16(pcm + tail + 2u));
            if (d.boundaryCount != 0u || d.blocks > 1u) {
                const uint32_t jump = static_cast<uint32_t>(
                    std::abs(firstLeft - d.lastLeft) + std::abs(firstRight - d.lastRight));
                d.boundaryAbsSum += jump;
                d.boundaryMax = std::max(d.boundaryMax, jump);
                ++d.boundaryCount;
            }
            d.lastLeft = lastLeft;
            d.lastRight = lastRight;
        }
        d.lastHash = hash;
    } catch (const Memory::AccessViolation&) {
        ++d.unexpectedAddr;
    }
    d.lastAddr = startAddr;
}

void LogThpAudioDiagIfDue()
{
    const auto& d = g_thpAudioDiag;
    if (!d.active || d.blocks == 0u || (d.blocks & 0x1FFu) != 0u) {
        return;
    }
    const uint64_t avgBoundary = d.boundaryCount != 0u ? d.boundaryAbsSum / d.boundaryCount : 0u;
    uint32_t sourceReadyDequeues = 0u;
    uint32_t sourceQueueDepth = 0u;
    uint32_t sourceCurrent = 0u;
    Memory::TryRead32(0x8054624Cu, sourceReadyDequeues);
    Memory::TryRead32(0x8054612Cu, sourceQueueDepth);
    Memory::TryRead32(0x80546254u, sourceCurrent);
    RT_LOG(RT_TAG_AUDIO) << "THP PCM seq: blocks=" << d.blocks
                         << " alt=" << d.alternatingAddr
                         << " sameAddr=" << d.sameAddr
                         << " unexpectedAddr=" << d.unexpectedAddr
                         << " sameHash=" << d.sameHash
                         << " sameHashZero=" << d.sameHashZero
                         << " sameHashNonZero=" << d.sameHashNonZero
                         << " lateTicks=" << d.lateTicks
                         << " extraPeriods=" << d.extraPeriods
                         << " boundaryAvg=" << avgBoundary
                         << " boundaryMax=" << d.boundaryMax
                         << " callbackMaxUs=" << d.callbackMaxUs
                         << " callbackOver3ms=" << d.callbackOver3ms
                         << " busySuppressed=" << d.busySuppressed
                         << " maskedPhysicalPolls=" << d.maskedPhysicalPolls
                         << " maskedPhysicalBlocks=" << d.maskedPhysicalBlocks
                         << " sourceReadyDequeues=" << sourceReadyDequeues
                         << " sourceQueueDepth=" << sourceQueueDepth
                         << " sourceCurrent=0x" << std::hex << sourceCurrent << std::dec
                         << std::endl;
}

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
uint32_t AdvanceAiPhysicalTo(uint64_t nowMicros)
{
    // No guest callbacks run until this batch has been published. Reuse the
    // storage instead of allocating/freeing it on every 3 ms audio interrupt.
    thread_local std::vector<CompletedAiDmaBlock> completed;
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

        // Do not manufacture seconds of audio after a debugger/host suspension.
        // Normal runtime polls are far below this ceiling; within that envelope
        // every elapsed physical DMA completion is preserved in order.
        constexpr uint64_t kMaxAdvanceMicros = 100'000u;
        elapsedMicros = std::min(elapsedMicros, kMaxAdvanceMicros);
        g_ai.accumulatorSeconds += static_cast<double>(elapsedMicros) / 1'000'000.0;

        for (;;) {
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

    for (size_t i = 0; i < completed.size(); ++i) {
        const CompletedAiDmaBlock& block = completed[i];
        if (block.callback != 0x801D45BCu && g_thpAudioDiag.active) {
            g_thpAudioDiag.active = false;
        }

        if (!EnsureAudioBackend(block.sampleRate)) {
            std::lock_guard<std::mutex> lock(g_ai.mutex);
            if (!g_ai.loggedBackendFailure) {
                g_ai.loggedBackendFailure = true;
                ReportAudioProblem("Audio", "audio backend unavailable; dropping samples");
            }
            continue;
        }

        const bool pushed = block.fresh
            ? PushAudioBlock(block.startAddr, block.length)
            : AudioBackend::Instance().PushGapFrames(block.length / (kAudioChannels * kBytesPerSample));
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
    return static_cast<uint32_t>(completed.size());
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
    AdvanceAiPhysicalTo(AudioSteadyMicros());
    std::lock_guard<std::mutex> lock(g_ai.mutex);
    g_ai.programmedStartAddr = start_addr;
    g_ai.registerStartAddr = EncodeAIDmaStartRegister(start_addr);
    g_ai.programmedLength = EncodeAIDmaLengthRegister(length);
    g_ai.playback.Program();
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
        }
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
    AdvanceAiPhysicalTo(AudioSteadyMicros());
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
    AdvanceAiPhysicalTo(AudioSteadyMicros());
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

void Audio_HLE_Tick(CpuContext* ctx, uint32_t deltaMicros)
{
    const HostStallTrace trace("audio-service");
    // The physical AID clock is absolute now; caller poll deltas are deliberately
    // ignored because nested polls used to consume callback wall time and could
    // either double-count or permanently drop it.
    (void)deltaMicros;
    AdvanceAiPhysicalTo(AudioSteadyMicros());

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
                if (callback == 0x801D45BCu) {
                    ++g_thpAudioDiag.busySuppressed;
                }
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
        if (callback == 0x801D45BCu) {
            ++g_thpAudioDiag.busySuppressed;
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
    // AID transport is hardware and continues even while EE is masked. Only
    // delivery of the guest interrupt/callback is gated by interrupt state.
    const uint32_t physicalBlocks = AdvanceAiPhysicalTo(AudioSteadyMicros());
    if (!OS_HLE_InterruptsEnabled()) {
        if (physicalBlocks != 0u) {
            std::lock_guard<std::mutex> lock(g_ai.mutex);
            if (g_ai.callback == 0x801D45BCu && g_thpAudioDiag.active) {
                ++g_thpAudioDiag.maskedPhysicalPolls;
                g_thpAudioDiag.maskedPhysicalBlocks += physicalBlocks;
            }
        }
        return;
    }

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
