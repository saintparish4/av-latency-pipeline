// The XYZ lookup table against independent closed-form checks. The full
// per-pixel comparison with the Ouster SDK lives in
// analysis/diff_reference.py; these are the fast invariants run every build.
#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <numbers>

#include "xyz.hpp"

namespace avl {
namespace {

SensorInfo simple_sensor() {
  SensorInfo s;
  s.columns_per_frame = 8;
  s.columns_per_packet = 4;
  s.pixels_per_column = 2;
  s.beam_altitude_deg = {10.0, -10.0};
  s.beam_azimuth_deg = {0.0, 0.0};
  s.lidar_to_sensor = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  return s;
}

TEST(XyzLut, ZeroBeamOffsetGivesPureSphericalProjection) {
  const XyzLut lut(simple_sensor());
  // mid 2 of 8: encoder angle 2*pi*(1 - 2/8) = 270 deg -> pointing -y.
  const std::size_t i = lut.index(2, 0);
  const float r = 10'000.0f;  // 10 m in mm
  const double c10 = std::cos(10.0 * std::numbers::pi / 180.0);
  EXPECT_NEAR(r * lut.dx[i] + lut.ox[i], 0.0, 1e-5);
  EXPECT_NEAR(r * lut.dy[i] + lut.oy[i], -10.0 * c10, 1e-5);
  EXPECT_NEAR(r * lut.dz[i] + lut.oz[i], 10.0 * std::sin(10.0 * std::numbers::pi / 180.0), 1e-5);
}

TEST(XyzLut, BeamOriginOffsetShiftsPointsByN) {
  SensorInfo s = simple_sensor();
  s.beam_origin_x_mm = 15.0;
  const XyzLut lut(s);
  // mid 0, encoder angle 0: with r == n the point is the beam origin itself.
  const std::size_t i = lut.index(0, 0);
  EXPECT_NEAR(15.0f * lut.dx[i] + lut.ox[i], 0.015, 1e-6);
  EXPECT_NEAR(15.0f * lut.dz[i] + lut.oz[i], 0.0, 1e-6);
}

TEST(XyzLut, SensorTransformRotatesAndTranslates) {
  SensorInfo s = simple_sensor();
  s.lidar_to_sensor = {-1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1, 36.18, 0, 0, 0, 1};
  const XyzLut lut(s);
  const std::size_t i = lut.index(0, 1);  // encoder 0, beam pointing down 10 deg
  const float r = 5'000.0f;
  EXPECT_LT(r * lut.dx[i] + lut.ox[i], 0.0f);  // lidar +x is sensor -x
  EXPECT_NEAR(r * lut.dz[i] + lut.oz[i], -5.0 * std::sin(10.0 * std::numbers::pi / 180.0) + 0.03618, 1e-5);
}

TEST(XyzLut, ForEachReturnSkipsZeroRangeInvalidColumnsAndBadIds) {
  const SensorInfo s = simple_sensor();
  const XyzLut lut(s);
  std::vector<std::uint8_t> pkt(s.lidar_packet_bytes(), 0);
  auto col = [&](int c) { return kPacketHeaderBytes + static_cast<std::size_t>(c) * s.column_bytes(); };
  const std::uint16_t ok = 1, bad_id = 99;
  const std::uint32_t range = 1000;
  for (int c = 0; c < 4; ++c) {
    const auto mid = static_cast<std::uint16_t>(c);
    std::memcpy(&pkt[col(c) + 8], c == 3 ? &bad_id : &mid, 2);
    if (c != 1) std::memcpy(&pkt[col(c) + 10], &ok, 2);         // column 1 invalid
    std::memcpy(&pkt[col(c) + kColumnHeaderBytes], &range, 4);  // row 0 only
  }
  int seen = 0;
  for_each_return(PacketView(pkt, s), lut, [&](int mid, int row, std::uint32_t r, float, float, float) {
    EXPECT_EQ(row, 0);
    EXPECT_EQ(r, 1000u);
    EXPECT_TRUE(mid == 0 || mid == 2);
    ++seen;
  });
  EXPECT_EQ(seen, 2);
}

TEST(XyzLut, PinnedCaptureRangesMatchPointNorms) {
  const std::string meta = std::string(AVL_DATA_DIR) + "/OS-2-128-U1_v2.3.0_1024x10.json";
  if (!std::filesystem::exists(meta)) GTEST_SKIP() << "run scripts/fetch_data.sh";
  const SensorInfo s = load_sensor_info(meta);
  const XyzLut lut(s);
  // |p - origin| must equal the range up to the beam-origin offset n.
  for (int mid : {0, 300, 512, 1023}) {
    for (int row : {0, 64, 127}) {
      const std::size_t i = lut.index(mid, row);
      const double r = 20'000.0;
      const double x = r * lut.dx[i] + lut.ox[i], y = r * lut.dy[i] + lut.oy[i];
      const double z = r * lut.dz[i] + lut.oz[i] - 0.078296;  // sensor-frame z offset
      EXPECT_NEAR(std::sqrt(x * x + y * y + z * z), 20.0, 0.02) << mid << "," << row;
    }
  }
}

}  // namespace
}  // namespace avl
