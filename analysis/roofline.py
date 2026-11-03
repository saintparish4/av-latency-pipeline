#!/usr/bin/env python3
"""Plot the measured roofline and place each kernel on it.

Inputs are the CSVs from tools/roofline_probe and tools/kernel_bench. The
attainable ceiling for a kernel is min(peak FLOPs of its precision,
intensity x bandwidth of the cache level its working set lives in). The
sector working set (~12k points x 20 B) fits in L2, so L2 is the honest
ceiling; DRAM is drawn for context only.

  python3 analysis/roofline.py results/roofline_probe.csv results/kernels.csv \\
      --out results/roofline.png --level L2
"""
import argparse
import csv

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("probe")
    ap.add_argument("kernels")
    ap.add_argument("--out", default="results/roofline.png")
    ap.add_argument("--level", default="L2", help="cache level that bounds the kernels")
    a = ap.parse_args()

    bw, peak, other = {}, {}, {}
    for row in csv.DictReader(open(a.probe)):
        target = {"bandwidth_gbps": bw, "peak_gflops": peak}.get(row["kind"], other)
        target[row["name"] if row["kind"] != "flops_per_cycle" else "fpc_" + row["name"]] = float(row["value"])
    if "core" in other:
        print(f"clock {other['core']:.2f} GHz during the probe; "
              f"float {other.get('fpc_float', float('nan')):.1f} FLOP/cycle, "
              f"double {other.get('fpc_double', float('nan')):.1f} FLOP/cycle\n")
    kernels = list(csv.DictReader(open(a.kernels)))

    fig, ax = plt.subplots(figsize=(8, 5.5))
    ai = np.logspace(-2, 1.5, 200)
    for prec, style in (("float", "-"), ("double", "--")):
        ax.axhline(peak[prec], color="0.3", ls=style, lw=1)
        ax.text(ai[-1], peak[prec] * 1.05, f"{prec} peak {peak[prec]:.0f} GFLOP/s", ha="right", fontsize=8)
    for level, color in (("L1", "tab:green"), ("L2", "tab:blue"), ("L3", "tab:purple"), ("DRAM", "tab:red")):
        if level in bw:
            ax.plot(ai, np.minimum(ai * bw[level], peak["float"]), color=color, lw=1,
                    label=f"{level} {bw[level]:.0f} GB/s")

    print(f"| kernel | precision | intensity (FLOP/B) | achieved GFLOP/s | {a.level} ceiling GFLOP/s | % of ceiling |")
    print("|---|---|---|---|---|---|")
    for k in kernels:
        x, y = float(k["intensity"]), float(k["gflops"])
        ceiling = min(peak[k["precision"]], x * bw[a.level])
        ax.plot(x, y, "o", color="black")
        ax.annotate(k["kernel"], (x, y), textcoords="offset points", xytext=(5, -10), fontsize=9)
        print(f"| {k['kernel']} | {k['precision']} | {x:.3f} | {y:.2f} | {ceiling:.2f} | {100 * y / ceiling:.0f}% |")

    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlabel("arithmetic intensity (FLOP / byte)")
    ax.set_ylabel("GFLOP/s")
    ax.set_title("Single-core roofline, sector kernels")
    ax.legend(loc="lower right", fontsize=8)
    ax.grid(True, which="both", alpha=0.2)
    fig.tight_layout()
    fig.savefig(a.out, dpi=150)
    print(f"\nwrote {a.out}")


if __name__ == "__main__":
    main()