// Vector kernels for the "simd" and "mt" rungs, written once with Google
// Highway so the same source targets AVX2 here and NEON/SVE on an aarch64
// vehicle computer.
//
// Every kernel works on a half-open range [b, e) of struct-of-arrays point
// buffers. Callers guarantee: b and e are multiples of kBlock; buffers hold at
// least e elements; lanes in [n, e) are zero with weight 0, so padding can be
// processed without a scalar tail. Ground membership is a float weight w in
// {0, 1} rather than a bool so it can be multiplied straight into the sums.
//
// Bit-exactness contract: decode, classify and cells use the same float
// operations in the same order as the scalar rungs (no FMA), so their outputs
// are bit-identical to naive. Only the covariance sums reorder additions, and
// tests/equivalence_test.cpp bounds the effect of that.
#pragma once

#include <cstddef>
#include <cstdint>

#include "geometry.hpp"
#include "xyz.hpp"

namespace avl::simd {

inline constexpr std::size_t kBlock = 16;  // >= lanes of any target we build for
[[nodiscard]] inline std::size_t round_up(std::size_t n) { return (n + kBlock - 1) / kBlock * kBlock; }

struct Roi {
  float x_min, x_max, y_abs;
};

// Decodes one valid column, appending returns that pass the ROI at x/y/z[n].
// Returns the new count. Output buffers need kBlock elements of slack.
std::size_t decode_column(const std::uint8_t* pixels, const XyzLut& lut, int mid, const Roi& roi, float* x,
                          float* y, float* z, std::size_t n);

struct Sums {  // weighted first moments
  double w = 0, x = 0, y = 0, z = 0;
};
struct Products {  // weighted centred second moments
  double xx = 0, xy = 0, xz = 0, yy = 0, yz = 0, zz = 0;
};

void seed(const float* z, float below, float* w, std::size_t b, std::size_t e);
[[nodiscard]] Sums sums(const float* x, const float* y, const float* z, const float* w, std::size_t b,
                        std::size_t e);
[[nodiscard]] Products products(const float* x, const float* y, const float* z, const float* w,
                                const double mean[3], std::size_t b, std::size_t e);
void classify(const Plane& p, float thresh, const float* x, const float* y, const float* z, float* h,
              float* w, std::size_t b, std::size_t e);
// cell[i] = GridSpec::cell(x, y, h) for non-ground points, -1 otherwise.
void cells(const GridSpec& g, const float* x, const float* y, const float* h, const float* w,
           std::int32_t* cell, std::size_t b, std::size_t e);

}  // namespace avl::simd
