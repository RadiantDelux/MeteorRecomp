#pragma once

#include "local_checkpoint.h"
#include "timebase_contract.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <thread>

// Guest-thread clock. Presentation, network deadlines and diagnostics retain
// their wall clocks. This mode is selected before boot, never during a match.
// Advancing translated call/back-edge boundaries is deterministic scheduling,
// not an instruction-accurate model of the Gekko's execution time.
namespace GuestClock {
using Clock = std::chrono::steady_clock;
using Nanoseconds = std::chrono::nanoseconds;
inline constexpr int64_t kInitialNanoseconds = 1'000'000'000;
inline constexpr int64_t kBoundaryNanoseconds = 250;
inline constexpr int64_t kDefaultRtcUnixSeconds = 946'684'800;

namespace Detail {
struct State {
    bool enabled = false;
    uint64_t generation = 0;
    int64_t nanoseconds = kInitialNanoseconds;
    int64_t rtcUnixSeconds = kDefaultRtcUnixSeconds;
};
inline thread_local State state;
inline std::atomic<uint64_t> nextGeneration{1};
}

inline bool Enabled() noexcept { return Detail::state.enabled; }

inline void ConfigureForBoot(bool deterministic,
                             int64_t rtcUnixSeconds = kDefaultRtcUnixSeconds) noexcept {
    Detail::state = {deterministic,
        Detail::nextGeneration.fetch_add(1, std::memory_order_relaxed),
        kInitialNanoseconds, rtcUnixSeconds};
}

inline Clock::time_point Now() noexcept {
    if (!Enabled()) return Clock::now();
    return Clock::time_point(std::chrono::duration_cast<Clock::duration>(
        Nanoseconds(Detail::state.nanoseconds)));
}

inline void Advance(Nanoseconds amount) noexcept {
    if (!Enabled() || amount.count() <= 0) return;
    auto& ns = Detail::state.nanoseconds;
    ns += std::min(amount.count(), std::numeric_limits<int64_t>::max() - ns);
}

inline void AdvanceExecutionBoundary() noexcept { Advance(Nanoseconds(kBoundaryNanoseconds)); }

inline void AdvanceTo(Clock::time_point deadline) noexcept {
    if (!Enabled()) return;
    const auto ns = std::chrono::duration_cast<Nanoseconds>(deadline.time_since_epoch()).count();
    if (ns > Detail::state.nanoseconds) Detail::state.nanoseconds = ns;
}

// Only meaningful in deterministic mode; normal PPC time-base behavior keeps
// its existing process-relative origin in ppc_helpers.cpp.
inline uint64_t TimeBase() noexcept {
    return TimeBaseContract::NanosecondsToTicks(
        static_cast<uint64_t>(Detail::state.nanoseconds));
}

inline int64_t RtcUnixSeconds() noexcept {
    if (!Enabled()) return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return Detail::state.rtcUnixSeconds +
        (Detail::state.nanoseconds - kInitialNanoseconds) / 1'000'000'000;
}

inline LocalCheckpoint CaptureState() {
    if (!Enabled()) return {};
    const auto owner = std::this_thread::get_id();
    const auto generation = Detail::state.generation;
    return LocalCheckpoint::Capture([owner, generation] {
        return std::this_thread::get_id() == owner && Enabled() &&
               Detail::state.generation == generation;
    }, Detail::state.nanoseconds, Detail::state.rtcUnixSeconds);
}
}
