#include <mutex>

#include "abi_bridge.h"
#include "hle_stubs.h"
#include "hle/audio/ax_dsp.h"
#include "runtime_log.h"

namespace {

std::once_flag g_meteorDspLayoutOnce;
std::once_flag g_meteorDspAddTaskLogOnce;
std::once_flag g_meteorAiRegisterLogOnce;
std::once_flag g_meteorAiDmaLogOnce;

// RDSPAF AI globals, proven from the retail AI block at 0x80221EA8..0x8022213C
// with SDA1 = 0x80631320:
//
//   0x80221EA8 AIRegisterDMACallback  -> r13-0x62E0
//   0x80222140 __AIDHandler          -> busy r13-0x6314 and stack flag r13-0x62E4
//   0x80221FC0 AIInit                -> initialized r13-0x6318
//
// Keep the translated AIInit body: it owns the retail SDK timer/IRQ bookkeeping.
// These mirrors only let the shared hardware boundary observe the guest-owned
// callback state while title-native DMA calls replace the physical DSP/AI MMIO.
constexpr uint32_t kMeteorAiInitialized = 0x8062B008u;       // r13 - 0x6318
constexpr uint32_t kMeteorAiCallbackBusy = 0x8062B00Cu;      // r13 - 0x6314
constexpr uint32_t kMeteorAiCallbackStackSwitch = 0x8062B03Cu; // r13 - 0x62E4
constexpr uint32_t kMeteorAiDmaCallback = 0x8062B040u;       // r13 - 0x62E0

// RDSPAF's linked Revolution SDK DSP scheduler globals (SDA1 = 0x80631320),
// validated with Ghidra headless from DSPInit 0x8022A1C4, DSPAddTask
// 0x8022A28C, the task-list helper 0x8022AB6C, and the DSP ISR 0x8022A408.
//
// -0x6188  initialization flag
// -0x6180  asserted/request task pending flag
// -0x617C  asserted/request task pointer
// -0x6178  reserved DSP scheduler state
// -0x6174  task-list tail (updated when appending at 0x8022AC00)
// -0x6170  task-list head/first task
// -0x616C  currently running DSP task used by the ISR
constexpr uint32_t kMeteorDspInitialized = 0x8062B198u;
constexpr uint32_t kMeteorDspAssertPending = 0x8062B1A0u;
constexpr uint32_t kMeteorDspAssertTask = 0x8062B1A4u;
constexpr uint32_t kMeteorDspReservedState = 0x8062B1A8u;
constexpr uint32_t kMeteorDspTaskTail = 0x8062B1ACu;
constexpr uint32_t kMeteorDspFirstTask = 0x8062B1B0u;
constexpr uint32_t kMeteorDspRunningTask = 0x8062B1B4u;

void ConfigureMeteorDspLayout()
{
    std::call_once(g_meteorDspLayoutOnce, [] {
        AiGuestStateLayout ai{};
        ai.initializedFlag = kMeteorAiInitialized;
        ai.callbackBusy = kMeteorAiCallbackBusy;
        ai.callbackStackSwitch = kMeteorAiCallbackStackSwitch;
        ai.dmaCallback = kMeteorAiDmaCallback;
        ai.suppressStaleDmaAudio = true;
        AI_HLE_SetGuestStateLayout(ai);

        DspGuestStateLayout layout{};
        layout.initializedFlag = kMeteorDspInitialized;
        layout.assertPending = kMeteorDspAssertPending;
        layout.assertTask = kMeteorDspAssertTask;
        layout.reservedState = kMeteorDspReservedState;

        // The shared HLE's currentTask field is the insertion tail used when a
        // later task is appended; runningTask is the task currently serviced by
        // the DSP ISR.  RDSPAF keeps those as distinct globals.
        layout.currentTask = kMeteorDspTaskTail;
        layout.firstTask = kMeteorDspFirstTask;
        layout.runningTask = kMeteorDspRunningTask;
        DSP_HLE_SetGuestStateLayout(layout);
    });
}

extern "C" uint32_t Meteor_AIRegisterDMACallback(uint32_t callback)
{
    ConfigureMeteorDspLayout();
    std::call_once(g_meteorAiRegisterLogOnce, [callback] {
        RT_LOG(RT_TAG_AUDIO)
            << "RDSPAF AIRegisterDMACallback: guest callback=0x" << std::hex << callback
            << std::dec << "; using shared AI DMA boundary" << std::endl;
    });
    return AI_HLE_RegisterDMACallback(callback);
}

extern "C" void Meteor_AIInitDMA(uint32_t startAddr, uint32_t length)
{
    ConfigureMeteorDspLayout();
    AI_HLE_InitDMA(startAddr, length);
    std::call_once(g_meteorAiDmaLogOnce, [startAddr, length] {
        RT_LOG(RT_TAG_AUDIO)
            << "RDSPAF AIInitDMA: start=0x" << std::hex << startAddr
            << " length=0x" << length << std::dec << std::endl;
    });
}

extern "C" void Meteor_AIStartDMA()
{
    ConfigureMeteorDspLayout();
    AI_HLE_StartDMA();
}

extern "C" uint32_t Meteor_AIGetDMABytesLeft()
{
    ConfigureMeteorDspLayout();
    return AI_HLE_GetDMABytesLeft();
}

extern "C" uint32_t Meteor_AIGetDMAStartAddr()
{
    ConfigureMeteorDspLayout();
    return AI_HLE_GetDMAStartAddr();
}

extern "C" uint32_t Meteor_DSPAddTask(uint32_t taskPtr)
{
    ConfigureMeteorDspLayout();
    // RDSPAF keeps DSPInit translated, so the guest has already initialized the
    // retail DSP scheduler globals. The shared HLE still needs its host-only mix
    // memory aliases/coefficient table before ConfigureFromTask can decode voices.
    // This preparation is idempotent and deliberately does not reset guest state
    // or inject an INIT mail.
    AxDspHle::PrepareHostMixState();
    std::call_once(g_meteorDspAddTaskLogOnce, [] {
        RT_LOG(RT_TAG_AUDIO)
            << "RDSPAF DSPAddTask: using shared DSP HLE first-task boot/init-callback boundary"
            << std::endl;
    });
    return DSP_HLE_AddTask(taskPtr);
}

} // namespace

// Retail 0x8022A28C links a DSP task and, for the first task, enters the hardware
// mailbox boot handshake at 0x8022A9E0.  The shared HLE preserves the same guest
// task/link state and synchronously raises task->initCallback (+0x28), which is
// the missing hardware event awaited by AX at 0x80223FE8.
REGISTER_TITLE_NATIVE_FUNCTION(0x8022A28C, Meteor_DSPAddTask);

// RDSPAF RVL SDK AI hardware boundary (validated HEADLESS).  AIInit itself at
// 0x80221FC0 remains translated; only operations that otherwise touch physical
// DSP/AI DMA registers are bridged to the shared host device.
REGISTER_TITLE_NATIVE_FUNCTION(0x80221EA8, Meteor_AIRegisterDMACallback);
REGISTER_TITLE_NATIVE_FUNCTION(0x80221EEC, Meteor_AIInitDMA);
REGISTER_TITLE_NATIVE_FUNCTION(0x80221F68, Meteor_AIStartDMA);
REGISTER_TITLE_NATIVE_FUNCTION(0x80221F7C, Meteor_AIGetDMABytesLeft);
REGISTER_TITLE_NATIVE_FUNCTION(0x80221F8C, Meteor_AIGetDMAStartAddr);
