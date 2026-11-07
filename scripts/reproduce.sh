#!/usr/bin/env bash
# One command from a fresh clone to results/REPORT.md.
#   scripts/reproduce.sh --check   correctness only: build, unit/scene/equivalence
#                                  tests, sanitizers, differential tests, short fuzz.
#                                  Runs anywhere, including the Docker image.
#   scripts/reproduce.sh           everything: --check, then on a prepared native
#                                  host the jitter floor, harness validation, the
#                                  ladder, compiler variants, counters, roofline.
# In Docker, TSan needs: docker run --security-opt seccomp=unconfined ...
# Env: CPU (measured core, default 3), HELPER_CPU (default CPU+1), RUNS (per arm).
# Results from a host that fails scripts/rt_check.sh are labelled NOT REPORTABLE.
set -euo pipefail
cd "$(dirname "$0")/.." || exit 1
MODE=${1:-full}
CPU=${CPU:-3}
HELPER_CPU=${HELPER_CPU:-$((CPU + 1))}
export CPU HELPER_CPU
CAP=data/OS-2-128-U1_v2.3.0_1024x10
mkdir -p results
step() { printf '\n==== %s\n' "$*"; }

# Use the active Python if it already has the pinned packages (the Docker
# image does); otherwise build .venv from requirements.txt once.
if ! python3 -c "import scipy, matplotlib, open3d, ouster.sdk" 2> /dev/null; then
  if [ ! -x .venv/bin/python ]; then
    step "python venv"
    python3 -m venv .venv
    .venv/bin/pip install -q -r requirements.txt
  fi
  export PATH="$PWD/.venv/bin:$PATH"
fi

# One command per line: `set -e` does not stop on a failure inside `a && b && c`.
step "data"
scripts/fetch_data.sh
for p in rel asan tsan; do
  step "build + test ($p)"
  cmake --preset "$p" > /dev/null
  cmake --build --preset "$p"
  ctest --preset "$p"
done
step "differential"
python3 -I analysis/diff_reference.py --build build/rel --data data
step "fuzz (60 s each)"
cmake --preset fuzz > /dev/null
cmake --build --preset fuzz
scripts/fuzz.sh 60
[ "$MODE" = "--check" ] && {
  echo "check: PASS"
  exit 0
}

R=results/REPORT.md
step "hardware"
scripts/hwinfo.sh > results/hwinfo.txt
if scripts/rt_check.sh "$CPU" > results/rt_check.txt; then REPORTABLE=yes; else REPORTABLE=no; fi
cat results/rt_check.txt
{
  echo "# Results"
  echo
  [ "$REPORTABLE" = yes ] || echo "**NOT REPORTABLE: this host failed scripts/rt_check.sh (see results/rt_check.txt).**"
  echo
  echo '```'
  cat results/hwinfo.txt
  echo '```'
  echo
  echo "## Budget"
  python3 analysis/budget.py
} > "$R"

# setcap can succeed and still not take effect (a nosuid mount ignores file
# capabilities). replay would then only label its runs "nofifo", and the
# rt= lines are filtered out of the report below, so prove it took.
grant_rt() {
  sudo setcap cap_sys_nice,cap_ipc_lock+ep "$1"
  local rt
  rt=$("$1" --pcap "$CAP.pcap" --meta "$CAP.json" --impl spin --speed 0 --decisions 20 \
    --cpu "$CPU" --fifo 80 | grep -o 'rt=[^ ]*')
  if [ "$rt" != rt=mlock+pin+fifo ]; then
    echo "reproduce: $1 got $rt after setcap; is the filesystem mounted nosuid?" >&2
    exit 1
  fi
}

step "privileges for SCHED_FIFO and mlock (one sudo prompt)"
scripts/build_link_orders.sh 4
for b in build/rel/bench/replay build/link-*/bench/replay; do grant_rt "$b"; done

step "OS jitter floor (cyclictest, 5 min)"
sudo scripts/jitter_floor.sh "$CPU" 5m | tee -a "$R"

step "harness validation"
{
  echo
  echo "## Harness validation"
} >> "$R"
RT="--cpu $CPU --fifo 80" scripts/validate_harness.sh | grep -vE "^\[|rt=" | tee -a "$R"

for sector in 60 360; do
  step "ladder, ${sector} degrees"
  {
    echo
    echo "## Ladder, ${sector}-degree sector"
  } >> "$R"
  scripts/ladder.sh "$sector" | grep -vE "^\[|rt=" | tee -a "$R"
  python3 analysis/summary.py "results/ladder_${sector}.csv" | tee -a "$R"
done

step "compiler variants"
IMPL=simd scripts/build_variants.sh
arms=()
names=()
for v in build/v-*/bench/replay; do
  name=$(basename "$(dirname "$(dirname "$v")")")
  name=${name#v-}
  case "$name" in pgo-gen | afdo-src) continue ;; esac # profiling builds, not variants
  grant_rt "$v"
  arms+=(--arm "$name|$v|--impl simd")
  names+=("$name")
done
rm -f results/variants.csv
python3 analysis/bench.py --out results/variants.csv --runs "${RUNS:-10}" "${arms[@]}" -- \
  --pcap "$CAP.pcap" --meta "$CAP.json" --speed 10 --decisions 5000 --cpu "$CPU" --fifo 80
{
  echo
  echo "## Compiler variants (simd rung)"
} >> "$R"
for name in "${names[@]}"; do
  [ "$name" = base ] && continue
  echo
  echo "### $name vs base"
  python3 analysis/compare.py results/variants.csv --base base --cand "$name"
done | tee -a "$R"

if command -v perf > /dev/null && perf stat -e cycles true > /dev/null 2>&1; then
  step "counters"
  {
    echo
    echo "## Counters (closed loop)"
    echo '```'
  } >> "$R"
  for r in naive nth soa simd mt; do scripts/perf_rung.sh "$r" "$CPU"; done | tee -a "$R"
  echo '```' >> "$R"
fi

step "roofline"
build/rel/tools/roofline_probe > results/roofline_probe.csv
taskset -c "$CPU" build/rel/tools/kernel_bench "$CAP.pcap" "$CAP.json" > results/kernels.csv
{
  echo
  echo "## Roofline"
  echo
  echo '![roofline](roofline.png)'
  echo
} >> "$R"
python3 analysis/roofline.py results/roofline_probe.csv results/kernels.csv --out results/roofline.png | tee -a "$R"

step "done: $R"
