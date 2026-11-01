// See simd_kernels.hpp. Static dispatch: the target is fixed by the ISA
// flags (AVL_ISA), so there is exactly one code path to profile and to feed to
// llvm-mca. Each kernel body is bracketed with LLVM-MCA markers so
// scripts/mca.sh can analyse the hot loop in isolation.
#include "simd_kernels.hpp"

#include <hwy/highway.h>

#include <cmath>

#if defined(AVL_MCA_MARKERS)
#define MCA_BEGIN(name) asm volatile("# LLVM-MCA-BEGIN " name)
#define MCA_END(name) asm volatile("# LLVM-MCA-END " name)
#else
#define MCA_BEGIN(name)
#define MCA_END(name)
#endif

// Highway picks its static target from the compiler's ISA macros and falls
// back silently. If the build asked for AVX2, make "got less" a compile error.
#if HWY_ARCH_X86 && defined(__AVX2__)
static_assert(HWY_STATIC_TARGET <= HWY_AVX2,
              "Highway fell back below AVX2: compile with -maes -mpclmul too (CMakeLists.txt AVL_ISA)");
#endif

namespace avl::simd {
namespace hn = hwy::HWY_NAMESPACE;

using DF = hn::ScalableTag<float>;
using DU = hn::RebindToUnsigned<DF>;
using DI = hn::RebindToSigned<DF>;
using DD = hn::ScalableTag<double>;
using DH = hn::Rebind<float, DD>;  // float vector with as many lanes as DD

std::size_t decode_column(const std::uint8_t* pixels, const XyzLut& lut, int mid, const Roi& roi, float* x,
                          float* y, float* z, std::size_t n) {
  const DF df;
  const DU du;
  const DI di;
  const std::size_t lanes = hn::Lanes(df);
  const auto rows = static_cast<std::size_t>(lut.rows());
  const std::size_t base = lut.index(mid, 0);
  // A pixel is three little-endian u32 words; word 0 holds the range.
  const auto* words = reinterpret_cast<const std::uint32_t*>(pixels);
  const auto mask = hn::Set(du, kRangeMask);
  const auto x_min = hn::Set(df, roi.x_min), x_max = hn::Set(df, roi.x_max), y_abs = hn::Set(df, roi.y_abs);
  std::size_t row = 0;
  MCA_BEGIN("decode");
  for (; row + lanes <= rows; row += lanes) {
    hn::Vec<DU> w0, w1, w2;
    hn::LoadInterleaved3(du, words + 3 * row, w0, w1, w2);
    const auto range = hn::And(w0, mask);
    const auto r = hn::ConvertTo(df, hn::BitCast(di, range));
    const std::size_t i = base + row;
    const auto px = hn::Add(hn::Mul(r, hn::LoadU(df, &lut.dx[i])), hn::LoadU(df, &lut.ox[i]));
    const auto py = hn::Add(hn::Mul(r, hn::LoadU(df, &lut.dy[i])), hn::LoadU(df, &lut.oy[i]));
    const auto pz = hn::Add(hn::Mul(r, hn::LoadU(df, &lut.dz[i])), hn::LoadU(df, &lut.oz[i]));
    const auto keep = hn::And(hn::And(hn::RebindMask(df, hn::Ne(range, hn::Zero(du))), hn::Ge(px, x_min)),
                              hn::And(hn::Lt(px, x_max), hn::Lt(hn::Abs(py), y_abs)));
    hn::CompressStore(px, keep, df, x + n);
    hn::CompressStore(py, keep, df, y + n);
    n += hn::CompressStore(pz, keep, df, z + n);
  }
  MCA_END("decode");
  for (; row < rows; ++row) {  // scalar tail for row counts that are not a lane multiple
    const std::uint32_t range = load_u32(pixels + row * kPixelBytes) & kRangeMask;
    if (range == 0) continue;
    const std::size_t i = base + row;
    const auto r = static_cast<float>(range);
    const float px = r * lut.dx[i] + lut.ox[i], py = r * lut.dy[i] + lut.oy[i];
    if (px < roi.x_min || px >= roi.x_max || std::abs(py) >= roi.y_abs) continue;
    x[n] = px, y[n] = py, z[n] = r * lut.dz[i] + lut.oz[i];
    ++n;
  }
  return n;
}

void seed(const float* z, float below, float* w, std::size_t b, std::size_t e) {
  const DF df;
  const auto lim = hn::Set(df, below), one = hn::Set(df, 1.0f), zero = hn::Zero(df);
  for (std::size_t i = b; i < e; i += hn::Lanes(df)) {
    hn::StoreU(hn::IfThenElse(hn::Lt(hn::LoadU(df, z + i), lim), one, zero), df, w + i);
  }
}

Sums sums(const float* x, const float* y, const float* z, const float* w, std::size_t b, std::size_t e) {
  const DD dd;
  const DH dh;
  auto sw = hn::Zero(dd), sx = sw, sy = sw, sz = sw;
  MCA_BEGIN("sums");
  for (std::size_t i = b; i < e; i += hn::Lanes(dd)) {
    const auto wv = hn::PromoteTo(dd, hn::LoadU(dh, w + i));
    sw = hn::Add(sw, wv);
    sx = hn::MulAdd(wv, hn::PromoteTo(dd, hn::LoadU(dh, x + i)), sx);
    sy = hn::MulAdd(wv, hn::PromoteTo(dd, hn::LoadU(dh, y + i)), sy);
    sz = hn::MulAdd(wv, hn::PromoteTo(dd, hn::LoadU(dh, z + i)), sz);
  }
  MCA_END("sums");
  return {hn::ReduceSum(dd, sw), hn::ReduceSum(dd, sx), hn::ReduceSum(dd, sy), hn::ReduceSum(dd, sz)};
}

Products products(const float* x, const float* y, const float* z, const float* w, const double mean[3],
                  std::size_t b, std::size_t e) {
  const DD dd;
  const DH dh;
  const auto mx = hn::Set(dd, mean[0]), my = hn::Set(dd, mean[1]), mz = hn::Set(dd, mean[2]);
  auto xx = hn::Zero(dd), xy = xx, xz = xx, yy = xx, yz = xx, zz = xx;
  MCA_BEGIN("products");
  for (std::size_t i = b; i < e; i += hn::Lanes(dd)) {
    const auto wv = hn::PromoteTo(dd, hn::LoadU(dh, w + i));
    // Weight folded into one factor: w * dx * dy with w in {0, 1}.
    const auto dx = hn::Sub(hn::PromoteTo(dd, hn::LoadU(dh, x + i)), mx);
    const auto dy = hn::Sub(hn::PromoteTo(dd, hn::LoadU(dh, y + i)), my);
    const auto dz = hn::Sub(hn::PromoteTo(dd, hn::LoadU(dh, z + i)), mz);
    const auto wx = hn::Mul(wv, dx), wy = hn::Mul(wv, dy);
    xx = hn::MulAdd(wx, dx, xx);
    xy = hn::MulAdd(wx, dy, xy);
    xz = hn::MulAdd(wx, dz, xz);
    yy = hn::MulAdd(wy, dy, yy);
    yz = hn::MulAdd(wy, dz, yz);
    zz = hn::MulAdd(hn::Mul(wv, dz), dz, zz);
  }
  MCA_END("products");
  return {hn::ReduceSum(dd, xx), hn::ReduceSum(dd, xy), hn::ReduceSum(dd, xz),
          hn::ReduceSum(dd, yy), hn::ReduceSum(dd, yz), hn::ReduceSum(dd, zz)};
}

void classify(const Plane& p, float thresh, const float* x, const float* y, const float* z, float* h,
              float* w, std::size_t b, std::size_t e) {
  const DF df;
  const auto nx = hn::Set(df, p.nx), ny = hn::Set(df, p.ny), nz = hn::Set(df, p.nz), d = hn::Set(df, p.d);
  const auto lim = hn::Set(df, thresh), one = hn::Set(df, 1.0f), zero = hn::Zero(df);
  MCA_BEGIN("classify");
  for (std::size_t i = b; i < e; i += hn::Lanes(df)) {
    // Same association as Plane::height: ((nx*x + ny*y) + nz*z) + d.
    const auto hv =
        hn::Add(hn::Add(hn::Add(hn::Mul(nx, hn::LoadU(df, x + i)), hn::Mul(ny, hn::LoadU(df, y + i))),
                        hn::Mul(nz, hn::LoadU(df, z + i))),
                d);
    hn::StoreU(hv, df, h + i);
    hn::StoreU(hn::IfThenElse(hn::Lt(hn::Abs(hv), lim), one, zero), df, w + i);
  }
  MCA_END("classify");
}

void cells(const GridSpec& g, const float* x, const float* y, const float* h, const float* w,
           std::int32_t* cell, std::size_t b, std::size_t e) {
  const DF df;
  const DI di;
  const auto x0 = hn::Set(df, g.x0), y0 = hn::Set(df, g.y0), h0 = hn::Set(df, g.h0);
  const auto inv = hn::Set(df, g.inv_voxel), zero = hn::Zero(df);
  const auto nx = hn::Set(df, static_cast<float>(g.nx)), ny = hn::Set(df, static_cast<float>(g.ny));
  const auto nh = hn::Set(df, static_cast<float>(g.nh));
  const auto nyi = hn::Set(di, g.ny), nhi = hn::Set(di, g.nh), none = hn::Set(di, -1);
  MCA_BEGIN("cells");
  for (std::size_t i = b; i < e; i += hn::Lanes(df)) {
    const auto fx = hn::Floor(hn::Mul(hn::Sub(hn::LoadU(df, x + i), x0), inv));
    const auto fy = hn::Floor(hn::Mul(hn::Sub(hn::LoadU(df, y + i), y0), inv));
    const auto fh = hn::Floor(hn::Mul(hn::Sub(hn::LoadU(df, h + i), h0), inv));
    const auto inside =
        hn::And(hn::And(hn::And(hn::Ge(fx, zero), hn::Lt(fx, nx)), hn::And(hn::Ge(fy, zero), hn::Lt(fy, ny))),
                hn::And(hn::And(hn::Ge(fh, zero), hn::Lt(fh, nh)), hn::Eq(hn::LoadU(df, w + i), zero)));
    // Lanes outside the grid may hold huge floats; zero them before converting.
    const auto ix = hn::ConvertTo(di, hn::IfThenElseZero(inside, fx));
    const auto iy = hn::ConvertTo(di, hn::IfThenElseZero(inside, fy));
    const auto ih = hn::ConvertTo(di, hn::IfThenElseZero(inside, fh));
    const auto flat = hn::Add(hn::Mul(hn::Add(hn::Mul(ix, nyi), iy), nhi), ih);
    hn::StoreU(hn::IfThenElse(hn::RebindMask(di, inside), flat, none), di, cell + i);
  }
  MCA_END("cells");
}

}  // namespace avl::simd
