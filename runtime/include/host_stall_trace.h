#pragma once

#include "runtime_log.h"
#include <chrono>
#include <cstdlib>

// Opt-in wall-time probes. Timings include waits/preemption and nested work;
// they are not CPU time and must not be added together. Normal play performs
// no clock reads or logging here.
class HostStallTrace {
public:
    using Clock = std::chrono::steady_clock;
    explicit HostStallTrace(const char* phase, Clock::duration limit = std::chrono::milliseconds(8))
        : phase_(phase), limit_(limit), active_(Enabled()) {
        if (active_) started_ = Clock::now();
    }
    ~HostStallTrace() {
        if (!active_) return;
        const auto finished = Clock::now();
        const auto elapsed = finished - started_;
        if (elapsed <= limit_) return;
        RT_LOG(RT_TAG_RUNTIME) << "Host stall: phase=" << phase_
            << " wall=" << std::chrono::duration<double, std::milli>(elapsed).count()
            << "ms endNs=" << std::chrono::duration_cast<std::chrono::nanoseconds>(
                finished.time_since_epoch()).count() << std::endl;
    }
    static bool Enabled() {
        static const bool enabled = std::getenv("METEOR_TRACE_FRAME_TIMING") != nullptr;
        return enabled;
    }
private:
    const char* phase_;
    Clock::duration limit_;
    bool active_;
    Clock::time_point started_{};
};
