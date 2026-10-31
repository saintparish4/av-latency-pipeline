// Ouster sensor metadata and the byte layout of one lidar UDP packet.
//
// Only the RNG19_RFL8_SIG16_NIR16 single-return profile is supported; any
// other profile is rejected at load time rather than mis-decoded later.
// Layout (Ouster Sensor Docs, "Lidar Data Format", FW 2.2+), little-endian:
//   packet header 32 B: packet_type u16 @0, frame_id u16 @2, init_id u24 @4
//   per column 12 B:    timestamp_ns u64 @0, measurement_id u16 @8, status u16 @10
//   per pixel 12 B:     range_mm u32 @0 (low 19 bits), reflectivity u8 @4,
//                       signal u16 @6, near_ir u16 @8
//   packet footer 32 B
// Accessors take a payload whose size the caller has already checked with
// is_lidar_packet(); nothing here allocates or throws on the hot path.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace avl {

inline constexpr std::size_t kPacketHeaderBytes = 32;
inline constexpr std::size_t kColumnHeaderBytes = 12;
inline constexpr std::size_t kPixelBytes = 12;
inline constexpr std::size_t kPacketFooterBytes = 32;
inline constexpr std::uint32_t kRangeMask = 0x7ffff;

struct SensorInfo {
  int columns_per_frame = 0;
  int columns_per_packet = 0;
  int pixels_per_column = 0;
  std::vector<double> beam_altitude_deg;
  std::vector<double> beam_azimuth_deg;
  // beam_to_lidar_transform translation, mm: x is the beam-origin offset n,
  // z is a vertical offset that only newer metadata files carry
  double beam_origin_x_mm = 0;
  double beam_origin_z_mm = 0;
  std::array<double, 16> lidar_to_sensor{};  // row-major 4x4, translation in mm

  [[nodiscard]] std::size_t column_bytes() const {
    return kColumnHeaderBytes + static_cast<std::size_t>(pixels_per_column) * kPixelBytes;
  }
  [[nodiscard]] std::size_t lidar_packet_bytes() const {
    return kPacketHeaderBytes + static_cast<std::size_t>(columns_per_packet) * column_bytes() +
           kPacketFooterBytes;
  }
};

// Accepts both the legacy flat metadata (FW 2.x) and the nested
// sensor_info/beam_intrinsics format. Throws std::runtime_error on anything
// unsupported, so a wrong file fails before the first packet
[[nodiscard]] SensorInfo parse_sensor_info(std::string_view json);
[[nodiscard]] SensorInfo load_sensor_info(const std::string& path);

inline std::uint16_t load_u16(const std::uint8_t* p) {
  std::uint16_t v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}
inline std::uint32_t load_u32(const std::uint8_t* p) {
  std::uint32_t v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}
inline std::uint64_t load_u64(const std::uint8_t* p) {
  std::uint64_t v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

struct ColumnHeader {
  std::uint64_t timestamp_ns;
  std::uint16_t measurement_id;
  std::uint16_t status;
  [[nodiscard]] bool valid() const { return (status & 1u) != 0; }
};

class PacketView {
 public:
  PacketView(std::span<const std::uint8_t> payload, const SensorInfo& info)
      : p_(payload.data()), col_bytes_(info.column_bytes()), cols_(info.columns_per_packet) {}

  [[nodiscard]] std::uint16_t frame_id() const { return load_u16(p_ + 2); }
  [[nodiscard]] int columns() const { return cols_; }
  [[nodiscard]] const std::uint8_t* column(int c) const {
    return p_ + kPacketHeaderBytes + static_cast<std::size_t>(c) * col_bytes_;
  }
  [[nodiscard]] ColumnHeader header(int c) const {
    const std::uint8_t* col = column(c);
    return {load_u64(col), load_u16(col + 8), load_u16(col + 10)};
  }
  [[nodiscard]] const std::uint8_t* pixels(int c) const { return column(c) + kColumnHeaderBytes; }

 private:
  const std::uint8_t* p_;
  std::size_t col_bytes_;
  int cols_;
};

[[nodiscard]] inline bool is_lidar_packet(std::span<const std::uint8_t> payload, const SensorInfo& info) {
  return payload.size() == info.lidar_packet_bytes();
}

// The forward sector: which measurement ids are processed, and which one
// completes the sector and therefore triggers a decision. Forward is +x of
// the sensor frame. Membership is decided per column from the encoder angle
// alone (beam azimuth offsets are a few degreees and only blur the edge)
class Sector {
 public:
  Sector(const SensorInfo& info, double sector_deg);
  [[nodiscard]] bool contains(int mid) const {
    return mid >= 0 && mid < static_cast<int>(in_.size()) && in_[static_cast<std::size_t>(mid)];
  }
  [[nodiscard]] int trigger_mid() const { return trigger_; }
  [[nodiscard]] int column_count() const { return count_; }

 private:
  std::vector<std::uint8_t> in_;
  int trigger_ = -1;
  int count_ = 0;
};

}  // namespace avl
