#pragma once

#include "wiicompiled_target.h"

#include <chrono>
#include <cstdint>

namespace TimeBaseContract {

// Broadway/Gekko time base runs at one quarter of the console bus clock.
#if WIICOMPILED_GUEST_IS_GAMECUBE
// GameCube: 162 MHz bus -> 40.5 MHz time base = 81 ticks / 2000 ns.
inline constexpr uint64_t kBusClockHz = 162'000'000u;
#else
// Wii: 243 MHz bus -> 60.75 MHz time base = 243 ticks / 4000 ns.
inline constexpr uint64_t kBusClockHz = 243'000'000u;
#endif
inline constexpr uint64_t kTimeBaseDivider = 4u;
inline constexpr uint64_t kTicksPerSecond = kBusClockHz / kTimeBaseDivider;
inline constexpr uint64_t kNanosecondsPerSecond = 1'000'000'000u;
#if WIICOMPILED_GUEST_IS_GAMECUBE
inline constexpr uint64_t kTickRatioNumerator = 81u;
inline constexpr uint64_t kTickRatioDenominator = 2'000u;
#else
inline constexpr uint64_t kTickRatioNumerator = 243u;
inline constexpr uint64_t kTickRatioDenominator = 4'000u;
#endif

#if WIICOMPILED_GUEST_IS_GAMECUBE
static_assert(kTicksPerSecond == 40'500'000u);
#else
static_assert(kTicksPerSecond == 60'750'000u);
#endif
static_assert(kTicksPerSecond * kTickRatioDenominator ==
              kNanosecondsPerSecond * kTickRatioNumerator);

// Split the rational conversion around the division so the intermediate
// product cannot overflow. The result is floor(nanoseconds * 243 / 4000).
constexpr uint64_t NanosecondsToTicks(uint64_t nanoseconds) noexcept
{
    return (nanoseconds / kTickRatioDenominator) * kTickRatioNumerator +
           ((nanoseconds % kTickRatioDenominator) * kTickRatioNumerator) /
               kTickRatioDenominator;
}

// Convert guest ticks to a host duration without applying scheduling policy.
// Oversized durations retain the existing zero-duration failure behavior.
constexpr std::chrono::nanoseconds TicksToDuration(uint64_t ticks) noexcept
{
    constexpr uint64_t kMaxNanoseconds =
        static_cast<uint64_t>(std::chrono::nanoseconds::max().count());
    constexpr uint64_t kMaxTicks = NanosecondsToTicks(kMaxNanoseconds);

    if (ticks == 0 || ticks > kMaxTicks) {
        return std::chrono::nanoseconds::zero();
    }

    const uint64_t nanoseconds =
        (ticks / kTickRatioNumerator) * kTickRatioDenominator +
        ((ticks % kTickRatioNumerator) * kTickRatioDenominator) /
            kTickRatioNumerator;
    return std::chrono::nanoseconds(static_cast<int64_t>(nanoseconds));
}

} // namespace TimeBaseContract
