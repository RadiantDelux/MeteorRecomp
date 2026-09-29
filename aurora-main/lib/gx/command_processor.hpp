#pragma once

#include "dolphin/gx/GXEnum.h"
#include "../internal.hpp"

namespace aurora::gx::fifo {

// Reset the CP write-deduplication state whenever the GX shadow state is reinitialized.
void reset_cp_register_cache();
void set_max_draws_per_batch(uint32_t limit);

struct ProcessTiming {
  uint64_t rendererLockWaitNs = 0;
  uint64_t decodeNs = 0;
};

// Process a buffer of GX FIFO commands
void process(const uint8_t* data, uint32_t size, bool bigEndian, ProcessTiming* timing = nullptr);

// Submit already-packed direct vertex bytes against the current GX state.
bool submit_raw_draw(GXPrimitive prim, GXVtxFmt fmt, const uint8_t* vertices, uint16_t vtxCount,
                     uint32_t vertexBytes);

} // namespace aurora::gx::fifo
