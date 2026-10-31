// Every tunable of the pipeline in one place, with the defaults derived in
// docs/budget.md (directive/p0.md). A value changed here changes what the
// latency numbers mean, so runs record the config they used.
#pragma once

namespace avl {

struct Config {
  // Forward sector processed per decision. 60 deg of a 10 Hz scan is
  // 16.7 ms of data age instead of 100 ms for a full frame
  double sector_deg = 60.0;

  // Region of interest in the sensor frame, metres. x is forward
  float roi_x_min = 1.0f;  // drop the ego vehicle's own body returns
  float roi_x_max = 40.0f;
  float roi_y_abs = 10.f;

  // Ground plane fitting (GPF, Zermas et al., ICRA 2017)
  int gpf_iterations = 3;
  int gpf_lpr_count = 200;         // lowest points averaged for the seed height
  float gpf_seed_thresh_m = 0.4f;  // seeds: z < lpr_mean + this
  float gpf_dist_thresh_m = 0.2f;  // ground: |distance to plane| < this

  // Occupancy grid over (x, y, height-above-ground), metres
  float voxel_m = 0.2f;
  float grid_x_min = 0.0f, grid_x_max = 40.0f;
  float grid_y_min = -10.0f, grid_y_max = 10.0f;
  float grid_h_min = 0.0f, grid_h_max = 2.4f;

  // Forward corridor and braking physics (budget §1)
  float corridor_half_width_m = 1.2f;  // half vehicle width + lateral margin
  float obstacle_h_min_m = 0.3f;       // below this is curb/debris, not a brake event
  float obstacle_h_max_m = 2.0f;       // above this is an overpass/sign
  int min_points_per_voxel = 2;        // single returns are treated as noise
  float ego_speed_mps = 13.9f;         // 50 km/h; replay has no CAN feed
  float decel_mps2 = 6.0f;
  float system_delay_s = 0.30f;  // sensing + compute + actuator build-up
  float stop_margin_m = 1.0f;

  // Record a Trace of each decision's internals (tests and tools only)
  bool trace = false;

  // Harness-only: busy-wait per decision for the synthetic "spin" pipeline
  double spin_us = 50.0;

  // Helper threads for the "mt" pipeline, in addition to
  // the calling thread. Helper i is pinned to first_helper_cpu + i when >= 0
  int helpers = 1;
  int first_helper_cpu = -1;
};

}  // namespace avl
