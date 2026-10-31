// See pcap.hpp. All multi-byte reads go through Reader, which checks bounds
// before every access; there is no pointer arithmetic outside it.
#include "pcap.hpp"

#include <fstream>
#include <iterator>
#include <stdexcept>

namespace avl {
namespace {

constexpr std::uint32_t kMagicMicros = 0xa1b2c3d4;
constexpr std::uint32_t kMagicNanos = 0xa1b23c4d;
constexpr std::uint32_t kLinkEthernet = 1;
constexpr std::uint16_t kEtherIpv4 = 0x0800;
constexpr std::uint16_t kEtherVlan = 0x8100;
constexpr std::uint8_t kProtoUdp = 17;

std::uint32_t bswap32(std::uint32_t v) {
  return (v >> 24) | ((v >> 8) & 0xff00u) | ((v << 8) & 0xff000u) | (v << 24);
}

class Reader {
 public:
  explicit Reader(std::span<const std::uint8_t> b) : b_(b) {}
  [[nodiscard]] bool has(std::size_t off, std::size_t n) const {
    return off <= b_.size() && n <= b_.size() - off;
  }
  [[nodiscard]] std::uint8_t u8(std::size_t off) const { return b_[off]; }
  [[nodiscard]] std::uint16_t be16(std::size_t off) const {
    return static_cast<std::uint16_t>((b_[off] << 8) | b_[off + 1]);
  }
  [[nodiscard]] std::uint32_t le32(std::size_t off) const {
    return static_cast<std::uint32_t>(b_[off]) | (static_cast<std::uint32_t>(b_[off + 1]) << 8) |
           (static_cast<std::uint32_t>(b_[off + 2]) << 16) | (static_cast<std::uint32_t>(b_[off + 3]) << 24);
  }
  [[nodiscard]] std::span<const std::uint8_t> sub(std::size_t off, std::size_t n) const {
    return b_.subspan(off, n);
  }

 private:
  std::span<const std::uint8_t> b_;
};

// Returns false (and leaves `out` untouched) for frames that are not a
// complete first-fragment IPv4/UDP datagram.
bool parse_frame(std::span<const std::uint8_t> frame, UdpDatagram& out) {
  Reader r(frame);
  std::size_t off = 12;
  if (!r.has(off, 2)) return false;
  std::uint16_t ether = r.be16(off);
  off += 2;
  if (ether == kEtherVlan) {
    if (!r.has(off, 4)) return false;
    ether = r.be16(off + 2);
    off += 4;
  }
  if (ether != kEtherIpv4 || !r.has(off, 20)) return false;
  const std::uint8_t ver_ihl = r.u8(off);
  const std::size_t ihl = static_cast<std::size_t>(ver_ihl & 0x0f) * 4;
  if ((ver_ihl >> 4) != 4 || ihl < 20 || !r.has(off, ihl)) return false;
  const std::uint16_t total_len = r.be16(off + 2);
  const std::uint16_t frag = r.be16(off + 6);
  const bool more_fragments = (frag & 0x2000) != 0;
  const bool nonzero_offset = (frag & 0x1fff) != 0;
  if (more_fragments || nonzero_offset || r.u8(off + 9) != kProtoUdp) return false;
  if (total_len < ihl + 8 || !r.has(off, total_len)) return false;
  const std::size_t udp = off + ihl;
  const std::uint16_t udp_len = r.be16(udp + 4);
  if (udp_len < 8 || udp_len > total_len - ihl) return false;
  out.src_port = r.be16(udp);
  out.dst_port = r.be16(udp + 2);
  out.payload = r.sub(udp + 8, udp_len - 8u);
  return true;
}

void put16be(std::vector<std::uint8_t>& v, std::uint16_t x) {
  v.push_back(static_cast<std::uint8_t>(x >> 8));
  v.push_back(static_cast<std::uint8_t>(x));
}
void put32le(std::vector<std::uint8_t>& v, std::uint32_t x) {
  for (int i = 0; i < 4; ++i) v.push_back(static_cast<std::uint8_t>(x >> (8 * i)));
}

}  // namespace

PcapResult parse_pcap(std::span<const std::uint8_t> bytes) {
  PcapResult res;
  Reader r(bytes);
  if (!r.has(0, 24)) {
    res.error = "truncated global header";
    return res;
  }
  std::uint32_t magic = r.le32(0);
  bool swapped = false;
  if (magic != kMagicMicros && magic != kMagicNanos) {
    magic = bswap32(magic);
    swapped = true;
    if (magic != kMagicMicros && magic != kMagicNanos) {
      res.error = "bad magic";
      return res;
    }
  }
  auto rd32 = [&](std::size_t off) { return swapped ? bswap32(r.le32(off)) : r.le32(off); };
  if (rd32(20) != kLinkEthernet) {
    res.error = "unsupported link type";
    return res;
  }
  const std::uint64_t frac_to_ns = magic == kMagicNanos ? 1 : 1000;
  std::size_t off = 24;
  while (off < bytes.size()) {
    if (!r.has(off, 16)) {
      res.error = "truncated record header";
      return res;
    }
    const std::uint64_t sec = rd32(off);
    const std::uint64_t frac = rd32(off + 4);
    const std::size_t incl = rd32(off + 8);
    off += 16;
    if (!r.has(off, incl)) {
      res.error = "truncated record body";
      return res;
    }
    UdpDatagram d;
    if (parse_frame(r.sub(off, incl), d)) {
      d.ts_ns = sec * 1'000'000'000ull + frac * frac_to_ns;
      res.datagrams.push_back(d);
    } else {
      ++res.skipped;
    }
    off += incl;
  }
  return res;
}

std::vector<std::uint8_t> write_pcap(const std::vector<OwnedDatagram>& datagrams) {
  std::vector<std::uint8_t> out;
  put32le(out, kMagicNanos);
  put32le(out, 2 | (4u << 16));  // version 2.4
  put32le(out, 0);               // thiszone
  put32le(out, 0);               // sigfigs
  put32le(out, 65535 * 4);       // snaplen, large enough for unfragmented Ouster packets
  put32le(out, kLinkEthernet);
  for (const auto& d : datagrams) {
    if (d.payload.size() > 65507) throw std::length_error("UDP payload exceeds IPv4 limit");
    const auto udp_len = static_cast<std::uint32_t>(8 + d.payload.size());
    const std::uint32_t ip_len = 20 + udp_len;
    const std::uint32_t frame_len = 14 + ip_len;
    put32le(out, static_cast<std::uint32_t>(d.ts_ns / 1'000'000'000ull));
    put32le(out, static_cast<std::uint32_t>(d.ts_ns % 1'000'000'000ull));
    put32le(out, frame_len);
    put32le(out, frame_len);
    const std::uint8_t macs[12] = {0x02, 0, 0, 0, 0, 2, 0x02, 0, 0, 0, 0, 1};
    out.insert(out.end(), std::begin(macs), std::end(macs));
    put16be(out, kEtherIpv4);
    out.push_back(0x45);  // IPv4, 20-byte header
    out.push_back(0);
    put16be(out, static_cast<std::uint16_t>(ip_len));
    put16be(out, 0);  // id
    put16be(out, 0);  // flags/frag
    out.push_back(64);
    out.push_back(kProtoUdp);
    put16be(out, 0);  // checksum: not validated by any consumer here
    const std::uint8_t ips[8] = {192, 168, 1, 2, 192, 168, 1, 1};
    out.insert(out.end(), std::begin(ips), std::end(ips));
    put16be(out, 7502);
    put16be(out, d.dst_port);
    put16be(out, static_cast<std::uint16_t>(udp_len));
    put16be(out, 0);
    out.insert(out.end(), d.payload.begin(), d.payload.end());
  }
  return out;
}

std::vector<std::uint8_t> read_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("cannot open " + path);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

void write_file(const std::string& path, std::span<const std::uint8_t> bytes) {
  std::ofstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("cannot write " + path);
  f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

}  // namespace avl
