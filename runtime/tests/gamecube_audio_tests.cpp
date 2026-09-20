#include "ax_gamecube_pb.h"
#include <cstdio>
#include <cstdlib>

namespace AxDspHle {
std::atomic<uint32_t> g_aramWindowGeneration{1};
static std::array<uint8_t, 4096> samples{};
bool ResolveAramWindow(uint32_t addr, AramWindow& w) {
    if (addr >= samples.size()) return false;
    w = {0, 4096, 1, samples.data()}; return true;
}
uint8_t ReadAramByteSlow(uint32_t) { return 0; }
}
using namespace AxDspHle;
static void Check(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
int main() {
    GameCubePB raw{};
    for (unsigned i = 0; i < raw.size(); ++i) raw[i] = 0x4000 + i;
    const auto original = raw;
    AXPBWii pb{};
    ConvertGameCubePB(raw, pb, false);
    Check(pb.running == original[7], "GC running field");
    Check(pb.audio_addr.cur_addr_lo == original[62], "GC sample position field");
    Check(pb.mixer.auxB_surround.volume == original[21], "GC surround channel order");
    ConvertGameCubePB(raw, pb, true);
    Check(raw == original, "GC PB round trip must preserve updates, padding and loop counter");
    pb.running = 0; pb.audio_addr.cur_addr_lo = 0x1234;
    ConvertGameCubePB(raw, pb, true);
    auto expected = original; expected[7] = 0; expected[62] = 0x1234;
    Check(raw == expected, "GC PB writeback must not overwrite adjacent fields");
    Check(GameCubeMixControl(3) == (MIX_MAIN_L | MIX_MAIN_R), "GC stereo control");
    // Two PCM16 samples loop inside ARAM. Streaming loops must advance the
    // guest-visible counter; one-shot completion must clear running.
    samples[0] = 0x12; samples[1] = 0x34;
    samples[2] = 0xFE; samples[3] = 0xDC;
    pb = {}; pb.running = 1; pb.is_stream = 1;
    pb.audio_addr.looping = 1; pb.audio_addr.sample_format = 0xA;
    pb.audio_addr.end_addr_lo = 1; pb.adpcm.gain = 0x800;
    uint16_t loops = 0; Accelerator accelerator;
    accelerator.Setup(&pb, &loops);
    Check(accelerator.ReadSample() == 0x1234, "PCM16 first ARAM sample");
    Check(accelerator.ReadSample() == static_cast<int16_t>(0xFEDC), "PCM16 signed ARAM sample");
    Check(loops == 1 && pb.audio_addr.cur_addr_lo == 0, "stream loop count and position");
    Check(accelerator.ReadSample() == 0x1234, "stream resumes after loop");
    pb.audio_addr.looping = 0; accelerator.Setup(&pb, &loops);
    accelerator.ReadSample();
    Check(pb.running == 0, "one-shot voice completes");
    std::puts("GameCube audio layout, PCM and stream loop tests passed");
}
