// "naive": the baseline rung. Written the way the algorithm reads on paper,
// array-of-structs with std::vector, no SIMD, no threads. It is the
// reference every faster rung is diffed against, so clarity beats speed here.
//
// It is not deliberately slow: member vectors are clear()ed rather than
// reallocated, so after the first sector it does not allocate. That keeps
// "allocation" out of the ladder; every rung differs from this one in exactly
// the property its name says.
//
// Stages, all on the packet that completes the sector:
//   1. GPF ground fit (Zermas et al. 2017): sort heights, seed from the mean
//      of the lowest gpf_lpr_count, refit the plane gpf_iterations times.
//   2. Voxelize non-ground points as (x, y, height above plane) counts.
//   3. Nearest occupied corridor voxel -> decide().
// Decoding happens per packet as packets arrive, so only the last packet's
// decode is on the decision's critical path.
#include <algorithm>
#include <vector>

#include "geometry.hpp"
#include "pipeline.hpp"
#include "xyz.hpp"

namespace avl {
namespace {

struct Point {
  float x, y, z;
  float h;      // signed height above the fitted ground plane
  bool ground;  // AoS: every stage drags every field through the cache
};

// kSelect = false is the baseline; true is ladder rung 1 ("nth"), which
// replaces the full sort with std::nth_element (directive/p3.md, rung 1).
template <bool kSelect>
class NaivePipeline final : public Pipeline {
 public:
  NaivePipeline(const SensorInfo& info, const Config& cfg)
      : info_(info),
        cfg_(cfg),
        lut_(info),
        sector_(info, cfg.sector_deg),
        grid_spec_(cfg),
        grid_(grid_spec_.cells(), 0) {}

  std::optional<Decision> on_packet(std::span<const std::uint8_t> payload) override {
    if (!is_lidar_packet(payload, info_)) return std::nullopt;
    const PacketView pkt(payload, info_);
    // A sector left incomplete by packet loss must not leak into the next frame.
    if (!points_.empty() && pkt.frame_id() != frame_id_) points_.clear();
    frame_id_ = pkt.frame_id();

    for_each_return(pkt, lut_, [&](int mid, int, std::uint32_t, float x, float y, float z) {
      if (!sector_.contains(mid)) return;
      if (x < cfg_.roi_x_min || x >= cfg_.roi_x_max || std::abs(y) >= cfg_.roi_y_abs) return;
      points_.push_back({x, y, z, 0.0f, false});
    });

    bool triggered = false;
    for (int c = 0; c < pkt.columns(); ++c) {
      triggered |= pkt.header(c).measurement_id == sector_.trigger_mid();
    }
    if (!triggered) return std::nullopt;
    Decision d = process();
    points_.clear();
    return d;
  }

 private:
  Decision process() {
    const auto n = static_cast<std::uint32_t>(points_.size());
    const std::optional<Plane> plane = fit_ground();
    if (!plane) {
      Decision d = decide(std::numeric_limits<float>::infinity(), cfg_);
      d.points = n;
      return d;
    }

    std::fill(grid_.begin(), grid_.end(), std::uint8_t{0});
    std::uint32_t ground = 0, occupied = 0;
    for (const Point& p : points_) {
      if (p.ground) {
        ++ground;
        continue;
      }
      const std::int64_t cell = grid_spec_.cell(p.x, p.y, p.h);
      if (cell < 0) continue;
      std::uint8_t& count = grid_[static_cast<std::size_t>(cell)];
      if (count < 255) ++count;
      if (count == cfg_.min_points_per_voxel) ++occupied;
    }
    Decision d = decide(nearest_in_corridor(grid_.data(), grid_spec_, cfg_.min_points_per_voxel), cfg_);
    d.points = n;
    d.ground = ground;
    d.occupied = occupied;
    if (cfg_.trace) record_trace(*plane);
    return d;
  }

  void record_trace(const Plane& plane) {
    trace_ = Trace{};
    for (const Point& p : points_) {
      trace_.x.push_back(p.x), trace_.y.push_back(p.y), trace_.z.push_back(p.z), trace_.h.push_back(p.h);
      trace_.ground.push_back(p.ground ? 1 : 0);
    }
    trace_.plane[0] = plane.nx, trace_.plane[1] = plane.ny, trace_.plane[2] = plane.nz,
    trace_.plane[3] = plane.d;
    trace_.grid = grid_;
  }

 public:
  [[nodiscard]] const Trace* trace() const override { return cfg_.trace ? &trace_ : nullptr; }

 private:
  std::optional<Plane> fit_ground() {
    if (points_.size() < 3) return std::nullopt;
    heights_.clear();
    for (const Point& p : points_) heights_.push_back(p.z);
    const std::size_t k = std::min(points_.size(), static_cast<std::size_t>(cfg_.gpf_lpr_count));
    if constexpr (kSelect) {
      // Only the k lowest are needed, not their order: O(n) instead of O(n log n).
      // Summing 200 floats of similar magnitude in double is exact, so lpr does
      // not depend on their order; equivalence_test.cpp checks that it holds.
      std::nth_element(heights_.begin(), heights_.begin() + static_cast<std::ptrdiff_t>(k - 1),
                       heights_.end());
    } else {
      std::sort(heights_.begin(), heights_.end());
    }
    double lpr = 0;
    for (std::size_t i = 0; i < k; ++i) lpr += heights_[i];
    lpr /= static_cast<double>(k);
    const auto seed_below = static_cast<float>(lpr + cfg_.gpf_seed_thresh_m);
    for (Point& p : points_) p.ground = p.z < seed_below;

    std::optional<Plane> plane;
    for (int it = 0; it < cfg_.gpf_iterations; ++it) {
      const std::optional<Plane> next = fit_plane(ground_covariance());
      if (!next) break;
      plane = next;
      for (Point& p : points_) {
        p.h = plane->height(p.x, p.y, p.z);
        p.ground = std::abs(p.h) < cfg_.gpf_dist_thresh_m;
      }
    }
    return plane;
  }

  // Two passes (mean, then centred products) for numerical stability.
  [[nodiscard]] Covariance ground_covariance() const {
    Covariance c;
    double sx = 0, sy = 0, sz = 0;
    for (const Point& p : points_) {
      if (!p.ground) continue;
      sx += p.x, sy += p.y, sz += p.z;
      ++c.n;
    }
    if (c.n == 0) return c;
    const auto inv = 1.0 / static_cast<double>(c.n);
    c.mean[0] = sx * inv, c.mean[1] = sy * inv, c.mean[2] = sz * inv;
    for (const Point& p : points_) {
      if (!p.ground) continue;
      const double dx = p.x - c.mean[0], dy = p.y - c.mean[1], dz = p.z - c.mean[2];
      c.xx += dx * dx, c.xy += dx * dy, c.xz += dx * dz;
      c.yy += dy * dy, c.yz += dy * dz, c.zz += dz * dz;
    }
    return c;
  }

  SensorInfo info_;
  Config cfg_;
  XyzLut lut_;
  Sector sector_;
  GridSpec grid_spec_;
  std::vector<std::uint8_t> grid_;
  std::vector<Point> points_;
  std::vector<float> heights_;
  std::uint16_t frame_id_ = 0;
  Trace trace_;
};

const Registrar kNaive{"naive", [](const SensorInfo& i, const Config& c) {
                         return std::make_unique<NaivePipeline<false>>(i, c);
                       }};
const Registrar kNth{
    "nth", [](const SensorInfo& i, const Config& c) { return std::make_unique<NaivePipeline<true>>(i, c); }};

}  // namespace
}  // namespace avl
