#!/usr/bin/env python3
"""Per-arm medians of the per-run quantiles, checked against the budget.

The budget comes from analysis/budget.py (docs/budget.md): p50 <= 1 ms,
p99 <= 2 ms, p99.9 <= 4 ms. A run set passes only if the median over runs
AND the worst run are inside the budget; the worst run is reported because a
budget is a promise about every run, not about a typical one.

  python3 analysis/summary.py results/ladder_60.csv [more.csv ...]
"""
import csv
import sys
from collections import defaultdict

import numpy as np

BUDGET_US = {"p50_ns": 1000, "p99_ns": 2000, "p999_ns": 4000}


def main():
    runs = defaultdict(lambda: defaultdict(list))
    for path in sys.argv[1:]:
        for row in csv.DictReader(open(path)):
            for m in BUDGET_US:
                runs[row["label"]][m].append(float(row[m]) / 1e3)
    print("| arm | runs | p50 us (worst) | p99 us (worst) | p99.9 us (worst) |  budget |")
    print("|---|---|---|---|---|---|")
    for arm, m in runs.items():
        cells, ok = [], True
        for k, limit in BUDGET_US.items():
            med, worst = np.median(m[k]), max(m[k])
            ok &= worst <= limit
            cells.append(f"{med:.1f} ({worst:.1f})")
        print(f"| {arm} | {len(m['p50_ns'])} | {' | '.join(cells)} | {'PASS' if ok else 'FAIL'} |") 

if __name__ == "__main__":
    main()
