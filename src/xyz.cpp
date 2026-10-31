// See xyz.hpp. Formula from the Ouster Sensor Docs ("Lidar Range to XYZ"):
//   theta_e = 2*pi*(1 - mid/W)            encoder angle
//   theta_a = -2*pi*azimuth_deg/360       per-beam azimuth offset
//   phi     =  2*pi*altitude_deg/360      per-beam altitude
//   x = (r - n) cos(theta_e + theta_a) cos(phi) + n cos(theta_e)
//   y = (r - n) sin(theta_e + theta_a) cos(phi) + n sin(theta_e)
//   z = (r - n) sin(phi) + beam_origin_z
// then lidar_to_sensor is applied and millimetres become metres.
#include "xyz.hpp"

#include <cmath>
#include <numbers>

namespace avl {

XyzLut::XyzLut(const SensorInfo& info) : rows_(info.pixels_per_column), cols_(info.columns_per_frame) {
  const int w = cols_;
  const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(rows_);
  for (auto* v : {&dx, &dy, &dz, &ox, &oy, &oz}) v->resize(n);
  const auto& t = info.lidar_to_sensor;
  const double bn = info.beam_origin_x_mm;
  const double pi2 = 2.0 * std::numbers::pi;
  for (int m = 0; m < w; ++m) {
    const double enc = pi2 * (1.0 - static_cast<double>(m) / w);
    for (int row = 0; row < rows_; ++row) {
      const auto r = static_cast<std::size_t>(row);
      const double az = -pi2 * info.beam_azimuth_deg[r] / 360.0;
      const double alt = pi2 * info.beam_altitude_deg[r] / 360.0;
      // Lidar-frame unit direction and the offset term (value at r = 0).
      const double ux = std::cos(enc + az) * std::cos(alt);
      const double uy = std::sin(enc + az) * std::cos(alt);
      const double uz = std::sin(alt);
      const double px = bn * std::cos(enc) - bn * ux;
      const double py = bn * std::sin(enc) - bn * uy;
      const double pz = info.beam_origin_z_mm - bn * uz;
      const std::size_t i = index(m, row);
      // Rotate direction; rotate and translate offset; mm -> m.
      dx[i] = static_cast<float>((t[0] * ux + t[1] * uy + t[2] * uz) * 1e-3);
      dy[i] = static_cast<float>((t[4] * ux + t[5] * uy + t[6] * uz) * 1e-3);
      dz[i] = static_cast<float>((t[8] * ux + t[9] * uy + t[10] * uz) * 1e-3);
      ox[i] = static_cast<float>((t[0] * px + t[1] * py + t[2] * pz + t[3]) * 1e-3);
      oy[i] = static_cast<float>((t[4] * px + t[5] * py + t[6] * pz + t[7]) * 1e-3);
      oz[i] = static_cast<float>((t[8] * px + t[9] * py + t[10] * pz + t[11]) * 1e-3);
    }
  }
}

}  // namespace avl
