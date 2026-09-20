#include "audio_backend.h"

#include "runtime_log.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include <SDL3/SDL_init.h>

AudioBackend& AudioBackend::Instance() {
    static AudioBackend instance;
    return instance;
}

AudioBackend::~AudioBackend() {
    Shutdown();
}

float AudioBackend::EffectiveGainLocked() const {
    return m_muted ? 0.0f : m_masterVolume;
}

void AudioBackend::ApplyGainLocked() {
    if (m_stream && !SDL_SetAudioStreamGain(m_stream, EffectiveGainLocked())) {
        RT_LOG(RT_TAG_AUDIO) << "SDL_SetAudioStreamGain failed: " << SDL_GetError() << std::endl;
    }
}

void AudioBackend::SetMasterVolume(float volume) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_masterVolume = std::clamp(volume, 0.0f, 1.0f);
    ApplyGainLocked();
}

void AudioBackend::SetMuted(bool muted) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_muted = muted;
    ApplyGainLocked();
}

bool AudioBackend::EnsureInitializedLocked(uint32_t sampleRate, uint32_t channels) {
    if (m_initialized && m_sampleRate == sampleRate && m_channels == channels) {
        return true;
    }

    if (m_stream) {
        SDL_DestroyAudioStream(m_stream);
        m_stream = nullptr;
    }
    {
        std::lock_guard<std::mutex> queueLock(m_queueMutex);
        m_pendingSamples.clear();
        m_queueReadSample = 0;
        m_queueWriteSample = 0;
        m_pendingSampleCount = 0;
        m_pendingBytes = 0;
        m_reportedDroppedBlock = false;
        m_queueLimitBytes = QueueLimitBytes(sampleRate, channels);
        m_pendingSamples.resize(m_queueLimitBytes / sizeof(int16_t));
    }

    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        RT_LOG(RT_TAG_AUDIO) << "SDL_InitSubSystem(SDL_INIT_AUDIO) failed: " << SDL_GetError() << std::endl;
        return false;
    }

    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_S16LE;
    spec.channels = static_cast<int>(channels);
    spec.freq = static_cast<int>(sampleRate);

    // Use SDL's pull callback instead of calling SDL_GetAudioStreamQueued /
    // SDL_PutAudioStreamData from the guest thread.  The GameCube AID service
    // runs at an interrupt/scheduler boundary; blocking that thread on a host
    // audio-stream lock deadlocks guest progress and makes the window appear
    // hung.  Guest producers only append to the fixed PCM ring.  SDL's playback
    // thread asks for PCM here when the device actually needs it.
    SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &AudioBackend::AudioStreamCallback, this);
    if (!stream) {
        RT_LOG(RT_TAG_AUDIO) << "SDL_OpenAudioDeviceStream failed: " << SDL_GetError() << std::endl;
        return false;
    }

    RT_LOG(RT_TAG_AUDIO) << "host stream ready: device=" << SDL_GetAudioStreamDevice(stream)
                         << " paused=" << (SDL_AudioStreamDevicePaused(stream) ? 1 : 0)
                         << " rate=" << sampleRate << " channels=" << channels << std::endl;

    if (!SDL_SetAudioStreamGain(stream, EffectiveGainLocked())) {
        RT_LOG(RT_TAG_AUDIO) << "SDL_SetAudioStreamGain failed: " << SDL_GetError() << std::endl;
        SDL_DestroyAudioStream(stream);
        return false;
    }

    m_stream = stream;
    m_spec = spec;
    m_sampleRate = sampleRate;
    m_channels = channels;
    m_playbackStarted.store(false, std::memory_order_release);
    m_rebuffering.store(false, std::memory_order_release);
    m_initialized = true;
    return true;
}

bool AudioBackend::Init(uint32_t sampleRate, uint32_t channels) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return EnsureInitializedLocked(sampleRate, channels);
}

void AudioBackend::Shutdown() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_stream) {
        SDL_DestroyAudioStream(m_stream);
        m_stream = nullptr;
    }
    if (m_initialized) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
    m_initialized = false;
    m_sampleRate = 0;
    m_channels = 0;
    m_playbackStarted.store(false, std::memory_order_release);
    m_rebuffering.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> queueLock(m_queueMutex);
        m_pendingSamples.clear();
        m_queueReadSample = 0;
        m_queueWriteSample = 0;
        m_pendingSampleCount = 0;
        m_pendingBytes = 0;
        m_queueLimitBytes = 0;
        m_reportedDroppedBlock = false;
    }
}

uint32_t AudioBackend::QueueLimitBytes(uint32_t sampleRate, uint32_t channels) {
    // Keep enough host-only PCM to ride through renderer/driver hitches without
    // changing the guest AID clock, sample rate, pitch, or callback cadence.
    constexpr uint32_t queueMs = 256;

    const uint64_t bytesPerSecond = static_cast<uint64_t>(sampleRate) *
                                    static_cast<uint64_t>(channels) *
                                    sizeof(int16_t);
    return static_cast<uint32_t>((bytesPerSecond * queueMs) / 1000u);
}

uint32_t AudioBackend::PrebufferBytes(uint32_t sampleRate, uint32_t channels) {
    // The degraded Meteor runs contain 50-77 ms frame gaps while the old ring
    // normally held only ~20-45 ms.  A 96 ms host playback cushion covers that
    // class of transient without touching guest timing.  It is latency only at
    // the host output boundary; emulated AID state remains unchanged.
    constexpr uint32_t prebufferMs = 96;
    const uint64_t bytesPerSecond = static_cast<uint64_t>(sampleRate) *
                                    static_cast<uint64_t>(channels) *
                                    sizeof(int16_t);
    return static_cast<uint32_t>((bytesPerSecond * prebufferMs) / 1000u);
}

uint32_t AudioBackend::RebufferBytes(uint32_t sampleRate, uint32_t channels) {
    // After a real underrun, resume with more hysteresis than the initial
    // startup cushion so the callback cannot chatter between PCM and silence.
    constexpr uint32_t rebufferMs = 128;
    const uint64_t bytesPerSecond = static_cast<uint64_t>(sampleRate) *
                                    static_cast<uint64_t>(channels) *
                                    sizeof(int16_t);
    return static_cast<uint32_t>((bytesPerSecond * rebufferMs) / 1000u);
}

bool AudioBackend::EnqueueSamples(const int16_t* samples, size_t sampleCount) {
    if (!samples || sampleCount == 0) {
        return false;
    }

    const size_t incomingBytes = sampleCount * sizeof(int16_t);
    size_t pendingAfterWrite = 0;
    {
        std::lock_guard<std::mutex> queueLock(m_queueMutex);
        if (m_queueLimitBytes == 0 || m_pendingSamples.empty()) {
            return false;
        }
        if (m_pendingBytes + incomingBytes > m_queueLimitBytes) {
            // Preserve already queued audio and drop the newest block.  Most
            // importantly, never make the guest wait for the host audio device.
            if (!m_reportedDroppedBlock) {
                m_reportedDroppedBlock = true;
                RT_LOG(RT_TAG_AUDIO) << "output queue full (" << m_pendingBytes << "/" << m_queueLimitBytes
                          << " bytes); dropping blocks to preserve continuity" << std::endl;
            }
            return true;
        }

        const size_t capacity = m_pendingSamples.size();
        const size_t firstCount = std::min(sampleCount, capacity - m_queueWriteSample);
        std::memcpy(m_pendingSamples.data() + m_queueWriteSample, samples,
                    firstCount * sizeof(int16_t));
        if (firstCount < sampleCount) {
            std::memcpy(m_pendingSamples.data(), samples + firstCount,
                        (sampleCount - firstCount) * sizeof(int16_t));
        }
        m_queueWriteSample = (m_queueWriteSample + sampleCount) % capacity;
        m_pendingSampleCount += sampleCount;
        m_pendingBytes += incomingBytes;
        pendingAfterWrite = m_pendingBytes;
    }
    m_enqueuedBytes.fetch_add(incomingBytes, std::memory_order_relaxed);
    const uint64_t block = m_producerBlocks.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((block & 0xffu) == 0u) {
        RT_LOG(RT_TAG_AUDIO) << "host ring: blocks=" << block
                             << " pending=" << pendingAfterWrite << "/" << m_queueLimitBytes
                             << " enqueued=" << m_enqueuedBytes.load(std::memory_order_relaxed)
                             << " callbacks=" << m_callbackCount.load(std::memory_order_relaxed)
                             << " requested=" << m_callbackRequestedBytes.load(std::memory_order_relaxed)
                             << " supplied=" << m_callbackSuppliedBytes.load(std::memory_order_relaxed)
                             << " realSupplied=" << m_realPcmSuppliedBytes.load(std::memory_order_relaxed)
                             << " underrun=" << m_underrunBytes.load(std::memory_order_relaxed)
                             << " rebuffer=" << (m_rebuffering.load(std::memory_order_relaxed) ? 1 : 0)
                             << std::endl;
    }

    // SDL opens playback paused.  Do not start from an empty queue: establish
    // a real cushion first so short GPU/driver stalls do not immediately starve
    // the device.  Lock ordering stays m_mutex -> no queue lock, matching init.
    if (!m_playbackStarted.load(std::memory_order_acquire) &&
        pendingAfterWrite >= PrebufferBytes(m_sampleRate, m_channels)) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_playbackStarted.load(std::memory_order_relaxed) && m_stream) {
            if (SDL_ResumeAudioStreamDevice(m_stream)) {
                m_playbackStarted.store(true, std::memory_order_release);
                RT_LOG(RT_TAG_AUDIO) << "host playback started after prebuffer=" << pendingAfterWrite
                                     << " bytes" << std::endl;
            } else {
                RT_LOG(RT_TAG_AUDIO) << "SDL_ResumeAudioStreamDevice failed: " << SDL_GetError() << std::endl;
            }
        }
    }
    return true;
}

void SDLCALL AudioBackend::AudioStreamCallback(void* userdata, SDL_AudioStream* stream,
                                               int additionalAmount, int /*totalAmount*/) {
    if (userdata == nullptr || stream == nullptr || additionalAmount <= 0) {
        return;
    }
    auto* backend = static_cast<AudioBackend*>(userdata);
    backend->m_callbackCount.fetch_add(1, std::memory_order_relaxed);
    backend->m_callbackRequestedBytes.fetch_add(static_cast<uint64_t>(additionalAmount),
                                                std::memory_order_relaxed);
    backend->FeedAudioStream(stream, additionalAmount);
}

void AudioBackend::FeedAudioStream(SDL_AudioStream* stream, int additionalAmount) {
    // Keep the host callback allocation-free too.  Copy a bounded piece out of
    // the ring under the short queue lock, then release it before calling SDL so
    // the translated guest never waits on a host audio-stream operation.
    std::array<int16_t, 4096> scratch{};
    int supplied = 0;
    while (supplied < additionalAmount) {
        size_t copiedSamples = 0;
        bool holdForRebuffer = false;
        {
            std::lock_guard<std::mutex> queueLock(m_queueMutex);
            const size_t requestedSamples =
                static_cast<size_t>(additionalAmount - supplied) / sizeof(int16_t);
            if (requestedSamples == 0) {
                break;
            }
            if (m_rebuffering.load(std::memory_order_acquire)) {
                const size_t threshold = RebufferBytes(m_sampleRate, m_channels);
                if (m_pendingBytes < threshold) {
                    holdForRebuffer = true;
                } else {
                    m_rebuffering.store(false, std::memory_order_release);
                }
            }
            if (!holdForRebuffer && m_pendingSampleCount != 0 && !m_pendingSamples.empty()) {
                copiedSamples = std::min({m_pendingSampleCount, requestedSamples, scratch.size()});
                const size_t capacity = m_pendingSamples.size();
                const size_t firstCount = std::min(copiedSamples, capacity - m_queueReadSample);
                std::memcpy(scratch.data(), m_pendingSamples.data() + m_queueReadSample,
                            firstCount * sizeof(int16_t));
                if (firstCount < copiedSamples) {
                    std::memcpy(scratch.data() + firstCount, m_pendingSamples.data(),
                                (copiedSamples - firstCount) * sizeof(int16_t));
                }
                m_queueReadSample = (m_queueReadSample + copiedSamples) % capacity;
                m_pendingSampleCount -= copiedSamples;
                const size_t copiedBytes = copiedSamples * sizeof(int16_t);
                m_pendingBytes = copiedBytes <= m_pendingBytes ? m_pendingBytes - copiedBytes : 0;
            }
        }

        const bool realPcmBlock = copiedSamples != 0;
        if (!realPcmBlock) {
            const size_t requestedSamples = std::min(
                static_cast<size_t>(additionalAmount - supplied) / sizeof(int16_t), scratch.size());
            if (requestedSamples == 0) {
                break;
            }
            std::fill_n(scratch.data(), requestedSamples, int16_t{0});
            copiedSamples = requestedSamples;
            m_rebuffering.store(true, std::memory_order_release);
            m_underrunBytes.fetch_add(copiedSamples * sizeof(int16_t), std::memory_order_relaxed);
        }
        const int blockBytes = static_cast<int>(copiedSamples * sizeof(int16_t));
        if (!SDL_PutAudioStreamData(stream, scratch.data(), blockBytes)) {
            RT_LOG(RT_TAG_AUDIO) << "SDL_PutAudioStreamData failed in audio callback: "
                                 << SDL_GetError() << std::endl;
            break;
        }
        if (realPcmBlock) {
            m_realPcmSuppliedBytes.fetch_add(static_cast<uint64_t>(blockBytes), std::memory_order_relaxed);
        }
        m_callbackSuppliedBytes.fetch_add(static_cast<uint64_t>(blockBytes), std::memory_order_relaxed);
        supplied += blockBytes;
    }
}

bool AudioBackend::PushWiiAiSamplesBE16(const uint8_t* data, size_t bytes) {
    if (!data || bytes == 0) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_initialized || !m_stream) {
            return false;
        }
    }

    const size_t sampleCount = bytes / sizeof(int16_t);
    if (sampleCount == 0) {
        return false;
    }

    const size_t frameCount = sampleCount / 2;
    if (frameCount == 0) {
        return false;
    }

    // Reuse this producer-side conversion scratch between AID interrupts. The
    // common DMA size is stable, so after the first resize this performs no heap
    // allocation in the IRQ path.
    thread_local std::vector<int16_t> converted;
    converted.resize(frameCount * 2);
    uint16_t peak = 0;
    size_t nonZeroSamples = 0;
    for (size_t frame = 0; frame < frameCount; ++frame) {
        const size_t rightOffset = frame * 4;
        const size_t leftOffset = rightOffset + 2;
        const uint16_t right = static_cast<uint16_t>(data[rightOffset]) << 8 |
                               static_cast<uint16_t>(data[rightOffset + 1]);
        const uint16_t left = static_cast<uint16_t>(data[leftOffset]) << 8 |
                              static_cast<uint16_t>(data[leftOffset + 1]);
        converted[frame * 2] = static_cast<int16_t>(left);
        converted[frame * 2 + 1] = static_cast<int16_t>(right);
        const int32_t leftSigned = static_cast<int16_t>(left);
        const int32_t rightSigned = static_cast<int16_t>(right);
        peak = static_cast<uint16_t>(std::max<int32_t>(
            peak, std::max(std::abs(leftSigned), std::abs(rightSigned))));
        nonZeroSamples += (left != 0u) ? 1u : 0u;
        nonZeroSamples += (right != 0u) ? 1u : 0u;
    }

    const uint64_t nextBlock = m_producerBlocks.load(std::memory_order_relaxed) + 1;
    if ((nextBlock & 0xffu) == 0u) {
        RT_LOG(RT_TAG_AUDIO) << "AI PCM: bytes=" << bytes << " peak=" << peak
                             << " nonzero=" << nonZeroSamples << "/" << sampleCount << std::endl;
    }

    return EnqueueSamples(converted.data(), converted.size());
}

bool AudioBackend::PushSamplesLE16(const int16_t* samples, size_t sampleCount) {
    if (!samples || sampleCount == 0) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_initialized || !m_stream) {
            return false;
        }
    }

    return EnqueueSamples(samples, sampleCount);
}
