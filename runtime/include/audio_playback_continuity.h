#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

// Host playback policy only; hardware DMA completions/interrupts still advance.
// A streaming producer explicitly re-arms each new buffer. Autoreloading that
// same submission while the producer is stalled must not become a 3 ms buzz.
struct AudioDmaPlaybackCursor {
    uint64_t programmed = 0;
    uint64_t active = 0;
    uint64_t lastPlayed = ~uint64_t{0};

    void Program() { ++programmed; }
    void Start() { active = programmed; lastPlayed = ~uint64_t{0}; }
    bool Complete(bool suppressStale) {
        const bool fresh = !suppressStale || active != lastPlayed;
        lastPlayed = active;
        active = programmed;
        return fresh;
    }
};

// Click-free transitions into/out of missing PCM. Valid continuous PCM is
// copied unchanged; there is no resampling, pitch change or block repetition.
class AudioPlaybackContinuity {
public:
    void Reset(uint32_t sampleRate, uint32_t channels) {
        channels_ = std::clamp(channels, 1u, 8u);
        fadeFrames_ = std::max(1u, sampleRate * 3u / 1000u);
        remaining_ = 0;
        gap_ = false;
        last_.fill(0);
        anchor_.fill(0);
    }

    void Process(int16_t* samples, size_t sampleCount, bool gap) {
        const size_t frames = sampleCount / channels_;
        if (frames == 0) return;
        if (gap != gap_) {
            gap_ = gap;
            anchor_ = last_;
            remaining_ = fadeFrames_;
        }
        if (!gap_ && remaining_ == 0) {
            for (size_t ch = 0; ch < channels_; ++ch) last_[ch] = samples[(frames - 1) * channels_ + ch];
            return;
        }
        for (size_t frame = 0; frame < frames; ++frame) {
            if (remaining_ != 0) --remaining_;
            for (size_t ch = 0; ch < channels_; ++ch) {
                const size_t i = frame * channels_ + ch;
                const int32_t target = gap_ ? 0 : samples[i];
                const int32_t value =
                    (anchor_[ch] * static_cast<int32_t>(remaining_) +
                     target * static_cast<int32_t>(fadeFrames_ - remaining_)) /
                    static_cast<int32_t>(fadeFrames_);
                samples[i] = static_cast<int16_t>(value);
                last_[ch] = samples[i];
            }
        }
    }

private:
    uint32_t channels_ = 2, fadeFrames_ = 96, remaining_ = 0;
    bool gap_ = false;
    std::array<int32_t, 8> last_{}, anchor_{};
};
