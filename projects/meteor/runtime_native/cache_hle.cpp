#include "abi_bridge.h"
#include "ppc_runtime.h"
#include "memory.h"
#include "data_cache_writeback.h"

extern "C" void GxNotifyGuestRamDmaWrite(uint32_t address, uint32_t size);

namespace {

void PublishSdkCacheRange(CpuContext* ctx, bool sync, uint32_t loopPc) {
    const uint32_t address = ctx->gpr[3];
    const uint32_t length = ctx->gpr[4];
    SetCR(ctx, 0, length, uint32_t{0});
    if (length == 0) return;

    const uint32_t lines = DataCacheWriteback::RangeLineCount(address, length);
    ctx->gpr[5] = address & 31u;
    ctx->gpr[4] = lines;
    ctx->ctr = lines;
    const bool published = DataCacheWriteback::PublishRange(address, length,
        [](uint32_t start, uint32_t size) { return Memory::Contains(start, size); },
        [](uint32_t start, uint32_t size) { GxNotifyGuestRamDmaWrite(start, size); });
    if (published) {
        ctx->gpr[3] = address + lines * 32u;
        ctx->ctr = 0;
    } else {
        // Keep original publication and progress semantics for unusual ranges
        // crossing a memory hole/wrap. Valid SDK buffers take the batched path.
        do {
            PPC_Dcbst(ctx->gpr[3]);
            ctx->gpr[3] += 32u;
            --ctx->ctr;
            if (ctx->ctr != 0) ApplyRuntimeLoopOptions(loopPc, ctx);
        } while (ctx->ctr != 0);
    }
    if (sync) OSSystemCall();
}

extern "C" void Meteor_DCFlushRange(CpuContext* ctx) { PublishSdkCacheRange(ctx, true, 0x80209544u); }
extern "C" void Meteor_DCStoreRange(CpuContext* ctx) { PublishSdkCacheRange(ctx, true, 0x80209574u); }
extern "C" void Meteor_DCFlushRangeNoSync(CpuContext* ctx) { PublishSdkCacheRange(ctx, false, 0x802095A4u); }
extern "C" void Meteor_DCStoreRangeNoSync(CpuContext* ctx) { PublishSdkCacheRange(ctx, false, 0x802095D0u); }

} // namespace

// RDSPAF retail disassembly: independent leaves at 80209528/58/88/B4,
// each preceded by blr. They round r3/r4 to cache lines, loop over dcbf or
// dcbst, and optionally finish with sc. Publish the same bytes in one cache
// invalidation, retaining r3/r4/r5/CTR/CR0 and the syscall boundary. No guest
// bytes or graphics work are discarded. DCInvalidate/DCZero remain translated.
REGISTER_TITLE_NATIVE_FUNCTION(0x80209528, Meteor_DCFlushRange);
REGISTER_TITLE_NATIVE_FUNCTION(0x80209558, Meteor_DCStoreRange);
REGISTER_TITLE_NATIVE_FUNCTION(0x80209588, Meteor_DCFlushRangeNoSync);
REGISTER_TITLE_NATIVE_FUNCTION(0x802095B4, Meteor_DCStoreRangeNoSync);
