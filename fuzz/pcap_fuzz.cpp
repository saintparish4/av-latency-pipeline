// Fuzzes parse_pcap, the only parser of bytes from disk. The invariants in
// pcap.hpp are asserted here: no crash, no sanitizer report, and every
// payload span lies inside the input buffer
#include <cstdint>
#include <cstdlib>
#include <span>

#include "pcap.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::span<const std::uint8_t> in(data, size);
  const avl::PcapResult res = avl::parse_pcap(in);
  for (const auto& d : res.datagrams) {
    if (d.payload.data() < data || d.payload.data() + d.payload.size() > data + size) std::abort();
  }
  return 0;
}
