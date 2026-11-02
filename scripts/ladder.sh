#!/usr/bin/env bash
# Runs the optimization ladder: every rung as an arm of one interleaved,
# randomized bench.py session (link orders from build/link-*), then compares
# each rung with the one below it and with naive.
#   usage: CPU=3 HELPER_CPU=4 RUNS=10 scripts/ladder.sh [sector_deg]
set -euo pipefail
cd "$(dirname "$0")/.." || exit 1
SECTOR=${1:-60}
CPU=${CPU:-3}
HELPER_CPU=${HELPER_CPU:-4}
RUNS=${RUNS:-10}
DECISIONS=${DECISIONS:-5000}
SPEED=${SPEED:-10}
RUNGS=(naive nth soa simd mt)
CAP=data/OS-2-128-U1_v2.3.0_1024x10
OUT=results/ladder_${SECTOR}.csv
ls build/link-*/bench/replay > /dev/null || scripts/build_link_orders.sh
rm -f "$OUT"
arms=()
for r in "${RUNGS[@]}"; do arms+=(--arm "$r|build/link-*/bench/replay|--impl $r"); done
python3 analysis/bench.py --out "$OUT" --runs "$RUNS" "${arms[@]}" -- \
  --pcap "$CAP.pcap" --meta "$CAP.json" --speed "$SPEED" --decisions "$DECISIONS" \
  --sector-deg "$SECTOR" --cpu "$CPU" --fifo 80 --helpers 1 --helper-cpu "$HELPER_CPU"
# 5 comparisons x 3 quantiles = 15 chances of a false "faster": 0.05 / 15.
ALPHA=0.0033
prev=naive
for r in "${RUNGS[@]:1}"; do
  echo
  echo "### $r vs $prev"
  python3 analysis/compare.py "$OUT" --base "$prev" --cand "$r" --alpha "$ALPHA"
  prev=$r
done
echo
echo "### mt vs naive (cumulative)"
python3 analysis/compare.py "$OUT" --base naive --cand mt --alpha "$ALPHA"
