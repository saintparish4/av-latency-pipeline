// kernel_bench: times each SIMD kernel on the real sector data and reports
// achieved GFLOP/s and GB/s for the roofline.
//
// FLOPs count only floating add/sub/mul (FMA = 2); compares, abs, floor and
// conversions are excluded, which is the roofline convention and also why a
// compare-heavy kernel like `cells` sits far below the FLOP ceiling while
// being limited by something else (see scripts/mca.sh). Bytes are the
// kernel's own loads and stores per element, read off the source:
//   decode    per pixel: 12 B packet + 24 B LUT in, 12 B out    6 float FLOPs
//   sums      per point: 16 B in                                 7 double FLOPs
//   products  per point: 16 B in                                18 double FLOPs
//   classify  per point: 12 B in, 8 B out                        6 float FLOPs
//   cells     per point: 16 B in, 4 B out                        6 float FLOPs
// The working set (~12k points) is cache-resident by design, so the right
// bandwidth ceiling is L2, not DRAM; analysis/roofline.py draws both.
//   usage: kernel_bench PCAP META [SECTOR_DEG]   -> CSV on stdout
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "geometry.hpp"
#include "pcap.hpp"
#include "simd_kernels.hpp"
#include "xyz.hpp"

using Clock = std::chrono::steady_clock;

namespace {

template <class F>
double best_ns(F&& f, int reps) {
  double best = 1e30;
  for (int trial = 0; trial < 9; ++trial) {
    const auto t0 = Clock::now();
    for (int r = 0; r < reps; ++r) f();
    best = std::min(best, std::chrono::duration<double, std::nano>(Clock::now() - t0).count() / reps);
  }
  return best;
}

}  // namespace

int main(int argc, char** argv) try {
  if (argc < 3) {
    std::fprintf(stderr, "usage: kernel_bench PCAP META [SECTOR_DEG]\n");
    return 2;
  }
  const auto info = avl::load_sensor_info(argv[2]);
  const auto bytes = avl::read_file(argv[1]);
  const auto cap = avl::parse_pcap(bytes);
  const avl::Config cfg;
  const avl::XyzLut lut(info);
  const avl::Sector sector(info, argc > 3 ? std::stod(argv[3]) : cfg.sector_deg);
  const avl::GridSpec grid(cfg);
  const avl::simd::Roi roi{cfg.roi_x_min, cfg.roi_x_max, cfg.roi_y_abs};

  struct Col {
    const std::uint8_t* px;
    int mid;
  };
  std::vector<Col> cols;
  for (const auto& d : cap.datagrams) {
    if (!avl::is_lidar_packet(d.payload, info)) continue;
    const avl::PacketView pkt(d.payload, info);
    for (int c = 0; c < pkt.columns(); ++c) {
      const auto h = pkt.header(c);
      if (h.valid() && sector.contains(h.measurement_id)) cols.push_back({pkt.pixels(c), h.measurement_id});
    }
  }
  const std::size_t cap_pts =
      cols.size() * static_cast<std::size_t>(info.pixels_per_column) + avl::simd::kBlock;
  std::vector<float> x(cap_pts), y(cap_pts), z(cap_pts), h(cap_pts), w(cap_pts);
  std::vector<std::int32_t> cell(cap_pts);
  std::size_t n = 0;
  auto decode_all = [&] {
    n = 0;
    for (const Col& c : cols)
      n = avl::simd::decode_column(c.px, lut, c.mid, roi, x.data(), y.data(), z.data(), n);
  };
  decode_all();
  const std::size_t np = avl::simd::round_up(n);
  const avl::Plane plane{0.0f, 0.0f, 1.0f, 1.9f};
  avl::simd::classify(plane, cfg.gpf_dist_thresh_m, x.data(), y.data(), z.data(), h.data(), w.data(), 0, np);
  const double mean[3] = {10.0, 0.0, -1.9};
  const auto pixels = static_cast<double>(cols.size()) * info.pixels_per_column;
  const auto pts = static_cast<double>(np);

  std::printf("kernel,elements,ns,flops_per_elem,bytes_per_elem,precision,gflops,gbps,intensity\n");
  auto report = [&](const char* name, double elems, double ns, double flops, double bytes_per,
                    const char* prec) {
    std::printf("%s,%.0f,%.1f,%.0f,%.0f,%s,%.3f,%.3f,%.4f\n", name, elems, ns, flops, bytes_per, prec,
                flops * elems / ns, bytes_per * elems / ns, flops / bytes_per);
  };
  volatile double sink = 0;
  report("decode", pixels, best_ns(decode_all, 200), 6, 48, "float");
  report("sums", pts,
         best_ns([&] { sink = avl::simd::sums(x.data(), y.data(), z.data(), w.data(), 0, np).w; }, 2000), 7,
         16, "double");
  report("products", pts,
         best_ns([&] { sink = avl::simd::products(x.data(), y.data(), z.data(), w.data(), mean, 0, np).xx; },
                 2000),
         18, 16, "double");
  report("classify", pts,
         best_ns(
             [&] {
               avl::simd::classify(plane, cfg.gpf_dist_thresh_m, x.data(), y.data(), z.data(), h.data(),
                                   w.data(), 0, np);
             },
             2000),
         6, 20, "float");
  report("cells", pts,
         best_ns([&] { avl::simd::cells(grid, x.data(), y.data(), h.data(), w.data(), cell.data(), 0, np); },
                 2000),
         6, 20, "float");
  std::fprintf(stderr, "sector columns %zu, points %zu (padded %zu)\n", cols.size(), n, np);
  return 0;
} catch (const std::exception& e) {
  std::fprintf(stderr, "kernel_bench: %s\n", e.what());
  return 1;
}
