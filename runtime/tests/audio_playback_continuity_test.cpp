#include "audio_playback_continuity.h"
#include <cassert>
#include <iostream>
#include <vector>

int main() {
    {
        AudioHostGapAccounting gaps;
        gaps.Reset(96);

        // With no prior host underrun, stale physical time must remain in the
        // output timeline instead of time-compressing the following fresh PCM.
        auto resolved = gaps.ResolveStaleFrames(24);
        assert(resolved.coveredFrames == 0);
        assert(resolved.queuedFrames == 24);

        // A host underrun that already rendered 40 frames of silence covers the
        // first 40 frames of the next stale AID catch-up.  Only the remainder is
        // allowed to enter the host timeline again.
        gaps.AccountHostSilence(40);
        resolved = gaps.ResolveStaleFrames(60);
        assert(resolved.coveredFrames == 40);
        assert(resolved.queuedFrames == 20);
        assert(gaps.CreditFrames() == 0);

        // Credit is bounded to the host queue window, and unused surplus never
        // leaks forward into an unrelated future stale DMA event.
        gaps.AccountHostSilence(1000);
        assert(gaps.CreditFrames() == 96);
        resolved = gaps.ResolveStaleFrames(32);
        assert(resolved.coveredFrames == 32);
        assert(resolved.queuedFrames == 0);
        assert(gaps.CreditFrames() == 0);

        gaps.AccountHostSilence(24);
        resolved = gaps.ResolveStaleFrames(0);
        assert(resolved.coveredFrames == 0 && resolved.queuedFrames == 0);
        assert(gaps.CreditFrames() == 0);
    }

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
