#include "../../projects/meteor/runtime_native/loop_service_cadence.h"
#include <cassert>
#include <iostream>

int main() {
    constexpr uint32_t busyPc = 0x8002D394;
    for (uint64_t serial = 1; serial <= 65536; ++serial) {
        const bool required = meteor::NeedsLoopCheckpoint(serial, busyPc, false);
        assert(required == ((serial % 256) == 0));
        assert(meteor::NeedsLoopCheckpoint(serial, 0x80210ADC, false));
        assert(meteor::NeedsLoopCheckpoint(serial, 0x80210AD8, false));
        assert(meteor::NeedsLoopCheckpoint(serial, 0, false));
        assert(meteor::NeedsLoopCheckpoint(serial, busyPc, true));
        if (serial % 8192 == 0 || serial % 16384 == 0) assert(required);
    }

    meteor::LoopServiceCadence idle;
    assert(idle.Poll(1, 0).audio);
    // No fixed-count boundary is reached during a second of idle waits, but
    // audio, VI and alarms must still be polled at every millisecond.
    for (uint64_t i = 1; i <= 1000; ++i) {
        const auto due = idle.Poll(i + 1, i * 1'000'000);
        assert(due.audio && due.async);
        const auto nested = idle.Poll(i + 2, i * 1'000'000);
        assert(!nested.audio && !nested.async);
    }

    meteor::LoopServiceCadence busy;
    busy.Poll(1, 0);
    assert(!busy.Poll(256, 500'000).async);
    const auto audioBoundary = busy.Poll(8192, 750'000);
    assert(audioBoundary.audio && !audioBoundary.async);
    const auto timeBoundary = busy.Poll(8448, 1'000'000);
    assert(!timeBoundary.audio && timeBoundary.async); // independent clocks
    const auto legacyBoundary = busy.Poll(16384, 1'100'000);
    assert(legacyBoundary.audio && legacyBoundary.async);
    const auto hostStall = busy.Poll(16640, 150'000'000);
    assert(hostStall.audio && hostStall.async);
    const auto immediate = busy.Poll(16896, 150'000'000);
    assert(!immediate.audio && !immediate.async); // no catch-up poll burst
    std::cout << "Meteor checkpoint cadence: idle, busy, nested and late service passed\n";
}
