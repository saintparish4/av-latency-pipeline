// pcap parsing: round trips through write_pcap, and every malformed shape
// the fuzzer might find is rejected cleanly instead of read past.
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>

#include "pcap.hpp"

namespace avl {
namespace {

std::uint32_t load_le32(const std::vector<std::uint8_t>& b, std::size_t off) {
  return static_cast<std::uint32_t>(b[off]) | (static_cast<std::uint32_t>(b[off + 1]) << 8) |
         (static_cast<std::uint32_t>(b[off + 2]) << 16) | (static_cast<std::uint32_t>(b[off + 3]) << 24);
}

std::vector<OwnedDatagram> two_datagrams() {
  return {{1'000'000'123, 7502, {1, 2, 3}}, {2'000'000'456, 7503, {9, 8, 7, 6, 5}}};
}

TEST(Pcap, RoundTripPreservesTimestampsPortsAndPayloads) {
  const auto bytes = write_pcap(two_datagrams());
  const auto res = parse_pcap(bytes);
  ASSERT_TRUE(res.ok()) << res.error;
  ASSERT_EQ(res.datagrams.size(), 2u);
  EXPECT_EQ(res.datagrams[0].ts_ns, 1'000'000'123u);
  EXPECT_EQ(res.datagrams[1].dst_port, 7503);
  EXPECT_EQ(std::vector<std::uint8_t>(res.datagrams[1].payload.begin(), res.datagrams[1].payload.end()),
            (std::vector<std::uint8_t>{9, 8, 7, 6, 5}));
  EXPECT_EQ(res.skipped, 0u);
}

TEST(Pcap, ParsesByteSwappedCapture) {
  // A big-endian writer's file: every header word has its bytes reversed.
  auto bytes = write_pcap(two_datagrams());
  const auto swap_word = [&bytes](std::size_t off) {
    const auto first = bytes.begin() + static_cast<std::ptrdiff_t>(off);
    std::reverse(first, first + 4);
  };
  for (const std::size_t off : {0u, 16u, 20u}) swap_word(off);  // magic, snaplen, link type
  for (std::size_t off = 24; off < bytes.size();) {
    const std::size_t incl = load_le32(bytes, off + 8);
    for (const std::size_t word : {0u, 4u, 8u, 12u}) swap_word(off + word);
    off += 16 + incl;
  }
  const auto res = parse_pcap(bytes);
  ASSERT_TRUE(res.ok()) << res.error;
  ASSERT_EQ(res.datagrams.size(), 2u);
  EXPECT_EQ(res.datagrams[0].ts_ns, 1'000'000'123u);
  EXPECT_EQ(res.datagrams[1].ts_ns, 2'000'000'456u);
  EXPECT_EQ(res.datagrams[1].payload.size(), 5u);
}

TEST(Pcap, RejectsShortGlobalHeader) {
  const std::vector<std::uint8_t> bytes(10, 0);
  EXPECT_FALSE(parse_pcap(bytes).ok());
}

TEST(Pcap, RejectsBadMagic) {
  auto bytes = write_pcap(two_datagrams());
  bytes[0] = 0x00;
  EXPECT_EQ(parse_pcap(bytes).error, "bad magic");
}

TEST(Pcap, RejectsTruncatedRecordAtEveryCutPoint) {
  const auto full = write_pcap(two_datagrams());
  for (std::size_t cut = 25; cut < full.size(); ++cut) {
    const std::vector<std::uint8_t> part(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(cut));
    const auto res = parse_pcap(part);
    // Either a clean prefix (cut exactly at a record boundary) or an error.
    if (res.ok()) EXPECT_LT(res.datagrams.size(), 2u) << "cut=" << cut;
  }
}

TEST(Pcap, SkipsNonFirstFragments) {
  auto bytes = write_pcap(two_datagrams());
  // First record's IPv4 flags/fragment-offset field: 24 global + 16 record + 14 eth + 6.
  bytes[24 + 16 + 14 + 6] = 0x00;
  bytes[24 + 16 + 14 + 7] = 0x10;  // fragment offset 16
  const auto res = parse_pcap(bytes);
  ASSERT_TRUE(res.ok());
  EXPECT_EQ(res.datagrams.size(), 1u);
  EXPECT_EQ(res.skipped, 1u);
}

TEST(Pcap, SkipsUdpLengthLargerThanIpPacket) {
  auto bytes = write_pcap(two_datagrams());
  // First record's UDP length: 24 + 16 + 14 + 20 + 4.
  bytes[24 + 16 + 14 + 20 + 4] = 0xff;
  const auto res = parse_pcap(bytes);
  ASSERT_TRUE(res.ok());
  EXPECT_EQ(res.skipped, 1u);
}

TEST(Pcap, ParsesPinnedOusterCapture) {
  const std::string path = std::string(AVL_DATA_DIR) + "/OS-2-128-U1_v2.3.0_1024x10.pcap";
  if (!std::filesystem::exists(path)) GTEST_SKIP() << "run scripts/fetch_data.sh";
  const auto bytes = read_file(path);
  const auto res = parse_pcap(bytes);
  ASSERT_TRUE(res.ok()) << res.error;
  std::size_t lidar = 0;
  for (const auto& d : res.datagrams) lidar += d.dst_port == 7502 ? 1 : 0;
  EXPECT_EQ(lidar, 64u);  // one 1024-column frame at 16 columns per packet
}

}  // namespace

}  // namespace avl
