// The one interface every implementation on the optimization ladder
// satisfies, plus a name -> factory registry.
//
// Contract: on_packet() is called once per lidar UDP payload, in arrival
// order, on one thread. It returns a Decision exactly when the packet
// completes the forward sector, and std::nullopt otherwise. After warm-up it
// must not allocate (the harness counts on steady-state behaviour) and it
// must never block on I/O.
//
// Implementations self-register from their own .cpp through a static
// Registrar, so adding a rung to the ladder never edits the driver. This
// relies on src/ being an OBJECT library: a static archive would let the
// linker drop the unreferenced registration objects.
#pragma once

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "config.hpp"
#include "sensor.hpp"

namespace avl {

struct Decision {
  bool brake = false;
  float nearest_m = std::numeric_limits<float>::infinity();  // corridor obstacle, voxel near face
  float ttc_s = std::numeric_limits<float>::infinity();      // nearest_m / ego speed
  // Diagnostics so tests and differential checks can see inside a decision
  std::uint32_t points = 0;    // returns kept after the ROI filter
  std::uint32_t ground = 0;    // of those, labelled ground
  std::uint32_t occupied = 0;  // voxels with >= min_points_per_voxel
};

// Internals of the most recent decision, recorded only when Config::trace is
// set. Differential tests and tools/avl_dump read it; measured runs never
// enable it, so it costs one predicatable branch.
struct Trace {
  std::vector<float> x, y, z, h;     // ROI-filtered sector points, sensor frame
  std::vector<std::uint8_t> ground;  // 1 = ground
  float plane[4] = {0, 0, 1, 0};     // nx, ny, nz, d
  std::vector<std::uint8_t> grid;    // voxel counts, GridSpec layout
};

class Pipeline {
 public:
  virtual ~Pipeline() = default;
  virtual std::optional<Decision> on_packet(std::span<const std::uint8_t> payload) = 0;
  [[nodiscard]] virtual const Trace* trace() const { return nullptr; }
};

using PipelineFactory = std::function<std::unique_ptr<Pipeline>(const SensorInfo&, const Config&)>;

[[nodiscard]] std::unique_ptr<Pipeline> make_pipeline(std::string_view name, const SensorInfo& info,
                                                      const Config& cfg);
[[nodiscard]] std::vector<std::string> pipeline_names();

struct Registrar {
  Registrar(std::string name, PipelineFactory factory);
};

// Braking rule shared by every implementation so they can only differ in how
// they find nearest_,, never in what they do with it
[[nodiscard]] Decision decide(float nearest_m, const Config& cfg);

};  // namespace avl
