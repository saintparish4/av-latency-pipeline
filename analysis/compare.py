#!/usr/bin/env python3
"""Compare two arms of a bench.py CSV: speedup with a bootstrap CI and a
Mann-Whitney U test, per latency quantile.

The unit of comparison is the run, not the sample: latencies within one run
are autocorrelated (they share caches, link order and environment), so
treating 5000 samples as independent would produce absurdly narrow CIs. Each
run contributes one value per metric (its own p50, p99, p99.9), and the
statistics are over those per-run values.

  speedup  = median_over_runs(base) / median_over_runs(cand)   (>1 = cand faster)
  CI       = 95% BCa bootstrap of that ratio (scipy.stats.bootstrap)
  p        = two-sided Mann-Whitney U on the per-run values
  verdict  = faster / slower   p < alpha and the whole CI is beyond the ROPE
             equivalent      the whole CI is inside 1 +/- ROPE
             inconclusive    anything else (too noisy to say either way)

The ROPE (region of practical equivalence, default 1%) exists because the
harness is sensitive enough to see effects nobody cares about: moving 1% of
samples up by 200 us shifts the median by a fraction of a percent, and with
tight per-run medians that is "significant". Significant is not the same as
meaningful; the ROPE is where that line is drawn, stated up front.

--expect METRIC=same|faster|slower turns the verdicts into assertions ("same"
accepts equivalent or inconclusive, i.e. "no detected difference"); the
script exits 1 if any expectation fails. That is how validate_harness.sh
proves the harness can see a 200 us stall and does not see an A/A difference.

  python3 analysis/compare.py results/aa.csv --base A --cand B --expect p50_ns=same
"""
import argparse
import csv
import sys
import warnings
from collections import defaultdict

import numpy as np
from scipy import stats

METRICS = ["p50_ns", "p99_ns", "p999_ns"]


def ratio_ci(base, cand, seed):
    def stat(b, c, axis=-1):
        return np.median(b, axis=axis) / np.median(c, axis=axis)

    for method in ("BCa", "percentile"):
        try:
            with warnings.catch_warnings():
                warnings.simplefilter("ignore")  # BCa degeneracy is handled below
                res = stats.bootstrap((base, cand), stat, n_resamples=10000, method=method,
                                      vectorized=True, random_state=seed)
            lo, hi = res.confidence_interval
            if np.isfinite(lo) and np.isfinite(hi):
                return lo, hi
        except ValueError:
            continue  # BCa can fail on degenerate resamples; fall back
    return float("nan"), float("nan")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("--base", required=True)
    ap.add_argument("--cand", required=True)
    ap.add_argument("--metrics", default=",".join(METRICS))
    ap.add_argument("--alpha", type=float, default=0.05)
    ap.add_argument("--rope", type=float, default=0.01, help="practical-equivalence margin")
    ap.add_argument("--expect", action="append", default=[])
    a = ap.parse_args()

    runs = defaultdict(lambda: defaultdict(list))
    with open(a.csv) as f:
        for row in csv.DictReader(f):
            for m in a.metrics.split(","):
                runs[row["label"]][m].append(float(row[m]))
    for arm in (a.base, a.cand):
        if arm not in runs:
            sys.exit(f"no runs labelled {arm} in {a.csv}")

    expect = dict(e.split("=") for e in a.expect)
    failed = []
    print(f"| metric | {a.base} median (us) | {a.cand} median (us) | speedup | 95% CI | MWU p | n | verdict |")
    print("|---|---|---|---|---|---|---|---|")
    for m in a.metrics.split(","):
        base, cand = np.array(runs[a.base][m]), np.array(runs[a.cand][m])
        speedup = np.median(base) / np.median(cand)
        lo, hi = ratio_ci(base, cand, seed=1)
        p = stats.mannwhitneyu(base, cand, alternative="two-sided").pvalue
        if p < a.alpha and lo > 1.0 + a.rope:
            verdict = "faster"
        elif p < a.alpha and hi < 1.0 - a.rope:
            verdict = "slower"
        elif 1.0 - a.rope <= lo and hi <= 1.0 + a.rope:
            verdict = "equivalent"
        else:
            verdict = "inconclusive"
        mark = ""
        if m in expect:
            met = verdict in ("equivalent", "inconclusive") if expect[m] == "same" else verdict == expect[m]
            mark = " (expected)" if met else f" (EXPECTED {expect[m]})"
            if not met:
                failed.append(m)
        print(f"| {m} | {np.median(base) / 1e3:.1f} | {np.median(cand) / 1e3:.1f} | {speedup:.3f} | "
              f"[{lo:.3f}, {hi:.3f}] | {p:.3g} | {len(base)}/{len(cand)} | {verdict}{mark} |")
    if failed:
        print(f"expectation failed for: {', '.join(failed)}")
        sys.exit(1)


if __name__ == "__main__":
    main()
