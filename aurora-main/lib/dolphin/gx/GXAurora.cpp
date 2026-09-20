#include "dolphin/gx/GXAurora.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "__gx.h"
#include "gx.hpp"
#include "../../window.hpp"

#include "../../gfx/common.hpp"
#include "../../gfx/texture.hpp"
#include "../../gx/fifo.hpp"
#include "../../webgpu/gpu.hpp"

// Single definition for the `Log` that gx.hpp declares for this directory.
aurora::Module Log("aurora::gx");

static void GXWriteString(const char* label) {
  auto length = strlen(label);

  if (length > std::numeric_limits<u16>::max()) {
    Log.warn("Debug marker size over u16 max, truncating");
    length = std::numeric_limits<u16>::max();
  }

  GX_WRITE_U16(length);
  GX_WRITE_DATA(label, length);
}

void GXPushDebugGroup(const char* label) {
  GX_WRITE_AURORA(GX_LOAD_AURORA_DEBUG_GROUP_PUSH);
  GXWriteString(label);
}

void GXPopDebugGroup() { GX_WRITE_AURORA(GX_LOAD_AURORA_DEBUG_GROUP_POP); }

void GXInsertDebugMarker(const char* label) {
  GX_WRITE_AURORA(GX_LOAD_AURORA_DEBUG_MARKER_INSERT);
  GXWriteString(label);
}

void AuroraSetViewportPolicy(AuroraViewportPolicy policy) {
  const bool changed = g_gxState.viewportPolicy != policy;
  if (changed) {
    // Finish commands using the old framebuffer mapping before changing it.
    aurora::gx::fifo::drain();
  }
  g_gxState.viewportPolicy = policy;
  aurora::window::set_frame_buffer_aspect_fit(policy == AURORA_VIEWPORT_FIT);
  aurora::window::set_present_surface_fill(policy == AURORA_VIEWPORT_STRETCH);
  if (changed) {
    // Reapply the guest viewport and scissor after a resize.
    aurora::gx::set_logical_viewport(g_gxState.logicalViewport);
    aurora::gx::set_logical_scissor(g_gxState.logicalScissor);
  }
}

void AuroraGetRenderSize(u32* width, u32* height) {
  // The guest CPU thread uses this safe render-size value outside render passes.
  const auto renderSize = aurora::gfx::get_frame_buffer_size();
  if (width != nullptr) {
    *width = renderSize.x;
  }
  if (height != nullptr) {
    *height = renderSize.y;
  }
}

void AuroraGetSurfaceSize(u32* width, u32* height) {
  const auto windowSize = aurora::window::get_window_size();
  if (width != nullptr) {
    *width = windowSize.native_fb_width;
  }
  if (height != nullptr) {
    *height = windowSize.native_fb_height;
  }
}

void GXSetViewportRender(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz) {
  GX_WRITE_AURORA(GX_LOAD_AURORA_VIEWPORT_RENDER);
  GX_WRITE_F32(left);
  GX_WRITE_F32(top);
  GX_WRITE_F32(wd);
  GX_WRITE_F32(ht);
  GX_WRITE_F32(nearz);
  GX_WRITE_F32(farz);
}

void GXSetScissorRender(u32 left, u32 top, u32 wd, u32 ht) {
  GX_WRITE_AURORA(GX_LOAD_AURORA_SCISSOR_RENDER);
  GX_WRITE_U32(left);
  GX_WRITE_U32(top);
  GX_WRITE_U32(wd);
  GX_WRITE_U32(ht);
}

namespace {
void WriteMappedRenderState(const aurora::gx::MappedRenderState& mapped) {
  GXSetViewportRender(mapped.viewport.left, mapped.viewport.top, mapped.viewport.width, mapped.viewport.height,
                      mapped.viewport.znear, mapped.viewport.zfar);
  GXSetScissorRender(static_cast<u32>(std::max(mapped.scissor.x, 0)),
                     static_cast<u32>(std::max(mapped.scissor.y, 0)),
                     static_cast<u32>(std::max(mapped.scissor.width, 0)),
                     static_cast<u32>(std::max(mapped.scissor.height, 0)));
}
} // namespace

void GXSetViewportScissorRenderSafeArea(f32 aspect) {
  const auto [targetWidth, targetHeight] = aurora::gfx::get_render_target_size();
  if (targetWidth == 0 || targetHeight == 0 || !std::isfinite(aspect) || aspect <= 0.0f) {
    return;
  }

  // Apply queued viewport changes before direct layout draws use the safe area.
  aurora::gx::fifo::drain();
  auto mapped = aurora::gx::map_logical_render_state();
  const float targetAspect = static_cast<float>(targetWidth) / static_cast<float>(targetHeight);

  float safeLeft = 0.0f;
  float safeTop = 0.0f;
  float safeWidth = static_cast<float>(targetWidth);
  float safeHeight = static_cast<float>(targetHeight);
  if (targetAspect > aspect) {
    safeWidth = safeHeight * aspect;
    safeLeft = (static_cast<float>(targetWidth) - safeWidth) * 0.5f;
  } else if (targetAspect < aspect) {
    safeHeight = safeWidth / aspect;
    safeTop = (static_cast<float>(targetHeight) - safeHeight) * 0.5f;
  }

  const float scaleX = safeWidth / static_cast<float>(targetWidth);
  const float scaleY = safeHeight / static_cast<float>(targetHeight);
  mapped.viewport.left = safeLeft + mapped.viewport.left * scaleX;
  mapped.viewport.top = safeTop + mapped.viewport.top * scaleY;
  mapped.viewport.width *= scaleX;
  mapped.viewport.height *= scaleY;

  const float scissorLeft = safeLeft + static_cast<float>(mapped.scissor.x) * scaleX;
  const float scissorTop = safeTop + static_cast<float>(mapped.scissor.y) * scaleY;
  const float scissorRight =
      safeLeft + static_cast<float>(mapped.scissor.x + mapped.scissor.width) * scaleX;
  const float scissorBottom =
      safeTop + static_cast<float>(mapped.scissor.y + mapped.scissor.height) * scaleY;
  const int32_t left = std::clamp(static_cast<int32_t>(std::floor(scissorLeft)), 0,
                                  static_cast<int32_t>(targetWidth));
  const int32_t top = std::clamp(static_cast<int32_t>(std::floor(scissorTop)), 0,
                                 static_cast<int32_t>(targetHeight));
  const int32_t right = std::clamp(static_cast<int32_t>(std::ceil(scissorRight)), left,
                                   static_cast<int32_t>(targetWidth));
  const int32_t bottom = std::clamp(static_cast<int32_t>(std::ceil(scissorBottom)), top,
                                    static_cast<int32_t>(targetHeight));
  mapped.scissor = {left, top, right - left, bottom - top};

  WriteMappedRenderState(mapped);
}

void GXRestoreViewportScissorRender() {
  // Run queued GX draws before leaving the direct layout safe area.
  aurora::gx::fifo::drain();
  WriteMappedRenderState(aurora::gx::map_logical_render_state());
}

void GXSetTexCopySrcRender(u16 left, u16 top, u16 wd, u16 ht) {
  aurora::gx::g_gxState.texCopySrc = {left, top, wd, ht};
  aurora::gx::g_gxState.texCopySrcRenderSpace = true;
}

void GXCreateFrameBuffer(u32 width, u32 height) {
  aurora::gx::fifo::drain();
  aurora::gfx::begin_offscreen(width, height);
}

void GXRestoreFrameBuffer() {
  aurora::gx::fifo::drain();
  aurora::gfx::end_offscreen();
}

void AuroraSetRamXfbPresentSource(const void* data, u32 width, u32 height, u32 stridePixels) {
#if defined(AURORA_GX_FIFO_TEST)
  (void)data;
  (void)width;
  (void)height;
  (void)stridePixels;
  return;
#else
  if (data == nullptr || width == 0 || height == 0 || stridePixels < width || (stridePixels & 1u) != 0) {
    return;
  }

  // Wii XFB memory is packed Y1/Cb/Y2/Cr (4 bytes for two pixels). Convert the
  // BT.601 studio-range signal once on the CPU; boot/logo XFBs change rarely,
  // and this avoids teaching the normal GX EFB-copy pipeline about guest RAM.
  const auto* src = static_cast<const u8*>(data);
  std::vector<u8> rgba(static_cast<size_t>(width) * height * 4u);
  const auto clamp8 = [](int value) -> u8 {
    return static_cast<u8>(std::clamp(value, 0, 255));
  };
  const auto writePixel = [&](size_t dst, int y, int cb, int cr) {
    const int c = std::max(y - 16, 0);
    const int d = cb - 128;
    const int e = cr - 128;
    rgba[dst + 0] = clamp8((298 * c + 409 * e + 128) >> 8);
    rgba[dst + 1] = clamp8((298 * c - 100 * d - 208 * e + 128) >> 8);
    rgba[dst + 2] = clamp8((298 * c + 516 * d + 128) >> 8);
    rgba[dst + 3] = 255;
  };

  for (u32 y = 0; y < height; ++y) {
    const u8* row = src + static_cast<size_t>(y) * stridePixels * 2u;
    for (u32 x = 0; x < width; x += 2) {
      const int y0 = row[x * 2u + 0u];
      const int cb = row[x * 2u + 1u];
      const int y1 = row[x * 2u + 2u];
      const int cr = row[x * 2u + 3u];
      const size_t dst0 = (static_cast<size_t>(y) * width + x) * 4u;
      writePixel(dst0, y0, cb, cr);
      if (x + 1u < width) {
        writePixel(dst0 + 4u, y1, cb, cr);
      }
    }
  }

  static aurora::gfx::TextureHandle s_ramXfbTexture;
  static wgpu::BindGroup s_ramXfbBindGroup;
  static u32 s_ramXfbWidth = 0;
  static u32 s_ramXfbHeight = 0;
  if (!s_ramXfbTexture || s_ramXfbWidth != width || s_ramXfbHeight != height) {
    s_ramXfbTexture = aurora::gfx::new_dynamic_texture_2d(width, height, 1, GX_TF_RGBA8_PC, "RAM XFB");
    s_ramXfbWidth = width;
    s_ramXfbHeight = height;
    s_ramXfbBindGroup = {};
  }
  if (!s_ramXfbTexture) {
    return;
  }

  aurora::gfx::write_texture(*s_ramXfbTexture, rgba);
  if (!s_ramXfbBindGroup) {
    s_ramXfbBindGroup = aurora::webgpu::create_copy_bind_group(
        s_ramXfbTexture->sampleTextureView, aurora::webgpu::present_source().sampler);
  }
  aurora::webgpu::set_present_source_override(s_ramXfbBindGroup, s_ramXfbTexture->texture,
                                               s_ramXfbTexture->size, s_ramXfbTexture->format);
#endif
}
