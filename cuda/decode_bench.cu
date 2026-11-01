// cuda_decode_bench: rung 6, the one CUDA kernel, with every host<->device
// cost on the clock.
//
// The kernel is packet decode (range -> xyz -> ROI test), the most
// data-parallel stage: one thread per pixel, no inter-thread communication.
// It is measured in the two shapes the pipeline could use it:
//   packet  one 16-column packet, as it arrives (what is on the critical
//           path when decode streams with the packets, as on the CPU)
//   sector  every packet of the forward sector at once, after the trigger
// Each repetition is timed end to end on the host clock: copy into pinned
// memory, H2D, kernel, D2H, stream sync, and the host compaction that turns
// the GPU's fixed-slot output into the dense SoA arrays the next stage reads.
// CUDA events split the device-side part. The CPU baseline is the Highway
// decode_column on the same columns.
//
// Device arithmetic uses __fmul_rn/__fadd_rn so nvcc cannot contract to FMA;
// the output is therefore bit-identical to the CPU's, and is checked.
//   usage: cuda_decode_bench PCAP META [reps]  -> CSV on stdout
#include <cuda_runtime.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "pcap.hpp"
#include "pipeline.hpp"
#include "simd_kernels.hpp"
#include "xyz.hpp"

#define CUDA_CHECK(x)                                                                   \
  do {                                                                                  \
    cudaError_t err_ = (x);                                                             \
    if (err_ != cudaSuccess) {                                                          \
      std::fprintf(stderr, "%s:%d %s\n", __FILE__, __LINE__, cudaGetErrorString(err_)); \
      std::exit(1);                                                                     \
    }                                                                                   \
  } while (0)

using Clock = std::chrono::steady_clock;

struct DeviceGeom {
  const float *dx, *dy, *dz, *ox, *oy, *oz;
  const unsigned char* in_sector;  // per measurement id
  int columns_per_frame, rows, columns_per_packet;
  unsigned packet_bytes, column_bytes;
  float x_min, x_max, y_abs;
};

// grid = (columns in batch), block = rows. Rejected slots get NaN in x.
__global__ void decode_kernel(const unsigned char* packets, DeviceGeom g, float* x, float* y, float* z) {
  const int col = blockIdx.x, row = threadIdx.x;
  const unsigned char* pkt = packets + (col / g.columns_per_packet) * g.packet_bytes;
  const unsigned char* c = pkt + 32 + (col % g.columns_per_packet) * g.column_bytes;
  const unsigned mid = c[8] | (c[9] << 8);
  const unsigned status = c[10] | (c[11] << 8);
  const unsigned char* px = c + 12 + row * 12;
  const unsigned range = (px[0] | (px[1] << 8) | (px[2] << 16) | (px[3] << 24)) & 0x7ffffu;
  const int slot = col * g.rows + row;
  float fx = __int_as_float(0x7fc00000), fy = 0.0f, fz = 0.0f;  // quiet NaN = "no point"
  if ((status & 1u) && mid < static_cast<unsigned>(g.columns_per_frame) && g.in_sector[mid] && range != 0) {
    const int i = mid * g.rows + row;
    const float r = static_cast<float>(range);
    const float px_ = __fadd_rn(__fmul_rn(r, g.dx[i]), g.ox[i]);
    const float py_ = __fadd_rn(__fmul_rn(r, g.dy[i]), g.oy[i]);
    if (px_ >= g.x_min && px_ < g.x_max && fabsf(py_) < g.y_abs) {
      fx = px_, fy = py_, fz = __fadd_rn(__fmul_rn(r, g.dz[i]), g.oz[i]);
    }
  }
  x[slot] = fx, y[slot] = fy, z[slot] = fz;
}

template <class T>
T* upload(const std::vector<T>& v) {
  T* d = nullptr;
  CUDA_CHECK(cudaMalloc(&d, v.size() * sizeof(T)));
  CUDA_CHECK(cudaMemcpy(d, v.data(), v.size() * sizeof(T), cudaMemcpyHostToDevice));
  return d;
}

double pct(std::vector<double> v, double p) {
  std::sort(v.begin(), v.end());
  return v[static_cast<std::size_t>(p * static_cast<double>(v.size() - 1))];
}

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: cuda_decode_bench PCAP META [reps]\n");
    return 2;
  }
  const int reps = argc > 3 ? std::stoi(argv[3]) : 2000;
  const auto info = avl::load_sensor_info(argv[2]);
  const auto file = avl::read_file(argv[1]);
  const auto cap = avl::parse_pcap(file);
  const avl::Config cfg;
  const avl::XyzLut lut(info);
  const avl::Sector sector(info, cfg.sector_deg);

  // Packets that carry at least one sector column, in arrival order.
  std::vector<std::span<const std::uint8_t>> sector_pkts;
  for (const auto& d : cap.datagrams) {
    if (!avl::is_lidar_packet(d.payload, info)) continue;
    const avl::PacketView pv(d.payload, info);
    for (int c = 0; c < pv.columns(); ++c) {
      if (sector.contains(pv.header(c).measurement_id)) {
        sector_pkts.push_back(d.payload);
        break;
      }
    }
  }
  std::vector<unsigned char> mask(static_cast<std::size_t>(info.columns_per_frame));
  for (int m = 0; m < info.columns_per_frame; ++m) mask[static_cast<std::size_t>(m)] = sector.contains(m);

  DeviceGeom g{upload(lut.dx),
               upload(lut.dy),
               upload(lut.dz),
               upload(lut.ox),
               upload(lut.oy),
               upload(lut.oz),
               upload(mask),
               info.columns_per_frame,
               info.pixels_per_column,
               info.columns_per_packet,
               static_cast<unsigned>(info.lidar_packet_bytes()),
               static_cast<unsigned>(info.column_bytes()),
               cfg.roi_x_min,
               cfg.roi_x_max,
               cfg.roi_y_abs};

  const std::size_t max_pkts = sector_pkts.size();
  const std::size_t pkt_bytes = info.lidar_packet_bytes();
  const std::size_t slots =
      max_pkts * static_cast<std::size_t>(info.columns_per_packet * info.pixels_per_column);
  unsigned char *h_in, *d_in;
  float *h_out, *d_out;
  CUDA_CHECK(cudaMallocHost(&h_in, max_pkts * pkt_bytes));
  CUDA_CHECK(cudaMallocHost(&h_out, 3 * slots * sizeof(float)));
  CUDA_CHECK(cudaMalloc(&d_in, max_pkts * pkt_bytes));
  CUDA_CHECK(cudaMalloc(&d_out, 3 * slots * sizeof(float)));
  cudaStream_t s;
  CUDA_CHECK(cudaStreamCreate(&s));
  cudaEvent_t e0, e1, e2, e3;
  for (auto* e : {&e0, &e1, &e2, &e3}) CUDA_CHECK(cudaEventCreate(e));

  std::vector<float> cx(slots + avl::simd::kBlock), cy(cx.size()), cz(cx.size());
  std::vector<float> gx(cx.size()), gy(cx.size()), gz(cx.size());
  const avl::simd::Roi roi{cfg.roi_x_min, cfg.roi_x_max, cfg.roi_y_abs};

  std::printf("mode,packets,stage,p50_us,p99_us\n");
  for (const std::size_t npk : {std::size_t{1}, max_pkts}) {
    const char* mode = npk == 1 ? "packet" : "sector";
    // The last npk packets: for "packet" that is the trigger packet itself.
    const std::size_t first = max_pkts - npk;
    const std::size_t nslots =
        npk * static_cast<std::size_t>(info.columns_per_packet * info.pixels_per_column);
    std::vector<double> total, h2d, kern, d2h, compact, cpu;
    std::size_t gpu_n = 0, cpu_n = 0;
    for (int r = 0; r < reps; ++r) {
      const auto t0 = Clock::now();
      for (std::size_t p = 0; p < npk; ++p)
        std::memcpy(h_in + p * pkt_bytes, sector_pkts[first + p].data(), pkt_bytes);
      CUDA_CHECK(cudaEventRecord(e0, s));
      CUDA_CHECK(cudaMemcpyAsync(d_in, h_in, npk * pkt_bytes, cudaMemcpyHostToDevice, s));
      CUDA_CHECK(cudaEventRecord(e1, s));
      decode_kernel<<<static_cast<unsigned>(npk) * info.columns_per_packet, info.pixels_per_column, 0, s>>>(
          d_in, g, d_out, d_out + slots, d_out + 2 * slots);
      CUDA_CHECK(cudaEventRecord(e2, s));
      CUDA_CHECK(cudaMemcpyAsync(h_out, d_out, nslots * sizeof(float), cudaMemcpyDeviceToHost, s));
      CUDA_CHECK(
          cudaMemcpyAsync(h_out + slots, d_out + slots, nslots * sizeof(float), cudaMemcpyDeviceToHost, s));
      CUDA_CHECK(cudaMemcpyAsync(h_out + 2 * slots, d_out + 2 * slots, nslots * sizeof(float),
                                 cudaMemcpyDeviceToHost, s));
      CUDA_CHECK(cudaEventRecord(e3, s));
      CUDA_CHECK(cudaStreamSynchronize(s));
      const auto t1 = Clock::now();
      gpu_n = 0;
      for (std::size_t i = 0; i < nslots; ++i) {
        if (std::isnan(h_out[i])) continue;
        gx[gpu_n] = h_out[i], gy[gpu_n] = h_out[slots + i], gz[gpu_n] = h_out[2 * slots + i];
        ++gpu_n;
      }
      const auto t2 = Clock::now();
      float ms;
      CUDA_CHECK(cudaEventElapsedTime(&ms, e0, e1));
      h2d.push_back(ms * 1e3);
      CUDA_CHECK(cudaEventElapsedTime(&ms, e1, e2));
      kern.push_back(ms * 1e3);
      CUDA_CHECK(cudaEventElapsedTime(&ms, e2, e3));
      d2h.push_back(ms * 1e3);
      compact.push_back(std::chrono::duration<double, std::micro>(t2 - t1).count());
      total.push_back(std::chrono::duration<double, std::micro>(t2 - t0).count());

      const auto c0 = Clock::now();
      cpu_n = 0;
      for (std::size_t p = 0; p < npk; ++p) {
        const avl::PacketView pv(sector_pkts[first + p], info);
        for (int c = 0; c < pv.columns(); ++c) {
          const auto hd = pv.header(c);
          if (!hd.valid() || !sector.contains(hd.measurement_id)) continue;
          cpu_n = avl::simd::decode_column(pv.pixels(c), lut, hd.measurement_id, roi, cx.data(), cy.data(),
                                           cz.data(), cpu_n);
        }
      }
      cpu.push_back(std::chrono::duration<double, std::micro>(Clock::now() - c0).count());
    }
    const bool same = gpu_n == cpu_n && std::memcmp(gx.data(), cx.data(), cpu_n * sizeof(float)) == 0 &&
                      std::memcmp(gy.data(), cy.data(), cpu_n * sizeof(float)) == 0 &&
                      std::memcmp(gz.data(), cz.data(), cpu_n * sizeof(float)) == 0;
    const std::pair<const char*, std::vector<double>*> rows[] = {
        {"h2d", &h2d},         {"kernel", &kern}, {"d2h", &d2h}, {"host_compact", &compact},
        {"gpu_total", &total}, {"cpu_simd", &cpu}};
    for (const auto& [name, v] : rows) {
      std::printf("%s,%zu,%s,%.2f,%.2f\n", mode, npk, name, pct(*v, 0.5), pct(*v, 0.99));
    }
    std::fprintf(stderr, "%s: %zu points, GPU output %s CPU output\n", mode, cpu_n,
                 same ? "bit-identical to" : "DIFFERS FROM");
    if (!same) return 1;
  }
  return 0;
}
