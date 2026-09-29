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
    uint64_t events, intervals, resets, rejected, last_received_ns;
    bool have_speed, have_yaw, have_reverse;
};
struct Diagnostic {
    mx5_dr_snapshot snapshot;
    Status status;
    mx5_dr_result result;
};

// Single worker, fixed capacity, no allocation/I/O. Raw ingestion ONLY enters
// MODEL domain. A separate qualified Pipeline requires externally verified
// normalized events; no profile flag can promote raw events to qualification.
class Pipeline {
public:
    static const size_t CAPACITY=128;
    Pipeline();
    // Opt-in MODEL bias/scale learning changes math only at a new GPS seed.
    // gps_wheel also enables fresh-wheel and GPS travel-course anchor gates.
    bool init_model(const ModelProfile&, const mx5_dr_config&, mx5_dr_context,
                    bool auto_bias=false, bool gps_wheel=false);
    bool init_qualified(const mx5_dr_config&, mx5_dr_context);
    PipelineResult enqueue_raw(const RawEvent&);
    PipelineResult enqueue_position(const adapter::Observation&);
    // Qualified external adapter API: evidence and normalized windows retained
    // exactly. Pending anchors immediately suppress output. To replace ACTIVE
    // or NATIVE, supply a newer generation and reserve position_seq-1 for the
    // GPS_RETURN control (both sequences must exceed the prior position seq).
    // A stale-context replacement revokes old output and rejects the anchor.
    PipelineResult enqueue_anchor(const mx5_dr_anchor&, uint64_t received_ns);
    PipelineResult enqueue_speed(const mx5_dr_evidence&, double speed_mps);
    PipelineResult enqueue_yaw(const mx5_dr_evidence&, double yaw_rad_s,
                               uint16_t raw_mean, uint16_t count,
                               uint64_t window_start_ns, uint64_t window_end_ns);
    PipelineResult enqueue_reverse(const mx5_dr_evidence&, int reverse);
    // Process only events whose effective time <= watermark. Runtime normally
    // passes now-profile.reorder_ns; actual receipt clock is never rewritten.
    PipelineResult drain(uint64_t watermark_ns);
    void reset(mx5_dr_context);
    // Normal MODEL holdout completion only: clear prediction and candidates,
    // retain applied zero/scale and raw source/time guards. Faults must use reset().
    bool restart_model_prediction(mx5_dr_context);
    Diagnostic diagnostic(uint64_t now_ns) const;
    runtime::CoreBridgeResult qualified_snapshot(uint64_t now_ns,
        const runtime::CoreBridgeQualification&, adapter::DrSnapshot*) const;
    const Status& status() const { return status_; }
    const GyroBiasStatus& calibration() const { return gyro_bias_.status(); }
    const WheelScaleStatus& wheel_calibration() const { return gps_wheel_.status(); }
    GpsAnchorGate anchor_gate() const { return gps_wheel_.gate(); }
    mx5_dr_context context() const { return core_.estimate.context; }
    uint64_t reorder_ns() const { return profile_.reorder_ns; }
private:
    enum Kind { SPEED_EVENT, REVERSE_EVENT, YAW_EVENT, ANCHOR_EVENT, POSITION_EVENT };
    struct Event {
        Kind kind;
        uint64_t time, received, window_end;
        mx5_dr_evidence evidence;
        double value, wheel_spread;
        uint16_t raw, count;
        mx5_dr_anchor anchor;
        adapter::Observation observation;
    };
    mx5_dr_core core_;
    ModelProfile profile_;
    GyroBias gyro_bias_;
    GpsWheel gps_wheel_;
    Status status_;
    Event queue_[CAPACITY], speed_, yaw_, reverse_;
    size_t size_;
    uint64_t watermark_, raw_epoch_, raw_seq_[4], raw_time_[4];
    int raw_transport_[4];
    uint64_t last_yaw_time_, interval_seq_, position_seq_;
    int position_mode_;
    bool configured_, model_, have_fix_;
    adapter::Observation previous_fix_;
    PipelineResult insert(const Event&);
    PipelineResult fault(PipelineResult);
    PipelineResult advance(uint64_t);
    PipelineResult apply_position(const adapter::Observation&);
    PipelineResult control(mx5_dr_control_kind);
    bool good_fix(const adapter::Observation&) const;
};
const char* pipeline_result_name(PipelineResult);
} }
#endif
