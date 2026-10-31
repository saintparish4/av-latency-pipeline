// See synth.hpp.
#include "synth.hpp"

#include <cmath>
#include <cstring>
#include <limits>

namespace avl::synth {
namespace {

constexpr float kMaxRange = 120.0f;

// Slab test: distance along unit ray (o, u) to the box, or +inf.
float hit_box(const float o[3], const float u[3], const Box& b) {
  const float lo[3] = {b.x0, b.y0, b.z0}, hi[3] = {b.x1, b.y1, b.z1};
  float tmin = 0.0f, tmax = std::numeric_limits<float>::infinity();
  for (int a = 0; a < 3; ++a) {
    if (std::abs(u[a]) < 1e-12f) {
      if (o[a] < lo[a] || o[a] > hi[a]) return std::numeric_limits<float>::infinity();
      continue;
    }
    float t0 = (lo[a] - o[a]) / u[a], t1 = (hi[a] - o[a]) / u[a];
    if (t0 > t1) std::swap(t0, t1);
    tmin = std::max(tmin, t0);
    tmax = std::min(tmax, t1);
    if (tmin > tmax) return std::numeric_limits<float>::infinity();
  }
  return tmin;
}

void put(std::vector<std::uint8_t>& p, std::size_t off, const void* v, std::size_t n) {
  std::memcpy(p.data() + off, v, n);
}

}  // namespace

std::vector<std::vector<std::uint8_t>> render(const SensorInfo& info, const XyzLut& lut, const Scene& scene,
                                              std::uint16_t frame_id) {
  std::vector<std::vector<std::uint8_t>> packets;
  const int w = info.columns_per_frame, cpp = info.columns_per_packet;
  for (int first = 0; first < w; first += cpp) {
    std::vector<std::uint8_t> p(info.lidar_packet_bytes(), 0);
    const std::uint16_t type = 1;
    put(p, 0, &type, 2);
    put(p, 2, &frame_id, 2);
    for (int c = 0; c < cpp; ++c) {
      const auto mid = static_cast<std::uint16_t>(first + c);
      const std::size_t col = kPacketHeaderBytes + static_cast<std::size_t>(c) * info.column_bytes();
      const std::uint64_t ts = 1'000'000'000ull + mid * 97'656ull;  // 10 Hz spacing
      const std::uint16_t status = 1;
      put(p, col, &ts, 8);
      put(p, col + 8, &mid, 2);
      put(p, col + 10, &status, 2);
      for (int row = 0; row < info.pixels_per_column; ++row) {
        const std::size_t i = lut.index(mid, row);
        // LUT point = r_mm * d + o, so the ray is origin o, direction d*1000.
        const float o[3] = {lut.ox[i], lut.oy[i], lut.oz[i]};
        const float u[3] = {lut.dx[i] * 1000.0f, lut.dy[i] * 1000.0f, lut.dz[i] * 1000.0f};
        float t = std::numeric_limits<float>::infinity();
        if (u[2] < 0) t = (scene.ground_z - o[2]) / u[2];
        for (const Box& b : scene.boxes) t = std::min(t, hit_box(o, u, b));
        if (!(t < kMaxRange)) continue;
        const auto range = static_cast<std::uint32_t>(std::lround(t * 1000.0f)) & kRangeMask;
        const std::uint8_t refl = 50;
        const std::uint16_t signal = 100, nir = 10;
        const std::size_t px = col + kColumnHeaderBytes + static_cast<std::size_t>(row) * kPixelBytes;
        put(p, px, &range, 4);
        put(p, px + 4, &refl, 1);
        put(p, px + 6, &signal, 2);
        put(p, px + 8, &nir, 2);
      }
    }
    packets.push_back(std::move(p));
  }
  return packets;
}

std::vector<Scene> braking_scenes() {
  // Box helper: a car-like obstacle whose near face is at x, ground-standing.
  const float g = -1.8f;
  auto car = [g](float x, float y_centre) {
    return Box{x, x + 4.5f, y_centre - 0.9f, y_centre + 0.9f, g, g + 1.5f};
  };
  // Config defaults brake below 21.27 m (pipeline_test.cpp). Distances sit at
  // least one voxel (0.2 m) away from that line so quantization cannot flip them.
  return {
      {"empty_road", g, {}, false, -1},
      {"car_30m_ahead", g, {car(30.0f, 0.0f)}, false, 30.0f},
      {"car_15m_ahead", g, {car(15.0f, 0.0f)}, true, 15.0f},
      {"car_20m_ahead", g, {car(20.0f, 0.0f)}, true, 20.0f},
      {"car_22m_ahead", g, {car(22.0f, 0.0f)}, false, 22.0f},
      {"car_12m_adjacent_lane", g, {car(12.0f, 3.4f)}, false, -1},
      {"pedestrian_18m", g, {Box{18.0f, 18.5f, 0.05f, 0.55f, g, g + 1.7f}}, true, 18.0f},
      {"debris_10cm_at_10m", g, {Box{10.0f, 10.6f, -0.5f, 0.5f, g, g + 0.1f}}, false, -1},
      {"overhead_sign_12m", g, {Box{12.0f, 12.3f, -3.0f, 3.0f, g + 3.0f, g + 4.0f}}, false, -1},
      {"car_25m_behind_car_16m", g, {car(25.0f, 0.0f), car(16.0f, 0.4f)}, true, 16.0f},
  };
}

}  // namespace avl::synth
