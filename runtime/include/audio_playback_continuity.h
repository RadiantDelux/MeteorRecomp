#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
struct AudioHostGapResolution {
    size_t coveredFrames = 0;
    size_t queuedFrames = 0;
};

// SDL can consume the host ring while the guest thread is stalled.  Silence
// emitted after the ring actually runs dry already represents part of the same
// physical AID time that a later catch-up reports as stale autoreloads.  Keep a
// bounded credit for that silence so catch-up can account for each stale frame
// exactly once: already-rendered silence covers it first, and only the remainder
// needs an explicit gap in the host timeline.
class AudioHostGapAccounting {
public:
    void Reset(size_t maxCreditFrames) {
        maxCreditFrames_ = maxCreditFrames;
        creditFrames_ = 0;
    }

    void ClearCredit() { creditFrames_ = 0; }

    void AccountHostSilence(size_t frames) {
        if (maxCreditFrames_ == 0 || frames == 0) return;
        const size_t room = maxCreditFrames_ - std::min(creditFrames_, maxCreditFrames_);
        creditFrames_ += std::min(frames, room);
    }

    AudioHostGapResolution ResolveStaleFrames(size_t frames) {
        const size_t covered = std::min(frames, creditFrames_);
        // Any surplus silence belongs to an older host underrun/rebuffer event,
        // not to an unrelated future stale DMA.  The next physical batch starts
        // with a clean accounting window.
        creditFrames_ = 0;
        return {covered, frames - covered};
    }

    size_t CreditFrames() const { return creditFrames_; }

private:
    size_t maxCreditFrames_ = 0;
    size_t creditFrames_ = 0;
};

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
