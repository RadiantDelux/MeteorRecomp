#pragma once
#include <array>
#include <cstdint>

// Host presentation classification only; never changes guest clocks or VI.
// A single slow native-60 frame is not evidence of a 30 Hz simulation.
class InterpolationCadence {
public:
    void Reset() { *this = {}; }
    bool Observe(uint64_t delta, uint64_t vi) {
        if (vi == 0 || delta >= vi * 7 / 2) { Reset(); return false; }
        if (interval_ != vi) { Reset(); interval_ = vi; }
        nativeStreak_ = delta < vi * 5 / 4 ? nativeStreak_ + 1 : 0;
        sum_ -= samples_[cursor_];
        samples_[cursor_] = delta;
        sum_ += delta;
        cursor_ = (cursor_ + 1) % samples_.size();
        if (count_ < samples_.size()) ++count_;
        // Absorb the 1/3-retrace phase alternation of a healthy 30 Hz source,
        // but reject 40 Hz delivery, occasional missed UI frames and real stalls.
        return count_ >= 4 && nativeStreak_ < 2 &&
               sum_ * 10 >= vi * count_ * 17 && sum_ * 10 <= vi * count_ * 23;
    }
private:
    std::array<uint64_t, 6> samples_{};
    uint64_t interval_ = 0, sum_ = 0;
    unsigned cursor_ = 0, count_ = 0, nativeStreak_ = 0;
};
