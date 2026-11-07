# av-latency-pipeline

[![ci](https://github.com/saintparish4/av-latency-pipeline/actions/workflows/ci.yml/badge.svg)](https://github.com/saintparish4/av-latency-pipeline/actions/workflows/ci.yml)

A small C++20 pipeline that turns raw Ouster lidar UDP packets into a
brake / no-brake decision: packet decode, ground segmentation, voxel
occupancy and a forward-corridor time-to-collision check. The point is the
measurement around it: a latency budget derived from braking physics
([docs/budget.md](docs/budget.md)), a validated benchmark harness, and an
optimization ladder with evidence for every step.

The full write-up is [docs/WRITEUP.md](docs/WRITEUP.md): budget, harness
validation, ladder, roofline and what didn't work.

## Build and test

Needs Ubuntu 24.04 with clang-18, lld-18, cmake and ninja.

```bash
scripts/fetch_data.sh
cmake --preset rel
cmake --build --preset rel
ctest --preset rel
```

The same tests under sanitizers, and a time-boxed fuzz run (seconds per target):

```bash
cmake --preset asan && cmake --build --preset asan && ctest --preset asan
cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan
cmake --preset fuzz && cmake --build --preset fuzz && scripts/fuzz.sh 60
```

CI runs all of these on every push, plus format and lint checks. It checks
correctness only: shared runners are too noisy to measure latency.

## Reproduce

One command runs the correctness check (build, all tests, ASan, TSan,
differential tests, a short fuzz) inside a pinned toolchain:

```bash
docker build -t avl .
docker run --rm --security-opt seccomp=unconfined -v "$PWD":/src -w /src avl \
  scripts/reproduce.sh --check
```

TSan needs `seccomp=unconfined` because it disables ASLR, which Docker's
default profile refuses. The full measurement run, `scripts/reproduce.sh`
with no flag, belongs on a native Linux host with an isolated core. Results
from a host that fails `scripts/rt_check.sh` are labelled NOT REPORTABLE.

## Results so far

Measured on a WSL2 dev box (Ryzen 5 5625U), so none of these numbers is
REPORTABLE. Each rung is timed in one interleaved, randomized session over
10 runs and 4 link orders. Speedups are p50 against the rung below, with
95% bootstrap CIs:

| Rung | Change | p50 | Speedup |
|---|---|---|---|
| naive | baseline | 775 µs | |
| nth | `nth_element` instead of `sort` | 243 µs | 3.19× [3.16, 3.27] |
| soa | struct of arrays | 219 µs | 1.11× [1.09, 1.12] |
| simd | Highway kernels | 141 µs | 1.55× [1.53, 1.58] |
| mt | fork-join, one helper | 125 µs | 1.13× [1.10, 1.17] |

Naive to mt is 6.20× at p50. The tail is not judged: on this host the
harness missed an injected 1% stall at p99.9, and OS jitter (cyclictest max
67.6 ms) dominates every rung's p99.9. Details are in the
[write-up](docs/WRITEUP.md).

All 39 tests pass under `rel`, `asan` (ASan + UBSan) and `tsan`, including
equivalence and labelled-scene checks for each ladder rung. Two libFuzzer
targets, for the pcap parser and for every pipeline fed arbitrary packets,
run in CI with no crashes.

One CUDA kernel (packet decode) against the CPU SIMD path. GPU output is
bit-identical to the CPU output in both modes. Median of 50,000 repetitions:

| Workload | GPU total | CPU SIMD | GPU slower by |
|---|---|---|---|
| 1 packet (767 points) | 38.85 µs | 0.75 µs | ~52× |
| 12 packets (11,557 points) | 159.22 µs | 19.60 µs | ~8× |

Measured on a shared Runpod host: RTX 2000 Ada, PCIe Gen 4 x4, AMD EPYC 7352,
nvcc 12.8.93. This host was not isolated, so these numbers are a CPU vs GPU
comparison on that machine and are not comparable with the latency ladder.
Reportable latency numbers need an isolated native Linux core.

## Status

- [x] Latency budget from braking physics
- [x] Replay harness with coordinated-omission-free timing
- [x] Naive baseline, labelled scenes, differential tests against the Ouster SDK and Open3D
- [ ] Harness validation on an isolated native Linux core
- [ ] Optimization ladder (all rungs measured on the dev box and CUDA on Runpod; compiler variants and isolated measurement pending)
- [ ] Roofline (measured on the dev box; isolated measurement pending)
- [x] Sanitizers, fuzzing and CI
- [x] One-command reproduction in a pinned Docker image
- [x] Dev-box write-up ([docs/WRITEUP.md](docs/WRITEUP.md))
- [ ] External validation with NIC hardware timestamps (tools written and tested over loopback; two-machine run pending)

## Data

Tests use a capture from the Ouster SDK test suite (BSD-3-Clause), fetched and
checksum-verified by `scripts/fetch_data.sh`. It is not committed here.

## License

MIT. See [LICENSE](LICENSE).