// "spin": a pipeline with a known, fixed cost (Config::spin_us of busy-wait
// per decision) and the real sector trigger. It exists to validate the
// harness before any real code is measured: if the harness cannot report
// ~spin_us with tight CIs here, nothing it says about the real pipeline counts.
#include <chrono>

#include "pipeline.hpp"

namespace avl {
namespace {

class SpinPipeline final : public Pipeline {
 public:
  SpinPipeline(const SensorInfo& info, const Config& cfg)
      : info_(info),
        sector_(info, cfg.sector_deg),
        spin_(std::chrono::nanoseconds(static_cast<long long>(cfg.spin_us * 1000.0))) {}

  std::optional<Decision> on_packet(std::span<const std::uint8_t> payload) override {
    if (!is_lidar_packet(payload, info_)) return std::nullopt;
    const PacketView pkt(payload, info_);
    bool triggered = false;
    for (int c = 0; c < pkt.columns(); ++c) {
      triggered |= pkt.header(c).measurement_id == sector_.trigger_mid();
    }
    if (!triggered) return std::nullopt;
    const auto until = std::chrono::steady_clock::now() + spin_;
    while (std::chrono::steady_clock::now() < until) {
    }
    return Decision{};
  }

 private:
  SensorInfo info_;
  Sector sector_;
  std::chrono::nanoseconds spin_;
};

const Registrar kRegister{
    "spin", [](const SensorInfo& i, const Config& c) { return std::make_unique<SpinPipeline>(i, c); }};

}  // namespace
}  // namespace avl
