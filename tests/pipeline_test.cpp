// Registry behaviour and the shared braking rule. The rule's numbers are the
// ones derived in docs/budget.md, so a change to Config defaults that breaks
// the budget's arithmetic fails here.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

#include "pipeline.hpp"

namespace avl {
namespace {

TEST(Registry, SpinPipelineIsRegistered) {
  const auto names = pipeline_names();
  EXPECT_NE(std::find(names.begin(), names.end(), "spin"), names.end());
}

TEST(Registry, UnknownNameThrows) {
  const SensorInfo info;
  EXPECT_THROW((void)make_pipeline("no-such-rung", info, Config{}), std::runtime_error);
}

TEST(Decide, StoppingDistanceAtFiftyKmhMatchesBudget) {
  // 13.9 m/s: 0.30 s * 13.9 = 4.17 m, 13.9^2 / 12 = 16.10 m, +1 m margin.
  const Config cfg;
  EXPECT_TRUE(decide(21.2f, cfg).brake);
  EXPECT_FALSE(decide(21.4f, cfg).brake);
}

TEST(Decide, NothingInCorridorNeverBrakes) {
  const Decision d = decide(std::numeric_limits<float>::infinity(), Config{});
  EXPECT_FALSE(d.brake);
  EXPECT_TRUE(std::isinf(d.ttc_s));
}

TEST(Decide, TimeToCollisionIsDistanceOverSpeed) {
  Config cfg;
  cfg.ego_speed_mps = 10.0f;
  EXPECT_FLOAT_EQ(decide(25.0f, cfg).ttc_s, 2.5f);
}

}  // namespace
}  // namespace avl
