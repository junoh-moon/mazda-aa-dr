#ifndef MX5_NAVIGATION_PIPELINE_H
#define MX5_NAVIGATION_PIPELINE_H
#include "adapter/adapter.h"
#include "navigation/gyro_bias.h"
#include "navigation/gps_wheel.h"
#include "core/dr_core.h"
#include "runtime/core_bridge.h"
#include <stddef.h>

namespace mx5 { namespace navigation {

enum SensorKind { WHEELS=1, YAW=2, REVERSE=3 };
// Values copied before OEM mutation. receive_seq is an observer sequence, not
// a producer sequence. source_mono_ms=0 means no original transport timestamp.
struct RawEvent {
    SensorKind kind;
    uint64_t epoch, receive_seq, received_ns;
    int64_t source_mono_ms;
    uint16_t raw[4];
    uint16_t count;
    int reverse;
};
enum Uncertainty {
    RECEIPT_TIME_MODEL=1, TRANSPORT_TIME_MODEL=2, YAW_WINDOW_MODEL=4,
    PHYSICAL_CALIBRATION_MODEL=8, REVERSE_ENUM_MODEL=16,
    REVERSE_LATCH_MODEL=32, GPS_TIME_HEADING_MODEL=64
};
struct ModelProfile {
    double yaw_zero, yaw_rad_per_count, wheel_kmh_per_count, wheel_zero_kmh;
    int reverse_forward_value, reverse_reverse_value;
    uint64_t reorder_ns;
    // These are explicit hypotheses for diagnostics, never qualification.
    double anchor_error_m, heading_error_rad;
};
ModelProfile research_model_profile();
enum PipelineResult {
    PIPELINE_OK=0, PIPELINE_WAITING, PIPELINE_BAD_INPUT, PIPELINE_LATE,
    PIPELINE_CLOCK_RESET, PIPELINE_SOURCE_RESET, PIPELINE_OVERFLOW,
    PIPELINE_MISSING_SENSOR, PIPELINE_CORE_REJECTED, PIPELINE_NO_ANCHOR
};
struct Status {
    PipelineResult result;
    mx5_dr_result core_result;
    uint32_t uncertainties;
    uint64_t events, intervals, resets, rejected, unpaired_positions, last_received_ns;
    bool have_speed, have_yaw, have_reverse;
};
struct Diagnostic {
    mx5_dr_snapshot snapshot;
    Status status;
    mx5_dr_result result;
};
struct FaultCalibration {
    bool valid;
    GyroBiasStatus gyro;
    WheelScaleStatus wheel;
};

// Single worker, fixed capacity, no allocation/I/O. Raw ingestion ONLY enters
// MODEL domain. A separate qualified Pipeline requires externally verified
// normalized events; no profile flag can promote raw events to qualification.
class Pipeline {
public:
    static const size_t CAPACITY=128;
    // Must make this Pipeline's current generation unselectable and return a
    // strictly newer adapter generation. If an external transition already
    // advanced it, return that generation without advancing it again. Worker
    // callback: no I/O, blocking lock, or reentry into this Pipeline.
    typedef uint64_t (*QualifiedRevoker)(void*);
    Pipeline();
    ~Pipeline();
    // A copy could alias publication ownership or overwrite its revoker.
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;
    // Opt-in MODEL bias/scale learning changes math only at a new GPS seed.
    // gps_wheel also enables fresh-wheel and GPS travel-course anchor gates.
    bool init_model(const ModelProfile&, const mx5_dr_config&, mx5_dr_context,
                    bool auto_bias=false, bool gps_wheel=false);
    bool init_qualified(const mx5_dr_config&, mx5_dr_context);
    // Required before qualified_snapshot()/qualified_publication(). Unbound
    // qualified instances cannot publish; the bound object owns revocation
    // and retires its candidate on reset/reinit/destruction.
    bool bind_qualified_revoker(QualifiedRevoker, void*);
    // End this qualified lifetime exactly once. The revoker may return an
    // already newer adapter generation; no candidate from this lifetime can
    // then be selected. Rearm keeps the original generation of a captured
    // BEGIN, even if later GAP controls are already in the input batch.
    bool retire_qualified();
    bool rearm_qualified(const mx5_dr_config&, mx5_dr_context);
    PipelineResult enqueue_raw(const RawEvent&);
    PipelineResult enqueue_position(const adapter::Observation&);
    // Qualified external adapter API: evidence and normalized windows retained
    // exactly. Pending anchors immediately suppress output. To replace ACTIVE
    // or NATIVE, supply a newer generation and reserve position_seq-1 for the
    // GPS_RETURN control (both sequences must exceed all prior anchor/control
    // sequences). A raw adapter callback count alone may lack this headroom
    // after a one-callback GAP; the qualified worker must allocate a separate
    // monotonic core sequence while retaining the raw count as provenance.
    // A stale-context replacement revokes old output and rejects the anchor.
    // A bound qualified anchor must name the exact adapter POSITION callback
    // it verifies. Receipt time and generation alone cannot distinguish two
    // same-mode GPS fixes. Zero remains usable only by nonpublishing callers.
    PipelineResult enqueue_anchor(const mx5_dr_anchor&, uint64_t received_ns,
                                  uint64_t position_call_sequence=0);
    PipelineResult enqueue_speed(const mx5_dr_evidence&, double speed_mps);
    PipelineResult enqueue_yaw(const mx5_dr_evidence&, double yaw_rad_s,
                               uint16_t raw_mean, uint16_t count,
                               uint64_t window_start_ns, uint64_t window_end_ns);
    PipelineResult enqueue_reverse(const mx5_dr_evidence&, int reverse);
    // Process only events whose effective time <= watermark. Runtime normally
    // passes now-profile.reorder_ns; actual receipt clock is never rewritten.
    PipelineResult drain(uint64_t watermark_ns);
    // Bound qualified resets revoke an adapter candidate and take the returned
    // generation; the supplied context contributes only source/session epochs.
    void reset(mx5_dr_context);
    // Normal MODEL holdout completion only: clear prediction and candidates,
    // retain applied zero/scale and raw source/time guards. Faults must use reset().
    bool restart_model_prediction(mx5_dr_context);
    Diagnostic diagnostic(uint64_t now_ns) const;
    // Holdout warmup may need to retry a reference after a yaw mean closes.
    // This is queue state only, never a freshness or validity claim.
    bool pending_position(uint64_t mono_ns) const;
    // Shared sensor-age deadline; seeded paths use the core frontier in both
    // domains, and unseeded MODEL uses the open yaw boundary. Read-only.
    bool sensor_timeout_due(uint64_t watermark_ns) const;
    // MODEL yaw callback silence only. The seeded core frontier may lag even
    // while newer yaw callbacks are safely queued behind an open window.
    bool yaw_source_timeout_due(uint64_t observed_ns) const;
    // Point-in-time result: its lease ends at now_ns even if the core's sensor
    // lease is longer. Use qualified_publication for a bounded future lease.
    runtime::CoreBridgeResult qualified_snapshot(uint64_t now_ns,
        const runtime::CoreBridgeQualification&, adapter::DrSnapshot*) const;
    // Worker-side asynchronous handoff. Original prediction timestamps stay
    // unchanged; queued GPS/native/anchor transitions cap its publication lease.
    runtime::CoreBridgeResult qualified_publication(uint64_t now_ns,
        const runtime::CoreBridgeQualification&, uint64_t requested_until_ns,
        adapter::DrSnapshot*) const;
    const Status& status() const { return status_; }
    const GyroBiasStatus& calibration() const { return gyro_bias_.status(); }
    const WheelScaleStatus& wheel_calibration() const { return gps_wheel_.status(); }
    const FaultCalibration& fault_calibration() const { return fault_calibration_; }
    GpsAnchorGate anchor_gate() const { return gps_wheel_.gate(); }
    mx5_dr_context context() const { return core_.estimate.context; }
    uint64_t reorder_ns() const { return profile_.reorder_ns; }
private:
    enum Kind { SPEED_EVENT, REVERSE_EVENT, YAW_EVENT, ANCHOR_EVENT, POSITION_EVENT };
    struct Event {
        Kind kind;
        uint64_t time, received, window_end;
        mx5_dr_evidence evidence;
        double value, wheel_spread, wheel_max;
        bool wheel_zero_conflict;
        uint16_t raw, count;
        mx5_dr_anchor anchor;
        uint64_t anchor_call_sequence;
        adapter::Observation observation;
    };
    static const size_t HISTORY_CAPACITY=64;
    struct SensorRecord { uint64_t time,received,lease; double value,spread,wheel_max; };
    struct SensorHistory { SensorRecord records[HISTORY_CAPACITY]; size_t size,next; };
    mx5_dr_core core_;
    ModelProfile profile_;
    GyroBias gyro_bias_;
    GpsWheel gps_wheel_;
    Status status_;
    FaultCalibration fault_calibration_;
    Event queue_[CAPACITY], speed_, yaw_, reverse_;
    SensorHistory wheel_history_,reverse_history_;
    size_t size_;
    uint64_t watermark_, raw_epoch_, raw_seq_[4], raw_time_[4];
    int raw_transport_[4];
    uint64_t last_yaw_time_, interval_seq_, position_seq_, wheel_conflict_since_;
    uint64_t qualified_anchor_call_sequence_, last_qualified_position_call_sequence_;
    // Retained across candidate retirement within one source/session epoch.
    uint64_t qualified_observed_position_call_sequence_;
    bool qualified_anchor_paired_;
    int position_mode_;
    bool configured_, model_, have_fix_;
    bool qualified_retired_;
    uint64_t retired_from_generation_;
    QualifiedRevoker qualified_revoker_;
    void* qualified_revoker_user_;
    const Pipeline* qualified_owner_;
    adapter::Observation previous_fix_;
    PipelineResult insert(const Event&);
    PipelineResult fault(PipelineResult);
    PipelineResult reject_core(PipelineResult);
    void reset_state(mx5_dr_context);
    bool owns_qualified_revoker() const {
        return qualified_revoker_ && qualified_owner_==this;
    }
    PipelineResult advance(uint64_t);
    PipelineResult apply_position(const adapter::Observation&);
    PipelineResult control(mx5_dr_control_kind, uint64_t observed_generation=0);
    bool valid_gps_position(const adapter::Observation&) const;
    bool good_fix(const adapter::Observation&) const;
    bool can_keep_stationary_heading(const adapter::Observation&) const;
    void clear_history();
    void remember(SensorHistory&,const Event&);
    const SensorRecord* causal(const SensorHistory&,uint64_t) const;
};
const char* pipeline_result_name(PipelineResult);
} }
#endif
