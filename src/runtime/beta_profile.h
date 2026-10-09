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
// Continuous tunnel tracking (MODEL only; validation/TUNNEL_UNBOUNDED_2026-10-09.md
// and validation/ENGAGEMENT_POLICY_M_2026-10-09.md). Every data-valid fix that
// passes the pair, displacement, HDOP and reverse checks refreshes the
// position (no speed gate, no settling period). A GPS course is used only
// when the GPS displacement over at least BETA_HEADING_BASELINE_M points the
// same way (within BETA_COURSE_CHORD_MAX_DEG): this is the first-heading seed
// condition and filters course-only multipath. A carried heading is blended
// with the GPS course only while the course change since the reference fix
// matches the closed yaw integral (BETA_COURSE_TURN_MAX_DEG +
// BETA_COURSE_TURN_FRACTION x |turn|). A carried heading that disagrees by
// more than BETA_RESYNC_MIN_DEG with BETA_RESYNC_FIXES consecutive
// chord-confirmed courses whose disagreement stays within
// BETA_RESYNC_CONSISTENT_DEG (so the course turns with the yaw) is reseeded
// from the GPS course. An outage starts FRESH from a position at most
// BETA_POSITION_MAX_AGE_NS old, or as a FALLBACK from the dead-reckoned
// estimate inside the bounded envelope of beta_profile_bounded() (60 s since
// the last accepted position, honest budget <= 40 m) when every rejected
// data-valid fix since then agreed with the estimate within the honest
// budget + position_slack_m; otherwise the outage stays stock.
static const double BETA_HEADING_BASELINE_M=3.0;
static const double BETA_COURSE_CHORD_MAX_DEG=30.0;
static const double BETA_COURSE_TURN_MAX_DEG=8.0;
static const double BETA_COURSE_TURN_FRACTION=0.5;
static const double BETA_RESYNC_MIN_DEG=20.0;
static const double BETA_RESYNC_CONSISTENT_DEG=10.0;
static const unsigned BETA_RESYNC_FIXES=5;
static const uint64_t BETA_RESYNC_GAP_NS=2500000000ULL;
static const uint64_t BETA_POSITION_MAX_AGE_NS=3000000000ULL;

struct BetaProfile {
    // Rule 1 / decision 3: reported accuracy =
    //   e0 + sv*t + h0*D + k*integral(v*tau) (the core budget with these inputs).
    double anchor_error_m;        // e0
    double heading_error_rad;     // h0
    double yaw_error_rad_s;       // k
    double speed_error_mps;       // sv
    // Core limits. Exceeding error_max_m is a core LIMIT failure (no clamp).
    double error_max_m, duration_max_s, distance_max_m;
    // Legacy bounded anchor gate (accuracy rule 3; beta_profile() and the
    // replay-only --engagement legacy counterfactual). The continuous tunnel
    // policy reuses only the pair, HDOP, yaw-quiet, wheel/GPS speed and
    // reverse fields (Pipeline::evaluate_beta_tracking); it has no speed,
    // course-step or settling gate.
    double anchor_speed_min_kmh;  // GPS speed of the anchor fix
    double anchor_speed_max_kmh;  // rule 2 validated range upper end
    double previous_speed_min_kmh;
    double course_step_max_deg;   // consecutive GPS course difference
    double yaw_quiet_max_rad_s;   // |yaw| over the preceding window
    uint64_t yaw_quiet_window_ns;
    double wheel_gps_speed_max_diff_kmh;
    uint64_t fix_pair_max_ns;     // a pair (repeated-utc polls skipped): at most this far apart
    // BETA_DECISIONS_2026-10-05.md 3.1-3.2 (pair rules; settling: legacy gate only).
    uint64_t utc_step_max_s;      // strictly increasing utc pair: at most this step
    double utc_mono_tolerance_s;  // |utc step - receipt mono step| (integer utc seconds)
    double anchor_hdop_max;       // HDOP (position horizontal) of the anchor fix
    double displacement_ratio_min, displacement_ratio_max; // pair distance / (v*dt)
    uint64_t anchor_settle_ns;    // consecutive increasing fixes before an anchor
    // 3.4: a latched reverse with wheels above this speed for this long is wrong.
    double reverse_suspect_kmh;
    uint64_t reverse_suspect_ns;
    // Rule 4 / 3.5: fixed yaw zero; stationary auto-bias is never applied.
    double yaw_zero;
    // Rule 5 heading budget: h0 + k*t + rotation_budget_per_rad*sum|rotation|.
    // 3.3: the same heading budget hb(tau) drives the position budget, so the
    // rotation part adds integral(v * rotation_budget_per_rad * R(tau)) to it.
    double rotation_budget_per_rad, heading_budget_max_rad;
    // Decision 4: accuracy = budget + (v+sv)*lease, valid_until = frontier+lease.
    uint64_t lease_ns;
    double accuracy_max_m;
    // Owner decision 2026-10-09 (validation/TUNNEL_UNBOUNDED_2026-10-09.md):
    // the LOST episode is not ended by the time/distance/error/heading budget.
    // The honest budget keeps being computed and journaled; the REPORTED
    // accuracy is min(honest budget, accuracy_max_m). Only GPS return, sensor
    // silence, a latch/session fault or a core fault end the replacement.
    bool unbounded;
    // Continuous BETA position/heading maintenance, independent of the legacy
    // bounded anchor gates. Speed weights course confidence; it never vetoes
    // an entry. These are MODEL heuristics, not qualified error bounds.
    // unbounded && !continuous_anchor is a replay-only counterfactual
    // (replay_beta --engagement legacy); no product profile selects it.
    bool continuous_anchor;
    double course_weight_kmh, course_innovation_rad, course_correction_rad;
    double position_slack_m;
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
    // 2.5 s (was 2.0 s, 2026-10-08): the OEM POSITION polls the hook sees come
    // about every 1.0 s (0.9-1.1 s) while the fix utc advances at 1 Hz on its
    // own phase, so a poll often repeats the previous utc second (UTC: the
    // baseline is kept) and the next one jumps by 2 s. That pair spans two
    // poll intervals, 2.0 s +- jitter on the receipt clock; with a 2.0 s limit
    // about half of them failed as PREVIOUS and restarted the 10 s settle
    // (first persistent BETA drive, beta.3, mono 1852-1878 s). The utc step
    // limit (2 s), the utc/receipt agreement (1.0 s) and every other check
    // are unchanged.
    p.wheel_gps_speed_max_diff_kmh=4.0; p.fix_pair_max_ns=2500000000ULL;
    p.utc_step_max_s=2; p.utc_mono_tolerance_s=1.0; p.anchor_hdop_max=3.0;
    p.displacement_ratio_min=0.5; p.displacement_ratio_max=1.5;
    p.anchor_settle_ns=10000000000ULL;
    p.reverse_suspect_kmh=15.0; p.reverse_suspect_ns=2000000000ULL;
    // 3.5: the middle of the 2045..2050 stationary/driving estimates of both
    // drives (2026-10-04/05). The k=0.002 rad/s heading term covers +-3 counts.
    p.yaw_zero=2048.0;
    p.rotation_budget_per_rad=0.10;
    p.heading_budget_max_rad=20.0*3.14159265358979323846/180.0;
    p.lease_ns=500000000ULL; p.accuracy_max_m=40.0;
    p.unbounded=false;
    p.continuous_anchor=false;
    p.course_weight_kmh=10.0;
    p.course_innovation_rad=5.0*3.14159265358979323846/180.0;
    p.course_correction_rad=10.0*3.14159265358979323846/180.0;
    p.position_slack_m=20.0;
    return p;
}

// Previous (v1.0.0-beta.5) behaviour: the 40 m honest budget ends the window.
inline BetaProfile beta_profile_bounded() { return beta_profile(); }

// Production BETA profile since v1.0.0-beta.6: unbounded tunnel mode. The
// numeric sanity caps (6 h, 1000 km) are far beyond any real outage.
inline BetaProfile beta_profile_tunnel() {
    BetaProfile p=beta_profile();
    p.unbounded=true;
    p.continuous_anchor=true;
    p.error_max_m=1000000.0; p.duration_max_s=21600.0; p.distance_max_m=1000000.0;
    return p;
}

// Core configuration for the BETA core: the shared defaults with the BETA
// budget rates and limits. Stop/time/physical guards keep the core defaults.
inline mx5_dr_config beta_core_config(const BetaProfile& p) {
    mx5_dr_config c=mx5_dr_default_config();
    c.speed_error_mps=p.speed_error_mps; c.yaw_error_rad_s=p.yaw_error_rad_s;
    c.error_max_m=p.error_max_m; c.duration_max_s=p.duration_max_s;
    c.distance_max_m=p.distance_max_m; c.extended_limits=p.unbounded?1:0;
    // Coordinator decision F (2026-10-05): the strict stationary freeze (a
    // stopped estimate fails E_FRAME on |yaw| > stop_yaw_max) applies only
    // while all four wheels read zero. Any wheel movement (one count of one
    // wheel is 0.0007 m/s mean) leaves the stopped state first, so a slow
    // creep while turning in a garage is integrated instead of being a frame
    // fault (2026-10-04, t=1360 s). The MODEL/SHADOW cores keep the defaults.
    c.stop_enter_mps=0.0; c.stop_exit_mps=0.0005;
    return c;
}

} }
#endif
