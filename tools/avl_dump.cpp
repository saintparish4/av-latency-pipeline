// avl_dump: writes pipeline internals as CSV so analysis/diff_reference.py
// can compare them with the Ouster SDK and Open3D. Not on any measured path.
//
//   avl_dump decode PCAP META OUT.csv            every return of the capture:
//                                                mid,row,range_mm,x,y,z
//   avl_dump trace  PCAP META IMPL OUTPREFIX     first decision's internals:
//     OUTPREFIX.points.csv  x,y,z,h,ground
//     OUTPREFIX.plane.csv   nx,ny,nz,d
//     OUTPREFIX.voxels.csv  ix,iy,ih,count   (non-zero cells only)
//     OUTPREFIX.decision.csv brake,nearest_m,points,ground,occupied
#include <cstdio>
#include <iostream>
#include <string>

#include "geometry.hpp"
#include "pcap.hpp"
#include "pipeline.hpp"
#include "xyz.hpp"

namespace {

FILE* open_or_throw(const std::string& path) {
  FILE* f = std::fopen(path.c_str(), "w");
  if (!f) throw std::runtime_error("cannot write " + path);
  return f;
}

int dump_decode(const avl::PcapResult& cap, const avl::SensorInfo& info, const std::string& out) {
  const avl::XyzLut lut(info);
  FILE* f = open_or_throw(out);
  std::fprintf(f, "mid,row,range_mm,x,y,z\n");
  for (const auto& d : cap.datagrams) {
    if (!avl::is_lidar_packet(d.payload, info)) continue;
    avl::for_each_return(avl::PacketView(d.payload, info), lut,
                         [&](int mid, int row, std::uint32_t r, float x, float y, float z) {
                           std::fprintf(f, "%d,%d,%u,%.9g,%.9g,%.9g\n", mid, row, r, x, y, z);
                         });
  }
  std::fclose(f);
  return 0;
}

int dump_trace(const avl::PcapResult& cap, const avl::SensorInfo& info, const std::string& impl,
               const std::string& prefix, double sector_deg) {
  avl::Config cfg;
  cfg.trace = true;
  cfg.sector_deg = sector_deg;
  auto p = avl::make_pipeline(impl, info, cfg);
  for (const auto& d : cap.datagrams) {
    const auto decided = p->on_packet(d.payload);
    if (!decided) continue;
    const avl::Decision& decision = *decided;
    const avl::Trace* t = p->trace();
    if (!t) throw std::runtime_error(impl + " does not record traces");
    FILE* f = open_or_throw(prefix + ".points.csv");
    std::fprintf(f, "x,y,z,h,ground\n");
    for (std::size_t i = 0; i < t->x.size(); ++i) {
      std::fprintf(f, "%.9g,%.9g,%.9g,%.9g,%d\n", t->x[i], t->y[i], t->z[i], t->h[i], t->ground[i]);
    }
    std::fclose(f);
    f = open_or_throw(prefix + ".plane.csv");
    std::fprintf(f, "nx,ny,nz,d\n%.9g,%.9g,%.9g,%.9g\n", t->plane[0], t->plane[1], t->plane[2], t->plane[3]);
    std::fclose(f);
    const avl::GridSpec g(cfg);
    f = open_or_throw(prefix + ".voxels.csv");
    std::fprintf(f, "ix,iy,ih,count\n");
    for (int ix = 0; ix < g.nx; ++ix) {
      for (int iy = 0; iy < g.ny; ++iy) {
        for (int ih = 0; ih < g.nh; ++ih) {
          const int c = t->grid[g.flat(ix, iy, ih)];
          if (c) std::fprintf(f, "%d,%d,%d,%d\n", ix, iy, ih, c);
        }
      }
    }
    std::fclose(f);
    f = open_or_throw(prefix + ".decision.csv");
    std::fprintf(f, "brake,nearest_m,points,ground,occupied\n%d,%.9g,%u,%u,%u\n", decision.brake ? 1 : 0,
                 decision.nearest_m, decision.points, decision.ground, decision.occupied);
    std::fclose(f);
    return 0;
  }
  throw std::runtime_error("capture produced no decision");
}

}  // namespace

int main(int argc, char** argv) try {
  const std::string mode = argc > 1 ? argv[1] : "";
  if (!((mode == "decode" && argc == 5) || (mode == "trace" && (argc == 6 || argc == 7)))) {
    std::cerr << "usage: avl_dump decode PCAP META OUT.csv\n"
                 "       avl_dump trace  PCAP META IMPL OUTPREFIX [SECTOR_DEG]\n";
    return 2;
  }
  const auto bytes = avl::read_file(argv[2]);
  const auto cap = avl::parse_pcap(bytes);
  if (!cap.ok()) throw std::runtime_error("pcap: " + cap.error);
  const auto info = avl::load_sensor_info(argv[3]);
  if (mode == "decode") return dump_decode(cap, info, argv[4]);
  return dump_trace(cap, info, argv[4], argv[5], argc == 7 ? std::stod(argv[6]) : avl::Config{}.sector_deg);
} catch (const std::exception& e) {
  std::cerr << "avl_dump: " << e.what() << '\n';
  return 1;
}
