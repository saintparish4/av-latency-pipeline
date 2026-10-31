#!/usr/bin/env bash
# Proves the harness before it is allowed to judge any optimization:
#   1. A/A, synthetic: two arms, same binary -> no metric may differ.
#   2. A/A, real:      same, on the naive pipeline (real cache/branch noise).
#   3. Stall, always:  +200 us on every decision -> p50 must get slower.
#   4. Stall, 1%:      +200 us on 1% of decisions -> p99.9 slower, p50 same.
# Any expectation that fails makes the script exit non-zero. A harness that
# passes 1-2 but fails 3-4 is blind; one that fails 1-2 is crying wolf.
#
# Env: REPLAY (binary glob, default build/rel/bench/replay), RUNS (per arm),
#      DECISIONS (per run), SPEED, RT (extra replay args, e.g. "--cpu 3 --fifo 80").
set -euo pipefail
cd "$(dirname "$0")/.." || exit 1

REPLAY=${REPLAY:-build/rel/bench/replay}
RUNS=${RUNS:-10}
DECISIONS=${DECISIONS:-3000}
SPEED=${SPEED:-50}
RT=${RT:-}
CAP=data/OS-2-128-U1_v2.3.0_1024x10
OUT=results/validate

# Checked before anything is deleted: a mistyped REPLAY must not wipe the last run.
first=$(compgen -G "$REPLAY" | head -1 || true)
if [ -z "$first" ]; then
  echo "validate_harness: no binary matches '$REPLAY'; run: cmake --build --preset rel" >&2
  exit 1
fi

mkdir -p "$OUT"
rm -f "$OUT"/*.csv

read -ra rt_args <<< "$RT" # RT is a flag list on purpose: split it into words
common=(--pcap "$CAP.pcap" --meta "$CAP.json" --speed "$SPEED" --decisions "$DECISIONS" "${rt_args[@]}")
bench() { python3 analysis/bench.py --runs "$RUNS" "$@"; }
status=0

echo "== 1. A/A, spin 50 us"
bench --out "$OUT/aa_spin.csv" --arm "A|$REPLAY" --arm "B|$REPLAY" -- "${common[@]}" --impl spin
python3 analysis/compare.py "$OUT/aa_spin.csv" --base A --cand B \
  --expect p50_ns=same --expect p99_ns=same --expect p999_ns=same || status=1

# Step 2 needs the naive pipeline (directive/p2.md); before it exists, skip.
# Captured first: `replay --help` exits 2, which pipefail would report as "no
# naive". Match the pipelines line only, because the usage text says "naive" too.
pipelines=$("$first" --help 2>&1 || true)
if grep -Eq '^pipelines:.* naive( |$)' <<< "$pipelines"; then
  echo "== 2. A/A, naive"
  bench --out "$OUT/aa_naive.csv" --arm "A|$REPLAY" --arm "B|$REPLAY" -- "${common[@]}" --impl naive
  python3 analysis/compare.py "$OUT/aa_naive.csv" --base A --cand B \
    --expect p50_ns=same --expect p99_ns=same --expect p999_ns=same || status=1
else
  echo "== 2. skipped: no naive pipeline yet"
fi

echo "== 3. 200 us stall on every decision"
bench --out "$OUT/stall_all.csv" --arm "base|$REPLAY" --arm "stall|$REPLAY|--stall-us 200 --stall-prob 1" \
  -- "${common[@]}" --impl spin
python3 analysis/compare.py "$OUT/stall_all.csv" --base base --cand stall --expect p50_ns=slower || status=1

echo "== 4. 200 us stall on 1% of decisions"
bench --out "$OUT/stall_1pct.csv" --arm "base|$REPLAY" --arm "stall|$REPLAY|--stall-us 200 --stall-prob 0.01" \
  -- "${common[@]}" --impl spin
python3 analysis/compare.py "$OUT/stall_1pct.csv" --base base --cand stall \
  --expect p50_ns=same --expect p999_ns=slower || status=1

[ "$status" -eq 0 ] && echo "harness validation: PASS" || echo "harness validation: FAIL"
exit "$status"
