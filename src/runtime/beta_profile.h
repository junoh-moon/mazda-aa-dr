#ifndef MX5_AA_DR_BETA_PROFILE_H
#define MX5_AA_DR_BETA_PROFILE_H
#include "core/dr_core.h"
#include <stdint.h>

namespace mx5 { namespace runtime {

// The single source of the v1.0 beta (BETA domain) constants. Pipeline, bridge,
// tests and the replay harness must all read these values from here.
// Sources: validation/ACCURACY_RULE_2026-10-05.md ("confirmed rules" 1-5) and
// validation/ASSIST_BETA_DESIGN_2026-10-05.md decisions 3, 4 and 7. They are
// replay-calibrated values from one drive, not worst-case bounds and not a
// physical sensor qualification. BETA output stays MODEL evidence.
struct BetaProfile {
    // Rule 1 / decision 3: reported accuracy =
    //   e0 + sv*t + h0*D + k*integral(v*tau) (the core budget with these inputs).
    double anchor_error_m;        // e0
    double heading_error_rad;     // h0
    double yaw_error_rad_s;       // k
    double speed_error_mps;       // sv
    // Core limits. Exceeding error_max_m is a core LIMIT failure (no clamp).
    double error_max_m, duration_max_s, distance_max_m;
    // Rule 3 anchor gate (only the BETA core uses it).
    double anchor_speed_min_kmh;  // GPS speed of the anchor fix
    double anchor_speed_max_kmh;  // rule 2 validated range upper end
    double previous_speed_min_kmh;
    double course_step_max_deg;   // consecutive GPS course difference
    double yaw_quiet_max_rad_s;   // |yaw| over the preceding window
    uint64_t yaw_quiet_window_ns;
    double wheel_gps_speed_max_diff_kmh;
    uint64_t fix_pair_max_ns;     // "consecutive" fixes: at most this far apart
    // Rule 4: fixed profile yaw zero; stationary auto-bias is never applied.
    double yaw_zero;
    // Rule 5 heading budget: h0 + k*t + rotation_budget_per_rad*sum|rotation|.
    double rotation_budget_per_rad, heading_budget_max_rad;
    // Decision 4: accuracy = budget + (v+sv)*lease, valid_until = frontier+lease.
    uint64_t lease_ns;
    double accuracy_max_m;
};

inline BetaProfile beta_profile() {
    BetaProfile p;
    p.anchor_error_m=20.0; p.heading_error_rad=0.03;
    p.yaw_error_rad_s=0.002; p.speed_error_mps=0.3;
    // 60 s is the validated outage length; 1000 m = 60 km/h for 60 s. The
    // 40 m budget ends the window much earlier in every validated case.
    p.error_max_m=40.0; p.duration_max_s=60.0; p.distance_max_m=1000.0;
    p.anchor_speed_min_kmh=20.0; p.anchor_speed_max_kmh=60.0;
    p.previous_speed_min_kmh=15.0; p.course_step_max_deg=3.0;
    p.yaw_quiet_max_rad_s=0.05; p.yaw_quiet_window_ns=2000000000ULL;
    p.wheel_gps_speed_max_diff_kmh=4.0; p.fix_pair_max_ns=2000000000ULL;
    p.yaw_zero=2047.0;
    p.rotation_budget_per_rad=0.10;
    p.heading_budget_max_rad=20.0*3.14159265358979323846/180.0;
    p.lease_ns=500000000ULL; p.accuracy_max_m=40.0;
    return p;
}

// Core configuration for the BETA core: the shared defaults with the BETA
// budget rates and limits. Stop/time/physical guards keep the core defaults.
inline mx5_dr_config beta_core_config(const BetaProfile& p) {
    mx5_dr_config c=mx5_dr_default_config();
    c.speed_error_mps=p.speed_error_mps; c.yaw_error_rad_s=p.yaw_error_rad_s;
    c.error_max_m=p.error_max_m; c.duration_max_s=p.duration_max_s;
    c.distance_max_m=p.distance_max_m;
    return c;
}

} }
#endif
