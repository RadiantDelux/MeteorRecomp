#pragma once

#include <cstdint>

#include "loop_service_cadence.h"

struct CpuContext;

void Meteor_RuntimeLoopCheckpoint(uint32_t guestPc, CpuContext* ctx) noexcept;
bool Meteor_RuntimeLoopCheckpointRequired(uint32_t guestPc) noexcept;
void Meteor_RuntimeCallCheckpoint(uint32_t target, CpuContext* ctx) noexcept;
void Meteor_RuntimeReturnCheckpoint(uint32_t target, CpuContext* ctx) noexcept;

struct MeteorLoopCheckpointGateState {
    uint64_t serial = 0u;
    uint64_t armedSerial = 0u;
    uint32_t armedPc = 0u;
};

inline thread_local MeteorLoopCheckpointGateState g_meteorLoopCheckpointGateState{};

inline bool Meteor_RuntimeLoopCheckpointShouldRun(uint32_t guestPc) noexcept {
    auto& gate = g_meteorLoopCheckpointGateState;
    const uint64_t serial = ++gate.serial;
    const bool required = meteor::NeedsLoopCheckpoint(serial, guestPc, false);
    if (required) {
        gate.armedSerial = serial;
        gate.armedPc = guestPc;
    }
    return required;
}

// Production gameplay has no functional work at generic translated-call
// boundaries. Historical bring-up tracing observed roughly two million of
// these boundaries per second, so even an out-of-line early-return checkpoint
// is avoidable host overhead. Asynchronous PE/audio/VI/alarm/IOS service lives
// in the loop checkpoint below instead.
#define WIICOMPILED_APPLY_TITLE_RUNTIME_CALL_OPTIONS(Target, Context) \
    do { } while (0)

// The only production return-side action is the rare Wii-language -> PAL
// resource-bank result at 0x80027780. Keep the comparison inline so direct
// translated calls to every other compile-time target fold to a true no-op.
#define WIICOMPILED_APPLY_TITLE_RUNTIME_RETURN_OPTIONS(Target, Context) \
    do { \
        if ((Target) == 0x80027780u) { \
            Meteor_RuntimeReturnCheckpoint((Target), (Context)); \
        } \
    } while (0)

// RDSPAF's OSCreateThread must create a host fiber, and SelectThread must perform
// the non-local host-fiber transfer that a translated C++ stack cannot model.
// Keep Resume/Suspend/Sleep translated for now: their guest queue semantics are
// valid and they naturally funnel real switches through SelectThread. This also
// avoids replacing unrelated early SDK sleeps before the worker pool is alive.
#define WIICOMPILED_FORCE_DYNAMIC_DIRECT_CALL(Target) \
    ((Target) == 0x80004338u || \
     (Target) == 0x80004388u || \
     (Target) == 0x80011F68u || \
     (Target) == 0x80041474u || \
     (Target) == 0x800E2B88u || \
     (Target) == 0x80108970u || \
     (Target) == 0x8019A288u || \
     (Target) == 0x8019B724u || \
     (Target) == 0x80209E24u || \
     (Target) == 0x80210990u || \
     (Target) == 0x80210BD0u || \
     (Target) == 0x8023C3DCu || \
     (Target) == 0x80257810u || \
     (Target) == 0x80231294u || \
     (Target) == 0x80231344u)

// RDSPAF's translated SDK scheduler can idle without crossing a call boundary.
// The title binding identifies that validated guest backedge; the actual wait
// service remains generic Wii runtime behavior.
#define WIICOMPILED_APPLY_TITLE_RUNTIME_LOOP_OPTIONS(GuestPc, Context) \
    Meteor_RuntimeLoopCheckpoint((GuestPc), (Context))

#define WIICOMPILED_SHOULD_APPLY_TITLE_RUNTIME_LOOP_OPTIONS(GuestPc) \
    Meteor_RuntimeLoopCheckpointShouldRun((GuestPc))

#define MKW_APPLY_TITLE_RUNTIME_LOOP_OPTIONS(GuestPc, Context) \
    WIICOMPILED_APPLY_TITLE_RUNTIME_LOOP_OPTIONS((GuestPc), (Context))

#define MKW_APPLY_TITLE_RUNTIME_CALL_OPTIONS(Target, Context) \
    WIICOMPILED_APPLY_TITLE_RUNTIME_CALL_OPTIONS((Target), (Context))
