#include "guest_clock.h"
#include "gamecube_audio_dma_timing.h"

#include <iostream>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

int main() {
    static_assert(TimeBaseContract::kTicksPerSecond == 40'500'000);
    GuestClock::ConfigureForBoot(true, 1'700'000'000);
    const auto start = GuestClock::Now();
    const auto initial = GuestClock::CaptureState();
    const auto tb = GuestClock::TimeBase();
    std::this_thread::sleep_for(12ms); // host delay cannot change any guest deadline
    Check(GuestClock::Now() == start && GuestClock::TimeBase() == tb, "Host delay advanced guest time");
    Check(GuestClock::RtcUnixSeconds() == 1'700'000'000, "RTC seed lost");

    // Follow the real DMA duration contract, checking the sample immediately
    // before and at its deadline, then replay it from the same clock checkpoint.
    for (int replay = 0; replay != 8; ++replay) {
        Check(initial.Restore(), "Clock restore failed");
        const auto duration = GameCubeAudioDmaTiming::DurationMicros(12, 32000);
        GuestClock::Advance(std::chrono::microseconds(duration - 1));
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(GuestClock::Now() - start).count();
        Check(GameCubeAudioDmaTiming::RemainingBlocks(12, 32000, elapsed) == 1, "DMA fired early");
        GuestClock::Advance(1us);
        elapsed = std::chrono::duration_cast<std::chrono::microseconds>(GuestClock::Now() - start).count();
        Check(GameCubeAudioDmaTiming::RemainingBlocks(12, 32000, elapsed) == 0, "DMA deadline missed");
    }
    Check(initial.Restore(), "Clock restore failed");
    for (int boundary = 0; boundary != 4'000'000; ++boundary) GuestClock::AdvanceExecutionBoundary();
    Check(GuestClock::Now() - start == 1s, "Execution boundary drift");
    Check(GuestClock::TimeBase() - tb == 40'500'000, "Gekko time-base conversion drift");
    Check(GuestClock::RtcUnixSeconds() == 1'700'000'001, "RTC did not advance");
    GuestClock::AdvanceTo(start);
    GuestClock::Advance(-1s);
    Check(GuestClock::Now() - start == 1s, "Ordinary clock advance moved backward");
    GuestClock::AdvanceTo(start + 2s);
    Check(GuestClock::Now() - start == 2s, "Guest idle deadline not reached");

    bool foreignRestore = true;
    std::thread foreign([&] { GuestClock::ConfigureForBoot(true); foreignRestore = initial.Restore(); });
    foreign.join();
    Check(!foreignRestore, "Foreign thread restored clock");
    GuestClock::ConfigureForBoot(true);
    Check(!initial.Restore(), "A different boot accepted a stale checkpoint");
    GuestClock::ConfigureForBoot(false);
    const auto nativeStart = GuestClock::Now();
    std::this_thread::sleep_for(2ms);
    Check(GuestClock::Now() > nativeStart && !GuestClock::CaptureState(), "Offline clock changed behavior");
    std::cout << "Guest clock: host stall isolation, DMA replay, Gekko ticks, RTC, idle and checkpoint lifetime passed\n";
}
