// Metadata parsing in both Ouster JSON layouts, profile rejection, and the
// forward-sector geometry that decides when a decision fires.
#include <gtest/gtest.h>

#include <filesystem>

#include "sensor.hpp"

namespace avl {
namespace {

const std::string kOs2 = std::string(AVL_DATA_DIR) + "/OS-2-128-U1_v2.3.0_1024x10.json";

std::string nested_json(const std::string& profile) {
  std::string alt, az;
  for (int i = 0; i < 4; ++i) {
    alt += (i ? "," : "") + std::to_string(10 - 5 * i);
    az += (i ? "," : "") + std::string("0");
  }
  return R"({"beam_intrinsics":{"beam_altitude_angles":[)" + alt + R"(],"beam_azimuth_angles":[)" + az +
         R"(],"beam_to_lidar_transform":[1,0,0,15.8,0,1,0,0,0,0,1,2.5,0,0,0,1],
          "lidar_origin_to_beam_origin_mm":15.8},
          "lidar_intrinsics":{"lidar_to_sensor_transform":[-1,0,0,0,0,-1,0,0,0,0,1,36.18,0,0,0,1]},
          "lidar_data_format":{"columns_per_frame":512,"columns_per_packet":16,"pixels_per_column":4},
          "config_params":{"udp_profile_lidar":")" +
         profile + R"("}})";
}

TEST(SensorInfo, ParsesNestedFormatIncludingBeamZOffset) {
  const SensorInfo s = parse_sensor_info(nested_json("RNG19_RFL8_SIG16_NIR16"));
  EXPECT_EQ(s.columns_per_frame, 512);
  EXPECT_EQ(s.pixels_per_column, 4);
  EXPECT_DOUBLE_EQ(s.beam_origin_x_mm, 15.8);
  EXPECT_DOUBLE_EQ(s.beam_origin_z_mm, 2.5);
  EXPECT_DOUBLE_EQ(s.lidar_to_sensor[11], 36.18);
  EXPECT_EQ(s.lidar_packet_bytes(), 32 + 16 * (12 + 4 * 12) + 32u);
}

TEST(SensorInfo, RejectsUnsupportedProfileByName) {
  try {
    (void)parse_sensor_info(nested_json("RNG19_RFL8_SIG16_NIR16_DUAL"));
    FAIL() << "expected rejection";
  } catch (const std::runtime_error& e) {
    EXPECT_NE(std::string(e.what()).find("RNG19_RFL8_SIG16_NIR16_DUAL"), std::string::npos);
  }
}

TEST(SensorInfo, ParsesLegacyFlatFormatFromPinnedCapture) {
  if (!std::filesystem::exists(kOs2)) GTEST_SKIP() << "run scripts/fetch_data.sh";
  const SensorInfo s = load_sensor_info(kOs2);
  EXPECT_EQ(s.columns_per_frame, 1024);
  EXPECT_EQ(s.pixels_per_column, 128);
  EXPECT_EQ(s.lidar_packet_bytes(), 24832u);
  EXPECT_DOUBLE_EQ(s.beam_origin_x_mm, 13.762);
}

TEST(Sector, SixtyDegreesForwardIsCentredOnSensorPlusX) {
  const SensorInfo s = parse_sensor_info(nested_json("RNG19_RFL8_SIG16_NIR16"));
  const Sector sec(s, 60.0);
  // lidar_to_sensor rotates 180 deg about z, so sensor +x is encoder angle
  // pi, i.e. measurement id W/2.
  EXPECT_TRUE(sec.contains(256));
  EXPECT_FALSE(sec.contains(0));
  EXPECT_NEAR(sec.column_count(), 512.0 / 6.0, 2);
  EXPECT_GT(sec.trigger_mid(), 256);
  EXPECT_TRUE(sec.contains(sec.trigger_mid()));
  EXPECT_FALSE(sec.contains(sec.trigger_mid() + 1));
}

TEST(Sector, FullCircleTriggersOnLastColumn) {
  const SensorInfo s = parse_sensor_info(nested_json("RNG19_RFL8_SIG16_NIR16"));
  const Sector sec(s, 360.0);
  EXPECT_EQ(sec.column_count(), 512);
  EXPECT_EQ(sec.trigger_mid(), 511);
}

TEST(Sector, OutOfRangeMeasurementIdIsNotContained) {
  const SensorInfo s = parse_sensor_info(nested_json("RNG19_RFL8_SIG16_NIR16"));
  const Sector sec(s, 360.0);
  EXPECT_FALSE(sec.contains(512));
  EXPECT_FALSE(sec.contains(-1));
}

}  // namespace

}  // namespace avl
