// See pipeline.hpp. The registry is a function-local static so registration
// from other translation units' static initializers is order-safe.
#include "pipeline.hpp"

#include <limits>
#include <map>
#include <stdexcept>
#include <string>

namespace avl {
namespace {

std::map<std::string, PipelineFactory, std::less<>>& registry() {
  static std::map<std::string, PipelineFactory, std::less<>> r;
  return r;
}

}  // namespace

Registrar::Registrar(std::string name, PipelineFactory factory) {
  registry().emplace(std::move(name), std::move(factory));
}

std::unique_ptr<Pipeline> make_pipeline(std::string_view name, const SensorInfo& info, const Config& cfg) {
  const auto it = registry().find(name);
  if (it == registry().end()) throw std::runtime_error("unknown pipeline: " + std::string(name));
  return it->second(info, cfg);
}

std::vector<std::string> pipeline_names() {
  std::vector<std::string> names;
  for (const auto& [name, _] : registry()) names.push_back(name);
  return names;
}

Decision decide(float nearest_m, const Config& cfg) {
  Decision d;
  d.nearest_m = nearest_m;
  const float v = cfg.ego_speed_mps;
  d.ttc_s = v > 0 ? nearest_m / v : std::numeric_limits<float>::infinity();
  // Distance covered before the brakes bite, plus the braking distance itself.
  const float needed = v * cfg.system_delay_s + v * v / (2.0f * cfg.decel_mps2) + cfg.stop_margin_m;
  d.brake = nearest_m <= needed;
  return d;
}

}  // namespace avl
