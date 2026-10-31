#!/usr/bin/env python3
"""Run interleaved, randomized replay runs for two or more arms.

Each run is one process and one CSV row. Three nuisance variables are
randomized per run so they become noise the statistics average over, rather
than a hidden bias that favours whichever arm got lucky:

  * run order: arms are interleaved in a shuffled order, so slow drift
    (thermals, background load) hits every arm equally;
  * environment size: AVL_PAD of 0..4095 bytes shifts the initial stack
    (Mytkowicz et al., ASPLOS'09);
  * link order: an arm's binary may be a glob over several builds made with
    different AVL_LINK_SEED values; one is picked per run.

Arm syntax:  NAME|BINARY_GLOB|EXTRA_ARGS   (EXTRA_ARGS optional)
Everything after "--" is passed to every run.

  python3 analysis/bench.py --out results/aa.csv --runs 10 \\
      --arm 'A|build/rel/bench/replay' --arm 'B|build/rel/bench/replay' \\
      -- --pcap data/X.pcap --meta data/X.json --impl spin --speed 50 --decisions 3000
"""
import argparse
import glob
import os
import random
import shlex
import subprocess
import sys


def main():
    argv = sys.argv[1:]
    common = []
    if "--" in argv:
        i = argv.index("--")
        argv, common = argv[:i], argv[i + 1:]
    ap = argparse.ArgumentParser()
    ap.add_argument("--arm", action="append", required=True)
    ap.add_argument("--runs", type=int, default=10, help="runs per arm")
    ap.add_argument("--out", required=True)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--max-pad", type=int, default=4096)
    a = ap.parse_args(argv)

    rng = random.Random(a.seed)
    arms = []
    for spec in a.arm:
        parts = spec.split("|")
        name, pattern = parts[0], parts[1]
        extra = shlex.split(parts[2]) if len(parts) > 2 else []
        binaries = sorted(glob.glob(pattern))
        if not binaries:
            sys.exit(f"arm {name}: no binary matches {pattern}")
        arms.append((name, binaries, extra))

    schedule = [(arm, i) for arm in arms for i in range(a.runs)]
    rng.shuffle(schedule)
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    for n, ((name, binaries, extra), i) in enumerate(schedule, 1):
        binary = rng.choice(binaries)
        env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"),
               "AVL_PAD": "x" * rng.randrange(a.max_pad)}
        cmd = [binary, *common, *extra, "--label", name, "--seed", str(rng.randrange(1 << 31)),
               "--csv", a.out]
        print(f"[{n}/{len(schedule)}] {name} run {i} ({os.path.relpath(binary)})", flush=True)
        subprocess.run(cmd, env=env, check=True)


if __name__ == "__main__":
    main()
