// replay: the in-process latency harness. Replays a pcap's lidar packets on
// a fixed schedule into one Pipeline and records, for every decision, the
// time from the *intended* arrival of the packet that completed the sector to
// the moment the decision was returned.
//
// Measuring from intended (scheduled) arrival rather than from when the loop
// got around to delivering the packet is what avoids coordinated omission: if
// one decision overruns, the packets queued behind it are charged for the
// wait, exactly as they would be on a real receive queue (Tene, "How NOT to
// Measure Latency"). With --speed 0 the loop is closed (no schedule) and the
// numbers are service time only; that mode exists for Coz and perf, never for
// reported latency.
//
// Coz (directive/p3.md) gets two kinds of progress point: a throughput point
// per decision, and a latency pair around the on_packet() call of each
// trigger packet. Which packets trigger is learned on the first loop of the
// capture, so the pair brackets exactly the decision's critical path.
//
// One process = one run = one CSV row. Run-to-run variation (link order,
// environment size, scheduling) is the unit of statistical comparison; see
// analysis/compare.py.
#include <hdr/hdr_histogram.h>
#include <sched.h>
#include <sys/mman.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "pcap.hpp"
#include "pipeline.hpp"
#include "sensor.hpp"

#if AVL_COZ
#include <coz.h>
#else
// Statement-shaped no-ops, so `if (x) COZ_BEGIN(...);` is not an empty if.
#define COZ_PROGRESS_NAMED(name) ((void)0)
#define COZ_BEGIN(name) ((void)0)
#define COZ_END(name) ((void)0)
#endif

namespace {

using Clock = std::chrono::steady_clock;

struct Args {
  std::map<std::string, std::string> kv;
  [[nodiscard]] std::string str(const std::string& k, const std::string& def = "") const {
    const auto it = kv.find(k);
    return it == kv.end() ? def : it->second;
  }
  [[nodiscard]] double num(const std::string& k, double def) const {
    const auto it = kv.find(k);
    return it == kv.end() ? def : std::stod(it->second);
  }
};

Args parse_args(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    std::string k = argv[i];
    if (!k.starts_with("--")) throw std::runtime_error("unexpected argument " + k);
    k = k.substr(2);
    const auto eq = k.find('=');
    if (eq != std::string::npos) {
      a.kv[k.substr(0, eq)] = k.substr(eq + 1);
    } else if (i + 1 < argc && !std::string(argv[i + 1]).starts_with("--")) {
      a.kv[k] = argv[++i];
    } else {
      a.kv[k] = "1";
    }
  }
  return a;
}

// Real-time setup is done in-process so a run cannot silently forget it; each
// step reports failure instead of aborting, and the CSV records what held.
std::string setup_realtime(int cpu, int fifo_prio) {
  std::string state;
  state += mlockall(MCL_CURRENT | MCL_FUTURE) == 0 ? "mlock" : "nomlock";
  if (cpu >= 0) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    state += sched_setaffinity(0, sizeof set, &set) == 0 ? "+pin" : "+nopin";
  }
  if (fifo_prio > 0) {
    sched_param sp{};
    sp.sched_priority = fifo_prio;
    state += sched_setscheduler(0, SCHED_FIFO, &sp) == 0 ? "+fifo" : "+nofifo";
  }
  return state;
}

std::int64_t ns_since(Clock::time_point t0, Clock::time_point t) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(t - t0).count();
}

}  // namespace

int main(int argc, char** argv) try {
  const Args args = parse_args(argc, argv);
  if (args.kv.count("help") || !args.kv.count("pcap") || !args.kv.count("meta")) {
    std::cerr << "usage: replay --pcap F --meta J [--impl naive] [--speed 10] [--decisions 5000]\n"
                 "              [--warmup 200] [--cpu N] [--fifo PRIO] [--stall-us 200 --stall-prob 0.01]\n"
                 "              [--seed 1] [--sector-deg 60] [--csv out.csv] [--label L] [--hgrm f]\n"
                 "pipelines:";
    for (const auto& n : avl::pipeline_names()) std::cerr << ' ' << n;
    std::cerr << '\n';
    return 2;
  }
  const std::string impl = args.str("impl", "naive");
  const double speed = args.num("speed", 10.0);
  const auto decisions = static_cast<std::int64_t>(args.num("decisions", 5000));
  const auto warmup = static_cast<std::int64_t>(args.num("warmup", 200));
  const double stall_us = args.num("stall-us", 0.0);
  const double stall_prob = args.num("stall-prob", 0.0);
  const auto seed = static_cast<std::uint64_t>(args.num("seed", 1));

  const avl::SensorInfo info = avl::load_sensor_info(args.str("meta"));
  avl::Config cfg;
  cfg.sector_deg = args.num("sector-deg", cfg.sector_deg);
  cfg.spin_us = args.num("spin-us", cfg.spin_us);
  cfg.helpers = static_cast<int>(args.num("helpers", cfg.helpers));
  cfg.first_helper_cpu = static_cast<int>(args.num("helper-cpu", cfg.first_helper_cpu));

  const std::vector<std::uint8_t> file = avl::read_file(args.str("pcap"));
  const avl::PcapResult cap = avl::parse_pcap(file);
  if (!cap.ok()) throw std::runtime_error("pcap: " + cap.error);
  std::vector<avl::UdpDatagram> pkts;
  for (const auto& d : cap.datagrams) {
    if (avl::is_lidar_packet(d.payload, info)) pkts.push_back(d);
  }
  if (pkts.size() < 2) throw std::runtime_error("pcap has fewer than 2 lidar packets");

  // One loop of the capture, as offsets from its first packet. The loop
  // period appends one mean inter-packet gap so the wrap is not a burst.
  std::vector<std::int64_t> offset_ns;
  offset_ns.reserve(pkts.size());
  for (const auto& d : pkts) offset_ns.push_back(static_cast<std::int64_t>(d.ts_ns - pkts[0].ts_ns));
  const std::int64_t span = offset_ns.back();
  const std::int64_t loop_ns = span + span / static_cast<std::int64_t>(pkts.size() - 1);

  auto pipeline = avl::make_pipeline(impl, info, cfg);
  hdr_histogram* hist = nullptr;
  if (hdr_init(1, 10'000'000'000LL, 3, &hist) != 0) throw std::runtime_error("hdr_init");

  const std::string rt =
      setup_realtime(static_cast<int>(args.num("cpu", -1)), static_cast<int>(args.num("fifo", 0)));
  std::mt19937_64 rng(seed);
  std::bernoulli_distribution stall(stall_prob);

  std::vector<char> is_trigger(pkts.size(), 0);
  std::int64_t seen = 0, brakes = 0, behind = 0;
  const Clock::time_point t0 = Clock::now() + std::chrono::milliseconds(10);
  for (std::int64_t loop = 0; seen < warmup + decisions; ++loop) {
    for (std::size_t i = 0; i < pkts.size() && seen < warmup + decisions; ++i) {
      Clock::time_point intended;
      if (speed > 0) {
        const double sched_ns = static_cast<double>(loop * loop_ns + offset_ns[i]) / speed;
        intended = t0 + std::chrono::nanoseconds(static_cast<std::int64_t>(sched_ns));
        Clock::time_point now = Clock::now();
        if (now > intended + std::chrono::microseconds(10)) ++behind;
        while (now < intended) now = Clock::now();
      } else {
        intended = Clock::now();
      }
      const bool coz_pair = loop > 0 && is_trigger[i];
      if (coz_pair) COZ_BEGIN("sector");
      const auto decision = pipeline->on_packet(pkts[i].payload);
      if (coz_pair) COZ_END("sector");
      if (!decision) continue;
      is_trigger[i] = 1;
      if (stall_us > 0 && stall(rng)) {
        const auto until = Clock::now() + std::chrono::nanoseconds(static_cast<long long>(stall_us * 1000));
        while (Clock::now() < until) {
        }
      }
      const std::int64_t lat = ns_since(intended, Clock::now());
      COZ_PROGRESS_NAMED("decision");
      if (seen++ >= warmup) {
        hdr_record_value(hist, lat);
        brakes += decision->brake ? 1 : 0;
      }
    }
  }

  const char* pad = std::getenv("AVL_PAD");
  const std::size_t env_pad = pad ? std::strlen(pad) : 0;
  const std::string csv = args.str("csv");
  if (!csv.empty()) {
    const bool fresh = !std::filesystem::exists(csv) || std::filesystem::file_size(csv) == 0;
    FILE* f = std::fopen(csv.c_str(), "a");
    if (!f) throw std::runtime_error("cannot open " + csv);
    if (fresh) {
      std::fprintf(f,
                   "label,impl,seed,env_pad,link_seed,speed,rt,decisions,brakes,behind,"
                   "mean_ns,p50_ns,p90_ns,p99_ns,p999_ns,max_ns\n");
    }
    std::fprintf(f, "%s,%s,%llu,%zu,%d,%g,%s,%lld,%lld,%lld,%.0f,%lld,%lld,%lld,%lld,%lld\n",
                 args.str("label", impl).c_str(), impl.c_str(), static_cast<unsigned long long>(seed),
                 env_pad, AVL_LINK_SEED, speed, rt.c_str(), static_cast<long long>(decisions),
                 static_cast<long long>(brakes), static_cast<long long>(behind), hdr_mean(hist),
                 static_cast<long long>(hdr_value_at_percentile(hist, 50.0)),
                 static_cast<long long>(hdr_value_at_percentile(hist, 90.0)),
                 static_cast<long long>(hdr_value_at_percentile(hist, 99.0)),
                 static_cast<long long>(hdr_value_at_percentile(hist, 99.9)),
                 static_cast<long long>(hdr_max(hist)));
    std::fclose(f);
  }
  const std::string hgrm = args.str("hgrm");
  if (!hgrm.empty()) {
    FILE* f = std::fopen(hgrm.c_str(), "w");
    if (!f) throw std::runtime_error("cannot open " + hgrm);
    hdr_percentiles_print(hist, f, 5, 1000.0, CLASSIC);  // values printed in microseconds
    std::fclose(f);
  }
  std::printf("%s rt=%s n=%lld p50=%.1fus p99=%.1fus p99.9=%.1fus max=%.1fus behind=%lld brakes=%lld\n",
              impl.c_str(), rt.c_str(), static_cast<long long>(decisions),
              static_cast<double>(hdr_value_at_percentile(hist, 50.0)) / 1e3,
              static_cast<double>(hdr_value_at_percentile(hist, 99.0)) / 1e3,
              static_cast<double>(hdr_value_at_percentile(hist, 99.9)) / 1e3,
              static_cast<double>(hdr_max(hist)) / 1e3, static_cast<long long>(behind),
              static_cast<long long>(brakes));
  hdr_close(hist);
  return 0;
} catch (const std::exception& e) {
  std::cerr << "replay: " << e.what() << '\n';
  return 1;
}
