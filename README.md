# av-latency-pipeline

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

## Status

- [x] Latency budget from braking physics
- [x] Replay harness with coordinated-omission-free timing
- [x] Naive baseline, labelled scenes, differential tests against the Ouster SDK and Open3D
- [ ] Harness validation on an isolated native Linux core
- [ ] Optimization ladder
- [ ] Roofline
- [ ] Sanitizers, fuzzing and CI
- [ ] External validation with NIC hardware timestamps

## Data

Tests use a capture from the Ouster SDK test suite (BSD-3-Clause), fetched and
checksum-verified by `scripts/fetch_data.sh`. It is not committed here.

## License

MIT. See [LICENSE](LICENSE).