// See geometry.hpp.
#include "geometry.hpp"

#include <algorithm>
#include <limits>

namespace avl {

std::optional<Plane> fit_plane(const Covariance& c) {
  if (c.n < 3) return std::nullopt;
  double a[3][3] = {{c.xx, c.xy, c.xz}, {c.xy, c.yy, c.yz}, {c.xz, c.yz, c.zz}};
  double v[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  // Cyclic Jacobi: rotate away each off-diagonal entry in turn. 3x3
  // symmetric converges quadratically; 32 sweeps is far beyond what is needed.
  for (int sweep = 0; sweep < 32; ++sweep) {
    const double off = a[0][1] * a[0][1] + a[0][2] * a[0][2] + a[1][2] * a[1][2];
    if (off < 1e-30) break;
    for (int p = 0; p < 2; ++p) {
      for (int q = p + 1; q < 3; ++q) {
        if (a[p][q] == 0.0) continue;
        const double theta = (a[q][q] - a[p][p]) / (2.0 * a[p][q]);
        const double t = (theta >= 0 ? 1.0 : -1.0) / (std::abs(theta) + std::sqrt(theta * theta + 1.0));
        const double cs = 1.0 / std::sqrt(t * t + 1.0), sn = t * cs;
        for (auto& row : a) {  // A <- A J
          const double akp = row[p], akq = row[q];
          row[p] = cs * akp - sn * akq;
          row[q] = sn * akp + cs * akq;
        }
        for (int k = 0; k < 3; ++k) {  // A <- J^T A
          const double apk = a[p][k], aqk = a[q][k];
          a[p][k] = cs * apk - sn * aqk;
          a[q][k] = sn * apk + cs * aqk;
        }
        for (auto& row : v) {  // V <- V J
          const double vkp = row[p], vkq = row[q];
          row[p] = cs * vkp - sn * vkq;
          row[q] = sn * vkp + cs * vkq;
        }
      }
    }
  }
  int best = 0;
  for (int i = 1; i < 3; ++i) {
    if (a[i][i] < a[best][best]) best = i;
  }
  // A line-like set has two near-zero eigenvalues and no unique normal.
  double eig[3] = {a[0][0], a[1][1], a[2][2]};
  std::sort(eig, eig + 3);
  if (eig[1] <= 1e-12 * std::max(eig[2], 1e-300)) return std::nullopt;
  double nx = v[0][best], ny = v[1][best], nz = v[2][best];
  const double norm = std::sqrt(nx * nx + ny * ny + nz * nz);
  const double s = (nz < 0 ? -1.0 : 1.0) / norm;
  nx *= s, ny *= s, nz *= s;
  Plane pl;
  pl.nx = static_cast<float>(nx);
  pl.ny = static_cast<float>(ny);
  pl.nz = static_cast<float>(nz);
  pl.d = static_cast<float>(-(nx * c.mean[0] + ny * c.mean[1] + nz * c.mean[2]));
  return pl;
}

GridSpec::GridSpec(const Config& cfg)
    : x0(cfg.grid_x_min),
      y0(cfg.grid_y_min),
      h0(cfg.grid_h_min),
      inv_voxel(1.0f / cfg.voxel_m),
      voxel(cfg.voxel_m) {
  const double v = cfg.voxel_m;
  nx = static_cast<int>(std::lround((cfg.grid_x_max - cfg.grid_x_min) / v));
  ny = static_cast<int>(std::lround((cfg.grid_y_max - cfg.grid_y_min) / v));
  nh = static_cast<int>(std::lround((cfg.grid_h_max - cfg.grid_h_min) / v));
  // Corridor = every voxel that overlaps |y| <= half_width and
  // h in [h_min, h_max). Computed in double with a small epsilon so an edge
  // that lands exactly on a voxel face is not lost to float rounding.
  constexpr double eps = 1e-6;
  const double hw = cfg.corridor_half_width_m;
  iy_lo = std::max(0, static_cast<int>(std::floor((-hw - cfg.grid_y_min) / v + eps)));
  iy_hi = std::min(ny - 1, static_cast<int>(std::ceil((hw - cfg.grid_y_min) / v - eps)) - 1);
  ih_lo = std::max(0, static_cast<int>(std::floor((cfg.obstacle_h_min_m - cfg.grid_h_min) / v + eps)));
  ih_hi =
      std::min(nh - 1, static_cast<int>(std::ceil((cfg.obstacle_h_max_m - cfg.grid_h_min) / v - eps)) - 1);
}

float nearest_in_corridor(const std::uint8_t* grid, const GridSpec& g, int min_points) {
  for (int ix = 0; ix < g.nx; ++ix) {
    for (int iy = g.iy_lo; iy <= g.iy_hi; ++iy) {
      const std::uint8_t* col = grid + g.flat(ix, iy, 0);
      for (int ih = g.ih_lo; ih <= g.ih_hi; ++ih) {
        if (col[ih] >= min_points) return g.x0 + static_cast<float>(ix) * g.voxel;
      }
    }
  }
  return std::numeric_limits<float>::infinity();
}

}  // namespace avl
