// Every registered pipeline (except the harness's "spin") must make the
// labelled decision on every synthetic braking scene, with the nearest
// obstacle located to within one voxel. New ladder rungs are picked up from
// the registry automatically, so a rung cannot ship without passing this.
#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>

#include "pipeline.hpp"
#include "synth.hpp"

namespace avl {
namespace {

const std::string kMeta = std::string(AVL_DATA_DIR) + "/OS-2-128-U1_v2.3.0_1024x10.json";

std::vector<std::string> real_pipelines() {
  std::vector<std::string> out;
  for (const auto& n : pipeline_names()) {
    if (n != "spin") out.push_back(n);
  }
  return out;
}

class SceneTest : public ::testing::TestWithParam<std::string> {};

TEST_P(SceneTest, LabelledDecisionAndNearestObstacle) {
  if (!std::filesystem::exists(kMeta)) GTEST_SKIP() << "run scripts/fetch_data.sh";
  const SensorInfo info = load_sensor_info(kMeta);
  const XyzLut lut(info);
  const Config cfg;
  for (const auto& scene : synth::braking_scenes()) {
    auto p = make_pipeline(GetParam(), info, cfg);
    std::optional<Decision> d;
    // Two frames: the first proves the pipeline resets between sectors.
    for (std::uint16_t frame = 0; frame < 2; ++frame) {
      for (const auto& pkt : synth::render(info, lut, scene, frame)) {
        if (auto out = p->on_packet(pkt)) d = out;
      }
    }
    ASSERT_TRUE(d.has_value()) << scene.name;
    const Decision& dec = d.value();
    EXPECT_EQ(dec.brake, scene.expect_brake) << scene.name << " nearest=" << dec.nearest_m;
    if (scene.expect_nearest_m < 0) {
      EXPECT_TRUE(std::isinf(dec.nearest_m)) << scene.name << " nearest=" << dec.nearest_m;
    } else {
      EXPECT_NEAR(dec.nearest_m, scene.expect_nearest_m, cfg.voxel_m) << scene.name;
    }
    EXPECT_GT(dec.ground, dec.points / 2) << scene.name << ": a flat road should be mostly ground";
  }
}

INSTANTIATE_TEST_SUITE_P(AllPipelines, SceneTest, ::testing::ValuesIn(real_pipelines()),
                         [](const auto& info) { return info.param; });

}  // namespace
}  // namespace avl
