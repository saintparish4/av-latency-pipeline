// Classic libpcap file parsing and writing, reduced to what replay needs: UDP
// datagrams over Ethernet/IPv4 with their capture timestamps.
//
// This is the only code that touches untrusted bytes from disk, so it is the
// fuzz target. Invariants: parse_pcap never reads outside `bytes`, never
// throws, and every returned payload span points inside `bytes`. Non-UDP
// records, IPv6, and non-first IP fragments are counted and skipped rather
// than treated as errors, because real captures contain them.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace avl {

struct UdpDatagram {
  std::uint64_t ts_ns = 0;  // capture time, nanoseconds since the epoch
  std::uint16_t src_port = 0;
  std::uint16_t dst_port = 0;
  std::span<const std::uint8_t> payload;  // view into the parsed buffer
};

struct PcapResult {
  std::vector<UdpDatagram> datagrams;
  std::size_t skipped = 0;  // records that are valid pcap but not usable UDP
  std::string error;        // empty on success
  [[nodiscard]] bool ok() const { return error.empty(); }
};

[[nodiscard]] PcapResult parse_pcap(std::span<const std::uint8_t> bytes);

// Owned datagram for writing synthetic captures (tests, tcpreplay scenes)
struct OwnedDatagram {
  std::uint64_t ts_ns = 0;
  std::uint16_t dst_port = 7502;
  std::vector<std::uint8_t> payload;
};

// Writes a nanosecond-resolution pcap with Ethernet/IPv4/UDP framing
// Datagrams are written unfragmented, exactly like Ousters own recordings
// tcprewrite fragments them to the wire MTU for tcpreplay
[[nodiscard]] std::vector<std::uint8_t> write_pcap(const std::vector<OwnedDatagram>& datagrams);

[[nodiscard]] std::vector<std::uint8_t> read_file(const std::string& path);
void write_file(const std::string& path, std::span<const std::uint8_t> bytes);

}  // namespace avl
