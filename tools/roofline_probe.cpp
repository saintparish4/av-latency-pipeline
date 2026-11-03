// roofline_probe: measures this core's roofline ceilings.
//   bandwidth  STREAM triad a[i] = b[i] + s * c[i] in double, single thread,
//              at working sets sized for L1, L2, L3 and DRAM. Bytes counted
//              the STREAM way (24 per element, write-allocate ignored), and
//              the best of N trials is reported, as STREAM does.
//   peak FLOPs independent fused multiply-add chains (12 accumulators, enough
//              to cover FMA latency x ports on current x86 and Arm cores),
//              float and double, using the same Highway target as the kernels.
//   clock      a chain of dependent integer adds runs at one add per cycle, so
//              adds per second is the core's actual clock during the probe.
//              Peak FLOPs divided by it is FLOPs per cycle, which separates
//              "the FMA units are not saturated" from "the clock is low".
// Output: CSV on stdout, read by analysis/roofline.py.
#include <hwy/highway.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

#if HWY_ARCH_X86 && defined(__AVX2__)
static_assert(HWY_STATIC_TARGET <= HWY_AVX2, "Highway fell back below AVX2 (see src/simd_kernels.cpp)");
#endif

namespace hn = hwy::HWY_NAMESPACE;
using Clock = std::chrono::steady_clock;

namespace {

double seconds_since(Clock::time_point t0) {
  return std::chrono::duration<double>(Clock::now() - t0).count();
}

double triad_gbps(std::size_t bytes_total) {
  const std::size_t n = bytes_total / (3 * sizeof(double));
  std::vector<double> a(n, 1.0), b(n, 2.0), c(n, 0.5);
  const double s = 3.0;
  // Enough repetitions that each trial moves ~1 GB, so timer resolution is irrelevant.
  const std::size_t reps = std::max<std::size_t>(1, (1u << 30) / (24 * n));
  double best = 1e30;
  for (int trial = 0; trial < 7; ++trial) {
    const auto t0 = Clock::now();
    for (std::size_t r = 0; r < reps; ++r) {
      for (std::size_t i = 0; i < n; ++i) a[i] = b[i] + s * c[i];
      asm volatile("" : : "r"(a.data()) : "memory");  // keep every pass
    }
    best = std::min(best, seconds_since(t0));
  }
  return 24.0 * static_cast<double>(n) * static_cast<double>(reps) / best / 1e9;
}

template <class T>
double peak_gflops() {
  const hn::ScalableTag<T> d;
  constexpr int kAcc = 12;
  hn::Vec<decltype(d)> acc[kAcc];
  for (int k = 0; k < kAcc; ++k) acc[k] = hn::Set(d, static_cast<T>(k));
  const auto m = hn::Set(d, static_cast<T>(0.999999)), add = hn::Set(d, static_cast<T>(1e-7));
  const std::size_t iters = 50'000'000;
  double best = 1e30;
  for (int trial = 0; trial < 5; ++trial) {
    const auto t0 = Clock::now();
    for (std::size_t i = 0; i < iters; ++i) {
      for (auto& a : acc) a = hn::MulAdd(a, m, add);
    }
    best = std::min(best, seconds_since(t0));
  }
  T sink = 0;
  for (const auto& a : acc) sink += hn::GetLane(a);
  asm volatile("" : : "r"(&sink) : "memory");
  const double flops = 2.0 * static_cast<double>(hn::Lanes(d)) * kAcc * static_cast<double>(iters);
  return flops / best / 1e9;
}

double clock_ghz() {
  constexpr std::uint64_t kIters = 200'000'000;
  double best = 1e30;
  for (int trial = 0; trial < 5; ++trial) {
    std::uint64_t x = 0;
    const auto t0 = Clock::now();
    for (std::uint64_t i = 0; i < kIters; ++i) {
#if defined(__x86_64__)
      asm volatile("add $1, %0\n\tadd $1, %0\n\tadd $1, %0\n\tadd $1, %0\n\tadd $1, %0" : "+r"(x));
#elif defined(__aarch64__)
      asm volatile("add %0, %0, #1\n\tadd %0, %0, #1\n\tadd %0, %0, #1\n\tadd %0, %0, #1\n\tadd %0, %0, #1"
                   : "+r"(x));
#endif
    }
    best = std::min(best, seconds_since(t0));
  }
  return 5.0 * static_cast<double>(kIters) / best / 1e9;  // five dependent adds per iteration
}

}  // namespace

int main() {
  std::printf("kind,name,bytes,value\n");
  const struct {
    const char* name;
    std::size_t bytes;
  } levels[] = {{"L1", 24u << 10}, {"L2", 384u << 10}, {"L3", 12u << 20}, {"DRAM", 1536u << 20}};
  for (const auto& l : levels) {
    std::printf("bandwidth_gbps,%s,%zu,%.2f\n", l.name, l.bytes, triad_gbps(l.bytes));
    std::fflush(stdout);
  }
  const double ghz = clock_ghz();
  const double pf = peak_gflops<float>(), pd = peak_gflops<double>();
  std::printf("clock_ghz,core,0,%.3f\n", ghz);
  std::printf("peak_gflops,float,0,%.2f\n", pf);
  std::printf("peak_gflops,double,0,%.2f\n", pd);
  std::printf("flops_per_cycle,float,0,%.1f\n", pf / ghz);
  std::printf("flops_per_cycle,double,0,%.1f\n", pd / ghz);
  return 0;
}
