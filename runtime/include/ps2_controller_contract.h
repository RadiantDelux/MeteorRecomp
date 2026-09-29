#pragma once

#include <array>
#include <cstdint>

// RDSPAF GameCube transport for the PAL SLES_549.45 default control layout.
// The PS2 art and the PS2 input preset share this face-action contract. The
// game itself still receives ordinary GameCube PAD bits; PS2-only controls are
// translated to the Wii action that retail code already understands.
namespace Ps2Controller {
// Reserved in the 16-bit GameCube wire format. They are kept only in the host
// PAD sample until input blocking has run, then carried beside the SI packet.
constexpr uint16_t kL1 = 0x0080;
constexpr uint16_t kSelect = 0x2000;
constexpr uint16_t kL3 = 0x4000;
constexpr uint16_t kR3 = 0x8000;
constexpr uint16_t kExtraButtons = kL1 | kSelect | kL3 | kR3;
enum class Face : uint8_t { Cross, Circle, Square, Triangle };
enum class Context : uint8_t { Frontend, Battle };
constexpr uint16_t kFaceButtons = 0x0F00;

// RDSPAF style3 controller-token metadata used by the renderer. The recovery
// HUD reuses the ordinary GC A animated row (resource040). PAL PS2 shows Cross
// for that recovery prompt, whose matching animated artwork is resource044 in
// the Wii token table -> real PS2 resident_system 025 in the prompt pack.
constexpr uint32_t kWiiAAnimMetadata = 0x80371250u;
constexpr uint32_t kWiiCircleAnimMetadata = 0x80371238u;
constexpr uint32_t kWiiTriangleAnimMetadata = 0x80371268u;
constexpr uint32_t kWiiCrossAnimMetadata = 0x80371280u;
// Composite "press/charge" rows. These occur in battle-move descriptions
// (e.g. Square+press Perfect Smash and charged Triangle sequences), so their
// semantics stay battle-relative even when the prose is displayed in a menu.
constexpr uint32_t kWiiAPressMetadata = 0x80371298u;
constexpr uint32_t kWiiYPressMetadata = 0x803712B0u;

// Pause/menu input must keep frontend semantics, but an embedded battle-action
// description still documents the controls used in combat. Keep those two
// contexts independent instead of changing the whole pause menu to Battle.
constexpr Context PromptContextForRender(Context inputContext, bool battleActionDescription) {
    return battleActionDescription ? Context::Battle : inputContext;
}

constexpr bool IsRecoveryReactionState(uint32_t state) {
    switch (state) {
    case 0x22:
    case 0x24:
    case 0x26:
    case 0x29:
    case 0x2A:
    case 0x2B:
    case 0x2D:
    case 0x2E:
    case 0x32:
    case 0x35:
        return true;
    default:
        return false;
    }
}

constexpr uint32_t RecoveryPromptMetadata(uint32_t metadata, uint32_t fighterState,
                                          Context context) {
    return context == Context::Battle && metadata == kWiiAAnimMetadata &&
                   IsRecoveryReactionState(fighterState)
               ? kWiiCrossAnimMetadata
               : metadata;
}

constexpr Context ContextForBattleState(bool battleCycle, bool battlePlayable, bool pause) {
    return battleCycle && battlePlayable && !pause ? Context::Battle : Context::Frontend;
}

constexpr uint16_t FaceMask(Face face, Context context) {
    constexpr std::array<uint16_t, 4> battle{0x0200, 0x0400, 0x0100, 0x0800};
    constexpr std::array<uint16_t, 4> frontend{0x0100, 0x0400, 0x0800, 0x0200};
    return (context == Context::Battle ? battle : frontend)[static_cast<unsigned>(face)];
}

// The Wii title changes the logical meaning of the four face buttons between
// gameplay and frontend/pause UI. A face already held when the context changes
// must not reappear under its new meaning and manufacture a fresh guest edge.
// Track that held tail by physical face position. The new context becomes active
// immediately, so another face pressed after the boundary (or another player's
// independent gate) is not swallowed while the old tail waits for release.
struct ContextGate {
    Context active = Context::Frontend;
    uint16_t previousFaces = 0;
    uint16_t suppressedFaces = 0;
    bool sampled = false;
};

inline void BeginContextSample(ContextGate& gate, Context desired) {
    gate.sampled = false;
    if (desired != gate.active) {
        gate.suppressedFaces |= gate.previousFaces;
        gate.active = desired;
    }
}

inline uint16_t MapContextFaces(ContextGate& gate, uint16_t battleMappedButtons) {
    const uint16_t physicalFaces = battleMappedButtons & kFaceButtons;
    gate.sampled = true;
    gate.suppressedFaces &= physicalFaces;
    gate.previousFaces = physicalFaces;

    uint16_t result = battleMappedButtons & ~kFaceButtons;
    const uint16_t visibleFaces = physicalFaces & ~gate.suppressedFaces;

    for (unsigned i = 0; i < 4; ++i) {
        const Face face = static_cast<Face>(i);
        if ((visibleFaces & FaceMask(face, Context::Battle)) != 0) {
            result |= FaceMask(face, gate.active);
        }
    }
    return result;
}

inline void EndContextSample(ContextGate& gate) {
    if (!gate.sampled) {
        gate.previousFaces = 0;
        gate.suppressedFaces = 0;
    }
}

// Original RDSPAF 80041474, fighter+680..6F8. 80040C3C consumes 31 masks;
// action 15 is an ALL-button chord, all other actions use ANY matching bit.
constexpr uint32_t kBindingOffset = 0x680;
using Bindings = std::array<uint32_t, 31>;
constexpr Bindings kGamecubeBindings{
    0x400, 0x200, 0x800, 0x100, 8, 4, 1, 2,
    0x100000, 0x10, 0x200000, 0x2000000, 0x1000000,
    0x400000, 0x300000, 0x800000,
    0x800000, 0x400000, 0x100000, 0x200000,
    0xF00, 0x20, 0x40, 0, 0x800, 0x100000, 0x200000,
    0x100, 0x200, 0x400, 0x800,
};

// SLES_549.45 001D4C58, fighter+578..5F0, expressed in the same
// normalized GameCube transport as above. Axes stay axes; clicks stay clicks.
// Raw PS2 face actions 27..30 are Circle/Cross/Triangle/Square, independent
// of the configurable combat actions 0..3. This also fixes recovery input
// without changing the entire face layout when a fighter changes state.
constexpr Bindings kPs2Bindings{
    0x400, 0x200, 0x800, 0x100, 8, 4, 1, 2,
    0x100000, 0x10, 0x200000, 0x2000000, 0x1000000,
    kR3, kL1, kL3 | kR3,
    0x800000, 0x400000, 0x100000, 0x200000,
    0xF00, 0, 0, kSelect, 0x800, 0x800000, 0x400000,
    0x400, 0x200, 0x800, 0x100,
};

constexpr uint32_t MapActions(uint32_t buttons, const Bindings& bindings) {
    uint32_t result = 0;
    for (unsigned i = 0; i < bindings.size(); ++i) {
        const bool down = i == 15 ? (buttons & bindings[i]) == bindings[i]
                                  : (buttons & bindings[i]) != 0;
        if (down) result |= uint32_t{1} << i;
    }
    return result;
}
}
