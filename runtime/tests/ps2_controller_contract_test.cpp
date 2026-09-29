#include "ps2_controller_contract.h"

#include <cassert>
#include <iostream>

// Independent reference from SLES_549.45 001D4C58 (+578..5F0), default
// control configuration. These are PS2 libpad bits, not GameCube masks.
constexpr Ps2Controller::Bindings kRetailPs2{
    0x2000, 0x4000, 0x1000, 0x8000, 0x10, 0x40, 0x80, 0x20,
    0x100000, 0x100, 0x200000, 0x800, 0x200, 4, 0x400, 6,
    0x800000, 0x400000, 0x100000, 0x200000,
    0xF000, 0, 0, 1, 0x1000, 0x800000, 0x400000,
    0x2000, 0x4000, 0x1000, 0x8000,
};

uint32_t Transport(uint32_t ps2) {
    // Hardware positions -> native PAD sample -> RDSPAF 80011F68 output.
    using Ps2Controller::Face;
    using Ps2Controller::FaceMask;
    constexpr auto battle = Ps2Controller::Context::Battle;
    constexpr std::array<uint32_t, 16> targets{
        Ps2Controller::kSelect, Ps2Controller::kL3, Ps2Controller::kR3, 0x1000,
        8, 2, 4, 1, 0x10, 0x1000040, Ps2Controller::kL1, 0x2000020,
        FaceMask(Face::Triangle, battle), FaceMask(Face::Circle, battle),
        FaceMask(Face::Cross, battle), FaceMask(Face::Square, battle),
    };
    uint32_t result = ps2 & 0xF00000;
    for (unsigned i = 0; i < targets.size(); ++i)
        if (ps2 & (1u << i)) result |= targets[i];
    return result;
}

int main() {
    using namespace Ps2Controller;
    // Exhaust every digital chord with neutral and all right-stick directions.
    // Catches interactions such as L3 alone incorrectly switching a fighter,
    // an R3 transform becoming a stick movement, and swapped raw face actions.
    uint32_t combinations = 0;
    for (uint32_t stick : {0u, 0x100000u, 0x200000u, 0x400000u, 0x800000u}) {
        for (uint32_t digital = 0; digital <= 0xFFFF; ++digital) {
            const uint32_t raw = digital | stick;
            assert(MapActions(raw, kRetailPs2) == MapActions(Transport(raw), kPs2Bindings));
            ++combinations;
        }
    }
    assert((MapActions(kL3, kPs2Bindings) & (1u << 15)) == 0);
    assert((MapActions(kR3, kPs2Bindings) & (1u << 15)) == 0);
    assert((MapActions(kL3 | kR3, kPs2Bindings) & (1u << 15)) != 0);
    assert((MapActions(kSelect, kPs2Bindings) & (1u << 23)) != 0);
    assert((MapActions(0x1000, kPs2Bindings) & (1u << 23)) == 0);
    assert((kExtraButtons & 0x1F7F) == 0); // no collision with real GC wire bits
    // Retail context is Battle only while the battle cycle exists, the fighter
    // state is actually playable, and the pause overlay is not active.
    for (bool battleCycle : {false, true}) {
        for (bool battlePlayable : {false, true}) {
            for (bool pause : {false, true}) {
                const Context expected = battleCycle && battlePlayable && !pause
                    ? Context::Battle : Context::Frontend;
                assert(ContextForBattleState(battleCycle, battlePlayable, pause) == expected);
            }
        }
    }
    // PAL frontend keeps Cross for confirm and Triangle for back. Battle's
    // Square attack and Cross dash must not leak into that menu convention.
    assert(FaceMask(Face::Cross, Context::Frontend) == 0x100);
    assert(FaceMask(Face::Triangle, Context::Frontend) == 0x200);
    assert(FaceMask(Face::Circle, Context::Frontend) == 0x400);
    assert(FaceMask(Face::Square, Context::Frontend) == 0x800);
    // Every menu face has one distinct binding and the glyph inverse is exact.
    for (Context context : {Context::Frontend, Context::Battle}) {
        uint16_t used = 0;
        for (Face face : {Face::Cross, Face::Circle, Face::Square, Face::Triangle}) {
            const auto mask = FaceMask(face, context);
            assert((used & mask) == 0);
            used |= mask;
        }
        assert(used == 0xF00);
    }

    // Recovery is a contextual exception to the shared GC-A animated row.
    // Never turn resource040 into Cross globally: only a proven reaction state
    // during active battle may substitute the Cross animated metadata.
    for (uint32_t state : {0x22u, 0x24u, 0x26u, 0x29u, 0x2Au,
                           0x2Bu, 0x2Du, 0x2Eu, 0x32u, 0x35u}) {
        assert(IsRecoveryReactionState(state));
        assert(RecoveryPromptMetadata(kWiiAAnimMetadata, state, Context::Battle) ==
               kWiiCrossAnimMetadata);
        assert(RecoveryPromptMetadata(kWiiAAnimMetadata, state, Context::Frontend) ==
               kWiiAAnimMetadata);
        assert(RecoveryPromptMetadata(0x80371244u, state, Context::Battle) == 0x80371244u);
    }
    for (uint32_t state : {0u, 1u, 0x21u, 0x23u, 0x28u, 0x2Cu, 0x33u, 0x43u}) {
        assert(!IsRecoveryReactionState(state));
        assert(RecoveryPromptMetadata(kWiiAAnimMetadata, state, Context::Battle) ==
               kWiiAAnimMetadata);
    }

    // Pause input stays Frontend, while the embedded battle-action description
    // renders battle semantics. The scope must not leak into ordinary menu text.
    assert(ContextForBattleState(true, true, true) == Context::Frontend);
    assert(PromptContextForRender(Context::Frontend, true) == Context::Battle);
    assert(PromptContextForRender(Context::Frontend, false) == Context::Frontend);
    assert(PromptContextForRender(Context::Battle, true) == Context::Battle);

    // Context transitions suppress only faces that were already held before the
    // boundary. The new context is active immediately, so a fresh face press is
    // still visible while an older face tail is waiting for release.
    ContextGate gate;
    BeginContextSample(gate, Context::Battle);
    assert(MapContextFaces(gate, 0) == 0);
    EndContextSample(gate);
    assert(gate.active == Context::Battle);

    const uint16_t crossBattle = FaceMask(Face::Cross, Context::Battle);
    const uint16_t crossFrontend = FaceMask(Face::Cross, Context::Frontend);
    const uint16_t squareBattle = FaceMask(Face::Square, Context::Battle);
    const uint16_t squareFrontend = FaceMask(Face::Square, Context::Frontend);
    BeginContextSample(gate, Context::Battle);
    assert(MapContextFaces(gate, crossBattle | 0x0008) == (crossBattle | 0x0008));
    EndContextSample(gate);

    BeginContextSample(gate, Context::Frontend);
    assert(MapContextFaces(gate, crossBattle | 0x0008) == 0x0008);
    EndContextSample(gate);
    assert(gate.active == Context::Frontend);

    // A different face pressed after the boundary is not part of the old held
    // tail and must use frontend semantics immediately.
    BeginContextSample(gate, Context::Frontend);
    assert(MapContextFaces(gate, crossBattle | squareBattle | 0x0008) == (squareFrontend | 0x0008));
    EndContextSample(gate);

    BeginContextSample(gate, Context::Frontend);
    assert(MapContextFaces(gate, squareBattle) == squareFrontend);
    EndContextSample(gate);

    BeginContextSample(gate, Context::Frontend);
    assert(MapContextFaces(gate, 0) == 0);
    EndContextSample(gate);
    BeginContextSample(gate, Context::Frontend);
    assert(MapContextFaces(gate, crossBattle) == crossFrontend);
    EndContextSample(gate);

    // Gates are per port. Player 1 holding Cross across Frontend/Battle cannot
    // suppress a fresh Cross from player 2 on the first Battle sample.
    ContextGate p1;
    ContextGate p2;
    BeginContextSample(p1, Context::Frontend);
    assert(MapContextFaces(p1, crossBattle) == crossFrontend);
    EndContextSample(p1);
    BeginContextSample(p2, Context::Frontend);
    assert(MapContextFaces(p2, 0) == 0);
    EndContextSample(p2);

    BeginContextSample(p1, Context::Battle);
    BeginContextSample(p2, Context::Battle);
    assert(MapContextFaces(p1, crossBattle) == 0);
    assert(MapContextFaces(p2, crossBattle) == crossBattle);
    EndContextSample(p1);
    EndContextSample(p2);

    // A port not sampled in a frame is neutral for tail purposes. This prevents
    // a disconnected controller from carrying stale suppression into reconnect.
    BeginContextSample(p1, Context::Frontend);
    EndContextSample(p1);
    assert(p1.previousFaces == 0);
    assert(p1.suppressedFaces == 0);

    std::cout << "PS2 action parity: " << combinations
              << " digital/stick combinations, clicks, Select, face contexts and per-port held-tail gates passed\n";
}
