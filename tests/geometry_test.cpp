// Plane fitting and voxel-grid indexing, the two pieces of math every rung
// shares.
#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <random>

#include "geometry.hpp"

namespace avl {
namespace {

Covariance covariance_of(const std::vector<std::array<double, 3>>& pts) {
  Covariance c;
  for (const auto& p : pts) {
    for (std::size_t a = 0; a < 3; ++a) c.mean[a] += p[a];
  }
  c.n = pts.size();
  for (double& m : c.mean) m /= static_cast<double>(c.n);
  for (const auto& p : pts) {
    const double dx = p[0] - c.mean[0], dy = p[1] - c.mean[1], dz = p[2] - c.mean[2];
    c.xx += dx * dx, c.xy += dx * dy, c.xz += dx * dz, c.yy += dy * dy, c.yz += dy * dz, c.zz += dz * dz;
  }
  return c;
}

TEST(FitPlane, RecoversTiltedNoisyPlane) {
  // z = 0.05 x - 0.02 y - 1.8, with 1 cm noise.
  std::mt19937 rng(7);
  std::uniform_real_distribution<double> ux(0, 40), uy(-10, 10);
  std::normal_distribution<double> noise(0, 0.01);
  std::vector<std::array<double, 3>> pts;
  for (int i = 0; i < 5000; ++i) {
    const double x = ux(rng), y = uy(rng);
    pts.push_back({x, y, 0.05 * x - 0.02 * y - 1.8 + noise(rng)});
  }
  const auto fitted = fit_plane(covariance_of(pts));
  ASSERT_TRUE(fitted.has_value());
  const Plane& pl = fitted.value();
  const double norm = std::sqrt(1 + 0.05 * 0.05 + 0.02 * 0.02);
  EXPECT_NEAR(pl.nx, -0.05 / norm, 1e-3);
  EXPECT_NEAR(pl.ny, 0.02 / norm, 1e-3);
  EXPECT_GT(pl.nz, 0.99f);  // normal points up
  EXPECT_NEAR(pl.height(10.0f, 0.0f, 0.05f * 10 - 1.8f + 0.5f), 0.5 / norm, 5e-3);
}

TEST(FitPlane, FewerThanThreePointsHasNoPlane) {
  EXPECT_FALSE(fit_plane(covariance_of({{0, 0, 0}, {1, 0, 0}})).has_value());
}

TEST(FitPlane, CollinearPointsHaveNoUniqueNormal) {
  EXPECT_FALSE(fit_plane(covariance_of({{0, 0, 0}, {1, 1, 0}, {2, 2, 0}, {3, 3, 0}})).has_value());
}

TEST(GridSpec, DefaultCorridorCoversPlusMinus1p2mAndObstacleHeights) {
  const GridSpec g{Config{}};
  EXPECT_EQ(g.nx, 200);
  EXPECT_EQ(g.ny, 100);
  EXPECT_EQ(g.nh, 12);
  EXPECT_EQ(g.iy_lo, 44);  // voxel [-1.2, -1.0)
  EXPECT_EQ(g.iy_hi, 55);  // voxel [ 1.0,  1.2)
  EXPECT_EQ(g.ih_lo, 1);   // voxel [0.2, 0.4) contains 0.3
  EXPECT_EQ(g.ih_hi, 9);   // voxel [1.8, 2.0)
}

TEST(GridSpec, CellIsMinusOneOutsideAndXMajorInside) {
  const GridSpec g{Config{}};
  EXPECT_EQ(g.cell(-0.01f, 0, 0.5f), -1);
  EXPECT_EQ(g.cell(40.0f, 0, 0.5f), -1);
  EXPECT_EQ(g.cell(1.0f, 0, -0.1f), -1);
  EXPECT_EQ(g.cell(0.1f, -9.9f, 0.1f), 0);
  EXPECT_EQ(g.cell(0.3f, -9.9f, 0.1f), static_cast<std::int64_t>(g.flat(1, 0, 0)));
}

TEST(NearestInCorridor, ReturnsNearFaceOfFirstQualifyingVoxel) {
  const Config cfg;
  const GridSpec g{cfg};
  std::vector<std::uint8_t> grid(g.cells(), 0);
  grid[g.flat(100, 50, 3)] = 1;  // below min_points: ignored
  grid[g.flat(120, 50, 3)] = 2;  // 24.0 m
  grid[g.flat(90, 20, 3)] = 9;   // outside corridor laterally
  grid[g.flat(80, 50, 11)] = 9;  // above obstacle_h_max
  EXPECT_FLOAT_EQ(nearest_in_corridor(grid.data(), g, cfg.min_points_per_voxel), 24.0f);
}

}  // namespace
}  // namespace avl
