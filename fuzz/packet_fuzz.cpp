// Fuzzes every registered pipeline with arbitrary lidar packets. The input
// is cut into packet-sized chunks and fed in order, so the fuzzer controls
// measurement ids, status bits, frame ids and ranges: out-of-range ids,
// duplicated columns and never-completed sectors are all reachable
//
// A small synthetic sensor ( 4 rows, 64 columns, 16 per packet) keeps packets
// to 1024 bytes so the fuzzer explores structure rather than padding
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

#include "pipeline.hpp"
#include "sensor.hpp"

namespace {

avl::SensorInfo tiny_sensor() {
  avl::SensorInfo s;
  s.columns_per_frame = 64;
  s.columns_per_packet = 16;
  s.pixels_per_column = 4;
  s.beam_altitude_deg = {3.0, 1.0, -1.0, -3.0};
  s.beam_azimuth_deg = {1.0, -1.0, 1.0, -1.0};
  s.beam_origin_x_mm = 15.8;
  s.lidar_to_sensor = {-1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1, 36.18, 0, 0, 0, 1};
  return s;
}

struct Pipelines {
  avl::SensorInfo info = tiny_sensor();
  std::vector<std::unique_ptr<avl::Pipeline>> all;
  Pipelines() {
    avl::Config cfg;
    cfg.spin_us = 0;
    cfg.helpers = 1;
    for (const auto& name : avl::pipeline_names()) all.push_back(avl::make_pipeline(name, info, cfg));
  }
};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  static Pipelines p;  // pipelines persist across inputs, like a long-running receiver
  const std::size_t pkt = p.info.lidar_packet_bytes();
  std::vector<std::uint8_t> buf(pkt);
  for (std::size_t off = 0; off < size; off += pkt) {
    const std::size_t n = std::min(pkt, size - off);
    std::memset(buf.data(), 0, pkt);
    std::memcpy(buf.data(), data + off, n);
    for (auto& pipeline : p.all) (void)pipeline->on_packet(buf);
  }
  return 0;
}
