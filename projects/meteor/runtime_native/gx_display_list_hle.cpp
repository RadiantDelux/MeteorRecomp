#include <cstdint>

#include "abi_bridge.h"
#include "hle_stubs.h"
#include "memory.h"
#include "ppc_runtime.h"
#include "runtime_log.h"

// Keep RDSPAF's retail GXBegin/EndDisplayList bodies authoritative for the
// guest GXData/CPU-FIFO bookkeeping, while mirroring only the actual list body
// into the generic HLE recorder.  Starting the recorder before the retail Begin
// or keeping it active through the retail End swallows FIFO-switch/restore
// traffic that must execute live.  Leaving recording disabled entirely has the
// opposite failure: list payload draws execute immediately and the temporary
// GXFifoObj never accumulates the list byte count returned by GXEndDisplayList.

extern "C" void func_80231294(CpuContext* cpu);
extern "C" void func_80231344(CpuContext* cpu);
extern "C" void func_8022DD68(CpuContext* cpu);

namespace {

// RDSPAF GXBeginDisplayList uses the title-linked temporary GXFifoObj at
// 0x80579F28 (generated func_80231294: r31=0x80580000-0x60D8; r29=r31).
constexpr uint32_t kMeteorDisplayListFifoObj = 0x80579F28u;

extern "C" void Meteor_GXBeginDisplayList(CpuContext* cpu)
{
    if (cpu == nullptr) {
        return;
    }

    const uint32_t listAddr = cpu->gpr[3];
    const uint32_t sizeBytes = cpu->gpr[4];

    static uint32_t s_beginLogCount = 0;
    if (s_beginLogCount < 16u) {
        RT_LOGF(RT_TAG_GX, "RDSPAF DL begin list=0x%08x size=0x%x\n", listAddr, sizeBytes);
        ++s_beginLogCount;
    }

    // The retail body flushes/saves the previous CPU FIFO and switches GXData
    // to the display-list FIFO.  Those pre-begin writes must not be recorded.
    func_80231294(cpu);

    // From this point until GXEndDisplayList the gather-pipe stream is the list
    // payload.  Capture it into guest RAM instead of executing its draws/state
    // live on the host parser.
    GX_HLE_BeginDisplayListRecordingForFifo(listAddr, sizeBytes,
                                            kMeteorDisplayListFifoObj);
}

extern "C" void Meteor_GXEndDisplayList(CpuContext* cpu)
{
    if (cpu == nullptr) {
        return;
    }

    // Retail GXEndDisplayList starts by calling GXFlush while the display-list
    // FIFO is still active. GXFlush first drains __GXData dirty state (notably
    // viewport/scissor/projection changes) into the list, then emits its padding
    // writes, and only later does End restore the previous CPU FIFO. Our HLE
    // recorder must therefore capture the dirty-state drain before it is stopped;
    // stopping first leaves the list with stale GX state (Meteor's battle shadow
    // list then retains its 360x360 viewport instead of the pending 640x512
    // restore).  Drain exactly the translated retail dirty-state routine here;
    // leave End's FIFO/padding/restore bookkeeping live as before.
    try {
        const uint32_t gxData = Memory::Read32(0x8062F518u);
        if (gxData != 0u && Memory::Read32(gxData + 0x5FCu) != 0u) {
            func_8022DD68(cpu);
        }
    } catch (...) {
    }

    // Publish/stop before entering the remainder of the retail End body.  That
    // makes the guest temporary GXFifoObj expose the captured byte count while
    // its FIFO flush padding/restore traffic executes live instead of being
    // swallowed into the list (the failure mode that previously blacked the
    // whole menu/scene).
    GX_HLE_EndDisplayListRecordingToFifo(kMeteorDisplayListFifoObj);
    const uint32_t recordedSize = Memory::Read32(kMeteorDisplayListFifoObj + 0x1Cu);
    func_80231344(cpu);

    // The translated SDK refreshes the temporary GXFifoObj from CP-visible FIFO
    // state before returning its rwDistance.  Aurora executes/records the gather
    // stream in software, so that hardware-facing shadow does not advance with
    // the bytes captured above and currently reports a stale value (typically
    // 2).  The guest display-list bytes and final write/count were already
    // published by the HLE recorder, so return that authoritative byte count.
    cpu->gpr[3] = recordedSize;
    static uint32_t s_endLogCount = 0;
    if (s_endLogCount < 16u) {
        RT_LOGF(RT_TAG_GX, "RDSPAF DL end list=0x%08x size=0x%x r3=0x%08x\n",
                cpu->gpr[3], cpu->gpr[4], cpu->gpr[3]);
        ++s_endLogCount;
    }
}

} // namespace

// These wrappers intentionally call the translated retail bodies directly, so
// keep those BaseTranslated functions emitted while Native remains the runtime
// winner at the guest addresses.
REGISTER_TITLE_NATIVE_FUNCTION_AS(0x80231294, Meteor_GXBeginDisplayList,
                                  "Meteor_GXBeginDisplayList");
REGISTER_TITLE_NATIVE_FUNCTION_AS(0x80231344, Meteor_GXEndDisplayList,
                                  "Meteor_GXEndDisplayList");
