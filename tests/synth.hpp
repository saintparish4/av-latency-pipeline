// Synthetic, labelled braking scenes rendered into real Ouster packet bytes.
//
// Each beam of each column is ray-cast against a flat ground plane and a set
// of axis-aligned boxes, using the pipeline's own XyzLut as the ray model, so
// a return the pipeline decodes lands exactly where the scene put it (to the
// 1 mm range quantum). Because the scene is the label, these tests answer
// "is the obstacle still detected?" for every rung on the ladder.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sensor.hpp"
#include "xyz.hpp"

namespace avl::synth {

struct Box {
  float x0, x1, y0, y1, z0, z1;  // sensor frame, metres
};

struct Scene {
  std::string name;
  float ground_z = -1.8f;  // sensor mounted 1.8m above a flat road
  std::vector<Box> boxes;
  bool expect_brake = false;
  float expect_nearest_m = -1;  // < 0: nothing in the corridor
};

// One frame of lidar packet payloads, in measurement-id order.
[[nodiscard]] std::vector<std::vector<std::uint8_t>> render(const SensorInfo& info, const XyzLut& lut,
                                                            const Scene& scene, std::uint16_t frame_id);

// The labelled scene set with expectations computed from Config defaults
[[nodiscard]] std::vector<Scene> braking_scenes();
}  // namespace avl::synth
