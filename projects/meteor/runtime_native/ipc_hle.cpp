#include "abi_bridge.h"
#include "memory.h"
#include "ppc_runtime.h"
#include "runtime_config.h"
#include "runtime_log.h"
#include "wii_extension_crypto.h"
#include "wii_remote_input.h"

#include <atomic>
#include <algorithm>
#include <cstdint>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <sstream>
#include <vector>

extern "C" int32_t NAND_IOS_Open_HLE(uint32_t pathPtr, uint32_t mode);
extern "C" int32_t NAND_IOS_Close_HLE(uint32_t fd);
extern "C" int32_t NAND_IOS_Read_HLE(uint32_t fd, uint32_t bufferPtr, uint32_t length);
extern "C" int32_t NAND_IOS_Write_HLE(uint32_t fd, uint32_t bufferPtr, uint32_t length);
extern "C" int32_t NAND_IOS_Seek_HLE(uint32_t fd, int32_t offset, int32_t whence);
extern "C" int32_t NAND_IOS_Ioctl_HLE(uint32_t fd, uint32_t cmd, uint32_t inBufPtr,
                                       uint32_t inLen, uint32_t outBufPtr, uint32_t outLen);
extern "C" int32_t NAND_IOS_Ioctlv_HLE(uint32_t fd, uint32_t cmd, uint32_t numIn,
                                        uint32_t numOut, uint32_t vectorPtr);
void NandQueueIosCallback(uint32_t callbackPtr, int32_t result, uint32_t callbackArg);
bool NandProcessPendingCallbacks(CpuContext* cpu, int maxToProcess);
bool NandIosCallbackDispatchActive() noexcept;

namespace {

// Runtime tracing is opt-in because this module sits on the asynchronous IOS
// and Bluetooth hot paths.  The cached value avoids touching the environment on
// every request while retaining the existing diagnostics for focused bring-up
// runs (`METEOR_TRACE_RUNTIME=1`).
bool MeteorRuntimeTraceEnabled() noexcept {
    static const bool enabled = std::getenv("METEOR_TRACE_RUNTIME") != nullptr;
    return enabled;
}


constexpr uint32_t kIpcInitFlagOffset = 0x6028u;
constexpr uint32_t kIpcHeapHandleOffset = 0x7D1Cu;
constexpr uint32_t kIpcHeapSize = 0x800u;
constexpr int32_t kMeteorBtUsbFd = 0x6001;
constexpr uint32_t kUsbIoctlvControl = 0;
constexpr uint32_t kUsbIoctlvBulk = 1;
constexpr uint32_t kUsbIoctlvInterrupt = 2;
constexpr uint8_t kBtAclOutEndpoint = 0x02;
constexpr uint8_t kBtAclInEndpoint = 0x82;
constexpr uint8_t kBtEventInEndpoint = 0x81;

struct MeteorBtPendingTransfer {
    bool active = false;
    uint8_t endpoint = 0;
    uint32_t request = 0;
    uint32_t buffer = 0;
    uint32_t length = 0;
    uint32_t callback = 0;
    uint32_t callbackArg = 0;
};

struct MeteorBtUsbDataTransfer {
    uint8_t endpoint = 0;
    uint16_t requestedLength = 0;
    uint32_t dataBuffer = 0;
    uint32_t dataVectorLength = 0;
};

struct MeteorBtUsbControlTransfer {
    uint8_t requestType = 0;
    uint8_t request = 0;
    uint16_t value = 0;
    uint16_t index = 0;
    uint16_t length = 0;
    uint32_t dataBuffer = 0;
    uint32_t dataVectorLength = 0;
};

std::mutex g_btUsbMutex;
MeteorBtPendingTransfer g_btEventIn;
MeteorBtPendingTransfer g_btAclIn;
std::deque<std::vector<uint8_t>> g_btQueuedEvents;
std::deque<std::vector<uint8_t>> g_btQueuedAclIn;
uint8_t g_btScanEnable = 0;
bool g_btVirtualConnectionRequestQueued = false;
bool g_btInitialL2capRequestQueued = false;
uint16_t g_btGuestHidControlCid = 0u;
uint16_t g_btGuestHidInterruptCid = 0u;
bool g_btControlPeerConfigQueued = false;
bool g_btInterruptConnectionRequestQueued = false;
bool g_btInterruptPeerConfigQueued = false;
uint8_t g_btReportMode = 0u;
bool g_btReportContinuous = false;
uint8_t g_btLedState = 0u;
std::array<uint8_t, 16> g_btExtensionKey{};
uint8_t g_btExtensionKeyParts = 0u;
bool g_btExtensionEncryptionEnabled = false;
std::chrono::steady_clock::time_point g_btNextInputReport{};
std::atomic<uint32_t> g_btEventCallbackArgWatch{0u};
std::atomic<bool> g_btEventCallbackCorruptionLogged{false};

void ResetMeteorBtUsbState() {
    std::lock_guard<std::mutex> lock(g_btUsbMutex);
    g_btEventIn = {};
    g_btAclIn = {};
    g_btQueuedEvents.clear();
    g_btQueuedAclIn.clear();
    g_btScanEnable = 0;
    g_btVirtualConnectionRequestQueued = false;
    g_btInitialL2capRequestQueued = false;
    g_btGuestHidControlCid = 0u;
    g_btGuestHidInterruptCid = 0u;
    g_btControlPeerConfigQueued = false;
    g_btInterruptConnectionRequestQueued = false;
    g_btInterruptPeerConfigQueued = false;
    g_btReportMode = 0u;
    g_btReportContinuous = false;
    g_btLedState = 0u;
    g_btExtensionKey.fill(0u);
    g_btExtensionKeyParts = 0u;
    g_btExtensionEncryptionEnabled = false;
    g_btNextInputReport = {};
    g_btEventCallbackArgWatch.store(0u, std::memory_order_release);
    g_btEventCallbackCorruptionLogged.store(false, std::memory_order_release);
}

void FreeMeteorAsyncRequest(CpuContext* cpu, uint32_t request) {
    if (!cpu || request == 0) {
        return;
    }
    const uint32_t savedR3 = cpu->gpr[3];
    const uint32_t savedR4 = cpu->gpr[4];
    cpu->gpr[3] = Memory::Read32(cpu->gpr[13] - kIpcHeapHandleOffset);
    cpu->gpr[4] = request;
    InvokeIndirectCpu(0x80252C14u, cpu);
    cpu->gpr[3] = savedR3;
    cpu->gpr[4] = savedR4;
}

void WriteMeteorBtUsbPayload(uint32_t buffer, const uint8_t* data, uint32_t length) {
    for (uint32_t i = 0; i < length; ++i) {
        Memory::Write8(buffer + i, data[i]);
    }
}

uint16_t ReadUsbLittleEndian16(uint32_t address) {
    return static_cast<uint16_t>(Memory::Read8(address) |
                                 (static_cast<uint16_t>(Memory::Read8(address + 1)) << 8));
}

bool DecodeMeteorBtUsbDataTransfer(uint32_t numIn, uint32_t numOut, uint32_t vectorPtr,
                                   MeteorBtUsbDataTransfer& transfer) {
    if (numIn != 2 || numOut != 1 || !Memory::Contains(vectorPtr, 3u * 8u)) {
        return false;
    }
    const uint32_t endpointPtr = Memory::Read32(vectorPtr + 0);
    const uint32_t endpointLen = Memory::Read32(vectorPtr + 4);
    const uint32_t lengthPtr = Memory::Read32(vectorPtr + 8);
    const uint32_t lengthLen = Memory::Read32(vectorPtr + 12);
    const uint32_t dataPtr = Memory::Read32(vectorPtr + 16);
    const uint32_t dataLen = Memory::Read32(vectorPtr + 20);
    if (endpointLen != 1 || lengthLen != 2 ||
        !Memory::Contains(endpointPtr, 1) || !Memory::Contains(lengthPtr, 2) ||
        (dataLen != 0 && !Memory::Contains(dataPtr, dataLen))) {
        return false;
    }
    transfer.endpoint = Memory::Read8(endpointPtr);
    transfer.requestedLength = Memory::Read16(lengthPtr);
    transfer.dataBuffer = dataPtr;
    transfer.dataVectorLength = dataLen;
    return transfer.requestedLength == dataLen;
}

bool DecodeMeteorBtUsbControlTransfer(uint32_t numIn, uint32_t numOut, uint32_t vectorPtr,
                                      MeteorBtUsbControlTransfer& transfer) {
    if (numIn != 6 || numOut != 1 || !Memory::Contains(vectorPtr, 7u * 8u)) {
        return false;
    }
    std::array<uint32_t, 7> addresses{};
    std::array<uint32_t, 7> lengths{};
    for (uint32_t i = 0; i < 7; ++i) {
        addresses[i] = Memory::Read32(vectorPtr + i * 8u);
        lengths[i] = Memory::Read32(vectorPtr + i * 8u + 4u);
    }
    if (lengths[0] != 1 || lengths[1] != 1 || lengths[2] != 2 ||
        lengths[3] != 2 || lengths[4] != 2 || lengths[5] != 1 ||
        !Memory::Contains(addresses[0], 1) || !Memory::Contains(addresses[1], 1) ||
        !Memory::Contains(addresses[2], 2) || !Memory::Contains(addresses[3], 2) ||
        !Memory::Contains(addresses[4], 2) || !Memory::Contains(addresses[5], 1) ||
        (lengths[6] != 0 && !Memory::Contains(addresses[6], lengths[6]))) {
        return false;
    }
    transfer.requestType = Memory::Read8(addresses[0]);
    transfer.request = Memory::Read8(addresses[1]);
    transfer.value = ReadUsbLittleEndian16(addresses[2]);
    transfer.index = ReadUsbLittleEndian16(addresses[3]);
    transfer.length = ReadUsbLittleEndian16(addresses[4]);
    transfer.dataBuffer = addresses[6];
    transfer.dataVectorLength = lengths[6];
    return transfer.length == transfer.dataVectorLength;
}

void DeliverMeteorBtEventIfPossible(CpuContext* cpu) {
    MeteorBtPendingTransfer pending{};
    std::vector<uint8_t> event;
    {
        std::lock_guard<std::mutex> lock(g_btUsbMutex);
        if (!g_btEventIn.active || g_btQueuedEvents.empty()) {
            return;
        }
        event = std::move(g_btQueuedEvents.front());
        g_btQueuedEvents.pop_front();
        if (event.size() > g_btEventIn.length) {
            RT_LOG(RT_TAG_OS) << "RDSPAF BT USB: HCI event len=" << event.size()
                              << " exceeds interrupt buffer len=" << g_btEventIn.length
                              << std::endl;
            return;
        }
        pending = g_btEventIn;
        g_btEventIn = {};
        g_btEventCallbackArgWatch.store(0u, std::memory_order_release);
    }
    const uint32_t callbackFnBefore = pending.callbackArg != 0 && Memory::Contains(pending.callbackArg, 4u)
        ? Memory::Read32(pending.callbackArg)
        : 0u;
    if (MeteorRuntimeTraceEnabled()) {
        RT_LOG(RT_TAG_OS) << "RDSPAF BT USB: HCI deliver request=0x" << std::hex
                          << pending.request << " buffer=0x" << pending.buffer
                          << " len=0x" << event.size()
                          << " callbackArg=0x" << pending.callbackArg
                          << " callbackFnBefore=0x" << callbackFnBefore
                          << std::dec << std::endl;
    }
    WriteMeteorBtUsbPayload(pending.buffer, event.data(), static_cast<uint32_t>(event.size()));
    const uint32_t callbackFnAfterWrite = pending.callbackArg != 0 && Memory::Contains(pending.callbackArg, 4u)
        ? Memory::Read32(pending.callbackArg)
        : 0u;
    Memory::Write32(pending.request + 0x04u, static_cast<uint32_t>(event.size()));
    NandQueueIosCallback(pending.callback, static_cast<int32_t>(event.size()), pending.callbackArg);
    FreeMeteorAsyncRequest(cpu, pending.request);
    const uint32_t callbackFnAfterFree = pending.callbackArg != 0 && Memory::Contains(pending.callbackArg, 4u)
        ? Memory::Read32(pending.callbackArg)
        : 0u;
    if (MeteorRuntimeTraceEnabled()) {
        RT_LOG(RT_TAG_OS) << "RDSPAF BT USB: HCI callbackArg fn after-write=0x" << std::hex
                          << callbackFnAfterWrite << " after-free=0x" << callbackFnAfterFree
                          << std::dec << std::endl;
        RT_LOG(RT_TAG_OS) << "RDSPAF BT USB: delivered HCI event len=" << event.size()
                          << " to ep=0x" << std::hex << static_cast<uint32_t>(pending.endpoint)
                          << std::dec << std::endl;
    }
}

void QueueMeteorBtHciEvent(CpuContext* cpu, const uint8_t* data, uint32_t length) {
    (void)cpu;
    {
        std::lock_guard<std::mutex> lock(g_btUsbMutex);
        g_btQueuedEvents.emplace_back(data, data + length);
    }
}

void QueueMeteorBtAclIn(const uint8_t* data, uint32_t length) {
    std::lock_guard<std::mutex> lock(g_btUsbMutex);
    g_btQueuedAclIn.emplace_back(data, data + length);
}

void QueueMeteorBtL2capData(uint16_t destinationCid, const uint8_t* payload,
                            uint16_t payloadLength) {
    const uint16_t hciLength = static_cast<uint16_t>(4u + payloadLength);
    std::vector<uint8_t> packet(static_cast<size_t>(4u + hciLength), 0u);
    packet[0] = 0x0B;
    packet[1] = 0x20; // handle 0x000B, PB numeric value 2.
    packet[2] = static_cast<uint8_t>(hciLength & 0xFFu);
    packet[3] = static_cast<uint8_t>(hciLength >> 8);
    packet[4] = static_cast<uint8_t>(payloadLength & 0xFFu);
    packet[5] = static_cast<uint8_t>(payloadLength >> 8);
    packet[6] = static_cast<uint8_t>(destinationCid & 0xFFu);
    packet[7] = static_cast<uint8_t>(destinationCid >> 8);
    if (payloadLength != 0u) {
        std::memcpy(packet.data() + 8u, payload, payloadLength);
    }
    QueueMeteorBtAclIn(packet.data(), static_cast<uint32_t>(packet.size()));
}

void QueueMeteorBtCoreInputReport(uint16_t destinationCid) {
    WiiRemoteInput::KpadSample sample{};
    const bool haveSample = WiiRemoteInput::ReadKpadSample(0u, sample);
    const uint16_t buttons = haveSample
        ? static_cast<uint16_t>(sample.hold & 0xFFFFu)
        : 0u;
    static uint16_t lastLoggedButtons = 0xFFFFu;
    if (buttons != lastLoggedButtons) {
        RT_LOG(RT_TAG_OS) << "RDSPAF BT USB: virtual Wii Remote core buttons=0x"
                          << std::hex << static_cast<uint32_t>(buttons)
                          << std::dec << " sampled=" << (haveSample ? 1 : 0)
                          << std::endl;
        lastLoggedButtons = buttons;
    }
    const uint8_t report[] = {
        0xA1, 0x30, // HID DATA input, core-buttons report.
        static_cast<uint8_t>(buttons & 0xFFu),
        static_cast<uint8_t>(buttons >> 8),
    };
    QueueMeteorBtL2capData(destinationCid, report,
                           static_cast<uint16_t>(sizeof(report)));
}

uint16_t EncodeMeteorBtAccel(float g, uint16_t zero, uint16_t one) {
    const float raw = static_cast<float>(zero) +
                      g * static_cast<float>(static_cast<int32_t>(one) - static_cast<int32_t>(zero));
    return static_cast<uint16_t>(std::clamp<long>(std::lround(raw), 0L, 1023L));
}

std::array<uint16_t, 3> EncodeMeteorBtAccelFromKpad(const float kpad[3],
                                                    const std::array<uint16_t, 3>& zero,
                                                    const std::array<uint16_t, 3>& one) {
    // ReadKpadSample exposes (-WiiX, -WiiZ, WiiY). Input report 0x35 carries
    // the Remote/Nunchuk's native Wii-axis accelerometer values.
    const std::array<float, 3> wii = {-kpad[0], kpad[2], -kpad[1]};
    return {
        EncodeMeteorBtAccel(wii[0], zero[0], one[0]),
        EncodeMeteorBtAccel(wii[1], zero[1], one[1]),
        EncodeMeteorBtAccel(wii[2], zero[2], one[2]),
    };
}

bool QueueMeteorBtCoreAccelNunchukInputReport(uint16_t destinationCid) {
    WiiRemoteInput::KpadSample sample{};
    const bool haveSample = WiiRemoteInput::ReadKpadSample(0u, sample);

    std::array<uint8_t, 16> extensionKey{};
    uint8_t extensionKeyParts = 0u;
    bool extensionEncryptionEnabled = false;
    {
        std::lock_guard<std::mutex> lock(g_btUsbMutex);
        extensionKey = g_btExtensionKey;
        extensionKeyParts = g_btExtensionKeyParts;
        extensionEncryptionEnabled = g_btExtensionEncryptionEnabled;
    }
    if (!extensionEncryptionEnabled || extensionKeyParts != 0x07u) {
        static bool loggedMissingKey = false;
        if (!loggedMissingKey) {
            RT_LOG(RT_TAG_OS)
                << "RDSPAF BT USB: report 0x35 requested before Nunchuk encryption/key setup completed; input deferred"
                << std::endl;
            loggedMissingKey = true;
        }
        return false;
    }

    MeteorWiiExtensionCrypto::KeyTables tables{};
    if (!MeteorWiiExtensionCrypto::GenerateFirstPartyKeyTables(extensionKey, tables)) {
        static bool loggedBadKey = false;
        if (!loggedBadKey) {
            RT_LOG(RT_TAG_OS)
                << "RDSPAF BT USB: report 0x35 Nunchuk key did not match first-party retail keygen; input deferred"
                << std::endl;
            loggedBadKey = true;
        }
        return false;
    }

    // Match the same factory calibration block exposed at EEPROM 0x16..0x1f.
    // Zero: 82 82 82 / packed LSBs 15 => 0x209 each.
    // +1g: 9c 9c 9e / packed LSBs 38 => 0x273,0x272,0x278.
    const std::array<uint16_t, 3> remoteZero = {0x209u, 0x209u, 0x209u};
    const std::array<uint16_t, 3> remoteOne = {0x273u, 0x272u, 0x278u};
    const float neutralRemote[3] = {0.0f, -1.0f, 0.0f};
    const auto remoteAccel = EncodeMeteorBtAccelFromKpad(
        haveSample ? sample.acc : neutralRemote, remoteZero, remoteOne);

    uint16_t coreButtons = haveSample ? static_cast<uint16_t>(sample.hold & 0xFFFFu) : 0u;
    // WPAD C/Z occupy bits which are accel LSB fields in a Remote core report;
    // they belong only in the Nunchuk extension bytes below.
    coreButtons = static_cast<uint16_t>(coreButtons & ~0x6000u);
    uint8_t core0 = static_cast<uint8_t>(coreButtons & 0xFFu);
    uint8_t core1 = static_cast<uint8_t>(coreButtons >> 8u);
    core0 = static_cast<uint8_t>((core0 & ~0x60u) | ((remoteAccel[0] & 0x03u) << 5u));
    core1 = static_cast<uint8_t>((core1 & ~0x60u) |
                                 (((remoteAccel[1] >> 1u) & 0x01u) << 5u) |
                                 (((remoteAccel[2] >> 1u) & 0x01u) << 6u));

    std::array<uint8_t, 16> extension{};
    const bool haveNunchuk = haveSample && sample.hasNunchuk;
    const float stickX = haveNunchuk ? std::clamp(sample.stick[0], -1.0f, 1.0f) : 0.0f;
    const float stickY = haveNunchuk ? std::clamp(sample.stick[1], -1.0f, 1.0f) : 0.0f;
    extension[0] = static_cast<uint8_t>(std::clamp<long>(
        std::lround(128.0f + stickX * 96.0f), 0L, 255L));
    extension[1] = static_cast<uint8_t>(std::clamp<long>(
        std::lround(128.0f + stickY * 96.0f), 0L, 255L));

    // Match the Nunchuk calibration returned from A40020: zero=0x80<<2,
    // +1g=0xb3<<2 for all axes. The streamed six-byte payload begins at A40008.
    const std::array<uint16_t, 3> nunchukZero = {0x200u, 0x200u, 0x200u};
    const std::array<uint16_t, 3> nunchukOne = {0x2CCu, 0x2CCu, 0x2CCu};
    const float neutralNunchuk[3] = {0.0f, -1.0f, 0.0f};
    const auto nunchukAccel = EncodeMeteorBtAccelFromKpad(
        haveNunchuk ? sample.nunchukAcc : neutralNunchuk, nunchukZero, nunchukOne);
    extension[2] = static_cast<uint8_t>(nunchukAccel[0] >> 2u);
    extension[3] = static_cast<uint8_t>(nunchukAccel[1] >> 2u);
    extension[4] = static_cast<uint8_t>(nunchukAccel[2] >> 2u);
    extension[5] = static_cast<uint8_t>(((nunchukAccel[2] & 0x03u) << 6u) |
                                        ((nunchukAccel[1] & 0x03u) << 4u) |
                                        ((nunchukAccel[0] & 0x03u) << 2u) |
                                        0x03u);
    if (haveNunchuk && (sample.hold & 0x4000u) != 0u) {
        extension[5] = static_cast<uint8_t>(extension[5] & ~0x02u); // C is active-low.
    }
    if (haveNunchuk && (sample.hold & 0x2000u) != 0u) {
        extension[5] = static_cast<uint8_t>(extension[5] & ~0x01u); // Z is active-low.
    }

    MeteorWiiExtensionCrypto::Encrypt(extension.data(), 0x08u, extension.size(), tables);

    std::array<uint8_t, 23> report{};
    report[0] = 0xA1;
    report[1] = 0x35; // Core buttons + Remote accel + 16 extension bytes.
    report[2] = core0;
    report[3] = core1;
    report[4] = static_cast<uint8_t>(remoteAccel[0] >> 2u);
    report[5] = static_cast<uint8_t>(remoteAccel[1] >> 2u);
    report[6] = static_cast<uint8_t>(remoteAccel[2] >> 2u);
    std::memcpy(report.data() + 7u, extension.data(), extension.size());
    QueueMeteorBtL2capData(destinationCid, report.data(), static_cast<uint16_t>(report.size()));

    static bool loggedFirst = false;
    static uint16_t lastLoggedHold = 0xFFFFu;
    const uint16_t hold = haveSample ? static_cast<uint16_t>(sample.hold & 0xFFFFu) : 0u;
    if (!loggedFirst || hold != lastLoggedHold) {
        if (MeteorRuntimeTraceEnabled()) {
            RT_LOG(RT_TAG_OS) << "RDSPAF BT USB: queued Wii Remote report 0x35 hold=0x"
                              << std::hex << static_cast<uint32_t>(hold)
                              << std::dec << " sampled=" << (haveSample ? 1 : 0)
                              << " nunchuk=" << (haveNunchuk ? 1 : 0)
                              << std::endl;
        }
        loggedFirst = true;
        lastLoggedHold = hold;
    }
    return true;
}

void MaybeQueueMeteorBtContinuousInputReport() {
    uint16_t destinationCid = 0u;
    uint8_t reportMode = 0u;
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(g_btUsbMutex);
        if (!g_btReportContinuous ||
            (g_btReportMode != 0x30u && g_btReportMode != 0x35u) ||
            g_btGuestHidInterruptCid == 0u || !g_btQueuedAclIn.empty() ||
            (g_btNextInputReport.time_since_epoch().count() != 0 && now < g_btNextInputReport)) {
            return;
        }
        destinationCid = g_btGuestHidInterruptCid;
        reportMode = g_btReportMode;
        g_btNextInputReport = now + std::chrono::milliseconds(16);
    }
    if (reportMode == 0x35u) {
        QueueMeteorBtCoreAccelNunchukInputReport(destinationCid);
    } else {
        QueueMeteorBtCoreInputReport(destinationCid);
    }
}

void QueueMeteorBtL2capSignal(uint8_t code, uint8_t identifier,
                              const uint8_t* payload, uint16_t payloadLength) {
    const uint16_t commandLength = static_cast<uint16_t>(4u + payloadLength);
    const uint16_t hciLength = static_cast<uint16_t>(4u + commandLength);
    std::vector<uint8_t> packet(static_cast<size_t>(4u + hciLength), 0u);
    packet[0] = 0x0B;
    packet[1] = 0x20; // handle 0x000B, PB numeric value 2.
    packet[2] = static_cast<uint8_t>(hciLength & 0xFFu);
    packet[3] = static_cast<uint8_t>(hciLength >> 8);
    packet[4] = static_cast<uint8_t>(commandLength & 0xFFu);
    packet[5] = static_cast<uint8_t>(commandLength >> 8);
    packet[6] = 0x01;
    packet[7] = 0x00; // signaling CID
    packet[8] = code;
    packet[9] = identifier;
    packet[10] = static_cast<uint8_t>(payloadLength & 0xFFu);
    packet[11] = static_cast<uint8_t>(payloadLength >> 8);
    if (payloadLength != 0u) {
        std::memcpy(packet.data() + 12u, payload, payloadLength);
    }
    QueueMeteorBtAclIn(packet.data(), static_cast<uint32_t>(packet.size()));
}

void QueueMeteorBtL2capConfigResponse(uint8_t identifier, uint16_t sourceCid) {
    const uint8_t payload[] = {
        static_cast<uint8_t>(sourceCid & 0xFFu),
        static_cast<uint8_t>(sourceCid >> 8),
        0x00, 0x00, // flags
        0x00, 0x00, // success
    };
    QueueMeteorBtL2capSignal(0x05u, identifier, payload, sizeof(payload));
}

void QueueMeteorBtL2capConfigRequest(uint8_t identifier, uint16_t destinationCid) {
    const uint8_t payload[] = {
        static_cast<uint8_t>(destinationCid & 0xFFu),
        static_cast<uint8_t>(destinationCid >> 8),
        0x00, 0x00, // flags; default MTU/flush timeout are sufficient.
    };
    QueueMeteorBtL2capSignal(0x04u, identifier, payload, sizeof(payload));
}

void QueueMeteorBtHidInterruptConnectionRequest() {
    const uint8_t payload[] = {
        0x13, 0x00, // HID Interrupt PSM
        0x41, 0x00, // peer SCID
    };
    QueueMeteorBtL2capSignal(0x02u, 0x03u, payload, sizeof(payload));
    RT_LOG(RT_TAG_OS) << "RDSPAF BT USB: queued virtual Wii Remote HID Interrupt L2CAP ConnReq"
                      << std::endl;
}

void HandleMeteorBtAclOutL2cap(uint32_t buffer, uint32_t length) {
    if (length < 8u || !Memory::Contains(buffer, length)) {
        return;
    }
    const uint16_t handleFlags = ReadUsbLittleEndian16(buffer);
    const uint16_t hciLength = ReadUsbLittleEndian16(buffer + 2u);
    if ((handleFlags & 0x0FFFu) != 0x000Bu || hciLength + 4u > length || hciLength < 4u) {
        return;
    }
    const uint16_t l2capLength = ReadUsbLittleEndian16(buffer + 4u);
    const uint16_t cid = ReadUsbLittleEndian16(buffer + 6u);
    if (l2capLength + 4u > hciLength || 8u + l2capLength > length) {
        return;
    }

    // Wii Remote HID output reports are sent by the host over the Interrupt
    // channel. The first retail request observed after both L2CAP channels are
    // configured is A2 17 00 00 17 70 00 01: EEPROM read 0x1770, size 1.
    // A real Wii Remote reports this first probe as invalid address (error 8),
    // then the host proceeds to the readable EEPROM calibration area.
    if (cid == 0x0041u && l2capLength >= 2u) {
        const uint32_t payload = buffer + 8u;
        const uint8_t hidTransaction = Memory::Read8(payload);
        const uint8_t reportId = Memory::Read8(payload + 1u);
        if (hidTransaction == 0xA2u && reportId == 0x1Au && l2capLength >= 3u &&
            g_btGuestHidInterruptCid != 0u) {
            const uint8_t flags = Memory::Read8(payload + 2u);
            if ((flags & 0x02u) != 0u) {
                const uint8_t ack[] = {
                    0xA1, 0x22,
                    0x00, 0x00,
                    0x1A, 0x00,
                };
                QueueMeteorBtL2capData(g_btGuestHidInterruptCid, ack,
                                       static_cast<uint16_t>(sizeof(ack)));
            }
            RT_LOG(RT_TAG_OS) << "RDSPAF BT USB: Wii Remote IR-enable-2 flags=0x"
                              << std::hex << static_cast<uint32_t>(flags)
                              << std::dec << " ack=" << (((flags & 0x02u) != 0u) ? 1 : 0)
                              << std::endl;
        } else if (hidTransaction == 0xA2u && reportId == 0x11u && l2capLength >= 3u &&
            g_btGuestHidInterruptCid != 0u) {
            const uint8_t flags = Memory::Read8(payload + 2u);
            g_btLedState = static_cast<uint8_t>(flags & 0xF0u);
            if ((flags & 0x02u) != 0u) {
                const uint8_t ack[] = {
                    0xA1, 0x22,
                    0x00, 0x00,
                    0x11, 0x00,
                };
                QueueMeteorBtL2capData(g_btGuestHidInterruptCid, ack,
                                       static_cast<uint16_t>(sizeof(ack)));
            }
            RT_LOG(RT_TAG_OS) << "RDSPAF BT USB: Wii Remote LED report flags=0x"
                              << std::hex << static_cast<uint32_t>(flags)
                              << std::dec << " ack=" << (((flags & 0x02u) != 0u) ? 1 : 0)
                              << std::endl;
        } else if (hidTransaction == 0xA2u && reportId == 0x15u && l2capLength >= 3u &&
                   g_btGuestHidInterruptCid != 0u) {
            // Status Information Request. Report 0x20 carries buttons, basic
            // feature/LED flags, two reserved bytes, and battery level. The
            // virtual peer reports the extension exposed by the same host-side
            // semantic sample used for its periodic input reports. Preserve the
            // LED state that the guest itself selected.
            WiiRemoteInput::KpadSample sample{};
            const bool haveSample = WiiRemoteInput::ReadKpadSample(0u, sample);
            const uint16_t buttons = haveSample
                ? static_cast<uint16_t>(sample.hold & 0xFFFFu)
                : 0u;
            const uint8_t statusFlags = static_cast<uint8_t>(
                g_btLedState | ((haveSample && sample.hasNunchuk) ? 0x02u : 0u));
            const uint8_t status[] = {
                0xA1, 0x20,
                static_cast<uint8_t>(buttons & 0xFFu),
                static_cast<uint8_t>(buttons >> 8),
                statusFlags,
                0x00, 0x00,
                0xC0,       // Healthy/full virtual battery.
            };
            QueueMeteorBtL2capData(g_btGuestHidInterruptCid, status,
                                   static_cast<uint16_t>(sizeof(status)));
            RT_LOG(RT_TAG_OS) << "RDSPAF BT USB: Wii Remote status request; queued report 0x20 flags=0x"
                              << std::hex << static_cast<uint32_t>(statusFlags)
                              << " battery=0xc0" << std::dec << std::endl;
        } else if (hidTransaction == 0xA2u && reportId == 0x12u && l2capLength >= 4u &&
            g_btGuestHidInterruptCid != 0u) {
            const uint8_t flags = Memory::Read8(payload + 2u);
            const uint8_t mode = Memory::Read8(payload + 3u);
            g_btReportMode = mode;
            g_btReportContinuous = (flags & 0x04u) != 0u;
            g_btNextInputReport = {};

            if ((flags & 0x02u) != 0u) {
                const uint8_t ack[] = {
                    0xA1, 0x22, // HID DATA input, output-report acknowledgement.
                    0x00, 0x00, // No buttons pressed.
                    0x12, 0x00, // Report 0x12 completed successfully.
                };
                QueueMeteorBtL2capData(g_btGuestHidInterruptCid, ack,
                                       static_cast<uint16_t>(sizeof(ack)));
            }
            if (mode == 0x30u) {
                QueueMeteorBtCoreInputReport(g_btGuestHidInterruptCid);
                g_btNextInputReport = std::chrono::steady_clock::now() +
                                      std::chrono::milliseconds(16);
            } else if (mode == 0x35u) {
                QueueMeteorBtCoreAccelNunchukInputReport(g_btGuestHidInterruptCid);
                g_btNextInputReport = std::chrono::steady_clock::now() +
                                      std::chrono::milliseconds(16);
            }
            RT_LOG(RT_TAG_OS) << "RDSPAF BT USB: Wii Remote report mode=0x"
                              << std::hex << static_cast<uint32_t>(mode)
                              << " flags=0x" << static_cast<uint32_t>(flags)
                              << std::dec << " ack=" << (((flags & 0x02u) != 0u) ? 1 : 0)
                              << " continuous=" << (g_btReportContinuous ? 1 : 0)
                              << std::endl;
        } else if (hidTransaction == 0xA2u && reportId == 0x16u && l2capLength >= 8u &&
                   g_btGuestHidInterruptCid != 0u) {
            const uint32_t address =
                (static_cast<uint32_t>(Memory::Read8(payload + 2u)) << 24u) |
                (static_cast<uint32_t>(Memory::Read8(payload + 3u)) << 16u) |
                (static_cast<uint32_t>(Memory::Read8(payload + 4u)) << 8u) |
                static_cast<uint32_t>(Memory::Read8(payload + 5u));
            const uint8_t size = Memory::Read8(payload + 6u);
            const uint8_t firstData = Memory::Read8(payload + 7u);
            bool acceptedExtensionWrite =
                (address == 0x04A400F0u && size == 1u && firstData == 0x55u) ||
                (address == 0x04A400F0u && size == 1u && firstData == 0xAAu) ||
                (address == 0x04A400FBu && size == 1u && firstData == 0x00u);
            if (address == 0x04A400F0u && size == 1u &&
                (firstData == 0x55u || firstData == 0xAAu)) {
                std::lock_guard<std::mutex> lock(g_btUsbMutex);
                g_btExtensionEncryptionEnabled = firstData == 0xAAu;
            }
            if (address == 0x04A40040u && size == 6u) {
                acceptedExtensionWrite = true;
                std::lock_guard<std::mutex> lock(g_btUsbMutex);
                for (uint32_t i = 0; i < 6u; ++i) {
                    g_btExtensionKey[i] = Memory::Read8(payload + 7u + i);
                }
                g_btExtensionKeyParts |= 0x01u;
            }
            if (address == 0x04A40046u && size == 6u) {
                acceptedExtensionWrite = true;
                std::lock_guard<std::mutex> lock(g_btUsbMutex);
                for (uint32_t i = 0; i < 6u; ++i) {
                    g_btExtensionKey[6u + i] = Memory::Read8(payload + 7u + i);
                }
                g_btExtensionKeyParts |= 0x02u;
            }
            if (address == 0x04A4004Cu && size == 4u) {
                acceptedExtensionWrite = true;
                std::lock_guard<std::mutex> lock(g_btUsbMutex);
                for (uint32_t i = 0; i < 4u; ++i) {
                    g_btExtensionKey[12u + i] = Memory::Read8(payload + 7u + i);
                }
                g_btExtensionKeyParts |= 0x04u;
            }
            if (acceptedExtensionWrite) {
                const uint8_t ack[] = {
                    0xA1, 0x22,
                    0x00, 0x00,
                    0x16, 0x00,
                };
                QueueMeteorBtL2capData(g_btGuestHidInterruptCid, ack,
                                       static_cast<uint16_t>(sizeof(ack)));
                RT_LOG(RT_TAG_OS) << "RDSPAF BT USB: Wii Remote extension init write address=0x"
                                  << std::hex << address
                                  << " value=0x" << static_cast<uint32_t>(firstData)
                                  << std::dec << "; acked" << std::endl;
            } else {
                RT_LOG(RT_TAG_OS) << "RDSPAF BT USB: unhandled Wii Remote write report address=0x"
                                  << std::hex << address
                                  << " size=" << std::dec << static_cast<uint32_t>(size)
                                  << " first=0x" << std::hex << static_cast<uint32_t>(firstData)
                                  << std::dec << std::endl;
            }
        } else
        if (hidTransaction == 0xA2u && reportId == 0x17u && l2capLength >= 8u &&
            g_btGuestHidInterruptCid != 0u) {
            const uint32_t address =
                (static_cast<uint32_t>(Memory::Read8(payload + 3u)) << 16u) |
                (static_cast<uint32_t>(Memory::Read8(payload + 4u)) << 8u) |
                static_cast<uint32_t>(Memory::Read8(payload + 5u));
            const uint16_t readSize =
                static_cast<uint16_t>((Memory::Read8(payload + 6u) << 8u) |
                                      Memory::Read8(payload + 7u));
            if (address == 0x00001770u && readSize == 1u) {
                uint8_t reply[23] = {};
                reply[0] = 0xA1; // HID DATA input transaction.
                reply[1] = 0x21; // Read Memory Data input report.
                reply[2] = 0x00; // Core buttons high/low: no buttons pressed.
                reply[3] = 0x00;
                reply[4] = 0x08; // one byte (size-1=0), invalid-address error 8.
                reply[5] = 0x17;
                reply[6] = 0x70;
                QueueMeteorBtL2capData(g_btGuestHidInterruptCid, reply,
                                       static_cast<uint16_t>(sizeof(reply)));
                RT_LOG(RT_TAG_OS)
                    << "RDSPAF BT USB: Wii Remote EEPROM probe 0x1770 size=1; queued report 0x21 error=8"
                    << std::endl;
            } else if (address == 0x00000000u && readSize == 0x2Au) {
                // WiiBrew's documented virgin-controller EEPROM dump for the
                // complete 0x0000..0x0029 calibration/preset region. These two
                // redundant IR calibration blocks and two accelerometer blocks
                // include valid checksums, unlike a fabricated all-zero block.
                static constexpr uint8_t kVirginCalibration[0x2A] = {
                    0xA1, 0xAA, 0x8B, 0x99, 0xAE, 0x9E, 0x78, 0x30,
                    0xA7, 0x74, 0xD3, 0xA1, 0xAA, 0x8B, 0x99, 0xAE,
                    0x9E, 0x78, 0x30, 0xA7, 0x74, 0xD3, 0x82, 0x82,
                    0x82, 0x15, 0x9C, 0x9C, 0x9E, 0x38, 0x40, 0x3E,
                    0x82, 0x82, 0x82, 0x15, 0x9C, 0x9C, 0x9E, 0x38,
                    0x40, 0x3E,
                };
                for (uint32_t offset = 0u; offset < sizeof(kVirginCalibration); offset += 0x10u) {
                    const uint32_t chunkSize = std::min<uint32_t>(0x10u,
                                                                  sizeof(kVirginCalibration) - offset);
                    const uint16_t chunkAddress = static_cast<uint16_t>(offset);
                    uint8_t reply[23] = {};
                    reply[0] = 0xA1;
                    reply[1] = 0x21;
                    reply[4] = static_cast<uint8_t>((chunkSize - 1u) << 4u);
                    reply[5] = static_cast<uint8_t>(chunkAddress >> 8u);
                    reply[6] = static_cast<uint8_t>(chunkAddress & 0xFFu);
                    std::memcpy(reply + 7u, kVirginCalibration + offset, chunkSize);
                    QueueMeteorBtL2capData(g_btGuestHidInterruptCid, reply,
                                           static_cast<uint16_t>(sizeof(reply)));
                }
                RT_LOG(RT_TAG_OS)
                    << "RDSPAF BT USB: Wii Remote calibration EEPROM read 0x0000 size=42; queued three report 0x21 chunks"
                    << std::endl;
            } else if ((address == 0x0000002Au || address == 0x00000062u) &&
                       readSize == 0x38u) {
                // The retail guest has now explicitly requested two adjacent
                // 56-byte windows from the clean Wii Remote EEPROM user-data
                // range: 0x002A..0x0061 and 0x0062..0x0099. Both are zero on a
                // clean controller. Report 0x21 carries at most 16 bytes, so
                // return three 16-byte chunks and a final 8-byte chunk.
                for (uint32_t offset = 0u; offset < 0x38u; offset += 0x10u) {
                    const uint32_t chunkSize = std::min<uint32_t>(0x10u, 0x38u - offset);
                    const uint16_t chunkAddress = static_cast<uint16_t>(address + offset);
                    uint8_t reply[23] = {};
                    reply[0] = 0xA1; // HID DATA input transaction.
                    reply[1] = 0x21; // Read Memory Data input report.
                    reply[2] = 0x00; // No buttons pressed.
                    reply[3] = 0x00;
                    reply[4] = static_cast<uint8_t>((chunkSize - 1u) << 4u); // size-1, error=0.
                    reply[5] = static_cast<uint8_t>(chunkAddress >> 8u);
                    reply[6] = static_cast<uint8_t>(chunkAddress & 0xFFu);
                    QueueMeteorBtL2capData(g_btGuestHidInterruptCid, reply,
                                           static_cast<uint16_t>(sizeof(reply)));
                }
                RT_LOG(RT_TAG_OS)
                    << "RDSPAF BT USB: Wii Remote EEPROM read 0x" << std::hex << address
                    << std::dec << " size=56; queued four zeroed report 0x21 chunks"
                    << std::endl;
            } else if (address == 0x00A400FEu && readSize == 2u) {
                // Observed retail Nunchuk identification probe after the new-way
                // extension initialization (F0=55, FB=00). In that mode the ID
                // bytes are unencrypted; 0xA400FE..FF are 00 00 for a Nunchuk.
                uint8_t reply[23] = {};
                reply[0] = 0xA1;
                reply[1] = 0x21;
                reply[4] = 0x10; // two bytes (size-1=1), error=0.
                reply[5] = 0x00;
                reply[6] = 0xFE;
                reply[7] = 0x00;
                reply[8] = 0x00;
                QueueMeteorBtL2capData(g_btGuestHidInterruptCid, reply,
                                       static_cast<uint16_t>(sizeof(reply)));
                RT_LOG(RT_TAG_OS)
                    << "RDSPAF BT USB: Wii Remote Nunchuk ID read 0xa400fe size=2; queued 00 00"
                    << std::endl;
            } else if (address == 0x00A40020u && readSize == 0x20u) {
                // The retail guest requests the complete encrypted Nunchuk calibration
                // area immediately after writing the 16-byte first-party extension key.
                // A physical Nunchuk exposes two copies of its 16-byte calibration block
                // at register offsets 0x20 and 0x30. These are typical physical-Nunchuk
                // values (zero/1G accelerometer and stick max/min/center) with the two
                // standard +0x55 checksum bytes.
                std::array<uint8_t, 0x20> calibration = {
                    0x80, 0x80, 0x80, 0x00, 0xB3, 0xB3, 0xB3, 0x00,
                    0xE0, 0x20, 0x80, 0xE0, 0x20, 0x80, 0xEE, 0x43,
                    0x80, 0x80, 0x80, 0x00, 0xB3, 0xB3, 0xB3, 0x00,
                    0xE0, 0x20, 0x80, 0xE0, 0x20, 0x80, 0xEE, 0x43,
                };
                std::array<uint8_t, 16> extensionKey{};
                uint8_t extensionKeyParts = 0u;
                bool extensionEncryptionEnabled = false;
                {
                    std::lock_guard<std::mutex> lock(g_btUsbMutex);
                    extensionKey = g_btExtensionKey;
                    extensionKeyParts = g_btExtensionKeyParts;
                    extensionEncryptionEnabled = g_btExtensionEncryptionEnabled;
                }
                MeteorWiiExtensionCrypto::KeyTables tables{};
                if (extensionEncryptionEnabled && extensionKeyParts == 0x07u &&
                    MeteorWiiExtensionCrypto::GenerateFirstPartyKeyTables(extensionKey,
                                                                           tables)) {
                    MeteorWiiExtensionCrypto::Encrypt(calibration.data(), 0x20u,
                                                      calibration.size(), tables);
                    for (uint32_t offset = 0u; offset < calibration.size(); offset += 0x10u) {
                        uint8_t reply[23] = {};
                        reply[0] = 0xA1;
                        reply[1] = 0x21;
                        reply[4] = 0xF0; // 16 bytes, error=0.
                        const uint16_t chunkAddress = static_cast<uint16_t>(0x20u + offset);
                        reply[5] = static_cast<uint8_t>(chunkAddress >> 8u);
                        reply[6] = static_cast<uint8_t>(chunkAddress & 0xFFu);
                        std::memcpy(reply + 7u, calibration.data() + offset, 0x10u);
                        QueueMeteorBtL2capData(g_btGuestHidInterruptCid, reply,
                                               static_cast<uint16_t>(sizeof(reply)));
                    }
                    RT_LOG(RT_TAG_OS)
                        << "RDSPAF BT USB: Wii Remote Nunchuk encrypted calibration read 0xa40020 size=32; queued two report 0x21 chunks"
                        << std::endl;
                } else {
                    RT_LOG(RT_TAG_OS)
                        << "RDSPAF BT USB: Wii Remote Nunchuk key did not match first-party retail keygen; calibration read not answered"
                        << std::endl;
                }
            } else {
                RT_LOG(RT_TAG_OS) << "RDSPAF BT USB: unhandled Wii Remote read report address=0x"
                                  << std::hex << address << std::dec
                                  << " size=" << readSize << std::endl;
            }
        }
        return;
    }

    if (cid != 0x0001u || l2capLength < 4u) {
        return;
    }

    uint32_t offset = 8u;
    const uint32_t end = 8u + l2capLength;
    while (offset + 4u <= end && offset + 4u <= length) {
        const uint8_t code = Memory::Read8(buffer + offset);
        const uint8_t identifier = Memory::Read8(buffer + offset + 1u);
        const uint16_t commandLength = ReadUsbLittleEndian16(buffer + offset + 2u);
        const uint32_t payload = buffer + offset + 4u;
        if (offset + 4u + commandLength > end || offset + 4u + commandLength > length) {
            break;
        }

        if (code == 0x03u && commandLength >= 8u) { // Connection Response
            const uint16_t dcid = ReadUsbLittleEndian16(payload);
            const uint16_t scid = ReadUsbLittleEndian16(payload + 2u);
            const uint16_t result = ReadUsbLittleEndian16(payload + 4u);
            if (result == 0u && scid == 0x0040u) {
                g_btGuestHidControlCid = dcid;
                RT_LOG(RT_TAG_OS) << "RDSPAF BT USB: HID Control connected guestCID=0x"
                                  << std::hex << dcid << std::dec << std::endl;
            } else if (result == 0u && scid == 0x0041u) {
                g_btGuestHidInterruptCid = dcid;
                RT_LOG(RT_TAG_OS) << "RDSPAF BT USB: HID Interrupt connected guestCID=0x"
                                  << std::hex << dcid << std::dec << std::endl;
            }
        } else if (code == 0x04u && commandLength >= 4u) { // Configuration Request
            const uint16_t destinationCid = ReadUsbLittleEndian16(payload);
            if (destinationCid == 0x0040u && g_btGuestHidControlCid != 0u) {
                QueueMeteorBtL2capConfigResponse(identifier, 0x0040u);
                if (!g_btControlPeerConfigQueued) {
                    g_btControlPeerConfigQueued = true;
                    QueueMeteorBtL2capConfigRequest(0x02u, g_btGuestHidControlCid);
                }
            } else if (destinationCid == 0x0041u && g_btGuestHidInterruptCid != 0u) {
                QueueMeteorBtL2capConfigResponse(identifier, 0x0041u);
                if (!g_btInterruptPeerConfigQueued) {
                    g_btInterruptPeerConfigQueued = true;
                    QueueMeteorBtL2capConfigRequest(0x04u, g_btGuestHidInterruptCid);
                }
            }
        } else if (code == 0x05u && commandLength >= 6u) { // Configuration Response
            const uint16_t result = ReadUsbLittleEndian16(payload + 4u);
            if (identifier == 0x02u && result == 0u && !g_btInterruptConnectionRequestQueued) {
                g_btInterruptConnectionRequestQueued = true;
                QueueMeteorBtHidInterruptConnectionRequest();
            }
        }

        offset += 4u + commandLength;
    }
}

bool MeteorBtGuestHasVirtualRemoteHidSlot() {
    static constexpr uint32_t kHidDeviceTable = 0x805BB4B8u;
    static constexpr uint32_t kHidDeviceStride = 0x34u;
    static constexpr uint32_t kHidDeviceCount = 16u;
    static constexpr uint8_t kRemoteBdAddr[6] = {
        0x00, 0x17, 0xAB, 0x00, 0x00, 0x01,
    };

    for (uint32_t slot = 0u; slot < kHidDeviceCount; ++slot) {
        const uint32_t record = kHidDeviceTable + slot * kHidDeviceStride;
        if (!Memory::Contains(record, 7u) || Memory::Read8(record) == 0u) {
            continue;
        }
        bool matches = true;
        for (uint32_t i = 0u; i < sizeof(kRemoteBdAddr); ++i) {
            if (Memory::Read8(record + 1u + i) != kRemoteBdAddr[i]) {
                matches = false;
                break;
            }
        }
        if (matches) {
            return true;
        }
    }
    return false;
}

void QueueMeteorBtInitialHidControlRequestIfReady() {
    if (!RuntimeConfigFile::WiiVirtualRemoteEnabled(false) ||
        !MeteorBtGuestHasVirtualRemoteHidSlot()) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_btUsbMutex);
        if (g_btInitialL2capRequestQueued) {
            return;
        }
        g_btInitialL2capRequestQueued = true;
    }

    // The peer initiates HID Control only after retail BTA HH has registered a
    // device slot for this BD_ADDR. PB=2 is the guest's first/full L2CAP PDU
    // path. Signaling CID=1, PSM=0x0011, peer SCID=0x0040.
    static constexpr uint8_t kHidControlConnectionRequest[] = {
        0x0B, 0x20, 0x0C, 0x00,
        0x08, 0x00, 0x01, 0x00,
        0x02, 0x01, 0x04, 0x00,
        0x11, 0x00, 0x40, 0x00,
    };
    QueueMeteorBtAclIn(kHidControlConnectionRequest,
                       static_cast<uint32_t>(sizeof(kHidControlConnectionRequest)));
    RT_LOG(RT_TAG_OS) << "RDSPAF BT USB: queued virtual Wii Remote HID Control L2CAP ConnReq"
                      << std::endl;
}

void DeliverMeteorBtAclIfPossible(CpuContext* cpu) {
    MeteorBtPendingTransfer pending{};
    std::vector<uint8_t> packet;
    {
        std::lock_guard<std::mutex> lock(g_btUsbMutex);
        // Preserve controller ordering: do not expose ACL data until every HCI
        // event already queued for the baseband transition has been consumed.
        if (!g_btAclIn.active || g_btQueuedAclIn.empty() || !g_btQueuedEvents.empty()) {
            return;
        }
        packet = std::move(g_btQueuedAclIn.front());
        g_btQueuedAclIn.pop_front();
        if (packet.size() > g_btAclIn.length) {
            RT_LOG(RT_TAG_OS) << "RDSPAF BT USB: ACL IN len=" << packet.size()
                              << " exceeds bulk buffer len=" << g_btAclIn.length
                              << std::endl;
            return;
        }
        pending = g_btAclIn;
        g_btAclIn = {};
    }

    WriteMeteorBtUsbPayload(pending.buffer, packet.data(), static_cast<uint32_t>(packet.size()));
    Memory::Write32(pending.request + 0x04u, static_cast<uint32_t>(packet.size()));
    NandQueueIosCallback(pending.callback, static_cast<int32_t>(packet.size()), pending.callbackArg);
    FreeMeteorAsyncRequest(cpu, pending.request);
    if (MeteorRuntimeTraceEnabled()) {
        RT_LOG(RT_TAG_OS) << "RDSPAF BT USB: ACL IN deliver len=" << packet.size()
                          << " callbackArg=0x" << std::hex << pending.callbackArg
                          << std::dec << std::endl;
    }
}

bool QueueMeteorBtHciCommandComplete(CpuContext* cpu, uint16_t opcode, uint32_t parameterBuffer,
                                     uint8_t parameterLength) {
    static constexpr uint8_t kReadBufferSizeComplete[] = {
        0x0E, 0x0B, 0x01, 0x05, 0x10, 0x00, 0x53, 0x01, 0x40, 0x0A, 0x00, 0x00, 0x00,
    };
    static constexpr uint8_t kReadLocalVersionComplete[] = {
        0x0E, 0x0C, 0x01, 0x01, 0x10, 0x00, 0x03, 0xA7, 0x40, 0x03, 0x0F, 0x00, 0x0E, 0x43,
    };
    static constexpr uint8_t kReadBdAddrComplete[] = {
        0x0E, 0x0A, 0x01, 0x09, 0x10, 0x00, 0xFF, 0x00, 0x79, 0x19, 0x02, 0x11,
    };
    static constexpr uint8_t kReadLocalFeaturesComplete[] = {
        0x0E, 0x0C, 0x01, 0x03, 0x10, 0x00, 0x03, 0x00, 0x00, 0x70, 0x00, 0x00, 0x00, 0x00,
    };
    static constexpr uint8_t kReadStoredLinkKeyComplete[] = {
        0x0E, 0x08, 0x01, 0x0D, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    static constexpr uint8_t kVendorFc4fComplete[] = {0x0E, 0x03, 0x01, 0x4F, 0xFC};
    static constexpr uint8_t kVendorFc4cComplete[] = {0x0E, 0x03, 0x01, 0x4C, 0xFC};

    switch (opcode) {
    case 0x1005:
        if (parameterLength != 0) return false;
        QueueMeteorBtHciEvent(cpu, kReadBufferSizeComplete, sizeof(kReadBufferSizeComplete));
        return true;
    case 0x1001:
        if (parameterLength != 0) return false;
        QueueMeteorBtHciEvent(cpu, kReadLocalVersionComplete, sizeof(kReadLocalVersionComplete));
        return true;
    case 0x1009:
        if (parameterLength != 0) return false;
        QueueMeteorBtHciEvent(cpu, kReadBdAddrComplete, sizeof(kReadBdAddrComplete));
        return true;
    case 0x1003:
        if (parameterLength != 0) return false;
        QueueMeteorBtHciEvent(cpu, kReadLocalFeaturesComplete, sizeof(kReadLocalFeaturesComplete));
        return true;
    case 0x0C0D: {
        if (parameterLength != 7 || !Memory::Contains(parameterBuffer, 7u)) return false;

        if (!RuntimeConfigFile::WiiVirtualRemoteEnabled(false)) {
            QueueMeteorBtHciEvent(cpu, kReadStoredLinkKeyComplete,
                                  sizeof(kReadStoredLinkKeyComplete));
            return true;
        }

        static constexpr uint8_t kRemoteBdAddr[6] = {
            0x01, 0x00, 0x00, 0xAB, 0x17, 0x00,
        };
        static constexpr uint8_t kVirtualStoredLinkKey[16] = {
            0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
            0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
        };

        const bool readAll = Memory::Read8(parameterBuffer + 6u) != 0u;
        bool addressMatches = true;
        for (uint32_t i = 0u; i < sizeof(kRemoteBdAddr); ++i) {
            if (Memory::Read8(parameterBuffer + i) != kRemoteBdAddr[i]) {
                addressMatches = false;
                break;
            }
        }
        const bool returnVirtualKey = readAll || addressMatches;

        if (returnVirtualKey) {
            std::array<uint8_t, 25> returnLinkKeys{};
            returnLinkKeys[0] = 0x15;
            returnLinkKeys[1] = 0x17;
            returnLinkKeys[2] = 0x01;
            std::memcpy(returnLinkKeys.data() + 3u, kRemoteBdAddr, sizeof(kRemoteBdAddr));
            std::memcpy(returnLinkKeys.data() + 9u, kVirtualStoredLinkKey,
                        sizeof(kVirtualStoredLinkKey));
            QueueMeteorBtHciEvent(cpu, returnLinkKeys.data(),
                                  static_cast<uint32_t>(returnLinkKeys.size()));
        }

        const uint8_t complete[] = {
            0x0E, 0x08, 0x01, 0x0D, 0x0C, 0x00,
            0x10, 0x00,
            static_cast<uint8_t>(returnVirtualKey ? 0x01 : 0x00), 0x00,
        };
        QueueMeteorBtHciEvent(cpu, complete, sizeof(complete));
        RT_LOG(RT_TAG_OS) << "RDSPAF BT USB: Read Stored Link Key virtual="
                          << (returnVirtualKey ? 1 : 0)
                          << " readAll=" << (readAll ? 1 : 0) << std::endl;
        return true;
    }
    case 0x0C12: {
        if (parameterLength != 7 || !Memory::Contains(parameterBuffer, 7u)) return false;

        static constexpr uint8_t kRemoteBdAddr[6] = {
            0x01, 0x00, 0x00, 0xAB, 0x17, 0x00,
        };
        const bool deleteAll = Memory::Read8(parameterBuffer + 6u) != 0u;
        bool addressMatches = true;
        for (uint32_t i = 0u; i < sizeof(kRemoteBdAddr); ++i) {
            if (Memory::Read8(parameterBuffer + i) != kRemoteBdAddr[i]) {
                addressMatches = false;
                break;
            }
        }
        const bool deletedVirtualKey = RuntimeConfigFile::WiiVirtualRemoteEnabled(false) &&
                                       (deleteAll || addressMatches);
        const uint8_t complete[] = {
            0x0E, 0x06, 0x01, 0x12, 0x0C, 0x00,
            static_cast<uint8_t>(deletedVirtualKey ? 0x01 : 0x00), 0x00,
        };
        QueueMeteorBtHciEvent(cpu, complete, sizeof(complete));
        RT_LOG(RT_TAG_OS) << "RDSPAF BT USB: Delete Stored Link Key virtual="
                          << (deletedVirtualKey ? 1 : 0)
                          << " deleteAll=" << (deleteAll ? 1 : 0) << std::endl;
        return true;
    }
    case 0xFC4F:
        QueueMeteorBtHciEvent(cpu, kVendorFc4fComplete, sizeof(kVendorFc4fComplete));
        return true;
    case 0xFC4C:
        QueueMeteorBtHciEvent(cpu, kVendorFc4cComplete, sizeof(kVendorFc4cComplete));
        return true;
    case 0x0C03:
        if (parameterLength != 0) return false;
        break;
    case 0x0C24:
        if (parameterLength != 3) return false;
        break;
    case 0x0C13:
        if (parameterLength != 248) return false;
        break;
    case 0x0C0A:
    case 0x0C45:
    case 0x0C47:
    case 0x0C43:
        if (parameterLength != 1) return false;
        break;
    case 0x0C1A: // Write Scan Enable
        if (parameterLength != 1) return false;
        break;
    case 0x0C33:
        if (parameterLength != 7) return false;
        break;
    case 0x0C18:
        if (parameterLength != 2) return false;
        break;
    default:
        return false;
    }
    const uint8_t complete[] = {
        0x0E, 0x04, 0x01,
        static_cast<uint8_t>(opcode & 0xFFu),
        static_cast<uint8_t>(opcode >> 8),
        0x00,
    };
    QueueMeteorBtHciEvent(cpu, complete, sizeof(complete));
    return true;
}

void QueueMeteorBtVirtualConnectionRequestIfNeeded(CpuContext* cpu) {
    if (!cpu || !RuntimeConfigFile::WiiVirtualRemoteEnabled(false)) {
        return;
    }

    bool shouldQueue = false;
    {
        std::lock_guard<std::mutex> lock(g_btUsbMutex);
        // Page scan (bit 1) is what allows a previously paired Wii Remote to
        // initiate a baseband connection to the console. Only expose the host-
        // backed virtual remote when the user explicitly enabled it and retail
        // BTE has enabled page scan through HCI Write Scan Enable.
        if ((g_btScanEnable & 0x02u) != 0u && !g_btVirtualConnectionRequestQueued) {
            g_btVirtualConnectionRequestQueued = true;
            shouldQueue = true;
        }
    }
    if (!shouldQueue) {
        return;
    }

    // HCI Connection Request event (0x04): BD_ADDR, Class of Device, link type.
    // 00:17:AB is Nintendo's OUI; the address is encoded least-significant byte
    // first on HCI, and 0x002504 is the Wii Remote's peripheral class.
    static constexpr uint8_t kConnectionRequest[] = {
        0x04, 0x0A,
        0x01, 0x00, 0x00, 0xAB, 0x17, 0x00,
        0x04, 0x25, 0x00,
        0x01,
    };
    QueueMeteorBtHciEvent(cpu, kConnectionRequest, sizeof(kConnectionRequest));
    RT_LOG(RT_TAG_OS) << "RDSPAF BT USB: queued virtual Wii Remote HCI Connection Request"
                      << std::endl;
}

bool QueueMeteorBtAcceptConnectionEvents(CpuContext* cpu, uint32_t parameterBuffer,
                                         uint8_t parameterLength) {
    if (!cpu || parameterLength != 7u || !Memory::Contains(parameterBuffer, 7u)) {
        return false;
    }

    static constexpr uint8_t kRemoteBdAddr[6] = {
        0x01, 0x00, 0x00, 0xAB, 0x17, 0x00,
    };
    for (uint32_t i = 0; i < 6u; ++i) {
        if (Memory::Read8(parameterBuffer + i) != kRemoteBdAddr[i]) {
            return false;
        }
    }

    // HCI Accept Connection Request is an asynchronous link-control command:
    // first Command Status acknowledges acceptance, then Connection Complete
    // publishes the ACL handle once the emulated baseband link is established.
    static constexpr uint8_t kCommandStatus[] = {
        0x0F, 0x04, 0x00, 0x01, 0x09, 0x04,
    };
    static constexpr uint8_t kConnectionComplete[] = {
        0x03, 0x0B, 0x00,
        0x0B, 0x00,
        0x01, 0x00, 0x00, 0xAB, 0x17, 0x00,
        0x01, 0x00,
    };
    QueueMeteorBtHciEvent(cpu, kCommandStatus, sizeof(kCommandStatus));
    QueueMeteorBtHciEvent(cpu, kConnectionComplete, sizeof(kConnectionComplete));
    // Do not synthesize an inbound HID Control L2CAP connection here. Retail
    // hidh_l2cif_connect_ind only accepts an inbound PSM 0x0011 request after
    // HID_HostAddDev has created a slot for the peer BD_ADDR. At this point the
    // guest has only accepted the baseband link; its BTA HH discovery/open path
    // has not populated that slot yet, so an immediate remote ConnReq is
    // correctly rejected with L2CAP result 0x0003. Let the guest finish its
    // normal HID-host discovery/open sequence and observe which L2CAP request it
    // initiates over ACL OUT instead of forcing the channel from the backend.
    RT_LOG(RT_TAG_OS) << "RDSPAF BT USB: accepted virtual Wii Remote baseband connection handle=0xb"
                      << std::endl;
    return true;
}

bool MeteorBtMatchesVirtualRemoteAddress(uint32_t address) {
    static constexpr uint8_t kRemoteBdAddr[6] = {
        0x01, 0x00, 0x00, 0xAB, 0x17, 0x00,
    };
    if (!Memory::Contains(address, sizeof(kRemoteBdAddr))) {
        return false;
    }
    for (uint32_t i = 0; i < sizeof(kRemoteBdAddr); ++i) {
        if (Memory::Read8(address + i) != kRemoteBdAddr[i]) {
            return false;
        }
    }
    return true;
}

bool QueueMeteorBtRemoteNameEvents(CpuContext* cpu, uint32_t parameterBuffer,
                                   uint8_t parameterLength) {
    if (!cpu || parameterLength != 10u || !MeteorBtMatchesVirtualRemoteAddress(parameterBuffer)) {
        return false;
    }

    static constexpr uint8_t kCommandStatus[] = {
        0x0F, 0x04, 0x00, 0x01, 0x19, 0x04,
    };
    std::vector<uint8_t> complete(257u, 0u);
    complete[0] = 0x07; // Remote Name Request Complete
    complete[1] = 0xFF; // status + BD_ADDR + 248-byte name
    complete[2] = 0x00;
    static constexpr uint8_t kRemoteBdAddr[6] = {
        0x01, 0x00, 0x00, 0xAB, 0x17, 0x00,
    };
    std::memcpy(complete.data() + 3u, kRemoteBdAddr, sizeof(kRemoteBdAddr));
    static constexpr char kRemoteName[] = "Nintendo RVL-CNT-01";
    std::memcpy(complete.data() + 9u, kRemoteName, sizeof(kRemoteName) - 1u);

    QueueMeteorBtHciEvent(cpu, kCommandStatus, sizeof(kCommandStatus));
    QueueMeteorBtHciEvent(cpu, complete.data(), static_cast<uint32_t>(complete.size()));
    RT_LOG(RT_TAG_OS) << "RDSPAF BT USB: replied to virtual Wii Remote name request"
                      << std::endl;
    return true;
}

bool QueueMeteorBtAuthenticationRequestedEvents(CpuContext* cpu, uint32_t parameterBuffer,
                                                uint8_t parameterLength) {
    if (!cpu || parameterLength != 2u || !Memory::Contains(parameterBuffer, 2u)) {
        return false;
    }
    const uint16_t handle = ReadUsbLittleEndian16(parameterBuffer);
    if ((handle & 0x0FFFu) != 0x000Bu) {
        return false;
    }

    static constexpr uint8_t kCommandStatus[] = {
        0x0F, 0x04, 0x00, 0x01, 0x11, 0x04,
    };
    static constexpr uint8_t kLinkKeyRequest[] = {
        0x17, 0x06,
        0x01, 0x00, 0x00, 0xAB, 0x17, 0x00,
    };
    QueueMeteorBtHciEvent(cpu, kCommandStatus, sizeof(kCommandStatus));
    QueueMeteorBtHciEvent(cpu, kLinkKeyRequest, sizeof(kLinkKeyRequest));
    RT_LOG(RT_TAG_OS) << "RDSPAF BT USB: Authentication Requested handle=0x"
                      << std::hex << handle
                      << "; requested stored link key from guest"
                      << std::dec << std::endl;
    return true;
}

bool QueueMeteorBtLinkKeyReplyEvents(CpuContext* cpu, uint16_t opcode,
                                    uint32_t parameterBuffer, uint8_t parameterLength) {
    const uint8_t expectedLength = opcode == 0x040Bu ? 22u : 6u;
    if (!cpu || parameterLength != expectedLength ||
        !MeteorBtMatchesVirtualRemoteAddress(parameterBuffer)) {
        return false;
    }

    static constexpr uint8_t kRemoteBdAddr[6] = {
        0x01, 0x00, 0x00, 0xAB, 0x17, 0x00,
    };
    const uint8_t complete[] = {
        0x0E, 0x0A, 0x01,
        static_cast<uint8_t>(opcode & 0xFFu),
        static_cast<uint8_t>(opcode >> 8),
        0x00,
        kRemoteBdAddr[0], kRemoteBdAddr[1], kRemoteBdAddr[2],
        kRemoteBdAddr[3], kRemoteBdAddr[4], kRemoteBdAddr[5],
    };
    QueueMeteorBtHciEvent(cpu, complete, sizeof(complete));

    const uint8_t authStatus = opcode == 0x040Bu ? 0x00u : 0x06u;
    const uint8_t authComplete[] = {
        0x06, 0x03, authStatus, 0x0B, 0x00,
    };
    QueueMeteorBtHciEvent(cpu, authComplete, sizeof(authComplete));
    RT_LOG(RT_TAG_OS) << "RDSPAF BT USB: Link Key Request "
                      << (opcode == 0x040Bu ? "Reply" : "Negative Reply")
                      << "; Authentication Complete status=0x"
                      << std::hex << static_cast<uint32_t>(authStatus)
                      << std::dec << std::endl;
    return true;
}

bool QueueMeteorBtReadClockOffsetEvents(CpuContext* cpu, uint32_t parameterBuffer,
                                        uint8_t parameterLength) {
    if (!cpu || parameterLength != 2u || !Memory::Contains(parameterBuffer, 2u)) {
        return false;
    }
    const uint16_t handle = ReadUsbLittleEndian16(parameterBuffer);
    if ((handle & 0x0FFFu) != 0x000Bu) {
        return false;
    }

    static constexpr uint8_t kCommandStatus[] = {
        0x0F, 0x04, 0x00, 0x01, 0x1F, 0x04,
    };
    const uint8_t complete[] = {
        0x1C, 0x05, 0x00,
        static_cast<uint8_t>(handle & 0xFFu),
        static_cast<uint8_t>((handle >> 8) & 0x0Fu),
        0x00, 0x00,
    };
    QueueMeteorBtHciEvent(cpu, kCommandStatus, sizeof(kCommandStatus));
    QueueMeteorBtHciEvent(cpu, complete, sizeof(complete));
    RT_LOG(RT_TAG_OS) << "RDSPAF BT USB: replied to Read Clock Offset handle=0x"
                      << std::hex << handle << std::dec << std::endl;
    return true;
}

bool QueueMeteorBtReadRemoteVersionEvents(CpuContext* cpu, uint32_t parameterBuffer,
                                          uint8_t parameterLength) {
    if (!cpu || parameterLength != 2u || !Memory::Contains(parameterBuffer, 2u)) {
        return false;
    }
    const uint16_t handle = ReadUsbLittleEndian16(parameterBuffer);
    if ((handle & 0x0FFFu) != 0x000Bu) {
        return false;
    }

    static constexpr uint8_t kCommandStatus[] = {
        0x0F, 0x04, 0x00, 0x01, 0x1D, 0x04,
    };
    const uint8_t complete[] = {
        0x0C, 0x08, 0x00,
        static_cast<uint8_t>(handle & 0xFFu),
        static_cast<uint8_t>((handle >> 8) & 0x0Fu),
        0x02,       // LMP 1.2, matching the emulated original Wii Remote profile.
        0x0F, 0x00, // Broadcom Corporation company identifier.
        0x29, 0x02, // LMP subversion 0x0229.
    };
    QueueMeteorBtHciEvent(cpu, kCommandStatus, sizeof(kCommandStatus));
    QueueMeteorBtHciEvent(cpu, complete, sizeof(complete));
    RT_LOG(RT_TAG_OS) << "RDSPAF BT USB: replied to Read Remote Version handle=0x"
                      << std::hex << handle << std::dec << std::endl;
    return true;
}

bool QueueMeteorBtReadRemoteFeaturesEvents(CpuContext* cpu, uint32_t parameterBuffer,
                                           uint8_t parameterLength) {
    if (!cpu || parameterLength != 2u || !Memory::Contains(parameterBuffer, 2u)) {
        return false;
    }
    const uint16_t handle = ReadUsbLittleEndian16(parameterBuffer);
    if ((handle & 0x0FFFu) != 0x000Bu) {
        return false;
    }

    static constexpr uint8_t kCommandStatus[] = {
        0x0F, 0x04, 0x00, 0x01, 0x1B, 0x04,
    };
    const uint8_t complete[] = {
        0x0B, 0x0B, 0x00,
        static_cast<uint8_t>(handle & 0xFFu),
        static_cast<uint8_t>((handle >> 8) & 0x0Fu),
        0xBC, 0x02, 0x04, 0x38, 0x08, 0x00, 0x00, 0x00,
    };
    QueueMeteorBtHciEvent(cpu, kCommandStatus, sizeof(kCommandStatus));
    QueueMeteorBtHciEvent(cpu, complete, sizeof(complete));
    RT_LOG(RT_TAG_OS) << "RDSPAF BT USB: replied to Read Remote Features handle=0x"
                      << std::hex << handle << std::dec << std::endl;
    return true;
}

bool MeteorBtReadVirtualHandle(uint32_t parameterBuffer, uint8_t parameterLength,
                               uint16_t& handle) {
    if (parameterLength < 2u || !Memory::Contains(parameterBuffer, parameterLength)) {
        return false;
    }
    handle = ReadUsbLittleEndian16(parameterBuffer);
    return (handle & 0x0FFFu) == 0x000Bu;
}

bool QueueMeteorBtChangePacketTypeEvents(CpuContext* cpu, uint32_t parameterBuffer,
                                         uint8_t parameterLength) {
    uint16_t handle = 0u;
    if (!cpu || parameterLength != 4u ||
        !MeteorBtReadVirtualHandle(parameterBuffer, parameterLength, handle)) {
        return false;
    }
    const uint16_t packetType = ReadUsbLittleEndian16(parameterBuffer + 2u);
    const uint8_t status[] = {
        0x0F, 0x04, 0x00, 0x01, 0x0F, 0x04,
    };
    const uint8_t changed[] = {
        0x1D, 0x05, 0x00,
        static_cast<uint8_t>(handle & 0xFFu),
        static_cast<uint8_t>((handle >> 8) & 0x0Fu),
        static_cast<uint8_t>(packetType & 0xFFu),
        static_cast<uint8_t>(packetType >> 8),
    };
    QueueMeteorBtHciEvent(cpu, status, sizeof(status));
    QueueMeteorBtHciEvent(cpu, changed, sizeof(changed));
    RT_LOG(RT_TAG_OS) << "RDSPAF BT USB: changed virtual link packet type handle=0x"
                      << std::hex << handle << " type=0x" << packetType
                      << std::dec << std::endl;
    return true;
}

bool QueueMeteorBtWriteHandleSettingComplete(CpuContext* cpu, uint16_t opcode,
                                             uint32_t parameterBuffer,
                                             uint8_t parameterLength) {
    uint16_t handle = 0u;
    if (!cpu || parameterLength != 4u ||
        !MeteorBtReadVirtualHandle(parameterBuffer, parameterLength, handle)) {
        return false;
    }
    const uint8_t complete[] = {
        0x0E, 0x06, 0x01,
        static_cast<uint8_t>(opcode & 0xFFu),
        static_cast<uint8_t>(opcode >> 8),
        0x00,
        static_cast<uint8_t>(handle & 0xFFu),
        static_cast<uint8_t>((handle >> 8) & 0x0Fu),
    };
    QueueMeteorBtHciEvent(cpu, complete, sizeof(complete));
    return true;
}

bool QueueMeteorBtDisconnectEvents(CpuContext* cpu, uint32_t parameterBuffer,
                                   uint8_t parameterLength) {
    uint16_t handle = 0u;
    if (!cpu || parameterLength != 3u ||
        !MeteorBtReadVirtualHandle(parameterBuffer, parameterLength, handle)) {
        return false;
    }
    const uint8_t reason = Memory::Read8(parameterBuffer + 2u);
    const uint8_t status[] = {
        0x0F, 0x04, 0x00, 0x01, 0x06, 0x04,
    };
    const uint8_t complete[] = {
        0x05, 0x04, 0x00,
        static_cast<uint8_t>(handle & 0xFFu),
        static_cast<uint8_t>((handle >> 8) & 0x0Fu),
        reason,
    };
    QueueMeteorBtHciEvent(cpu, status, sizeof(status));
    QueueMeteorBtHciEvent(cpu, complete, sizeof(complete));
    {
        std::lock_guard<std::mutex> lock(g_btUsbMutex);
        // A physical Wii Remote remains present after a baseband disconnect. If
        // page scan is still enabled, let the next hardware pump expose a new
        // Connection Request after the queued Disconnection Complete event.
        g_btVirtualConnectionRequestQueued = false;
        g_btQueuedAclIn.clear();
        g_btInitialL2capRequestQueued = false;
        g_btGuestHidControlCid = 0u;
        g_btGuestHidInterruptCid = 0u;
        g_btControlPeerConfigQueued = false;
        g_btInterruptConnectionRequestQueued = false;
        g_btInterruptPeerConfigQueued = false;
        g_btReportMode = 0u;
        g_btReportContinuous = false;
        g_btNextInputReport = {};
    }
    RT_LOG(RT_TAG_OS) << "RDSPAF BT USB: disconnected virtual link handle=0x"
                      << std::hex << handle << " reason=0x"
                      << static_cast<uint32_t>(reason) << std::dec << std::endl;
    return true;
}

bool IsMeteorBtUsbPath(uint32_t pathPtr) {
    if (pathPtr == 0 || !Memory::Contains(pathPtr, 1u)) {
        return false;
    }
    const char* path = reinterpret_cast<const char*>(Memory::GetPointer(pathPtr));
    return path != nullptr && std::strcmp(path, "/dev/usb/oh1/57e/305") == 0;
}

uint32_t IpcPhysicalToGuest(uint32_t address) {
    if (address == 0) {
        return 0;
    }
    // Broadway hands IOS physical MEM1/MEM2 addresses. The generic IOS backend
    // consumes the cached guest aliases used everywhere else in WiiCompiled.
    if (address < 0x01800000u || (address >= 0x10000000u && address < 0x18000000u)) {
        return address + 0x80000000u;
    }
    return address;
}

int32_t DispatchIpcRequest(uint32_t request, CpuContext* cpu, bool& deferAsyncCompletion) {
    deferAsyncCompletion = false;
    const uint32_t command = Memory::Read32(request + 0x00u);
    const uint32_t fd = Memory::Read32(request + 0x08u);

    switch (command) {
    case 1: { // IOS_Open
        const uint32_t pathPtr = IpcPhysicalToGuest(Memory::Read32(request + 0x0Cu));
        if (IsMeteorBtUsbPath(pathPtr)) {
            ResetMeteorBtUsbState();
            RT_LOG(RT_TAG_OS) << "RDSPAF BT USB: opened virtual Nintendo 057e:0305 controller"
                              << std::endl;
            return kMeteorBtUsbFd;
        }
        return NAND_IOS_Open_HLE(pathPtr, Memory::Read32(request + 0x10u));
    }
    case 2: // IOS_Close
        if (static_cast<int32_t>(fd) == kMeteorBtUsbFd) {
            ResetMeteorBtUsbState();
            return 0;
        }
        return NAND_IOS_Close_HLE(fd);
    case 3: // IOS_Read
        return NAND_IOS_Read_HLE(
            fd,
            IpcPhysicalToGuest(Memory::Read32(request + 0x0Cu)),
            Memory::Read32(request + 0x10u));
    case 4: // IOS_Write
        return NAND_IOS_Write_HLE(
            fd,
            IpcPhysicalToGuest(Memory::Read32(request + 0x0Cu)),
            Memory::Read32(request + 0x10u));
    case 5: // IOS_Seek
        return NAND_IOS_Seek_HLE(
            fd,
            static_cast<int32_t>(Memory::Read32(request + 0x0Cu)),
            static_cast<int32_t>(Memory::Read32(request + 0x10u)));
    case 6: // IOS_Ioctl
        return NAND_IOS_Ioctl_HLE(
            fd,
            Memory::Read32(request + 0x0Cu),
            IpcPhysicalToGuest(Memory::Read32(request + 0x10u)),
            Memory::Read32(request + 0x14u),
            IpcPhysicalToGuest(Memory::Read32(request + 0x18u)),
            Memory::Read32(request + 0x1Cu));
    case 7: { // IOS_Ioctlv
        const uint32_t cmd = Memory::Read32(request + 0x0Cu);
        const uint32_t numIn = Memory::Read32(request + 0x10u);
        const uint32_t numOut = Memory::Read32(request + 0x14u);
        const uint32_t vectorPtr = IpcPhysicalToGuest(Memory::Read32(request + 0x18u));

        // The SDK converts each ioctlv buffer pointer to a physical IOS address
        // in-place before submission. The shared host backend expects guest
        // aliases, so translate only those pointer words for the duration of the
        // call and restore the SDK-visible physical form afterwards.
        const uint32_t vectorCount = numIn + numOut;
        std::vector<uint32_t> physicalPointers;
        physicalPointers.reserve(vectorCount);
        for (uint32_t i = 0; i < vectorCount; ++i) {
            const uint32_t pointerWord = vectorPtr + i * 8u;
            const uint32_t physical = Memory::Read32(pointerWord);
            physicalPointers.push_back(physical);
            Memory::Write32(pointerWord, IpcPhysicalToGuest(physical));
        }
        int32_t result = 0;
        if (static_cast<int32_t>(fd) == kMeteorBtUsbFd) {
            if (cmd == kUsbIoctlvInterrupt || cmd == kUsbIoctlvBulk) {
                MeteorBtUsbDataTransfer transfer{};
                if (!DecodeMeteorBtUsbDataTransfer(numIn, numOut, vectorPtr, transfer)) {
                    result = -22;
                } else if (transfer.endpoint == kBtEventInEndpoint || transfer.endpoint == kBtAclInEndpoint) {
                    MeteorBtPendingTransfer* slot = transfer.endpoint == kBtEventInEndpoint
                        ? &g_btEventIn : &g_btAclIn;
                    bool accepted = false;
                    {
                        std::lock_guard<std::mutex> lock(g_btUsbMutex);
                        if (!slot->active) {
                            *slot = {
                                true,
                                transfer.endpoint,
                                request,
                                transfer.dataBuffer,
                                transfer.requestedLength,
                                Memory::Read32(request + 0x20u),
                                Memory::Read32(request + 0x24u),
                            };
                            accepted = true;
                            if (transfer.endpoint == kBtEventInEndpoint && slot->callbackArg != 0u &&
                                Memory::Contains(slot->callbackArg, 0x10u)) {
                                g_btEventCallbackCorruptionLogged.store(false, std::memory_order_release);
                                g_btEventCallbackArgWatch.store(slot->callbackArg, std::memory_order_release);
                                if (MeteorRuntimeTraceEnabled()) {
                                    RT_LOG(RT_TAG_OS) << "RDSPAF BT USB: arm event callbackArg=0x"
                                                      << std::hex << slot->callbackArg
                                                      << " fn=0x" << Memory::Read32(slot->callbackArg + 0x00u)
                                                      << " word4=0x" << Memory::Read32(slot->callbackArg + 0x04u)
                                                      << " word8=0x" << Memory::Read32(slot->callbackArg + 0x08u)
                                                      << " wordC=0x" << Memory::Read32(slot->callbackArg + 0x0Cu)
                                                      << std::dec << std::endl;
                                }
                            }
                        }
                    }
                    if (!accepted) {
                        result = -4;
                    } else {
                        deferAsyncCompletion = true;
                        result = 0;
                        if (MeteorRuntimeTraceEnabled()) {
                            RT_LOG(RT_TAG_OS) << "RDSPAF BT USB: armed async IN ep=0x"
                                              << std::hex << static_cast<uint32_t>(transfer.endpoint)
                                              << std::dec << " len=" << transfer.requestedLength
                                              << std::endl;
                        }
                    }
                } else if (cmd == kUsbIoctlvBulk && transfer.endpoint == kBtAclOutEndpoint) {
                    result = static_cast<int32_t>(transfer.requestedLength);
                    if (MeteorRuntimeTraceEnabled()) {
                        auto& os = RT_LOG(RT_TAG_OS);
                        os << "RDSPAF BT USB: ACL OUT len=" << transfer.requestedLength << " data=";
                        const uint32_t dumpLength = std::min<uint32_t>(transfer.requestedLength, 32u);
                        for (uint32_t i = 0u; i < dumpLength; ++i) {
                            if (i != 0u) {
                                os << ' ';
                            }
                            os << std::hex << static_cast<uint32_t>(Memory::Read8(transfer.dataBuffer + i));
                        }
                        os << std::dec << std::endl;
                    }
                    HandleMeteorBtAclOutL2cap(transfer.dataBuffer, transfer.requestedLength);
                    // Read Buffer Size advertises ten ACL packets to the guest
                    // controller stack. A physical controller returns those
                    // host-to-controller credits with HCI Number Of Completed
                    // Packets (event 0x13) after transmitting each ACL packet.
                    // Without this event BTE correctly exhausts all ten credits
                    // (six L2CAP setup packets + four HID output packets) and
                    // queues the next Wii Remote command forever.
                    if (transfer.requestedLength >= 4u &&
                        Memory::Contains(transfer.dataBuffer, transfer.requestedLength)) {
                        const uint16_t aclHandle =
                            static_cast<uint16_t>(ReadUsbLittleEndian16(transfer.dataBuffer) & 0x0FFFu);
                        if (aclHandle == 0x000Bu) {
                            const uint8_t completed[] = {
                                0x13, 0x05, // Number Of Completed Packets, parameter length 5.
                                0x01,       // One connection handle follows.
                                0x0B, 0x00, // ACL handle 0x000B, little-endian.
                                0x01, 0x00, // One completed host-to-controller ACL packet.
                            };
                            QueueMeteorBtHciEvent(cpu, completed, sizeof(completed));
                            if (MeteorRuntimeTraceEnabled()) {
                                RT_LOG(RT_TAG_OS)
                                    << "RDSPAF BT USB: returned one ACL TX credit handle=0xb"
                                    << std::endl;
                            }
                        }
                    }
                } else {
                    result = -4;
                    RT_LOG(RT_TAG_OS) << "RDSPAF BT USB: unsupported data endpoint 0x"
                                      << std::hex << static_cast<uint32_t>(transfer.endpoint)
                                      << std::dec << " cmd=" << cmd << std::endl;
                }
            } else if (cmd == kUsbIoctlvControl) {
                MeteorBtUsbControlTransfer transfer{};
                if (!DecodeMeteorBtUsbControlTransfer(numIn, numOut, vectorPtr, transfer)) {
                    result = -22;
                } else {
                    RT_LOG(RT_TAG_OS) << "RDSPAF BT USB: control type=0x" << std::hex
                                      << static_cast<uint32_t>(transfer.requestType)
                                      << " req=0x" << static_cast<uint32_t>(transfer.request)
                                      << " value=0x" << transfer.value
                                      << " index=0x" << transfer.index
                                      << std::dec << " len=" << transfer.length;
                    if (transfer.requestType == 0x20 && transfer.request == 0 &&
                        transfer.length >= 3 && Memory::Contains(transfer.dataBuffer, transfer.length)) {
                        const uint16_t opcode = static_cast<uint16_t>(
                            Memory::Read8(transfer.dataBuffer) |
                            (static_cast<uint16_t>(Memory::Read8(transfer.dataBuffer + 1)) << 8));
                        const uint8_t parameterLength = Memory::Read8(transfer.dataBuffer + 2);
                        RT_LOG(RT_TAG_OS) << " HCI opcode=0x" << std::hex << opcode
                                          << std::dec << " plen="
                                          << static_cast<uint32_t>(parameterLength) << std::endl;
                        if (opcode == 0x0C1Au && parameterLength == 1u && transfer.length >= 4u) {
                            const uint8_t scanEnable = Memory::Read8(transfer.dataBuffer + 3u);
                            {
                                std::lock_guard<std::mutex> lock(g_btUsbMutex);
                                g_btScanEnable = scanEnable;
                            }
                            RT_LOG(RT_TAG_OS) << "RDSPAF BT USB: Write Scan Enable value=0x"
                                              << std::hex << static_cast<uint32_t>(scanEnable)
                                              << std::dec << std::endl;
                        }
                        bool queuedHciReply = false;
                        if (opcode == 0x0409u) {
                            queuedHciReply = QueueMeteorBtAcceptConnectionEvents(
                                cpu, transfer.dataBuffer + 3u, parameterLength);
                        } else if (opcode == 0x0419u) {
                            queuedHciReply = QueueMeteorBtRemoteNameEvents(
                                cpu, transfer.dataBuffer + 3u, parameterLength);
                        } else if (opcode == 0x041Fu) {
                            queuedHciReply = QueueMeteorBtReadClockOffsetEvents(
                                cpu, transfer.dataBuffer + 3u, parameterLength);
                        } else if (opcode == 0x041Du) {
                            queuedHciReply = QueueMeteorBtReadRemoteVersionEvents(
                                cpu, transfer.dataBuffer + 3u, parameterLength);
                        } else if (opcode == 0x041Bu) {
                            queuedHciReply = QueueMeteorBtReadRemoteFeaturesEvents(
                                cpu, transfer.dataBuffer + 3u, parameterLength);
                        } else if (opcode == 0x0411u) {
                            queuedHciReply = QueueMeteorBtAuthenticationRequestedEvents(
                                cpu, transfer.dataBuffer + 3u, parameterLength);
                        } else if (opcode == 0x040Bu || opcode == 0x040Cu) {
                            queuedHciReply = QueueMeteorBtLinkKeyReplyEvents(
                                cpu, opcode, transfer.dataBuffer + 3u, parameterLength);
                        } else if (opcode == 0x040Fu) {
                            queuedHciReply = QueueMeteorBtChangePacketTypeEvents(
                                cpu, transfer.dataBuffer + 3u, parameterLength);
                        } else if (opcode == 0x080Du || opcode == 0x0C37u) {
                            if (opcode == 0x0C37u) {
                                try {
                                    RT_LOG("meteor-bt-supervision-complete")
                                        << "pendingCallback=0x" << std::hex
                                        << (Memory::Contains(0x805B8E14u, 4u)
                                                ? Memory::Read32(0x805B8E14u)
                                                : 0u)
                                        << " state=0x"
                                        << static_cast<uint32_t>(Memory::Read8(0x805B8E46u))
                                        << " retry=0x"
                                        << static_cast<uint32_t>(Memory::Read8(0x805B8E47u))
                                        << std::dec << std::endl;
                                } catch (const Memory::AccessViolation&) {
                                    RT_LOG("meteor-bt-supervision-complete")
                                        << "guest snapshot failed" << std::endl;
                                }
                            }
                            queuedHciReply = QueueMeteorBtWriteHandleSettingComplete(
                                cpu, opcode, transfer.dataBuffer + 3u, parameterLength);
                            if (opcode == 0x0C37u && queuedHciReply) {
                                QueueMeteorBtInitialHidControlRequestIfReady();
                            }
                        } else if (opcode == 0x0406u) {
                            try {
                                auto& os = RT_LOG("meteor-bt-disconnect-source");
                                os << "lr=0x" << std::hex << cpu->lr
                                   << " sp=0x" << cpu->gpr[1]
                                   << " r3=0x" << cpu->gpr[3]
                                   << " r4=0x" << cpu->gpr[4]
                                   << " state=0x" << static_cast<uint32_t>(Memory::Read8(0x805B8E46u))
                                   << " retry=0x" << static_cast<uint32_t>(Memory::Read8(0x805B8E47u));
                                uint32_t sp = cpu->gpr[1];
                                for (uint32_t depth = 0u; depth < 10u && sp != 0u &&
                                     Memory::Contains(sp, 8u); ++depth) {
                                    const uint32_t next = Memory::Read32(sp + 0u);
                                    const uint32_t savedLr = Memory::Read32(sp + 4u);
                                    os << " frame" << std::dec << depth << "=[0x" << std::hex
                                       << sp << ",lr=0x" << savedLr << "]";
                                    if (next <= sp || next - sp > 0x100000u) {
                                        break;
                                    }
                                    sp = next;
                                }
                                os << std::dec << std::endl;
                            } catch (const Memory::AccessViolation&) {
                                RT_LOG("meteor-bt-disconnect-source") << "stack snapshot failed" << std::endl;
                            }
                            queuedHciReply = QueueMeteorBtDisconnectEvents(
                                cpu, transfer.dataBuffer + 3u, parameterLength);
                        } else {
                            queuedHciReply = QueueMeteorBtHciCommandComplete(
                                cpu, opcode, transfer.dataBuffer + 3u, parameterLength);
                        }
                        if (!queuedHciReply) {
                            RT_LOG(RT_TAG_OS) << "RDSPAF BT USB: unsupported/unverified HCI opcode=0x"
                                              << std::hex << opcode << std::dec
                                              << " plen=" << static_cast<uint32_t>(parameterLength)
                                              << std::endl;
                        }
                    } else {
                        RT_LOG(RT_TAG_OS) << std::endl;
                    }
                    result = static_cast<int32_t>(transfer.length);
                }
            } else {
                result = -4;
            }
        } else {
            result = NAND_IOS_Ioctlv_HLE(fd, cmd, numIn, numOut, vectorPtr);
        }
        for (uint32_t i = 0; i < vectorCount; ++i) {
            Memory::Write32(vectorPtr + i * 8u, physicalPointers[i]);
        }
        return result;
    }
    default:
        RT_LOG(RT_TAG_OS) << "RDSPAF IPC submit: unsupported IOS command " << command
                          << " request=0x" << std::hex << request << std::dec << std::endl;
        return -4;
    }
}

} // namespace

extern "C" void Meteor_BtUsbPump(CpuContext* cpu) {
    if (!cpu) {
        return;
    }
    QueueMeteorBtVirtualConnectionRequestIfNeeded(cpu);
    MaybeQueueMeteorBtContinuousInputReport();

    // Do not complete another virtual USB IN transfer while a previous IOS
    // completion callback is still executing. The shared callback dispatcher is
    // deliberately non-reentrant, but translated callback code can cross a
    // runtime loop checkpoint and re-enter this hardware pump. Delivering an
    // ep81 completion there queues a second D0C8 ownership instance before the
    // first one has fully unwound; once the guest heap recycles that wrapper
    // address, the late completion can free the new generation. Real IPC/USB
    // interrupt delivery is serialized at this boundary, so leave the HCI event
    // queued until the next host pump after the current callback returns.
    if (NandIosCallbackDispatchActive()) {
        static std::atomic<bool> loggedDeferredDuringCallback{false};
        if (!loggedDeferredDuringCallback.exchange(true, std::memory_order_relaxed)) {
            RT_LOG(RT_TAG_OS) << "RDSPAF BT USB: deferred IN completion while IOS callback dispatch is active"
                              << std::endl;
        }
        return;
    }

    DeliverMeteorBtEventIfPossible(cpu);
    // Event-IN completions are asynchronous IOS completions just like hardware
    // USB IRQ completions. Drain a bounded batch here so the guest callback is
    // observable even when no other synchronous IPC request happens afterward.
    (void)NandProcessPendingCallbacks(cpu, 8);
    DeliverMeteorBtAclIfPossible(cpu);
    (void)NandProcessPendingCallbacks(cpu, 8);
}

extern "C" void Meteor_BtUsbObserveGuestHeapFree(uint32_t pointer, uint32_t callerLr) {
    if (!MeteorRuntimeTraceEnabled()) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_btUsbMutex);
    if (!g_btEventIn.active || pointer == 0u || g_btEventIn.callbackArg == 0u) {
        return;
    }
    const uint32_t callbackArg = g_btEventIn.callbackArg;
    const bool exactCallbackFree = pointer == callbackArg;
    const int64_t callbackDelta = static_cast<int64_t>(callbackArg) - static_cast<int64_t>(pointer);

    // 0x8026D0C8 first releases each child pointer through 0x80252C14 and only
    // afterward releases its own completion container. A stale completion can
    // therefore destroy a newly recycled ep81 wrapper during the child loop,
    // while the later container free at LR=0x8026D1E8 is merely the last call
    // boundary seen by the generic corruption detector. Record that exact case
    // before any allocator state changes.
    if (callerLr == 0x8026D170u && callbackDelta >= -0x1000 && callbackDelta <= 0x1000) {
        static std::atomic<uint32_t> childFreeTraceCount{0u};
        const uint32_t n = childFreeTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (n < 256u || exactCallbackFree) {
            uint32_t marker = 0u;
            uint32_t size = 0u;
            uint32_t word0 = 0u;
            uint32_t word4 = 0u;
            try {
                if (pointer >= 0x10u && Memory::Contains(pointer - 0x10u, 0x10u)) {
                    uint32_t header = pointer - 0x10u;
                    marker = Memory::Read32(header);
                    if (marker == 0xBABE0002u) {
                        header = Memory::Read32(header + 0x08u);
                        marker = Memory::Contains(header, 8u) ? Memory::Read32(header) : 0u;
                    }
                    if (Memory::Contains(header, 8u)) {
                        size = Memory::Read32(header + 0x04u);
                    }
                }
                if (Memory::Contains(pointer, 8u)) {
                    word0 = Memory::Read32(pointer);
                    word4 = Memory::Read32(pointer + 4u);
                }
            } catch (const Memory::AccessViolation&) {
            }
            RT_LOG("meteor-bt-child-free")
                << "#" << std::dec << (n + 1u)
                << " ptr=0x" << std::hex << pointer
                << " activeCallback=0x" << callbackArg
                << " delta=0x" << callbackDelta
                << " exact=" << std::dec << (exactCallbackFree ? 1 : 0)
                << " marker=0x" << std::hex << marker
                << " size=0x" << size
                << " word0=0x" << word0
                << " word4=0x" << word4
                << std::dec << std::endl;
        }
    }

    const bool suspiciousCompletionFree =
        callerLr == 0x8026D1E8u && callbackDelta >= -0x1000 && callbackDelta <= 0x1000;
    if (!exactCallbackFree && !suspiciousCompletionFree) {
        return;
    }

    struct HeapBlockSnapshot {
        uint32_t probeHeader = 0u;
        uint32_t probeMagic = 0u;
        uint32_t header = 0u;
        uint32_t magic = 0u;
        uint32_t size = 0u;
        uint32_t link8 = 0u;
        uint32_t linkC = 0u;
        bool valid = false;
    };

    const auto snapshotBlock = [](uint32_t userPointer) {
        HeapBlockSnapshot snapshot{};
        if (userPointer < 0x10u || !Memory::Contains(userPointer - 0x10u, 0x10u)) {
            return snapshot;
        }
        snapshot.probeHeader = userPointer - 0x10u;
        snapshot.probeMagic = Memory::Read32(snapshot.probeHeader + 0x00u);
        snapshot.header = snapshot.probeHeader;
        // Aligned allocations carry BABE0002 immediately before the returned
        // pointer and keep the real heap-block header at probe+8.
        if (snapshot.probeMagic == 0xBABE0002u) {
            snapshot.header = Memory::Read32(snapshot.probeHeader + 0x08u);
        }
        if (!Memory::Contains(snapshot.header, 0x10u)) {
            return snapshot;
        }
        snapshot.magic = Memory::Read32(snapshot.header + 0x00u);
        snapshot.size = Memory::Read32(snapshot.header + 0x04u);
        snapshot.link8 = Memory::Read32(snapshot.header + 0x08u);
        snapshot.linkC = Memory::Read32(snapshot.header + 0x0Cu);
        snapshot.valid = snapshot.magic == 0xBABE0000u || snapshot.magic == 0xBABE0001u;
        return snapshot;
    };

    uint32_t word0 = 0u;
    uint32_t word4 = 0u;
    try {
        if (Memory::Contains(pointer, 8u)) {
            word0 = Memory::Read32(pointer + 0u);
            word4 = Memory::Read32(pointer + 4u);
        }
        const HeapBlockSnapshot freed = snapshotBlock(pointer);
        const HeapBlockSnapshot callback = snapshotBlock(callbackArg);
        const uint64_t freedPayloadBegin = static_cast<uint64_t>(freed.header) + 0x10u;
        const uint64_t freedBlockEnd = freedPayloadBegin + static_cast<uint64_t>(freed.size);
        const bool callbackInsideFreedBlock =
            freed.valid && static_cast<uint64_t>(callbackArg) >= freedPayloadBegin &&
            static_cast<uint64_t>(callbackArg) < freedBlockEnd;

        std::ostringstream physicalChain;
        uint32_t cursor = freed.header;
        for (uint32_t i = 0u; freed.valid && i < 16u && cursor != 0u; ++i) {
            if (!Memory::Contains(cursor, 8u)) {
                physicalChain << " [unmapped@0x" << std::hex << cursor << "]";
                break;
            }
            const uint32_t magic = Memory::Read32(cursor + 0x00u);
            const uint32_t size = Memory::Read32(cursor + 0x04u);
            physicalChain << " [0x" << std::hex << cursor << ":m=0x" << magic
                          << ",s=0x" << size << "]";
            if ((magic != 0xBABE0000u && magic != 0xBABE0001u) || size > 0x10000u) {
                break;
            }
            const uint64_t nextWide = static_cast<uint64_t>(cursor) + 0x10u + size;
            if (nextWide > 0xFFFFFFFFull || nextWide <= cursor) {
                break;
            }
            const uint32_t next = static_cast<uint32_t>(nextWide);
            if (next > callbackArg + 0x200u) {
                break;
            }
            cursor = next;
        }

        RT_LOG("meteor-bt-free-layout")
            << "lr=0x" << std::hex << callerLr
            << " free=0x" << pointer
            << " callbackArg=0x" << callbackArg
            << " delta=0x" << callbackDelta
            << " freeProbe=0x" << freed.probeHeader
            << " freeProbeMagic=0x" << freed.probeMagic
            << " freeHeader=0x" << freed.header
            << " freeMagic=0x" << freed.magic
            << " freeSize=0x" << freed.size
            << " freeLink8=0x" << freed.link8
            << " freeLinkC=0x" << freed.linkC
            << " cbProbe=0x" << callback.probeHeader
            << " cbProbeMagic=0x" << callback.probeMagic
            << " cbHeader=0x" << callback.header
            << " cbMagic=0x" << callback.magic
            << " cbSize=0x" << callback.size
            << " cbLink8=0x" << callback.link8
            << " cbLinkC=0x" << callback.linkC
            << " callbackInsideFree=" << std::dec << (callbackInsideFreedBlock ? 1 : 0)
            << " chain=" << physicalChain.str()
            << std::endl;
    } catch (const Memory::AccessViolation&) {
    }
    RT_LOG("meteor-bt-pending-free")
        << "lr=0x" << std::hex << callerLr
        << " ptr=0x" << pointer
        << " callbackArg=0x" << callbackArg
        << " word0=0x" << word0
        << " word4=0x" << word4
        << " request=0x" << g_btEventIn.request
        << " buffer=0x" << g_btEventIn.buffer
        << " len=0x" << g_btEventIn.length
        << std::dec << std::endl;
}

extern "C" void Meteor_BtUsbObserveCallBoundary(uint32_t target, uint32_t callerLr,
                                                  uint32_t previousTarget, uint32_t previousLr) {
    if (!MeteorRuntimeTraceEnabled()) {
        return;
    }
    const uint32_t pointer = g_btEventCallbackArgWatch.load(std::memory_order_acquire);
    if (pointer == 0u || !Memory::Contains(pointer, 8u)) {
        return;
    }

    uint32_t word0 = 0u;
    try {
        word0 = Memory::Read32(pointer);
    } catch (const Memory::AccessViolation&) {
        return;
    }
    if (word0 == 0x80270270u ||
        g_btEventCallbackCorruptionLogged.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    uint32_t word4 = 0u;
    uint32_t request = 0u;
    uint32_t buffer = 0u;
    uint32_t length = 0u;
    try {
        word4 = Memory::Read32(pointer + 4u);
        std::lock_guard<std::mutex> lock(g_btUsbMutex);
        if (g_btEventIn.active && g_btEventIn.callbackArg == pointer) {
            request = g_btEventIn.request;
            buffer = g_btEventIn.buffer;
            length = g_btEventIn.length;
        }
    } catch (const Memory::AccessViolation&) {
    }

    RT_LOG("meteor-bt-callback-corrupt")
        << "target=0x" << std::hex << target
        << " lr=0x" << callerLr
        << " previousTarget=0x" << previousTarget
        << " previousLr=0x" << previousLr
        << " ptr=0x" << pointer
        << " word0=0x" << word0
        << " word4=0x" << word4
        << " request=0x" << request
        << " buffer=0x" << buffer
        << " len=0x" << length
        << std::dec << std::endl;
}

extern "C" void Meteor_BtUsbObservePostHeapFree(uint32_t callerLr) {
    if (!MeteorRuntimeTraceEnabled()) {
        return;
    }
    const uint32_t pointer = g_btEventCallbackArgWatch.load(std::memory_order_acquire);
    if (pointer == 0u || !Memory::Contains(pointer, 8u)) {
        return;
    }
    try {
        const uint32_t word0 = Memory::Read32(pointer);
        const uint32_t word4 = Memory::Read32(pointer + 4u);
        if (callerLr == 0x8026D1E8u || word0 != 0x80270270u) {
            RT_LOG("meteor-bt-post-free")
                << "lr=0x" << std::hex << callerLr
                << " ptr=0x" << pointer
                << " word0=0x" << word0
                << " word4=0x" << word4
                << std::dec << std::endl;
        }
    } catch (const Memory::AccessViolation&) {
    }
}

extern "C" void Meteor_BtUsbObserveMemset(uint32_t destination, uint32_t value,
                                            uint32_t length, uint32_t callerLr) {
    if (!MeteorRuntimeTraceEnabled()) {
        return;
    }
    const uint32_t pointer = g_btEventCallbackArgWatch.load(std::memory_order_acquire);
    if (length == 0u) {
        return;
    }

    // Narrow lifetime proof for the IPC arena. A BT callback can be injected from
    // a translated memset loop after the caller has already captured its
    // destination pointer. Log those small arena clears even when they overlap a
    // *future* ep81 generation rather than the currently armed callbackArg.
    const uint64_t arenaBegin = 0x97FC1E00ull;
    const uint64_t arenaEnd = 0x97FC2400ull;
    const uint64_t rawWriteBegin = destination;
    const uint64_t rawWriteEnd = rawWriteBegin + static_cast<uint64_t>(length);
    if (rawWriteEnd > arenaBegin && rawWriteBegin < arenaEnd) {
        RT_LOG("meteor-bt-arena-memset")
            << "lr=0x" << std::hex << callerLr
            << " dst=0x" << destination
            << " value=0x" << (value & 0xFFu)
            << " len=0x" << length
            << " activeCallback=0x" << pointer
            << std::dec << std::endl;
    }

    if (pointer == 0u) {
        return;
    }

    const uint64_t writeBegin = destination;
    const uint64_t writeEnd = writeBegin + static_cast<uint64_t>(length);
    // The callback container is 0x80 bytes in the guest builder and may be
    // returned through an alignment shim. Watching the first 0x80 bytes is
    // enough to catch the function pointer/user argument corruption without
    // producing noise from unrelated memset traffic.
    const uint64_t callbackBegin = pointer;
    const uint64_t callbackEnd = callbackBegin + 0x80u;
    if (writeEnd <= callbackBegin || writeBegin >= callbackEnd) {
        return;
    }

    uint32_t word0 = 0u;
    uint32_t word4 = 0u;
    try {
        if (Memory::Contains(pointer, 8u)) {
            word0 = Memory::Read32(pointer);
            word4 = Memory::Read32(pointer + 4u);
        }
    } catch (const Memory::AccessViolation&) {
    }

    RT_LOG("meteor-bt-memset-overlap")
        << "lr=0x" << std::hex << callerLr
        << " dst=0x" << destination
        << " value=0x" << (value & 0xFFu)
        << " len=0x" << length
        << " activeCallback=0x" << pointer
        << " word0Before=0x" << word0
        << " word4Before=0x" << word4
        << std::dec << std::endl;
}

namespace {
struct MeteorBtAllocWatchState {
    uint32_t heapIndex = 0u;
    uint32_t size = 0u;
    uint32_t alignment = 0u;
    uint32_t callerLr = 0u;
    uint32_t callbackHeader = 0u;
    bool callbackInFreeList = false;
};

thread_local MeteorBtAllocWatchState g_btAllocWatch{};

uint32_t ResolveMeteorHeapHeader(uint32_t userPointer) {
    if (userPointer < 0x10u || !Memory::Contains(userPointer - 0x10u, 0x10u)) {
        return 0u;
    }
    uint32_t header = userPointer - 0x10u;
    const uint32_t marker = Memory::Read32(header);
    if (marker == 0xBABE0002u) {
        header = Memory::Read32(header + 0x08u);
    }
    return Memory::Contains(header, 0x10u) ? header : 0u;
}

bool MeteorHeapFreeListContains(uint32_t heapIndex, uint32_t header) {
    if (header == 0u || heapIndex >= 8u) {
        return false;
    }
    constexpr uint32_t kHeapMetaBase = 0x8057D1A8u;
    const uint32_t entry = kHeapMetaBase + (heapIndex << 4u);
    if (!Memory::Contains(entry + 0x0Cu, 4u)) {
        return false;
    }
    uint32_t cursor = Memory::Read32(entry + 0x0Cu);
    for (uint32_t i = 0u; cursor != 0u && i < 4096u; ++i) {
        if (cursor == header) {
            return true;
        }
        if (!Memory::Contains(cursor + 0x0Cu, 4u)) {
            break;
        }
        const uint32_t next = Memory::Read32(cursor + 0x0Cu);
        if (next == cursor) {
            break;
        }
        cursor = next;
    }
    return false;
}
} // namespace

extern "C" void Meteor_BtUsbObserveHeapAllocCall(uint32_t heapIndex, uint32_t size,
                                                   uint32_t alignment, uint32_t callerLr) {
    if (!MeteorRuntimeTraceEnabled()) {
        return;
    }
    g_btAllocWatch = {};
    const uint32_t callbackArg = g_btEventCallbackArgWatch.load(std::memory_order_acquire);
    if (callbackArg == 0u) {
        return;
    }
    try {
        const uint32_t callbackHeader = ResolveMeteorHeapHeader(callbackArg);
        const bool inFreeList = MeteorHeapFreeListContains(heapIndex, callbackHeader);
        g_btAllocWatch.heapIndex = heapIndex;
        g_btAllocWatch.size = size;
        g_btAllocWatch.alignment = alignment;
        g_btAllocWatch.callerLr = callerLr;
        g_btAllocWatch.callbackHeader = callbackHeader;
        g_btAllocWatch.callbackInFreeList = inFreeList;
        if (inFreeList) {
            const uint32_t magic = callbackHeader ? Memory::Read32(callbackHeader) : 0u;
            const uint32_t blockSize = callbackHeader ? Memory::Read32(callbackHeader + 4u) : 0u;
            RT_LOG("meteor-bt-free-list-stale")
                << "phase=pre-alloc lr=0x" << std::hex << callerLr
                << " heap=0x" << heapIndex
                << " reqSize=0x" << size
                << " align=0x" << alignment
                << " callbackArg=0x" << callbackArg
                << " callbackHeader=0x" << callbackHeader
                << " magic=0x" << magic
                << " blockSize=0x" << blockSize
                << std::dec << std::endl;
        }
    } catch (const Memory::AccessViolation&) {
        g_btAllocWatch = {};
    }
}

extern "C" void Meteor_BtUsbObserveHeapAllocReturn(uint32_t result, uint32_t callerLr) {
    if (!MeteorRuntimeTraceEnabled()) {
        return;
    }
    const MeteorBtAllocWatchState watch = g_btAllocWatch;
    g_btAllocWatch = {};
    const uint32_t callbackArg = g_btEventCallbackArgWatch.load(std::memory_order_acquire);
    if (callbackArg == 0u || watch.callbackHeader == 0u || result == 0u) {
        return;
    }
    try {
        const uint32_t resultHeader = ResolveMeteorHeapHeader(result);
        const bool sameHeader = resultHeader == watch.callbackHeader;
        const bool overlapsCallback =
            result <= callbackArg && static_cast<uint64_t>(result) + watch.size > callbackArg;
        if (watch.callbackInFreeList || sameHeader || overlapsCallback) {
            RT_LOG("meteor-bt-alloc-overlap")
                << "lr=0x" << std::hex << callerLr
                << " originalLr=0x" << watch.callerLr
                << " heap=0x" << watch.heapIndex
                << " reqSize=0x" << watch.size
                << " align=0x" << watch.alignment
                << " result=0x" << result
                << " resultHeader=0x" << resultHeader
                << " callbackArg=0x" << callbackArg
                << " callbackHeader=0x" << watch.callbackHeader
                << " preInFreeList=" << std::dec << (watch.callbackInFreeList ? 1 : 0)
                << " sameHeader=" << (sameHeader ? 1 : 0)
                << " overlaps=" << (overlapsCallback ? 1 : 0)
                << std::endl;
        }
    } catch (const Memory::AccessViolation&) {
    }
}

extern "C" void meteor_IPCCltInit(CpuContext* cpu) {
    if (!cpu) {
        return;
    }

    // RDSPAF 0x80251700, validated with Ghidra headless.
    // The original routine:
    //   1) initializes the IPC arena globals,
    //   2) creates an 0x800-byte IOS heap,
    //   3) advances the IPC arena low pointer,
    //   4) installs IRQ 27 / unmasks the hardware source,
    //   5) writes Hollywood IPC control at 0xCD000004,
    //   6) clears the SDK's RAM-side IPC request bookkeeping.
    // Keep the RAM-visible SDK work and replace only the physical IRQ/MMIO setup.
    const uint32_t r13 = cpu->gpr[13];
    const uint32_t initFlagAddr = r13 - kIpcInitFlagOffset;

    if (Memory::Read32(initFlagAddr) != 0) {
        cpu->gpr[3] = 0;
        return;
    }
    Memory::Write32(initFlagAddr, 1u);

    // 0x80251240 initializes the SDK IPC buffer lo/hi globals from the OS IPC arena.
    InvokeIndirectCpu(0x80251240u, cpu);

    // 0x802512B4 = get IPC buffer low; 0x802512AC = get IPC buffer high.
    InvokeIndirectCpu(0x802512B4u, cpu);
    const uint32_t bufferLo = cpu->gpr[3];
    const uint64_t newBufferLoWide = static_cast<uint64_t>(bufferLo) + kIpcHeapSize;

    InvokeIndirectCpu(0x802512ACu, cpu);
    const uint32_t bufferHi = cpu->gpr[3];
    if (bufferLo == 0 || newBufferLoWide > bufferHi) {
        RT_LOG(RT_TAG_OS) << "RDSPAF IPCCltInit: insufficient IPC arena lo=0x"
                          << std::hex << bufferLo << " hi=0x" << bufferHi
                          << std::dec << std::endl;
        cpu->gpr[3] = static_cast<uint32_t>(-22);
        return;
    }

    // Preserve the title's real guest heap creation.
    cpu->gpr[3] = bufferLo;
    cpu->gpr[4] = kIpcHeapSize;
    InvokeIndirectCpu(0x802528DCu, cpu);
    Memory::Write32(r13 - kIpcHeapHandleOffset, cpu->gpr[3]);

    // 0x802512BC stores the new IPC buffer low pointer.
    cpu->gpr[3] = static_cast<uint32_t>(newBufferLoWide);
    InvokeIndirectCpu(0x802512BCu, cpu);

    // 0x80252E00 is RAM-only request bookkeeping and is part of IPC init state.
    InvokeIndirectCpu(0x80252E00u, cpu);

    RT_LOG(RT_TAG_OS) << "RDSPAF IPCCltInit: initialized guest IPC arena 0x"
                      << std::hex << bufferLo << "-0x" << bufferHi
                      << "; skipped IRQ 27 / Hollywood IPC MMIO"
                      << std::dec << std::endl;
    cpu->gpr[3] = 0;
}

REGISTER_TITLE_NATIVE_FUNCTION(0x80251700, meteor_IPCCltInit);

extern "C" void meteor_IPCSubmit(CpuContext* cpu) {
    static bool dumpedDvdServiceTable = false;
    if (!cpu) {
        return;
    }

    // RDSPAF 0x802517B4 is the common SDK submission point for IOS commands.
    // Ghidra confirms that all command builders (1=open ... 7=ioctlv) funnel
    // through here before the routine writes the request's physical pointer to
    // Hollywood IPC register 0xCD000000. Complete the request against the
    // generic host IOS backend instead of touching Broadway-IOS MMIO.
    const uint32_t request = cpu->gpr[3];
    const uint32_t asyncCallback = cpu->gpr[4];
    if (request == 0) {
        cpu->gpr[3] = static_cast<uint32_t>(-4);
        return;
    }

    const uint32_t command = Memory::Read32(request + 0x00u);
    const uint32_t fd = Memory::Read32(request + 0x08u);
    const uint32_t callback = Memory::Read32(request + 0x20u);
    const uint32_t callbackArg = Memory::Read32(request + 0x24u);
    const bool traceRequest = MeteorRuntimeTraceEnabled() &&
                              (asyncCallback != 0 || command == 1u || fd == 7u);
    if (traceRequest) {
        std::ostringstream trace;
        trace << "RDSPAF IPC trace: submit request=0x" << std::hex << request
              << " command=0x" << command
              << " fd=0x" << fd
              << " asyncArg=0x" << asyncCallback
              << " callback=0x" << callback
              << " callbackArg=0x" << callbackArg;
        if (command == 6u) {
            trace << " ioctl=0x" << Memory::Read32(request + 0x0Cu)
                  << " in=0x" << Memory::Read32(request + 0x10u)
                  << "/0x" << Memory::Read32(request + 0x14u)
                  << " out=0x" << Memory::Read32(request + 0x18u)
                  << "/0x" << Memory::Read32(request + 0x1Cu);
        } else if (command == 7u) {
            trace << " ioctlv=0x" << Memory::Read32(request + 0x0Cu)
                  << " inCount=0x" << Memory::Read32(request + 0x10u)
                  << " outCount=0x" << Memory::Read32(request + 0x14u)
                  << " vectors=0x" << Memory::Read32(request + 0x18u);
        } else if (command == 1u) {
            trace << " path=0x" << Memory::Read32(request + 0x0Cu)
                  << " mode=0x" << Memory::Read32(request + 0x10u);
        }
        RT_LOG(RT_TAG_OS) << trace.str() << std::endl;
    }

    bool deferAsyncCompletion = false;
    const int32_t result = DispatchIpcRequest(request, cpu, deferAsyncCompletion);
    Memory::Write32(request + 0x04u, static_cast<uint32_t>(result));

    if (!dumpedDvdServiceTable && fd == 7u && command == 6u &&
        Memory::Read32(request + 0x0Cu) == 0x71u) {
        dumpedDvdServiceTable = true;
        constexpr uint32_t kServiceSlots = 0x805C7240u;
        for (uint32_t i = 0; i < 32u; ++i) {
            const uint32_t slot = kServiceSlots + i * 0x10u;
            const uint32_t iface = Memory::Contains(slot, 4u) ? Memory::Read32(slot) : 0u;
            uint32_t fn0 = 0u;
            uint32_t arg4 = 0u;
            if (iface != 0u && Memory::Contains(iface, 8u)) {
                fn0 = Memory::Read32(iface + 0x00u);
                arg4 = Memory::Read32(iface + 0x04u);
            }
            if (iface != 0u || fn0 != 0u) {
                RT_LOG(RT_TAG_OS) << "RDSPAF service-table: index=" << std::dec << i
                                  << " iface=0x" << std::hex << iface
                                  << " fn0=0x" << fn0
                                  << " arg4=0x" << arg4
                                  << std::dec << std::endl;
            }
        }
    }

    if (traceRequest) {
        RT_LOG(RT_TAG_OS) << "RDSPAF IPC trace: complete request=0x" << std::hex << request
                          << " command=0x" << command
                          << " fd=0x" << fd
                          << " result=0x" << static_cast<uint32_t>(result)
                          << " callback=0x" << callback
                          << " callbackArg=0x" << callbackArg
                          << std::dec << std::endl;
    }

    if (asyncCallback != 0) {
        if (deferAsyncCompletion) {
            // IOS accepted a long-lived async USB IN URB. Its callback and IPC
            // request lifetime remain pending until the virtual controller has
            // actual event/ACL bytes to return.
            cpu->gpr[3] = 0;
            return;
        }
        const uint32_t dvdExecutingGlobal = cpu->gpr[13] - 0x6458u;
        const uint32_t dvdExecutingBefore = Memory::Contains(dvdExecutingGlobal, 4u)
            ? Memory::Read32(dvdExecutingGlobal)
            : 0u;
        if (traceRequest && callbackArg != 0 && Memory::Contains(callbackArg, 0x10u)) {
            RT_LOG(RT_TAG_OS) << "RDSPAF IPC trace: callback-arg-before arg=0x" << std::hex
                              << callbackArg
                              << " fn=0x" << Memory::Read32(callbackArg + 0x00u)
                              << " word4=0x" << Memory::Read32(callbackArg + 0x04u)
                              << " word8=0x" << Memory::Read32(callbackArg + 0x08u)
                              << " wordC=0x" << Memory::Read32(callbackArg + 0x0Cu)
                              << std::dec << std::endl;
        }
        if (traceRequest && fd == 7u && dvdExecutingBefore != 0 &&
            Memory::Contains(dvdExecutingBefore, 0x2Cu)) {
            RT_LOG(RT_TAG_OS) << "RDSPAF IPC trace: dvd-exec-before ptr=0x" << std::hex
                              << dvdExecutingBefore
                              << " state=0x" << Memory::Read32(dvdExecutingBefore + 0x0Cu)
                              << " length=0x" << Memory::Read32(dvdExecutingBefore + 0x14u)
                              << " transferred=0x" << Memory::Read32(dvdExecutingBefore + 0x20u)
                              << " callback=0x" << Memory::Read32(dvdExecutingBefore + 0x28u)
                              << std::dec << std::endl;
        }
        NandQueueIosCallback(callback, result, callbackArg);

        // The original IPC completion path releases async request blocks after
        // harvesting callback state. Mirror that guest-heap lifetime now that
        // no hardware interrupt will run the original completion handler.
        cpu->gpr[3] = Memory::Read32(cpu->gpr[13] - kIpcHeapHandleOffset);
        cpu->gpr[4] = request;
        InvokeIndirectCpu(0x80252C14u, cpu);
        cpu->gpr[3] = 0;
        // SC/SYSCONF starts async IOS work before the normal alarm/retrace pump
        // exists. Host IOS work has already completed synchronously, so drain a
        // bounded batch now; nand_async.cpp dispatches callbacks on scratch CPU
        // contexts and guards against nested drains.
        const bool drained = NandProcessPendingCallbacks(cpu, 64);

        // Do not materialize the matching HCI interrupt event until the USB
        // control-OUT completion callback has actually executed. Queue order by
        // itself is insufficient here: when this submit happens from inside an
        // IOS callback, the nested drain is intentionally suppressed and the
        // old code could write the next ep81 payload into a guest wrapper before
        // retail BTE had finished the control completion. The regular translated
        // loop checkpoint pumps BT immediately afterward and then drains the HCI
        // interrupt completion, matching the hardware-visible OUT -> IN order.
        if (traceRequest) {
            RT_LOG(RT_TAG_OS) << "RDSPAF IPC trace: callback-drain target=0x" << std::hex
                              << callback << " result=0x" << static_cast<uint32_t>(result)
                              << " arg=0x" << callbackArg
                              << " drained=" << std::dec << (drained ? 1 : 0) << std::endl;
            if (callbackArg != 0 && Memory::Contains(callbackArg, 0x10u)) {
                RT_LOG(RT_TAG_OS) << "RDSPAF IPC trace: callback-arg-after arg=0x" << std::hex
                                  << callbackArg
                                  << " fn=0x" << Memory::Read32(callbackArg + 0x00u)
                                  << " word4=0x" << Memory::Read32(callbackArg + 0x04u)
                                  << " word8=0x" << Memory::Read32(callbackArg + 0x08u)
                                  << " wordC=0x" << Memory::Read32(callbackArg + 0x0Cu)
                                  << std::dec << std::endl;
            }
            const uint32_t dvdExecutingAfter = Memory::Contains(dvdExecutingGlobal, 4u)
                ? Memory::Read32(dvdExecutingGlobal)
                : 0u;
            RT_LOG(RT_TAG_OS) << "RDSPAF IPC trace: dvd-exec-after global=0x" << std::hex
                              << dvdExecutingAfter;
            if (dvdExecutingBefore != 0 && Memory::Contains(dvdExecutingBefore, 0x2Cu)) {
                RT_LOG(RT_TAG_OS) << " previous=0x" << dvdExecutingBefore
                                  << " previousState=0x" << Memory::Read32(dvdExecutingBefore + 0x0Cu)
                                  << " previousTransferred=0x"
                                  << Memory::Read32(dvdExecutingBefore + 0x20u);
            }
            RT_LOG(RT_TAG_OS) << std::dec << std::endl;
        }
        return;
    }

    cpu->gpr[3] = static_cast<uint32_t>(result);
}

REGISTER_TITLE_NATIVE_FUNCTION(0x802517B4, meteor_IPCSubmit);
