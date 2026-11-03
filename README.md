# av-latency-pipeline

[![ci](https://github.com/saintparish4/av-latency-pipeline/actions/workflows/ci.yml/badge.svg)](https://github.com/saintparish4/av-latency-pipeline/actions/workflows/ci.yml)

A small C++20 pipeline that turns raw Ouster lidar UDP packets into a
brake / no-brake decision: packet decode, ground segmentation, voxel
occupancy and a forward-corridor time-to-collision check. The point is the
measurement around it: a latency budget derived from braking physics
([docs/budget.md](docs/budget.md)), a validated benchmark harness, and an
optimization ladder with evidence for every step.

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

## Results so far

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
Latency numbers for the pipeline will come from an isolated native Linux core.

## Status

- [x] Latency budget from braking physics
- [x] Replay harness with coordinated-omission-free timing
- [x] Naive baseline, labelled scenes, differential tests against the Ouster SDK and Open3D
- [ ] Harness validation on an isolated native Linux core
- [ ] Optimization ladder (all rungs implemented, CUDA rung measured; isolated measurement pending)
- [ ] Roofline (probe, kernel bench and plot implemented; isolated measurement pending)
- [x] Sanitizers, fuzzing and CI
- [ ] External validation with NIC hardware timestamps

## Data

Tests use a capture from the Ouster SDK test suite (BSD-3-Clause), fetched and
checksum-verified by `scripts/fetch_data.sh`. It is not committed here.

## License

MIT. See [LICENSE](LICENSE).