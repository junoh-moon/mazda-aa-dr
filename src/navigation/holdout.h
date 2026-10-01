#ifndef MX5_NAVIGATION_HOLDOUT_H
#define MX5_NAVIGATION_HOLDOUT_H
#include "pipeline.h"

namespace mx5 { namespace navigation {
enum HoldoutPhase { HOLDOUT_WARMUP, HOLDOUT_RUNNING, HOLDOUT_COOLDOWN };
enum HoldoutEvent { HOLDOUT_BEGIN, HOLDOUT_COMPARED, HOLDOUT_END, HOLDOUT_ABORT, HOLDOUT_SKIPPED };
enum HoldoutReason {
    HOLDOUT_NONE, HOLDOUT_COMPLETE, HOLDOUT_BAD_GPS, HOLDOUT_GPS_TIMEOUT,
    HOLDOUT_REAL_GAP, HOLDOUT_NATIVE, HOLDOUT_SOURCE_FAULT, HOLDOUT_AUDIT_RESET,
    HOLDOUT_REFERENCE_OVERFLOW, HOLDOUT_OUTPUT_OVERFLOW, HOLDOUT_TIME_ORDER,
    HOLDOUT_PREDICTION_INVALID, HOLDOUT_CAPTURE_STOP, HOLDOUT_SESSION_RESET, HOLDOUT_BUS_RESET,
    HOLDOUT_STALE_REFERENCE
};
struct HoldoutConfig {
    uint64_t duration_ns, cooldown_ns, gps_timeout_ns;
};
HoldoutConfig default_holdout_config();
struct HoldoutResult {
    HoldoutEvent event;
    HoldoutReason reason;
    uint64_t window_id, anchor_ns, reference_ns, prediction_frontier_ns;
    adapter::PositionInput reference;
    mx5_dr_snapshot prediction;
    double position_error_m, heading_error_rad;
    bool has_heading_error;
    double applied_yaw_zero;
    uint64_t calibration_version;
    double applied_wheel_scale;
    uint64_t wheel_scale_version;
};
// Diagnostic-only worker: independent MODEL pipeline, fixed queues, no I/O or
// allocation. GNSS references are receipt-time aligned, not ground truth.
// During RUNNING no reference coordinate, speed, or heading enters prediction.
// Runtime must pop results each worker turn, and reset on every audit fault.
class GpsHoldout {
public:
    static const size_t REFERENCE_CAPACITY=32, RESULT_CAPACITY=64;
    GpsHoldout();
    bool init_model(const ModelProfile&, const mx5_dr_config&, mx5_dr_context,
                    const HoldoutConfig& = default_holdout_config());
    PipelineResult enqueue_raw(const RawEvent&);
    PipelineResult enqueue_position(const adapter::Observation&);
    void drain(uint64_t watermark_ns);
    void reset(mx5_dr_context, HoldoutReason = HOLDOUT_AUDIT_RESET);
    bool pop(HoldoutResult*);
    HoldoutPhase phase() const { return phase_; }
private:
    Pipeline pipeline_;
    HoldoutConfig config_;
    HoldoutPhase phase_;
    adapter::Observation references_[REFERENCE_CAPACITY], previous_;
    HoldoutResult results_[RESULT_CAPACITY];
    size_t reference_count_, result_head_, result_count_;
    uint64_t window_id_, anchor_ns_, end_ns_, cooldown_until_, last_gps_ns_;
    uint64_t latest_received_ns_, watermark_, utc_progress_ns_, sample_age_ns_;
    bool configured_, have_previous_, reference_submitted_;
    bool eligible(const adapter::Observation&, bool moving) const;
    bool consistent(const adapter::Observation&) const;
    void abort(HoldoutReason, uint64_t);
    void restart(uint64_t, bool complete = false);
    void emit(HoldoutEvent, HoldoutReason, const adapter::Observation*,
              const mx5_dr_snapshot*);
    void remove_reference();
    bool source_ok(PipelineResult) const;
};
const char* holdout_event_name(HoldoutEvent);
const char* holdout_reason_name(HoldoutReason);
} }
#endif
