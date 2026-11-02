#!/usr/bin/env bash
# Static throughput analysis of the SIMD kernels' hot loops with llvm-mca.
# The kernels carry "# LLVM-MCA-BEGIN/END <name>" markers when compiled with
# -DAVL_MCA_MARKERS; this compiles that one file to assembly with exactly the
# measured build's flags and analyses each marked region.
#   usage: scripts/mca.sh [mcpu]     mcpu defaults to the host (znver3, skylake, ...)
# uiCA (https://uica.uops.info) models Intel cores only; on an Intel machine,
# paste a region from build/mca/simd_kernels.s into it as a second opinion.
set -euo pipefail
cd "$(dirname "$0")/.." || exit 1
MCPU=${1:-$(llc-18 --version | sed -n 's/.*Host CPU: *//p')}
HWY=$(sed -n 's/^hwy_SOURCE_DIR:STATIC=//p' build/rel/CMakeCache.txt 2> /dev/null)
[ -n "$HWY" ] || {
  echo "configure the rel preset first"
  exit 1
}
mkdir -p build/mca results
clang++-18 -std=c++20 -O2 -march=x86-64-v3 -maes -mpclmul -ffp-contract=off -fno-omit-frame-pointer \
  -DAVL_MCA_MARKERS -DHWY_STATIC_DEFINE -Isrc -isystem "$HWY" \
  -S -o build/mca/simd_kernels.s src/simd_kernels.cpp
llvm-mca-18 -mcpu="$MCPU" -iterations=200 -bottleneck-analysis build/mca/simd_kernels.s \
  > "results/mca_$MCPU.txt" 2>&1
grep -E "^\[[0-9]+\] Code Region|^Iterations|^Total Cycles|^Block RThroughput|^IPC|^Instructions" \
  "results/mca_$MCPU.txt"
echo "full report: results/mca_$MCPU.txt"
