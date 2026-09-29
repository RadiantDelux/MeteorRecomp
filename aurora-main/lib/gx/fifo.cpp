#include "fifo.hpp"
#include "command_processor.hpp"
#include "../internal.hpp"

#include <chrono>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "tracy/Tracy.hpp"

namespace aurora::gx::fifo {
static Module Log("aurora::gx::fifo");

namespace detail {
uint8_t* sBufferData = nullptr;
uint32_t sBufferSize = 0;
uint32_t sBufferCapacity = 0;
bool sInDisplayList = false;
uint8_t* sDlBuffer = nullptr;
uint32_t sDlSize = 0;
uint32_t sDlWritePos = 0;
} // namespace detail

void init() {
  constexpr uint32_t initialCapacity = 64 * 1024;
  reset_cp_register_cache();
  free(detail::sBufferData);
  detail::sBufferData = static_cast<uint8_t*>(malloc(initialCapacity));
  detail::sBufferSize = 0;
  detail::sBufferCapacity = initialCapacity;
  detail::sInDisplayList = false;
  detail::sDlBuffer = nullptr;
  detail::sDlSize = 0;
  detail::sDlWritePos = 0;
}

void write_data_grow(const void* data, uint32_t length) {
  uint32_t needed = detail::sBufferSize + length;
  uint32_t newCap = std::max(detail::sBufferCapacity * 2, needed);
  detail::sBufferData = static_cast<uint8_t*>(realloc(detail::sBufferData, newCap));
  std::memcpy(detail::sBufferData + detail::sBufferSize, data, length);
  detail::sBufferSize = needed;
  detail::sBufferCapacity = newCap;
}

void begin_display_list(uint8_t* buf, uint32_t size) {
  detail::sInDisplayList = true;
  detail::sDlBuffer = buf;
  detail::sDlSize = size;
  detail::sDlWritePos = 0;
}

uint32_t end_display_list() {
  detail::sInDisplayList = false;
  uint32_t bytesWritten = detail::sDlWritePos;
  uint32_t padded = (bytesWritten + 31) & ~31u;
  while (detail::sDlWritePos < padded && detail::sDlWritePos < detail::sDlSize) {
    detail::sDlBuffer[detail::sDlWritePos++] = 0;
  }
  detail::sDlBuffer = nullptr;
  detail::sDlSize = 0;
  detail::sDlWritePos = 0;
  return padded;
}

bool in_display_list() { return detail::sInDisplayList; }

// How much of the producer's frame is spent blocked before it may decode the next batch of GX commands.
static void note_drain_wait(uint64_t nanos) noexcept {
  ZoneScopedN("FIFO drain wait");
  TracyPlot("aurora: fifoDrainWaitUs", static_cast<int64_t>(nanos / 1000));
}

uint64_t wait_until_ready() {
  // SEALED, not DONE.
  const auto waited = aurora::wait_for_frame_worker_sealed();
  if (waited.count() > 0) UNLIKELY {
    note_drain_wait(static_cast<uint64_t>(waited.count()));
  }
  return static_cast<uint64_t>(waited.count());
}

void drain() {
  const uint64_t waitNs = wait_until_ready();
  static const bool traceTiming = std::getenv("METEOR_TRACE_FRAME_TIMING") != nullptr;
  const uint32_t bufferBytes = detail::sBufferSize;
  ProcessTiming processTiming{};
  if (detail::sBufferSize != 0) {
    process(detail::sBufferData, detail::sBufferSize, true, traceTiming ? &processTiming : nullptr);
    detail::sBufferSize = 0;
  }
  if (traceTiming) {
    using Clock = std::chrono::steady_clock;
    struct Window {
      Clock::time_point first{};
      uint64_t calls = 0;
      uint64_t nonEmpty = 0;
      uint64_t bytes = 0;
      uint64_t waitNs = 0;
      uint64_t maxWaitNs = 0;
      uint64_t rendererLockWaitNs = 0;
      uint64_t maxRendererLockWaitNs = 0;
      uint64_t decodeNs = 0;
      uint64_t maxDecodeNs = 0;
      uint64_t maxProcessNs = 0;
      uint32_t clockCheckCountdown = 0;
    };
    thread_local Window w;
    ++w.calls;
    if (bufferBytes != 0) ++w.nonEmpty;
    w.bytes += bufferBytes;
    w.waitNs += waitNs;
    w.maxWaitNs = std::max(w.maxWaitNs, waitNs);
    w.rendererLockWaitNs += processTiming.rendererLockWaitNs;
    w.maxRendererLockWaitNs = std::max(w.maxRendererLockWaitNs, processTiming.rendererLockWaitNs);
    w.decodeNs += processTiming.decodeNs;
    w.maxDecodeNs = std::max(w.maxDecodeNs, processTiming.decodeNs);
    w.maxProcessNs = std::max(w.maxProcessNs, processTiming.rendererLockWaitNs + processTiming.decodeNs);
    if (w.first == Clock::time_point{}) {
      w.first = Clock::now();
    }
    // A trace run can see hundreds of thousands of synchronization points per
    // second. Sampling the wall clock every 1024 drains keeps the diagnostic
    // from becoming the producer load it is meant to measure; non-empty drains
    // are still timed individually above.
    if (++w.clockCheckCountdown >= 1024) {
      w.clockCheckCountdown = 0;
      const auto now = Clock::now();
      if (now - w.first >= std::chrono::seconds(1)) {
        const uint64_t processNs = w.rendererLockWaitNs + w.decodeNs;
        Log.info("FIFO drain timing: calls={} nonEmpty={} bytes={} waitAvg={:.3f}ms waitMax={:.3f}ms "
                 "processAvg={:.3f}ms processMax={:.3f}ms lockAvg={:.3f}ms lockMax={:.3f}ms "
                 "decodeAvg={:.3f}ms decodeMax={:.3f}ms",
                 w.calls, w.nonEmpty, w.bytes, double(w.waitNs) / double(w.calls) / 1e6,
                 double(w.maxWaitNs) / 1e6,
                 w.nonEmpty ? double(processNs) / double(w.nonEmpty) / 1e6 : 0.0,
                 double(w.maxProcessNs) / 1e6,
                 w.nonEmpty ? double(w.rendererLockWaitNs) / double(w.nonEmpty) / 1e6 : 0.0,
                 double(w.maxRendererLockWaitNs) / 1e6,
                 w.nonEmpty ? double(w.decodeNs) / double(w.nonEmpty) / 1e6 : 0.0,
                 double(w.maxDecodeNs) / 1e6);
        w = Window{.first = now};
      }
    }
  }
}

const uint8_t* get_buffer_data() { return detail::sBufferData; }
uint32_t get_buffer_size() { return detail::sBufferSize; }
void clear_buffer() {
  detail::sBufferSize = 0;
}

} // namespace aurora::gx::fifo
