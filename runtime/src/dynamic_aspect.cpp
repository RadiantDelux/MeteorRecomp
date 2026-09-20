#include "hle_stubs.h"
#include "memory.h"

#include <algorithm>
#include "aurora_events.h"
#include "hle/gx/gx_dynamic_aspect.h"

#include <dolphin/vi.h>

// Advanced by GXCopyDisp (gx_copy.cpp), defined in gx_utils.cpp. Declared here
// rather than via gx_internal.h, which is private to the GX HLE sources.
extern "C" int g_gxFrameCount;

namespace {

bool g_widescreenConfigured = false;
uint32_t g_lastEggWidth43 = 0;
uint32_t g_lastEggWidth169 = 0;

#if defined(MKW_GENERIC_DOL_BOOT)
// Dragon Ball Z: Budokai Tenkaichi 3 PAL (RDSPAF) uses three title-owned
// projection/layout constants for its anamorphic widescreen path.  These are
// the exact PAL constants used by the established BT3 widescreen patch, mapped
// from DOL file offsets 0x3ACB04/0x3ACEC4/0x3AD570 into guest addresses.
// Keeping the correction in guest/title state avoids touching unrelated GX
// projections such as the 360x360 shadow scratch pass.
constexpr uint32_t kMeteorAspectScaleAddress = 0x8062BA44u;
constexpr uint32_t kMeteorCanvasWidthAddress = 0x8062BE04u;
constexpr uint32_t kMeteorCameraScaleAddress = 0x8062C4B0u;

constexpr float kMeteorAspect43 = 1.0666667222976685f;
constexpr float kMeteorCanvas43 = 477.8666687011719f;
constexpr float kMeteorCamera43 = 0.75f;

constexpr float kMeteorAspect169 = 1.4551676511764526f;
constexpr float kMeteorCanvas169 = 577.32275390625f;
constexpr float kMeteorCamera169 = 0.5f;

void ApplyMeteorWidescreenConstants(bool widescreen) {
    Memory::WriteFloat32(kMeteorAspectScaleAddress, widescreen ? kMeteorAspect169 : kMeteorAspect43);
    Memory::WriteFloat32(kMeteorCanvasWidthAddress, widescreen ? kMeteorCanvas169 : kMeteorCanvas43);
    Memory::WriteFloat32(kMeteorCameraScaleAddress, widescreen ? kMeteorCamera169 : kMeteorCamera43);
}
#endif

// EGG::Screen's static canvas records (PAL). See gx_dynamic_aspect.h for the
// record layout and the anamorphic presentation model.
constexpr uint32_t kEggScreenRecord43 = 0x802A3EE8u;
constexpr uint32_t kEggScreenRecord169 = 0x802A3EF4u;
constexpr uint32_t kEggActiveScreenPtr = 0x80386F14u;

constexpr uint32_t kEggScreenProjScaleX = 0x80386F20u;
constexpr uint32_t kEggScreenProjScaleY = 0x80386F24u;

constexpr uint32_t kEggScreenAspectHandler = 0x8023E53Cu;

constexpr uint32_t kMkwUpdateAllScreens = 0x805653D0u;
constexpr uint32_t kMkwGfxDrawList = 0x809C1830u;
constexpr uint32_t kSystemManagerInstance = 0x80386000u;

// The draw list is an nw4r::ut::List: +0x00 head, +0x04 tail, +0x08 u16 count,
// +0x0A u16 link offset, with next = *(node + linkOffset + 4) (List_GetNext,
// 0x800AF180).
constexpr uint32_t kMkwGfxDrawListLinkOffset = 0x0Au;

constexpr uint32_t kMkwGfxOffscreenList = 0x809C183Cu;
constexpr uint32_t kMkwGfxOffscreenNodeScreenSlot = 0x10u;
constexpr uint32_t kEggScreenVTableOffset = 0x38u;
constexpr uint32_t kEggScreenVTable = 0x802A3F0Cu;
constexpr uint32_t kMkwScreenVTable = 0x808B4C20u;
constexpr uint32_t kEggScreenFlagsOffset = 0x34u;
constexpr uint16_t kEggScreenFlagFramebufferCanvas = 0x0008u;
constexpr uint16_t kEggScreenFlagKeepFrustumScale = 0x0040u;
constexpr uint32_t kMkwGfxNodeScreenSlots[] = {0x10u, 0x28u, 0x2Cu};

bool IsEggScreen(uint32_t address) {
    if (address == 0 || (address & 3u) != 0 || !Memory::Contains(address, 0x40u)) {
        return false;
    }
    const uint32_t vtable = Memory::Read32(address + kEggScreenVTableOffset);
    return vtable == kMkwScreenVTable || vtable == kEggScreenVTable;
}

void KeepFrustumScale(uint32_t screen) {
    if (!IsEggScreen(screen)) {
        return;
    }
    const uint16_t flags = Memory::Read16(screen + kEggScreenFlagsOffset);
    if ((flags & kEggScreenFlagKeepFrustumScale) != 0) {
        return;
    }
    Memory::Write16(screen + kEggScreenFlagsOffset,
                    static_cast<uint16_t>(flags | kEggScreenFlagKeepFrustumScale));
}

void KeepFrustumScaleOnFramebufferCanvasScreen(uint32_t screen) {
    if (!IsEggScreen(screen) ||
        (Memory::Read16(screen + kEggScreenFlagsOffset) &
         kEggScreenFlagFramebufferCanvas) == 0) {
        return;
    }
    KeepFrustumScale(screen);
}

// Walk one gfx-node list. On the on-screen list only screens that have already
// declared themselves framebuffer-canvas (bit 3) may be bypassed. On the
// offscreen list every screen is offscreen by construction, so no predicate is
// needed - and none would work anyway, because bit 3 is only set inside the
// bake itself.
void SweepGfxNodeList(uint32_t list, bool offscreenList) {
    if (!Memory::Contains(list, 0x0Cu)) {
        return;
    }
    const uint32_t linkOffset = Memory::Read16(list + kMkwGfxDrawListLinkOffset);
    // The on-screen walk also reads the menu node's parked-screen slots at
    // +0x28/+0x2C, which sit past the link words on small nodes.
    const uint32_t nodeSpan =
        std::max(linkOffset + 8u, offscreenList ? 0u : kMkwGfxNodeScreenSlots[2] + 4u);
    uint32_t node = Memory::Read32(list);
    for (int guard = 0; node != 0 && guard < 64; ++guard) {
        if (!Memory::Contains(node, nodeSpan)) {
            break;
        }
        if (offscreenList) {
            KeepFrustumScale(Memory::Read32(node + kMkwGfxOffscreenNodeScreenSlot));
        } else {
            for (const uint32_t slot : kMkwGfxNodeScreenSlots) {
                KeepFrustumScaleOnFramebufferCanvasScreen(Memory::Read32(node + slot));
            }
        }
        node = Memory::Read32(node + linkOffset + 4u);
    }
}

// Vertical expansion rides EGG::Screen's global scale, which any screen without bit 6 imports
// at projection-build time. MKW's offscreen passes only set bit 3 (fixed-size framebuffer canvas,
// record 2), so without this bypass they'd inherit the expansion and render vertically squashed.
// Re-arms every scene transition since the offscreen renderer is a per-scene singleton.
void AssertOffscreenScreenBypass() {
    SweepGfxNodeList(kMkwGfxDrawList, /*offscreenList=*/false);
    SweepGfxNodeList(kMkwGfxOffscreenList, /*offscreenList=*/true);
}

void WriteEggScreenRecord(uint32_t recordAddr, uint32_t width) {
    Memory::Write16(recordAddr, static_cast<uint16_t>(width));
    Memory::Write16(recordAddr + 0x02u,
                    static_cast<uint16_t>(MkwDynamicAspect::kEggScreenHeight));
    Memory::WriteFloat32(recordAddr + 0x04u, MkwDynamicAspect::EggHorizontalScale(width));
    Memory::WriteFloat32(recordAddr + 0x08u, 1.0f);
}

void ApplyEggScreenRecords(uint32_t surfaceWidth, uint32_t surfaceHeight) {
#if defined(MKW_GENERIC_DOL_BOOT)
    // This path is Mario Kart Wii-specific: every guest address below belongs
    // to MKW's EGG::Screen/Gfx layout.  A generic DOL product
    // must never patch those addresses or invoke the MKW aspect callbacks just
    // because the host window changed size.
    (void)surfaceWidth;
    (void)surfaceHeight;
    return;
#else
    using namespace MkwDynamicAspect;
    const uint32_t width43 =
        g_widescreenConfigured ? EggRecordWidth(kEggRecordWidth43, surfaceWidth, surfaceHeight)
                               : kEggRecordWidth43;
    const uint32_t width169 =
        g_widescreenConfigured ? EggRecordWidth(kEggRecordWidth169, surfaceWidth, surfaceHeight)
                               : kEggRecordWidth169;
    WriteEggScreenRecord(kEggScreenRecord43, width43);
    WriteEggScreenRecord(kEggScreenRecord169, width169);
    Memory::WriteFloat32(kEggScreenProjScaleX, 1.0f);
    Memory::WriteFloat32(kEggScreenProjScaleY,
                         g_widescreenConfigured
                             ? VerticalExpansion(surfaceWidth, surfaceHeight)
                             : 1.0f);
    AssertOffscreenScreenBypass();
    if (width43 == g_lastEggWidth43 && width169 == g_lastEggWidth169) {
        return;
    }
    g_lastEggWidth43 = width43;
    g_lastEggWidth169 = width169;
    CpuContext callbackCpu = GetPersistentCpuContext();
    if (Memory::Read32(kSystemManagerInstance) != 0 && Memory::Read32(kMkwGfxDrawList) != 0) {
        InvokeIndirectCpu(kMkwUpdateAllScreens, &callbackCpu);
        return;
    }
    if (Memory::Read32(kEggActiveScreenPtr) != 0) {
        InvokeIndirectCpu(kEggScreenAspectHandler, &callbackCpu);
    }
#endif
}

} // namespace


void AssertMkwOffscreenScreenBypass() {
#if defined(MKW_GENERIC_DOL_BOOT)
    return;
#else
    static int lastSweptFrame = -1;
    if (lastSweptFrame == g_gxFrameCount) {
        return;
    }
    lastSweptFrame = g_gxFrameCount;
    AssertOffscreenScreenBypass();
#endif
}

void UpdateMkwDynamicAspectSurface(uint32_t surfaceWidth, uint32_t surfaceHeight) {
#if defined(MKW_GENERIC_DOL_BOOT)
    // Generic-DOL products keep the guest's own camera/projection logic.  The
    // presentation aspect is locked in ConfigureMkwDynamicAspect(), so a host
    // resize needs no guest-memory mutation or guest callback.
    (void)surfaceWidth;
    (void)surfaceHeight;
    return;
#else
    if (!g_widescreenConfigured || surfaceWidth == 0 || surfaceHeight == 0) {
        return;
    }
    AuroraSetViewportPolicy(AURORA_VIEWPORT_STRETCH);
    ApplyEggScreenRecords(surfaceWidth, surfaceHeight);
#endif
}

void ConfigureMkwDynamicAspect(bool widescreen, uint32_t surfaceWidth, uint32_t surfaceHeight) {
#if defined(MKW_GENERIC_DOL_BOOT)
    (void)surfaceWidth;
    (void)surfaceHeight;
    g_widescreenConfigured = false;
    g_dynamicAspectRatioEnabled = false;
    AuroraSetViewportPolicy(AURORA_VIEWPORT_FIT);
    ApplyMeteorWidescreenConstants(widescreen);
    if (widescreen) {
        VILockAspectRatio(16, 9);
    } else {
        VILockAspectRatio(4, 3);
    }
    return;
#else
    g_widescreenConfigured = widescreen;
    g_dynamicAspectRatioEnabled = widescreen;
    g_lastEggWidth43 = 0;
    g_lastEggWidth169 = 0;
    if (widescreen) {
        VIUnlockAspectRatio();
        ApplyEggScreenRecords(surfaceWidth, surfaceHeight);
        return;
    }

    AuroraSetViewportPolicy(AURORA_VIEWPORT_FIT);
    VILockAspectRatio(4, 3);
    ApplyEggScreenRecords(surfaceWidth, surfaceHeight);
#endif
}
