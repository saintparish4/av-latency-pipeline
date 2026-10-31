// Math shared by every pipeline implementation: plane fitting from a
// covariance, and the voxel-grid indexing rule.
//
// Sharing these is what makes the ladder's equivalence tests meaningful:
// rungs may differ in memory layout, vector width and threading, but they all
// feed the same covariance-to-plane solver and the same float expression for
// a voxel index, so any output difference traces back to summation order.
#pragma once

#include <cmath>
#include <cstdint>
#include <optional>

#include "config.hpp"

namespace avl {

struct Plane {
  float nx = 0, ny = 0, nz = 1, d = 0;  // unit normal pointing up; n.p + d = 0
  [[nodiscard]] float height(float x, float y, float z) const { return nx * x + ny * y + nz * z + d; }
};

// Centred second moments of a point set, accumulated in double.
struct Covariance {
  double mean[3] = {0, 0, 0};
  double xx = 0, xy = 0, xz = 0, yy = 0, yz = 0, zz = 0;
  std::uint64_t n = 0;
};

// Normal = eigenvector of the smallest eigenvalue (cyclic Jacobi). Returns
// nullopt for fewer than 3 points or a degenerate (line-like) set.
[[nodiscard]] std::optional<Plane> fit_plane(const Covariance& c);

// Voxel grid over (x forward, y left, h above ground). Cells are x-major so
// the corridor scan walks memory in the same order it walks distance.
struct GridSpec {
  explicit GridSpec(const Config& cfg);

  float x0, y0, h0, inv_voxel, voxel;
  int nx, ny, nh;
  // Inclusive index ranges of the braking corridor (see geometry.cpp).
  int iy_lo, iy_hi, ih_lo, ih_hi;

  [[nodiscard]] std::size_t cells() const {
    return static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny) * static_cast<std::size_t>(nh);
  }
  [[nodiscard]] std::size_t flat(int ix, int iy, int ih) const {
    return (static_cast<std::size_t>(ix) * static_cast<std::size_t>(ny) + static_cast<std::size_t>(iy)) *
               static_cast<std::size_t>(nh) +
           static_cast<std::size_t>(ih);
  }
  // -1 if outside the grid. The float expression floor((v - v0) * inv) is
  // part of the contract: the SIMD rung must reproduce it exactly.
  [[nodiscard]] std::int64_t cell(float x, float y, float h) const {
    const float fx = std::floor((x - x0) * inv_voxel);
    const float fy = std::floor((y - y0) * inv_voxel);
    const float fh = std::floor((h - h0) * inv_voxel);
    if (!(fx >= 0 && fx < static_cast<float>(nx) && fy >= 0 && fy < static_cast<float>(ny) && fh >= 0 &&
          fh < static_cast<float>(nh))) {
      return -1;
    }
    return static_cast<std::int64_t>(flat(static_cast<int>(fx), static_cast<int>(fy), static_cast<int>(fh)));
  }
};

// Nearest x (voxel near face) of a corridor voxel holding >= min_points, or
// +inf. Identical for every rung that uses a uint8 count grid.
[[nodiscard]] float nearest_in_corridor(const std::uint8_t* grid, const GridSpec& g, int min_points);

}  // namespace avl
