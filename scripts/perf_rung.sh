#!/usr/bin/env bash
# Counter evidence for one ladder rung: perf stat (IPC, cache, TLB, branch),
# top-down level 1 where the CPU supports it, and a call-graph profile whose
# top entries are where the next rung's hypothesis comes from.
#   usage: scripts/perf_rung.sh <impl> [cpu] [sector_deg]
# Closed loop (--speed 0) so the counters measure work, not idle spinning.
set -euo pipefail
cd "$(dirname "$0")/.." || exit 1
IMPL=${1:?impl}
CPU=${2:-3}
SECTOR=${3:-60}
CAP=data/OS-2-128-U1_v2.3.0_1024x10
N=20000
RUN=(taskset -c "$CPU" build/rel/bench/replay --pcap "$CAP.pcap" --meta "$CAP.json" --impl "$IMPL"
  --speed 0 --decisions "$N" --sector-deg "$SECTOR" --helper-cpu "$((CPU + 1))")
mkdir -p results/perf
OUT=results/perf/$IMPL
EV=cycles,instructions,branches,branch-misses,L1-dcache-loads,L1-dcache-load-misses,LLC-loads,LLC-load-misses,dTLB-loads,dTLB-load-misses
perf stat -x, -e "$EV" -o "$OUT.stat.csv" -- "${RUN[@]}" > /dev/null
perf stat --topdown -o "$OUT.topdown.txt" -- "${RUN[@]}" > /dev/null 2>&1 ||
  echo "topdown not supported on this CPU/kernel (needs Intel, or AMD Zen 4+ with perf >= 6.2)" > "$OUT.topdown.txt"
perf record -q -g --call-graph=fp -o "$OUT.data" -- "${RUN[@]}" > /dev/null
perf report -i "$OUT.data" --stdio --no-children --percent-limit 2 --sort symbol 2> /dev/null |
  grep -E "^ +[0-9]" > "$OUT.top.txt" || true

python3 - "$OUT.stat.csv" "$N" "$IMPL" << 'PY'
import csv, sys
vals = {}
for row in csv.reader(l for l in open(sys.argv[1]) if l.strip() and not l.startswith("#")):
    try:
        vals[row[2].split(":")[0]] = float(row[0])
    except (ValueError, IndexError):
        pass  # "<not supported>" counters
n = float(sys.argv[2])
g = lambda k: vals.get(k, float("nan"))
print(f"{sys.argv[3]}: IPC {g('instructions') / g('cycles'):.2f}, "
      f"{g('cycles') / n / 1e3:.1f}k cycles/decision, "
      f"L1d miss {100 * g('L1-dcache-load-misses') / g('L1-dcache-loads'):.2f}%, "
      f"LLC miss {100 * g('LLC-load-misses') / max(g('LLC-loads'), 1):.2f}%, "
      f"dTLB miss {100 * g('dTLB-load-misses') / g('dTLB-loads'):.3f}%, "
      f"branch miss {100 * g('branch-misses') / g('branches'):.2f}%")
PY
echo "top symbols:"
head -8 "$OUT.top.txt"
tail -6 "$OUT.topdown.txt"
