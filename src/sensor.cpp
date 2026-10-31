// See sensor.hpp. JSON is parsed once at startup; nlohmann/json never
// appears on the packet path.
#include "sensor.hpp"

#include <cmath>
#include <fstream>
#include <numbers>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace avl {
namespace {

constexpr std::string_view kSupportedProfile = "RNG19_RFL8_SIG16_NIR16";

template <std::size_t N>
std::array<double, N> to_array(const nlohmann::json& j, const char* what) {
  if (!j.is_array() || j.size() != N) throw std::runtime_error(std::string("bad ") + what);
  std::array<double, N> a{};
  for (std::size_t i = 0; i < N; ++i) a[i] = j[i].get<double>();
  return a;
}

}  // namespace

SensorInfo parse_sensor_info(std::string_view text) {
  const auto j = nlohmann::json::parse(text);
  const bool nested = j.contains("beam_intrinsics");
  const auto& fmt = nested ? j.at("lidar_data_format") : j.at("data_format");
  const auto& beams = nested ? j.at("beam_intrinsics") : j;

  const std::string profile = nested ? j.at("config_params").value("udp_profile_lidar", "")
                                     : fmt.value("udp_profile_lidar", "LEGACY");
  if (profile != kSupportedProfile) {
    throw std::runtime_error("unsupported udp_profile_lidar '" + profile + "' (need " +
                             std::string(kSupportedProfile) + ")");
  }

  SensorInfo s;
  s.columns_per_frame = fmt.at("columns_per_frame").get<int>();
  s.columns_per_packet = fmt.at("columns_per_packet").get<int>();
  s.pixels_per_column = fmt.at("pixels_per_column").get<int>();
  s.beam_altitude_deg = beams.at("beam_altitude_angles").get<std::vector<double>>();
  s.beam_azimuth_deg = beams.at("beam_azimuth_angles").get<std::vector<double>>();
  if (beams.contains("beam_to_lidar_transform")) {
    const auto b2l = to_array<16>(beams.at("beam_to_lidar_transform"), "beam_to_lidar_transform");
    s.beam_origin_x_mm = b2l[3];
    s.beam_origin_z_mm = b2l[11];
  } else {
    s.beam_origin_x_mm = beams.at("lidar_origin_to_beam_origin_mm").get<double>();
  }
  const auto& l2s =
      nested ? j.at("lidar_intrinsics").at("lidar_to_sensor_transform") : j.at("lidar_to_sensor_transform");
  s.lidar_to_sensor = to_array<16>(l2s, "lidar_to_sensor_transform");

  const auto rows = static_cast<std::size_t>(s.pixels_per_column);
  if (s.columns_per_frame <= 0 || s.columns_per_packet <= 0 || rows == 0 ||
      s.columns_per_frame % s.columns_per_packet != 0 || s.beam_altitude_deg.size() != rows ||
      s.beam_azimuth_deg.size() != rows) {
    throw std::runtime_error("inconsistent data format in metadata");
  }
  return s;
}

SensorInfo load_sensor_info(const std::string& path) {
  std::ifstream f(path);
  if (!f) throw std::runtime_error("cannot open " + path);
  std::stringstream ss;
  ss << f.rdbuf();
  return parse_sensor_info(ss.str());
}

Sector::Sector(const SensorInfo& info, double sector_deg) {
  const int w = info.columns_per_frame;
  in_.assign(static_cast<std::size_t>(w), 0);
  const auto& t = info.lidar_to_sensor;
  const double half = sector_deg * 0.5;
  for (int m = 0; m < w; ++m) {
    // Encoder angle convention from the Ouster docs: theta = 2*pi*(1 - m/W).
    const double enc = 2.0 * std::numbers::pi * (1.0 - static_cast<double>(m) / w);
    const double lx = std::cos(enc), ly = std::sin(enc);
    const double sx = t[0] * lx + t[1] * ly, sy = t[4] * lx + t[5] * ly;
    const double az = std::atan2(sy, sx) * 180.0 / std::numbers::pi;
    if (sector_deg >= 360.0 || std::abs(az) <= half) {
      in_[static_cast<std::size_t>(m)] = 1;
      ++count_;
    }
  }
  if (count_ == 0) throw std::runtime_error("sector contains no columns");
  // Columns arrive in increasing measurement id, so the sector completes at
  // the member whose successor (circularly) is not a member.
  for (int m = 0; m < w; ++m) {
    if (in_[static_cast<std::size_t>(m)] && !in_[static_cast<std::size_t>((m + 1) % w)]) trigger_ = m;
  }
  if (trigger_ < 0) trigger_ = w - 1;  // full circle
}

}  // namespace avl
