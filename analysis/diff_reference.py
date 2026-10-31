#!/usr/bin/env python3
"""Differential tests of the C++ pipeline against independent references.

Three comparisons, each matched to how strict the reference allows:

  decode  vs Ouster SDK XYZLut (float64), per pixel. Same function, so the
          bound is in float32 ULPs of the point's norm (the axis components
          can legitimately cancel to near zero, so per-axis ULPs mislead).
  ground  vs an independent float64 re-implementation of GPF whose covariance
          step is Open3D's compute_mean_and_covariance. Same algorithm, so
          labels must agree except for points within rounding of a threshold.
          Open3D's RANSAC segment_plane is printed for context but is NOT an
          oracle: on this frame it fits a tilted plane through road and
          embankment (directive/p2.md, "Gaps found").
  voxel   vs Open3D VoxelGrid on our non-ground (x, y, h) points. Same
          function in float64; any disagreement must be a point within float
          rounding of a voxel face, and anything else is a failure.

Exit status is non-zero if any check fails, so CI can run it directly.
Usage: python -I analysis/diff_reference.py --build build/rel --data data
"""
import argparse
import os
import subprocess
import sys
import tempfile

import numpy as np

CAPTURE = "OS-2-128-U1_v2.3.0_1024x10"
MAX_ULP = 8          # decode bound, float32 ULPs of |p|
MAX_ANGLE_DEG = 0.01  # ground plane vs float64 GPF
MIN_AGREE = 0.999     # ground label agreement vs float64 GPF
GPF = dict(lpr=200, seed=0.4, dist=0.2, iters=3)  # must match Config defaults
FACE_EPS = 1e-4      # metres: "on a voxel face" for the voxel check
VOXEL, BOUNDS_MIN, DIMS = 0.2, np.array([0.0, -10.0, 0.0]), np.array([200, 100, 12])


def run(cmd):
    subprocess.run(cmd, check=True)


def ulp_distance_of_norm(ours, ref):
    """|ours - ref| in units of the float32 spacing at |ref|."""
    spacing = np.spacing(np.linalg.norm(ref, axis=1).astype(np.float32)).astype(np.float64)
    return np.linalg.norm(ours - ref, axis=1) / spacing


def check_decode(dump, pcap, meta, tmp):
    from ouster.sdk import open_source
    from ouster.sdk import core

    out = os.path.join(tmp, "decode.csv")
    run([dump, "decode", pcap, meta, out])
    ours = np.loadtxt(out, delimiter=",", skiprows=1)

    src = open_source(pcap, meta=[meta])
    lut = core.XYZLut(src.sensor_info[0])
    frame = next(iter(src))[0]
    rng = frame.field("RANGE")
    xyz = lut(frame)  # rows x columns x 3, columns indexed by measurement id
    rows, mids = np.nonzero(rng)

    ref = {(int(m), int(r)): xyz[r, m] for r, m in zip(rows, mids)}
    keys = [(int(m), int(r)) for m, r in ours[:, :2]]
    missing = len(ref) - len(set(keys) & set(ref))
    ref_pts = np.array([ref[k] for k in keys if k in ref])
    our_pts = np.array([row[3:6] for row, k in zip(ours, keys) if k in ref])
    ulps = ulp_distance_of_norm(our_pts, ref_pts)
    ok = missing == 0 and len(keys) == len(ref) and ulps.max() <= MAX_ULP
    print(f"decode: {len(keys)} returns (sdk {len(ref)}, missing {missing}); "
          f"ULP of |p|: p50 {np.median(ulps):.2f}, p99.9 {np.percentile(ulps, 99.9):.2f}, "
          f"max {ulps.max():.2f} (bound {MAX_ULP}) -> {'PASS' if ok else 'FAIL'}")
    return ok


def gpf_reference(xyz):
    """GPF (Zermas et al. 2017) in float64, covariance from Open3D."""
    import open3d as o3d

    lpr = np.sort(xyz[:, 2])[:GPF["lpr"]].mean()
    ground = xyz[:, 2] < lpr + GPF["seed"]
    plane = None
    for _ in range(GPF["iters"]):
        sel = o3d.geometry.PointCloud(o3d.utility.Vector3dVector(xyz[ground]))
        mean, cov = sel.compute_mean_and_covariance()
        n = np.linalg.eigh(cov)[1][:, 0]
        n *= np.sign(n[2])
        plane = np.append(n, -np.dot(n, mean))
        ground = np.abs(xyz @ n + plane[3]) < GPF["dist"]
    return ground, plane


def check_ground_and_voxels(dump, pcap, meta, tmp):
    import open3d as o3d

    prefix = os.path.join(tmp, "trace")
    run([dump, "trace", pcap, meta, "naive", prefix])
    pts = np.loadtxt(prefix + ".points.csv", delimiter=",", skiprows=1)
    plane = np.loadtxt(prefix + ".plane.csv", delimiter=",", skiprows=1)
    xyz, h, ground = pts[:, :3], pts[:, 3], pts[:, 4].astype(bool)

    ref_ground, ref_plane = gpf_reference(xyz)
    angle = np.degrees(np.arccos(min(1.0, abs(float(np.dot(ref_plane[:3], plane[:3]))))))
    agree = np.mean(ref_ground == ground)
    ok_g = angle <= MAX_ANGLE_DEG and agree >= MIN_AGREE
    print(f"ground: {ground.sum()}/{len(xyz)} ground; vs float64 GPF angle {angle:.5f} deg, "
          f"d diff {abs(ref_plane[3] - plane[3]):.2e} m, label agreement {agree:.5f} -> "
          f"{'PASS' if ok_g else 'FAIL'}")

    o3d.utility.random.seed(1)
    cloud = o3d.geometry.PointCloud(o3d.utility.Vector3dVector(xyz))
    model, inliers = cloud.segment_plane(distance_threshold=GPF["dist"], ransac_n=3, num_iterations=2000)
    n_r = np.array(model[:3]) / np.linalg.norm(model[:3])
    r_angle = np.degrees(np.arccos(min(1.0, abs(float(np.dot(n_r, plane[:3]))))))
    print(f"  (context, not a check) Open3D RANSAC: {len(inliers)} inliers, {r_angle:.2f} deg from ours")

    vox = np.loadtxt(prefix + ".voxels.csv", delimiter=",", skiprows=1, ndmin=2)
    ours = {tuple(int(v) for v in row[:3]) for row in vox}
    xyh = np.column_stack([xyz[~ground, 0], xyz[~ground, 1], h[~ground]])
    vg = o3d.geometry.VoxelGrid.create_from_point_cloud_within_bounds(
        o3d.geometry.PointCloud(o3d.utility.Vector3dVector(xyh)), VOXEL,
        BOUNDS_MIN, BOUNDS_MIN + DIMS * VOXEL)
    ref = {tuple(int(i) for i in v.grid_index) for v in vg.get_voxels()}
    ref = {v for v in ref if all(0 <= v[a] < DIMS[a] for a in range(3))}  # Open3D keeps out-of-bounds
    diff = ours ^ ref
    rel = (xyh - BOUNDS_MIN) / VOXEL
    near_face = np.any(np.abs(rel - np.round(rel)) * VOXEL < FACE_EPS, axis=1)
    cells = np.floor(rel).astype(int)
    unexplained = [v for v in diff
                   if not np.any(near_face & np.all(np.abs(cells - np.array(v)) <= 1, axis=1))]
    ok_v = not unexplained
    print(f"voxel: {len(ours)} occupied (Open3D {len(ref)}); {len(diff)} differ, "
          f"{len(diff) - len(unexplained)} explained by points within {FACE_EPS} m of a face "
          f"-> {'PASS' if ok_v else 'FAIL'}")
    return ok_g and ok_v


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default="build/rel")
    ap.add_argument("--data", default="data")
    a = ap.parse_args()
    dump = os.path.join(a.build, "tools", "avl_dump")
    pcap = os.path.join(a.data, CAPTURE + ".pcap")
    meta = os.path.join(a.data, CAPTURE + ".json")
    with tempfile.TemporaryDirectory() as tmp:
        ok = check_decode(dump, pcap, meta, tmp)
        ok = check_ground_and_voxels(dump, pcap, meta, tmp) and ok
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()