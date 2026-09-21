#include "gx_internal.h"
#include "gx_stream_common.h"
#include "gx_cp_decode.h"
#include "isa/big_endian.h"
#include "isa/ppc_isa_context.h"
#include "gx_matrix_provenance.h"
#include "recomp_mod_loader.h"
#include "settings_overlay.h"

// Opcode constants and the stream helpers this file shares with gx_dl.cpp /
// gx_vertex.cpp; see gx_stream_common.h.
using namespace GxCmd;
using namespace GxStream;

HleGxState g_hleGxState;

namespace {
std::atomic<bool> g_cpRegisterVirtualizationEnabled{false};
std::atomic<bool> g_loggedCpRegisterVirtualization{false};
std::atomic<bool> g_loggedCpBreakpointHit{false};
uint32_t g_rawTlutSourcePhysical = 0;
bool g_pendingRawDisplayPresent = false;
}

namespace aurora::gx::fifo {
void init();
void clear_buffer();
void drain();
bool submit_raw_draw(GXPrimitive prim, GXVtxFmt fmt, const uint8_t* vertices, uint16_t vtxCount,
                     uint32_t vertexBytes);
}

namespace {

uint32_t CopyPhysicalToCachedGuest(uint32_t physical) noexcept {
    if (physical < Memory::kMem1Size) {
        return Memory::kMem1CachedBase + physical;
    }
    if (physical >= Memory::kMem2PhysicalBase && physical < Memory::kMem2PhysicalEnd) {
        return Memory::kMem2CachedBase + (physical - Memory::kMem2PhysicalBase);
    }
    return 0;
}

float ReadRawXfFloat(const uint8_t* data) noexcept {
    const uint32_t bits = ReadBE32(data);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

} // namespace

void GX_HLE_ApplyBPReg(uint8_t reg, uint32_t value) {
    value &= 0x00FFFFFFu;

    if (reg == 0x64u) {
        // LOADTLUT0 stores the 32-byte-aligned source physical address as
        // physicalAddress >> 5 in the BP register's full 24-bit payload.  Wii
        // MEM2 lives at physical 0x10000000+, so its encoded source uses payload
        // bits above bit 19 (for example 0x10CCAF80 >> 5 == 0x86657C).  Masking
        // this down to the GameCube-era low 20 bits silently aliases MEM2 TLUTs
        // into low/unmapped physical memory and feeds CI textures no palette.
        g_rawTlutSourcePhysical = value << 5;
    }

    // Keep Aurora's BP shadow authoritative for all passive state first.  The
    // copy execution below is the side effect real BP hardware performs when
    // register 0x52 is written after 0x4B has selected a destination.
    GXApplyBPReg(reg, value);

    if (reg == 0x65u) {
        const uint16_t tmemOffset = static_cast<uint16_t>(value & 0x3FFu);
        const uint32_t sizeUnits = (value >> 10) & 0x7FFu;
        const uint32_t byteSize = sizeUnits * 16u * sizeof(uint16_t);
        const uint32_t guestAddr = CopyPhysicalToCachedGuest(g_rawTlutSourcePhysical);
        void* const source = guestAddr != 0 && byteSize != 0
                                 ? GuestToHostPtr(guestAddr, byteSize)
                                 : nullptr;
        GXSetDirectTlutData(tmemOffset, source, byteSize);
    }

    // TX_SETIMAGE3 carries the texture base as a 32-byte-aligned Wii physical
    // address. Games are allowed to program these BP registers directly rather
    // than call GXLoadTexObj; in that case Aurora still needs the corresponding
    // host pointer in order to resolve/upload the sampled texture.
    uint8_t directTexMap = 0xFFu;
    if (reg >= 0x94u && reg <= 0x97u) {
        directTexMap = static_cast<uint8_t>(reg - 0x94u);
    } else if (reg >= 0xB4u && reg <= 0xB7u) {
        directTexMap = static_cast<uint8_t>(4u + reg - 0xB4u);
    }
    if (directTexMap != 0xFFu) {
        const uint32_t physical = value << 5;
        const uint32_t guestAddr = CopyPhysicalToCachedGuest(physical);
        GXSetDirectTextureData(directTexMap, guestAddr != 0 ? GuestToHostPtr(guestAddr) : nullptr);
    }

    // PE draw-sync token commands are consumed by the BP parser itself, so the
    // token becomes CPU-visible in the same FIFO order as the real PE. Nintendo
    // emits PETOKENINT (0x48) followed by PETOKEN (0x47) with the same token.
    // The former raises the TOKEN interrupt condition; the latter is a plain
    // current-token update and must not create a second interrupt.
    if (reg == 0x48u) {
        g_hleGxState.peTokenCurrent = static_cast<uint16_t>(value);
        g_hleGxState.peTokenInterruptQueue.push_back(static_cast<uint16_t>(value));
        return;
    }
    if (reg == 0x47u) {
        g_hleGxState.peTokenCurrent = static_cast<uint16_t>(value);
        return;
    }

    // PE FINISH (BP 0x45, command value bit 1) is a FIFO-ordered completion
    // request used by GXDrawDone.  Aurora may still have raw draw work queued
    // when the BP packet is decoded, so drain it to the host renderer before
    // making FINISH CPU-visible.  The actual interrupt is delivered later at
    // an interrupt-safe guest boundary through GX_HLE_ClaimPeFinishInterrupt.
    if (reg == 0x45u && (value & 0x000002u) != 0u) {
        aurora::gx::fifo::drain();
        GXDrawDone();
        if (!g_hleGxState.peFinishInterruptPending) {
            g_hleGxState.peFinishInterruptPending = true;
            g_hleGxState.peFinishInterruptDispatched = false;
        }
        return;
    }

    if (reg == 0x4Bu) {
        g_hleGxState.bpCopyDestPhysical = value << 5;
        g_hleGxState.bpCopyDestValid = true;
        return;
    }

    if (reg != 0x52u || !g_hleGxState.bpCopyDestValid) {
        return;
    }

    // GXCopyDisp/GXCopyTex encode clear in bit 11 and display-vs-texture copy
    // in bit 14 of the PE copy-control word.  This is an execution trigger,
    // not merely a register-cache update.
    const GXBool clear = (value & (1u << 11)) != 0 ? GX_TRUE : GX_FALSE;
    const bool copyToXfb = (value & (1u << 14)) != 0;
    const uint32_t physical = g_hleGxState.bpCopyDestPhysical;
    const uint32_t cachedGuest = CopyPhysicalToCachedGuest(physical);
    if (cachedGuest == 0 || !Memory::Contains(cachedGuest, 1)) {
        return;
    }

    EnsureAuroraFrameActive();
    void* const dest = GuestToHostPtr(physical);
    if (dest == nullptr) {
        return;
    }

    if (copyToXfb) {
        // GXCopyDisp drains Aurora's pending raw FIFO before resolving, which
        // preserves the hardware ordering of all draws preceding this BP
        // trigger without introducing an artificial guest-side wait.
        GXCopyDisp(dest, clear);
        ++g_gxFrameCount;
        GXMarkFrameWork();
        VI_HLE_SetXfbReady(cachedGuest);
        // Do not present from inside GX_HLE_ApplyBPReg: scalar FIFO decoding
        // still owns this five-byte BP packet until its caller consumes it.
        // The command-boundary helpers below finish the frame immediately
        // afterwards, matching the native GXCopyDisp HLE path without parser
        // re-entrancy.
        g_pendingRawDisplayPresent = true;
    } else {
        // Texture-copy triggers use the same BP destination/control pair.  The
        // Aurora copy path preserves GPU copy/readback semantics for consumers
        // that later bind or read the destination.
        GXCopyTex(dest, clear);
        GXMarkFrameWork();
    }
}

void GX_HLE_FinishPendingRawDisplayCopy() {
    if (!g_pendingRawDisplayPresent) {
        return;
    }

    g_pendingRawDisplayPresent = false;
    // GXCopyDisp above drains all raw FIFO work preceding the copy.  Join the
    // asynchronous renderer before adding the overlay for exactly the same
    // reason as GX__CopyDisp_8016fc38, then let VI_HLE_PresentFrame seal and
    // pace.  If the producer already missed a retrace, VI's pacer presents
    // immediately instead of quantizing a ~50 Hz producer down to 30/20 Hz.
    aurora_wait_for_frame_worker();
    settings_overlay::Draw();
    VI_HLE_PresentFrame(/*presentedXfb=*/true, /*paceToRetrace=*/true);
}

void GX_HLE_ResetFifoParser() {
    aurora::gx::fifo::init();
    g_hleGxState = HleGxState{};
    g_rawTlutSourcePhysical = 0;
    g_pendingRawDisplayPresent = false;
    g_cpRegisterVirtualizationEnabled.store(true, std::memory_order_release);
}

void GX_HLE_AbortFrameBackend() {
    // GXAbortFrame resets the GP so commands which have not completed are not
    // allowed to leak into subsequent work.  Aurora consumes decoded GX work
    // synchronously into the current EFB/render state; those completed effects
    // must survive an abort just as pixels already produced by the real GP do.
    // Therefore discard only FIFO bytes which have not been processed yet.
    GXAbortPendingPrimitive();
    aurora::gx::fifo::clear_buffer();

    // Preserve decoded GX state (VCD/VAT/array bindings etc.).  A hardware
    // abort only invalidates the in-flight command/primitive, not the SDK's
    // software shadow configuration.  Drop only the incremental parser state
    // which could otherwise splice a pre-abort partial packet into the next
    // frame's first command.
    g_hleGxState.vertsRemaining = 0;
    g_hleGxState.inBegin = false;
    g_hleGxState.auroraBeginCalled = false;
    g_hleGxState.currentAttr = GX_VA_NULL;
    g_hleGxState.currentComp = 0;
    g_hleGxState.fifoReadOffset = 0;
    g_hleGxState.fifoByteCount = 0;
}

namespace {

bool TryReachCpBreakpoint() {
    constexpr uint16_t kCpReadEnable = 0x0001u;
    constexpr uint16_t kCpBreakpointEnable = 0x0002u;

    if (g_hleGxState.cpBreakpointHit ||
        !g_hleGxState.cpBreakpointAddressValid ||
        (g_hleGxState.cpControl & (kCpReadEnable | kCpBreakpointEnable)) !=
            (kCpReadEnable | kCpBreakpointEnable)) {
        return g_hleGxState.cpBreakpointHit;
    }

    // A CP breakpoint at the current FIFO write pointer is reached only after
    // every byte before that pointer has been decoded. GXFlush emits a full
    // gather block before GXSetBreakPt, so the normal title path arrives here
    // at a clean packet boundary. If a caller arms one mid-packet, leave it
    // pending rather than fabricating progress.
    if (g_hleGxState.fifoByteCount != 0 || g_hleGxState.inBegin || IsDisplayListActive()) {
        return false;
    }

    // Aurora's raw FIFO buffer is the host equivalent of commands already
    // written by the CPU but not yet consumed by the CP decoder. Draining it is
    // sufficient for the CP read pointer to reach the programmed boundary; a
    // PE/GPU completion fence would be stronger than the hardware contract.
    aurora::gx::fifo::drain();
    g_hleGxState.cpBreakpointHit = true;
    if (!g_loggedCpBreakpointHit.exchange(true, std::memory_order_relaxed)) {
        RT_LOG(RT_TAG_GX) << "CP breakpoint reached at physical 0x" << std::hex
                          << g_hleGxState.cpBreakpointPhysical << std::dec
                          << " after host FIFO decode" << std::endl;
    }
    return true;
}

void RebuildCpBreakpointAddress() {
    g_hleGxState.cpBreakpointPhysical =
        (static_cast<uint32_t>(g_hleGxState.cpBreakpointHigh & 0x3FFFu) << 16) |
        static_cast<uint32_t>(g_hleGxState.cpBreakpointLow);
    g_hleGxState.cpBreakpointAddressValid = true;
    g_hleGxState.cpBreakpointHit = false;
    g_hleGxState.cpBreakpointInterruptDispatched = false;
}

} // namespace

bool GX_HLE_CpBreakpointHit() {
    return TryReachCpBreakpoint();
}

bool GX_HLE_ClaimCpBreakpointInterrupt() {
    constexpr uint16_t kCpBreakpointInterruptEnable = 0x0020u;
    if (!TryReachCpBreakpoint() ||
        g_hleGxState.cpBreakpointInterruptDispatched ||
        (g_hleGxState.cpControl & kCpBreakpointInterruptEnable) == 0u) {
        return false;
    }

    g_hleGxState.cpBreakpointInterruptDispatched = true;
    return true;
}

bool GX_HLE_ClaimPeTokenInterrupt(uint16_t* token) {
    constexpr uint16_t kPeTokenInterruptEnable = 0x0001u;
    if (g_hleGxState.peTokenInterruptQueue.empty() ||
        g_hleGxState.peTokenInterruptDispatched ||
        (g_hleGxState.peControl & kPeTokenInterruptEnable) == 0u) {
        return false;
    }

    g_hleGxState.peTokenInterruptDispatched = true;
    if (token != nullptr) {
        *token = g_hleGxState.peTokenInterruptQueue.front();
    }
    return true;
}

bool GX_HLE_ClaimPeFinishInterrupt() {
    constexpr uint16_t kPeFinishInterruptEnable = 0x0002u;
    if (!g_hleGxState.peFinishInterruptPending ||
        g_hleGxState.peFinishInterruptDispatched ||
        (g_hleGxState.peControl & kPeFinishInterruptEnable) == 0u) {
        return false;
    }

    g_hleGxState.peFinishInterruptDispatched = true;
    return true;
}

bool GX_HLE_TryPeRegisterRead16(uint32_t addr, uint16_t* value) {
    constexpr uint32_t kPeZConfig = 0xCC001000u;
    constexpr uint32_t kPeAlphaConfig = 0xCC001002u;
    constexpr uint32_t kPeDstAlpha = 0xCC001004u;
    constexpr uint32_t kPeAlphaMode = 0xCC001006u;
    constexpr uint32_t kPeAlphaRead = 0xCC001008u;
    constexpr uint32_t kPeControl = 0xCC00100Au;
    constexpr uint32_t kPeToken = 0xCC00100Eu;
    if (value == nullptr) {
        return false;
    }

    switch (addr) {
    case kPeZConfig: *value = g_hleGxState.peZConfig; return true;
    case kPeAlphaConfig: *value = g_hleGxState.peAlphaConfig; return true;
    case kPeDstAlpha: *value = g_hleGxState.peDstAlpha; return true;
    case kPeAlphaMode: *value = g_hleGxState.peAlphaMode; return true;
    case kPeAlphaRead: *value = g_hleGxState.peAlphaRead; return true;
    default: break;
    }

    if (addr == kPeToken) {
        *value = g_hleGxState.peTokenCurrent;
        return true;
    }

    if (addr == kPeControl) {
        // Bits 0/1 are TOKEN/FINISH interrupt enables. Bits 2/3 expose the
        // corresponding pending conditions and are write-one-to-clear, which
        // is why the SDK handlers use a read/OR/write acknowledgement.
        uint16_t control = static_cast<uint16_t>(g_hleGxState.peControl & 0x0003u);
        if (!g_hleGxState.peTokenInterruptQueue.empty()) {
            control |= 0x0004u;
        }
        if (g_hleGxState.peFinishInterruptPending) {
            control |= 0x0008u;
        }
        *value = control;
        return true;
    }

    return false;
}

bool GX_HLE_TryPeRegisterWrite16(uint32_t addr, uint16_t val) {
    constexpr uint32_t kPeZConfig = 0xCC001000u;
    constexpr uint32_t kPeAlphaConfig = 0xCC001002u;
    constexpr uint32_t kPeDstAlpha = 0xCC001004u;
    constexpr uint32_t kPeAlphaMode = 0xCC001006u;
    constexpr uint32_t kPeAlphaRead = 0xCC001008u;
    constexpr uint32_t kPeControl = 0xCC00100Au;

    switch (addr) {
    case kPeZConfig: g_hleGxState.peZConfig = val; return true;
    case kPeAlphaConfig: g_hleGxState.peAlphaConfig = val; return true;
    case kPeDstAlpha: g_hleGxState.peDstAlpha = val; return true;
    case kPeAlphaMode: g_hleGxState.peAlphaMode = val; return true;
    case kPeAlphaRead: g_hleGxState.peAlphaRead = val; return true;
    default: break;
    }

    if (addr != kPeControl) {
        return false;
    }

    // PE control/status is a mixed enable + W1C register. Preserve only the
    // two proved enable bits; acknowledging a pending condition also releases
    // its host-delivery latch so a later BP event can interrupt again.
    g_hleGxState.peControl = static_cast<uint16_t>(val & 0x0003u);
    if ((val & 0x0004u) != 0u) {
        if (!g_hleGxState.peTokenInterruptQueue.empty()) {
            g_hleGxState.peTokenInterruptQueue.pop_front();
        }
        g_hleGxState.peTokenInterruptDispatched = false;
    }
    if ((val & 0x0008u) != 0u) {
        g_hleGxState.peFinishInterruptPending = false;
        g_hleGxState.peFinishInterruptDispatched = false;
    }
    return true;
}

bool GX_HLE_TryCpRegisterRead16(uint32_t addr, uint16_t* value) {
    constexpr uint32_t kCpMmioBase = 0xCC000000u;
    if (value == nullptr ||
        !g_cpRegisterVirtualizationEnabled.load(std::memory_order_acquire)) {
        return false;
    }

    switch (addr - kCpMmioBase) {
    case 0x30u: *value = g_hleGxState.cpRwDistanceLow; return true;
    case 0x32u: *value = g_hleGxState.cpRwDistanceHigh; return true;
    case 0x38u: *value = g_hleGxState.cpReadPointerLow; return true;
    case 0x3Au: *value = g_hleGxState.cpReadPointerHigh; return true;
    default: return false;
    }
}

bool GX_HLE_TryCpRegisterWrite16(uint32_t addr, uint16_t val) {
    constexpr uint32_t kCpMmioBase = 0xCC000000u;
    constexpr uint32_t kCpMmioEnd = 0xCC000080u;
    if (!g_cpRegisterVirtualizationEnabled.load(std::memory_order_acquire) ||
        addr < kCpMmioBase || addr >= kCpMmioEnd || (addr & 1u) != 0) {
        return false;
    }

    const uint32_t reg = addr - kCpMmioBase;
    switch (reg) {
    case 0x02u: { // CP control
        constexpr uint16_t kCpBreakpointEnable = 0x0002u;
        const uint16_t oldControl = g_hleGxState.cpControl;
        g_hleGxState.cpControl = val;
        if ((val & kCpBreakpointEnable) == 0u) {
            g_hleGxState.cpBreakpointHit = false;
            g_hleGxState.cpBreakpointInterruptDispatched = false;
        } else if ((oldControl & kCpBreakpointEnable) == 0u) {
            // A newly armed breakpoint must be reached afresh even if the same
            // address was used by a previous frame.
            g_hleGxState.cpBreakpointHit = false;
            g_hleGxState.cpBreakpointInterruptDispatched = false;
        }
        (void)TryReachCpBreakpoint();
        break;
    }
    case 0x3Cu: // breakpoint address low 16
        g_hleGxState.cpBreakpointLow = val;
        RebuildCpBreakpointAddress();
        break;
    case 0x3Eu: // breakpoint address high 14 (cached/uncached tag stripped)
        g_hleGxState.cpBreakpointHigh = val;
        RebuildCpBreakpointAddress();
        break;
    case 0x30u: // FIFO read/write distance low 16
        g_hleGxState.cpRwDistanceLow = val;
        break;
    case 0x32u: // FIFO read/write distance high 16
        g_hleGxState.cpRwDistanceHigh = val;
        break;
    case 0x38u: // FIFO read pointer low 16
        g_hleGxState.cpReadPointerLow = val;
        break;
    case 0x3Au: // FIFO read pointer high 14
        g_hleGxState.cpReadPointerHigh = val;
        break;
    default:
        break;
    }

    // The remaining CP register stores are still hardware-only programming
    // owned by the host GX command parser. Reads remain strict; only the modeled
    // breakpoint state above is surfaced through the title's GXGetGPStatus.
    if (!g_loggedCpRegisterVirtualization.exchange(true, std::memory_order_relaxed)) {
        RT_LOG(RT_TAG_GX) << "CP register writes virtualized by host GX FIFO backend" << std::endl;
    }
    return true;
}

void HleGxState::ResetVertex() {
    currentAttr = NextEnabledAttr(GX_VA_PNMTXIDX - 1);
    currentComp = 0;
}

GXAttr HleGxState::NextEnabledAttr(int startAttr) {
    for (int i = startAttr + 1; i < 26; ++i) {
        if (vtxDesc[i] != GX_NONE) {
            return static_cast<GXAttr>(i);
        }
    }
    return GX_VA_NULL;
}

int HleGxState::GetExpectedCompCount(GXAttr attr, const VtxAttrFmt& fmt) {
    // Attributes with no component layout (matrix indices, XF arrays, GX_VA_NBT)
    // count as one component here: the incremental parser advances one element
    // per raw stream item for them.
    return static_cast<int>(AttrCompCount(attr, fmt, 1u));
}

static void ApplyAuroraVtxStateForRawBegin(GXVtxFmt fmt) {
    // GX_VA_NBT is an SDK pseudo-attribute that aliases the normal descriptor/VAT slot.
    // g_hleGxState keeps the real normal layout in GX_VA_NRM and leaves GX_VA_NBT at its
    // default VtxAttrFmt. Publishing that default after GX_VA_NRM rewrites an XYZ normal
    // VAT as NBT/F32, so Aurora expects nine normal components while the raw FIFO producer
    // emits the three components described by GX_VA_NRM. Publish the canonical NRM slot only.
    PublishAuroraVtxState(fmt, AuroraVtxPublishOptions{/*includeNbt=*/false,
                                                       /*fmtLoopFirst=*/0,
                                                       /*fmtLoopLast=*/25});
}

// There is deliberately no indexed-aurora path here. Immediate-mode indexed
// draws are parsed incrementally, and aurora's indexed-array upload has to
// precede the draw carrying the max-index bounds, which cannot be known without
// buffering the whole primitive. Indexed attributes are therefore expanded into
// the packed direct stream above (GX_INDEX8/16 -> GX_DIRECT).

static uint32_t GetDirectAttrByteSizeForFifo(GXAttr attr, const VtxAttrFmt& fmt) {
    return DirectAttrByteSize(attr, fmt, /*matrixAttrIsOneByte=*/true, /*fallbackComps=*/1u);
}

static bool TryGetRawDirectFifoVertexSize(GXVtxFmt fmt, uint32_t& vertexSize) {
    vertexSize = 0;
    if (fmt >= GX_MAX_VTXFMT) {
        return false;
    }

    for (int attr = 0; attr < 26; ++attr) {
        const GXAttrType type = g_hleGxState.vtxDesc[attr];
        if (type == GX_NONE) {
            continue;
        }
        if (type != GX_DIRECT) {
            return false;
        }

        const GXAttr gxAttr = static_cast<GXAttr>(attr);
        const uint32_t attrBytes =
            GetDirectAttrByteSizeForFifo(gxAttr, g_hleGxState.vtxAttrFmt[fmt][attr]);
        if (attrBytes == 0) {
            return false;
        }
        vertexSize += attrBytes;
    }

    return vertexSize != 0;
}

static bool TrySubmitRawDirectFifoDraw(const uint8_t* packet, uint32_t packetBytes, GXPrimitive prim,
                                       GXVtxFmt vtxFmt, uint16_t vtxCount) {
    if (packet == nullptr || packetBytes == 0 || vtxCount == 0) {
        return false;
    }

    EnsureAuroraFrameActive();

    g_hleGxState.currentVtxFmt = vtxFmt;
    g_hleGxState.currentPrim = prim;
    g_hleGxState.vertsRemaining = vtxCount;
    g_hleGxState.inBegin = false;
    g_hleGxState.auroraBeginCalled = false;
    g_hleGxState.ResetVertex();

    ApplyAuroraVtxStateForRawBegin(vtxFmt);
    EnsureDefaultGxAlphaCompare();

    const bool submitted = aurora::gx::fifo::submit_raw_draw(prim, vtxFmt, packet + 3, vtxCount, packetBytes - 3u);
    static std::atomic<uint32_t> s_rawDrawLogCount{0};
    const uint32_t rawDrawLog = s_rawDrawLogCount.fetch_add(1, std::memory_order_relaxed);
    if (rawDrawLog < 24u) {
        RT_LOGF(RT_TAG_GX, "raw direct draw #%u prim=%u fmt=%u verts=%u bytes=0x%x submitted=%u\n",
                rawDrawLog, static_cast<unsigned>(prim), static_cast<unsigned>(vtxFmt),
                static_cast<unsigned>(vtxCount), packetBytes, submitted ? 1u : 0u);
    }
    if (!submitted) {
        return false;
    }
    GXMarkFrameWork();
    return true;
}

static void DecodeColorFromArray(uint32_t addr, GXCompType type, GXCompCnt cnt, GXColor& out) {
    // Gather exactly the bytes the shared decoder consumes. GX_RGBX8 is the one
    // format whose stream footprint (4 bytes) exceeds what is decoded (3), and
    // the pre-dedup code read only those 3 from the guest array - keep it that
    // way so a 3-byte tail at the end of an array cannot start faulting.
    const uint32_t byteCount =
        (type == GX_RGBX8) ? 3u : ColorByteSize(type, cnt);
    uint8_t bytes[4] = {0, 0, 0, 0};
    for (uint32_t i = 0; i < byteCount && i < 4u; ++i) {
        bytes[i] = Memory::Read8(addr + i);
    }
    DecodeColorBytes(bytes, type, cnt, out);
}

void SubmitIndexedAttribute(GXAttr attr, uint32_t index) {
    const auto& fmt = g_hleGxState.vtxAttrFmt[g_hleGxState.currentVtxFmt][attr];
    const auto& arr = g_hleGxState.vtxArray[attr];
    if (arr.base == 0 || arr.stride == 0) {
        float comps[9]{};
        switch (attr) {
        case GX_VA_CLR0:
        case GX_VA_CLR1:
            if (fmt.cnt == GX_CLR_RGB) {
                GXColor3u8(0, 0, 0);
            } else {
                GXColor4u8(0, 0, 0, 0xFF);
            }
            break;
        case GX_VA_POS:
        case GX_VA_NRM:
        case GX_VA_TEX0:
        case GX_VA_TEX1:
        case GX_VA_TEX2:
        case GX_VA_TEX3:
        case GX_VA_TEX4:
        case GX_VA_TEX5:
        case GX_VA_TEX6:
        case GX_VA_TEX7:
            SubmitAttribute(attr, comps, fmt);
            break;
        default:
            break;
        }
        return;
    }
    const uint32_t baseAddr = arr.base + index * arr.stride;
    float comps[9]{};
    u32 rawComps[9]{};
    switch (attr) {
    case GX_VA_POS: {
        const int count = static_cast<int>(AttrCompCount(GX_VA_POS, fmt, 0u));
        const int step = GetCompSizeBytes(fmt.type);
        uint32_t addr = baseAddr;
        for (int i = 0; i < count; ++i, addr += step) {
            rawComps[i] = ReadArrayRawComp(addr, fmt.type);
            comps[i] = ConvertCompToFloat(rawComps[i], fmt.type, fmt.frac);
        }
        SubmitAttribute(attr, comps, fmt, rawComps);
        break;
    }
    case GX_VA_NRM: {
        const int count = static_cast<int>(AttrCompCount(GX_VA_NRM, fmt, 0u));
        const int step = GetCompSizeBytes(fmt.type);
        uint32_t addr = baseAddr;
        for (int i = 0; i < count; ++i, addr += step) {
            rawComps[i] = ReadArrayRawComp(addr, fmt.type);
            comps[i] = ConvertCompToFloat(rawComps[i], fmt.type, fmt.frac);
        }
        SubmitAttribute(attr, comps, fmt, rawComps);
        break;
    }
    case GX_VA_CLR0:
    case GX_VA_CLR1: {
        GXColor color{};
        DecodeColorFromArray(baseAddr, fmt.type, fmt.cnt, color);
        if (fmt.cnt == GX_CLR_RGB) {
            GXColor3u8(color.r, color.g, color.b);
        } else {
            GXColor4u8(color.r, color.g, color.b, color.a);
        }
        break;
    }
    case GX_VA_TEX0: case GX_VA_TEX1: case GX_VA_TEX2: case GX_VA_TEX3:
    case GX_VA_TEX4: case GX_VA_TEX5: case GX_VA_TEX6: case GX_VA_TEX7: {
        // Every GX_VA_TEXn shares one component layout, so the constant here is
        // exact for whichever of them `attr` is.
        const int count = static_cast<int>(AttrCompCount(GX_VA_TEX0, fmt, 0u));
        const int step = GetCompSizeBytes(fmt.type);
        uint32_t addr = baseAddr;
        for (int i = 0; i < count; ++i, addr += step) {
            rawComps[i] = ReadArrayRawComp(addr, fmt.type);
            comps[i] = ConvertCompToFloat(rawComps[i], fmt.type, fmt.frac);
        }
        SubmitAttribute(attr, comps, fmt, rawComps);
        break;
    }
    default:
        break;
    }
}

// Mirrors the raw-integer fast paths in SubmitAttribute below: for those
// (attr, type) pairs the float component buffer is never read, so the direct
// FIFO path can skip building it.
static bool SubmitAttributeReadsFloatComps(GXAttr attr, const VtxAttrFmt& fmt) {
    switch (attr) {
    case GX_VA_POS:
    case GX_VA_NRM:
    case GX_VA_TEX0: case GX_VA_TEX1: case GX_VA_TEX2: case GX_VA_TEX3:
    case GX_VA_TEX4: case GX_VA_TEX5: case GX_VA_TEX6: case GX_VA_TEX7:
        break;
    default:
        return true;
    }
    switch (fmt.type) {
    case GX_U8:
    case GX_S8:
    case GX_U16:
    case GX_S16:
        return false;
    default:
        return true;
    }
}

void SubmitAttribute(GXAttr attr, float* comps, const VtxAttrFmt& fmt, const u32* rawComps) {
    auto r8 = [rawComps](int idx) { return static_cast<u8>(rawComps[idx]); };
    auto rs8 = [rawComps](int idx) { return static_cast<s8>(rawComps[idx]); };
    auto r16 = [rawComps](int idx) { return static_cast<u16>(rawComps[idx]); };
    auto rs16 = [rawComps](int idx) { return static_cast<s16>(rawComps[idx]); };

    switch (attr) {
    case GX_VA_POS:
        if (rawComps) {
            if (fmt.cnt == GX_POS_XY) {
                switch (fmt.type) {
                case GX_U8: GXPosition2u8(r8(0), r8(1)); return;
                case GX_S8: GXPosition2s8(rs8(0), rs8(1)); return;
                case GX_U16: GXPosition2u16(r16(0), r16(1)); return;
                case GX_S16: GXPosition2s16(rs16(0), rs16(1)); return;
                default: break;
                }
            } else {
                switch (fmt.type) {
                case GX_U8: GXPosition3u8(r8(0), r8(1), r8(2)); return;
                case GX_S8: GXPosition3s8(rs8(0), rs8(1), rs8(2)); return;
                case GX_U16: GXPosition3u16(r16(0), r16(1), r16(2)); return;
                case GX_S16: GXPosition3s16(rs16(0), rs16(1), rs16(2)); return;
                default: break;
                }
            }
        }
        if (fmt.cnt == GX_POS_XY) {
            GXPosition2f32(comps[0], comps[1]);
        } else {
            GXPosition3f32(comps[0], comps[1], comps[2]);
        }
        break;
    case GX_VA_NRM:
        if (rawComps) {
            const bool nbt = fmt.cnt == GX_NRM_NBT || fmt.cnt == GX_NRM_NBT3;
            const int groups = nbt ? 3 : 1;
            switch (fmt.type) {
            case GX_U8:
                for (int i = 0; i < groups; ++i) {
                    GXNormal3u8(r8(i * 3), r8(i * 3 + 1), r8(i * 3 + 2));
                }
                return;
            case GX_S8:
                for (int i = 0; i < groups; ++i) {
                    GXNormal3s8(rs8(i * 3), rs8(i * 3 + 1), rs8(i * 3 + 2));
                }
                return;
            case GX_U16:
                for (int i = 0; i < groups; ++i) {
                    GXNormal3u16(r16(i * 3), r16(i * 3 + 1), r16(i * 3 + 2));
                }
                return;
            case GX_S16:
                for (int i = 0; i < groups; ++i) {
                    GXNormal3s16(rs16(i * 3), rs16(i * 3 + 1), rs16(i * 3 + 2));
                }
                return;
            default: break;
            }
        }
        if (fmt.cnt == GX_NRM_NBT || fmt.cnt == GX_NRM_NBT3) {
            for (int i = 0; i < 3; ++i) {
                GXNormal3f32(comps[i * 3], comps[i * 3 + 1], comps[i * 3 + 2]);
            }
        } else {
            GXNormal3f32(comps[0], comps[1], comps[2]);
        }
        break;
    case GX_VA_CLR0:
    case GX_VA_CLR1:
        if (fmt.cnt == GX_CLR_RGB) {
            GXColor3u8(static_cast<u8>(comps[0]), static_cast<u8>(comps[1]),
                       static_cast<u8>(comps[2]));
        } else {
            GXColor4u8(static_cast<u8>(comps[0]), static_cast<u8>(comps[1]),
                       static_cast<u8>(comps[2]), static_cast<u8>(comps[3]));
        }
        break;
    case GX_VA_TEX0: case GX_VA_TEX1: case GX_VA_TEX2: case GX_VA_TEX3:
    case GX_VA_TEX4: case GX_VA_TEX5: case GX_VA_TEX6: case GX_VA_TEX7:
        if (rawComps) {
            if (fmt.cnt == GX_TEX_S) {
                switch (fmt.type) {
                case GX_U8: GXTexCoord1u8(r8(0)); return;
                case GX_S8: GXTexCoord1s8(rs8(0)); return;
                case GX_U16: GXTexCoord1u16(r16(0)); return;
                case GX_S16: GXTexCoord1s16(rs16(0)); return;
                default: break;
                }
            } else {
                switch (fmt.type) {
                case GX_U8: GXTexCoord2u8(r8(0), r8(1)); return;
                case GX_S8: GXTexCoord2s8(rs8(0), rs8(1)); return;
                case GX_U16: GXTexCoord2u16(r16(0), r16(1)); return;
                case GX_S16: GXTexCoord2s16(rs16(0), rs16(1)); return;
                default: break;
                }
            }
        }
        if (fmt.cnt == GX_TEX_S) {
            GXTexCoord1f32(comps[0]);
        } else {
            GXTexCoord2f32(comps[0], comps[1]);
        }
        break;
    default: break;
    }
}

// `val` is a raw big-endian bit pattern: the FIFO stream is type-agnostic, and
// the float entry point converts before it gets here.
void HleFifoWrite(u32 val, uint32_t sizeBytes) {
    const bool recordOnly = IsDisplayListActive();
    if (recordOnly) {
        WriteDisplayListData(val, sizeBytes);
        return;
    }

    auto resetFifoBuffer = [&]() {
        g_hleGxState.fifoReadOffset = 0;
        g_hleGxState.fifoByteCount = 0;
    };

    auto compactFifoBuffer = [&]() {
        if (g_hleGxState.fifoByteCount == 0) {
            g_hleGxState.fifoReadOffset = 0;
            return;
        }
        if (g_hleGxState.fifoReadOffset == 0) {
            return;
        }
        std::memmove(g_hleGxState.fifoBytes.data(),
                     g_hleGxState.fifoBytes.data() + g_hleGxState.fifoReadOffset,
                     g_hleGxState.fifoByteCount);
        g_hleGxState.fifoReadOffset = 0;
    };

    auto fifoData = [&]() -> uint8_t* {
        return g_hleGxState.fifoBytes.data() + g_hleGxState.fifoReadOffset;
    };

    auto pushBytes = [&](u32 value, uint32_t count) {
        if (count == 0) return;
        if (count > 4) count = 4;
        if (g_hleGxState.fifoReadOffset + g_hleGxState.fifoByteCount + count > g_hleGxState.fifoBytes.size()) {
            compactFifoBuffer();
            if (g_hleGxState.fifoReadOffset + g_hleGxState.fifoByteCount + count > g_hleGxState.fifoBytes.size()) {
                resetFifoBuffer();
            }
        }
        const size_t writeOffset = g_hleGxState.fifoReadOffset + g_hleGxState.fifoByteCount;
        switch (count) {
        case 4:
            BigEndian::Write32(g_hleGxState.fifoBytes.data() + writeOffset, value);
            break;
        case 2:
            BigEndian::Write16(g_hleGxState.fifoBytes.data() + writeOffset,
                               static_cast<uint16_t>(value));
            break;
        default:
            g_hleGxState.fifoBytes[writeOffset] = static_cast<uint8_t>(value & 0xFF);
            break;
        }
        g_hleGxState.fifoByteCount += count;
    };

    auto consumeBytes = [&](uint32_t count, u32& out) -> bool {
        if (count == 0 || g_hleGxState.fifoByteCount < count) return false;
        u32 value = 0;
        const uint32_t foldCount = (count > 4) ? 4 : count;
        uint8_t* data = fifoData();
        for (uint32_t i = 0; i < foldCount; ++i) {
            value = (value << 8) | data[i];
        }
        g_hleGxState.fifoReadOffset += count;
        g_hleGxState.fifoByteCount -= count;
        if (g_hleGxState.fifoByteCount == 0) {
            g_hleGxState.fifoReadOffset = 0;
        }
        out = value;
        return true;
    };

    pushBytes(val, sizeBytes == 0 ? 4 : sizeBytes);

    // Parse raw FIFO command packets written directly to the gather pipe
    // (e.g. NW4R/G3D paths that do not call the GXBegin wrapper function).
    while (!g_hleGxState.inBegin) {
        if (g_hleGxState.fifoByteCount < 1) {
            break;
        }

        uint8_t* data = fifoData();
        const uint8_t cmd = data[0];
        const uint8_t opcode = cmd & GX_OPCODE_MASK_CMD;
        u32 sink = 0;

        if (cmd == GX_NOP_CMD) {
            if (!consumeBytes(1, sink)) break;
            continue;
        }

        if (opcode == GX_CMD_INVL_VC_CMD) {
            // CPU-written indexed attributes must be fetched again after this
            // command. It is not a NOP: Aurora caches their uploaded bytes.
            GXCallDisplayList(data, 1);
            GXMarkFrameWork();
            if (!consumeBytes(1, sink)) break;
            continue;
        }

        if (cmd == GX_LOAD_BP_REG_CMD) {
            if (g_hleGxState.fifoByteCount < 5) break;
            const uint32_t bpWord = ReadBE32(data + 1);
            const uint8_t bpReg = static_cast<uint8_t>(bpWord >> 24);
            GX_HLE_ApplyBPReg(bpReg, bpWord & 0x00FFFFFFu);
            if (!consumeBytes(5, sink)) break;
            GX_HLE_FinishPendingRawDisplayCopy();
            continue;
        }

        if (opcode == GX_LOAD_CP_REG_CMD) {
            if (g_hleGxState.fifoByteCount < 6) break;
            const uint8_t reg = data[1];
            const uint32_t cpValue = ReadBE32(data + 2);
            GxCpDecode::ApplyCpRegWrite(reg, cpValue);
            // Matrix indices have no HLE vertex-layout shadow. Forward these
            // execution-time writes, just as the display-list interpreter does.
            // Do not put this side effect in ApplyCpRegWrite: scan/cache walks
            // also call that helper and must remain renderer-side-effect free.
            if (reg == 0x30 || reg == 0x40) {
                GXCallDisplayList(data, 6);
                GXMarkFrameWork();
            }
            if (!consumeBytes(6, sink)) break;
            continue;
        }

        if (opcode == GX_LOAD_XF_REG_CMD) {
            if (g_hleGxState.fifoByteCount < 5) break;
            const uint16_t countWords = ReadBE16(data + 1);
            const uint32_t packetBytes = 1u + 4u + (static_cast<uint32_t>(countWords) + 1u) * 4u;
            if (g_hleGxState.fifoByteCount < packetBytes) break;
            GxMatrixProvenance::Observe(data, packetBytes);
            GXCallDisplayList(data, packetBytes);
            GXMarkFrameWork();
            if (!consumeBytes(packetBytes, sink)) break;
            continue;
        }

        if (opcode >= GX_LOAD_INDX_A_CMD && opcode <= GX_LOAD_INDX_D_CMD) {
            if (g_hleGxState.fifoByteCount < 5) break;
            const uint32_t xfValue = ReadBE32(data + 1);
            ApplyIndexedXfArrayForPacket(cmd, xfValue);
            GXCallDisplayList(data, 5);
            GXMarkFrameWork();
            if (!consumeBytes(5, sink)) break;
            continue;
        }

        if (opcode == GX_CMD_CALL_DL_CMD) {
            if (g_hleGxState.fifoByteCount < 9) break;
            const uint32_t listAddr = ReadBE32(data + 1);
            const uint32_t listSize = ReadBE32(data + 5);
            if (!consumeBytes(9, sink)) break;
            if (listAddr != 0 && listSize > 0) {
                GX__CallDisplayList_80172f64(listAddr, listSize);
            }
            continue;
        }

        if (IsDrawOpcode(opcode)) {
            if (g_hleGxState.fifoByteCount < 3) break;
            const uint16_t vtxCount = ReadBE16(data + 1);
            const GXVtxFmt vtxFmt = static_cast<GXVtxFmt>(cmd & GX_VAT_MASK_CMD);
            const GXPrimitive prim = OpcodeToGXPrimitive(cmd);
            uint32_t rawVertexSize = 0;
            if (TryGetRawDirectFifoVertexSize(vtxFmt, rawVertexSize)) {
                const uint32_t packetBytes = 3u + static_cast<uint32_t>(vtxCount) * rawVertexSize;
                if (g_hleGxState.fifoByteCount < packetBytes) {
                    break;
                }
                if (TrySubmitRawDirectFifoDraw(data, packetBytes, prim, vtxFmt, vtxCount)) {
                    if (!consumeBytes(packetBytes, sink)) break;
                    continue;
                }
            }
            if (!consumeBytes(3, sink)) break;

            g_hleGxState.currentVtxFmt = vtxFmt;
            g_hleGxState.currentPrim = prim;
            g_hleGxState.vertsRemaining = vtxCount;
            g_hleGxState.inBegin = true;
            g_hleGxState.auroraBeginCalled = false;
            g_hleGxState.ResetVertex();

            break;
        }

        // Unknown FIFO command byte outside a begin packet; discard it so stream parsing can recover.
        if (!consumeBytes(1, sink)) break;
    }

    if (!g_hleGxState.inBegin) {
        return;
    }

    if (!recordOnly && !g_hleGxState.auroraBeginCalled) {
        EnsureAuroraFrameActive();
        ApplyAuroraVtxStateForRawBegin(g_hleGxState.currentVtxFmt);
        EnsureDefaultGxAlphaCompare();
        GXBegin(g_hleGxState.currentPrim, g_hleGxState.currentVtxFmt, static_cast<u16>(g_hleGxState.vertsRemaining));
        g_hleGxState.auroraBeginCalled = true;
        GXMarkFrameWork();
    }

    auto finishVertexIfNeeded = [&](GXAttr prevAttr) {
        g_hleGxState.currentAttr = g_hleGxState.NextEnabledAttr(prevAttr);
        g_hleGxState.currentComp = 0;
        if (g_hleGxState.currentAttr == GX_VA_NULL) {
            g_hleGxState.ResetVertex();
            if (g_hleGxState.vertsRemaining > 0) {
                g_hleGxState.vertsRemaining--;
                if (g_hleGxState.vertsRemaining == 0) {
                    g_hleGxState.inBegin = false;
                    g_hleGxState.auroraBeginCalled = false;
                    if (!recordOnly) {
                        GXEnd();
                    }
                }
            }
        }
    };

    while (true) {
        if (!g_hleGxState.inBegin) {
            break;
        }
        GXAttr attr = g_hleGxState.currentAttr;
        if (attr == GX_VA_NULL) {
            resetFifoBuffer();
            return;
        }

        GXAttrType inputType = g_hleGxState.vtxDesc[attr];
        const VtxAttrFmt& fmt = g_hleGxState.vtxAttrFmt[g_hleGxState.currentVtxFmt][attr];

        if (IsMatrixIndexAttr(attr)) {
            if (g_hleGxState.fifoByteCount < 1) break;
            u32 raw = 0;
            if (!consumeBytes(1, raw)) break;
            if (!recordOnly) {
                GXMatrixIndex1u8(attr, static_cast<u8>(raw));
            }
            finishVertexIfNeeded(attr);
            continue;
        }

        if (inputType == GX_DIRECT) {
            if (attr == GX_VA_CLR0 || attr == GX_VA_CLR1) {
                const uint32_t colorSize = ColorByteSize(fmt.type, fmt.cnt);

                if (g_hleGxState.fifoByteCount < colorSize) break;

                uint8_t colorBytes[4] = {0, 0, 0, 0};
                uint8_t* data = fifoData();
                for (uint32_t i = 0; i < colorSize && i < 4; ++i) {
                    colorBytes[i] = data[i];
                }
                g_hleGxState.fifoReadOffset += colorSize;
                g_hleGxState.fifoByteCount -= colorSize;
                if (g_hleGxState.fifoByteCount == 0) {
                    g_hleGxState.fifoReadOffset = 0;
                }

                GXColor color{};
                DecodeColorBytes(colorBytes, fmt.type, fmt.cnt, color);

                if (!recordOnly) {
                    if (fmt.cnt == GX_CLR_RGB) {
                        GXColor3u8(color.r, color.g, color.b);
                    } else {
                        GXColor4u8(color.r, color.g, color.b, color.a);
                    }
                }
                finishVertexIfNeeded(attr);
                continue;
            }
            
            const int compSize = GetCompSizeBytes(fmt.type);
            if (compSize <= 0 || g_hleGxState.fifoByteCount < static_cast<size_t>(compSize)) break;
            u32 raw = 0;
            if (!consumeBytes(static_cast<uint32_t>(compSize), raw)) break;
            constexpr int kMaxComps = 9;
            if (g_hleGxState.currentComp < kMaxComps) {
                if (SubmitAttributeReadsFloatComps(attr, fmt)) {
                    g_hleGxState.compBuffer[g_hleGxState.currentComp] =
                        ConvertCompToFloat(raw, fmt.type, fmt.frac);
                }
                g_hleGxState.rawCompBuffer[g_hleGxState.currentComp] = raw;
            }
            g_hleGxState.currentComp++;
            const int expected = g_hleGxState.GetExpectedCompCount(attr, fmt);
            if (g_hleGxState.currentComp >= expected) {
                if (!recordOnly) {
                    SubmitAttribute(attr, g_hleGxState.compBuffer, fmt, g_hleGxState.rawCompBuffer);
                }
                finishVertexIfNeeded(attr);
            }
            continue;
        }

        if (inputType == GX_INDEX8 || inputType == GX_INDEX16) {
            const uint32_t idxSize = (inputType == GX_INDEX8) ? 1u : 2u;
            const uint32_t indexCount = (attr == GX_VA_NRM) ? NormalIndexCount(fmt) : 1u;
            if (g_hleGxState.fifoByteCount < idxSize * indexCount) break;
            u32 raw[3]{};
            for (uint32_t i = 0; i < indexCount; ++i) {
                if (!consumeBytes(idxSize, raw[i])) break;
            }
            if (!recordOnly) {
                if (attr == GX_VA_NRM && indexCount == 3u) {
                    SubmitIndexedNormalNBT3(raw, fmt);
                } else {
                    SubmitIndexedAttribute(attr, raw[0]);
                }
            }
            finishVertexIfNeeded(attr);
            continue;
        }

        g_hleGxState.currentAttr = g_hleGxState.NextEnabledAttr(attr);
        g_hleGxState.currentComp = 0;
        if (g_hleGxState.currentAttr == GX_VA_NULL) {
            g_hleGxState.ResetVertex();
        }
        break;
    }
}

// Must behave exactly like GX_HLE_FIFO_Write32/16/8 run byte by byte. The ring is not
// bulk-appended ahead of a single parse because the parser can re-enter GX HLE (nested display
// lists, XF/draw calls), which would misorder that work relative to the stream.
static void HleFifoWriteBurstChunked(const uint8_t* data, uint32_t sizeBytes) {
    uint32_t offset = 0;
    for (; offset + 4u <= sizeBytes; offset += 4u) {
        HleFifoWrite(ReadBE32(data + offset), 4);
    }
    if (offset + 2u <= sizeBytes) {
        HleFifoWrite(static_cast<u32>(ReadBE16(data + offset)), 2);
        offset += 2u;
    }
    if (offset < sizeBytes) {
        HleFifoWrite(static_cast<u32>(data[offset]), 1);
    }
}

// Applies complete register-load packets straight to the parser's own entry points, skipping the
// serialize/ring/re-parse round trip; returns bytes consumed, caller hands the rest to
// HleFifoWriteBurstChunked. Safe (per gd_fifo_hle.cpp's GdCanApplyDirect) only on a packet
// boundary with nothing buffered, outside a GXBegin packet and outside display-list recording;
// any other opcode or a truncated packet just stops the walk.
static uint32_t ApplyFifoPacketsDirect(const uint8_t* data, uint32_t sizeBytes) {
    uint32_t offset = 0;

    while (offset < sizeBytes) {
        // Re-tested per packet, not once per burst: nothing currently re-enters GX HLE mid-walk,
        // but if it ever does, breaking here just hands the remainder to the ring.
        if (IsDisplayListActive() || g_hleGxState.inBegin || g_hleGxState.fifoByteCount != 0) {
            break;
        }

        const uint8_t* packet = data + offset;
        const uint32_t avail = sizeBytes - offset;
        const uint8_t cmd = packet[0];
        const uint8_t opcode = cmd & GX_OPCODE_MASK_CMD;

        if (cmd == GX_NOP_CMD) {
            offset += 1u;
            continue;
        }

        if (cmd == GX_LOAD_BP_REG_CMD) {
            if (avail < 5u) break;
            const uint32_t bpWord = ReadBE32(packet + 1);
            const uint8_t bpReg = static_cast<uint8_t>(bpWord >> 24);
            GX_HLE_ApplyBPReg(bpReg, bpWord & 0x00FFFFFFu);
            offset += 5u;
            GX_HLE_FinishPendingRawDisplayCopy();
            continue;
        }

        if (opcode == GX_LOAD_CP_REG_CMD) {
            if (avail < 6u) break;
            const uint8_t reg = packet[1];
            const uint32_t cpValue = ReadBE32(packet + 2);
            GxCpDecode::ApplyCpRegWrite(reg, cpValue);
            // Match the scalar FIFO execution path without adding renderer
            // side effects to the CP helper used by display-list scans.
            if (reg == 0x30 || reg == 0x40) {
                GXCallDisplayList(packet, 6);
                GXMarkFrameWork();
            }
            offset += 6u;
            continue;
        }

        if (opcode == GX_LOAD_XF_REG_CMD) {
            if (avail < 5u) break;
            const uint16_t countWords = ReadBE16(packet + 1);
            const uint32_t packetBytes = 1u + 4u + (static_cast<uint32_t>(countWords) + 1u) * 4u;
            if (avail < packetBytes) break;
            GxMatrixProvenance::Observe(packet, packetBytes);
            GXCallDisplayList(packet, packetBytes);
            GXMarkFrameWork();
            offset += packetBytes;
            continue;
        }

        break;
    }

    return offset;
}

// Display-list recording just copies bytes into the guest list buffer and advances the shadow
// cursor, so a burst is one logical append as long as it doesn't cross the list end; a burst that
// would wrap falls back to the per-write path instead. Returns false to signal that fallback.
static bool WriteDisplayListBurst(const uint8_t* data, uint32_t sizeBytes) {
    GxDisplayListState& dl = g_dlRecordState;
    if (!dl.active || dl.base == 0 || dl.size == 0 || dl.writePtr == 0) {
        // Same guard WriteDisplayListData applies; let the per-write path
        // reproduce its (silent) drop.
        return false;
    }

    const uint32_t end = dl.base + dl.size;
    if (dl.writePtr > end || (end - dl.writePtr) < sizeBytes) {
        return false;
    }

    // Guest-visible bytes are written through the ordinary store path, so
    // unaligned cursors, MMIO policy and executable-write guards all behave as
    // they do for a single store. The cursor and count advance per element so a
    // failed write in the middle leaves exactly the completed prefix recorded.
    try {
        uint32_t offset = 0;
        for (; offset + 4u <= sizeBytes; offset += 4u) {
            Memory::Write32(dl.writePtr, ReadBE32(data + offset));
            dl.writePtr += 4u;
            dl.count += 4u;
        }
        if (offset + 2u <= sizeBytes) {
            Memory::Write16(dl.writePtr, ReadBE16(data + offset));
            dl.writePtr += 2u;
            dl.count += 2u;
            offset += 2u;
        }
        if (offset < sizeBytes) {
            Memory::Write8(dl.writePtr, data[offset]);
            dl.writePtr += 1u;
            dl.count += 1u;
        }
    } catch (const Memory::AccessViolation&) {
    }
    return true;
}

extern "C" void GX_HLE_FIFO_WriteBurst(const uint8_t* data, uint32_t sizeBytes) {
    if (data == nullptr || sizeBytes == 0) {
        return;
    }

    if (IsDisplayListActive() && WriteDisplayListBurst(data, sizeBytes)) {
        return;
    }

    const uint32_t applied = ApplyFifoPacketsDirect(data, sizeBytes);
    if (applied < sizeBytes) {
        HleFifoWriteBurstChunked(data + applied, sizeBytes - applied);
    }
}
