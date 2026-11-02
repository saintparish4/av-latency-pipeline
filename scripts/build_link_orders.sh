#!/usr/bin/env bash
# Builds the replay driver with N different object link orders
# (AVL_LINK_SEED=1..N) so analysis/bench.py can treat link order as noise.
#   usage: scripts/build_link_orders.sh [N]      (default 4) -> build/link-*/bench/replay
set -euo pipefail
cd "$(dirname "$0")/.." || exit 1
N=${1:-4}
for seed in $(seq 1 "$N"); do
  cmake -S . -B "build/link-$seed" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DAVL_TESTS=OFF \
    -DCMAKE_C_COMPILER=clang-18 -DCMAKE_CXX_COMPILER=clang++-18 \
    -DCMAKE_EXE_LINKER_FLAGS="-fuse-ld=lld -Wl,--emit-relocs" -DAVL_LINK_SEED="$seed" > /dev/null
  cmake --build "build/link-$seed" --target replay > /dev/null
  echo "built build/link-$seed/bench/replay"
done
