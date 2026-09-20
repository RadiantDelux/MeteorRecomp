#pragma once
#include <algorithm>
#include <chrono>

// Host-only phase for deterministic guest execution. A slow frame can borrow
// at most one interval from the next frame, never an unbounded fast-forward.
class HostPresentationPacer {
public:
    using Clock = std::chrono::steady_clock;
    Clock::time_point Deadline(Clock::time_point now, Clock::duration interval) {
        if (deadline_ == Clock::time_point{}) deadline_ = now;
        deadline_ += interval;
        if (now >= deadline_) {
            deadline_ = std::max(deadline_, now - interval);
            return now;
        }
        return deadline_;
    }
    void Reset() { deadline_ = {}; }
private:
    Clock::time_point deadline_{};
};
