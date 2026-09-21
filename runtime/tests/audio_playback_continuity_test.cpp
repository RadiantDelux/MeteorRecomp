#include "audio_playback_continuity.h"
#include <cassert>
#include <iostream>
#include <vector>

int main() {
    AudioDmaPlaybackCursor dma;
    dma.Program(); dma.Start();
    assert(dma.Complete(true));
    for (int i = 0; i < 20; ++i) assert(!dma.Complete(true));
    // Programming affects the next autoreload, not the currently active block.
    dma.Program();
    assert(!dma.Complete(true));
    assert(dma.Complete(true));
    assert(!dma.Complete(true));
    // Fresh submissions with the same address/samples must still play.
    for (int i = 0; i < 100; ++i) {
        dma.Program();
        dma.Complete(true);
        assert(dma.Complete(true));
    }
    assert(dma.Complete(false)); // intentional loops in other titles
    dma.Start();
    assert(dma.Complete(true));

    for (uint32_t rate : {32000u, 48000u}) {
        AudioPlaybackContinuity playback;
        playback.Reset(rate, 2);
        const size_t ramp = rate * 3 / 1000;
        std::vector<int16_t> pcm(2 * (ramp + 17));
        for (size_t i = 0; i < pcm.size(); i += 2) { pcm[i] = 24000; pcm[i + 1] = -12000; }
        const auto original = pcm;
        playback.Process(pcm.data(), pcm.size(), false);
        assert(pcm == original); // continuous audio is bit exact
        playback.Process(pcm.data(), pcm.size(), true);
        for (size_t i = 2; i < pcm.size(); i += 2) {
            assert(pcm[i] <= pcm[i - 2] && pcm[i] >= 0);
            assert(pcm[i + 1] >= pcm[i - 1] && pcm[i + 1] <= 0);
            assert(pcm[i - 2] - pcm[i] <= 251);
        }
        assert(pcm[2 * ramp - 2] == 0 && pcm.back() == 0);
        pcm = original;
        // One stereo frame per callback gives the same continuous ramp.
        for (size_t i = 0; i < pcm.size(); i += 2) playback.Process(pcm.data() + i, 2, false);
        assert(pcm[0] > 0 && pcm[0] <= 250);
        for (size_t i = 2; i < pcm.size(); i += 2) {
            assert(pcm[i] >= pcm[i - 2]);
            assert(pcm[i] - pcm[i - 2] <= 251);
        }
        assert(pcm[2 * ramp - 2] == 24000 && pcm.back() == -12000);
        pcm = original;
        playback.Process(pcm.data(), 8, true); // short gap interrupted mid-fade
        const auto last = pcm[6];
        pcm = original;
        playback.Process(pcm.data(), 2, false);
        assert(pcm[0] >= last && pcm[0] - last < 251);
    }
    std::cout << "Audio continuity: fresh/stale DMA, looping, stereo fades and recovery passed\n";
}
