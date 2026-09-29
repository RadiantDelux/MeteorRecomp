#include "host_presentation_pacer.h"
#include <stdexcept>
#include <iostream>
using namespace std::chrono_literals;
void Check(bool value) { if (!value) throw std::runtime_error("Host presentation pacing failed"); }
int main() {
    using Clock = HostPresentationPacer::Clock;
    const auto start = Clock::time_point(1s);
    constexpr auto interval = 16666667ns;
    HostPresentationPacer pacer;
    auto now = pacer.Deadline(start, interval);
    const auto first = now;
    // Full checkpoints and light intermediate frames alternate. Average work
    // fits in 60 Hz, so short overruns must not turn this stream into 48 Hz.
    for (int frame = 0; frame < 600; ++frame) {
        now += frame % 2 == 0 ? 25ms : 8ms;
        now = pacer.Deadline(now, interval);
    }
    Check(now - first == interval * 600);
    // A long network stall does not queue seconds of fast-forward. Only one
    // following interval can be recovered, then ordinary pacing resumes.
    now += 5s;
    Check(pacer.Deadline(now, interval) == now);
    Check(pacer.Deadline(now + 2ms, interval) == now + 2ms);
    Check(pacer.Deadline(now + 4ms, interval) == now + interval);
    pacer.Reset();
    Check(pacer.Deadline(now, interval) == now + interval);
    std::cout << "Host presentation: 60 Hz under alternating work and bounded recovery after a network stall\n";
}
