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
    PIPELINE_MISSING_SENSOR, PIPELINE_CORE_REJECTED, PIPELINE_NO_ANCHOR,
    // A POSITION from a retired generation with no live or queued seed.
    // Replay of an applied callback, or a candidate that could later publish,
    // is BAD_INPUT instead.
    PIPELINE_STALE_INPUT
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
// Why the last GPS fix did (not) refresh the BETA position/heading.
// Legacy bounded profile (accuracy rule 3, BETA_DECISIONS_2026-10-05.md
// 3.1-3.4): UTC (same utc second: not a new pair, the baseline is kept),
// UTC_MONO (utc and receipt steps disagree), HDOP, SETTLING (fewer than 10 s
// of consecutive increasing fixes since the first fix or GPS return; legacy
// gate only, the continuous tunnel policy has no settling period),
// DISPLACEMENT (pair distance inconsistent with v*dt) and REVERSE_UNPROVEN (in
// this source epoch neither a reverse 1->0 transition nor a forward first
// REVERSE message was seen). The ENTRY_* values are not fix evaluations: the
// continuous policy records its decision at each GPS loss (mode 0) once:
// FRESH (last accepted position <= 3 s old), FALLBACK (older, but the
// dead-reckoned estimate is inside the bounded envelope) or REFUSED.
enum BetaAnchorGate {
    BETA_GATE_DISABLED=0, BETA_GATE_WAITING, BETA_GATE_ACCEPTED, BETA_GATE_BAD_FIX,
    BETA_GATE_SPEED, BETA_GATE_PREVIOUS, BETA_GATE_COURSE, BETA_GATE_YAW,
    BETA_GATE_WHEEL, BETA_GATE_REVERSE, BETA_GATE_CORE,
    BETA_GATE_UTC, BETA_GATE_UTC_MONO, BETA_GATE_HDOP, BETA_GATE_SETTLING,
    BETA_GATE_DISPLACEMENT, BETA_GATE_REVERSE_UNPROVEN,
    BETA_GATE_ENTRY_FRESH, BETA_GATE_ENTRY_FALLBACK, BETA_GATE_ENTRY_REFUSED
};
// One BETA fix evaluation (every mode 1/2 POSITION drained by the BETA core)
// or one continuous-policy entry decision at a GPS loss, kept in a small ring
// for the worker journal (beta_anchor rows).
struct BetaAnchorRecord {
    uint64_t seq, mono_ns, utc_s;
    int mode;
    BetaAnchorGate gate;
    double hdop, kmh, displacement_ratio, streak_s; // NaN when not evaluated
    double heading_deg, heading_error_rad, course_weight;
    const char* heading_source; // see beta_heading_source_code()
    bool continuous;
    // ENTRY_* rows: age of the last accepted GPS position (s, NaN if none),
    // honest budget at the loss (m, NaN if unseeded) and the reason
    // ("fresh", "fallback", "unseeded", "age", "budget", "disagree", "state").
    double entry_age_s, entry_budget_m;
    const char* entry_reason;
};
const char* beta_anchor_gate_name(BetaAnchorGate);
// One-digit journal code of BetaAnchorRecord::heading_source: 1 seed, 2 blend,
// 3 yaw, 4 reverse, 5 resync, 9 legacy, 0 none/unknown.
unsigned beta_heading_source_code(const char*);
// Why a valid MODEL reverse latch was dropped (journaled by the BETA worker).
enum LatchClear {
    LATCH_CLEAR_RESET=0,            // pipeline reset/fault (queued REVERSE possibly lost)
    LATCH_CLEAR_SOURCE_EPOCH,       // producer or model source epoch changed
    LATCH_CLEAR_REJECTED_REVERSE,   // the pipeline rejected a REVERSE message
    LATCH_CLEAR_EXCLUDED_REVERSE,   // the runtime did not hand a REVERSE message over
    LATCH_CLEAR_INPUT_GAP,          // an input gap larger than the keep limit
    LATCH_CLEAR_COUNT
};
const char* latch_clear_name(LatchClear);
// BETA speed overlay input (BETA_DECISIONS_2026-10-05.md 2): the last drained
// wheel SPEED event only. No anchor, core or GPS is involved. stopped means
// all four wheels <= 0.05 m/s and then speed_mps is 0. MODEL evidence.
struct SpeedPublication {
    bool ok;
    bool stopped;
    double speed_mps;            // unscaled mean wheel speed (BETA uses no learned scale)
    uint64_t measured_ns, received_ns;
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
    // reverse_latch (design decision 7, MODEL only): a change-only REVERSE
    // message stays valid (lease UINT64_MAX, REVERSE_LATCH_MODEL) until a
    // source epoch change, a rejected REVERSE message, or a reset that drops
    // a queued REVERSE message. Wheel speed 0 does not release it. Before the
    // first REVERSE message the state is unknown and nothing seeds.
    bool init_model(const ModelProfile&, const mx5_dr_config&, mx5_dr_context,
                    bool auto_bias=false, bool gps_wheel=false, bool reverse_latch=false);
    // Adds the separate BETA core after init_model(..., reverse_latch=true).
    // It sees the same sensor intervals as the MODEL core but uses the BETA
    // configuration, the fixed profile yaw zero, unscaled wheel speed and only
    // GPS fixes that pass the BETA anchor gate. It never changes the MODEL
    // core, diagnostic() or any qualified state. Cleared by init_*().
    bool enable_beta(const runtime::BetaProfile&);
    // Cadence fence (runtime BetaController::cadence_fence): forget the BETA
    // anchor, pair baseline, settle streak and yaw window and re-initialize
    // the BETA core unseeded, so the next anchor needs the full gate again.
    // The MODEL core, the reverse latch and the gate record sequence stay.
    void fence_beta();
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
    // BETA core result for the BETA bridge: queried at its frontier (or the
    // earliest admissible later time), suppressed by any due GPS/anchor event,
    // and lease-capped before any queued one. Never extrapolates coordinates.
    runtime::BetaModelInput model_publication(uint64_t now_ns) const;
    // Speed overlay input at now_ns: ok only with a BETA-enabled MODEL
    // pipeline, a drained SPEED event no older than the BETA lease, finite and
    // non-negative, without the one-stopped-wheel contradiction.
    SpeedPublication speed_publication(uint64_t now_ns) const;
    bool beta_enabled() const { return beta_enabled_; }
    BetaAnchorGate beta_gate() const { return beta_gate_; }
    mx5_dr_result beta_core_result() const { return beta_core_result_; }
    // The last BETA core step/seed failure since the last accepted anchor
    // (MX5_DR_OK when none): the real reason behind an unseeded BETA core.
    mx5_dr_result beta_core_failure() const { return beta_core_failure_; }
    double beta_rotation_rad() const { return beta_rotation_rad_; }
    double beta_rotation_budget_m() const { return beta_rotation_budget_m_; }
    // 3.4: the latched reverse contradicted the wheels (> 15 km/h for > 2 s)
    // and the BETA core was disabled; cleared by the next accepted anchor.
    bool beta_reverse_suspect() const { return beta_reverse_suspect_; }
    bool reverse_exit_seen() const { return reverse_exit_seen_; }
    // Newest anchor record sequence (0: none) and a record by sequence; false
    // when it was overwritten (ring of BETA_RECORD_CAPACITY).
    uint64_t beta_anchor_sequence() const { return beta_record_seq_; }
    bool beta_anchor_record(uint64_t seq,BetaAnchorRecord* out) const;
    // A REVERSE message the runtime did not hand to this pipeline (excluded
    // or not computed): its change is lost, so the latch is cleared.
    void exclude_reverse(LatchClear why=LATCH_CLEAR_EXCLUDED_REVERSE);
    // A reset that must not drop the reverse latch (task E, 2026-10-05): the
    // runtime's input-rejection epoch bump for a stale/sequence gap of at most
    // 2 s and 16 missing events. The latch is still dropped if a queued
    // REVERSE message is discarded by this reset. Everything else as reset().
    void reset_keep_reverse(mx5_dr_context);
    uint64_t latch_clears(LatchClear why) const { return why<LATCH_CLEAR_COUNT?latch_clears_[why]:0; }
    uint64_t latch_clears_total() const;
    LatchClear last_latch_clear() const { return last_latch_clear_; }
    int latch_value() const { return latch_value_; }
    bool reverse_latched() const { return reverse_latch_&&latch_valid_; }
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
        // Exact window mean (sum/count). raw is the truncated integer kept
        // only for the core's raw-encoding guard; rates use mean_counts.
        double mean_counts;
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
    // Negative boundaries from discarded queued or ignored old-generation
    // POSITIONs. Neither observation time nor callback order qualifies an anchor.
    uint64_t qualified_stale_position_cutoff_ns_;
    uint64_t qualified_stale_position_call_sequence_;
    bool qualified_anchor_paired_;
    int position_mode_;
    bool configured_, model_, have_fix_;
    bool qualified_retired_;
    uint64_t retired_from_generation_;
    QualifiedRevoker qualified_revoker_;
    void* qualified_revoker_user_;
    const Pipeline* qualified_owner_;
    adapter::Observation previous_fix_;
    // Reverse latch (MODEL only).
    bool reverse_latch_, latch_valid_;
    int latch_value_;
    uint64_t latch_time_, latch_received_, latch_epoch_, latch_seq_;
    // BETA core and its anchor gate state.
    struct YawRecord { uint64_t begin,end; double rate; };
    bool beta_enabled_, beta_have_prev_;
    runtime::BetaProfile beta_;
    mx5_dr_core beta_core_;
    BetaAnchorGate beta_gate_;
    mx5_dr_result beta_core_result_, beta_core_failure_;
    int beta_mode_;
    uint64_t beta_position_seq_, beta_conflict_since_;
    double beta_rotation_rad_, beta_rotation_budget_m_;
    adapter::Observation beta_prev_;
    // Continuous tracking: the GPS course reference fix (chord baseline) and
    // the current run of mutually consistent, chord-confirmed course
    // disagreements with the carried heading (resync evidence).
    adapter::Observation beta_heading_ref_;
    bool beta_have_heading_ref_;
    unsigned beta_resync_count_;
    double beta_resync_innovation_;
    uint64_t beta_resync_ns_;
    // Continuous tracking entry evidence: receipt time of the last GPS fix that
    // refreshed the position (0: none since reset), and whether every rejected
    // data-valid fix since then agreed with the dead-reckoned estimate.
    uint64_t beta_fix_ns_;
    bool beta_fix_agrees_;
    // 3.1-3.2: start of the current run of consecutive increasing fixes.
    bool beta_streak_;
    uint64_t beta_streak_mono_, beta_streak_utc_;
    // 3.4 reverse latch safety.
    bool reverse_exit_seen_, beta_reverse_suspect_, reverse_any_seen_;
    bool keep_latch_on_reset_;
    uint64_t latch_clears_[LATCH_CLEAR_COUNT];
    LatchClear last_latch_clear_;
    uint64_t beta_reverse_fast_since_;
    static const size_t BETA_RECORD_CAPACITY=32;
    BetaAnchorRecord beta_records_[BETA_RECORD_CAPACITY];
    uint64_t beta_record_seq_;
    YawRecord beta_yaw_[HISTORY_CAPACITY];
    size_t beta_yaw_size_, beta_yaw_next_;
    PipelineResult enqueue_raw_event(const RawEvent&);
    void clear_latch();
    void drop_latch(LatchClear);
    bool reverse_known() const;
    const SensorRecord* reverse_at(uint64_t,SensorRecord*) const;
    bool latch_evidence(uint64_t start,mx5_dr_evidence*);
    void reset_beta(mx5_dr_context);
    mx5_dr_result beta_control(mx5_dr_control_kind);
    void beta_position(const adapter::Observation&);
    BetaAnchorGate evaluate_beta_gate(const adapter::Observation&,double* ratio) const;
    // Continuous tracking: what one data-valid fix says about the heading.
    struct CourseCheck {
        bool chord;      // a >= BETA_HEADING_BASELINE_M GPS displacement since the reference fix
        bool agrees;     // ... and the GPS course points along it
        bool candidate;  // chord-confirmed course far from the carried heading
        bool consistent; // ... and it continues the previous candidate run
        bool resync;     // ... and the run is long enough: the heading is reseeded
        bool reversing;  // a position-only refresh while reverse is latched
        double innovation;
    };
    BetaAnchorGate evaluate_beta_tracking(const adapter::Observation&,const mx5_dr_snapshot*,
                                         double* heading,double* error,double* ratio,double* weight,
                                         CourseCheck*) const;
    bool beta_pair_continues(const adapter::Observation&) const;
    bool beta_yaw_turn(uint64_t begin,uint64_t end,double* turn) const;
    bool beta_entry(const adapter::Observation&);
    bool beta_carry_across_return(const adapter::Observation&,const mx5_dr_snapshot&,double stop_dwell);
    void beta_record(const adapter::Observation&,BetaAnchorGate,double ratio,
                     double weight=0,const char* source="none");
    void beta_step(const mx5_dr_interval&,double rate);
    PipelineResult insert(const Event&);
    PipelineResult fault(PipelineResult);
    PipelineResult reject_core(PipelineResult);
    void preserve_discarded_positions();
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
