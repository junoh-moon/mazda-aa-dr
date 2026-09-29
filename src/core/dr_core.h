#ifndef MX5_DR_CORE_H
#define MX5_DR_CORE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Pure C99, single-owner worker. No live-source verification is inferred here.
 * All timestamps are one validated monotonic ns domain. Structures are NOT an
 * IPC/wire ABI. Raw-yaw guards do not establish physical units or validity. */
typedef enum { MX5_DR_UNKNOWN=0, MX5_DR_VALID=1, MX5_DR_INVALID_QUALITY=2,
               MX5_DR_MODEL=3 } mx5_dr_quality;
typedef enum { MX5_DR_UNPROVEN_POLL=0, MX5_DR_PRODUCER_TIME=1,
               MX5_DR_SEQUENCE_WITH_BOUND=2, MX5_DR_MODEL_TIME=3 } mx5_dr_freshness;
/* Model results are diagnostic hypotheses, never qualified locations. */
typedef enum { MX5_DR_QUALIFIED_DOMAIN=0, MX5_DR_MODEL_DOMAIN=1 } mx5_dr_domain;
typedef enum { MX5_DR_UNSEEDED=0, MX5_DR_READY, MX5_DR_ACTIVE,
               MX5_DR_REACQUIRING, MX5_DR_NATIVE, MX5_DR_INVALID,
               MX5_DR_LIMIT_REACHED } mx5_dr_state;
typedef enum { MX5_DR_OK=0, MX5_DR_DUPLICATE, MX5_DR_E_CONFIG,
               MX5_DR_E_NO_SEED, MX5_DR_E_CONTEXT, MX5_DR_E_QUALITY,
               MX5_DR_E_TIME, MX5_DR_E_SEQUENCE, MX5_DR_E_FRAME,
               MX5_DR_E_LIMIT, MX5_DR_E_STALE, MX5_DR_E_NUMERIC,
               MX5_DR_E_STATE } mx5_dr_result;
typedef enum { MX5_DR_GAP=0, MX5_DR_GPS_RETURN, MX5_DR_NATIVE_POSITION,
               MX5_DR_DISABLE } mx5_dr_control_kind;

typedef struct {
    uint64_t source_epoch, session_epoch, generation;
} mx5_dr_context;

typedef struct {
    uint64_t source_id, source_epoch, producer_seq;
    uint64_t measured_ns, received_ns, lease_until_ns, time_uncertainty_ns;
    mx5_dr_quality quality;
    mx5_dr_freshness freshness;
} mx5_dr_evidence;

typedef struct {
    double duration_max_s, distance_max_m, error_max_m;
    double integration_step_s, speed_error_mps, yaw_error_rad_s;
    double stop_enter_mps, stop_exit_mps, stop_hold_s, stop_yaw_max_rad_s;
    double physical_speed_max_mps, physical_yaw_max_rad_s;
    uint64_t interval_max_ns, sample_age_max_ns, snapshot_age_max_ns;
    uint64_t time_uncertainty_max_ns;
} mx5_dr_config;

typedef struct {
    mx5_dr_context context;
    uint64_t anchor_id, position_seq, measured_ns, utc_ns;
    double latitude_deg, longitude_deg, body_heading_rad;
    double position_error_m, heading_error_rad;
    /* Upstream must actually verify consecutive fixes, heading frame, physical
     * calibration, and anchor timing. Flags are explicit claims, not defaults. */
    int validated, heading_valid, calibration_verified;
    mx5_dr_quality quality;
} mx5_dr_anchor;

typedef struct {
    mx5_dr_context context;
    uint64_t interval_seq, start_ns, end_ns, received_ns;
    mx5_dr_evidence speed, yaw, reverse;
    double speed_mps;             /* magnitude, never signed */
    double yaw_rad_s;             /* body, north=0 clockwise positive */
    int reverse_active;          /* exactly 0 or 1, not UNKNOWN */
    uint16_t raw_yaw;             /* averaged encoding, 0..4093 only */
    uint16_t yaw_count;           /* nonzero even when already averaged */
    int yaw_is_mean;
    uint64_t yaw_window_start_ns, yaw_window_end_ns;
    /* This entire interval is time-aligned by upstream. A mean is treated as
     * constant over its explicit window; it is never a future instantaneous
     * sample. Subintervals may reuse an IDENTICAL producer evidence record. */
} mx5_dr_interval;

typedef struct {
    mx5_dr_context context;
    uint64_t anchor_id, processed_position_seq, frontier_ns, derived_utc_ns;
    uint64_t sensor_lease_until_ns, solution_seq;
    double latitude_deg, longitude_deg, body_heading_rad, travel_bearing_rad;
    double speed_mps, elapsed_s, distance_m, error_budget_m, heading_budget_rad;
    /* Flat-frame displacement is a numerical diagnostic, not an ENU datum. */
    double accumulated_east_m, accumulated_north_m;
    int has_bearing, stopped, valid;
    mx5_dr_domain domain;
    int model_valid; /* numerical diagnostic only; valid remains zero */
    mx5_dr_state state;
    mx5_dr_result reason;
} mx5_dr_snapshot;

typedef struct {
    mx5_dr_config config;
    mx5_dr_snapshot estimate;
    mx5_dr_anchor anchor;
    mx5_dr_interval last_interval;
    double stop_dwell_s;
    uint64_t last_control_seq, highest_position_seq;
    int configured, seeded, have_interval;
    mx5_dr_domain domain;
} mx5_dr_core;

mx5_dr_config mx5_dr_default_config(void);
mx5_dr_result mx5_dr_init(mx5_dr_core *, const mx5_dr_config *, mx5_dr_context);
/* Explicit speculative calculation. Requires MODEL evidence and unverified
 * anchor flags. Neither ordinary snapshot API nor bridge accepts its output. */
mx5_dr_result mx5_dr_init_model(mx5_dr_core *, const mx5_dr_config *, mx5_dr_context);
/* Reset clears all source bindings/history; new epoch/generation supplied by
 * runtime. Calling reset is not permission to reuse a historical anchor. */
mx5_dr_result mx5_dr_reset(mx5_dr_core *, mx5_dr_context);
mx5_dr_result mx5_dr_seed(mx5_dr_core *, const mx5_dr_anchor *);
/* Every genuine control transition requires generation > current and control
 * sequence > prior. GAP retains a READY seed; GPS_RETURN discards it. */
mx5_dr_result mx5_dr_control(mx5_dr_core *, mx5_dr_control_kind,
                           mx5_dr_context, uint64_t control_seq);
mx5_dr_result mx5_dr_step(mx5_dr_core *, const mx5_dr_interval *);
/* Copies an immutable prediction; no extrapolation or mutation. 'now' must be
 * supplied by runtime. Runtime MUST additionally gate OEM ABI/provider/mode,
 * live readiness, calibration, epoch and control sequence at send selection. */
mx5_dr_result mx5_dr_get_snapshot(const mx5_dr_core *, uint64_t now_ns,
                                mx5_dr_context expected, mx5_dr_snapshot *);
mx5_dr_result mx5_dr_get_model_snapshot(const mx5_dr_core *, uint64_t now_ns,
                                      mx5_dr_context expected, mx5_dr_snapshot *);
const char *mx5_dr_result_name(mx5_dr_result);
#ifdef __cplusplus
}
#endif
#endif
