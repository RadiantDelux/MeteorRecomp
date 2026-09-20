#ifndef DOLPHIN_GXAURORA_H
#define DOLPHIN_GXAURORA_H

#include <dolphin/types.h>

#if __cplusplus
extern "C" {
#endif

//
// Subcommands for GX_LOAD_AURORA.
//

/**
 * Sets the actual render viewport in native framebuffer coordinates.
 * Must be followed by six f32 values: left, top, width, height, nearz, farz.
 */
#define GX_LOAD_AURORA_VIEWPORT_RENDER 0x0001

/**
 * Sets the actual render scissor in native framebuffer coordinates.
 * Must be followed by four u32 values: left, top, width, height.
 */
#define GX_LOAD_AURORA_SCISSOR_RENDER 0x0002

/**
 * Aurora equivalent of CP_REG_ARRAYBASE_ID: sets the base address and size of a vertex array.
 * This command must be followed by a 64-bit memory address, 32-bit size, and 1-byte little-endian flag.
 * The index of the vertex array is given by the lowest 4 bits of the command ID,
 * e.g. writing GX_LOAD_AURORA_ARRAYBASE + 5 will set the vertex array for the sixth vertex attribute.
 * To set strides, use the normal CP_REG_ARRAYSTRIDE_ID register.
 */
#define GX_LOAD_AURORA_ARRAYBASE 0x0010

/**
 * Pushes a debug group to the backend graphics API. These may show in debugging tools such as RenderDoc.
 * Must be followed by a u16 string length and that many UTF-8 characters (no null terminator required).
 * It is considered an error to have unpopped debug groups at the end of the frame. They will be automatically cleared.
 */
#define GX_LOAD_AURORA_DEBUG_GROUP_PUSH 0x0020

/**
 * Pops a previously pushed debug group.
 * Followed by nothing.
 */
#define GX_LOAD_AURORA_DEBUG_GROUP_POP 0x0021

/**
 * Sends a debug marker to the backend graphics API.
 * Must be followed by a u16 string length and that many UTF-8 characters (no null terminator required).
 */
#define GX_LOAD_AURORA_DEBUG_MARKER_INSERT 0x0022

#define GX_LOAD_AURORA_TEXOBJ 0x0030

#define GX_LOAD_AURORA_TLUT 0x0031

#define GX_LOAD_AURORA_DESTROY_TEXOBJ 0x0032

#define GX_LOAD_AURORA_DESTROY_TLUT 0x0033

#define GX_LOAD_AURORA_DESTROY_COPY_TEX 0x0034

#define GX_LOAD_AURORA_INVALIDATE_TEX_ALL 0x0035


/*
 * Debug marker stuff
 */

/**
 * Pushes a debug group to the backend graphics API. These may show in debugging tools such as RenderDoc.
 * It is considered an error to have unpopped debug groups at the end of the frame. They will be automatically cleared.
 */
void GXPushDebugGroup(const char* label);

/**
 * Pop a debug group previously pushed via GXPushDebugGroup().
 */
void GXPopDebugGroup();

/**
 * Sends a debug marker to the backend graphics API. These may show in debugging tools such as RenderDoc.
 */
void GXInsertDebugMarker(const char* label);

typedef enum _AuroraViewportPolicy {
  AURORA_VIEWPORT_FIT = 0,     // Preserve logical aspect in the content framebuffer
  AURORA_VIEWPORT_STRETCH = 1, // Match content framebuffer aspect to the native surface
  AURORA_VIEWPORT_NATIVE = 2,  // Use active framebuffer pixels directly
} AuroraViewportPolicy;

/**
 * Configures content framebuffer sizing and how GXSetViewport/GXSetScissor parameters are applied to rendering.
 * When AURORA_VIEWPORT_NATIVE is used, GXSetTexCopySrc/GXSetTexCopyDst will use native framebuffer resolution.
 */
void AuroraSetViewportPolicy(AuroraViewportPolicy policy);

/**
 * Retrieves the current content framebuffer size.
 */
void AuroraGetRenderSize(u32* width, u32* height);

/** Retrieves the current native presentation-surface size, ignoring the GX sizing policy. */
void AuroraGetSurfaceSize(u32* width, u32* height);

/**
 * Sets the actual render viewport in native framebuffer coordinates.
 * Overrides the automatically scaled values set by the logical GXSetViewport.
 */
void GXSetViewportRender(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz);

/**
 * Sets the actual render scissor in native framebuffer coordinates.
 * Overrides the automatically scaled values set by the logical GXSetScissor.
 */
void GXSetScissorRender(u32 left, u32 top, u32 wd, u32 ht);

/** Maps the logical GX viewport and scissor into a centered safe area, for fixed-aspect UI bridges. */
void GXSetViewportScissorRenderSafeArea(f32 aspect);

/** Restores the viewport and scissor from logical GX state after GXSetViewportScissorRenderSafeArea. */
void GXRestoreViewportScissorRender(void);

/** Sets the texture-copy source in active render-target coordinates, for bridges that already
 *  resolved VI/window scaling. */
void GXSetTexCopySrcRender(u16 left, u16 top, u16 wd, u16 ht);

/**
 * Create an offscreen framebuffer and switch rendering to it.
 * All subsequent GX rendering will target this framebuffer until GXRestoreFrameBuffer() is called.
 * Use GXCopyTex to resolve the offscreen content into a texture.
 */
void GXCreateFrameBuffer(u32 width, u32 height);

/**
 * Restore rendering to the main EFB framebuffer.
 * Must be called after GXCreateFrameBuffer() to resume normal rendering.
 */
void GXRestoreFrameBuffer(void);

void GXApplyBPReg(u8 reg, u32 value);

/**
 * Supplies the host pointer implied by a texture image3 BP register write.
 *
 * Real GX permits software to program TX_SETIMAGE0/TX_SETIMAGE3 directly,
 * without going through GXLoadTexObj. The BP stream carries the physical
 * texture address but not a host pointer, so native runtimes that decode guest
 * BP writes must provide the translated address separately.
 */
void GXSetDirectTextureData(u8 texMapId, const void* data);

/**
 * Supplies the RAM bytes transferred by a raw BP LOADTLUT0/LOADTLUT1 pair.
 * `tmemOffset` is the 10-bit hardware TMEM field in 512-byte units.  Aurora
 * snapshots the bytes because real GX copies RAM into TMEM at load time.
 */
void GXSetDirectTlutData(u16 tmemOffset, const void* data, u32 byteSize);

/**
 * Uploads a Wii VI XFB stored directly in guest RAM (Y1/Cb/Y2/Cr, 16 bpp)
 * and makes it the presentation source for the current Aurora frame.
 *
 * This is intentionally separate from GXCopyDisp: some retail boot/logo paths
 * write an XFB in RAM and hand it straight to VI without performing an EFB copy.
 */
void AuroraSetRamXfbPresentSource(const void* data, u32 width, u32 height, u32 stridePixels);

#if __cplusplus
}
#endif

#endif
