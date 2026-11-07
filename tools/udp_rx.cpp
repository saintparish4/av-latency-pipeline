// udp_rx: the pipeline behind a real UDP socket, timed from outside the
// process. Every datagram carries kernel timestamps (SO_TIMESTAMPING); for
// each decision three latencies are recorded, all ending at the moment the
// decision is returned (CLOCK_REALTIME):
//   wire    NIC hardware receive timestamp of the trigger datagram
//   kernel  kernel software receive timestamp
//   user    when recvmsg() returned it to this process
// wire - kernel is interrupt/softirq/reassembly delay, kernel - user is
// socket queueing and wake-up, and user is what replay measures in-process.
// The hardware clock must be disciplined to CLOCK_REALTIME first, e.g.
//   phc2sys -s CLOCK_REALTIME -c <iface> -O 0 -m
// otherwise "wire" is meaningless (scripts/external_rx.sh does this).
//
// Each source becomes one CSV row in replay's schema (label-wire, ...), so
// analysis/compare.py can compare in-process and external numbers directly.
#include <arpa/inet.h>
#include <hdr/hdr_histogram.h>
#include <linux/errqueue.h>
#include <linux/net_tstamp.h>
#include <netinet/in.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "pipeline.hpp"
#include "sensor.hpp"

namespace {

std::int64_t to_ns(const timespec& t) { return std::int64_t{t.tv_sec} * 1'000'000'000 + t.tv_nsec; }

std::int64_t now_realtime() {
  timespec t;
  clock_gettime(CLOCK_REALTIME, &t);
  return to_ns(t);
}

std::map<std::string, std::string> parse_args(int argc, char** argv) {
  std::map<std::string, std::string> kv;
  for (int i = 1; i < argc; ++i) {
    std::string k = argv[i];
    if (!k.starts_with("--")) throw std::runtime_error("unexpected argument " + k);
    k = k.substr(2);
    if (i + 1 < argc && !std::string(argv[i + 1]).starts_with("--")) {
      kv[k] = argv[++i];
    } else {
      kv[k] = "1";
    }
  }
  return kv;
}

struct Source {
  const char* name;
  hdr_histogram* hist = nullptr;
  std::int64_t missing = 0;  // decisions whose datagram lacked this timestamp
};

}  // namespace

int main(int argc, char** argv) try {
  auto args = parse_args(argc, argv);
  auto get = [&](const std::string& k, const std::string& d) { return args.count(k) ? args[k] : d; };
  if (!args.count("meta")) {
    std::cerr << "usage: udp_rx --meta J [--port 7502] [--impl simd] [--decisions 5000] [--warmup 200]\n"
                 "              [--cpu N] [--fifo PRIO] [--spin] [--csv out.csv] [--label L] [--log f]\n";
    return 2;
  }
  const auto info = avl::load_sensor_info(args["meta"]);
  const std::string impl = get("impl", "simd");
  const long decisions = std::stol(get("decisions", "5000"));
  const long warmup = std::stol(get("warmup", "200"));
  const bool spin = args.count("spin") != 0;
  avl::Config cfg;
  cfg.helpers = std::stoi(get("helpers", "1"));
  cfg.first_helper_cpu = std::stoi(get("helper-cpu", "-1"));

  const int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) throw std::runtime_error(std::string("socket: ") + std::strerror(errno));
  const int rcvbuf = 8 << 20;
  setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
  const int flags = SOF_TIMESTAMPING_RX_HARDWARE | SOF_TIMESTAMPING_RAW_HARDWARE |
                    SOF_TIMESTAMPING_RX_SOFTWARE | SOF_TIMESTAMPING_SOFTWARE;
  if (setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPING, &flags, sizeof flags) != 0) {
    throw std::runtime_error(std::string("SO_TIMESTAMPING: ") + std::strerror(errno));
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<std::uint16_t>(std::stoi(get("port", "7502"))));
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
    throw std::runtime_error(std::string("bind: ") + std::strerror(errno));
  }

  // Same order as replay: real-time setup before the pipeline creates threads.
  std::string rt = mlockall(MCL_CURRENT | MCL_FUTURE) == 0 ? "mlock" : "nomlock";
  if (args.count("cpu")) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(std::stoi(args["cpu"]), &set);
    rt += sched_setaffinity(0, sizeof set, &set) == 0 ? "+pin" : "+nopin";
  }
  if (args.count("fifo")) {
    sched_param sp{};
    sp.sched_priority = std::stoi(args["fifo"]);
    rt += sched_setscheduler(0, SCHED_FIFO, &sp) == 0 ? "+fifo" : "+nofifo";
  }
  auto pipeline = avl::make_pipeline(impl, info, cfg);

  Source src[3] = {{"wire"}, {"kernel"}, {"user"}};
  for (auto& s : src) hdr_init(1, 10'000'000'000LL, 3, &s.hist);
  FILE* log = args.count("log") ? std::fopen(args["log"].c_str(), "w") : nullptr;
  if (log) std::fprintf(log, "frame_id,wire_ns,kernel_ns,user_ns,decision_ns,brake\n");

  std::vector<std::uint8_t> buf(65536);
  alignas(cmsghdr) char ctrl[512];
  long seen = 0, truncated = 0, other = 0;
  std::fprintf(stderr, "udp_rx: %s listening on port %s (%s)\n", impl.c_str(), get("port", "7502").c_str(),
               rt.c_str());
  while (seen < warmup + decisions) {
    iovec iov{buf.data(), buf.size()};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctrl;
    msg.msg_controllen = sizeof ctrl;
    const ssize_t n = recvmsg(fd, &msg, spin ? MSG_DONTWAIT : 0);
    if (n < 0) {
      if (errno == EAGAIN || errno == EINTR) continue;
      throw std::runtime_error(std::string("recvmsg: ") + std::strerror(errno));
    }
    const std::int64_t user_ns = now_realtime();
    if (msg.msg_flags & MSG_TRUNC) {
      ++truncated;
      continue;
    }
    std::int64_t wire_ns = 0, kernel_ns = 0;
    for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
      if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_TIMESTAMPING) {
        scm_timestamping ts;
        std::memcpy(&ts, CMSG_DATA(c), sizeof ts);
        kernel_ns = to_ns(ts.ts[0]);
        wire_ns = to_ns(ts.ts[2]);
      }
    }
    const std::span<const std::uint8_t> payload(buf.data(), static_cast<std::size_t>(n));
    if (!avl::is_lidar_packet(payload, info)) {
      ++other;
      continue;
    }
    const auto decided = pipeline->on_packet(payload);
    if (!decided) continue;
    const avl::Decision& d = *decided;
    const std::int64_t done = now_realtime();
    if (seen++ < warmup) continue;
    const std::int64_t starts[3] = {wire_ns, kernel_ns, user_ns};
    for (int i = 0; i < 3; ++i) {
      if (starts[i] == 0) {
        ++src[i].missing;
      } else {
        hdr_record_value(src[i].hist, done - starts[i]);
      }
    }
    if (log) {
      std::fprintf(log, "%u,%lld,%lld,%lld,%lld,%d\n", avl::PacketView(payload, info).frame_id(),
                   static_cast<long long>(wire_ns), static_cast<long long>(kernel_ns),
                   static_cast<long long>(user_ns), static_cast<long long>(done), d.brake ? 1 : 0);
    }
  }
  if (log) std::fclose(log);

  const std::string csv = get("csv", "");
  FILE* out = nullptr;
  if (!csv.empty()) {
    const bool fresh = !std::filesystem::exists(csv) || std::filesystem::file_size(csv) == 0;
    out = std::fopen(csv.c_str(), "a");
    if (!out) throw std::runtime_error("cannot open " + csv);
    if (fresh) {
      std::fprintf(out,
                   "label,impl,seed,env_pad,link_seed,speed,rt,decisions,brakes,behind,"
                   "mean_ns,p50_ns,p90_ns,p99_ns,p999_ns,max_ns\n");
    }
  }
  for (auto& s : src) {
    hdr_histogram* h = s.hist;
    const long recorded = static_cast<long>(h->total_count);
    std::printf(
        "%-6s n=%ld missing=%lld p50=%.1fus p99=%.1fus p99.9=%.1fus max=%.1fus\n", s.name, recorded,
        static_cast<long long>(s.missing), static_cast<double>(hdr_value_at_percentile(h, 50.0)) / 1e3,
        static_cast<double>(hdr_value_at_percentile(h, 99.0)) / 1e3,
        static_cast<double>(hdr_value_at_percentile(h, 99.9)) / 1e3, static_cast<double>(hdr_max(h)) / 1e3);
    if (out && recorded > 0) {
      std::fprintf(out, "%s-%s,%s,0,0,%d,0,%s,%ld,0,0,%.0f,%lld,%lld,%lld,%lld,%lld\n",
                   get("label", impl).c_str(), s.name, impl.c_str(), AVL_LINK_SEED, rt.c_str(), recorded,
                   hdr_mean(h), static_cast<long long>(hdr_value_at_percentile(h, 50.0)),
                   static_cast<long long>(hdr_value_at_percentile(h, 90.0)),
                   static_cast<long long>(hdr_value_at_percentile(h, 99.0)),
                   static_cast<long long>(hdr_value_at_percentile(h, 99.9)),
                   static_cast<long long>(hdr_max(h)));
    }
    hdr_close(h);
  }
  if (out) std::fclose(out);
  std::printf("truncated=%ld non_lidar=%ld\n", truncated, other);
  close(fd);
  return 0;
} catch (const std::exception& e) {
  std::cerr << "udp_rx: " << e.what() << '\n';
  return 1;
}
