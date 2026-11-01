// Every ladder rung against the naive baseline on the real capture, for
// several sector widths. Scalar rungs must match bit for bit; vector rungs
// reorder the covariance sums, so they may move a handful of points that sit
// within float rounding of the 0.2 m ground threshold, and nothing else.
#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>

#include "pcap.hpp"
#include "pipeline.hpp"

namespace avl {
namespace {

const std::string kCap = std::string(AVL_DATA_DIR) + "/OS-2-128-U1_v2.3.0_1024x10";

std::vector<Decision> run(const std::string& impl, const SensorInfo& info, const PcapResult& cap,
                          double sector_deg, int loops) {
  Config cfg;
  cfg.sector_deg = sector_deg;
  auto p = make_pipeline(impl, info, cfg);
  std::vector<Decision> out;
  for (int l = 0; l < loops; ++l) {
    for (const auto& d : cap.datagrams) {
      if (auto dec = p->on_packet(d.payload)) out.push_back(*dec);
    }
  }
  return out;
}

class EquivalenceTest : public ::testing::TestWithParam<std::string> {};

TEST_P(EquivalenceTest, MatchesNaiveOnPinnedCapture) {
  if (!std::filesystem::exists(kCap + ".pcap")) GTEST_SKIP() << "run scripts/fetch_data.sh";
  const auto bytes = read_file(kCap + ".pcap");
  const auto cap = parse_pcap(bytes);
  const auto info = load_sensor_info(kCap + ".json");
  const bool scalar = GetParam() == "nth" || GetParam() == "soa";
  for (double sector : {30.0, 60.0, 120.0, 360.0}) {
    const auto ref = run("naive", info, cap, sector, 3);
    const auto got = run(GetParam(), info, cap, sector, 3);
    ASSERT_EQ(ref.size(), got.size()) << sector;
    for (std::size_t i = 0; i < ref.size(); ++i) {
      EXPECT_EQ(got[i].brake, ref[i].brake) << sector;
      EXPECT_EQ(got[i].nearest_m, ref[i].nearest_m) << sector;
      EXPECT_EQ(got[i].points, ref[i].points) << sector;
      if (scalar) {
        EXPECT_EQ(got[i].ground, ref[i].ground) << sector;
        EXPECT_EQ(got[i].occupied, ref[i].occupied) << sector;
      } else {
        EXPECT_NEAR(got[i].ground, ref[i].ground, 1.0 + ref[i].points / 1000.0) << sector;
        EXPECT_NEAR(got[i].occupied, ref[i].occupied, 1.0 + ref[i].occupied / 200.0) << sector;
      }
    }
  }
}

std::vector<std::string> rungs() {
  std::vector<std::string> out;
  for (const auto& n : pipeline_names()) {
    if (n != "spin" && n != "naive") out.push_back(n);
  }
  return out;
}

INSTANTIATE_TEST_SUITE_P(Rungs, EquivalenceTest, ::testing::ValuesIn(rungs()),
                         [](const auto& info) { return info.param; });
// Before rung 1 exists there is nothing to compare; that is not a failure.
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(EquivalenceTest);

}  // namespace
}  // namespace avl
