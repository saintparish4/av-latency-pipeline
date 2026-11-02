#!/usr/bin/env bash
# Causal profile: which lines, if made faster, would shorten the decision's
# critical path? replay brackets each trigger packet's on_packet() with a Coz
# latency pair ("sector"), so a line that only speeds up decode of earlier
# packets shows no virtual-speedup benefit here, however hot it is in perf.
#   usage: scripts/coz.sh <impl>      (needs: sudo apt install coz-profiler)
# View results/coz_<impl>.coz at https://plasma-umass.org/coz/
set -euo pipefail
cd "$(dirname "$0")/.." || exit 1
IMPL=${1:?impl}
CAP=data/OS-2-128-U1_v2.3.0_1024x10
cmake -S . -B build/coz -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DAVL_TESTS=OFF -DAVL_COZ=ON \
  -DCMAKE_C_COMPILER=clang-18 -DCMAKE_CXX_COMPILER=clang++-18 \
  -DCMAKE_C_FLAGS=-gdwarf-4 -DCMAKE_CXX_FLAGS=-gdwarf-4 > /dev/null
cmake --build build/coz --target replay > /dev/null
mkdir -p results
coz run --source-scope "$PWD/src/%" --- build/coz/bench/replay --pcap "$CAP.pcap" --meta "$CAP.json" \
  --impl "$IMPL" --speed 0 --decisions 300000
mv profile.coz "results/coz_$IMPL.coz"
echo "wrote results/coz_$IMPL.coz"
