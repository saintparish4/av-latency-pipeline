// SpinPool: persistent helper threads that busy-wait for work, for fork-join
// parallelism on a latency path.
//
// Why spin instead of std::condition_variable / std::barrier: a futex wake
// costs microseconds to tens of microseconds and is itself a jitter source,
// against a parallel region that is tens of microseconds long. Spinning
// trades whole cores (which an isolcpus layout reserves anyway) for a
// hand-off of a few hundred nanoseconds. This is a hypothesis the ladder
// tests, not a law; see directive/p3.md.
//
// Contract: run() is called from one thread only, and returns after every
// worker (the caller is worker 0) has finished job(worker). Work is handed
// over through a release store of the generation counter and completion
// through an acquire load of remaining_, so the job's writes are visible to
// the caller on return (and TSan can see why).
#pragma once

#include <pthread.h>
#include <sched.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

namespace avl {

inline void cpu_relax() {
#if defined(__x86_64__)
  __builtin_ia32_pause();
#elif defined(__aarch64__)
  asm volatile("yield");
#endif
}

class SpinPool {
 public:
  SpinPool(int helpers, int first_cpu) : helpers_(helpers) {
    for (int i = 0; i < helpers; ++i) {
      threads_.emplace_back([this, i] { loop(i + 1); });
      if (first_cpu >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(first_cpu + i, &set);
        if (pthread_setaffinity_np(threads_.back().native_handle(), sizeof set, &set) != 0) {
          std::fprintf(stderr, "SpinPool: cannot pin helper %d to CPU %d\n", i, first_cpu + i);
        }
      }
    }
  }
  SpinPool(const SpinPool&) = delete;
  SpinPool& operator=(const SpinPool&) = delete;
  ~SpinPool() {
    stop_.store(true, std::memory_order_relaxed);
    for (auto& t : threads_) t.join();
  }

  [[nodiscard]] int workers() const { return helpers_ + 1; }

  template <class F>
  void run(F& job) {
    fn_ = [](void* ctx, int worker) { (*static_cast<F*>(ctx))(worker); };
    ctx_ = &job;
    remaining_.store(helpers_, std::memory_order_relaxed);
    generation_.fetch_add(1, std::memory_order_release);
    job(0);
    while (remaining_.load(std::memory_order_acquire) != 0) cpu_relax();
  }

 private:
  void loop(int worker) {
    std::uint64_t seen = 0;
    for (;;) {
      std::uint64_t g;
      while ((g = generation_.load(std::memory_order_acquire)) == seen) {
        if (stop_.load(std::memory_order_relaxed)) return;
        cpu_relax();
      }
      seen = g;
      fn_(ctx_, worker);
      remaining_.fetch_sub(1, std::memory_order_release);
    }
  }

  int helpers_;
  std::vector<std::thread> threads_;
  void (*fn_)(void*, int) = nullptr;
  void* ctx_ = nullptr;
  // Separate cache lines: helpers hammer generation_, the caller remaining_.
  alignas(64) std::atomic<std::uint64_t> generation_{0};
  alignas(64) std::atomic<int> remaining_{0};
  alignas(64) std::atomic<bool> stop_{false};
};

}  // namespace avl
