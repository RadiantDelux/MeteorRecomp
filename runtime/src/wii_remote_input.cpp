#include "wii_remote_input.h"

#include "memory.h"
#include "runtime_config.h"
#include "ps2_controller_contract.h"
#include "RuntimeConfig.h"
#include "runtime_log.h"

#include <dolphin/pad.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_video.h>
#include <SDL3/SDL_sensor.h>
#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <cstdlib>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace WiiRemoteInput {
namespace {

// Dolphin's continuous scanning polls Bluetooth about once a second; SDL's
// enumeration walks every HID device on the main thread, so stay a bit lazier.
constexpr uint64_t kScanIntervalMs = 2000;
// Right after a remote drops it is almost certainly still there, but the first
// re-open attempts tend to fail on timed-out reads, so retry quickly for a while.
constexpr uint64_t kFastScanIntervalMs = 500;
constexpr uint64_t kFastScanWindowMs = 15000;
// How long the Wii driver hint stays at "0" during a rescan; SDL applies hint
// changes on its next joystick update, once per pumped frame.
constexpr uint64_t kRescanDriverOffMs = 100;

constexpr float kStandardGravity = 9.80665f;

// SDL's Wii driver posts the remote's own buttons as raw joystick buttons
// starting at SDL_GAMEPAD_BUTTON_MISC1, in this order (SDL_hidapi_wii.c,
// EWiiButtons), whatever the extension.
enum RawWiiButton : int {
    kRawA = SDL_GAMEPAD_BUTTON_MISC1,
    kRawB,
    kRawOne,
    kRawTwo,
    kRawPlus,
    kRawMinus,
    kRawHome,
    kRawDpadUp,
    kRawDpadDown,
    kRawDpadLeft,
    kRawDpadRight,
};

// WPAD_BUTTON_* bits as the game reads them from KPADStatus.hold.
constexpr uint32_t kWpadLeft = 0x0001, kWpadRight = 0x0002, kWpadDown = 0x0004, kWpadUp = 0x0008,
                   kWpadPlus = 0x0010, kWpadTwo = 0x0100, kWpadOne = 0x0200, kWpadB = 0x0400, kWpadA = 0x0800,
                   kWpadMinus = 0x1000, kWpadZ = 0x2000, kWpadC = 0x4000, kWpadHome = 0x8000;

// WPAD_CL_BUTTON_* bits (WPADCLStatus.clButton / KPADStatus.ex_status.cl.hold):
// the Classic Controller's two button bytes, inverted, high byte first.
constexpr uint32_t kClUp = 0x0001, kClLeft = 0x0002, kClZR = 0x0004, kClX = 0x0008, kClA = 0x0010, kClY = 0x0020,
                   kClB = 0x0040, kClZL = 0x0080, kClR = 0x0200, kClPlus = 0x0400, kClHome = 0x0800,
                   kClMinus = 0x1000, kClL = 0x2000, kClDown = 0x4000, kClRight = 0x8000;

// How long a vanished remote keeps its channel alive with neutral input. SDL's
// in-place reconnect after an extension change takes well under a second; a
// remote that is really gone shows up as disconnected after this.
constexpr uint64_t kExtensionSwapGraceMs = 3000;
// Rescanning closes and re-opens the Bluetooth HID handle, which some Windows
// stacks answer by dropping the link; leave SDL's own reconnect this long first.
constexpr uint64_t kScanStartDelayMs = 3000;

// Per-port memory of the last Wii controller seen there, for EffectiveKind.
struct PortMemory {
    Kind lastKind = Kind::NotWii;
    uint64_t lastSeenMs = 0;
};
std::array<PortMemory, PAD_MAX_CONTROLLERS> g_ports{};

bool g_wiiDriverEnabled = false;
uint64_t g_lastScanMs = 0;
uint32_t g_scanCount = 0;
bool g_scanning = false;
// Non-zero while a rescan has the Wii driver hint switched off (see RescanNow).
uint64_t g_driverOffSinceMs = 0;
// When the current scan started (last Wii controller seen).
uint64_t g_lostAtMs = 0;

// Instance ids whose accelerometers have been switched on. SDL keeps sensors
// off until asked and forgets that when the gamepad is closed, so a re-paired
// remote gets a fresh id and is enabled again.
std::array<SDL_JoystickID, PAD_MAX_CONTROLLERS> g_sensorsEnabledFor{};

// Zero-point correction subtracted from the remote's accelerometer, in g and in
// SDL's sensor frame; loaded from Config.toml on first use, replaced by a
// calibration run. The Nunchuk accelerometer is left uncorrected.
std::array<float, 3> g_accelOffset{};
bool g_accelOffsetLoaded = false;

// Frames sampled by a calibration run (about 1.5 s at 60 Hz) and how far a
// sample may stray from the first one before the run is declared "moved".
constexpr int kCalibrationSamples = 90;
constexpr float kCalibrationMaxDeviationG = 0.15f;
// |mean| outside this range means the remote was not at rest or the
// accelerometer is far off its nominal scale; either way the offset is useless.
constexpr float kCalibrationMinGravityG = 0.8f;
constexpr float kCalibrationMaxGravityG = 1.2f;

struct AccelCalibration {
    bool active = false;
    uint32_t chan = 0;
    int count = 0;
    double sum[3] = {};
    float first[3] = {};
};
AccelCalibration g_calibration;
char g_calibrationMessage[160] = {};

// Last accepted KPAD acc per port, for the remote and for the Nunchuk. Reused on
// frames that bring no sample or a glitched one, so the game never sees a jump.
struct LastAcc {
    bool valid = false;
    float acc[3] = {0.0f, -1.0f, 0.0f};
};
std::array<LastAcc, PAD_MAX_CONTROLLERS> g_lastAcc{};
std::array<LastAcc, PAD_MAX_CONTROLLERS> g_lastNunchukAcc{};

using Ps2SemanticContext = Ps2Controller::Context;
std::array<Ps2Controller::ContextGate, PAD_MAX_CONTROLLERS> g_ps2ContextGates{};

std::array<uint32_t, PAD_MAX_CONTROLLERS> g_ps2Fighters{};
std::array<uint16_t, PAD_MAX_CONTROLLERS> g_ps2ExtraButtons{};
bool g_ps2Enabled = false;
thread_local uint32_t g_ps2RecoveryPromptDrawScopeDepth = 0;

struct Ps2PromptRecordSource {
    uint32_t record = 0;
    uint32_t sourceReturnAddress = 0;
};
constexpr size_t kPs2PromptSourceHistory = 512;
std::array<Ps2PromptRecordSource, kPs2PromptSourceHistory> g_ps2PromptRecordSources{};
size_t g_ps2PromptRecordSourceCursor = 0;

uint32_t Ps2PromptSourceForRecord(uint32_t record) {
    if (!record) return 0;
    for (size_t i = 0; i < g_ps2PromptRecordSources.size(); ++i) {
        const size_t index = (g_ps2PromptRecordSourceCursor + g_ps2PromptRecordSources.size() - 1u - i) %
                             g_ps2PromptRecordSources.size();
        const auto& entry = g_ps2PromptRecordSources[index];
        if (entry.record == record) return entry.sourceReturnAddress;
    }
    return 0;
}

struct Ps2PromptRowBinding {
    uint32_t metadataPointerField = 0;
    uint32_t originalMetadata = 0;

    bool valid() const {
        return metadataPointerField != 0 && originalMetadata != 0;
    }
};

struct Ps2PromptBindings {
    bool initialized = false;
    bool valid = false;
    Ps2PromptRowBinding gcAStatic;
    Ps2PromptRowBinding gcBStatic;
    Ps2PromptRowBinding gcXStatic;
    Ps2PromptRowBinding gcYStatic;
    Ps2PromptRowBinding gcAAnim;
    Ps2PromptRowBinding gcBAnim;
    Ps2PromptRowBinding gcXAnim;
    Ps2PromptRowBinding gcYAnim;
    Ps2PromptRowBinding gcStart;
    Ps2PromptRowBinding gcCUp;
    Ps2PromptRowBinding gcL;
    Ps2PromptRowBinding gcCRight;
    Ps2PromptRowBinding gcZ;
    Ps2PromptRowBinding gcR;
    Ps2PromptRowBinding gcCDown;
    Ps2PromptRowBinding gcAPress;
    Ps2PromptRowBinding gcYPress;
};

Ps2PromptBindings g_ps2PromptBindings{};
bool g_ps2PromptProfileApplied = false;
Ps2SemanticContext g_ps2PromptProfileContext = Ps2SemanticContext::Frontend;
uint32_t g_ps2BattlePromptScopeDepth = 0;
Ps2SemanticContext g_ps2BattlePromptRestoreContext = Ps2SemanticContext::Frontend;

// Any axis beyond this is not a reading the remote's +-3 g sensor can produce.
constexpr float kMaxPlausibleRemoteG = 4.0f;

// Case-sensitive substring test that tolerates a null name.
bool NameContains(const char* name, const char* needle) {
    return name != nullptr && std::strstr(name, needle) != nullptr;
}

// Turns on the remote (and Nunchuk) accelerometers once per gamepad instance.
void EnsureSensors(SDL_Gamepad* gamepad, uint32_t port) {
    const SDL_JoystickID id = SDL_GetGamepadID(gamepad);
    if (g_sensorsEnabledFor[port] == id) {
        return;
    }
    bool allEnabled = true;
    for (SDL_SensorType sensor : {SDL_SENSOR_ACCEL, SDL_SENSOR_ACCEL_L}) {
        if (SDL_GamepadHasSensor(gamepad, sensor) && !SDL_SetGamepadSensorEnabled(gamepad, sensor, true)) {
            RT_LOG(RT_TAG_CONFIG) << "Wii Remote on port " << (port + 1)
                                  << ": could not enable an accelerometer: " << SDL_GetError() << std::endl;
            allEnabled = false;
        }
    }
    // Only remember the instance once every sensor is on, so a failed attempt
    // is retried on the next sample instead of leaving the accelerometer off.
    if (allEnabled) {
        g_sensorsEnabledFor[port] = id;
    }
}

// Loads the stored zero-point correction once.
const std::array<float, 3>& AccelOffset() {
    if (!g_accelOffsetLoaded) {
        const std::array<double, 3> stored = RuntimeConfigFile::WiiAccelOffset();
        for (size_t i = 0; i < 3; ++i) g_accelOffset[i] = static_cast<float>(stored[i]);
        g_accelOffsetLoaded = true;
    }
    return g_accelOffset;
}

// Raw SDL sample in m/s^2, rejecting anything SDL has not delivered yet.
bool ReadSdlAccel(SDL_Gamepad* gamepad, SDL_SensorType sensor, float* sdl) {
    return SDL_GamepadSensorEnabled(gamepad, sensor) && SDL_GetGamepadSensorData(gamepad, sensor, sdl, 3) &&
           std::isfinite(sdl[0]) && std::isfinite(sdl[1]) && std::isfinite(sdl[2]);
}

// True for a sample that cannot have come from the sensor. Over Bluetooth on
// Windows the remote delivers, a few times a minute, a report whose
// accelerometer bytes are all zero; SDL decodes that as -0x200 on every axis,
// i.e. (+5.12, -5.12, -5.12) g for the remote (100 units/g) and
// (+2.56, -2.56, -2.56) g for the Nunchuk (200 units/g). Handed to the game as
// is, one such frame is a full-lock steer plus a 9 g "shake". `g` is the
// uncorrected SDL sample.
bool IsGlitchedSample(SDL_SensorType sensor, const float* g) {
    // An exact zero vector is SDL's sensor buffer before the first report, not
    // a reading (the remote never delivers 0 g on all three axes at once).
    if (g[0] == 0.0f && g[1] == 0.0f && g[2] == 0.0f) {
        return true;
    }
    const float zero = sensor == SDL_SENSOR_ACCEL ? 5.12f : 2.56f;
    if (std::fabs(g[0] - zero) < 0.03f && std::fabs(g[1] + zero) < 0.03f && std::fabs(g[2] + zero) < 0.03f) {
        return true;
    }
    if (sensor == SDL_SENSOR_ACCEL) {
        for (int i = 0; i < 3; ++i) {
            if (std::fabs(g[i]) > kMaxPlausibleRemoteG) return true;
        }
    }
    return false;
}

// One accelerometer sample in g, SDL's sensor frame. The remote's own axes are
// +x left, +y towards the user, +z out of the button face (wiibrew, Dolphin);
// SDL_hidapi_wii.c posts (-wiiX, wiiZ, wiiY): x right across the face, y out of
// the face (+1 at rest, buttons up), z towards the user, i.e. away from the tip.
// The remote's sample gets the zero-point correction; the Nunchuk's does not.
// False when there is no sample yet or the sample is a glitch (see above).
bool ReadAccelG(SDL_Gamepad* gamepad, SDL_SensorType sensor, float* g) {
    float sdl[3] = {};
    if (!ReadSdlAccel(gamepad, sensor, sdl)) {
        return false;
    }
    for (int i = 0; i < 3; ++i) g[i] = sdl[i] / kStandardGravity;
    if (IsGlitchedSample(sensor, g)) {
        return false;
    }
    if (sensor == SDL_SENSOR_ACCEL) {
        const std::array<float, 3>& offset = AccelOffset();
        for (int i = 0; i < 3; ++i) g[i] -= offset[i];
    }
    return true;
}

// KPAD's acc is the remote reading as (-wiiX, -wiiZ, wiiY): x right across the
// face, y through the back of the remote (rest: -1 with the buttons up), z
// towards the user. That is SDL's frame with y negated. Held sideways as a
// wheel the rest vector is (1, 0, 0) and a turn shows up as z = sin(angle), so
// the sign of z is the direction of the turn; Cemu does the same conversion for
// real remotes on the Wii U.
void AccelGToKpad(const float* g, float* kpad) {
    kpad[0] = g[0];
    kpad[1] = -g[1];
    kpad[2] = g[2];
}

// Sensor -> KPAD acc for ReadKpadSample.
bool ReadAccelAsKpad(SDL_Gamepad* gamepad, SDL_SensorType sensor, float* kpad) {
    float g[3] = {};
    if (!ReadAccelG(gamepad, sensor, g)) {
        return false;
    }
    AccelGToKpad(g, kpad);
    return true;
}

// Debug trace of every remote sample (controller.wii_accel_trace = true):
// milliseconds, port, WPAD hold bits, the uncorrected SDL sample in g and the
// KPAD acc handed to the game. One CSV per run, truncated at startup.
void TraceSample(uint32_t chan, const KpadSample& sample, const float* rawG, bool haveRaw, bool accepted) {
    static std::ofstream trace;
    static bool opened = false;
    if (!opened) {
        opened = true;
        const std::filesystem::path path = RuntimeConfigFile::ApplicationDataDirectory() / "wii_accel_trace.csv";
        trace.open(path, std::ios::trunc);
        if (trace) {
            trace << "ms,port,hold,raw_x,raw_y,raw_z,ok,kpad_x,kpad_y,kpad_z\n";
            RT_LOG(RT_TAG_CONFIG) << "Wii Remote accelerometer trace: " << path.string() << std::endl;
        } else {
            RT_LOG(RT_TAG_CONFIG) << "Wii Remote accelerometer trace: could not open " << path.string() << std::endl;
        }
    }
    if (!trace) {
        return;
    }
    char line[192];
    if (haveRaw) {
        std::snprintf(line, sizeof(line), "%llu,%u,%04x,%.4f,%.4f,%.4f,%d,%.4f,%.4f,%.4f\n",
                      static_cast<unsigned long long>(SDL_GetTicks()), chan + 1, sample.hold, rawG[0], rawG[1],
                      rawG[2], accepted ? 1 : 0, sample.acc[0], sample.acc[1], sample.acc[2]);
    } else {
        std::snprintf(line, sizeof(line), "%llu,%u,%04x,,,,0,%.4f,%.4f,%.4f\n",
                      static_cast<unsigned long long>(SDL_GetTicks()), chan + 1, sample.hold, sample.acc[0],
                      sample.acc[1], sample.acc[2]);
    }
    trace << line;
    // A frame per line; flush so a crash or a killed process keeps the tail.
    trace.flush();
}

// Ends a calibration run with a message for the overlay.
void FinishAccelCalibration(const char* message) {
    g_calibration.active = false;
    std::snprintf(g_calibrationMessage, sizeof(g_calibrationMessage), "%s", message);
    RT_LOG(RT_TAG_CONFIG) << "Wii Remote accelerometer calibration: " << message << std::endl;
}

// One frame of a calibration run: accumulates the uncorrected sample and, once
// enough frames are in, stores the mean minus the ideal rest vector (0, 1, 0).
void StepAccelCalibration() {
    if (!g_calibration.active) {
        return;
    }
    SDL_Gamepad* gamepad = SDL_GetGamepadFromPlayerIndex(static_cast<int>(g_calibration.chan));
    if (gamepad == nullptr || !IsRemoteChannel(g_calibration.chan)) {
        FinishAccelCalibration("Cancelled: the Wii Remote went away.");
        return;
    }
    EnsureSensors(gamepad, g_calibration.chan);
    float sdl[3] = {};
    if (!ReadSdlAccel(gamepad, SDL_SENSOR_ACCEL, sdl)) {
        return; // no sample this frame; keep waiting
    }
    float g[3];
    for (int i = 0; i < 3; ++i) g[i] = sdl[i] / kStandardGravity;
    if (IsGlitchedSample(SDL_SENSOR_ACCEL, g)) {
        return; // a zeroed report, not a movement
    }
    if (g_calibration.count == 0) {
        for (int i = 0; i < 3; ++i) g_calibration.first[i] = g[i];
    } else {
        for (int i = 0; i < 3; ++i) {
            if (std::fabs(g[i] - g_calibration.first[i]) > kCalibrationMaxDeviationG) {
                FinishAccelCalibration("Failed: the remote moved. Put it down, buttons up, and try again.");
                return;
            }
        }
    }
    for (int i = 0; i < 3; ++i) g_calibration.sum[i] += g[i];
    if (++g_calibration.count < kCalibrationSamples) {
        return;
    }
    std::array<double, 3> mean{};
    for (int i = 0; i < 3; ++i) mean[i] = g_calibration.sum[i] / g_calibration.count;
    const double length = std::sqrt(mean[0] * mean[0] + mean[1] * mean[1] + mean[2] * mean[2]);
    if (length < kCalibrationMinGravityG || length > kCalibrationMaxGravityG || mean[1] < 0.5) {
        FinishAccelCalibration("Failed: the remote was not resting flat with the buttons up.");
        return;
    }
    const std::array<double, 3> offset = {mean[0], mean[1] - 1.0, mean[2]};
    for (int i = 0; i < 3; ++i) g_accelOffset[i] = static_cast<float>(offset[i]);
    g_accelOffsetLoaded = true;
    const bool saved = RuntimeConfigFile::SetWiiAccelOffset(offset);
    char message[160];
    std::snprintf(message, sizeof(message), "%s offset x %+.3f  y %+.3f  z %+.3f g",
                  saved ? "Calibrated:" : "Calibrated (could not write Config.toml):", offset[0], offset[1],
                  offset[2]);
    FinishAccelCalibration(message);
}

// True when any controller aurora knows about is a Wii device.
bool AnyWiiControllerConnected() {
    const uint32_t count = PADCount();
    for (uint32_t index = 0; index < count; ++index) {
        if (KindForName(PADGetNameForControllerIndex(index)) != Kind::NotWii) {
            return true;
        }
    }
    return false;
}

[[maybe_unused]] void TracePs2PortStateOnce(uint32_t chan, SDL_Gamepad* gamepad) {
    if (chan != 0 || RuntimeConfigFile::ControllerPromptStyle("gamecube") != "ps2") {
        return;
    }

    static bool reportedMissing = false;
    static bool reportedPresent = false;
    if (gamepad != nullptr) {
        if (reportedPresent) return;
        reportedPresent = true;
        RT_LOG(RT_TAG_CONFIG) << "PS2 input port 1 assigned: index=" << PADGetIndexForPort(chan)
                              << " name=\"" << (SDL_GetGamepadName(gamepad) ? SDL_GetGamepadName(gamepad) : "")
                              << "\" controllers=" << PADCount() << std::endl;
        return;
    }

    if (reportedMissing) return;
    reportedMissing = true;
    RT_LOG(RT_TAG_CONFIG) << "PS2 input port 1 has no assigned gamepad; controllers=" << PADCount();
    const uint32_t count = PADCount();
    for (uint32_t index = 0; index < count; ++index) {
        const char* name = PADGetNameForControllerIndex(index);
        RT_LOG(RT_TAG_CONFIG) << " [" << index << "]=\"" << (name ? name : "") << "\"";
    }
    RT_LOG(RT_TAG_CONFIG) << std::endl;
}

int16_t ClassicStickRaw(Sint16 axis, bool invert) {
    float value = static_cast<float>(axis) / 32767.0f;
    if (invert) value = -value;
    value = std::clamp(value, -1.0f, 1.0f);
    return static_cast<int16_t>(std::clamp(std::lround(value * 512.0f), -512L, 511L));
}

// Host-side semantic Wii Remote + Nunchuk. This is intentionally built from
// the same PAD layer used by the settings/mapping UI so an ordinary SDL
// controller keeps the user's mappings. Keyboard bindings below provide a
// deterministic usable fallback even when the PAD keyboard profile is not
// enabled. Nothing here changes guest state directly: WPADProbe/KPADRead see a
// normal connected FREESTYLE device and the game consumes its buttons/stick.
bool ReadVirtualNunchukSample(uint32_t chan, KpadSample& sample) {
    if (chan != 0 || !RuntimeConfigFile::WiiVirtualRemoteEnabled(false)) {
        return false;
    }

    static bool announced = false;
    if (!announced) {
        announced = true;
        RT_LOG(RT_TAG_CONFIG) << "Virtual Wii Remote + Nunchuk active on channel 1 "
                             << "(host gamepad/keyboard -> WPAD/KPAD)" << std::endl;
    }

    PADStatus pads[PAD_CHANMAX]{};
    PADRead(pads);
    const PADStatus& pad = pads[chan];

    sample = {};
    sample.hasNunchuk = true;
    sample.acc[0] = 0.0f;
    sample.acc[1] = -1.0f;
    sample.acc[2] = 0.0f;
    sample.nunchukAcc[0] = 0.0f;
    sample.nunchukAcc[1] = -1.0f;
    sample.nunchukAcc[2] = 0.0f;

    if (pad.err == PAD_ERR_NONE) {
        if (pad.button & PAD_BUTTON_A) sample.hold |= kWpadA;
        if (pad.button & PAD_BUTTON_B) sample.hold |= kWpadB;
        if (pad.button & PAD_BUTTON_X) sample.hold |= kWpadOne;
        if (pad.button & PAD_BUTTON_Y) sample.hold |= kWpadTwo;
        if (pad.button & PAD_BUTTON_START) sample.hold |= kWpadPlus;
        if (pad.button & PAD_BUTTON_UP) sample.hold |= kWpadUp;
        if (pad.button & PAD_BUTTON_DOWN) sample.hold |= kWpadDown;
        if (pad.button & PAD_BUTTON_LEFT) sample.hold |= kWpadLeft;
        if (pad.button & PAD_BUTTON_RIGHT) sample.hold |= kWpadRight;
        if (pad.button & PAD_TRIGGER_L) sample.hold |= kWpadC;
        if ((pad.button & PAD_TRIGGER_Z) || pad.triggerR >= 128) sample.hold |= kWpadZ;
        sample.stick[0] = std::clamp(static_cast<float>(pad.stickX) / 127.0f, -1.0f, 1.0f);
        sample.stick[1] = std::clamp(static_cast<float>(pad.stickY) / 127.0f, -1.0f, 1.0f);

        // The right stick represents the orientation of the virtual Remote.
        // Keep the vector on the 1 g sphere so this looks like a tilted remote,
        // not a fabricated shake impulse. Motion gestures can be layered on as
        // actual acceleration later without corrupting orientation semantics.
        const float tiltX = std::clamp(static_cast<float>(pad.substickX) / 127.0f, -1.0f, 1.0f);
        const float tiltZ = std::clamp(static_cast<float>(pad.substickY) / 127.0f, -1.0f, 1.0f);
        const float horizontalSq = std::min(1.0f, tiltX * tiltX + tiltZ * tiltZ);
        sample.acc[0] = tiltX;
        sample.acc[1] = -std::sqrt(1.0f - horizontalSq);
        sample.acc[2] = tiltZ;
    }

    int keyCount = 0;
    const bool* keys = SDL_GetKeyboardState(&keyCount);
    const auto down = [&](SDL_Scancode key) {
        return keys != nullptr && static_cast<int>(key) < keyCount && keys[key];
    };
    if (down(SDL_SCANCODE_SPACE)) sample.hold |= kWpadA;
    if (down(SDL_SCANCODE_LSHIFT) || down(SDL_SCANCODE_RSHIFT)) sample.hold |= kWpadB;
    if (down(SDL_SCANCODE_1)) sample.hold |= kWpadOne;
    if (down(SDL_SCANCODE_2)) sample.hold |= kWpadTwo;
    if (down(SDL_SCANCODE_RETURN)) sample.hold |= kWpadPlus;
    if (down(SDL_SCANCODE_BACKSPACE)) sample.hold |= kWpadMinus;
    if (down(SDL_SCANCODE_UP)) sample.hold |= kWpadUp;
    if (down(SDL_SCANCODE_DOWN)) sample.hold |= kWpadDown;
    if (down(SDL_SCANCODE_LEFT)) sample.hold |= kWpadLeft;
    if (down(SDL_SCANCODE_RIGHT)) sample.hold |= kWpadRight;
    if (down(SDL_SCANCODE_Q)) sample.hold |= kWpadC;
    if (down(SDL_SCANCODE_E)) sample.hold |= kWpadZ;

    // Diagnostic-only boot shortcut: the long RDSPAF intro otherwise delays
    // graphics validation by more than a minute. It is opt-in and expires
    // after a bounded A press, so normal runs and controller behavior are
    // unchanged.
    static uint32_t autoSkipFrames = 0;
    if (std::getenv("METEOR_SKIP_INTRO") != nullptr && autoSkipFrames < 300u) {
        sample.hold |= kWpadA;
        ++autoSkipFrames;
    }
    float mouseX = 0.0f;
    float mouseY = 0.0f;
    const SDL_MouseButtonFlags mouseButtons = SDL_GetMouseState(&mouseX, &mouseY);
    if ((mouseButtons & SDL_BUTTON_LMASK) != 0) sample.hold |= kWpadA;
    if (SDL_Window* mouseWindow = SDL_GetMouseFocus()) {
        int windowWidth = 0;
        int windowHeight = 0;
        if (SDL_GetWindowSize(mouseWindow, &windowWidth, &windowHeight) &&
            windowWidth > 0 && windowHeight > 0 && mouseX >= 0.0f && mouseY >= 0.0f &&
            mouseX < static_cast<float>(windowWidth) && mouseY < static_cast<float>(windowHeight)) {
            sample.pointerValid = true;
            sample.pointer[0] = std::clamp((mouseX / static_cast<float>(windowWidth)) * 2.0f - 1.0f, -1.0f, 1.0f);
            sample.pointer[1] = std::clamp((mouseY / static_cast<float>(windowHeight)) * 2.0f - 1.0f, -1.0f, 1.0f);
        }
    }
    const float keyboardX = (down(SDL_SCANCODE_D) ? 1.0f : 0.0f) - (down(SDL_SCANCODE_A) ? 1.0f : 0.0f);
    const float keyboardY = (down(SDL_SCANCODE_W) ? 1.0f : 0.0f) - (down(SDL_SCANCODE_S) ? 1.0f : 0.0f);
    if (keyboardX != 0.0f || keyboardY != 0.0f) {
        sample.stick[0] = keyboardX;
        sample.stick[1] = keyboardY;
    }
    return true;
}

// Route SDL's input diagnostics (HIDAPI open failures, the Wii driver's
// extension/status messages) into console.log, minus the periodic chatter.
void SDLCALL LogSdlMessage(void*, int category, SDL_LogPriority priority, const char* message) {
    if (message == nullptr) {
        return;
    }
    if (category == SDL_LOG_CATEGORY_INPUT && priority < SDL_LOG_PRIORITY_WARN &&
        (std::strstr(message, "Motion Plus") != nullptr || std::strstr(message, "Resetting report mode") != nullptr)) {
        return;
    }
    if (category == SDL_LOG_CATEGORY_INPUT || priority >= SDL_LOG_PRIORITY_WARN) {
        RT_LOG("sdl") << message << std::endl;
    }
}

// Second half of a rescan: re-enables the Wii driver once SDL has seen it off.
void FinishRescan(uint64_t now) {
    if (g_driverOffSinceMs == 0 || now - g_driverOffSinceMs < kRescanDriverOffMs) {
        return;
    }
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_WII, "1");
    g_driverOffSinceMs = 0;
    g_lastScanMs = now;
    ++g_scanCount;
}

} // namespace

// Enables SDL's HIDAPI Wii driver and player LEDs, and routes SDL's input log.
void ConfigureSdlHints(bool enabled) {
    // A rescan may be mid-flight; drop its bookkeeping so Poll() is not left
    // waiting for a FinishRescan() that can no longer happen.
    g_driverOffSinceMs = 0;
    if (!SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_WII, enabled ? "1" : "0")) {
        RT_LOG(RT_TAG_CONFIG) << "Failed to set " << SDL_HINT_JOYSTICK_HIDAPI_WII << ": " << SDL_GetError()
                              << std::endl;
    }
    // Light the player LED that matches the SDL player index, like the console does.
    if (!SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_WII_PLAYER_LED, "1")) {
        RT_LOG(RT_TAG_CONFIG) << "Failed to set " << SDL_HINT_JOYSTICK_HIDAPI_WII_PLAYER_LED << ": "
                              << SDL_GetError() << std::endl;
    }
    RT_LOG(RT_TAG_CONFIG) << "Bluetooth Wii Remote support " << (enabled ? "enabled" : "disabled") << std::endl;
    g_wiiDriverEnabled = enabled;
    if (enabled) {
        // Release behavior: retain actionable SDL input warnings/errors without
        // enabling the verbose HIDAPI debug stream during normal gameplay.
        SDL_SetLogPriority(SDL_LOG_CATEGORY_INPUT, SDL_LOG_PRIORITY_WARN);
        SDL_SetLogOutputFunction(LogSdlMessage, nullptr);
    }
}

// Starts a rescan by disabling the Wii driver hint; Poll() finishes it.
void RescanNow() {
    if (!g_wiiDriverEnabled || g_driverOffSinceMs != 0) {
        return;
    }
    // SDL only closes the HID handle of a remote it dropped while the Wii driver
    // is disabled, and only re-opens it when the driver is enabled again; both
    // must happen on separate joystick updates, so the hint stays at "0" until
    // FinishRescan() a few frames later. Flipping 1->0->1 within one frame does
    // nothing: SDL only ever sees the final "1".
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_WII, "0");
    g_driverOffSinceMs = SDL_GetTicks();
}

// Per-frame scanning state machine: rescans while no Wii controller is present.
// Also advances an accelerometer calibration run, which needs a sample per frame
// whether or not the game is reading KPAD at that moment.
void Poll() {
    StepAccelCalibration();
    // Remember what each port had, so EffectiveKind can bridge a swap.
    for (uint32_t port = 0; port < PAD_MAX_CONTROLLERS; ++port) {
        (void)EffectiveKind(port);
    }
    if (!g_wiiDriverEnabled) {
        return;
    }
    // Always complete a rescan in progress so the driver is never left disabled.
    FinishRescan(SDL_GetTicks());
    if (g_driverOffSinceMs != 0) {
        return;
    }
    if (AnyWiiControllerConnected()) {
        g_scanning = false;
        g_scanCount = 0;
        g_lastScanMs = SDL_GetTicks();
        return;
    }
    if (!RuntimeConfigFile::WiiContinuousScanEnabled(true)) {
        g_scanning = false;
        return;
    }
    const uint64_t now = SDL_GetTicks();
    if (!g_scanning) {
        g_scanning = true;
        g_lostAtMs = now;
    }
    if (now - g_lostAtMs < kScanStartDelayMs) {
        return;
    }
    const uint64_t interval = now - g_lostAtMs < kFastScanWindowMs ? kFastScanIntervalMs : kScanIntervalMs;
    if (now - g_lastScanMs < interval) {
        return;
    }
    RescanNow();
}

// True while Poll() is looking for a remote.
bool IsScanning() {
    return g_scanning;
}

// Number of rescans since a Wii controller was last seen.
uint32_t ScanCount() {
    return g_scanCount;
}

// Maps the gamepad name SDL's Wii driver reports to a Kind.
Kind KindForName(const char* name) {
    // Names come from SDL's hidapi Wii driver: "Nintendo Wii Remote",
    // "Nintendo Wii Remote with Nunchuk", "Nintendo Wii Remote with Classic
    // Controller" and "Nintendo Wii U Pro Controller".
    if (NameContains(name, "Wii U Pro Controller")) return Kind::WiiUPro;
    if (!NameContains(name, "Wii Remote")) return Kind::NotWii;
    if (NameContains(name, "Nunchuk")) return Kind::RemoteWithNunchuk;
    if (NameContains(name, "Classic Controller")) return Kind::RemoteWithClassic;
    return Kind::Remote;
}

// Kind of the SDL gamepad assigned to a game port, NotWii when empty.
Kind KindForPort(uint32_t port) {
    if (port >= PAD_MAX_CONTROLLERS) return Kind::NotWii;
    SDL_Gamepad* gamepad = PADGetGamepadForPort(port);
    if (gamepad == nullptr) return Kind::NotWii;
    return KindForName(SDL_GetGamepadName(gamepad));
}

// Human-readable name of a Kind for the settings overlay.
const char* KindLabel(Kind kind) {
    switch (kind) {
    case Kind::Remote: return "Wii Remote";
    case Kind::RemoteWithNunchuk: return "Wii Remote + Nunchuk";
    case Kind::RemoteWithClassic: return "Wii Remote + Classic Controller";
    case Kind::WiiUPro: return "Wii U Pro Controller";
    default: return "Not a Wii controller";
    }
}

// True for the kinds the game reads through KPAD.
static bool IsKpadKind(Kind kind) {
    return kind == Kind::Remote || kind == Kind::RemoteWithNunchuk || kind == Kind::RemoteWithClassic;
}

// Live kind of the port, or the remembered one while a swap is in flight.
// Called from the guest thread only (PADRead, KPADRead, WPADProbe and the
// overlay's Draw all run there), so the port memory needs no locking.
Kind EffectiveKind(uint32_t chan) {
    if (chan >= PAD_MAX_CONTROLLERS) return Kind::NotWii;
    PortMemory& memory = g_ports[chan];
    // Match Aurora's persistent port assignment after controller remaps.
    SDL_Gamepad* gamepad = PADGetGamepadForPort(chan);
    const Kind live = gamepad != nullptr ? KindForName(SDL_GetGamepadName(gamepad)) : Kind::NotWii;
    const uint64_t now = SDL_GetTicks();
    if (live != Kind::NotWii) {
        memory.lastKind = live;
        memory.lastSeenMs = now;
        return live;
    }
    if (chan == 0 && RuntimeConfigFile::WiiVirtualRemoteEnabled(false)) {
        return Kind::RemoteWithNunchuk;
    }
    if (gamepad != nullptr) {
        // Another controller took the port: the remote is not coming back here.
        memory.lastKind = Kind::NotWii;
        return Kind::NotWii;
    }
    if (IsKpadKind(memory.lastKind) && memory.lastSeenMs != 0 && now - memory.lastSeenMs < kExtensionSwapGraceMs) {
        return memory.lastKind;
    }
    return Kind::NotWii;
}

// True when the game reads the port through KPAD (live or bridging a swap).
bool IsRemoteChannel(uint32_t chan) {
    return IsKpadKind(EffectiveKind(chan));
}

// Marks KPAD-served ports as "no controller" in the GameCube pad statuses.
void HideRemotesFromPad(PADStatus* statuses, uint32_t count) {
    for (uint32_t port = 0; port < count && port < PAD_MAX_CONTROLLERS; ++port) {
        if (IsRemoteChannel(port)) {
            statuses[port] = {};
            statuses[port].err = PAD_ERR_NO_CONTROLLER;
        }
    }
}

namespace {

Ps2SemanticContext ReadPs2SemanticContext() {
    if (!Memory::Contains(0x803D10B4u, sizeof(uint32_t)) ||
        !Memory::Contains(0x803D10B8u, sizeof(uint32_t)) ||
        !Memory::Contains(0x803CF6E4u, sizeof(uint32_t))) {
        return Ps2SemanticContext::Frontend;
    }
    const bool battleCycle = Memory::Read32(0x803D10B8u) != 0;
    // The battle-cycle flag can remain set during loading.
    const bool battlePlayable = Memory::Read32(0x803CF6E4u) == 1u;
    // Some battle paths leave the playable flag clear after fighter setup.
    // Registered fighters keep PS2 face mapping in combat until the cycle ends.
    bool hasLiveFighter = false;
    for (uint32_t port = 0; port < g_ps2Fighters.size(); ++port) {
        const uint32_t fighter = g_ps2Fighters[port];
        if (fighter && Memory::Contains(fighter, sizeof(uint32_t)) && Memory::Read32(fighter) == port) {
            hasLiveFighter = true;
            break;
        }
    }
    const bool pause = (Memory::Read32(0x803D10B4u) & 0x4000u) != 0;
    return Ps2Controller::ContextForBattleState(battleCycle, battlePlayable || hasLiveFighter, pause);
}

struct Ps2RecoveryReactionMatch {
    uint32_t fighter = 0;
    uint32_t state = 0;
    uint32_t port = 0;
    unsigned matches = 0;
};

Ps2RecoveryReactionMatch FindPs2RecoveryReactionMatchRegistered() {
    Ps2RecoveryReactionMatch result{};
    try {
        for (uint32_t port = 0; port < g_ps2Fighters.size(); ++port) {
            const uint32_t fighter = g_ps2Fighters[port];
            if (!fighter || !Memory::Contains(fighter, 0x40u) || Memory::Read32(fighter) != port) {
                continue;
            }
            const uint32_t state = Memory::Read32(fighter + 0x3Cu);
            if (!Ps2Controller::IsRecoveryReactionState(state)) continue;

            ++result.matches;
            result.fighter = fighter;
            result.state = state;
            result.port = port;
        }
    } catch (const Memory::AccessViolation&) {
        return {};
    }
    return result;
}

Ps2RecoveryReactionMatch FindPs2RecoveryReactionMatch(Ps2SemanticContext context) {
    if (context != Ps2SemanticContext::Battle) return {};
    return FindPs2RecoveryReactionMatchRegistered();
}

void TracePs2RecoveryProbeState() {
    if (std::getenv("METEOR_PS2_RECOVERY_PROBE") == nullptr) return;

    constexpr uint32_t kScenePointer = 0x8062A5F0u;
    constexpr uint32_t kTrainingPointer = 0x8062AAC0u;
    if (!Memory::Contains(kScenePointer, sizeof(uint32_t)) ||
        !Memory::Contains(kTrainingPointer, sizeof(uint32_t))) {
        return;
    }

    const uint32_t scene = Memory::Read32(kScenePointer);
    const uint32_t training = Memory::Read32(kTrainingPointer);
    uint32_t sceneId = 0xffffffffu;
    uint32_t originMenuId = 0xffffffffu;
    uint32_t trainingStyle = 0xffffffffu;
    uint32_t trainingFsm = 0xffffffffu;
    if (scene && Memory::Contains(scene, 0x534u)) {
        sceneId = Memory::Read32(scene + 0x18u);
        originMenuId = Memory::Read32(scene + 0x2Cu);
    }
    if (training && Memory::Contains(training, 0x494u)) {
        trainingStyle = Memory::Read32(training + 0x480u);
        trainingFsm = Memory::Read32(training + 0x490u);
    }

    uint32_t battleFlags = 0xffffffffu;
    uint32_t battleCycle = 0xffffffffu;
    uint32_t battlePlayable = 0xffffffffu;
    if (Memory::Contains(0x803D10B4u, sizeof(uint32_t))) battleFlags = Memory::Read32(0x803D10B4u);
    if (Memory::Contains(0x803D10B8u, sizeof(uint32_t))) battleCycle = Memory::Read32(0x803D10B8u);
    if (Memory::Contains(0x803CF6E4u, sizeof(uint32_t))) battlePlayable = Memory::Read32(0x803CF6E4u);

    struct Snapshot {
        uint32_t scene = 0xffffffffu;
        uint32_t sceneId = 0xffffffffu;
        uint32_t originMenuId = 0xffffffffu;
        uint32_t training = 0xffffffffu;
        uint32_t trainingStyle = 0xffffffffu;
        uint32_t trainingFsm = 0xffffffffu;
        uint32_t battleFlags = 0xffffffffu;
        uint32_t battleCycle = 0xffffffffu;
        uint32_t battlePlayable = 0xffffffffu;
    };
    static Snapshot last;
    const Snapshot now{scene, sceneId, originMenuId, training, trainingStyle, trainingFsm,
                       battleFlags, battleCycle, battlePlayable};
    if (std::memcmp(&now, &last, sizeof(now)) == 0) return;
    last = now;

    RT_LOG(RT_TAG_CONFIG) << "PS2 recovery probe state: scene=0x" << std::hex << scene
                          << " sceneId=" << std::dec << sceneId
                          << " originMenuId=" << originMenuId
                          << " training=0x" << std::hex << training
                          << " style=" << std::dec << trainingStyle
                          << " fsm=" << trainingFsm
                          << " battleFlags=0x" << std::hex << battleFlags
                          << " battleCycle=0x" << battleCycle
                          << " playable=0x" << battlePlayable
                          << std::dec << std::endl;
}

void UpdatePs2FighterBindings(bool enabled) {
    for (uint32_t& fighter : g_ps2Fighters) {
        if (!fighter) continue;
        if (!Memory::Contains(fighter, Ps2Controller::kBindingOffset + sizeof(Ps2Controller::Bindings))) {
            fighter = 0;
            continue;
        }
        // Scene transitions can free the fighter table.
        bool original = true, ps2 = true;
        for (unsigned i = 0; i < Ps2Controller::kGamecubeBindings.size(); ++i) {
            const uint32_t value = Memory::Read32(fighter + Ps2Controller::kBindingOffset + i * 4);
            original &= value == Ps2Controller::kGamecubeBindings[i];
            ps2 &= value == Ps2Controller::kPs2Bindings[i];
        }
        if (!original && !ps2) {
            fighter = 0;
            continue;
        }
        if ((enabled && ps2) || (!enabled && original)) continue;
        const auto& bindings = enabled ? Ps2Controller::kPs2Bindings : Ps2Controller::kGamecubeBindings;
        for (unsigned i = 0; i < bindings.size(); ++i) {
            Memory::Write32(fighter + Ps2Controller::kBindingOffset + i * 4, bindings[i]);
        }
    }
}

bool DiscoverPs2PromptBindings() {
    if (g_ps2PromptBindings.initialized) {
        return g_ps2PromptBindings.valid;
    }

    Ps2PromptBindings found;
    try {
        // Find style3 token rows by resource ID; PS2 art preserves their identity.
        constexpr uint32_t kGcPromptTable = 0x80371958u;
        constexpr uint32_t kRowStride = 0x0Cu;
        constexpr uint32_t kMaxRows = 32u;
        for (uint32_t i = 0; i < kMaxRows; ++i) {
            const uint32_t row = kGcPromptTable + i * kRowStride;
            const uint32_t token = Memory::Read32(row);
            if (token == 0) {
                break;
            }
            const uint32_t metadata = Memory::Read32(row + 4u);
            if (metadata == 0) {
                continue;
            }
            const uint8_t resource = static_cast<uint8_t>(Memory::Read32(metadata) >> 24);
            const Ps2PromptRowBinding binding{row + 4u, metadata};
            switch (resource) {
            case 15: found.gcStart = binding; break;
            case 18: found.gcXStatic = binding; break;
            case 20: found.gcAStatic = binding; break;
            case 22: found.gcYStatic = binding; break;
            case 24: found.gcBStatic = binding; break;
            case 26: found.gcCUp = binding; break;
            case 28: found.gcL = binding; break;
            case 30: found.gcCRight = binding; break;
            case 32: found.gcZ = binding; break;
            case 34: found.gcR = binding; break;
            case 36: found.gcCDown = binding; break;
            case 38: found.gcXAnim = binding; break;
            case 40: found.gcAAnim = binding; break;
            case 42: found.gcYAnim = binding; break;
            case 44: found.gcBAnim = binding; break;
            case 46: found.gcAPress = binding; break;
            case 48: found.gcYPress = binding; break;
            default: break;
            }
        }
    } catch (const Memory::AccessViolation&) {
        return false;
    }

    found.valid =
        found.gcAStatic.valid() && found.gcBStatic.valid() &&
        found.gcXStatic.valid() && found.gcYStatic.valid() &&
        found.gcAAnim.valid() && found.gcBAnim.valid() &&
        found.gcXAnim.valid() && found.gcYAnim.valid() &&
        found.gcStart.valid() && found.gcCUp.valid() &&
        found.gcL.valid() && found.gcCRight.valid() &&
        found.gcZ.valid() && found.gcR.valid() &&
        found.gcCDown.valid() && found.gcAPress.valid() && found.gcYPress.valid();
    found.initialized = found.valid;
    if (!found.valid) {
        return false;
    }
    g_ps2PromptBindings = found;
    RT_LOG(RT_TAG_CONFIG) << "PS2 semantic prompt bindings discovered from GC style3 retail table" << std::endl;
    return true;
}

void WritePs2PromptBinding(const Ps2PromptRowBinding& destination,
                           const Ps2PromptRowBinding& semanticSource) {
    Memory::Write32(destination.metadataPointerField, semanticSource.originalMetadata);
}

void RestorePs2PromptBindings() {
    if (!g_ps2PromptProfileApplied || !DiscoverPs2PromptBindings()) {
        return;
    }
    const Ps2PromptBindings& p = g_ps2PromptBindings;
    try {
        WritePs2PromptBinding(p.gcAStatic, p.gcAStatic);
        WritePs2PromptBinding(p.gcBStatic, p.gcBStatic);
        WritePs2PromptBinding(p.gcXStatic, p.gcXStatic);
        WritePs2PromptBinding(p.gcYStatic, p.gcYStatic);
        WritePs2PromptBinding(p.gcAAnim, p.gcAAnim);
        WritePs2PromptBinding(p.gcBAnim, p.gcBAnim);
        WritePs2PromptBinding(p.gcXAnim, p.gcXAnim);
        WritePs2PromptBinding(p.gcYAnim, p.gcYAnim);
    } catch (const Memory::AccessViolation&) {
    }
    g_ps2PromptProfileApplied = false;
}

void ApplyPs2PromptSemanticProfile(Ps2SemanticContext context) {
    if (!DiscoverPs2PromptBindings()) {
        return;
    }
    if (g_ps2PromptProfileApplied) {
        g_ps2PromptProfileContext = context;
        return;
    }

    const Ps2PromptBindings& p = g_ps2PromptBindings;
    try {
        // Prompt rows retain their PAL PS2 token identity across input contexts.
        WritePs2PromptBinding(p.gcAStatic, p.gcAStatic);
        WritePs2PromptBinding(p.gcBStatic, p.gcBStatic);
        WritePs2PromptBinding(p.gcXStatic, p.gcXStatic);
        WritePs2PromptBinding(p.gcYStatic, p.gcYStatic);
        WritePs2PromptBinding(p.gcAAnim, p.gcAAnim);
        WritePs2PromptBinding(p.gcBAnim, p.gcBAnim);
        WritePs2PromptBinding(p.gcXAnim, p.gcXAnim);
        WritePs2PromptBinding(p.gcYAnim, p.gcYAnim);
        g_ps2PromptProfileContext = context;
        g_ps2PromptProfileApplied = true;
        RT_LOG(RT_TAG_CONFIG) << "PS2 face prompt profile: preserve literal PAL PS2 tokens" << std::endl;
    } catch (const Memory::AccessViolation&) {
        g_ps2PromptProfileApplied = false;
    }
}

} // namespace

namespace {

void ApplyPs2Gamepad(uint32_t port, SDL_Gamepad* gamepad, PADStatus* status) {
    if (port >= PAD_MAX_CONTROLLERS || status == nullptr) return;
    if (gamepad != nullptr && IsKpadKind(KindForName(SDL_GetGamepadName(gamepad)))) return;

    // Use Aurora's merged controller and keyboard mappings.
    const uint16_t mappedButtons = status->button;
    status->button = Ps2Controller::MapContextFaces(g_ps2ContextGates[port], mappedButtons);

    // Z/L transport L1/Select. Provenance distinguishes mapped buttons from
    // GameCube trigger emulation; raw SDL buttons must not bypass user mappings.
    const bool l1 = (mappedButtons & PAD_TRIGGER_Z) != 0;
    const bool select = (mappedButtons & PAD_TRIGGER_L) != 0 &&
                        (status->extButton & PAD_BUTTON_EXT_MAPPED_TRIGGER_L) != 0;
    const bool r1 = (mappedButtons & PAD_TRIGGER_R) != 0 &&
                    (status->extButton & PAD_BUTTON_EXT_MAPPED_TRIGGER_R) != 0;
    status->button &= static_cast<uint16_t>(~(PAD_TRIGGER_Z | PAD_TRIGGER_L | PAD_TRIGGER_R));
    if (l1) status->button |= Ps2Controller::kL1;
    if (select) status->button |= Ps2Controller::kSelect;
    if ((status->extButton & PAD_BUTTON_LEFT_STICK) != 0) status->button |= Ps2Controller::kL3;
    if ((status->extButton & PAD_BUTTON_RIGHT_STICK) != 0) status->button |= Ps2Controller::kR3;

    // Map L2/R2 axes to the guest transport without duplicating GC trigger bits.
    constexpr uint8_t kTriggerThreshold = 32;
    const bool l2 = status->triggerLeft > kTriggerThreshold;
    const bool r2 = status->triggerRight > kTriggerThreshold;
    if (l2) status->button |= PAD_TRIGGER_Z;
    if (r2) status->button |= PAD_TRIGGER_L;
    if (r1) status->button |= PAD_TRIGGER_R;
    status->triggerLeft = r2 ? 255 : 0;
    status->triggerRight = r1 ? 255 : 0;
    status->analogA = (status->button & PAD_BUTTON_A) ? 255 : 0;
    status->analogB = (status->button & PAD_BUTTON_B) ? 255 : 0;
}
}

void RegisterPlayStation2Fighter(uint32_t fighter) {
    if (!fighter || !Memory::Contains(fighter, sizeof(uint32_t))) return;
    const uint32_t player = Memory::Read32(fighter);
    if (player >= g_ps2Fighters.size()) return;
    g_ps2Fighters[player] = fighter;
    UpdatePs2FighterBindings(RuntimeConfigFile::ControllerPromptStyle("gamecube") == "ps2");
}

void RegisterPlayStation2PromptRecordSource(uint32_t record, uint32_t sourceReturnAddress) {
    if (!record || !sourceReturnAddress || RuntimeConfig::GAME_CODE != 0x52445350u) return;
    auto& entry = g_ps2PromptRecordSources[g_ps2PromptRecordSourceCursor];
    entry.record = record;
    entry.sourceReturnAddress = sourceReturnAddress;
    g_ps2PromptRecordSourceCursor = (g_ps2PromptRecordSourceCursor + 1u) % g_ps2PromptRecordSources.size();
}

void BeginPlayStation2BattlePromptScope() {
    if (RuntimeConfig::GAME_CODE != 0x52445350u ||
        RuntimeConfigFile::ControllerPromptStyle("gamecube") != "ps2") {
        return;
    }
    if (g_ps2BattlePromptScopeDepth++ != 0) return;

    g_ps2BattlePromptRestoreContext = ReadPs2SemanticContext();
    const Ps2SemanticContext renderContext = Ps2Controller::PromptContextForRender(
        g_ps2BattlePromptRestoreContext, true);
    ApplyPs2PromptSemanticProfile(renderContext);

    if (std::getenv("METEOR_PS2_RECOVERY_PROBE") != nullptr &&
        g_ps2BattlePromptRestoreContext != renderContext) {
        RT_LOG(RT_TAG_CONFIG) << "PS2 battle-action prompt scope: frontend -> battle" << std::endl;
    }
}

void EndPlayStation2BattlePromptScope() {
    if (g_ps2BattlePromptScopeDepth == 0) return;
    if (--g_ps2BattlePromptScopeDepth != 0) return;

    if (RuntimeConfig::GAME_CODE == 0x52445350u &&
        RuntimeConfigFile::ControllerPromptStyle("gamecube") == "ps2") {
        ApplyPs2PromptSemanticProfile(g_ps2BattlePromptRestoreContext);
        if (std::getenv("METEOR_PS2_RECOVERY_PROBE") != nullptr &&
            g_ps2BattlePromptRestoreContext != Ps2SemanticContext::Battle) {
            RT_LOG(RT_TAG_CONFIG) << "PS2 battle-action prompt scope: restore frontend" << std::endl;
        }
    } else {
        RestorePs2PromptBindings();
    }
}

bool IsPlayStation2RecoveryReactionActive() {
    if (RuntimeConfig::GAME_CODE != 0x52445350u ||
        RuntimeConfigFile::ControllerPromptStyle("gamecube") != "ps2") {
        return false;
    }
    const Ps2SemanticContext context = ReadPs2SemanticContext();
    // Pausing must not hide a fighter's recovery state.
    // Registered fighters are scene-owned and cleared when the battle cycle
    // ends, so their retail state field is the precise source of truth here.
    const Ps2RecoveryReactionMatch match = FindPs2RecoveryReactionMatchRegistered();

    if (std::getenv("METEOR_PS2_RECOVERY_PROBE") != nullptr) {
        struct Snapshot {
            uint32_t context = 0xffffffffu;
            uint32_t matches = 0xffffffffu;
            std::array<uint32_t, PAD_MAX_CONTROLLERS> fighter{};
            std::array<uint32_t, PAD_MAX_CONTROLLERS> state{};
        };
        Snapshot now{};
        now.context = static_cast<uint32_t>(context);
        now.matches = match.matches;
        now.state.fill(0xffffffffu);
        for (uint32_t port = 0; port < g_ps2Fighters.size(); ++port) {
            const uint32_t fighter = g_ps2Fighters[port];
            now.fighter[port] = fighter;
            if (fighter && Memory::Contains(fighter, 0x40u) && Memory::Read32(fighter) == port) {
                now.state[port] = Memory::Read32(fighter + 0x3Cu);
            }
        }

        static Snapshot last{};
        static bool haveLast = false;
        if (!haveLast || std::memcmp(&now, &last, sizeof(now)) != 0) {
            last = now;
            haveLast = true;
            RT_LOG(RT_TAG_CONFIG) << "PS2 recovery scope predicate: context="
                                  << (context == Ps2SemanticContext::Battle ? "battle" : "frontend")
                                  << " matches=" << match.matches;
            for (uint32_t port = 0; port < g_ps2Fighters.size(); ++port) {
                RT_LOG(RT_TAG_CONFIG) << " p" << (port + 1)
                                      << "=0x" << std::hex << now.fighter[port]
                                      << "/state=0x" << now.state[port] << std::dec;
            }
            RT_LOG(RT_TAG_CONFIG) << std::endl;
        }
    }

    return match.matches == 1;
}

void BeginPlayStation2RecoveryPromptDrawScope() {
    ++g_ps2RecoveryPromptDrawScopeDepth;
}

void EndPlayStation2RecoveryPromptDrawScope() {
    if (g_ps2RecoveryPromptDrawScopeDepth != 0) {
        --g_ps2RecoveryPromptDrawScopeDepth;
    }
}

bool IsPlayStation2RecoveryPromptDrawScope() {
    return g_ps2RecoveryPromptDrawScopeDepth != 0;
}

uint32_t ResolvePlayStation2RecoveryPromptMetadata(uint32_t record, uint32_t metadata,
                                                   uint32_t drawX, uint32_t drawY) {
    // resource040 also appears in melee; require matching renderer metadata
    // and a live fighter reaction before selecting Cross.
    if (RuntimeConfig::GAME_CODE != 0x52445350u ||
        RuntimeConfigFile::ControllerPromptStyle("gamecube") != "ps2") {
        return metadata;
    }

    const uint32_t sourceReturnAddress = Ps2PromptSourceForRecord(record);

    const bool animatedFace = metadata == Ps2Controller::kWiiCircleAnimMetadata ||
                              metadata == Ps2Controller::kWiiAAnimMetadata ||
                              metadata == Ps2Controller::kWiiTriangleAnimMetadata ||
                              metadata == Ps2Controller::kWiiCrossAnimMetadata;
    if (animatedFace && std::getenv("METEOR_PS2_BATTLE_PROMPT_TRACE") != nullptr &&
        ReadPs2SemanticContext() == Ps2SemanticContext::Battle) {
        const uint32_t textPointer = record && Memory::Contains(record, 8u)
            ? Memory::Read32(record + 4u) : 0u;
        static uint32_t lastMetadata = 0;
        static uint32_t lastRecord = 0;
        static uint32_t lastText = 0;
        static uint32_t lastSource = 0;
        static uint32_t lastX = 0xffffffffu;
        static uint32_t lastY = 0xffffffffu;
        if (metadata != lastMetadata || record != lastRecord || textPointer != lastText ||
            sourceReturnAddress != lastSource || drawX != lastX || drawY != lastY) {
            RT_LOG(RT_TAG_CONFIG) << "PS2 battle HUD face prompt: metadata=0x" << std::hex << metadata
                                  << " record=0x" << record
                                  << " text=0x" << textPointer
                                  << " source_lr=0x" << sourceReturnAddress
                                  << std::dec << " x=" << static_cast<int32_t>(drawX)
                                  << " y=" << static_cast<int32_t>(drawY) << std::endl;
            lastMetadata = metadata;
            lastRecord = record;
            lastText = textPointer;
            lastSource = sourceReturnAddress;
            lastX = drawX;
            lastY = drawY;
        }
    }

    const bool provenanceCandidate = metadata == Ps2Controller::kWiiAAnimMetadata ||
                                     metadata == Ps2Controller::kWiiAPressMetadata ||
                                     metadata == Ps2Controller::kWiiYPressMetadata;
    if (provenanceCandidate && std::getenv("METEOR_PS2_RECOVERY_PROBE") != nullptr) {
        uint32_t textPointer = 0;
        try {
            if (record && Memory::Contains(record, 8u)) textPointer = Memory::Read32(record + 4u);
        } catch (const Memory::AccessViolation&) {
            textPointer = 0;
        }
        static uint32_t lastMetadata = 0;
        static uint32_t lastRecord = 0;
        static uint32_t lastText = 0;
        static uint32_t lastSource = 0;
        if (metadata != lastMetadata || record != lastRecord || textPointer != lastText ||
            sourceReturnAddress != lastSource) {
            RT_LOG(RT_TAG_CONFIG) << "PS2 prompt provenance: metadata=0x" << std::hex << metadata
                                  << " record=0x" << record
                                  << " text=0x" << textPointer
                                  << " source_lr=0x" << sourceReturnAddress
                                  << std::dec << " x=" << static_cast<int32_t>(drawX)
                                  << " y=" << static_cast<int32_t>(drawY) << std::endl;
            lastMetadata = metadata;
            lastRecord = record;
            lastText = textPointer;
            lastSource = sourceReturnAddress;
        }
    }

    if (metadata != Ps2Controller::kWiiAAnimMetadata) return metadata;

    const Ps2SemanticContext context = ReadPs2SemanticContext();
    if (context != Ps2SemanticContext::Battle) return metadata;

    uint32_t textPointer = 0;
    try {
        if (record && Memory::Contains(record, 8u)) {
            textPointer = Memory::Read32(record + 4u);
        }
    } catch (const Memory::AccessViolation&) {
        textPointer = 0;
    }

    if (std::getenv("METEOR_PS2_RECOVERY_PROBE") != nullptr) {
        static uint32_t lastCandidateRecord = 0;
        static uint32_t lastCandidateText = 0;
        static uint32_t lastCandidateSource = 0;
        if (record != lastCandidateRecord || textPointer != lastCandidateText ||
            sourceReturnAddress != lastCandidateSource) {
            RT_LOG(RT_TAG_CONFIG) << "PS2 resource040 candidate: record=0x" << std::hex << record
                                  << " text=0x" << textPointer
                                  << " source_lr=0x" << sourceReturnAddress
                                  << std::dec << " x=" << static_cast<int32_t>(drawX)
                                  << " y=" << static_cast<int32_t>(drawY) << std::endl;
            lastCandidateRecord = record;
            lastCandidateText = textPointer;
            lastCandidateSource = sourceReturnAddress;
        }
    }

    const Ps2RecoveryReactionMatch match = FindPs2RecoveryReactionMatch(context);

    // The renderer has no port; require a unique matching fighter.
    if (match.matches != 1) return metadata;

    static uint32_t lastLoggedFighter = 0;
    static uint32_t lastLoggedState = 0xffffffffu;
    static uint32_t lastLoggedTextPointer = 0;
    static uint32_t lastLoggedDrawX = 0xffffffffu;
    static uint32_t lastLoggedDrawY = 0xffffffffu;
    if (match.fighter != lastLoggedFighter || match.state != lastLoggedState ||
        textPointer != lastLoggedTextPointer || drawX != lastLoggedDrawX || drawY != lastLoggedDrawY) {
        RT_LOG(RT_TAG_CONFIG) << "PS2 recovery prompt: port " << (match.port + 1)
                              << " fighter=0x" << std::hex << match.fighter
                              << " state=0x" << match.state
                              << " record=0x" << record
                              << " text=0x" << textPointer
                              << " source_lr=0x" << sourceReturnAddress
                              << std::dec << " x=" << static_cast<int32_t>(drawX)
                              << " y=" << static_cast<int32_t>(drawY)
                              << " resource040 -> Cross anim" << std::dec << std::endl;
        lastLoggedFighter = match.fighter;
        lastLoggedState = match.state;
        lastLoggedTextPointer = textPointer;
        lastLoggedDrawX = drawX;
        lastLoggedDrawY = drawY;
    }
    return Ps2Controller::kWiiCrossAnimMetadata;
}

void CompletePlayStation2PadNormalization(uint32_t port) {
    if (port >= PAD_MAX_CONTROLLERS || !g_ps2Enabled) return;
    // Normalize before the guest computes edges, repeats and menu buttons.
    constexpr uint32_t kNormalizedPadButtons = 0x803B260Cu;
    const uint32_t address = kNormalizedPadButtons + port * 0x9CCu;
    if (!Memory::Contains(address, sizeof(uint32_t))) return;
    Memory::Write32(address, Memory::Read32(address) | g_ps2ExtraButtons[port]);
}

uint32_t ReadPadStatuses(PADStatus* statuses) {
    if (!statuses) return 0;
    TracePs2RecoveryProbeState();
    const bool ps2 = RuntimeConfig::GAME_CODE == 0x52445350u &&
                     RuntimeConfigFile::ControllerPromptStyle("gamecube") == "ps2";
    Ps2SemanticContext semanticContext = Ps2SemanticContext::Frontend;
    if (ps2) {
        semanticContext = ReadPs2SemanticContext();
        for (auto& gate : g_ps2ContextGates) {
            Ps2Controller::BeginContextSample(gate, semanticContext);
        }
        // Discard fighter addresses when their scene ends.
        if (Memory::Contains(0x803D10B8u, sizeof(uint32_t)) && Memory::Read32(0x803D10B8u) == 0u) {
            g_ps2Fighters.fill(0);
        }
    } else {
        RestorePs2PromptBindings();
        g_ps2ExtraButtons.fill(0);
        g_ps2ContextGates = {};
    }
    UpdatePs2FighterBindings(ps2);
    g_ps2Enabled = ps2;
    const uint32_t rumble = PADReadWithGamepadTransform(statuses, ps2 ? ApplyPs2Gamepad : nullptr);
    if (ps2) {
        for (auto& gate : g_ps2ContextGates) {
            Ps2Controller::EndContextSample(gate);
        }
        ApplyPs2PromptSemanticProfile(semanticContext);
    }
    for (uint32_t port = 0; port < PAD_MAX_CONTROLLERS; ++port) {
        // Use the filtered sample after focus suppression.
        g_ps2ExtraButtons[port] = ps2 && statuses[port].err == PAD_ERR_NONE
            ? statuses[port].button & Ps2Controller::kExtraButtons : 0;
        if (ps2) statuses[port].button &= ~Ps2Controller::kExtraButtons;
    }
    return rumble;
}

// Neutral sample of the remembered kind, for the frames of an extension swap.
void FillGraceSample(uint32_t chan, Kind kind, KpadSample& sample) {
    sample = {};
    for (int i = 0; i < 3; ++i) sample.acc[i] = g_lastAcc[chan].acc[i];
    sample.hasNunchuk = kind == Kind::RemoteWithNunchuk;
    sample.hasClassic = kind == Kind::RemoteWithClassic;
}

// Samples buttons, accelerometers and the extension of the remote on a port.
bool ReadKpadSample(uint32_t chan, KpadSample& sample) {
    if (chan >= PAD_MAX_CONTROLLERS) {
        return false;
    }
    SDL_Gamepad* gamepad = PADGetGamepadForPort(chan);
    const Kind kind = gamepad != nullptr ? KindForName(SDL_GetGamepadName(gamepad)) : Kind::NotWii;
    if (!IsKpadKind(kind)) {
        if (ReadVirtualNunchukSample(chan, sample)) {
            return true;
        }
        const Kind remembered = EffectiveKind(chan);
        if (!IsKpadKind(remembered)) {
            return false;
        }
        FillGraceSample(chan, remembered, sample);
        return true;
    }
    SDL_Joystick* joystick = SDL_GetGamepadJoystick(gamepad);
    if (joystick == nullptr) {
        return false;
    }
    EnsureSensors(gamepad, chan);

    sample = {};
    const auto raw = [&](int index, uint32_t bit) {
        if (SDL_GetJoystickButton(joystick, index)) sample.hold |= bit;
    };
    raw(kRawA, kWpadA);
    raw(kRawB, kWpadB);
    raw(kRawOne, kWpadOne);
    raw(kRawTwo, kWpadTwo);
    raw(kRawPlus, kWpadPlus);
    raw(kRawMinus, kWpadMinus);
    raw(kRawHome, kWpadHome);
    raw(kRawDpadUp, kWpadUp);
    raw(kRawDpadDown, kWpadDown);
    raw(kRawDpadLeft, kWpadLeft);
    raw(kRawDpadRight, kWpadRight);

    float rawG[3] = {};
    const bool haveRaw = ReadSdlAccel(gamepad, SDL_SENSOR_ACCEL, rawG);
    for (float& v : rawG) v /= kStandardGravity;
    LastAcc& last = g_lastAcc[chan];
    const bool accepted = ReadAccelAsKpad(gamepad, SDL_SENSOR_ACCEL, sample.acc);
    if (accepted) {
        last.valid = true;
        for (int i = 0; i < 3; ++i) last.acc[i] = sample.acc[i];
    } else {
        // No sample this frame or a glitched one: repeat the last good reading
        // (rest pose, buttons up, until there is one).
        for (int i = 0; i < 3; ++i) sample.acc[i] = last.acc[i];
    }
    if (RuntimeConfigFile::WiiAccelTraceEnabled(false)) {
        TraceSample(chan, sample, rawG, haveRaw, accepted);
    }

    if (kind == Kind::RemoteWithClassic) {
        sample.hasClassic = true;
        // The driver posts the extension's buttons as joystick buttons numbered
        // by SDL_GAMEPAD_BUTTON_*: a/b/x/y by position (a on the east), +/-,
        // Home, the L/R clicks as shoulders, the D-pad as buttons 11-14 (never
        // through SDL's gamepad mapping, which expects a hat), ZL/ZR as the
        // trigger axes.
        const auto cl = [&](int index, uint32_t bit) {
            if (SDL_GetJoystickButton(joystick, index)) sample.clHold |= bit;
        };
        cl(SDL_GAMEPAD_BUTTON_EAST, kClA);
        cl(SDL_GAMEPAD_BUTTON_SOUTH, kClB);
        cl(SDL_GAMEPAD_BUTTON_NORTH, kClX);
        cl(SDL_GAMEPAD_BUTTON_WEST, kClY);
        cl(SDL_GAMEPAD_BUTTON_START, kClPlus);
        cl(SDL_GAMEPAD_BUTTON_BACK, kClMinus);
        cl(SDL_GAMEPAD_BUTTON_GUIDE, kClHome);
        cl(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, kClL);
        cl(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, kClR);
        cl(SDL_GAMEPAD_BUTTON_DPAD_UP, kClUp);
        cl(SDL_GAMEPAD_BUTTON_DPAD_DOWN, kClDown);
        cl(SDL_GAMEPAD_BUTTON_DPAD_LEFT, kClLeft);
        cl(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, kClRight);
        if (SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 0) sample.clHold |= kClZL;
        if (SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 0) sample.clHold |= kClZR;
        const Sint16 lx = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX);
        const Sint16 ly = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY);
        const Sint16 rx = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTX);
        const Sint16 ry = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTY);
        sample.clLStick[0] = std::clamp(static_cast<float>(lx) / 32767.0f, -1.0f, 1.0f);
        sample.clLStick[1] = std::clamp(-static_cast<float>(ly) / 32767.0f, -1.0f, 1.0f);
        sample.clRStick[0] = std::clamp(static_cast<float>(rx) / 32767.0f, -1.0f, 1.0f);
        sample.clRStick[1] = std::clamp(-static_cast<float>(ry) / 32767.0f, -1.0f, 1.0f);
        sample.clLStickRaw[0] = ClassicStickRaw(lx, false);
        sample.clLStickRaw[1] = ClassicStickRaw(ly, true);
        sample.clRStickRaw[0] = ClassicStickRaw(rx, false);
        sample.clRStickRaw[1] = ClassicStickRaw(ry, true);
        // Only the full-press click of L/R reaches SDL; report it as a full pull.
        sample.clTriggerL = (sample.clHold & kClL) ? 255 : 0;
        sample.clTriggerR = (sample.clHold & kClR) ? 255 : 0;
    }

    if (kind == Kind::RemoteWithNunchuk) {
        sample.hasNunchuk = true;
        if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) sample.hold |= kWpadC;
        if (SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 0) sample.hold |= kWpadZ;
        sample.stick[0] = static_cast<float>(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX)) / 32767.0f;
        // SDL's y grows downwards; KPAD's stick y is up-positive.
        sample.stick[1] = -static_cast<float>(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY)) / 32767.0f;
        for (float& v : sample.stick) v = std::clamp(v, -1.0f, 1.0f);
        LastAcc& lastNunchuk = g_lastNunchukAcc[chan];
        if (ReadAccelAsKpad(gamepad, SDL_SENSOR_ACCEL_L, sample.nunchukAcc)) {
            lastNunchuk.valid = true;
            for (int i = 0; i < 3; ++i) lastNunchuk.acc[i] = sample.nunchukAcc[i];
        } else {
            for (int i = 0; i < 3; ++i) sample.nunchukAcc[i] = lastNunchuk.acc[i];
        }
    }
    return true;
}

// Corrected SDL sample and KPAD vector of the remote on a port, for the overlay.
bool ReadAccelDebug(uint32_t chan, float sdlG[3], float kpadAcc[3]) {
    if (!IsRemoteChannel(chan)) {
        return false;
    }
    SDL_Gamepad* gamepad = SDL_GetGamepadFromPlayerIndex(static_cast<int>(chan));
    if (gamepad == nullptr) {
        return false;
    }
    EnsureSensors(gamepad, chan);
    if (!ReadAccelG(gamepad, SDL_SENSOR_ACCEL, sdlG)) {
        return false;
    }
    AccelGToKpad(sdlG, kpadAcc);
    return true;
}

// Begins collecting rest samples from the remote on `chan`.
void StartAccelCalibration(uint32_t chan) {
    if (!IsRemoteChannel(chan)) {
        FinishAccelCalibration("No Wii Remote on this port.");
        return;
    }
    g_calibration = {};
    g_calibration.active = true;
    g_calibration.chan = chan;
    g_calibrationMessage[0] = '\0';
}

// Drops the stored correction and goes back to SDL's raw reading.
void ClearAccelCalibration() {
    g_calibration.active = false;
    g_accelOffset = {};
    g_accelOffsetLoaded = true;
    RuntimeConfigFile::SetWiiAccelOffset({0.0, 0.0, 0.0});
    std::snprintf(g_calibrationMessage, sizeof(g_calibrationMessage), "Calibration cleared.");
}

// True while a calibration run is collecting samples.
bool IsAccelCalibrating() {
    return g_calibration.active;
}

// Share of the calibration samples collected so far, 0..1; 0 when idle.
float AccelCalibrationProgress() {
    return g_calibration.active ? static_cast<float>(g_calibration.count) / kCalibrationSamples : 0.0f;
}

// Outcome of the last calibration run for the overlay, or nullptr before any.
const char* AccelCalibrationMessage() {
    return g_calibrationMessage[0] != '\0' ? g_calibrationMessage : nullptr;
}

} // namespace WiiRemoteInput
