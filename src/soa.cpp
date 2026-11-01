// "soa": rung 2. Exactly the "nth" algorithm, with one change: points live
// in struct-of-arrays form (separate x, y, z, h, ground arrays) in buffers
// sized once for the whole sector.
//
// Hypothesis (directive/p3.md): the GPF passes each read 1-3 fields of a
// 20-byte AoS Point, so most of every cache line fetched is unused; SoA
// makes each pass stream only what it reads. Arithmetic and summation order
// are identical to naive.cpp, so outputs must be bit-identical, which
// tests/equivalence_test.cpp checks.
#include <algorithm>
#include <vector>

#include "geometry.hpp"
#include "pipeline.hpp"
#include "xyz.hpp"

namespace avl {
namespace {

class SoaPipeline final : public Pipeline {
 public:
  SoaPipeline(const SensorInfo& info, const Config& cfg)
      : info_(info),
        cfg_(cfg),
        lut_(info),
        sector_(info, cfg.sector_deg),
        grid_spec_(cfg),
        grid_(grid_spec_.cells(), 0) {
    const std::size_t cap =
        static_cast<std::size_t>(sector_.column_count()) * static_cast<std::size_t>(info.pixels_per_column);
    for (auto* v : {&x_, &y_, &z_, &h_, &heights_}) v->resize(cap);
    ground_.resize(cap);
  }

  std::optional<Decision> on_packet(std::span<const std::uint8_t> payload) override {
    if (!is_lidar_packet(payload, info_)) return std::nullopt;
    const PacketView pkt(payload, info_);
    if (n_ != 0 && pkt.frame_id() != frame_id_) n_ = 0;
    frame_id_ = pkt.frame_id();
    for_each_return(pkt, lut_, [&](int mid, int, std::uint32_t, float x, float y, float z) {
      // Duplicated packets could exceed one sector's worth; drop, never overflow.
      if (!sector_.contains(mid) || n_ == x_.size()) return;
      if (x < cfg_.roi_x_min || x >= cfg_.roi_x_max || std::abs(y) >= cfg_.roi_y_abs) return;
      x_[n_] = x, y_[n_] = y, z_[n_] = z;
      ++n_;
    });
    bool triggered = false;
    for (int c = 0; c < pkt.columns(); ++c) {
      triggered |= pkt.header(c).measurement_id == sector_.trigger_mid();
    }
    if (!triggered) return std::nullopt;
    Decision d = process();
    n_ = 0;
    return d;
  }

 private:
  Decision process() {
    const auto n = static_cast<std::uint32_t>(n_);
    const std::optional<Plane> plane = fit_ground();
    if (!plane) {
      Decision d = decide(std::numeric_limits<float>::infinity(), cfg_);
      d.points = n;
      return d;
    }
    std::fill(grid_.begin(), grid_.end(), std::uint8_t{0});
    std::uint32_t ground = 0, occupied = 0;
    for (std::size_t i = 0; i < n_; ++i) {
      if (ground_[i]) {
        ++ground;
        continue;
      }
      const std::int64_t cell = grid_spec_.cell(x_[i], y_[i], h_[i]);
      if (cell < 0) continue;
      std::uint8_t& count = grid_[static_cast<std::size_t>(cell)];
      if (count < 255) ++count;
      if (count == cfg_.min_points_per_voxel) ++occupied;
    }
    Decision d = decide(nearest_in_corridor(grid_.data(), grid_spec_, cfg_.min_points_per_voxel), cfg_);
    d.points = n;
    d.ground = ground;
    d.occupied = occupied;
    return d;
  }

  std::optional<Plane> fit_ground() {
    if (n_ < 3) return std::nullopt;
    std::copy(z_.begin(), z_.begin() + static_cast<std::ptrdiff_t>(n_), heights_.begin());
    const std::size_t k = std::min(n_, static_cast<std::size_t>(cfg_.gpf_lpr_count));
    const auto first = heights_.begin(), last = first + static_cast<std::ptrdiff_t>(n_);
    std::nth_element(first, first + static_cast<std::ptrdiff_t>(k - 1), last);
    double lpr = 0;
    for (std::size_t i = 0; i < k; ++i) lpr += heights_[i];
    lpr /= static_cast<double>(k);
    const auto seed_below = static_cast<float>(lpr + cfg_.gpf_seed_thresh_m);
    for (std::size_t i = 0; i < n_; ++i) ground_[i] = z_[i] < seed_below;

    std::optional<Plane> plane;
    for (int it = 0; it < cfg_.gpf_iterations; ++it) {
      const std::optional<Plane> next = fit_plane(ground_covariance());
      if (!next) break;
      const Plane pl = *next;
      plane = pl;
      for (std::size_t i = 0; i < n_; ++i) {
        h_[i] = pl.height(x_[i], y_[i], z_[i]);
        ground_[i] = std::abs(h_[i]) < cfg_.gpf_dist_thresh_m;
      }
    }
    return plane;
  }

  [[nodiscard]] Covariance ground_covariance() const {
    Covariance c;
    double sx = 0, sy = 0, sz = 0;
    for (std::size_t i = 0; i < n_; ++i) {
      if (!ground_[i]) continue;
      sx += x_[i], sy += y_[i], sz += z_[i];
      ++c.n;
    }
    if (c.n == 0) return c;
    const auto inv = 1.0 / static_cast<double>(c.n);
    c.mean[0] = sx * inv, c.mean[1] = sy * inv, c.mean[2] = sz * inv;
    for (std::size_t i = 0; i < n_; ++i) {
      if (!ground_[i]) continue;
      const double dx = x_[i] - c.mean[0], dy = y_[i] - c.mean[1], dz = z_[i] - c.mean[2];
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
  std::vector<float> x_, y_, z_, h_, heights_;
  // char8_t, not std::uint8_t (rung 2b): a store through unsigned char may
  // alias any object, so every label write forced reloads of the array
  // pointers and the plane and blocked auto-vectorization. char8_t is a
  // distinct 1-byte type that does not carry that licence. (enum class :
  // std::uint8_t was tried and measured no faster, consistent with clang
  // giving an enum its underlying type's alias set.)
  std::vector<char8_t> ground_;
  std::size_t n_ = 0;
  std::uint16_t frame_id_ = 0;
};

const Registrar kRegister{
    "soa", [](const SensorInfo& i, const Config& c) { return std::make_unique<SoaPipeline>(i, c); }};

}  // namespace
}  // namespace avl
