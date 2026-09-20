#include "gx_internal.h"
#include "gx_stream_common.h"
#include "gx_cp_decode.h"
#include "isa/big_endian.h"

#include <cstdlib>
#include <unordered_map>

// The display-list scan cache validates a cached scan against the live guest
// bytes with a 64-bit XXH3 digest (see GxDisplayListScanCache::CanReuse).
#include <xxhash.h>

namespace aurora::gx::fifo {
bool in_display_list();
bool submit_raw_draw(GXPrimitive prim, GXVtxFmt fmt, const uint8_t* vertices, uint16_t vtxCount,
                     uint32_t vertexBytes);
void set_max_draws_per_batch(uint32_t limit);
}
namespace aurora::gfx {
void set_bind_group_cache_policy(uint32_t retainFrames, uint32_t sweepPeriod) noexcept;
}
namespace aurora::gx {
}

namespace {

// Opcode constants and the stream helpers this file shares with gx_fifo.cpp /
// gx_vertex.cpp; see gx_stream_common.h.
using namespace GxCmd;
using namespace GxStream;
using GxCpDecode::ApplyCpRegWrite;
using GxCpDecode::SameVtxAttrFmt;

// Small display lists dominate the in-race call count. Cache them as well, but
// cap both individual entries and aggregate copied command bytes so malformed
// guest input cannot turn this optimization into unbounded host allocation.
constexpr uint32_t kDlScanCacheMaxEntryBytes = 64u * 1024u;
constexpr size_t kDlScanCacheMaxEntries = 8192;
constexpr size_t kDlScanCacheMaxStoredBytes = 8u * 1024u * 1024u;

// Bring-up traces must not tax the normal display-list hot path.  In particular,
// even the diagnostic counter used to perform an atomic RMW for every CALL_DL long
// after its first 32 records had been printed.
static bool MeteorRuntimeTraceEnabled() noexcept {
    static const bool enabled = std::getenv("METEOR_TRACE_RUNTIME") != nullptr;
    return enabled;
}

// Bounded bring-up diagnostic only. This does not alter display-list parsing;
// it records enough context to prove whether a nonsensical nested CALL_DL came
// from real command bytes or from a walker stride/layout desynchronization.
thread_local uint32_t s_dlDiagRootAddr = 0;
thread_local uint32_t s_dlDiagRootBytes = 0;
static std::atomic<uint32_t> s_dlInvalidNestedDiagCount{0};

struct DlDiagRootScope {
    uint32_t oldAddr;
    uint32_t oldBytes;
    DlDiagRootScope(uint32_t addr, uint32_t bytes)
        : oldAddr(s_dlDiagRootAddr), oldBytes(s_dlDiagRootBytes) {
        s_dlDiagRootAddr = addr;
        s_dlDiagRootBytes = bytes;
    }
    ~DlDiagRootScope() {
        s_dlDiagRootAddr = oldAddr;
        s_dlDiagRootBytes = oldBytes;
    }
};

// Display-list write tracking (audit F6a): re-digesting every list every call is the
// costliest step of GX__CallDisplayList, and wasted on BRRES shape lists that are written
// once at load and never touched again.
constexpr uint64_t kDlWriteGenerationUntracked = GxGuestWrite::kUntracked;

// Byte footprint of one vertex of the currently configured layout. Indexed attributes are
// sized before the matrix-attribute check here (unlike BuildScanVertexLayout), but a
// GX_INDEX16 matrix attribute is impossible on real hardware so both are unreachable.
static uint32_t CalcDLVertexSize(GXVtxFmt vtxfmt) {
    uint32_t size = 0;
    for (int attr = 0; attr < 26; ++attr) {
        const auto type = g_hleGxState.vtxDesc[attr];
        if (type == GX_NONE) continue;
        const auto& fmt = g_hleGxState.vtxAttrFmt[vtxfmt][attr];
        if (type == GX_INDEX8) size += (attr == GX_VA_NRM) ? NormalIndexCount(fmt) : 1u;
        else if (type == GX_INDEX16) size += ((attr == GX_VA_NRM) ? NormalIndexCount(fmt) : 1u) * 2u;
        else if (type == GX_DIRECT) {
            size += DirectAttrByteSize(static_cast<GXAttr>(attr), fmt, /*matrixAttrIsOneByte=*/true,
                                       /*fallbackComps=*/0u);
        }
    }
    return size;
}

static void ApplyAuroraVtxDesc();
static void ApplyAuroraVtxAttrFmtForDisplayList(GXVtxFmt fmt, bool mixed);
static void ApplyAuroraArrayIfChanged(GXAttr attr, const void* data, uint32_t size, uint8_t stride);
static void SyncAppliedVtxStateFromHleReal();
static void SubmitLytDrawPacket(const uint8_t* packet, uint32_t packetBytes);

static inline uint8_t ScaleLytAlpha(uint8_t alpha, uint32_t modulate) {
    const int32_t product = static_cast<int32_t>(alpha) * static_cast<int32_t>(modulate & 0xFFu);
    const int32_t magic = static_cast<int32_t>(0x80808081u);
    int32_t quotient = static_cast<int32_t>((static_cast<int64_t>(magic) * product) >> 32);
    quotient = static_cast<int32_t>(quotient + product) >> 7;
    quotient += (static_cast<uint32_t>(quotient) >> 31);
    return static_cast<uint8_t>(quotient);
}

static inline uint32_t ReadLytColor(uint32_t colorAddr, int index) {
    return Memory::Read32(colorAddr + static_cast<uint32_t>(index * 4));
}

static inline uint32_t ReadModulatedLytColor(uint32_t colorAddr, int index, uint32_t alpha) {
    const uint32_t addr = colorAddr + static_cast<uint32_t>(index * 4);
    const uint8_t r = Memory::Read8(addr);
    const uint8_t g = Memory::Read8(addr + 1);
    const uint8_t b = Memory::Read8(addr + 2);
    uint8_t a = Memory::Read8(addr + 3);
    if ((alpha & 0xFFu) != 0xFFu) {
        a = ScaleLytAlpha(a, alpha);
    }
    return (static_cast<uint32_t>(r) << 24) |
           (static_cast<uint32_t>(g) << 16) |
           (static_cast<uint32_t>(b) << 8) |
           static_cast<uint32_t>(a);
}

static inline void AppendLytTexCoords(uint8_t* packet, uint32_t& pos, uint32_t texCoordAddr,
                                      int texCoordCount, uint32_t cornerOffset) {
    if (texCoordCount <= 0) {
        return;
    }
    for (int i = 0; i < texCoordCount; ++i) {
        const uint32_t coordAddr = texCoordAddr + static_cast<uint32_t>(i * 32) + cornerOffset;
        BigEndian::AppendFloat32(packet, pos, Memory::ReadFloat32(coordAddr));
        BigEndian::AppendFloat32(packet, pos, Memory::ReadFloat32(coordAddr + 4));
    }
}

static inline void AppendLytQuadVertex(uint8_t* packet, uint32_t& pos, float x, float y,
                                       uint32_t texCoordAddr, int texCoordCount,
                                       uint32_t cornerOffset, const uint32_t* colors, int colorIndex) {
    BigEndian::AppendFloat32(packet, pos, x);
    BigEndian::AppendFloat32(packet, pos, y);
    if (colors) {
        BigEndian::Append32(packet, pos, colors[colorIndex]);
    }
    AppendLytTexCoords(packet, pos, texCoordAddr, texCoordCount, cornerOffset);
}

// The four corners of an nw4r::lyt quad, in the order GX_QUADS expects, with
// each corner's texcoord byte offset and colour index. The winding order is
// load-bearing and both emit paths below share this one copy of it.
static inline void AppendLytQuadVertices(uint8_t* packet, uint32_t& pos, float x0, float y0,
                                         float x1, float y1, uint32_t texCoordAddr,
                                         int texCoordCount, const uint32_t* colors) {
    AppendLytQuadVertex(packet, pos, x0, y0, texCoordAddr, texCoordCount, 0, colors, 0);
    AppendLytQuadVertex(packet, pos, x1, y0, texCoordAddr, texCoordCount, 8, colors, 1);
    AppendLytQuadVertex(packet, pos, x1, y1, texCoordAddr, texCoordCount, 24, colors, 3);
    AppendLytQuadVertex(packet, pos, x0, y1, texCoordAddr, texCoordCount, 16, colors, 2);
}

static bool CanSubmitLytDrawDirect(int texCoordCount, const uint32_t* colors) {
    if (texCoordCount < 0 || texCoordCount > 8) {
        return false;
    }
    const auto& posFmt = g_hleGxState.vtxAttrFmt[GX_VTXFMT0][GX_VA_POS];
    if (g_hleGxState.vtxDesc[GX_VA_POS] != GX_DIRECT ||
        posFmt.cnt != GX_POS_XY ||
        posFmt.type != GX_F32) {
        return false;
    }

    const bool hasColors = colors != nullptr;
    const auto& clrFmt = g_hleGxState.vtxAttrFmt[GX_VTXFMT0][GX_VA_CLR0];
    if (hasColors) {
        if (g_hleGxState.vtxDesc[GX_VA_CLR0] != GX_DIRECT ||
            clrFmt.cnt != GX_CLR_RGBA ||
            clrFmt.type != GX_RGBA8) {
            return false;
        }
    } else if (g_hleGxState.vtxDesc[GX_VA_CLR0] != GX_NONE) {
        return false;
    }

    if (g_hleGxState.vtxDesc[GX_VA_NRM] != GX_NONE ||
        g_hleGxState.vtxDesc[GX_VA_CLR1] != GX_NONE) {
        return false;
    }

    for (int attr = GX_VA_PNMTXIDX; attr <= GX_VA_TEX7MTXIDX; ++attr) {
        if (g_hleGxState.vtxDesc[attr] != GX_NONE) {
            return false;
        }
    }

    for (int i = 0; i < 8; ++i) {
        const GXAttr attr = static_cast<GXAttr>(GX_VA_TEX0 + i);
        const auto& fmt = g_hleGxState.vtxAttrFmt[GX_VTXFMT0][attr];
        if (i < texCoordCount) {
            if (g_hleGxState.vtxDesc[attr] != GX_DIRECT ||
                fmt.cnt != GX_TEX_ST ||
                fmt.type != GX_F32) {
                return false;
            }
        } else if (g_hleGxState.vtxDesc[attr] != GX_NONE) {
            return false;
        }
    }

    return true;
}

static bool SubmitLytDrawDirect(float x0, float y0, float x1, float y1, int texCoordCount,
                                uint32_t texCoordAddr, const uint32_t* colors) {
    if (!CanSubmitLytDrawDirect(texCoordCount, colors)) {
        return false;
    }


    EnsureAuroraFrameActive();

    ApplyAuroraVtxDesc();

    ApplyAuroraVtxAttrFmtForDisplayList(GX_VTXFMT0, false);

    EnsureDefaultGxAlphaCompare();


    std::array<uint8_t, 4u * (8u + 4u + 8u * 8u)> vertices{};
    uint32_t pos = 0;
    AppendLytQuadVertices(vertices.data(), pos, x0, y0, x1, y1, texCoordAddr, texCoordCount, colors);

    if (!aurora::gx::fifo::submit_raw_draw(GX_QUADS, GX_VTXFMT0, vertices.data(), 4, pos)) {
        return false;
    }
    GXMarkFrameWork();

    SyncAppliedVtxStateFromHleReal();
    return true;
}

static inline void EmitLytDrawQuad(uint32_t posAddr, uint32_t sizeAddr, int texCoordCount,
                                   uint32_t texCoordAddr, const uint32_t* colors) {
    const float x0 = Memory::ReadFloat32(posAddr);
    const float y0 = Memory::ReadFloat32(posAddr + 4);
    // The casts pin the arithmetic to PPC single precision regardless of the
    // host's FLT_EVAL_METHOD.
    const float x1 = static_cast<float>(x0 + Memory::ReadFloat32(sizeAddr));
    const float y1 = static_cast<float>(y0 - Memory::ReadFloat32(sizeAddr + 4));

    if (SubmitLytDrawDirect(x0, y0, x1, y1, texCoordCount, texCoordAddr, colors)) {
        return;
    }

    // GX has exactly 8 texture coordinates, so nw4r::lyt cannot ask for more.
    // The fixed packet buffer below is sized for that maximum; bail rather than
    // overrun it if the guest ever hands us something else.
    if (texCoordCount > 8) {
        return;
    }

    std::array<uint8_t, 3u + 4u * (8u + 4u + 8u * 8u)> packet{};
    uint32_t pos = 0;
    packet[pos++] = GX_DRAW_QUADS_CMD | GX_VTXFMT0;
    BigEndian::Append16(packet.data(), pos, 4);
    AppendLytQuadVertices(packet.data(), pos, x0, y0, x1, y1, texCoordAddr, texCoordCount, colors);
    SubmitLytDrawPacket(packet.data(), pos);
}

static uint32_t SubmitDLVertex(const uint8_t* ptr, GXVtxFmt vtxfmt, const GXAttrType* sourceVtxDesc) {
    uint32_t offset = 0;
    for (int attr = 0; attr < 26; ++attr) {
        const auto type = sourceVtxDesc[attr]; if (type == GX_NONE) continue;
        const auto& fmt = g_hleGxState.vtxAttrFmt[vtxfmt][attr];
        const GXAttr gxAttr = static_cast<GXAttr>(attr);
        if (type == GX_INDEX8) {
            const uint32_t indexCount = (gxAttr == GX_VA_NRM) ? NormalIndexCount(fmt) : 1u;
            if (gxAttr == GX_VA_NRM && indexCount == 3u) {
                const uint32_t indices[3] = {ptr[offset], ptr[offset + 1], ptr[offset + 2]};
                SubmitIndexedNormalNBT3(indices, fmt);
            } else {
                SubmitIndexedAttribute(gxAttr, ptr[offset]);
            }
            offset += indexCount;
        }
        else if (type == GX_INDEX16) {
            const uint32_t indexCount = (gxAttr == GX_VA_NRM) ? NormalIndexCount(fmt) : 1u;
            if (gxAttr == GX_VA_NRM && indexCount == 3u) {
                const uint32_t indices[3] = {
                    ReadBE16(ptr + offset),
                    ReadBE16(ptr + offset + 2),
                    ReadBE16(ptr + offset + 4),
                };
                SubmitIndexedNormalNBT3(indices, fmt);
            } else {
                SubmitIndexedAttribute(gxAttr, ReadBE16(ptr + offset));
            }
            offset += indexCount * 2u;
        }
        else if (type == GX_DIRECT) {
            float comps[9]{};
            switch (attr) {
            case GX_VA_PNMTXIDX: {
                const u8 idx = ptr[offset];
                offset += 1;
                GXMatrixIndex1u8(gxAttr, idx);
                break;
            }
            case GX_VA_TEX0MTXIDX: case GX_VA_TEX1MTXIDX: case GX_VA_TEX2MTXIDX:
            case GX_VA_TEX3MTXIDX: case GX_VA_TEX4MTXIDX: case GX_VA_TEX5MTXIDX:
            case GX_VA_TEX6MTXIDX: case GX_VA_TEX7MTXIDX:
                GXMatrixIndex1u8(gxAttr, ptr[offset]);
                offset += 1;
                break;
            case GX_VA_POS: {
                int count = static_cast<int>(AttrCompCount(GX_VA_POS, fmt, 0u)), compSize = GetCompSizeBytes(fmt.type);
                u32 rawComps[9]{};
                for (int i = 0; i < count; ++i) {
                    // Keep the CP's original component bits as well as the
                    // decoded float.  GXPosition* emits according to the
                    // active VAT; passing only the float would quantize an
                    // integer direct attribute a second time (and could turn
                    // a signed fixed-point value into a different stream).
                    rawComps[i] = ReadBECompRaw(ptr + offset, fmt.type);
                    comps[i] = ConvertCompToFloat(rawComps[i], fmt.type, fmt.frac);
                    offset += compSize;
                }
                SubmitAttribute(gxAttr, comps, fmt, rawComps);
                break;
            }
            case GX_VA_NRM: {
                int count = static_cast<int>(AttrCompCount(GX_VA_NRM, fmt, 0u)), compSize = GetCompSizeBytes(fmt.type);
                u32 rawComps[9]{};
                for (int i = 0; i < count; ++i) {
                    rawComps[i] = ReadBECompRaw(ptr + offset, fmt.type);
                    comps[i] = ConvertCompToFloat(rawComps[i], fmt.type, fmt.frac);
                    offset += compSize;
                }
                SubmitAttribute(gxAttr, comps, fmt, rawComps);
                break;
            }
            case GX_VA_CLR0: case GX_VA_CLR1: {
                GXColor color{};
                DecodeColorBytes(ptr + offset, fmt.type, fmt.cnt, color);
                offset += ColorByteSize(fmt.type, fmt.cnt);
                if (fmt.cnt == GX_CLR_RGB) GXColor3u8(color.r, color.g, color.b); else GXColor4u8(color.r, color.g, color.b, color.a);
                break;
            }
            case GX_VA_TEX0: case GX_VA_TEX1: case GX_VA_TEX2: case GX_VA_TEX3: case GX_VA_TEX4: case GX_VA_TEX5: case GX_VA_TEX6: case GX_VA_TEX7: {
                // All eight GX_VA_TEXn share one component layout.
                int count = static_cast<int>(AttrCompCount(GX_VA_TEX0, fmt, 0u)), compSize = GetCompSizeBytes(fmt.type);
                u32 rawComps[9]{};
                for (int i = 0; i < count; ++i) {
                    rawComps[i] = ReadBECompRaw(ptr + offset, fmt.type);
                    comps[i] = ConvertCompToFloat(rawComps[i], fmt.type, fmt.frac);
                    offset += compSize;
                }
                SubmitAttribute(gxAttr, comps, fmt, rawComps);
                break;
            }
            default: break;
            }
        }
    }
    return offset;
}

static void ApplyAuroraVtxStateForDlBegin(GXVtxFmt fmt) {
    // Only this publish site latches the format into the HLE state; the FIFO and
    // immediate-mode sites have already done it by the time they publish.
    g_hleGxState.currentVtxFmt = fmt;

    PublishAuroraVtxState(fmt, AuroraVtxPublishOptions{/*includeNbt=*/false,
                                                       /*fmtLoopFirst=*/0,
                                                       /*fmtLoopLast=*/25});
}


struct HleGxVertexStateSnapshot {
    GXAttrType vtxDesc[26];
    VtxAttrFmt vtxAttrFmt[8][26];
    HleGxState::VtxArray vtxArray[26];
    GXVtxFmt currentVtxFmt;
    uint64_t vtxLayoutHash;
    bool vtxLayoutHashDirty;
    // Bit per vtxAttrFmt row carried in this snapshot.
    uint32_t vtxAttrFmtRows;
    bool hasVtxDesc;
    bool hasVtxArray;
    bool hasCurrentVtxFmt;
};

constexpr uint32_t kAllVtxAttrFmtRows = 0xFFu;

static HleGxVertexStateSnapshot CaptureGxVertexState() {
    HleGxVertexStateSnapshot snapshot;
    std::memcpy(snapshot.vtxDesc, g_hleGxState.vtxDesc, sizeof(snapshot.vtxDesc));
    std::memcpy(snapshot.vtxAttrFmt, g_hleGxState.vtxAttrFmt, sizeof(snapshot.vtxAttrFmt));
    std::memcpy(snapshot.vtxArray, g_hleGxState.vtxArray, sizeof(snapshot.vtxArray));
    snapshot.currentVtxFmt = g_hleGxState.currentVtxFmt;
    snapshot.vtxLayoutHash = g_hleGxState.vtxLayoutHash;
    snapshot.vtxLayoutHashDirty = g_hleGxState.vtxLayoutHashDirty;
    snapshot.vtxAttrFmtRows = kAllVtxAttrFmtRows;
    snapshot.hasVtxDesc = true;
    snapshot.hasVtxArray = true;
    snapshot.hasCurrentVtxFmt = true;
    return snapshot;
}

static void RestoreGxVertexState(const HleGxVertexStateSnapshot& snapshot) {
    if (snapshot.hasVtxDesc) {
        std::memcpy(g_hleGxState.vtxDesc, snapshot.vtxDesc, sizeof(snapshot.vtxDesc));
    }
    if (snapshot.vtxAttrFmtRows == kAllVtxAttrFmtRows) {
        std::memcpy(g_hleGxState.vtxAttrFmt, snapshot.vtxAttrFmt, sizeof(snapshot.vtxAttrFmt));
    } else {
        for (uint32_t fmt = 0; fmt < 8; ++fmt) {
            if ((snapshot.vtxAttrFmtRows & (1u << fmt)) == 0) {
                continue;
            }
            std::memcpy(g_hleGxState.vtxAttrFmt[fmt], snapshot.vtxAttrFmt[fmt],
                        sizeof(snapshot.vtxAttrFmt[fmt]));
        }
    }
    if (snapshot.hasVtxArray) {
        std::memcpy(g_hleGxState.vtxArray, snapshot.vtxArray, sizeof(snapshot.vtxArray));
    }
    if (snapshot.hasCurrentVtxFmt) {
        g_hleGxState.currentVtxFmt = snapshot.currentVtxFmt;
    }
    g_hleGxState.vtxLayoutHash = snapshot.vtxLayoutHash;
    g_hleGxState.vtxLayoutHashDirty = snapshot.vtxLayoutHashDirty;
    // The wholesale desc/attr-fmt overwrite above bypasses the per-field change
    // checks that normally bump this, so mirrors must be told to resync.
    ++g_hleGxState.vtxStateGeneration;
}

struct ScanAttrStep {
    GXAttr attr = GX_VA_NULL;
    uint32_t offset = 0;
    uint32_t size = 0;
    bool indexed = false;
    bool index16 = false;
};

struct ScanLayoutCacheEntry {
    // Only the indexed steps are consumed (ScanDLVerticesMaxIndices); the scan
    // has no use for the direct ones beyond their contribution to vertexSize.
    std::array<ScanAttrStep, 26> indexedSteps{};
    uint32_t indexedStepCount = 0;
    uint32_t vertexSize = 0;
    uint32_t version = 0;
    uint32_t generation = 0;
    bool valid = false;
};


struct DlCpWrite {
    uint8_t reg = 0;
    uint32_t value = 0;
};


static HleGxVertexStateSnapshot CaptureGxVertexStateForCpWrites(const std::vector<DlCpWrite>& writes) {
    HleGxVertexStateSnapshot snapshot;
    snapshot.vtxAttrFmtRows = 0;
    snapshot.hasVtxDesc = false;
    snapshot.hasVtxArray = false;
    snapshot.hasCurrentVtxFmt = false;
    for (const auto& write : writes) {
        const uint8_t reg = write.reg;
        if (reg == 0x50 || reg == 0x60) {
            snapshot.hasVtxDesc = true;
        } else if (reg >= 0x70 && reg <= 0x77) {
            snapshot.vtxAttrFmtRows |= 1u << (reg - 0x70);
        } else if (reg >= 0x80 && reg <= 0x87) {
            snapshot.vtxAttrFmtRows |= 1u << (reg - 0x80);
        } else if (reg >= 0x90 && reg <= 0x97) {
            snapshot.vtxAttrFmtRows |= 1u << (reg - 0x90);
        } else if (reg >= 0xA0 && reg <= 0xBF) {
            snapshot.hasVtxArray = true;
        }
    }
    if (snapshot.hasVtxDesc) {
        std::memcpy(snapshot.vtxDesc, g_hleGxState.vtxDesc, sizeof(snapshot.vtxDesc));
    }
    for (uint32_t fmt = 0; fmt < 8; ++fmt) {
        if ((snapshot.vtxAttrFmtRows & (1u << fmt)) == 0) {
            continue;
        }
        std::memcpy(snapshot.vtxAttrFmt[fmt], g_hleGxState.vtxAttrFmt[fmt],
                    sizeof(snapshot.vtxAttrFmt[fmt]));
    }
    if (snapshot.hasVtxArray) {
        std::memcpy(snapshot.vtxArray, g_hleGxState.vtxArray, sizeof(snapshot.vtxArray));
    }
    snapshot.vtxLayoutHash = g_hleGxState.vtxLayoutHash;
    snapshot.vtxLayoutHashDirty = g_hleGxState.vtxLayoutHashDirty;
    return snapshot;
}

struct DlScanCacheEntry {
    uint32_t listAddr = 0;
    uint32_t nbytes = 0;
    uint64_t layoutHash = 0;
    std::array<uint32_t, GX_VA_MAX_ATTR> maxIdx{};
    std::array<bool, GX_VA_MAX_ATTR> sawIdx{};
    std::array<uint32_t, GX_VA_MAX_ATTR> maxXfIdx{};
    std::array<uint32_t, GX_VA_MAX_ATTR> maxXfBytes{};
    std::array<bool, GX_VA_MAX_ATTR> sawXfIdx{};
    GXVtxFmt dlVtxFmt = GX_MAX_VTXFMT;
    bool dlVtxFmtMixed = false;
    bool dlHasNestedDl = false;
    bool dlHasArrayStateWrites = false;
    bool dlHasCpWrites = false;
    bool scanOk = false;
    // Flatten results for lists that need one. `flattenVerbatim` means the
    // flattened stream is byte-identical to the source list, so aurora can be
    // handed the guest bytes directly and no copy has to be stored.
    bool needsFlatten = false;
    bool flattenOk = false;
    bool flattenVerbatim = false;
};

struct DlScanCacheRecord {
    DlScanCacheEntry result{};
    // Answer of DisplayListMayContainDraw for these bytes
    bool mayContainDraw = false;
    uint64_t contentDigest = 0;
    uint64_t writeGeneration = kDlWriteGenerationUntracked;
    std::vector<DlCpWrite> cpWrites{};
    std::vector<uint8_t> flattened{};
};

struct DlScanCacheState {
    std::unordered_map<uint64_t, DlScanCacheRecord> entries{};
    size_t storedCommandBytes = 0;
};

static uint64_t HashScanLayoutState() {
    if (!g_hleGxState.vtxLayoutHashDirty) {
        return g_hleGxState.vtxLayoutHash;
    }
    uint64_t hash = 1469598103934665603ull;
    auto mix = [&hash](uint32_t value) {
        hash ^= static_cast<uint64_t>(value);
        hash *= 1099511628211ull;
    };
    for (int attr = 0; attr < 26; ++attr) {
        mix(static_cast<uint32_t>(g_hleGxState.vtxDesc[attr]));
    }
    for (int fmt = 0; fmt < 8; ++fmt) {
        for (int attr = 0; attr < 26; ++attr) {
            const auto& f = g_hleGxState.vtxAttrFmt[fmt][attr];
            mix(static_cast<uint32_t>(f.cnt));
            mix(static_cast<uint32_t>(f.type));
            mix(static_cast<uint32_t>(f.frac));
        }
    }
    g_hleGxState.vtxLayoutHash = hash;
    g_hleGxState.vtxLayoutHashDirty = false;
    return g_hleGxState.vtxLayoutHash;
}

static uint64_t DlScanCacheKey(uint32_t listAddr, uint32_t nbytes, uint64_t layoutHash) {
    return GxDisplayListScanCache::MakeIdentityKey(listAddr, nbytes, layoutHash);
}

static DlScanCacheState& DlScanCache() {
    static thread_local DlScanCacheState s_cache;
    return s_cache;
}

static uint64_t DlContentDigest(const uint8_t* list, uint32_t nbytes) {
    return static_cast<uint64_t>(XXH3_64bits(list, static_cast<size_t>(nbytes)));
}


struct DlScanCacheProbe {
    const DlScanCacheRecord* record = nullptr;
    uint64_t contentDigest = 0;
    bool digestValid = false;
    uint64_t writeGeneration = kDlWriteGenerationUntracked;
};

static DlScanCacheProbe ProbeDlScanCache(const uint8_t* list, uint32_t listAddr, uint32_t nbytes,
                                         uint64_t layoutHash) {
    DlScanCacheProbe probe{};
    probe.writeGeneration = GxGuestWrite::GenerationForRange(listAddr, nbytes);

    auto& s_cache = DlScanCache();
    const uint64_t key = DlScanCacheKey(listAddr, nbytes, layoutHash);
    const auto it = s_cache.entries.find(key);
    if (it != s_cache.entries.end()) {
        auto& record = it->second;
        const auto& entry = record.result;
        const bool identityMatches = entry.listAddr == CanonicalizeGxMainRamAddress(listAddr) &&
                                     entry.nbytes == nbytes && entry.layoutHash == layoutHash;
        if (identityMatches && probe.writeGeneration != kDlWriteGenerationUntracked &&
            record.writeGeneration == probe.writeGeneration) {
            // Nothing has flushed a write over these bytes since the digest was
            // taken, so the stored digest still describes them. Skip XXH3.
            probe.record = &record;
            probe.contentDigest = record.contentDigest;
            probe.digestValid = true;
        } else {
            probe.contentDigest = DlContentDigest(list, nbytes);
            probe.digestValid = true;
            if (identityMatches &&
                GxDisplayListScanCache::CanReuse(entry.listAddr, entry.nbytes, entry.layoutHash,
                                                 record.contentDigest, listAddr, nbytes, layoutHash,
                                                 probe.contentDigest)) {
                // False alarm: the bump did not actually change this list
                record.writeGeneration = probe.writeGeneration;
                probe.record = &record;
            }
        }
    }
    return probe;
}

static size_t DlScanCacheRecordBytes(const DlScanCacheRecord& record) {
    return record.flattened.size() + record.cpWrites.size() * sizeof(DlCpWrite);
}

static void StoreDlScanCache(uint32_t listAddr, uint32_t nbytes, uint64_t layoutHash,
                             uint64_t contentDigest, uint64_t writeGeneration,
                             bool mayContainDraw, const DlScanCacheEntry& entry,
                             std::vector<DlCpWrite>&& cpWrites, const uint8_t* flattened,
                             uint32_t flattenedBytes) {
    auto& s_cache = DlScanCache();
    const uint64_t key = DlScanCacheKey(listAddr, nbytes, layoutHash);
    auto old = s_cache.entries.find(key);
    bool replacing = old != s_cache.entries.end();
    size_t replacedBytes = replacing ? DlScanCacheRecordBytes(old->second) : 0;
    const size_t incomingBytes = static_cast<size_t>(flattenedBytes) +
                                 cpWrites.size() * sizeof(DlCpWrite);
    if ((!replacing && s_cache.entries.size() >= kDlScanCacheMaxEntries) ||
        s_cache.storedCommandBytes - replacedBytes + incomingBytes > kDlScanCacheMaxStoredBytes) {
        s_cache.entries.clear();
        s_cache.storedCommandBytes = 0;
        replacing = false;
        replacedBytes = 0;
    }
    DlScanCacheRecord record{};
    record.result = entry;
    record.mayContainDraw = mayContainDraw;
    record.result.listAddr = CanonicalizeGxMainRamAddress(listAddr);
    record.result.nbytes = nbytes;
    record.result.layoutHash = layoutHash;
    record.contentDigest = contentDigest;
    // Sampled before the digest was taken, so a write racing the digest can only
    // make the next call re-digest, never make it trust a stale entry.
    record.writeGeneration = writeGeneration;
    record.cpWrites = std::move(cpWrites);
    if (flattened != nullptr && flattenedBytes != 0) {
        record.flattened.assign(flattened, flattened + flattenedBytes);
    }
    auto [it, inserted] = s_cache.entries.insert_or_assign(key, std::move(record));
    (void)inserted;
    s_cache.storedCommandBytes += DlScanCacheRecordBytes(it->second);
    if (replacing) {
        s_cache.storedCommandBytes -= replacedBytes;
    }
}

static void BuildScanVertexLayout(GXVtxFmt vtxfmt, std::array<ScanAttrStep, 26>& indexedSteps,
                                  uint32_t& indexedStepCount, uint32_t& vertexSize) {
    indexedStepCount = 0;
    vertexSize = 0;
    for (int attr = 0; attr < 26; ++attr) {
        const auto type = g_hleGxState.vtxDesc[attr];
        if (type == GX_NONE) {
            continue;
        }
        const GXAttr gxAttr = static_cast<GXAttr>(attr);
        ScanAttrStep step{};
        step.attr = gxAttr;
        step.offset = vertexSize;
        if (IsMatrixIndexAttr(gxAttr)) {
            step.size = 1;
        } else if (type == GX_INDEX8) {
            step.size = (gxAttr == GX_VA_NRM) ? NormalIndexCount(g_hleGxState.vtxAttrFmt[vtxfmt][attr]) : 1u;
            step.indexed = true;
        } else if (type == GX_INDEX16) {
            step.size = ((gxAttr == GX_VA_NRM) ? NormalIndexCount(g_hleGxState.vtxAttrFmt[vtxfmt][attr]) : 1u) * 2u;
            step.indexed = true;
            step.index16 = true;
        } else if (type == GX_DIRECT) {
            // Matrix attributes are handled above, so this reports 0 for them.
            step.size = DirectAttrByteSize(gxAttr, g_hleGxState.vtxAttrFmt[vtxfmt][attr],
                                           /*matrixAttrIsOneByte=*/false, /*fallbackComps=*/0u);
        }
        if (step.size == 0) {
            continue;
        }
        if (step.indexed) {
            indexedSteps[indexedStepCount++] = step;
        }
        // Every step contributes to the vertex stride, indexed or not: the
        // offsets the indexed steps carry are relative to the whole vertex.
        vertexSize += step.size;
    }
}

static const ScanLayoutCacheEntry& GetScanVertexLayout(GXVtxFmt vtxfmt,
                                                       std::array<ScanLayoutCacheEntry, 8>& cache,
                                                       uint32_t version,
                                                       uint32_t generation) {
    auto& entry = cache[static_cast<uint32_t>(vtxfmt)];
    if (entry.valid && entry.version == version && entry.generation == generation) {
        return entry;
    }
    BuildScanVertexLayout(vtxfmt, entry.indexedSteps, entry.indexedStepCount, entry.vertexSize);
    entry.version = version;
    entry.generation = generation;
    entry.valid = true;
    return entry;
}

static void ScanDLVerticesMaxIndices(const uint8_t* ptr, uint16_t vtxCount,
                                     const std::array<ScanAttrStep, 26>& steps, uint32_t stepCount,
                                     uint32_t vertexSize,
                                     std::array<uint32_t, GX_VA_MAX_ATTR>& maxIdx,
                                     std::array<bool, GX_VA_MAX_ATTR>& sawIdx) {
    if (stepCount == 0) {
        return;
    }
    const uint8_t* vertex = ptr;
    for (uint16_t v = 0; v < vtxCount; ++v, vertex += vertexSize) {
        for (uint32_t i = 0; i < stepCount; ++i) {
            const auto& step = steps[i];
            const int attr = static_cast<int>(step.attr);
            const uint32_t indexSize = step.index16 ? 2u : 1u;
            const uint32_t indexCount = std::max(1u, step.size / indexSize);
            for (uint32_t indexIdx = 0; indexIdx < indexCount; ++indexIdx) {
                const uint8_t* indexPtr = vertex + step.offset + indexIdx * indexSize;
                const uint32_t index = step.index16 ? ReadBE16(indexPtr) : *indexPtr;
                if (!sawIdx[attr] || index > maxIdx[attr]) {
                    maxIdx[attr] = index;
                    sawIdx[attr] = true;
                }
            }
        }
    }
}


static void AppendBytes(std::vector<uint8_t>& out, const uint8_t* data, uint32_t count) {
    out.insert(out.end(), data, data + count);
}

template <class Visitor>
static bool WalkDisplayList(const uint8_t* data, uint32_t nbytes, Visitor& visitor, int depth) {
    if (depth > Visitor::kMaxDepth) {
        return false;
    }
    uint32_t pos = 0;
    uint32_t lastDrawOff = 0xFFFFFFFFu;
    uint32_t lastDrawPayloadOff = 0xFFFFFFFFu;
    uint32_t lastDrawPayloadBytes = 0;
    uint32_t lastDrawVertexSize = 0;
    uint16_t lastDrawVertexCount = 0;
    GXVtxFmt lastDrawVtxFmt = GX_MAX_VTXFMT;
    while (pos < nbytes) {
        const uint8_t* const cmdPtr = data + pos;
        const uint8_t cmd = data[pos++];
        if (cmd == GX_NOP_CMD) {
            if (!visitor.OnNop(cmd)) return false;
            continue;
        }
        if (cmd == GX_LOAD_BP_REG_CMD) {
            if (pos + 4u > nbytes) return false;
            if (!visitor.OnBpReg(cmdPtr, ReadBE32(data + pos))) return false;
            pos += 4u;
            continue;
        }
        const uint8_t opcode = cmd & GX_OPCODE_MASK_CMD;
        if (opcode == GX_LOAD_CP_REG_CMD) {
            if (pos + 5u > nbytes) return false;
            if (!visitor.OnCpReg(cmdPtr, data[pos], ReadBE32(data + pos + 1))) return false;
            pos += 5u;
            continue;
        }
        if (opcode == GX_LOAD_XF_REG_CMD) {
            // Overflow-safe spelling of "pos + 4 <= nbytes && pos + payload <= nbytes".
            if (pos > nbytes || nbytes - pos < 4u) return false;
            const uint16_t count = ReadBE16(data + pos);
            const uint32_t payloadBytes = 4u + (static_cast<uint32_t>(count) + 1u) * 4u;
            if (payloadBytes > nbytes - pos) return false;
            if (!visitor.OnXfReg(cmdPtr, payloadBytes)) return false;
            pos += payloadBytes;
            continue;
        }
        if (opcode >= GX_LOAD_INDX_A_CMD && opcode <= GX_LOAD_INDX_D_CMD) {
            if (pos + 4u > nbytes) return false;
            if (!visitor.OnIndexedXf(cmdPtr, cmd, ReadBE32(data + pos))) return false;
            pos += 4u;
            continue;
        }
        if (opcode == GX_CMD_CALL_DL_CMD) {
            if (pos + 8u > nbytes) return false;
            const uint32_t addr = ReadBE32(data + pos);
            const uint32_t size = ReadBE32(data + pos + 4);
            const bool invalidNestedRange = addr != 0 && size != 0 && !Memory::Contains(addr, size);
            if (invalidNestedRange) {
                const uint32_t diagIndex =
                    s_dlInvalidNestedDiagCount.fetch_add(1, std::memory_order_relaxed);
                if (diagIndex < 16u) {
                    RT_LOGF(RT_TAG_GX,
                            "DL_INVALID_NESTED root=0x%08X rootBytes=0x%X depth=%d "
                            "cmdOff=0x%X addr=0x%08X size=0x%08X "
                            "vcdPos=%u vcdNrm=%u vcdClr0=%u vcdTex0=%u "
                            "lastDrawOff=0x%X lastPayloadOff=0x%X lastPayloadBytes=0x%X "
                            "lastVtxFmt=%u lastVtxCount=%u lastVtxSize=%u\n",
                            s_dlDiagRootAddr, s_dlDiagRootBytes, depth, pos - 1u,
                            addr, size,
                            static_cast<unsigned>(g_hleGxState.vtxDesc[GX_VA_POS]),
                            static_cast<unsigned>(g_hleGxState.vtxDesc[GX_VA_NRM]),
                            static_cast<unsigned>(g_hleGxState.vtxDesc[GX_VA_CLR0]),
                            static_cast<unsigned>(g_hleGxState.vtxDesc[GX_VA_TEX0]),
                            lastDrawOff, lastDrawPayloadOff, lastDrawPayloadBytes,
                            static_cast<unsigned>(lastDrawVtxFmt), lastDrawVertexCount,
                            lastDrawVertexSize);
                    if (lastDrawVtxFmt != GX_MAX_VTXFMT) {
                        const auto& posFmt = g_hleGxState.vtxAttrFmt[lastDrawVtxFmt][GX_VA_POS];
                        const auto& nrmFmt = g_hleGxState.vtxAttrFmt[lastDrawVtxFmt][GX_VA_NRM];
                        const auto& tex0Fmt = g_hleGxState.vtxAttrFmt[lastDrawVtxFmt][GX_VA_TEX0];
                        RT_LOGF(RT_TAG_GX,
                                "DL_INVALID_NESTED_VAT fmt=%u "
                                "pos(cnt=%u type=%u frac=%u) "
                                "nrm(cnt=%u type=%u frac=%u) "
                                "tex0(cnt=%u type=%u frac=%u)\n",
                                static_cast<unsigned>(lastDrawVtxFmt),
                                static_cast<unsigned>(posFmt.cnt),
                                static_cast<unsigned>(posFmt.type),
                                static_cast<unsigned>(posFmt.frac),
                                static_cast<unsigned>(nrmFmt.cnt),
                                static_cast<unsigned>(nrmFmt.type),
                                static_cast<unsigned>(nrmFmt.frac),
                                static_cast<unsigned>(tex0Fmt.cnt),
                                static_cast<unsigned>(tex0Fmt.type),
                                static_cast<unsigned>(tex0Fmt.frac));
                    }
                    if (diagIndex == 0u) {
                        for (uint32_t dumpOff = 0; dumpOff < nbytes; dumpOff += 16u) {
                            const uint32_t remain = nbytes - dumpOff;
                            const uint32_t count = std::min(16u, remain);
                            char hex[16 * 3 + 1]{};
                            uint32_t hexPos = 0;
                            static constexpr char kHex[] = "0123456789ABCDEF";
                            for (uint32_t i = 0; i < count; ++i) {
                                const uint8_t b = data[dumpOff + i];
                                hex[hexPos++] = kHex[b >> 4];
                                hex[hexPos++] = kHex[b & 0x0F];
                                if (i + 1u != count) hex[hexPos++] = ' ';
                            }
                            hex[hexPos] = '\0';
                            RT_LOGF(RT_TAG_GX, "DL_INVALID_NESTED_BYTES off=0x%X %s\n",
                                    dumpOff, hex);
                        }
                    }
                }
            }
            pos += 8u;
            // A malformed nested range is diagnostic evidence of a stream
            // layout mismatch, not a guest pointer to recurse into. Skipping
            // it keeps all visitors on the command boundary and prevents a
            // bad vertex payload from becoming a host memory access.
            if (invalidNestedRange) {
                continue;
            }
            if (!visitor.OnCallDisplayList(addr, size, depth)) return false;
            continue;
        }
        if (opcode == GX_CMD_INVL_VC_CMD) {
            if (!visitor.OnInvalidateVertexCache(cmd)) return false;
            continue;
        }
        if constexpr (Visitor::kHandlesDraw) {
            if (IsDrawOpcode(opcode)) {
                const GXVtxFmt vtxfmt = static_cast<GXVtxFmt>(cmd & GX_VAT_MASK_CMD);
                // Resolved before the count bounds check: the index scan latches
                // the list's vertex format here, and for the other two this is a
                // pure function of the current layout.
                const uint32_t vertexSize = visitor.DrawVertexSize(vtxfmt);
                if (pos + 2u > nbytes) return false;
                const uint16_t vtxCount = ReadBE16(data + pos);
                pos += 2u;
                uint32_t payloadBytes = static_cast<uint32_t>(vtxCount) * vertexSize;
                if (pos + payloadBytes > nbytes) return false;
                const uint32_t drawOff = static_cast<uint32_t>(cmdPtr - data);
                const uint32_t payloadOff = pos;
                if (!visitor.OnDraw(cmdPtr, opcode, vtxfmt, vtxCount, data + pos, payloadBytes)) {
                    return false;
                }
                lastDrawOff = drawOff;
                lastDrawPayloadOff = payloadOff;
                lastDrawPayloadBytes = payloadBytes;
                lastDrawVertexSize = vertexSize;
                lastDrawVertexCount = vtxCount;
                lastDrawVtxFmt = vtxfmt;
                pos += payloadBytes;
                continue;
            }
        }
        if (!visitor.OnUnknownCommand(cmd)) return false;
    }
    return true;
}

// Does this list carry anything that can produce geometry? Anything the walker
// cannot model counts as "yes", which is why every hook that would have to look
// at a payload just fails the walk instead.
struct DlMayContainDrawVisitor {
    static constexpr int kMaxDepth = 8;
    static constexpr bool kHandlesDraw = false;

    bool OnNop(uint8_t) { return true; }
    bool OnBpReg(const uint8_t*, uint32_t) { return true; }
    // An array base/stride write means the list is setting up indexed geometry.
    bool OnCpReg(const uint8_t*, uint8_t reg, uint32_t) { return !(reg >= 0xA0 && reg <= 0xBF); }
    bool OnXfReg(const uint8_t*, uint32_t) { return true; }
    bool OnIndexedXf(const uint8_t*, uint8_t, uint32_t) { return true; }
    bool OnInvalidateVertexCache(uint8_t) { return true; }
    // Draw opcodes reach this too (kHandlesDraw is false): a draw opcode, or a
    // command byte this walker does not model, both mean "assume it draws".
    bool OnUnknownCommand(uint8_t) { return false; }
    bool OnCallDisplayList(uint32_t addr, uint32_t size, int depth) {
        if (addr == 0 || size == 0) return true;
        const uint8_t* nested = static_cast<const uint8_t*>(GuestToHostPtr(addr, size));
        if (!nested) return false;
        DlMayContainDrawVisitor child;
        return WalkDisplayList(nested, size, child, depth + 1);
    }
};

static bool DisplayListMayContainDraw(const uint8_t* data, uint32_t nbytes) {
    DlMayContainDrawVisitor visitor;
    return !WalkDisplayList(data, nbytes, visitor, 0);
}

// STF's stage renderer commonly submits a four-vertex indexed display list whose
// indices point far apart in large shared arrays. Binding base..maxIndex makes
// Aurora snapshot tens or hundreds of KiB even though the draw consumes only a
// handful of elements. Keep the indexed GX path, but rewrite those indices to a
// dense 0..N-1 domain and pack only the elements the list actually references.
// This avoids the direct-vertex expansion that previously caused host-memory
// growth while preserving the shader/pipeline path and the fetched byte values.
struct SparseDlScratch {
    std::vector<uint8_t> rewritten;
    std::array<std::vector<uint32_t>, GX_VA_MAX_ATTR> usedIndices;
    std::array<std::array<std::vector<uint8_t>, 2>, GX_VA_MAX_ATTR> packedArrays;
    std::array<uint32_t, GX_VA_MAX_ATTR> packedCursor{};

    void ResetForList(uint32_t nbytes) {
        rewritten.clear();
        rewritten.reserve(nbytes);
        for (auto& indices : usedIndices) indices.clear();
    }
};

static SparseDlScratch& GetSparseDlScratch() {
    static thread_local SparseDlScratch scratch;
    return scratch;
}

static uint32_t DenseIndexFor(std::vector<uint32_t>& used, uint32_t original) {
    const auto it = std::find(used.begin(), used.end(), original);
    if (it != used.end()) {
        return static_cast<uint32_t>(it - used.begin());
    }
    used.push_back(original);
    return static_cast<uint32_t>(used.size() - 1u);
}

static void StoreBE16(uint8_t* ptr, uint16_t value) {
    ptr[0] = static_cast<uint8_t>(value >> 8);
    ptr[1] = static_cast<uint8_t>(value);
}

struct DlSparseIndexRewriteVisitor {
    static constexpr int kMaxDepth = 0;
    static constexpr bool kHandlesDraw = true;
    static constexpr uint32_t kMaxDraws = 64;
    static constexpr uint32_t kMaxVertices = 1024;

    SparseDlScratch& scratch;
    ScanLayoutCacheEntry layout{};
    uint32_t drawCount = 0;
    uint32_t totalVertices = 0;

    bool OnNop(uint8_t cmd) {
        scratch.rewritten.push_back(cmd);
        return true;
    }
    bool OnInvalidateVertexCache(uint8_t cmd) {
        scratch.rewritten.push_back(cmd);
        return true;
    }
    bool OnUnknownCommand(uint8_t) { return false; }
    // CP writes can change the vertex layout/bindings in the middle of the
    // list. The sparse path is intentionally restricted to state-stable lists.
    bool OnCpReg(const uint8_t*, uint8_t, uint32_t) { return false; }
    bool OnBpReg(const uint8_t* cmdPtr, uint32_t) {
        AppendBytes(scratch.rewritten, cmdPtr, 5);
        return true;
    }
    bool OnXfReg(const uint8_t* cmdPtr, uint32_t payloadBytes) {
        AppendBytes(scratch.rewritten, cmdPtr, 1u + payloadBytes);
        return true;
    }
    bool OnIndexedXf(const uint8_t*, uint8_t, uint32_t) { return false; }
    bool OnCallDisplayList(uint32_t, uint32_t, int) { return false; }

    uint32_t DrawVertexSize(GXVtxFmt vtxfmt) {
        BuildScanVertexLayout(vtxfmt, layout.indexedSteps, layout.indexedStepCount, layout.vertexSize);
        return layout.vertexSize;
    }

    bool OnDraw(const uint8_t* cmdPtr, uint8_t, GXVtxFmt, uint16_t vtxCount,
                const uint8_t* vertices, uint32_t& payloadBytes) {
        // A state-stable list can safely share one remap domain across multiple
        // draws: usedIndices is the union in first-use order and every rewritten
        // index points at the correspondingly packed element after the walk.
        // Bound the walk so DenseIndexFor's small linear lookup cannot become an
        // unbounded host-side cost on malformed or unexpected guest input.
        if (++drawCount > kMaxDraws) return false;
        totalVertices += vtxCount;
        if (totalVertices > kMaxVertices) return false;

        const size_t outBase = scratch.rewritten.size();
        AppendBytes(scratch.rewritten, cmdPtr, 3u + payloadBytes);
        uint8_t* dstVertices = scratch.rewritten.data() + outBase + 3u;

        for (uint16_t v = 0; v < vtxCount; ++v) {
            const uint8_t* srcVertex = vertices + static_cast<size_t>(v) * layout.vertexSize;
            uint8_t* dstVertex = dstVertices + static_cast<size_t>(v) * layout.vertexSize;
            for (uint32_t i = 0; i < layout.indexedStepCount; ++i) {
                const auto& step = layout.indexedSteps[i];
                const int attr = static_cast<int>(step.attr);
                const uint32_t indexSize = step.index16 ? 2u : 1u;
                const uint32_t indexCount = std::max(1u, step.size / indexSize);
                for (uint32_t indexIdx = 0; indexIdx < indexCount; ++indexIdx) {
                    const size_t indexOffset = static_cast<size_t>(step.offset) +
                                               static_cast<size_t>(indexIdx) * indexSize;
                    const uint8_t* srcIndex = srcVertex + indexOffset;
                    uint8_t* dstIndex = dstVertex + indexOffset;
                    const uint32_t original = step.index16 ? ReadBE16(srcIndex) : *srcIndex;
                    const uint32_t dense = DenseIndexFor(scratch.usedIndices[attr], original);
                    if (step.index16) {
                        if (dense > 0xffffu) return false;
                        StoreBE16(dstIndex, static_cast<uint16_t>(dense));
                    } else {
                        if (dense > 0xffu) return false;
                        *dstIndex = static_cast<uint8_t>(dense);
                    }
                }
            }
        }
        return true;
    }
};

static bool RewriteSparseIndexedDisplayList(const uint8_t* data, uint32_t nbytes,
                                            SparseDlScratch& scratch) {
    if (nbytes == 0 || nbytes > kDlScanCacheMaxEntryBytes) return false;
    scratch.ResetForList(nbytes);
    DlSparseIndexRewriteVisitor visitor{scratch};
    if (!WalkDisplayList(data, nbytes, visitor, 0) || visitor.drawCount == 0u) {
        return false;
    }
    for (const auto& used : scratch.usedIndices) {
        if (!used.empty()) return true;
    }
    return false;
}

// Interpreter fallback
struct DlInterpretVisitor {
    static constexpr int kMaxDepth = 8;
    static constexpr bool kHandlesDraw = true;

    bool OnNop(uint8_t) { return true; }
    bool OnInvalidateVertexCache(uint8_t cmd) {
        // Keep Aurora's indexed-array upload cache coherent with guest CPU
        // writes. GXCallDisplayList consumes this one-byte command
        // synchronously, so a stack-local packet is sufficient here.
        GXCallDisplayList(&cmd, 1);
        GXMarkFrameWork();
        return true;
    }
    bool OnUnknownCommand(uint8_t) { return true; }
    bool OnCpReg(const uint8_t* cmdPtr, uint8_t reg, uint32_t value) {
        ApplyCpRegWrite(reg, value);
        // The HLE CP shadow intentionally virtualizes VCD/VAT and guest array
        // bases, but does not model the CP matrix-index registers. Preserve
        // those two writes in-order in Aurora rather than losing them while
        // interpreting a state-only display list.
        if (reg == 0x30 || reg == 0x40) {
            GXCallDisplayList(cmdPtr, 6);
            GXMarkFrameWork();
        }
        return true;
    }
    bool OnBpReg(const uint8_t*, uint32_t bpWord) {
        GX_HLE_ApplyBPReg(static_cast<uint8_t>(bpWord >> 24), bpWord & 0x00FFFFFFu);
        // GX_HLE_ApplyBPReg defers raw display-copy presentation until the BP
        // packet boundary.  The display-list walker is already at that boundary
        // here, so finish it before interpreting any following command.
        GX_HLE_FinishPendingRawDisplayCopy();
        return true;
    }
    bool OnXfReg(const uint8_t* cmdPtr, uint32_t payloadBytes) {
        GXCallDisplayList(cmdPtr, 1u + payloadBytes);
        GXMarkFrameWork();
        return true;
    }
    bool OnIndexedXf(const uint8_t* cmdPtr, uint8_t cmd, uint32_t value) {
        ApplyIndexedXfArrayForPacket(cmd, value);
        GXCallDisplayList(cmdPtr, 5);
        GXMarkFrameWork();
        return true;
    }
    bool OnCallDisplayList(uint32_t addr, uint32_t size, int depth) {
        if (addr == 0 || size == 0) return true;
        const uint8_t* nested = static_cast<const uint8_t*>(GuestToHostPtr(addr, size));
        if (!nested) return true;
        DlInterpretVisitor child;
        WalkDisplayList(nested, size, child, depth + 1);
        return true;
    }
    uint32_t DrawVertexSize(GXVtxFmt vtxfmt) { return CalcDLVertexSize(vtxfmt); }
    bool OnDraw(const uint8_t*, uint8_t opcode, GXVtxFmt vtxfmt, uint16_t vtxCount,
                const uint8_t* vertices, uint32_t& payloadBytes) {
        EnsureAuroraFrameActive();
        const HleGxVertexStateSnapshot drawGuestState = CaptureGxVertexState();
        ApplyAuroraVtxStateForDlBegin(vtxfmt);
        GXBegin(OpcodeToGXPrimitive(opcode), vtxfmt, vtxCount);
        GXMarkFrameWork();
        // The per-vertex decode is what advances the cursor here, exactly as
        // before the walk was shared: report what it actually consumed rather
        // than the vtxCount * stride product the walker precomputed.
        uint32_t consumed = 0;
        for (uint16_t v = 0; v < vtxCount; ++v) {
            consumed += SubmitDLVertex(vertices + consumed, vtxfmt, drawGuestState.vtxDesc);
        }
        GXEnd();
        RestoreGxVertexState(drawGuestState);
        payloadBytes = consumed;
        return true;
    }
};

static void ParseDisplayList(const uint8_t* data, uint32_t nbytes) {
    DlInterpretVisitor visitor;
    WalkDisplayList(data, nbytes, visitor, 0);
}

struct DlIndexScanVisitor {
    static constexpr int kMaxDepth = 4;
    static constexpr bool kHandlesDraw = true;

    static std::array<ScanLayoutCacheEntry, 8>& LayoutCache() {
        static thread_local std::array<ScanLayoutCacheEntry, 8> cache;
        return cache;
    }
    static uint32_t NextLayoutGeneration() {
        static thread_local uint32_t counter = 0;
        return ++counter;
    }

    std::array<uint32_t, GX_VA_MAX_ATTR>& maxIdx;
    std::array<bool, GX_VA_MAX_ATTR>& sawIdx;
    std::array<uint32_t, GX_VA_MAX_ATTR>& maxXfIdx;
    std::array<uint32_t, GX_VA_MAX_ATTR>& maxXfBytes;
    std::array<bool, GX_VA_MAX_ATTR>& sawXfIdx;
    GXVtxFmt* outFmt;
    bool* outFmtMixed;
    bool* outHasNestedDl;
    bool* outHasArrayStateWrites;
    bool* outHasCpWrites;
    std::vector<DlCpWrite>* outCpWrites;
    // Every walk - including each nested one - gets its own layout generation, so
    // a cached layout can only be reused inside the walk that built it.
    uint32_t layoutVersion = 1;
    uint32_t layoutGeneration = NextLayoutGeneration();
    const ScanLayoutCacheEntry* pendingLayout = nullptr;

    bool OnNop(uint8_t) { return true; }
    bool OnBpReg(const uint8_t*, uint32_t) { return true; }
    bool OnXfReg(const uint8_t*, uint32_t) { return true; }
    bool OnInvalidateVertexCache(uint8_t) { return true; }
    bool OnUnknownCommand(uint8_t) { return false; }
    bool OnCpReg(const uint8_t*, uint8_t reg, uint32_t value) {
        if (outHasCpWrites) *outHasCpWrites = true;
        if (reg >= 0xA0 && reg <= 0xBF && outHasArrayStateWrites) *outHasArrayStateWrites = true;
        if (outCpWrites != nullptr) outCpWrites->push_back(DlCpWrite{reg, value});
        ApplyCpRegWrite(reg, value);
        if (reg == 0x50 || reg == 0x60 || (reg >= 0x70 && reg <= 0x97)) ++layoutVersion;
        return true;
    }
    bool OnIndexedXf(const uint8_t*, uint8_t cmd, uint32_t value) {
        const int attr =
            GX_POS_MTX_ARRAY + (((cmd & GX_OPCODE_MASK_CMD) - GX_LOAD_INDX_A_CMD) / 0x08);
        const uint32_t index = value >> 16;
        const uint32_t bytes = (((value >> 12) & 0x0fu) + 1u) * 4u;
        if (attr >= 0 && attr < GX_VA_MAX_ATTR) {
            if (!sawXfIdx[attr] || index > maxXfIdx[attr]) maxXfIdx[attr] = index;
            if (bytes > maxXfBytes[attr]) maxXfBytes[attr] = bytes;
            sawXfIdx[attr] = true;
        }
        return true;
    }
    bool OnCallDisplayList(uint32_t addr, uint32_t size, int depth) {
        if (addr == 0 || size == 0) return true;
        if (outHasNestedDl) *outHasNestedDl = true;
        try {
            const uint8_t* nested = static_cast<const uint8_t*>(GuestToHostPtr(addr, size));
            if (nested) {
                DlIndexScanVisitor child{maxIdx,         sawIdx,         maxXfIdx,
                                         maxXfBytes,     sawXfIdx,       outFmt,
                                         outFmtMixed,    outHasNestedDl, outHasArrayStateWrites,
                                         outHasCpWrites, outCpWrites};
                if (!WalkDisplayList(nested, size, child, depth + 1)) return false;
            }
            ++layoutVersion;
        } catch (...) {
            return false;
        }
        return true;
    }
    uint32_t DrawVertexSize(GXVtxFmt vtxfmt) {
        if (outFmt) {
            if (*outFmt == GX_MAX_VTXFMT) *outFmt = vtxfmt;
            else if (*outFmt != vtxfmt && outFmtMixed) *outFmtMixed = true;
        }
        pendingLayout = &GetScanVertexLayout(vtxfmt, LayoutCache(), layoutVersion, layoutGeneration);
        return pendingLayout->vertexSize;
    }
    bool OnDraw(const uint8_t*, uint8_t, GXVtxFmt, uint16_t vtxCount, const uint8_t* vertices,
                uint32_t&) {
        ScanDLVerticesMaxIndices(vertices, vtxCount, pendingLayout->indexedSteps,
                                 pendingLayout->indexedStepCount, pendingLayout->vertexSize, maxIdx,
                                 sawIdx);
        return true;
    }
};

static bool ScanDisplayListMaxIndices(const uint8_t* data, uint32_t nbytes,
                                      std::array<uint32_t, GX_VA_MAX_ATTR>& maxIdx,
                                      std::array<bool, GX_VA_MAX_ATTR>& sawIdx,
                                      std::array<uint32_t, GX_VA_MAX_ATTR>& maxXfIdx,
                                      std::array<uint32_t, GX_VA_MAX_ATTR>& maxXfBytes,
                                      std::array<bool, GX_VA_MAX_ATTR>& sawXfIdx,
                                      GXVtxFmt* outFmt, bool* outFmtMixed, bool* outHasNestedDl,
                                      bool* outHasArrayStateWrites, bool* outHasCpWrites,
                                      std::vector<DlCpWrite>* outCpWrites) {
    DlIndexScanVisitor visitor{maxIdx,         sawIdx,         maxXfIdx,
                               maxXfBytes,     sawXfIdx,       outFmt,
                               outFmtMixed,    outHasNestedDl, outHasArrayStateWrites,
                               outHasCpWrites, outCpWrites};
    return WalkDisplayList(data, nbytes, visitor, 0);
}

// Inlines nested lists into one flat command stream aurora can consume directly.
// `outVerbatim` stays true only while every consumed command is appended
// unchanged, i.e. the flattened stream is a byte-for-byte copy of the source
// list. Callers use that to hand aurora the guest bytes directly.
struct DlFlattenVisitor {
    static constexpr int kMaxDepth = 8;
    static constexpr bool kHandlesDraw = true;

    std::vector<uint8_t>& out;
    bool skipArrayState;
    bool* outVerbatim;

    bool OnNop(uint8_t cmd) { out.push_back(cmd); return true; }
    bool OnInvalidateVertexCache(uint8_t cmd) { out.push_back(cmd); return true; }
    bool OnUnknownCommand(uint8_t) { return false; }
    bool OnBpReg(const uint8_t* cmdPtr, uint32_t) { AppendBytes(out, cmdPtr, 5); return true; }
    bool OnIndexedXf(const uint8_t* cmdPtr, uint8_t, uint32_t) { AppendBytes(out, cmdPtr, 5); return true; }
    bool OnXfReg(const uint8_t* cmdPtr, uint32_t payloadBytes) {
        AppendBytes(out, cmdPtr, 1u + payloadBytes);
        return true;
    }
    // Command byte + the 2-byte vertex count + the vertex payload.
    bool OnDraw(const uint8_t* cmdPtr, uint8_t, GXVtxFmt, uint16_t, const uint8_t*,
                uint32_t& payloadBytes) {
        AppendBytes(out, cmdPtr, 3u + payloadBytes);
        return true;
    }
    uint32_t DrawVertexSize(GXVtxFmt vtxfmt) { return CalcDLVertexSize(vtxfmt); }
    bool OnCpReg(const uint8_t* cmdPtr, uint8_t reg, uint32_t value) {
        const bool arrayBaseOrStride = (reg >= 0xA0 && reg <= 0xBF);
        if (!skipArrayState || !arrayBaseOrStride) {
            AppendBytes(out, cmdPtr, 6);
        } else if (outVerbatim != nullptr) {
            *outVerbatim = false;
        }
        ApplyCpRegWrite(reg, value);
        return true;
    }
    bool OnCallDisplayList(uint32_t addr, uint32_t size, int depth) {
        if (outVerbatim != nullptr) *outVerbatim = false;
        if (addr == 0 || size == 0) return true;
        const uint8_t* nested = static_cast<const uint8_t*>(GuestToHostPtr(addr, size));
        if (!nested) return true;
        DlFlattenVisitor child{out, skipArrayState, outVerbatim};
        return WalkDisplayList(nested, size, child, depth + 1);
    }
};

static bool FlattenDisplayListForAurora(const uint8_t* data, uint32_t nbytes,
                                        std::vector<uint8_t>& out, bool skipArrayState,
                                        bool* outVerbatim = nullptr) {
    DlFlattenVisitor visitor{out, skipArrayState, outVerbatim};
    return WalkDisplayList(data, nbytes, visitor, 0);
}

static std::vector<uint8_t>& FlattenDisplayListScratch() {
    static thread_local std::vector<uint8_t> s_scratch;
    return s_scratch;
}

static bool s_appliedVtxDescValid = false;
static std::array<GXAttrType, 26> s_appliedVtxDesc{};
static std::array<GXAttrType, 26> s_appliedSourceVtxDesc{};
static bool s_appliedVtxAttrFmtValid = false;
static VtxAttrFmt s_appliedVtxAttrFmt[8][26]{};

struct AppliedArrayState {
    const void* data = nullptr;
    uint32_t size = 0;
    uint8_t stride = 0;
    bool valid = false;
};

static std::array<AppliedArrayState, GX_VA_MAX_ATTR> s_appliedArrays{};
static bool s_preferDeindexedDisplayLists = false;
static bool s_enableSparseIndexedDisplayLists = false;

// True only while the applied-state mirror is a verbatim copy of HLE vertex state at
// generation s_appliedVtxStateGeneration; any HLE-side mutation bumps that generation, so the
// memoized republish below can never observe a stale match.
static bool s_appliedVtxStateMatchesHle = false;
static uint32_t s_appliedVtxStateGeneration = 0;

static void InvalidateAppliedAuroraState() {
    s_appliedVtxDescValid = false;
    s_appliedVtxAttrFmtValid = false;
    s_appliedVtxStateMatchesHle = false;
    for (auto& array : s_appliedArrays) {
        array.valid = false;
    }
}

static void SyncAppliedVtxStateFromHleReal() {
    // skip the republish only when the mirror is already an exact copy of
    // HLE state
    if (s_appliedVtxStateMatchesHle && s_appliedVtxDescValid && s_appliedVtxAttrFmtValid &&
        s_appliedVtxStateGeneration == g_hleGxState.vtxStateGeneration) {
        return;
    }
    // Both mirrors are dense arrays of the same trivially copyable element types
    // as their HLE counterparts, so publish them with three memcpys instead of
    // 234 individual element assignments.
    static_assert(sizeof(s_appliedVtxDesc) == sizeof(g_hleGxState.vtxDesc));
    static_assert(sizeof(s_appliedSourceVtxDesc) == sizeof(g_hleGxState.vtxDesc));
    static_assert(sizeof(s_appliedVtxAttrFmt) == sizeof(g_hleGxState.vtxAttrFmt));
    std::memcpy(s_appliedVtxDesc.data(), g_hleGxState.vtxDesc, sizeof(g_hleGxState.vtxDesc));
    std::memcpy(s_appliedSourceVtxDesc.data(), g_hleGxState.vtxDesc, sizeof(g_hleGxState.vtxDesc));
    s_appliedVtxDescValid = true;

    std::memcpy(s_appliedVtxAttrFmt, g_hleGxState.vtxAttrFmt, sizeof(g_hleGxState.vtxAttrFmt));
    s_appliedVtxAttrFmtValid = true;

    s_appliedVtxStateMatchesHle = true;
    s_appliedVtxStateGeneration = g_hleGxState.vtxStateGeneration;
}

static void ApplyAuroraArrayIfChanged(GXAttr attr, const void* data, uint32_t size, uint8_t stride) {
    auto& applied = s_appliedArrays[attr];
    if (applied.valid && applied.data == data && applied.stride == stride) {
        // Display-list scans bind exactly the byte span needed by that list's
        // highest index.  Retail commonly reuses the same base/stride across
        // many lists whose maxima differ.  Shrinking the Aurora binding for a
        // smaller list is unnecessary (all indices still fall inside the
        // existing superset) and, more importantly, invalidates cachedRange and
        // forces the same guest array to be copied into storage again.  Keep a
        // monotonic span for an unchanged binding and only republish when a
        // later list actually needs more bytes.
        if (applied.size >= size) {
            return;
        }
    }
    GXSetArray(attr, data, size, stride);
    applied.data = data;
    applied.size = size;
    applied.stride = stride;
    applied.valid = true;
}

// Aurora's live vertex descriptor for one attribute, read straight back out of
// its CP shadow registers (GXGetVtxDesc is the exact inverse of GXSetVtxDesc,
// GX_VA_NRM included). This is the ground truth the applied-state mirror is
// checked against below; it is a switch plus a shift/mask, no state change and
// no FIFO traffic.
static bool AuroraVtxDescMatchesLive(GXAttr attr, GXAttrType expected) {
    GXAttrType live = GX_NONE;
    GXGetVtxDesc(attr, &live);
    return live == expected;
}

static bool AuroraVtxAttrFmtMatchesLive(GXVtxFmt fmt, GXAttr attr, const VtxAttrFmt& expected) {
    GXCompCnt liveCnt = expected.cnt;
    GXCompType liveType = expected.type;
    uint8_t liveFrac = expected.frac;
    GXGetVtxAttrFmt(fmt, attr, &liveCnt, &liveType, &liveFrac);
    return liveCnt == expected.cnt && liveType == expected.type && liveFrac == expected.frac;
}


static void ApplyAuroraVtxDesc() {
    const bool hadValidState = s_appliedVtxDescValid;
    if (!hadValidState) {
        s_appliedVtxStateMatchesHle = false;
    }
    for (int attr = 0; attr < 26; ++attr) {
        if (attr == GX_VA_NBT) continue;
        const GXAttr gxAttr = static_cast<GXAttr>(attr);
        const GXAttrType type = g_hleGxState.vtxDesc[attr];
        bool needsPublish = !hadValidState || s_appliedVtxDesc[attr] != type;
        // The mirror can be invalidated behind our back by immediate/raw GX paths
        // that publish directly into Aurora.  For real CP descriptor attributes,
        // validate the live shadow before trusting a cached equality.  XF array
        // pseudo-attributes above GX_VA_TEX7 have no VCD bits and are not probed.
        if (!needsPublish && gxAttr <= GX_VA_TEX7) {
            needsPublish = !AuroraVtxDescMatchesLive(gxAttr, type);
        }
        if (needsPublish) {
            GXSetVtxDesc(gxAttr, type);
            s_appliedVtxDesc[attr] = type;
        }
        if (!hadValidState || s_appliedSourceVtxDesc[attr] != type) {
            GXSetSourceVtxDesc(gxAttr, type);
            s_appliedSourceVtxDesc[attr] = type;
        }
    }
    s_appliedVtxDescValid = true;
}

// Size of one element of the guest array an indexed attribute reads from. An
// NBT3 normal is indexed per 3-component group, not per 9-component triple.
static uint32_t GetIndexedAttrElementSizeBytes(GXAttr attr, const VtxAttrFmt& fmt) {
    if (attr == GX_VA_NRM && fmt.cnt == GX_NRM_NBT3) {
        return 3u * static_cast<uint32_t>(GetCompSizeBytes(fmt.type));
    }
    return DirectAttrByteSize(attr, fmt, /*matrixAttrIsOneByte=*/false, /*fallbackComps=*/0u);
}

static void ApplyAuroraVtxAttrFmt(GXVtxFmt fmt) {
    const bool hadValidState = s_appliedVtxAttrFmtValid;
    // Rows other than `fmt` keep their contents and every write below copies
    // g_hleGxState, so an already-matching mirror still matches. Only the rebuild
    // from an invalidated mirror leaves the untouched rows divergent.
    if (!hadValidState) {
        s_appliedVtxStateMatchesHle = false;
    }
    for (int attr = 0; attr < 26; ++attr) {
        if (attr == GX_VA_NBT) continue;
        const GXAttr gxAttr = static_cast<GXAttr>(attr);
        if (IsMatrixIndexAttr(gxAttr)) continue;
        const auto& f = g_hleGxState.vtxAttrFmt[fmt][attr];
        bool needsPublish = !hadValidState || !SameVtxAttrFmt(s_appliedVtxAttrFmt[fmt][attr], f);
        // As with VCD, other HLE draw paths may have changed Aurora's live VAT
        // row without touching this display-list mirror.  Query only real VAT
        // attributes; the XF array pseudo-attributes have no VAT encoding.
        if (!needsPublish && gxAttr >= GX_VA_POS && gxAttr <= GX_VA_TEX7) {
            needsPublish = !AuroraVtxAttrFmtMatchesLive(fmt, gxAttr, f);
        }
        if (!needsPublish) {
            continue;
        }
        GXSetVtxAttrFmt(fmt, gxAttr, f.cnt, f.type, f.frac);
        s_appliedVtxAttrFmt[fmt][attr] = f;
    }
    s_appliedVtxAttrFmtValid = true;
}

static void ApplyAuroraVtxAttrFmtAll() {
    for (int fmt = 0; fmt < 8; ++fmt) {
        ApplyAuroraVtxAttrFmt(static_cast<GXVtxFmt>(fmt));
    }
}

static void ApplyAuroraVtxAttrFmtForDisplayList(GXVtxFmt fmt, bool mixed) {
    if (!mixed && fmt < GX_MAX_VTXFMT) {
        ApplyAuroraVtxAttrFmt(fmt);
        return;
    }
    ApplyAuroraVtxAttrFmtAll();
}

static void SubmitLytDrawPacket(const uint8_t* packet, uint32_t packetBytes) {
    if (packet == nullptr || packetBytes == 0) {
        return;
    }
    EnsureAuroraFrameActive();
    ApplyAuroraVtxDesc();
    ApplyAuroraVtxAttrFmtForDisplayList(GX_VTXFMT0, false);
    EnsureDefaultGxAlphaCompare();

    GXCallDisplayList(packet, packetBytes);
    GXMarkFrameWork();
    SyncAppliedVtxStateFromHleReal();
}

static bool ApplyAuroraSparseArraysForDisplayList(SparseDlScratch& scratch, GXVtxFmt vtxfmt) {
    if (vtxfmt >= GX_MAX_VTXFMT) {
        return false;
    }

    struct PreparedBinding {
        GXAttr attr = GX_VA_NULL;
        const uint8_t* source = nullptr;
        uint32_t sourceStride = 0;
        uint32_t elemSize = 0;
        uint32_t packedSize = 0;
        uint32_t packedSlot = 0;
    };
    std::array<PreparedBinding, GX_VA_MAX_ATTR> prepared{};
    uint32_t preparedCount = 0;
    uint64_t originalBytes = 0;
    uint64_t compactBytes = 0;

    // Validate every binding and estimate the gain before publishing any GX
    // state. A failed/weak candidate can then fall back to the normal max-index
    // path without leaving a partially rewritten array configuration behind.
    for (int attr = 0; attr < 26; ++attr) {
        const auto& used = scratch.usedIndices[attr];
        if (used.empty()) continue;

        const GXAttrType type = g_hleGxState.vtxDesc[attr];
        if (type != GX_INDEX8 && type != GX_INDEX16) return false;
        const auto& arr = g_hleGxState.vtxArray[attr];
        if (arr.base == 0 || arr.stride == 0) return false;

        const GXAttr gxAttr = static_cast<GXAttr>(attr);
        const auto& fmt = g_hleGxState.vtxAttrFmt[vtxfmt][attr];
        const uint32_t elemSize = GetIndexedAttrElementSizeBytes(gxAttr, fmt);
        if (elemSize == 0) return false;

        uint32_t maxIndex = 0;
        for (uint32_t index : used) maxIndex = std::max(maxIndex, index);
        const uint64_t span64 = static_cast<uint64_t>(maxIndex) * arr.stride + elemSize;
        const uint64_t packed64 = static_cast<uint64_t>(used.size()) * elemSize;
        if (span64 == 0 || span64 > std::numeric_limits<uint32_t>::max() ||
            packed64 == 0 || packed64 > 256u * 1024u) {
            return false;
        }
        const uint32_t span = static_cast<uint32_t>(span64);
        if (!Memory::Contains(arr.base, span)) return false;
        const uint8_t* source = static_cast<const uint8_t*>(GuestToHostPtr(arr.base, span));
        if (!source) return false;

        originalBytes += span64;
        compactBytes += packed64;
        prepared[preparedCount++] = PreparedBinding{
            .attr = gxAttr,
            .source = source,
            .sourceStride = arr.stride,
            .elemSize = elemSize,
            .packedSize = static_cast<uint32_t>(packed64),
            .packedSlot = (scratch.packedCursor[attr] + 1u) & 1u,
        };
    }

    // Restrict the optimization to the pathological case. This keeps ordinary
    // small/index-local display lists on the established zero-copy scan path.
    // STF's current hot loop contains a recurring ~8 KiB class, so use 4 KiB as
    // the floor but demand at least a 4x reduction before taking the remap.
    if (preparedCount == 0 || originalBytes < 4u * 1024u || compactBytes * 4u > originalBytes) {
        return false;
    }

    // Prepare every packed backing store before publishing any GX array state.
    // If allocation/copy fails, the outer caller can fall back without leaving
    // a partially switched set of array bindings behind.
    for (uint32_t i = 0; i < preparedCount; ++i) {
        const auto& binding = prepared[i];
        const int attr = static_cast<int>(binding.attr);
        const auto& used = scratch.usedIndices[attr];
        auto& packed = scratch.packedArrays[attr][binding.packedSlot];
        packed.resize(binding.packedSize);
        for (size_t dense = 0; dense < used.size(); ++dense) {
            const uint8_t* src = binding.source + static_cast<size_t>(used[dense]) * binding.sourceStride;
            uint8_t* dst = packed.data() + dense * binding.elemSize;
            std::memcpy(dst, src, binding.elemSize);
        }
    }

    for (uint32_t i = 0; i < preparedCount; ++i) {
        const auto& binding = prepared[i];
        const int attr = static_cast<int>(binding.attr);
        auto& packed = scratch.packedArrays[attr][binding.packedSlot];
        scratch.packedCursor[attr] = binding.packedSlot;
        // Packed scratch is ephemeral: unlike a guest array, a reused vector
        // slot may shrink. Do not keep the normal monotonic-size superset or
        // Aurora could snapshot bytes beyond this pack's current logical size.
        s_appliedArrays[attr].valid = false;
        ApplyAuroraArrayIfChanged(binding.attr, packed.data(), binding.packedSize,
                                  static_cast<uint8_t>(binding.elemSize));
    }
    return true;
}

static bool ApplyAuroraArraysForDisplayList(const std::array<uint32_t, GX_VA_MAX_ATTR>& maxIdx,
                                            const std::array<bool, GX_VA_MAX_ATTR>& sawIdx,
                                            GXVtxFmt vtxfmt,
                                            bool vtxfmtMixed) {
    static std::array<std::array<std::vector<uint8_t>, 2>, GX_VA_MAX_ATTR> s_packedArrays;
    static std::array<uint32_t, GX_VA_MAX_ATTR> s_packedArrayCursor{};
    bool ok = true;
    if (vtxfmt >= GX_MAX_VTXFMT) {
        vtxfmt = g_hleGxState.currentVtxFmt;
    }
    for (int attr = 0; attr < 26; ++attr) {
        const GXAttrType type = g_hleGxState.vtxDesc[attr]; if (type != GX_INDEX8 && type != GX_INDEX16) continue;
        if (!sawIdx[attr]) continue;
        const auto& arr = g_hleGxState.vtxArray[attr];
        if (arr.base == 0 || arr.stride == 0) { ok = false; continue; }
        const uint32_t count = maxIdx[attr] + 1u;
        const GXAttr gxAttr = static_cast<GXAttr>(attr);
        const auto& fmt = g_hleGxState.vtxAttrFmt[vtxfmt][attr];
        const uint32_t elemSize = GetIndexedAttrElementSizeBytes(gxAttr, fmt);
        if (elemSize == 0) {
            ok = false;
            continue;
        }
        const uint32_t span = (count > 0) ? (static_cast<uint32_t>((count - 1u) * arr.stride) + elemSize) : 0u;
        const uint64_t packedBytes = static_cast<uint64_t>(count) * elemSize;
        // A display list usually references only one attribute out of an
        // interleaved retail vertex record. Binding base..maxIndex with the
        // original record stride makes Aurora snapshot all of the unrelated
        // padding/other attributes between those elements. STF's hot renderer
        // does this thousands of times per frame, turning tiny DLs into 20-230
        // KiB storage uploads each. A dense 0..maxIndex copy preserves every
        // index value while shrinking only the physical stride seen by Aurora.
        //
        // Do not compact mixed-VAT lists: one max-index summary cannot prove a
        // single element width covers every VAT row used inside that list. The
        // old >255-stride compatibility path is retained as-is for those lists.
        const bool materiallySmaller =
            packedBytes < span && (static_cast<uint64_t>(span) - packedBytes >= 4096u) &&
            (packedBytes * 4u <= static_cast<uint64_t>(span) * 3u);
        const bool needsPack = arr.stride > 0xffu || (!vtxfmtMixed && materiallySmaller);
        if (!Memory::Contains(arr.base, span)) {
            ok = false;
            continue;
        }
        const uint8_t* hostPtr = static_cast<const uint8_t*>(GuestToHostPtr(arr.base, span));
        if (!hostPtr) {
            ok = false;
            continue;
        }
        // Aurora's storage-fetch shaders consume guest big-endian bytes directly
        // and honor the array stride. Keep the backing pointer stable so per-frame
        // storage uploads can be cached instead of repacking every display-list call.
        if (!needsPack) {
            ApplyAuroraArrayIfChanged(gxAttr, hostPtr, span, static_cast<uint8_t>(arr.stride));
            continue;
        }
        if (packedBytes > std::numeric_limits<uint32_t>::max()) {
            ok = false;
            continue;
        }
        auto& packed = s_packedArrays[attr][++s_packedArrayCursor[attr] & 1u];
        if (!PackIndexedAttrData(hostPtr, arr.stride, count, elemSize, packed)) {
            ok = false;
            continue;
        }
        ApplyAuroraArrayIfChanged(gxAttr, packed.data(), static_cast<uint32_t>(packed.size()), static_cast<uint8_t>(elemSize));
    }
    return ok;
}

static bool ApplyAuroraIndexedXFArraysForDisplayList(const std::array<uint32_t, GX_VA_MAX_ATTR>& maxXfIdx,
                                                     const std::array<uint32_t, GX_VA_MAX_ATTR>& maxXfBytes,
                                                     const std::array<bool, GX_VA_MAX_ATTR>& sawXfIdx) {
    bool ok = true;
    for (int attr = GX_POS_MTX_ARRAY; attr <= GX_LIGHT_ARRAY; ++attr) {
        if (!sawXfIdx[attr]) {
            continue;
        }
        const auto& arr = g_hleGxState.vtxArray[attr];
        const uint32_t bytes = maxXfBytes[attr];
        if (arr.base == 0 || arr.stride == 0 || bytes == 0) {
            ok = false;
            continue;
        }
        const uint32_t span = maxXfIdx[attr] * arr.stride + bytes;
        if (!Memory::Contains(arr.base, span)) {
            ok = false;
            continue;
        }
        const uint8_t* hostPtr = static_cast<const uint8_t*>(GuestToHostPtr(arr.base, span));
        if (!hostPtr) {
            ok = false;
            continue;
        }
        ApplyAuroraArrayIfChanged(static_cast<GXAttr>(attr), hostPtr, span, static_cast<uint8_t>(arr.stride));
    }
    return ok;
}

} // namespace

void GX_HLE_SetPreferDeindexedDisplayLists(bool enabled) {
    s_preferDeindexedDisplayLists = enabled;
}

void GX_HLE_SetSparseIndexedDisplayLists(bool enabled) {
    s_enableSparseIndexedDisplayLists = enabled;
    // STF's stage renderer legitimately emits many thousands of CALL_DL draws
    // before GXCopyDisp. Sparse remap removes the 8 MiB storage storm, then this
    // moderate cap lets Aurora retire completed command/bind-group resources
    // without falling back to the old hundreds-of-draw storage split cadence.
    aurora::gx::fifo::set_max_draws_per_batch(enabled ? 8192u : 0u);
    // STF produces ~6K distinct draw bindings per hot frame. Retaining 32 frames
    // of those made Dawn hold tens of GiB. The worker only needs the immediately
    // previous sealed frame alive, so STF sweeps every frame and retains one;
    // disabling the title option restores the generic runtime defaults.
    aurora::gfx::set_bind_group_cache_policy(enabled ? 1u : 32u, enabled ? 1u : 16u);
}

extern "C" void GxNotifyDisplayListMemoryWrite(uint32_t addr, uint32_t size) {
    GxGuestWrite::NotifyWrite(addr, size);
}

extern "C" void GX__CallDisplayList_80172f64(uint32_t listAddr, uint32_t nbytes) {
    if (nbytes == 0 || listAddr == 0) return;
    static std::atomic<uint32_t> s_callLogCount{0};
    uint32_t callLog = 32u;
    if (MeteorRuntimeTraceEnabled()) {
        callLog = s_callLogCount.fetch_add(1, std::memory_order_relaxed);
    }
    if (callLog < 32u) {
        RT_LOGF(RT_TAG_GX, "DL call #%u list=0x%08x bytes=0x%x pos=%u nrm=%u tex0=%u\n",
                callLog, listAddr, nbytes,
                static_cast<unsigned>(g_hleGxState.vtxDesc[GX_VA_POS]),
                static_cast<unsigned>(g_hleGxState.vtxDesc[GX_VA_NRM]),
                static_cast<unsigned>(g_hleGxState.vtxDesc[GX_VA_TEX0]));
    }
    DlDiagRootScope diagRootScope(listAddr, nbytes);
    try {
        const uint8_t* list = static_cast<const uint8_t*>(GuestToHostPtr(listAddr, nbytes));
        if (!list) return;

        const bool allowScanCache = nbytes <= kDlScanCacheMaxEntryBytes;
        const uint64_t scanLayoutHash = allowScanCache ? HashScanLayoutState() : 0;
        // The digest is computed lazily inside the probe: an entry whose covering
        // write-tracking granules have not moved since it was stored keeps its
        // recorded digest and skips the XXH3 pass over the whole list entirely.
        const DlScanCacheProbe probe =
            allowScanCache ? ProbeDlScanCache(list, listAddr, nbytes, scanLayoutHash)
                           : DlScanCacheProbe{};
        const DlScanCacheRecord* cached = probe.record;

        // The scan a cached record came from already walked the list for this,
        // so only a miss pays for DisplayListMayContainDraw.
        const bool mayContainDraw =
            (cached != nullptr) ? cached->mayContainDraw : DisplayListMayContainDraw(list, nbytes);
        if (!mayContainDraw) {
            EnsureAuroraFrameActive();
            GXMarkFrameWork();
            // Draw-free lists still have strict command ordering. Interpret
            // them packet-by-packet so CP array writes take effect before the
            // indexed-XF command that consumes them. The interpreter also
            // forwards CP 0x30/0x40 and GXInvalidateVtxCache explicitly, while
            // virtualizing guest array bases before Aurora sees LOAD_INDX_A-D.
            InvalidateAppliedAuroraState();
            ParseDisplayList(list, nbytes);
            InvalidateAppliedAuroraState();
            return;
        }
        std::array<uint32_t, GX_VA_MAX_ATTR> maxIdx{}; std::array<bool, GX_VA_MAX_ATTR> sawIdx{};
        std::array<uint32_t, GX_VA_MAX_ATTR> maxXfIdx{}; std::array<uint32_t, GX_VA_MAX_ATTR> maxXfBytes{};
        std::array<bool, GX_VA_MAX_ATTR> sawXfIdx{};
        GXVtxFmt dlVtxFmt = GX_MAX_VTXFMT; bool dlVtxFmtMixed = false;
        bool dlHasNestedDl = false;
        bool dlHasArrayStateWrites = false;
        bool dlHasCpWrites = false;

        HleGxVertexStateSnapshot stateBeforeScan;
        bool haveStateBeforeScan = false;

        bool scanOk = false;
        bool needsFlatten = false;
        bool flattenOk = false;
        const uint8_t* auroraStream = nullptr;
        uint32_t auroraStreamBytes = 0;
        if (cached != nullptr) {
            const auto& entry = cached->result;
            scanOk = entry.scanOk;
            maxIdx = entry.maxIdx;
            sawIdx = entry.sawIdx;
            maxXfIdx = entry.maxXfIdx;
            maxXfBytes = entry.maxXfBytes;
            sawXfIdx = entry.sawXfIdx;
            dlVtxFmt = entry.dlVtxFmt;
            dlVtxFmtMixed = entry.dlVtxFmtMixed;
            dlHasNestedDl = entry.dlHasNestedDl;
            dlHasArrayStateWrites = entry.dlHasArrayStateWrites;
            dlHasCpWrites = entry.dlHasCpWrites;
            needsFlatten = entry.needsFlatten;
            flattenOk = entry.flattenOk;
            // A scan/flatten only mutates HLE vertex state through the CP writes
            // embedded in the list. Replaying the recorded writes in stream order
            // lands g_hleGxState exactly where the uncached path would have left it.
            if (dlHasCpWrites) {
                stateBeforeScan = CaptureGxVertexStateForCpWrites(cached->cpWrites);
                haveStateBeforeScan = true;
            }
            for (const auto& write : cached->cpWrites) {
                ApplyCpRegWrite(write.reg, write.value);
            }
            if (needsFlatten && flattenOk) {
                if (entry.flattenVerbatim) {
                    auroraStream = list;
                    auroraStreamBytes = nbytes;
                } else {
                    auroraStream = cached->flattened.data();
                    auroraStreamBytes = static_cast<uint32_t>(cached->flattened.size());
                }
            }
        } else {
            std::vector<DlCpWrite> cpWrites{};
            // The scan discovers the list's CP writes as it walks, so this path
            // cannot narrow the snapshot the way the cached one does.
            stateBeforeScan = CaptureGxVertexState();
            haveStateBeforeScan = true;
            scanOk = ScanDisplayListMaxIndices(list, nbytes, maxIdx, sawIdx, maxXfIdx, maxXfBytes, sawXfIdx,
                                               &dlVtxFmt, &dlVtxFmtMixed, &dlHasNestedDl,
                                               &dlHasArrayStateWrites, &dlHasCpWrites, &cpWrites);
            needsFlatten = dlHasNestedDl || dlHasArrayStateWrites || dlHasCpWrites;
            bool flattenVerbatim = true;
            auto& flattened = FlattenDisplayListScratch();
            if (needsFlatten) {
                RestoreGxVertexState(stateBeforeScan);
                flattened.clear();
                flattenOk = FlattenDisplayListForAurora(list, nbytes, flattened, dlHasArrayStateWrites,
                                                        &flattenVerbatim);
                if (flattenOk) {
                    if (flattenVerbatim) {
                        auroraStream = list;
                        auroraStreamBytes = nbytes;
                    } else {
                        auroraStream = flattened.data();
                        auroraStreamBytes = static_cast<uint32_t>(flattened.size());
                    }
                }
            }
            // Nested lists stay uncached: the digest only covers the outer list, so a
            // rewritten callee would go undetected. Everything else is cacheable since the
            // scan/flatten are pure functions of the list bytes and the layout-hash cache key.
            if (allowScanCache && scanOk && !dlHasNestedDl && (!needsFlatten || flattenOk)) {
                DlScanCacheEntry entry{};
                entry.listAddr = CanonicalizeGxMainRamAddress(listAddr);
                entry.nbytes = nbytes;
                entry.layoutHash = scanLayoutHash;
                entry.maxIdx = maxIdx;
                entry.sawIdx = sawIdx;
                entry.maxXfIdx = maxXfIdx;
                entry.maxXfBytes = maxXfBytes;
                entry.sawXfIdx = sawXfIdx;
                entry.dlVtxFmt = dlVtxFmt;
                entry.dlVtxFmtMixed = dlVtxFmtMixed;
                entry.dlHasNestedDl = dlHasNestedDl;
                entry.dlHasArrayStateWrites = dlHasArrayStateWrites;
                entry.dlHasCpWrites = dlHasCpWrites;
                entry.scanOk = scanOk;
                entry.needsFlatten = needsFlatten;
                entry.flattenOk = flattenOk;
                entry.flattenVerbatim = flattenVerbatim;
                const bool storeFlattened = needsFlatten && flattenOk && !flattenVerbatim;

                const uint64_t contentDigest =
                    probe.digestValid ? probe.contentDigest : DlContentDigest(list, nbytes);
                StoreDlScanCache(listAddr, nbytes, scanLayoutHash, contentDigest,
                                 probe.writeGeneration, mayContainDraw, entry, std::move(cpWrites),
                                 storeFlattened ? flattened.data() : nullptr,
                                 storeFlattened ? static_cast<uint32_t>(flattened.size()) : 0u);
            }
        }

        // STF's retail stage renderer issues thousands of small indexed display
        // lists from one finite traversal. Only divert lists that the scan proved
        // actually fetch vertex attributes by index; direct-only, nested/stateful,
        // and indexed-XF lists keep their established paths. The interpreter
        // expands the indexed fetches to GX_DIRECT at execution time, avoiding
        // repeated whole-array snapshots while preserving the guest's fetch result.
        bool hasIndexedVertexDraw = false;
        for (int attr = GX_VA_POS; attr <= GX_VA_TEX7; ++attr) {
            hasIndexedVertexDraw = hasIndexedVertexDraw || sawIdx[attr];
        }
        bool hasIndexedXf = false;
        for (int attr = 0; attr < GX_VA_MAX_ATTR; ++attr) {
            hasIndexedXf = hasIndexedXf || sawXfIdx[attr];
        }
        if (callLog < 32u) {
            RT_LOGF(RT_TAG_GX,
                    "DL scan #%u may=%u cached=%u scan=%u fmt=%u mixed=%u nested=%u cp=%u arrays=%u flatten=%u/%u indexed=%u xf=%u maxPos=%u maxTex0=%u\n",
                    callLog, mayContainDraw ? 1u : 0u, cached != nullptr ? 1u : 0u,
                    scanOk ? 1u : 0u, static_cast<unsigned>(dlVtxFmt), dlVtxFmtMixed ? 1u : 0u,
                    dlHasNestedDl ? 1u : 0u, dlHasCpWrites ? 1u : 0u,
                    dlHasArrayStateWrites ? 1u : 0u, needsFlatten ? 1u : 0u,
                    flattenOk ? 1u : 0u, hasIndexedVertexDraw ? 1u : 0u,
                    hasIndexedXf ? 1u : 0u, sawIdx[GX_VA_POS] ? maxIdx[GX_VA_POS] : 0u,
                    sawIdx[GX_VA_TEX0] ? maxIdx[GX_VA_TEX0] : 0u);
        }
        if (s_preferDeindexedDisplayLists && scanOk && hasIndexedVertexDraw &&
            !dlHasCpWrites && !dlHasNestedDl && !hasIndexedXf) {
            EnsureAuroraFrameActive();
            GXMarkFrameWork();
            InvalidateAppliedAuroraState();
            ParseDisplayList(list, nbytes);
            InvalidateAppliedAuroraState();
            if (callLog < 32u) RT_LOGF(RT_TAG_GX, "DL path #%u=deindexed\n", callLog);
            return;
        }

        if (dlHasCpWrites) {
            // A whole-list scan ends with the LAST CP state, but draws and
            // indexed-XF loads inside the list consume the state at their own
            // position in the stream. A single prebound host array (or VAT)
            // cannot represent a list which changes its bindings between uses.
            // Replay in order from the entry state; draw-only lists retain the
            // indexed fast path below, including its scan cache.
            if (haveStateBeforeScan) {
                RestoreGxVertexState(stateBeforeScan);
            }
            EnsureAuroraFrameActive();
            GXMarkFrameWork();
            InvalidateAppliedAuroraState();
            ParseDisplayList(list, nbytes);
            InvalidateAppliedAuroraState();
            if (callLog < 32u) RT_LOGF(RT_TAG_GX, "DL path #%u=cp-replay\n", callLog);
            return;
        }

        if (needsFlatten) {
            bool flattenApplyOk = flattenOk && scanOk && auroraStream != nullptr && auroraStreamBytes != 0;
            if (flattenApplyOk) {
                ApplyAuroraVtxDesc();
                ApplyAuroraVtxAttrFmtForDisplayList(dlVtxFmt, dlVtxFmtMixed);
                const bool flattenArraysOk =
                    ApplyAuroraArraysForDisplayList(maxIdx, sawIdx, dlVtxFmt, dlVtxFmtMixed);
                const bool flattenXfOk =
                    ApplyAuroraIndexedXFArraysForDisplayList(maxXfIdx, maxXfBytes, sawXfIdx);
                flattenApplyOk = flattenArraysOk && flattenXfOk;
            }
            if (flattenApplyOk) {
                EnsureAuroraFrameActive();
                GXMarkFrameWork();
                GXCallDisplayList(auroraStream, auroraStreamBytes);
                // No flip back to GX_DIRECT here
                SyncAppliedVtxStateFromHleReal();
                if (callLog < 32u) RT_LOGF(RT_TAG_GX, "DL path #%u=flatten bytes=0x%x\n", callLog, auroraStreamBytes);
                return;
            }

        } else {
            bool applyOk = scanOk;
            bool arraysOk = false;
            bool xfOk = false;
            if (scanOk) {
                ApplyAuroraVtxDesc();
                ApplyAuroraVtxAttrFmtForDisplayList(dlVtxFmt, dlVtxFmtMixed);

                // Keep the indexed renderer, but compact a small state-stable
                // list when its actual index set is dramatically smaller than
                // the base..maxIndex spans the normal path would snapshot.
                if (s_enableSparseIndexedDisplayLists && !dlVtxFmtMixed &&
                    !dlHasNestedDl && !dlHasCpWrites && !hasIndexedXf) {
                    auto& sparse = GetSparseDlScratch();
                    if (RewriteSparseIndexedDisplayList(list, nbytes, sparse) &&
                        ApplyAuroraSparseArraysForDisplayList(sparse, dlVtxFmt)) {
                        EnsureAuroraFrameActive();
                        GXMarkFrameWork();
                        GXCallDisplayList(sparse.rewritten.data(),
                                          static_cast<uint32_t>(sparse.rewritten.size()));
                        SyncAppliedVtxStateFromHleReal();
                        if (callLog < 32u) RT_LOGF(RT_TAG_GX, "DL path #%u=sparse bytes=0x%x\n", callLog,
                                                   static_cast<unsigned>(sparse.rewritten.size()));
                        return;
                    }
                }

                arraysOk = ApplyAuroraArraysForDisplayList(maxIdx, sawIdx, dlVtxFmt, dlVtxFmtMixed);
                xfOk = ApplyAuroraIndexedXFArraysForDisplayList(maxXfIdx, maxXfBytes, sawXfIdx);
                applyOk = arraysOk && xfOk;
            }
            if (applyOk) {
                EnsureAuroraFrameActive();
                GXMarkFrameWork();
                GXCallDisplayList(list, nbytes);

                SyncAppliedVtxStateFromHleReal();
                if (callLog < 32u) RT_LOGF(RT_TAG_GX, "DL path #%u=indexed\n", callLog);
                return;
            }

        }

        if (haveStateBeforeScan) {
            RestoreGxVertexState(stateBeforeScan);
        }
        EnsureAuroraFrameActive();
        GXMarkFrameWork();
        InvalidateAppliedAuroraState();
        ParseDisplayList(list, nbytes);
        InvalidateAppliedAuroraState();
        if (callLog < 32u) RT_LOGF(RT_TAG_GX, "DL path #%u=interpreter\n", callLog);
    } catch (...) {}
}

PPC_NATIVE_OVERRIDE_VOID(80172F64, GX__CallDisplayList_80172f64, (uint32_t listAddr, uint32_t nbytes), (listAddr, nbytes));

extern "C" void nw4r__lyt__detail__DrawQuad_800847c0(CpuContext* ctx) {
    static std::atomic<uint32_t> s_drawQuadLogCount{0};
    const uint32_t drawQuadLog = s_drawQuadLogCount.fetch_add(1, std::memory_order_relaxed);
    if (drawQuadLog < 16u) {
        RT_LOGF(RT_TAG_GX, "Lyt DrawQuad A #%u pos=0x%08x size=0x%08x tex=%d texCoord=0x%08x color=0x%08x\n",
                drawQuadLog, ctx->gpr[3], ctx->gpr[4], static_cast<int32_t>(ctx->gpr[5]),
                ctx->gpr[6], ctx->gpr[7]);
    }
    uint32_t colors[4];
    const uint32_t colorAddr = ctx->gpr[7];
    const uint32_t* colorPtr = nullptr;
    if (colorAddr != 0) {
        colors[0] = ReadLytColor(colorAddr, 0);
        colors[1] = ReadLytColor(colorAddr, 1);
        colors[2] = ReadLytColor(colorAddr, 2);
        colors[3] = ReadLytColor(colorAddr, 3);
        colorPtr = colors;
    }

    EmitLytDrawQuad(ctx->gpr[3], ctx->gpr[4], static_cast<int32_t>(ctx->gpr[5]), ctx->gpr[6], colorPtr);
}

REGISTER_NATIVE_FUNCTION(0x800847C0, nw4r__lyt__detail__DrawQuad_800847c0);

extern "C" void nw4r__lyt__detail__DrawQuad_80084d20(CpuContext* ctx) {
    static std::atomic<uint32_t> s_drawQuadLogCount{0};
    const uint32_t drawQuadLog = s_drawQuadLogCount.fetch_add(1, std::memory_order_relaxed);
    if (drawQuadLog < 16u) {
        RT_LOGF(RT_TAG_GX, "Lyt DrawQuad B #%u pos=0x%08x size=0x%08x tex=%d texCoord=0x%08x color=0x%08x alpha=0x%08x\n",
                drawQuadLog, ctx->gpr[3], ctx->gpr[4], static_cast<int32_t>(ctx->gpr[5]),
                ctx->gpr[6], ctx->gpr[7], ctx->gpr[8]);
    }
    uint32_t colors[4];
    const uint32_t colorAddr = ctx->gpr[7];
    const uint32_t* colorPtr = nullptr;
    if (colorAddr != 0) {
        const uint32_t alpha = ctx->gpr[8];
        colors[0] = ReadModulatedLytColor(colorAddr, 0, alpha);
        colors[1] = ReadModulatedLytColor(colorAddr, 1, alpha);
        colors[2] = ReadModulatedLytColor(colorAddr, 2, alpha);
        colors[3] = ReadModulatedLytColor(colorAddr, 3, alpha);
        colorPtr = colors;
    }

    EmitLytDrawQuad(ctx->gpr[3], ctx->gpr[4], static_cast<int32_t>(ctx->gpr[5]), ctx->gpr[6], colorPtr);
}

REGISTER_NATIVE_FUNCTION(0x80084D20, nw4r__lyt__detail__DrawQuad_80084d20);
