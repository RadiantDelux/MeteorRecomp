#pragma once

#include <cstdint>

namespace meteor {

constexpr bool IsSchedulerIdleLoop(uint32_t pc) noexcept {
    return pc == 0x80210ADCu || pc == 0x80210AD8u;
}

// PC zero is the shared native scheduler's idle checkpoint. Other than that
// and the two translated idle backedges, service runs at a multiple of 256.
// Trace mode retains every diagnostic observation.
constexpr bool NeedsLoopCheckpoint(uint64_t serial, uint32_t pc, bool trace) noexcept {
    return trace || pc == 0u || IsSchedulerIdleLoop(pc) || (serial & 0xFFu) == 0u;
}

class LoopServiceCadence {
public:
    struct Due {
        bool audio;
        bool async;
    };

    // Retain the original instruction-count service opportunities, but also
    // poll by elapsed host time. An idle backedge sleeps for up to 1 ms, so
    // waiting 8192/16384 such backedges can otherwise starve hardware timers.
    // This only polls devices: each device still owns its actual IRQ deadline.
    Due Poll(uint64_t serial, uint64_t nowNanos) noexcept {
        const Due due{
            !initialized_ || (serial & 0x1FFFu) == 0u || nowNanos - audioNanos_ >= kPollNanos,
            !initialized_ || (serial & 0x3FFFu) == 0u || nowNanos - asyncNanos_ >= kPollNanos,
        };
        initialized_ = true;
        // Publish before invoking guest callbacks, which can re-enter through
        // another cooperative fiber on the same host thread.
        if (due.audio) audioNanos_ = nowNanos;
        if (due.async) asyncNanos_ = nowNanos;
        return due;
    }

private:
    static constexpr uint64_t kPollNanos = 1'000'000;
    bool initialized_ = false;
    uint64_t audioNanos_ = 0;
    uint64_t asyncNanos_ = 0;
};

} // namespace meteor
