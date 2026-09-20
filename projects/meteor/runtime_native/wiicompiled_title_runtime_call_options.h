#pragma once

#include <cstdint>

struct CpuContext;

void Meteor_RuntimeLoopCheckpoint(uint32_t guestPc, CpuContext* ctx) noexcept;
bool Meteor_RuntimeLoopCheckpointRequired(uint32_t guestPc) noexcept;
void Meteor_RuntimeCallCheckpoint(uint32_t target, CpuContext* ctx) noexcept;
void Meteor_RuntimeReturnCheckpoint(uint32_t target, CpuContext* ctx) noexcept;

#define WIICOMPILED_APPLY_TITLE_RUNTIME_CALL_OPTIONS(Target, Context) \
    Meteor_RuntimeCallCheckpoint((Target), (Context))

#define WIICOMPILED_APPLY_TITLE_RUNTIME_RETURN_OPTIONS(Target, Context) \
    Meteor_RuntimeReturnCheckpoint((Target), (Context))

// RDSPAF's OSCreateThread must create a host fiber, and SelectThread must perform
// the non-local host-fiber transfer that a translated C++ stack cannot model.
// Keep Resume/Suspend/Sleep translated for now: their guest queue semantics are
// valid and they naturally funnel real switches through SelectThread. This also
// avoids replacing unrelated early SDK sleeps before the worker pool is alive.
#define WIICOMPILED_FORCE_DYNAMIC_DIRECT_CALL(Target) \
    ((Target) == 0x80004338u || \
     (Target) == 0x80004388u || \
     (Target) == 0x80209E24u || \
     (Target) == 0x80210990u || \
     (Target) == 0x80210BD0u || \
     (Target) == 0x80231294u || \
     (Target) == 0x80231344u)

// RDSPAF's translated SDK scheduler can idle without crossing a call boundary.
// The title binding identifies that validated guest backedge; the actual wait
// service remains generic Wii runtime behavior.
#define WIICOMPILED_APPLY_TITLE_RUNTIME_LOOP_OPTIONS(GuestPc, Context) \
    Meteor_RuntimeLoopCheckpoint((GuestPc), (Context))

#define WIICOMPILED_SHOULD_APPLY_TITLE_RUNTIME_LOOP_OPTIONS(GuestPc) \
    Meteor_RuntimeLoopCheckpointRequired((GuestPc))

#define MKW_APPLY_TITLE_RUNTIME_LOOP_OPTIONS(GuestPc, Context) \
    WIICOMPILED_APPLY_TITLE_RUNTIME_LOOP_OPTIONS((GuestPc), (Context))

#define MKW_APPLY_TITLE_RUNTIME_CALL_OPTIONS(Target, Context) \
    WIICOMPILED_APPLY_TITLE_RUNTIME_CALL_OPTIONS((Target), (Context))
