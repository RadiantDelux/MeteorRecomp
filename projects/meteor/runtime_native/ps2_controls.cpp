#include "abi_bridge.h"
#include "memory.h"
#include "runtime_config.h"
#include "wii_remote_input.h"
#include "RuntimeConfig.h"

#include <cmath>

extern "C" void func_80041474(CpuContext* cpu);
extern "C" void func_80011F68(CpuContext* cpu);
extern "C" void func_800E2B88(CpuContext* cpu);
extern "C" void func_80108970(CpuContext* cpu);
extern "C" void func_8010F1D0(CpuContext* cpu);
extern "C" void func_8019A288(CpuContext* cpu);
extern "C" void func_8019B724(CpuContext* cpu);

namespace {
thread_local bool g_ps2RecoveryHudDrawScope = false;

bool Ps2PresetEnabled() {
    return RuntimeConfig::GAME_CODE == 0x52445350u &&
           RuntimeConfigFile::ControllerPromptStyle("gamecube") == "ps2";
}

bool IsPs2FacePromptResource(uint16_t resource) {
    return (resource >= 17u && resource <= 24u) ||
           (resource >= 37u && resource <= 44u) ||
           resource == 46u || resource == 48u;
}

struct Ps2PromptGeometryRestore {
    uint32_t descriptor = 0;
    uint16_t x0 = 0;
    uint16_t x1 = 0;
    uint16_t y0 = 0;
    uint16_t y1 = 0;
    bool changed = false;
};

Ps2PromptGeometryRestore NormalizePs2FacePromptGeometry(uint32_t descriptor) {
    Ps2PromptGeometryRestore restore{};
    if (!descriptor || !Memory::Contains(descriptor, 0x1Cu)) return restore;

    const uint16_t baseResource = Memory::Read16(descriptor + 4u);
    if ((baseResource & 0x8000u) != 0u) return restore;
    const uint16_t resourceDelta = Memory::Read16(descriptor + 6u);
    const uint32_t finalResourceWide = static_cast<uint32_t>(baseResource) + resourceDelta;
    if (finalResourceWide > 0xFFFFu) return restore;
    const uint16_t finalResource = static_cast<uint16_t>(finalResourceWide);
    if (!IsPs2FacePromptResource(baseResource) && !IsPs2FacePromptResource(finalResource)) {
        return restore;
    }

    const int32_t x0 = static_cast<int16_t>(Memory::Read16(descriptor + 8u));
    const int32_t x1 = static_cast<int16_t>(Memory::Read16(descriptor + 10u));
    const int32_t y0 = static_cast<int16_t>(Memory::Read16(descriptor + 12u));
    const int32_t y1 = static_cast<int16_t>(Memory::Read16(descriptor + 14u));
    const int32_t u0 = static_cast<int16_t>(Memory::Read16(descriptor + 16u));
    const int32_t u1 = static_cast<int16_t>(Memory::Read16(descriptor + 18u));
    const int32_t v0 = static_cast<int16_t>(Memory::Read16(descriptor + 20u));
    const int32_t v1 = static_cast<int16_t>(Memory::Read16(descriptor + 22u));

    const int32_t quadW = x1 >= x0 ? x1 - x0 : x0 - x1;
    const int32_t quadH = y1 >= y0 ? y1 - y0 : y0 - y1;
    const int32_t uvW = u1 >= u0 ? u1 - u0 : u0 - u1;
    const int32_t uvH = v1 >= v0 ? v1 - v0 : v0 - v1;
    if (quadW <= 0 || quadH <= 0 || uvW <= 0 || uvH <= 0) return restore;

    // One texture texel should have the same presentation scale on X and Y.
    // Several battle-HUD face descriptors violate that for the Nintendo source
    // art (for example 22x46 over a 21x32 UV rectangle), which turns the square
    // PS2 shell into a visibly tall/narrow oval. Preserve quad area and center,
    // but remove only that anisotropic scale. Already-isotropic slots are left
    // byte-for-byte untouched.
    const int64_t lhs = static_cast<int64_t>(quadW) * uvH;
    const int64_t rhs = static_cast<int64_t>(quadH) * uvW;
    const int64_t larger = lhs > rhs ? lhs : rhs;
    const int64_t diff = lhs > rhs ? lhs - rhs : rhs - lhs;
    if (larger == 0 || diff * 100 <= larger * 5) return restore;

    int32_t newW = 0;
    int32_t newH = 0;
    if (g_ps2RecoveryHudDrawScope && finalResource == 44u) {
        // The recovery field has a tight horizontal clip region. The generic
        // area-preserving correction widens its 22x46 quad to 26x39, which is
        // enough to shave the Cross at the sides. Keep the retail field width
        // exactly and correct only Y so one UV texel has the same X/Y scale:
        // 22 * 32 / 21 = 33.52 -> 34.
        newW = quadW;
        newH = std::max<int32_t>(1, static_cast<int32_t>(std::lround(
            static_cast<double>(quadW) * static_cast<double>(uvH) /
            static_cast<double>(uvW))));
    } else {
        const double area = static_cast<double>(quadW) * static_cast<double>(quadH);
        newW = std::max<int32_t>(1, static_cast<int32_t>(std::lround(
            std::sqrt(area * static_cast<double>(uvW) / static_cast<double>(uvH)))));
        newH = std::max<int32_t>(1, static_cast<int32_t>(std::lround(
            std::sqrt(area * static_cast<double>(uvH) / static_cast<double>(uvW)))));
    }
    if (newW > 0x7FFF || newH > 0x7FFF) return restore;

    auto centered = [](int32_t a, int32_t b, int32_t span, int32_t& outA, int32_t& outB) {
        const bool forward = b >= a;
        const double center = (static_cast<double>(a) + static_cast<double>(b)) * 0.5;
        const int32_t low = static_cast<int32_t>(std::lround(center - span * 0.5));
        const int32_t high = low + span;
        outA = forward ? low : high;
        outB = forward ? high : low;
    };

    int32_t newX0 = 0;
    int32_t newX1 = 0;
    int32_t newY0 = 0;
    int32_t newY1 = 0;
    centered(x0, x1, newW, newX0, newX1);
    centered(y0, y1, newH, newY0, newY1);
    if (newX0 < -0x8000 || newX0 > 0x7FFF || newX1 < -0x8000 || newX1 > 0x7FFF ||
        newY0 < -0x8000 || newY0 > 0x7FFF || newY1 < -0x8000 || newY1 > 0x7FFF) {
        return restore;
    }

    restore.descriptor = descriptor;
    restore.x0 = Memory::Read16(descriptor + 8u);
    restore.x1 = Memory::Read16(descriptor + 10u);
    restore.y0 = Memory::Read16(descriptor + 12u);
    restore.y1 = Memory::Read16(descriptor + 14u);
    restore.changed = true;
    Memory::Write16(descriptor + 8u, static_cast<uint16_t>(static_cast<int16_t>(newX0)));
    Memory::Write16(descriptor + 10u, static_cast<uint16_t>(static_cast<int16_t>(newX1)));
    Memory::Write16(descriptor + 12u, static_cast<uint16_t>(static_cast<int16_t>(newY0)));
    Memory::Write16(descriptor + 14u, static_cast<uint16_t>(static_cast<int16_t>(newY1)));

    if (std::getenv("METEOR_PS2_RECOVERY_PROBE") != nullptr) {
        RT_LOGF(RT_TAG_CONFIG,
                "PS2 face geometry: resource=%u/%u quad=%dx%d uv=%dx%d -> quad=%dx%d descriptor=0x%08X\n",
                static_cast<unsigned>(baseResource), static_cast<unsigned>(finalResource),
                quadW, quadH, uvW, uvH, newW, newH, descriptor);
    }
    return restore;
}

void RestorePs2FacePromptGeometry(const Ps2PromptGeometryRestore& restore) {
    if (!restore.changed || !Memory::Contains(restore.descriptor + 8u, 8u)) return;
    Memory::Write16(restore.descriptor + 8u, restore.x0);
    Memory::Write16(restore.descriptor + 10u, restore.x1);
    Memory::Write16(restore.descriptor + 12u, restore.y0);
    Memory::Write16(restore.descriptor + 14u, restore.y1);
}

void Meteor_DrawPs2FaceSprite(CpuContext* cpu) {
    const Ps2PromptGeometryRestore restore =
        Ps2PresetEnabled() ? NormalizePs2FacePromptGeometry(cpu->gpr[3]) : Ps2PromptGeometryRestore{};
    func_80108970(cpu);
    RestorePs2FacePromptGeometry(restore);
}

void ForcePs2TrainingControllerStyle() {
    if (!Ps2PresetEnabled()) return;

    // RDSPAF globals (SDA1 0x80631320): training object at r13-0x6860,
    // scene state at r13-0x6D30. Ultimate Training uses local style 0/1/2
    // (Remote/Classic/GC); PS2 deliberately follows the GC/PAD instruction
    // family without spoofing the game's global controller-family state.
    constexpr uint32_t kTrainingPointer = 0x8062AAC0u;
    constexpr uint32_t kScenePointer = 0x8062A5F0u;
    if (Memory::Contains(kTrainingPointer, sizeof(uint32_t))) {
        const uint32_t training = Memory::Read32(kTrainingPointer);
        if (training && Memory::Contains(training + 0x480u, sizeof(uint32_t))) {
            Memory::Write32(training + 0x480u, 2u);
        }
    }
    if (Memory::Contains(kScenePointer, sizeof(uint32_t))) {
        const uint32_t scene = Memory::Read32(kScenePointer);
        if (scene && Memory::Contains(scene + 0x7DCu, sizeof(uint32_t))) {
            Memory::Write32(scene + 0x7DCu, 2u);
        }
    }
}

void Meteor_InitControllerActions(CpuContext* cpu) {
    const uint32_t fighter = cpu->gpr[3];
    func_80041474(cpu);
    WiiRemoteInput::RegisterPlayStation2Fighter(fighter);
}

void Meteor_NormalizeGamecubePad(CpuContext* cpu) {
    const uint32_t port = cpu->gpr[3];
    func_80011F68(cpu);
    WiiRemoteInput::CompletePlayStation2PadNormalization(port);
}

void Meteor_DrawBattleActionDescription(CpuContext* cpu) {
    // HEADLESS call-graph/dataflow: 800E2B88 is reached only from the type-3
    // rich description branch in 800DC9CC. During pause the normal PS2 input
    // context is correctly Frontend, but this text still documents battle
    // actions. Scope only the visual token table to Battle for the synchronous
    // renderer body, then restore the pause/menu profile immediately.
    const bool scoped = Ps2PresetEnabled();
    if (scoped) WiiRemoteInput::BeginPlayStation2BattlePromptScope();
    func_800E2B88(cpu);
    if (scoped) WiiRemoteInput::EndPlayStation2BattlePromptScope();
}

void Meteor_DrawRecoveryHudGroup(CpuContext* cpu) {
    // Headless decompilation + live table inspection identify 8010F1D0 as the
    // callback that owns the exact HUD descriptor row using resource040:
    //   *(r13-0x6A40) -> object, object+4 -> descriptor array,
    //   descriptor[1] (+0x1C), resource index at row+4 == 40.
    // 8010F1D0 is installed as this HUD group's callback by 801106C0 and draws
    // descriptor[1] through 80108970.  Patch that semantic owner rather than
    // guessing from pause/global battle state; restore immediately afterward so
    // every other resource040 use (normal Square/melee) remains unchanged.
    constexpr uint16_t kSquareResource = 40u;
    constexpr uint16_t kCrossResource = 44u;
    const uint32_t r13 = cpu->gpr[13];
    if (!Ps2PresetEnabled() || r13 < 0x6A40u) {
        func_8010F1D0(cpu);
        return;
    }

    const uint32_t hudSlot = r13 - 0x6A40u;
    if (!Memory::Contains(hudSlot, sizeof(uint32_t))) {
        func_8010F1D0(cpu);
        return;
    }
    const uint32_t hud = Memory::Read32(hudSlot);
    if (!hud || !Memory::Contains(hud + 4u, sizeof(uint32_t))) {
        func_8010F1D0(cpu);
        return;
    }
    const uint32_t descriptors = Memory::Read32(hud + 4u);
    constexpr uint32_t kRowStride = 0x1Cu;
    const uint32_t recoveryRow = descriptors + kRowStride;
    if (!descriptors || !Memory::Contains(recoveryRow + 4u, 4u) ||
        Memory::Read16(recoveryRow + 4u) != kSquareResource ||
        Memory::Read16(recoveryRow + 6u) != 0u) {
        func_8010F1D0(cpu);
        return;
    }

    Memory::Write16(recoveryRow + 4u, kCrossResource);
    const bool previousRecoveryHudScope = g_ps2RecoveryHudDrawScope;
    g_ps2RecoveryHudDrawScope = true;
    func_8010F1D0(cpu);
    g_ps2RecoveryHudDrawScope = previousRecoveryHudScope;
    Memory::Write16(recoveryRow + 4u, kSquareResource);

    if (std::getenv("METEOR_PS2_RECOVERY_PROBE") != nullptr) {
        RT_LOG(RT_TAG_CONFIG)
            << "PS2 recovery HUD callback: descriptor[1] resource040 -> resource044 (Cross)"
            << std::endl;
    }
}

void Meteor_AdjustTrainingControllerStyle(CpuContext* cpu) {
    if (Ps2PresetEnabled() && Memory::Contains(0x8062AAC0u, sizeof(uint32_t))) {
        const uint32_t training = Memory::Read32(0x8062AAC0u);
        if (training && Memory::Contains(training + 0x480u, 0x18u) &&
            Memory::Read32(training + 0x490u) == 2u) {
            Memory::Write32(training + 0x480u, 2u);
            // Retail maps r3=0 to -1, r3=1 to +1, and every other value to 0.
            // Passing 2 keeps style=GC while retaining the original animation
            // and instruction refresh performed by the retail body.
            cpu->gpr[3] = 2u;
        }
    }
    func_8019A288(cpu);
    ForcePs2TrainingControllerStyle();
}

void Meteor_InitTrainingControllerStyle(CpuContext* cpu) {
    // The retail body copies scene+0x7DC into training+0x480 in two paths, so
    // seed both before entry and reaffirm the local field after it returns.
    ForcePs2TrainingControllerStyle();
    func_8019B724(cpu);
    ForcePs2TrainingControllerStyle();
}
}

// Run the original bodies, then extend only the input contract. The original
// action/edge/hold/double-tap code remains responsible for interpreting presses.
REGISTER_TITLE_NATIVE_FUNCTION_AS(0x80041474, Meteor_InitControllerActions,
                                  "Meteor_InitControllerActions");
REGISTER_TITLE_NATIVE_FUNCTION_AS(0x80011F68, Meteor_NormalizeGamecubePad,
                                  "Meteor_NormalizeGamecubePad");
REGISTER_TITLE_NATIVE_FUNCTION_AS(0x800E2B88, Meteor_DrawBattleActionDescription,
                                  "Meteor_DrawBattleActionDescription");
REGISTER_TITLE_NATIVE_FUNCTION_AS(0x80108970, Meteor_DrawPs2FaceSprite,
                                  "Meteor_DrawPs2FaceSprite");
REGISTER_TITLE_NATIVE_FUNCTION_AS(0x8010F1D0, Meteor_DrawRecoveryHudGroup,
                                  "Meteor_DrawRecoveryHudGroup");
REGISTER_TITLE_NATIVE_FUNCTION_AS(0x8019A288, Meteor_AdjustTrainingControllerStyle,
                                  "Meteor_AdjustTrainingControllerStyle");
REGISTER_TITLE_NATIVE_FUNCTION_AS(0x8019B724, Meteor_InitTrainingControllerStyle,
                                  "Meteor_InitTrainingControllerStyle");
