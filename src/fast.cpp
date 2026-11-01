// "simd" (rung 3) and "mt" (rung 4): the soa algorithm built from the
// Highway kernels in simd_kernels.cpp, run either on the calling thread or
// fork-joined across a SpinPool.
//
// Both rungs run the same seven parallel regions per decision:
//   seed+sums, products -> plane1, classify+sums, products -> plane2,
//   classify+sums, products -> plane3, classify+cells
// then a scalar scatter into the grid. Per-worker partial sums are reduced
// in worker order, so a given helper count is deterministic run to run.
// Decode stays on the packet thread: it happens as packets arrive, and only
// the trigger packet's decode is on the decision's critical path.
#include <algorithm>
#include <memory>
#include <vector>

#include "geometry.hpp"
#include "pipeline.hpp"
#include "pool.hpp"
#include "simd_kernels.hpp"
#include "xyz.hpp"

namespace avl {
namespace {

struct alignas(64) Partial {  // one cache line per worker: no false sharing
  simd::Sums sums;
  simd::Products products;
};

template <bool kThreaded>
class FastPipeline final : public Pipeline {
 public:
  FastPipeline(const SensorInfo& info, const Config& cfg)
      : info_(info),
        cfg_(cfg),
        lut_(info),
        sector_(info, cfg.sector_deg),
        grid_spec_(cfg),
        grid_(grid_spec_.cells(), 0),
        roi_{cfg.roi_x_min, cfg.roi_x_max, cfg.roi_y_abs} {
    cap_ =
        static_cast<std::size_t>(sector_.column_count()) * static_cast<std::size_t>(info.pixels_per_column);
    for (auto* v : {&x_, &y_, &z_, &h_, &w_, &heights_}) v->assign(cap_ + simd::kBlock, 0.0f);
    cell_.assign(cap_ + simd::kBlock, -1);
    if constexpr (kThreaded) pool_ = std::make_unique<SpinPool>(cfg.helpers, cfg.first_helper_cpu);
    partial_.resize(static_cast<std::size_t>(workers()));
  }

  std::optional<Decision> on_packet(std::span<const std::uint8_t> payload) override {
    if (!is_lidar_packet(payload, info_)) return std::nullopt;
    const PacketView pkt(payload, info_);
    if (n_ != 0 && pkt.frame_id() != frame_id_) n_ = 0;
    frame_id_ = pkt.frame_id();
    const auto rows = static_cast<std::size_t>(info_.pixels_per_column);
    bool triggered = false;
    for (int c = 0; c < pkt.columns(); ++c) {
      const ColumnHeader hdr = pkt.header(c);
      triggered |= hdr.measurement_id == sector_.trigger_mid();
      if (!hdr.valid() || !sector_.contains(hdr.measurement_id) || n_ + rows > cap_) continue;
      n_ = simd::decode_column(pkt.pixels(c), lut_, hdr.measurement_id, roi_, x_.data(), y_.data(), z_.data(),
                               n_);
    }
    if (!triggered) return std::nullopt;
    Decision d = process();
    n_ = 0;
    return d;
  }

 private:
  [[nodiscard]] int workers() const {
    if constexpr (kThreaded) return pool_->workers();
    return 1;
  }

  // Calls body(b, e, worker) on contiguous kBlock-aligned chunks of [0, np).
  template <class Body>
  void parallel(std::size_t np, Body&& body) {
    const auto w = static_cast<std::size_t>(workers());
    const std::size_t chunk = simd::round_up((np + w - 1) / w);
    auto job = [&](int worker) {
      const std::size_t b = static_cast<std::size_t>(worker) * chunk, e = std::min(np, b + chunk);
      if (b < e) body(b, e, static_cast<std::size_t>(worker));
    };
    if constexpr (kThreaded) {
      pool_->run(job);
    } else {
      job(0);
    }
  }

  // Lanes past the last real point must carry weight 0 whatever the kernel wrote.
  void zero_padding(std::size_t b, std::size_t e) {
    for (std::size_t i = std::max(b, n_); i < e; ++i) w_[i] = 0.0f;
  }

  Decision process() {
    const auto n = static_cast<std::uint32_t>(n_);
    const std::size_t np = simd::round_up(n_);
    std::fill(x_.begin() + static_cast<std::ptrdiff_t>(n_), x_.begin() + static_cast<std::ptrdiff_t>(np),
              0.0f);
    std::fill(y_.begin() + static_cast<std::ptrdiff_t>(n_), y_.begin() + static_cast<std::ptrdiff_t>(np),
              0.0f);
    std::fill(z_.begin() + static_cast<std::ptrdiff_t>(n_), z_.begin() + static_cast<std::ptrdiff_t>(np),
              0.0f);
    const std::optional<Plane> plane = fit_ground(np);
    if (!plane) {
      Decision d = decide(std::numeric_limits<float>::infinity(), cfg_);
      d.points = n;
      return d;
    }
    parallel(np, [&](std::size_t b, std::size_t e, std::size_t) {
      simd::classify(*plane, cfg_.gpf_dist_thresh_m, x_.data(), y_.data(), z_.data(), h_.data(), w_.data(), b,
                     e);
      zero_padding(b, e);
      simd::cells(grid_spec_, x_.data(), y_.data(), h_.data(), w_.data(), cell_.data(), b, e);
    });
    std::fill(grid_.begin(), grid_.end(), std::uint8_t{0});
    std::uint32_t ground = 0, occupied = 0;
    for (std::size_t i = 0; i < n_; ++i) {
      const std::int32_t c = cell_[i];
      if (c < 0) {
        ground += w_[i] != 0.0f ? 1u : 0u;
        continue;
      }
      std::uint8_t& count = grid_[static_cast<std::size_t>(c)];
      if (count < 255) ++count;
      if (count == cfg_.min_points_per_voxel) ++occupied;
    }
    Decision d = decide(nearest_in_corridor(grid_.data(), grid_spec_, cfg_.min_points_per_voxel), cfg_);
    d.points = n;
    d.ground = ground;
    d.occupied = occupied;
    return d;
  }

  std::optional<Plane> fit_ground(std::size_t np) {
    if (n_ < 3) return std::nullopt;
    std::copy(z_.begin(), z_.begin() + static_cast<std::ptrdiff_t>(n_), heights_.begin());
    const std::size_t k = std::min(n_, static_cast<std::size_t>(cfg_.gpf_lpr_count));
    const auto first = heights_.begin(), last = first + static_cast<std::ptrdiff_t>(n_);
    std::nth_element(first, first + static_cast<std::ptrdiff_t>(k - 1), last);
    double lpr = 0;
    for (std::size_t i = 0; i < k; ++i) lpr += heights_[i];
    lpr /= static_cast<double>(k);
    const auto seed_below = static_cast<float>(lpr + cfg_.gpf_seed_thresh_m);

    std::optional<Plane> plane;
    for (int it = 0; it < cfg_.gpf_iterations; ++it) {
      parallel(np, [&](std::size_t b, std::size_t e, std::size_t worker) {
        if (plane) {
          simd::classify(*plane, cfg_.gpf_dist_thresh_m, x_.data(), y_.data(), z_.data(), h_.data(),
                         w_.data(), b, e);
        } else {
          simd::seed(z_.data(), seed_below, w_.data(), b, e);
        }
        zero_padding(b, e);
        partial_[worker].sums = simd::sums(x_.data(), y_.data(), z_.data(), w_.data(), b, e);
      });
      simd::Sums s;
      for (const Partial& p : partial_) s.w += p.sums.w, s.x += p.sums.x, s.y += p.sums.y, s.z += p.sums.z;
      if (s.w < 3) break;
      Covariance c;
      c.n = static_cast<std::uint64_t>(s.w);
      c.mean[0] = s.x / s.w, c.mean[1] = s.y / s.w, c.mean[2] = s.z / s.w;
      parallel(np, [&](std::size_t b, std::size_t e, std::size_t worker) {
        partial_[worker].products = simd::products(x_.data(), y_.data(), z_.data(), w_.data(), c.mean, b, e);
      });
      for (const Partial& p : partial_) {
        c.xx += p.products.xx, c.xy += p.products.xy, c.xz += p.products.xz;
        c.yy += p.products.yy, c.yz += p.products.yz, c.zz += p.products.zz;
      }
      const std::optional<Plane> next = fit_plane(c);
      if (!next) break;
      plane = next;
    }
    return plane;
  }

  SensorInfo info_;
  Config cfg_;
  XyzLut lut_;
  Sector sector_;
  GridSpec grid_spec_;
  std::vector<std::uint8_t> grid_;
  simd::Roi roi_;
  std::size_t cap_ = 0, n_ = 0;
  std::vector<float> x_, y_, z_, h_, w_, heights_;
  std::vector<std::int32_t> cell_;
  std::unique_ptr<SpinPool> pool_;
  std::vector<Partial> partial_;
  std::uint16_t frame_id_ = 0;
};

const Registrar kSimd{
    "simd", [](const SensorInfo& i, const Config& c) { return std::make_unique<FastPipeline<false>>(i, c); }};
const Registrar kMt{
    "mt", [](const SensorInfo& i, const Config& c) { return std::make_unique<FastPipeline<true>>(i, c); }};

}  // namespace
}  // namespace avl
