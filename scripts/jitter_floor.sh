#!/usr/bin/env bash
# Measures the OS wake-up jitter floor on the measured CPU with cyclictest
# (rt-tests). No pipeline result below this floor's p99.9 can be attributed to
# the pipeline, so the write-up reports both side by side.
#   usage: sudo scripts/jitter_floor.sh <cpu> [duration]    e.g. sudo scripts/jitter_floor.sh 3 10m
set -euo pipefail
CPU=${1:-3}
DUR=${2:-10m}
cd "$(dirname "$0")/.." || exit 1
mkdir -p results
OUT=results/cyclictest_cpu$CPU.txt
# -m mlockall, -p95 SCHED_FIFO 95, -a pin, -i 200 us interval, -h histogram up to 1000 us.
cyclictest -m -p95 -a "$CPU" -t1 -i 200 -D "$DUR" -h 1000 -q > "$OUT"
python3 - "$OUT" << 'EOF'
import sys
counts, overflow, maxv = {}, 0, None
for line in open(sys.argv[1]):
    if line.startswith("#"):
        if "Max Latencies" in line:
            maxv = int(line.split(":")[1].split()[0])
        if "Histogram Overflows" in line:
            overflow = int(line.split(":")[1].split()[0])
        continue
    parts = line.split()
    if len(parts) == 2 and parts[0].isdigit():
        counts[int(parts[0])] = int(parts[1])
total = sum(counts.values()) + overflow
def q(p):
    target, acc = p * total, 0
    for us in sorted(counts):
        acc += counts[us]
        if acc >= target:
            return us
    return float("inf")
print(f"cyclictest {sys.argv[1]}: n={total} p50={q(.5)}us p99={q(.99)}us p99.9={q(.999)}us "
      f"p99.99={q(.9999)}us max={maxv}us overflows={overflow}")
EOF
