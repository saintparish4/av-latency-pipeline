// Range -> Cartesian conversion as a precomputed lookup table, the same
// formulation the Ouster SDK uses (point = range_mm * direction + offset), so
// the differential test against the SDK compares like with like.
//
// The table is built once in double precision and stored as float; the per
// return cost is one multiply and one add per axis. Entries are indexed
// [measurement_id * H + row] so a packet's column walks the table linearly.
// Output is the sensor frame in metres: +x forward, +y left, +z up.
#pragma once

#include <cstdint>
#include <vector>

#include "sensor.hpp"

namespace avl {

class XyzLut {
 public:
  explicit XyzLut(const SensorInfo& info);

  [[nodiscard]] int rows() const { return rows_; }
  [[nodiscard]] int columns() const { return cols_; }
  [[nodiscard]] std::size_t index(int mid, int row) const {
    return static_cast<std::size_t>(mid) * static_cast<std::size_t>(rows_) + static_cast<std::size_t>(row);
  }
  // Separate arrays (not xyz triples) so the SIMD rung can load them as lanes.
  std::vector<float> dx, dy, dz;  // direction scaled to metres per millimetre
  std::vector<float> ox, oy, oz;  // offset, metres

 private:
  int rows_;
  int cols_;
};

// Calls f(mid, row, range_mm, x, y, z) for every valid column's non-zero
// return in one packet. Scalar reference used by the naive pipeline, the
// dump tool and the tests; inlined so it costs nothing to share.
template <class F>
inline void for_each_return(const PacketView& pkt, const XyzLut& lut, F&& f) {
  for (int c = 0; c < pkt.columns(); ++c) {
    const ColumnHeader h = pkt.header(c);
    // measurement_id comes off the wire: bound it before it indexes the LUT.
    if (!h.valid() || h.measurement_id >= lut.columns()) continue;
    const std::uint8_t* px = pkt.pixels(c);
    for (int row = 0; row < lut.rows(); ++row, px += kPixelBytes) {
      const std::uint32_t range = load_u32(px) & kRangeMask;
      if (range == 0) continue;
      const std::size_t i = lut.index(h.measurement_id, row);
      const auto r = static_cast<float>(range);
      f(static_cast<int>(h.measurement_id), row, range, r * lut.dx[i] + lut.ox[i], r * lut.dy[i] + lut.oy[i],
        r * lut.dz[i] + lut.oz[i]);
    }
  }
}

}  // namespace avl
