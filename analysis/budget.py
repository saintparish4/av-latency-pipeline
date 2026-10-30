#!/usr/bin/env python3
"""Derive the latency budget from braking physics (docs/budget.md).

Every number in the budget comes out of this script, so changing an
assumption (speed, deceleration, actuator delay) re-derives the targets
instead of leaving a stale constant in the write-up. The defaults must match
src/config.hpp; tests/pipeline_test.cpp checks the stopping distance.
"""
import argparse

KMH = 1 / 3.6


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--speed-mps", type=float, default=13.9, help="design speed, 50 km/h urban AEB")
    ap.add_argument("--decel", type=float, default=6.0, help="m/s^2, ~0.6 g on dry asphalt")
    ap.add_argument("--actuator-s", type=float, default=0.25, help="brake pressure build-up")
    ap.add_argument("--frame-hz", type=float, default=10.0)
    ap.add_argument("--sector-deg", type=float, default=60.0)
    ap.add_argument("--voxel-m", type=float, default=0.2)
    ap.add_argument("--roi-m", type=float, default=40.0)
    ap.add_argument("--margin-m", type=float, default=1.0)
    ap.add_argument("--system-delay-s", type=float, default=0.30, help="Config::system_delay_s")
    a = ap.parse_args()

    v = a.speed_mps
    kmh = v / KMH
    brake = v * v / (2 * a.decel)
    stop = v * a.system_delay_s + brake + a.margin_m
    frame_s = 1 / a.frame_hz
    sector_age = frame_s * a.sector_deg / 360

    print(f"## 1. Stopping distance at {kmh:.0f} km/h ({v:.2f} m/s), a = {a.decel} m/s^2\n")
    print("| term | value |")
    print("|---|---|")
    print(f"| braking distance v^2/2a | {brake:.2f} m |")
    print(f"| distance during system delay ({a.system_delay_s * 1e3:.0f} ms) | {v * a.system_delay_s:.2f} m |")
    print(f"| margin | {a.margin_m:.2f} m |")
    print(f"| brake if obstacle nearer than | {stop:.2f} m |")
    print(f"| ROI slack (ROI {a.roi_m:.0f} m - stop distance) | {a.roi_m - stop:.2f} m |")

    print("\n## 2. What latency costs, in distance\n")
    print("| latency | 30 km/h | 50 km/h | 90 km/h | 130 km/h |")
    print("|---|---|---|---|---|")
    for ms in (1, 5, 10, 100):
        cells = " | ".join(f"{s * KMH * ms / 1e3 * 100:.1f} cm" for s in (30, 50, 90, 130))
        print(f"| {ms} ms | {cells} |")

    print("\n## 3. Where the delay goes (data age vs compute)\n")
    print("| stage | budget | distance at design speed |")
    print("|---|---|---|")
    rows = [
        (f"sector scan ({a.sector_deg:.0f} deg of {a.frame_hz:.0f} Hz)", sector_age),
        ("full-frame scan, for comparison (not used)", frame_s),
        ("actuator pressure build-up (assumed)", a.actuator_s),
    ]
    for name, s in rows:
        print(f"| {name} | {s * 1e3:.1f} ms | {v * s:.2f} m |")
    compute_p999 = a.voxel_m / 3 / v
    rest = a.system_delay_s - sector_age - a.actuator_s
    print(f"| left for transport + compute inside system_delay | {rest * 1e3:.1f} ms | {v * rest:.2f} m |")

    print("\n## 4. Compute targets (stated before any code is measured)\n")
    print(f"Rule: compute latency at p99.9 may cost at most one third of a voxel "
          f"({a.voxel_m / 3 * 100:.1f} cm) of travel at design speed, so latency can never be "
          f"what flips a decision the grid resolution would not. That is "
          f"{compute_p999 * 1e3:.2f} ms, rounded down to whole milliseconds:\n")
    print("| quantile | target | why |")
    print("|---|---|---|")
    print("| p50 | <= 1 ms | 1% duty cycle of one core at 10 Hz; leaves the core for decode and I/O |")
    print("| p99 | <= 2 ms | tail no worse than 2x median before OS jitter is added |")
    target = int(compute_p999 * 1e3)
    print(f"| p99.9 | <= {target} ms | {v * target / 1e3 * 100:.1f} cm at {kmh:.0f} km/h: inside the "
          f"voxel/3 rule and the {rest * 1e3:.0f} ms system-delay slack |")


if __name__ == "__main__":
    main()