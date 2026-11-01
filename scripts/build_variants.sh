#!/usr/bin/env bash
# Rung 5: one source revision, one pipeline rung, built five ways so the
# compiler techniques are compared on the same code:
#   base     -O2 -g, the measured preset
#   thinlto  + -flto=thin
#   pgo      instrumented build -> training run -> -fprofile-instr-use
#   autofdo  perf branch samples of base -> llvm-profgen -> -fprofile-sample-use
#   bolt     perf branch samples of base -> llvm-bolt rewrite of base's binary
# autofdo and bolt need `perf record -b` / `-j any,u`, i.e. LBR (Intel) or
# AMD BRS/LBRv2 (Zen 3 and later), on bare metal; they are skipped with a
# message when perf cannot sample branches.
#
# Training workload: closed-loop replay of the full 360-degree frame, while
# the measured workload is the 60-degree sector. Same code paths, different
# trip counts, so the profile is not simply the benchmark replayed.
#   usage: IMPL=simd scripts/build_variants.sh
set -euo pipefail
cd "$(dirname "$0")/.." || exit 1
IMPL=${IMPL:-simd}
CAP=data/OS-2-128-U1_v2.3.0_1024x10
TRAIN=(--pcap "$CAP.pcap" --meta "$CAP.json" --impl "$IMPL" --speed 0 --decisions 20000 --sector-deg 360)
LD="-fuse-ld=lld -Wl,--emit-relocs"

variant() { # variant NAME CXXFLAGS LDFLAGS
  cmake -S . -B "build/v-$1" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DAVL_TESTS=OFF \
    -DCMAKE_C_COMPILER=clang-18 -DCMAKE_CXX_COMPILER=clang++-18 \
    -DCMAKE_C_FLAGS="$2" -DCMAKE_CXX_FLAGS="$2" -DCMAKE_EXE_LINKER_FLAGS="$LD $3" > /dev/null
  cmake --build "build/v-$1" --target replay > /dev/null
  echo "built build/v-$1/bench/replay"
}

variant base "" ""
variant thinlto "-flto=thin" "-flto=thin"

variant pgo-gen "-fprofile-instr-generate" "-fprofile-instr-generate"
rm -f build/v-pgo-gen/*.profraw
LLVM_PROFILE_FILE="$PWD/build/v-pgo-gen/%p.profraw" build/v-pgo-gen/bench/replay "${TRAIN[@]}" > /dev/null
llvm-profdata-18 merge -o build/pgo.profdata build/v-pgo-gen/*.profraw
# A handful of header functions hash differently between the two build trees;
# clang reports them and ignores their data, which is not worth failing -Werror.
variant pgo "-fprofile-instr-use=$PWD/build/pgo.profdata -Wno-profile-instr-out-of-date" ""

if perf record -b -e cycles:u -o /dev/null -- true > /dev/null 2>&1; then
  variant afdo-src "-fdebug-info-for-profiling -funique-internal-linkage-names" ""
  perf record -b -e cycles:u -o build/autofdo.data -- build/v-afdo-src/bench/replay "${TRAIN[@]}" > /dev/null
  llvm-profgen-18 --binary=build/v-afdo-src/bench/replay --perfdata=build/autofdo.data \
    --output=build/autofdo.prof
  variant autofdo "-fdebug-info-for-profiling -funique-internal-linkage-names \
-fprofile-sample-use=$PWD/build/autofdo.prof" ""

  perf record -e cycles:u -j any,u -o build/bolt.data -- build/v-base/bench/replay "${TRAIN[@]}" > /dev/null
  perf2bolt-18 -p build/bolt.data -o build/bolt.fdata build/v-base/bench/replay
  mkdir -p build/v-bolt/bench
  llvm-bolt-18 build/v-base/bench/replay -o build/v-bolt/bench/replay -data=build/bolt.fdata \
    -reorder-blocks=ext-tsp -reorder-functions=cdsort -split-functions -split-all-cold -dyno-stats
  echo "built build/v-bolt/bench/replay"
else
  echo "perf cannot sample branches here (no LBR/BRS, or not bare metal): skipping autofdo and bolt"
fi
