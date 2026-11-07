// udp_tx: sends a capture's lidar payloads to host:port on the capture's
// own schedule (scaled by --speed), looping. It exists for the loopback
// smoke test and for machines without tcpreplay; the reported external run
// uses tcpreplay from a second machine (scripts/external_tx.sh), because a
// sender on the same host shares its CPU, cache and clock with the receiver.
//   usage: udp_tx PCAP META [--host 127.0.0.1] [--port 7502] [--speed 1] [--loops 100]
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "pcap.hpp"
#include "sensor.hpp"

int main(int argc, char** argv) try {
  if (argc < 3) {
    std::cerr << "usage: udp_tx PCAP META [--host H] [--port P] [--speed S] [--loops N]\n";
    return 2;
  }
  std::string host = "127.0.0.1";
  int port = 7502;
  double speed = 1.0;
  long loops = 100;
  for (int i = 3; i + 1 < argc; i += 2) {
    const std::string k = argv[i], v = argv[i + 1];
    if (k == "--host")
      host = v;
    else if (k == "--port")
      port = std::stoi(v);
    else if (k == "--speed")
      speed = std::stod(v);
    else if (k == "--loops")
      loops = std::stol(v);
    else
      throw std::runtime_error("unknown option " + k);
  }
  const auto info = avl::load_sensor_info(argv[2]);
  const auto bytes = avl::read_file(argv[1]);
  const auto cap = avl::parse_pcap(bytes);
  std::vector<avl::UdpDatagram> pkts;
  for (const auto& d : cap.datagrams) {
    if (avl::is_lidar_packet(d.payload, info)) pkts.push_back(d);
  }
  if (pkts.size() < 2) throw std::runtime_error("need at least 2 lidar packets");
  const auto span_ns = static_cast<std::int64_t>(pkts.back().ts_ns - pkts.front().ts_ns);
  const std::int64_t loop_ns = span_ns + span_ns / static_cast<std::int64_t>(pkts.size() - 1);

  const int fd = socket(AF_INET, SOCK_DGRAM, 0);
  sockaddr_in dst{};
  dst.sin_family = AF_INET;
  dst.sin_port = htons(static_cast<std::uint16_t>(port));
  if (inet_pton(AF_INET, host.c_str(), &dst.sin_addr) != 1) throw std::runtime_error("bad host " + host);
  const auto t0 = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
  for (long l = 0; l < loops; ++l) {
    for (const auto& d : pkts) {
      const auto off =
          static_cast<double>(l * loop_ns + static_cast<std::int64_t>(d.ts_ns - pkts.front().ts_ns));
      const auto when = t0 + std::chrono::nanoseconds(static_cast<std::int64_t>(off / speed));
      while (std::chrono::steady_clock::now() < when) {
      }
      if (sendto(fd, d.payload.data(), d.payload.size(), 0, reinterpret_cast<const sockaddr*>(&dst),
                 sizeof dst) < 0) {
        throw std::runtime_error(std::string("sendto: ") + std::strerror(errno));
      }
    }
  }
  close(fd);
  return 0;
} catch (const std::exception& e) {
  std::cerr << "udp_tx: " << e.what() << '\n';
  return 1;
}
