#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace aurora::gx {

// One dispatch at a time. Every helper acknowledges the generation, including
// helpers that wake after the caller has already consumed the last chunk.
class InterpolationWorkerPool {
public:
  static InterpolationWorkerPool& instance() {
    static InterpolationWorkerPool pool;
    return pool;
  }

  explicit InterpolationWorkerPool(unsigned helpers = default_helpers()) {
    for (unsigned i = 0; i < helpers; ++i) {
      m_workers.emplace_back([this] { worker_loop(); });
    }
  }

  ~InterpolationWorkerPool() {
    {
      std::lock_guard lock(m_mutex);
      m_stop = true;
    }
    m_wake.notify_all();
    for (auto& worker : m_workers) worker.join();
  }

  template <typename Fn>
  void run(size_t count, const Fn& fn) {
    // Dispatch/wakeup costs dominate small batches. Leave those on the caller.
    if (m_workers.empty() || count < 256) {
      for (size_t i = 0; i < count; ++i) fn(i);
      return;
    }
    {
      std::lock_guard lock(m_mutex);
      m_invoke = [&fn](size_t index) { fn(index); };
      m_count = count;
      m_next.store(0, std::memory_order_relaxed);
      m_outstanding = m_workers.size();
      ++m_generation;
    }
    m_wake.notify_all();
    consume();
    std::unique_lock lock(m_mutex);
    m_done.wait(lock, [this] { return m_outstanding == 0; });
    // No helper can still read the borrowed callable or this frame's uniforms.
    m_invoke = nullptr;
  }

private:
  static unsigned default_helpers() {
    const unsigned hardware = std::thread::hardware_concurrency();
    // Guest, render, audio and driver threads need CPU time as well.
    return hardware > 4 ? std::min(hardware - 4, 3u) : 0u;
  }
  void consume() {
    constexpr size_t chunk = 16;
    for (;;) {
      const size_t begin = m_next.fetch_add(chunk, std::memory_order_relaxed);
      if (begin >= m_count) return;
      for (size_t i = begin; i < std::min(begin + chunk, m_count); ++i) m_invoke(i);
    }
  }
  void worker_loop() {
    uint64_t generation = 0;
    for (;;) {
      {
        std::unique_lock lock(m_mutex);
        m_wake.wait(lock, [&] { return m_stop || generation != m_generation; });
        if (m_stop) return;
        generation = m_generation;
      }
      consume();
      {
        std::lock_guard lock(m_mutex);
        if (--m_outstanding == 0) m_done.notify_one();
      }
    }
  }
  std::vector<std::thread> m_workers;
  std::mutex m_mutex;
  std::condition_variable m_wake, m_done;
  std::function<void(size_t)> m_invoke;
  std::atomic_size_t m_next{0};
  size_t m_count = 0, m_outstanding = 0;
  uint64_t m_generation = 0;
  bool m_stop = false;
};

} // namespace aurora::gx
