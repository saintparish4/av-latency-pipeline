# av-latency-pipeline

A small C++20 pipeline that turns raw Ouster lidar UDP packets into a
brake / no-brake decision: packet decode, ground segmentation, voxel
occupancy and a forward-corridor time-to-collision check. The point is the
measurement around it: a latency budget derived from braking physics
([docs/budget.md](docs/budget.md)), a validated benchmark harness, and an
optimization ladder with evidence for every step.

**Status:** work in progress. No latency results yet.