#pragma once

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <vector>
#include "audio_playback_continuity.h"

#include <SDL3/SDL_audio.h>

class AudioBackend {
public:
    static AudioBackend& Instance();

    bool Init(uint32_t sampleRate, uint32_t channels);
    void Shutdown();

    // Wii AI DMA frames are big-endian and ordered right, left. SDL expects
    // native-endian interleaved left, right samples.
    bool PushWiiAiSamplesBE16(const uint8_t* data, size_t bytes);
    bool PushSamplesLE16(const int16_t* samples, size_t sampleCount);
    bool PushGapFrames(size_t frames);

    // Applied to the final host output, covering both AX and direct AI DMA.
    void SetMasterVolume(float volume);
    void SetMuted(bool muted);

private:
    AudioBackend() = default;
    ~AudioBackend();
    AudioBackend(const AudioBackend&) = delete;
    AudioBackend& operator=(const AudioBackend&) = delete;

    bool EnsureInitializedLocked(uint32_t sampleRate, uint32_t channels);
    bool EnqueueSamples(const int16_t* samples, size_t sampleCount, bool gap = false);
    void FeedAudioStream(SDL_AudioStream* stream, int additionalAmount);
    static void SDLCALL AudioStreamCallback(void* userdata, SDL_AudioStream* stream,
                                            int additionalAmount, int totalAmount);
    static uint32_t QueueLimitBytes(uint32_t sampleRate, uint32_t channels);
    static uint32_t PrebufferBytes(uint32_t sampleRate, uint32_t channels);
    static uint32_t RebufferBytes(uint32_t sampleRate, uint32_t channels);
    float EffectiveGainLocked() const;
    void ApplyGainLocked();

    mutable std::mutex m_mutex;
    std::mutex m_queueMutex;
    SDL_AudioStream* m_stream = nullptr;
    SDL_AudioSpec m_spec{};
    uint32_t m_sampleRate = 0;
    uint32_t m_channels = 0;
    bool m_initialized = false;
    std::atomic_bool m_playbackStarted{false};
    std::atomic_bool m_rebuffering{false};
    float m_masterVolume = 1.0f;
    bool m_muted = false;
    bool m_reportedDroppedBlock = false;
    uint32_t m_queueLimitBytes = 0;
    size_t m_pendingBytes = 0;
    // Fixed-capacity PCM ring. The GameCube AID producer runs hundreds of
    // times per second; allocating a vector/deque node for every DMA block put
    // RtlAllocateHeap directly in the guest IRQ hot path. Allocate once when
    // the host stream is configured and only copy samples thereafter.
    std::vector<int16_t> m_pendingSamples;
    std::vector<uint8_t> m_pendingGap;
    AudioPlaybackContinuity m_continuity;
    size_t m_queueReadSample = 0;
    size_t m_queueWriteSample = 0;
    size_t m_pendingSampleCount = 0;
    std::atomic<uint64_t> m_callbackCount{0};
    std::atomic<uint64_t> m_callbackRequestedBytes{0};
    std::atomic<uint64_t> m_callbackSuppliedBytes{0};
    std::atomic<uint64_t> m_realPcmSuppliedBytes{0};
    std::atomic<uint64_t> m_enqueuedBytes{0};
    std::atomic<uint64_t> m_producerBlocks{0};
    std::atomic<uint64_t> m_underrunBytes{0};
    std::atomic<uint64_t> m_staleDmaFrames{0};
};
