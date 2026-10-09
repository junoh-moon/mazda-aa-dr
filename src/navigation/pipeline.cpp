#include "pipeline.h"
#include <cmath>
#include <cstring>
#include <limits>

namespace mx5 { namespace navigation {
namespace {
const double PI=3.14159265358979323846;
uint64_t max64(uint64_t a,uint64_t b) { return a>b?a:b; }
uint64_t min64(uint64_t a,uint64_t b) { return a<b?a:b; }
uint64_t add(uint64_t a,uint64_t b) {
    return UINT64_MAX-a<b?UINT64_MAX:a+b;
}
bool finite(double x) { return std::isfinite(x); }
}
ModelProfile research_model_profile() {
    ModelProfile p={2047.0,-0.000658615,0.01,-100.0,0,1,
                    100000000ULL,10.0,0.15};
    return p;
}
Pipeline::Pipeline() : size_(0), watermark_(0), raw_epoch_(0),
    last_yaw_time_(0), interval_seq_(0), position_seq_(0), wheel_conflict_since_(0),
    qualified_anchor_call_sequence_(0), last_qualified_position_call_sequence_(0),
    qualified_observed_position_call_sequence_(0), qualified_stale_position_cutoff_ns_(0),
    qualified_stale_position_call_sequence_(0),
    qualified_anchor_paired_(false), position_mode_(-1),
    configured_(false), model_(false), have_fix_(false),
    qualified_retired_(false), retired_from_generation_(0),
    qualified_revoker_(0), qualified_revoker_user_(0), qualified_owner_(0),
    reverse_latch_(false), latch_valid_(false), latch_value_(0), latch_time_(0),
    latch_received_(0), latch_epoch_(0), latch_seq_(0),
    beta_enabled_(false), beta_have_prev_(false), beta_gate_(BETA_GATE_DISABLED),
    beta_core_result_(MX5_DR_E_CONFIG), beta_core_failure_(MX5_DR_OK), beta_mode_(-1), beta_position_seq_(0),
    beta_conflict_since_(0), beta_rotation_rad_(0), beta_rotation_budget_m_(0),
    beta_streak_(false), beta_streak_mono_(0), beta_streak_utc_(0),
    reverse_exit_seen_(false), beta_reverse_suspect_(false), reverse_any_seen_(false),
    keep_latch_on_reset_(false), last_latch_clear_(LATCH_CLEAR_RESET), beta_reverse_fast_since_(0),
    beta_record_seq_(0), beta_yaw_size_(0), beta_yaw_next_(0) {
    std::memset(beta_records_,0,sizeof beta_records_);
    std::memset(latch_clears_,0,sizeof latch_clears_);
    std::memset(&core_,0,sizeof core_); std::memset(&status_,0,sizeof status_);
    std::memset(&beta_core_,0,sizeof beta_core_);
    beta_=runtime::beta_profile_tunnel(); beta_prev_=adapter::Observation();
    fault_calibration_=FaultCalibration();
    std::memset(raw_seq_,0,sizeof raw_seq_); std::memset(raw_time_,0,sizeof raw_time_);
    for (unsigned i=0;i<4;++i) raw_transport_[i]=-1;
    clear_history();
    profile_=research_model_profile();
}
Pipeline::~Pipeline() {
    // A worker may be replaced while its last bounded publication remains
    // selectable. Retire that candidate before releasing the owner.
    if(configured_&&!model_&&owns_qualified_revoker()&&!qualified_retired_)
        qualified_revoker_(qualified_revoker_user_);
}
bool Pipeline::init_model(const ModelProfile& p,const mx5_dr_config& c,mx5_dr_context x,bool auto_bias,bool gps_wheel,
                          bool reverse_latch) {
    const bool valid_profile=finite(p.yaw_zero)&&finite(p.yaw_rad_per_count)&&p.yaw_rad_per_count!=0 &&
        finite(p.wheel_kmh_per_count)&&p.wheel_kmh_per_count>0 &&
        finite(p.wheel_zero_kmh)&&p.reorder_ns<=c.sample_age_max_ns &&
        finite(p.anchor_error_m)&&p.anchor_error_m>=0 &&
        finite(p.heading_error_rad)&&p.heading_error_rad>=0;
    if(configured_&&!model_&&owns_qualified_revoker()) {
        const uint64_t previous=context().generation;
        const uint64_t next=qualified_revoker_(qualified_revoker_user_);
        if(!next||next>UINT32_MAX||next<=previous) {
            mx5_dr_context terminal=context();terminal.generation=UINT64_MAX;
            reset_state(terminal);configured_=false;
            status_.result=PIPELINE_BAD_INPUT;
            qualified_revoker_=0;qualified_revoker_user_=0;qualified_owner_=0;
            return false;
        }
        x.generation=next;
        if(!valid_profile) {
            reset_state(x);configured_=false;
            status_.result=PIPELINE_BAD_INPUT;
            qualified_revoker_=0;qualified_revoker_user_=0;qualified_owner_=0;
            return false;
        }
    }
    if(!valid_profile)return false;
    model_=true; profile_=p; qualified_retired_=false;retired_from_generation_=0;
    reverse_latch_=reverse_latch; clear_latch(); reverse_exit_seen_=reverse_any_seen_=false;
    beta_enabled_=false; beta_gate_=BETA_GATE_DISABLED;
    qualified_stale_position_cutoff_ns_=0;
    qualified_stale_position_call_sequence_=0;
    qualified_revoker_=0; qualified_revoker_user_=0;
    qualified_owner_=0;
    gyro_bias_.configure(auto_bias,p.yaw_zero,c.sample_age_max_ns);
    gps_wheel_.configure(gps_wheel,c.sample_age_max_ns);
    configured_=mx5_dr_init_model(&core_,&c,x)==MX5_DR_OK;
    if (configured_) reset_state(x);
    else status_.result=PIPELINE_BAD_INPUT;
    return configured_;
}
bool Pipeline::init_qualified(const mx5_dr_config& c,mx5_dr_context x) {
    const bool same_source_session=configured_&&!model_&&
        x.source_epoch==context().source_epoch&&x.session_epoch==context().session_epoch;
    // This entry replaces the revoker before reset_state can see its owner.
    if(same_source_session)preserve_discarded_positions();
    if(configured_&&!model_&&owns_qualified_revoker()) {
        const uint64_t previous=context().generation;
        const uint64_t next=qualified_revoker_(qualified_revoker_user_);
        if(!next||next>UINT32_MAX||next<=previous) {
            mx5_dr_context terminal=context();terminal.generation=UINT64_MAX;
            reset_state(terminal);configured_=false;
            status_.result=PIPELINE_BAD_INPUT;
            qualified_revoker_=0;qualified_revoker_user_=0;qualified_owner_=0;
            return false;
        }
        x.generation=next;
    }
    model_=false; qualified_retired_=false;retired_from_generation_=0;
    qualified_revoker_=0; qualified_revoker_user_=0;qualified_owner_=0;
    reverse_latch_=false; clear_latch(); beta_enabled_=false; beta_gate_=BETA_GATE_DISABLED;
    // MODEL assumptions cannot remain attached to a new qualified domain.
    status_.uncertainties=0;
    gyro_bias_.configure(false,profile_.yaw_zero,c.sample_age_max_ns);
    gps_wheel_.configure(false,c.sample_age_max_ns);
    configured_=mx5_dr_init(&core_,&c,x)==MX5_DR_OK;
    if (configured_) reset_state(x);
    else status_.result=PIPELINE_BAD_INPUT;
    if(!same_source_session) {
        qualified_observed_position_call_sequence_=0;
        qualified_stale_position_cutoff_ns_=0;
        qualified_stale_position_call_sequence_=0;
    }
    return configured_;
}
bool Pipeline::bind_qualified_revoker(QualifiedRevoker revoke,void* user) {
    if(!configured_||model_||qualified_revoker_||!revoke||
       !context().generation||context().generation>UINT32_MAX)return false;
    qualified_revoker_=revoke;qualified_revoker_user_=user;qualified_owner_=this;return true;
}
bool Pipeline::retire_qualified() {
    if(!configured_||model_||!owns_qualified_revoker())return false;
    if(qualified_retired_)return true;
    const uint64_t previous=context().generation;
    const uint64_t next=qualified_revoker_(qualified_revoker_user_);
    if(!next||next>UINT32_MAX||next<=previous) {
        mx5_dr_context terminal=context();terminal.generation=UINT64_MAX;
        reset_state(terminal);configured_=false;status_.result=PIPELINE_BAD_INPUT;
        return false;
    }
    mx5_dr_context x=context();x.generation=next;
    reset_state(x);qualified_retired_=true;retired_from_generation_=previous;
    return true;
}
bool Pipeline::rearm_qualified(const mx5_dr_config& c,mx5_dr_context x) {
    if(!configured_||model_||!owns_qualified_revoker()||!qualified_retired_||
       !x.source_epoch||!x.session_epoch||x.generation<=retired_from_generation_||
       x.generation>UINT32_MAX)return false;
    // The old lifetime was invalidated before this call. The incoming BEGIN
    // may precede a later captured GAP, so retain its original generation.
    const bool same_source_session=x.source_epoch==context().source_epoch&&
        x.session_epoch==context().session_epoch;
    if(mx5_dr_init(&core_,&c,x)!=MX5_DR_OK) {
        configured_=false;status_.result=PIPELINE_BAD_INPUT;return false;
    }
    reset_state(x);qualified_retired_=false;retired_from_generation_=0;
    if(!same_source_session) {
        qualified_observed_position_call_sequence_=0;
        qualified_stale_position_cutoff_ns_=0;
        qualified_stale_position_call_sequence_=0;
    }
    return true;
}
void Pipeline::preserve_discarded_positions() {
    if(!configured_||model_||!owns_qualified_revoker())return;
    // Retirement can discard callbacks queued beyond the drain watermark.
    // Keep the same negative evidence as later old-generation delivery, but
    // never mark these unprocessed callbacks as applied or infer sample time.
    for(size_t j=0;j<size_;++j)if(queue_[j].kind==POSITION_EVENT) {
        const adapter::Observation& o=queue_[j].observation;
        qualified_stale_position_cutoff_ns_=max64(qualified_stale_position_cutoff_ns_,o.mono_ns);
        qualified_stale_position_call_sequence_=max64(qualified_stale_position_call_sequence_,
                                                      o.call_sequence);
    }
}
void Pipeline::reset_state(mx5_dr_context x) {
    if (!configured_) return;
    if(!model_&&owns_qualified_revoker()) {
        if(x.source_epoch==context().source_epoch&&x.session_epoch==context().session_epoch)
            preserve_discarded_positions();
        else {
            qualified_observed_position_call_sequence_=0;
            qualified_stale_position_cutoff_ns_=0;
            qualified_stale_position_call_sequence_=0;
        }
    }
    // BETA_DECISIONS 3.4: a reset ends the latch (it may discard a queued or
    // in-flight REVERSE change) and a source epoch change also forgets the
    // producer's history. Exception (task E): the runtime's small-gap input
    // rejection keeps the latch unless a queued REVERSE message is discarded.
    if(reverse_latch_) {
        bool queued_reverse=false;
        for(size_t j=0;j<size_;++j)if(queue_[j].kind==REVERSE_EVENT)queued_reverse=true;
        if(keep_latch_on_reset_) {
            if(queued_reverse)drop_latch(LATCH_CLEAR_RESET);
        } else {
            const bool epoch=x.source_epoch!=context().source_epoch;
            if(epoch)reverse_exit_seen_=reverse_any_seen_=false;
            drop_latch(epoch?LATCH_CLEAR_SOURCE_EPOCH:LATCH_CLEAR_RESET);
        }
    }
    mx5_dr_reset(&core_,x); gyro_bias_.reset(); gps_wheel_.reset(); size_=0; watermark_=0; raw_epoch_=0;
    reset_beta(x);
    fault_calibration_.valid=false;
    std::memset(raw_seq_,0,sizeof raw_seq_); std::memset(raw_time_,0,sizeof raw_time_);
    for (unsigned i=0;i<4;++i) raw_transport_[i]=-1;
    last_yaw_time_=0; interval_seq_=0; position_seq_=0; position_mode_=-1;
    qualified_anchor_call_sequence_=last_qualified_position_call_sequence_=0;
    qualified_anchor_paired_=false;
    wheel_conflict_since_=0;clear_history();
    have_fix_=false; status_.have_speed=status_.have_yaw=status_.have_reverse=false;
    status_.result=PIPELINE_WAITING; status_.core_result=MX5_DR_E_NO_SEED;
}
void Pipeline::reset(mx5_dr_context x) {
    if(!configured_)return;
    if(!model_&&owns_qualified_revoker()&&qualified_retired_) {
        x.generation=context().generation;reset_state(x);return;
    }
    if(!model_&&owns_qualified_revoker()) {
        const uint64_t previous=context().generation;
        const uint64_t next=qualified_revoker_(qualified_revoker_user_);
        if(!next||next>UINT32_MAX||next<=previous) {
            x.generation=UINT64_MAX;reset_state(x);configured_=false;
            status_.result=PIPELINE_BAD_INPUT;return;
        }
        x.generation=next;
        qualified_retired_=true;retired_from_generation_=previous;
    }
    reset_state(x);
}
bool Pipeline::restart_model_prediction(mx5_dr_context x) {
    if (!configured_ || !model_) return false;
    GyroBias applied=gyro_bias_; applied.restart_prediction();
    GpsWheel wheel_applied=gps_wheel_; wheel_applied.restart_prediction();
    const uint64_t epoch=raw_epoch_;
    uint64_t seq[4],time[4]; int transport[4];
    std::memcpy(seq,raw_seq_,sizeof seq); std::memcpy(time,raw_time_,sizeof time);
    std::memcpy(transport,raw_transport_,sizeof transport);
    reset(x);
    gyro_bias_=applied; gps_wheel_=wheel_applied; raw_epoch_=epoch;
    std::memcpy(raw_seq_,seq,sizeof seq); std::memcpy(raw_time_,time,sizeof time);
    std::memcpy(raw_transport_,transport,sizeof transport);
    return true;
}
PipelineResult Pipeline::fault(PipelineResult r) {
    FaultCalibration before=FaultCalibration();
    before.valid=configured_;
    if(before.valid) { before.gyro=gyro_bias_.status();before.wheel=gps_wheel_.status(); }
    mx5_dr_context x=context();
    bool exhausted=x.generation==UINT64_MAX;
    if(configured_&&!model_&&owns_qualified_revoker()&&!qualified_retired_) {
        const uint64_t next=qualified_revoker_(qualified_revoker_user_);
        if(!next||next>UINT32_MAX||next<=x.generation) exhausted=true;
        else { retired_from_generation_=x.generation;x.generation=next;qualified_retired_=true; }
    } else if (!exhausted && (model_||!owns_qualified_revoker())) ++x.generation;
    if(exhausted)x.generation=UINT64_MAX;
    if (configured_) reset_state(x);
    fault_calibration_=before;
    // Clearing estimate flags alone is insufficient: a later snapshot query
    // recomputes validity from the still-seeded core. Reset before disabling.
    if (exhausted) configured_=false;
    ++status_.resets; ++status_.rejected; status_.result=r;
    return r;
}
PipelineResult Pipeline::reject_core(PipelineResult r) {
    if(!model_&&owns_qualified_revoker()) {
        const mx5_dr_result cause=status_.core_result;
        fault(r);
        status_.core_result=cause;
    } else status_.result=r;
    return r;
}
PipelineResult Pipeline::insert(const Event& e) {
    if (!configured_ || !e.time || e.received<e.time) return fault(PIPELINE_BAD_INPUT);
    if (e.time<watermark_) return fault(PIPELINE_LATE);
    if (size_==CAPACITY) return fault(PIPELINE_OVERFLOW);
    size_t at=size_;
    while (at && (queue_[at-1].time>e.time ||
           (queue_[at-1].time==e.time && queue_[at-1].kind>e.kind))) {
        queue_[at]=queue_[at-1]; --at;
    }
    queue_[at]=e; ++size_; ++status_.events;
    status_.last_received_ns=max64(status_.last_received_ns,e.received);
    status_.result=PIPELINE_OK; return PIPELINE_OK;
}
PipelineResult Pipeline::enqueue_raw(const RawEvent& r) {
    // Any source epoch change ends the latch; so does a REVERSE message the
    // pipeline could not accept (its change would otherwise be lost).
    if(reverse_latch_&&latch_valid_&&r.epoch!=latch_epoch_) {
        drop_latch(LATCH_CLEAR_SOURCE_EPOCH);reverse_exit_seen_=reverse_any_seen_=false;
    }
    const PipelineResult result=enqueue_raw_event(r);
    if(reverse_latch_&&r.kind==REVERSE&&result!=PIPELINE_OK)drop_latch(LATCH_CLEAR_REJECTED_REVERSE);
    return result;
}
PipelineResult Pipeline::enqueue_raw_event(const RawEvent& r) {
    if (!configured_||!model_||r.kind<WHEELS||r.kind>REVERSE||!r.epoch||
        !r.receive_seq||!r.received_ns) return fault(PIPELINE_BAD_INPUT);
    if (raw_epoch_ && raw_epoch_!=r.epoch) return fault(PIPELINE_SOURCE_RESET);
    raw_epoch_=r.epoch;
    if (r.receive_seq<=raw_seq_[r.kind]) return fault(PIPELINE_BAD_INPUT);
    uint64_t time=r.received_ns;
    if (r.source_mono_ms<0 ||
        uint64_t(r.source_mono_ms)>UINT64_MAX/1000000ULL) return fault(PIPELINE_CLOCK_RESET);
    if (r.source_mono_ms) {
        time=uint64_t(r.source_mono_ms)*1000000ULL;
        status_.uncertainties|=TRANSPORT_TIME_MODEL;
    } else status_.uncertainties|=RECEIPT_TIME_MODEL;
    if (!time||time>r.received_ns||time<raw_time_[r.kind]) return fault(PIPELINE_CLOCK_RESET);
    // A change between receipt and transport clocks invalidates the learned
    // calibration and anchor together; never switch model units inside an outage.
    if ((gyro_bias_.status().enabled || gps_wheel_.status().enabled) && raw_transport_[r.kind]!=-1 &&
        raw_transport_[r.kind]!=int(r.source_mono_ms!=0)) return fault(PIPELINE_CLOCK_RESET);
    raw_transport_[r.kind]=int(r.source_mono_ms!=0);
    raw_seq_[r.kind]=r.receive_seq; raw_time_[r.kind]=time;
    Event e=Event(); e.time=time; e.received=r.received_ns;
    e.evidence.source_id=uint64_t(r.kind); e.evidence.source_epoch=r.epoch;
    e.evidence.producer_seq=r.receive_seq; // MODEL sequence, never physical claim.
    e.evidence.measured_ns=time; e.evidence.received_ns=r.received_ns;
    e.evidence.lease_until_ns=add(time,core_.config.sample_age_max_ns);
    e.evidence.quality=MX5_DR_MODEL; e.evidence.freshness=MX5_DR_MODEL_TIME;
    status_.uncertainties|=PHYSICAL_CALIBRATION_MODEL;
    if (r.kind==WHEELS) {
        e.kind=SPEED_EVENT; double kmh=0, wheels[4];
        for (unsigned i=0;i<4;++i) {
            if (r.raw[i]>40000) return fault(PIPELINE_BAD_INPUT);
            double wheel=r.raw[i]*profile_.wheel_kmh_per_count+profile_.wheel_zero_kmh;
            if (wheel<0) return fault(PIPELINE_BAD_INPUT);
            kmh+=wheel*0.25; wheels[i]=wheel/3.6;
        }
        e.value=kmh/3.6;
        double low=wheels[0],high=wheels[0];
        for (unsigned j=1;j<4;++j) {
            if (wheels[j]<low) low=wheels[j];
            if (wheels[j]>high) high=wheels[j];
        }
        e.wheel_spread=high-low;e.wheel_max=high;
        unsigned stopped=0;
        double moving_low=std::numeric_limits<double>::max(),moving_high=0,moving_sum=0;
        for(unsigned j=0;j<4;++j) {
            if(wheels[j]<=0.05)++stopped;
            else {
                moving_sum+=wheels[j];
                if(wheels[j]<moving_low)moving_low=wheels[j];
                if(wheels[j]>moving_high)moving_high=wheels[j];
            }
        }
        const double moving_tolerance=moving_sum/3*0.05;
        e.wheel_zero_conflict=stopped==1&&moving_low>=2.0&&
            moving_high-moving_low<=(moving_tolerance>0.3?moving_tolerance:0.3);
        gyro_bias_.wheels(time,r.received_ns,r.source_mono_ms!=0,wheels);
    } else if (r.kind==REVERSE) {
        e.kind=REVERSE_EVENT;
        if (profile_.reverse_forward_value<0 || profile_.reverse_reverse_value<0 ||
            profile_.reverse_forward_value==profile_.reverse_reverse_value)
            return fault(PIPELINE_BAD_INPUT);
        if (r.reverse==profile_.reverse_forward_value) e.value=0;
        else if (r.reverse==profile_.reverse_reverse_value) e.value=1;
        else return fault(PIPELINE_BAD_INPUT);
        status_.uncertainties|=REVERSE_ENUM_MODEL|REVERSE_LATCH_MODEL;
        // Change-only producer: the state holds until the next message.
        if(reverse_latch_)e.evidence.lease_until_ns=UINT64_MAX;
    } else {
        // VIP adds 12-bit samples into a wrapping u16 sum and a u8 count.
        // If adding one modulus still fits that many samples, this payload
        // cannot distinguish a small mean from a wrapped large sum.
        if (!r.count || r.count>255 ||
            uint32_t(r.raw[0])+65536U<=uint32_t(r.count)*4095U)
            return fault(PIPELINE_BAD_INPUT);
        // Passing this bound does not rule out count wrap, skipped inputs,
        // stale samples or invalid constituents; this remains MODEL-only.
        unsigned mean=unsigned(r.raw[0])/r.count;
        if (mean>=4094) return fault(PIPELINE_BAD_INPUT);
        if (!last_yaw_time_) { last_yaw_time_=time; return PIPELINE_WAITING; }
        if (time==last_yaw_time_) return PIPELINE_WAITING;
        if (time-last_yaw_time_>core_.config.sample_age_max_ns) return fault(PIPELINE_MISSING_SENSOR);
        gyro_bias_.yaw(last_yaw_time_,time,r.received_ns,r.source_mono_ms!=0,double(mean));
        e.kind=YAW_EVENT; e.time=last_yaw_time_; e.window_end=time;
        e.value=(double(mean)-profile_.yaw_zero)*profile_.yaw_rad_per_count;
        e.raw=uint16_t(mean); e.count=r.count; last_yaw_time_=time;
        status_.uncertainties|=YAW_WINDOW_MODEL;
    }
    return insert(e);
}
PipelineResult Pipeline::enqueue_position(const adapter::Observation& o) {
    if (o.kind!=adapter::Observation::POSITION) return PIPELINE_BAD_INPUT;
    // The adapter can have captured this callback before an independent
    // worker fault invalidated its generation. Ignore it only if the old
    // calculator has already been retired and no replacement seed is queued.
    if(!model_&&owns_qualified_revoker()&&o.prediction_generation&&
       o.prediction_generation<context().generation) {
        // A callback already consumed by this calculator is a source replay,
        // not merely delayed work from the retired generation. Discarded
        // callbacks do not advance this watermark: concurrent old callbacks
        // can arrive out of order and none can alter the retired calculator.
        if(!o.call_sequence||!o.mono_ns||
           o.call_sequence<=qualified_observed_position_call_sequence_)
            return fault(PIPELINE_BAD_INPUT);
        // Retain negative evidence even if a live candidate must be faulted:
        // a direct caller can otherwise re-seed the same source/session using
        // an earlier anchor after the old callback has been discarded.
        qualified_stale_position_cutoff_ns_=max64(qualified_stale_position_cutoff_ns_,o.mono_ns);
        qualified_stale_position_call_sequence_=max64(qualified_stale_position_call_sequence_,
                                                      o.call_sequence);
        // Even without a published snapshot, ACTIVE/READY may become eligible
        // after another sensor interval. A delayed GPS decision cannot be
        // ignored across that live seed. A queued anchor can revive a retired
        // calculator before this old callback would otherwise be consumed.
        if(!qualified_retired_)return fault(PIPELINE_BAD_INPUT);
        for(size_t j=0;j<size_;++j)
            if(queue_[j].kind==ANCHOR_EVENT||
               (queue_[j].kind==POSITION_EVENT&&queue_[j].observation.position.mode!=0))
                return fault(PIPELINE_BAD_INPUT);
        // The observer clock is not producer measurement time. These retained
        // boundaries can veto later input, never qualify a new anchor.
        ++status_.rejected;status_.result=PIPELINE_STALE_INPUT;return PIPELINE_STALE_INPUT;
    }
    if(!model_&&owns_qualified_revoker()&&
       ((qualified_stale_position_cutoff_ns_&&
         o.mono_ns<=qualified_stale_position_cutoff_ns_)||
        (qualified_stale_position_call_sequence_&&
         o.call_sequence<=qualified_stale_position_call_sequence_)))
        return fault(PIPELINE_BAD_INPUT);
    Event e=Event(); e.kind=POSITION_EVENT; e.time=e.received=o.mono_ns; e.observation=o;
    return insert(e);
}
PipelineResult Pipeline::enqueue_anchor(const mx5_dr_anchor& a,uint64_t received,
                                        uint64_t position_call_sequence) {
    if (model_) return PIPELINE_BAD_INPUT;
    if(owns_qualified_revoker()&&a.context.generation&&
       a.context.generation<context().generation) {
        // Direct qualified callers must not keep an ACTIVE or queued seed
        // after an old anchor is rejected. The worker also treats BAD_INPUT
        // as fatal, but the Pipeline itself owns publication revocation.
        if(!qualified_retired_)return fault(PIPELINE_BAD_INPUT);
        for(size_t j=0;j<size_;++j)
            if(queue_[j].kind==ANCHOR_EVENT||
               (queue_[j].kind==POSITION_EVENT&&queue_[j].observation.position.mode!=0))
                return fault(PIPELINE_BAD_INPUT);
        ++status_.rejected;status_.result=PIPELINE_BAD_INPUT;return PIPELINE_BAD_INPUT;
    }
    if(owns_qualified_revoker()&&
       ((qualified_stale_position_cutoff_ns_&&
         a.measured_ns<=qualified_stale_position_cutoff_ns_)||
        (qualified_stale_position_call_sequence_&&
         position_call_sequence<=qualified_stale_position_call_sequence_)))
        return fault(PIPELINE_BAD_INPUT);
    if(owns_qualified_revoker()&&!position_call_sequence)return fault(PIPELINE_BAD_INPUT);
    if(owns_qualified_revoker()&&
       position_call_sequence<=qualified_observed_position_call_sequence_) {
        ++status_.rejected;status_.result=PIPELINE_BAD_INPUT;return PIPELINE_BAD_INPUT;
    }
    Event e=Event(); e.kind=ANCHOR_EVENT; e.time=a.measured_ns; e.received=received;
    e.anchor=a;e.anchor_call_sequence=position_call_sequence;
    return insert(e);
}
PipelineResult Pipeline::enqueue_speed(const mx5_dr_evidence& v,double speed) {
    if (model_) return PIPELINE_BAD_INPUT;
    Event e=Event(); e.kind=SPEED_EVENT; e.time=v.measured_ns; e.received=v.received_ns;
    e.evidence=v; e.value=speed; return insert(e);
}
PipelineResult Pipeline::enqueue_yaw(const mx5_dr_evidence& v,double yaw,uint16_t raw,
        uint16_t count,uint64_t begin,uint64_t end) {
    if (model_) return PIPELINE_BAD_INPUT;
    if (begin>=end||v.received_ns<end) return fault(PIPELINE_BAD_INPUT);
    if (status_.have_yaw&&begin<yaw_.window_end&&end>yaw_.time)
        return fault(PIPELINE_BAD_INPUT);
    for (size_t j=0;j<size_;++j)
        if (queue_[j].kind==YAW_EVENT&&begin<queue_[j].window_end&&end>queue_[j].time)
            return fault(PIPELINE_BAD_INPUT);
    Event e=Event(); e.kind=YAW_EVENT; e.time=begin; e.received=v.received_ns;
    e.evidence=v; e.value=yaw; e.raw=raw; e.count=count; e.window_end=end;
    return insert(e);
}
PipelineResult Pipeline::enqueue_reverse(const mx5_dr_evidence& v,int reverse) {
    if (model_) return PIPELINE_BAD_INPUT;
    Event e=Event(); e.kind=REVERSE_EVENT; e.time=v.measured_ns; e.received=v.received_ns;
    e.evidence=v; e.value=reverse; return insert(e);
}
PipelineResult Pipeline::control(mx5_dr_control_kind kind,uint64_t observed_generation) {
    mx5_dr_context x=context();
    if (x.generation==UINT64_MAX || position_seq_==UINT64_MAX) return fault(PIPELINE_BAD_INPUT);
    // A bound qualified worker must use the adapter's observed generation.
    // Local increments could later alias a real adapter transition.
    if (!model_&&owns_qualified_revoker()&&!observed_generation)
        return fault(PIPELINE_BAD_INPUT);
    if (!model_&&observed_generation) {
        // The adapter revokes on every raw mode transition, including GPS
        // quality changes which leave this calculator READY. Its captured
        // generation may therefore advance by more than our control count.
        if(observed_generation<=x.generation)return fault(PIPELINE_BAD_INPUT);
        x.generation=observed_generation;
    } else {
        // MODEL identity (and the original untagged normalized test API) is
        // local. An adapter observation never qualifies a MODEL estimate.
        ++x.generation;
    }
    ++position_seq_;
    status_.core_result=mx5_dr_control(&core_,kind,x,position_seq_);
    if(!model_&&kind!=MX5_DR_GAP)qualified_anchor_paired_=false;
    clear_history();
    // GAP before the first qualified anchor is an expected absence of a
    // solution. The core has accepted and recorded the newer generation; no
    // candidate exists to retire again.
    if (!model_&&kind==MX5_DR_GAP&&status_.core_result==MX5_DR_E_NO_SEED)
        return PIPELINE_NO_ANCHOR;
    return status_.core_result==MX5_DR_OK?PIPELINE_OK:reject_core(PIPELINE_NO_ANCHOR);
}
void Pipeline::clear_history() {
    wheel_history_.size=wheel_history_.next=reverse_history_.size=reverse_history_.next=0;
}
void Pipeline::remember(SensorHistory& history,const Event& e) {
    SensorRecord& record=history.records[history.next];
    record.time=e.time;record.received=e.received;
    record.lease=e.evidence.lease_until_ns;record.value=e.value;
    record.spread=e.wheel_spread;record.wheel_max=e.wheel_max;
    history.next=(history.next+1)%HISTORY_CAPACITY;
    if(history.size<HISTORY_CAPACITY)++history.size;
}
const Pipeline::SensorRecord* Pipeline::causal(const SensorHistory& history,uint64_t time) const {
    // Drain consumes each stream chronologically. Select the latest eligible
    // evidence first; its value must not influence which record is selected.
    for(size_t j=0;j<history.size;++j) {
        const SensorRecord& record=history.records[(history.next+HISTORY_CAPACITY-1-j)%HISTORY_CAPACITY];
        if(record.time<=time&&record.received<=time&&record.lease>=time&&
           time-record.time<=core_.config.sample_age_max_ns)return &record;
    }
    return 0;
}
bool Pipeline::valid_gps_position(const adapter::Observation& o) const {
    const adapter::PositionInput& p=o.position;
    return (p.mode==1||p.mode==2)&&p.utc_seconds&&p.utc_seconds<=UINT64_MAX/1000000000ULL &&
        finite(p.latitude_deg)&&std::fabs(p.latitude_deg)<85 &&
        finite(p.longitude_deg)&&std::fabs(p.longitude_deg)<=180 &&
        finite(p.velocity_kmh)&&p.velocity_kmh>=0&&p.velocity_kmh<=360;
}
bool Pipeline::good_fix(const adapter::Observation& o) const {
    const adapter::PositionInput& p=o.position;
    return valid_gps_position(o)&&finite(p.heading_deg)&&p.heading_deg>=0&&p.heading_deg<360 &&
        p.velocity_kmh>=1.8;
}
bool Pipeline::can_keep_stationary_heading(const adapter::Observation& o) const {
    // A stopped GPS fix cannot establish travel heading. It may leave a prior
    // READY prediction alone only when continuous sensor coverage still carries
    // that heading and the GPS position agrees with its existing MODEL budget.
    // This does not seed from GPS, reset elapsed time, or apply a calibration.
    if(!model_||!valid_gps_position(o)||o.position.velocity_kmh>=1.8 ||
       !core_.seeded||!core_.have_interval||core_.estimate.state!=MX5_DR_READY ||
       core_.estimate.frontier_ns!=o.mono_ns ||
       !status_.have_speed||!status_.have_yaw||!reverse_known() ||
       std::fabs(core_.last_interval.yaw_rad_s)>core_.config.stop_yaw_max_rad_s)
        return false;
    // The worker can already have consumed newer transport samples received
    // after GPS. Use retained causal evidence, including a causal braking
    // endpoint at GPS time; never refresh its original measurement or lease.
    SensorRecord latched;
    const SensorRecord* speed=causal(wheel_history_,o.mono_ns);
    const SensorRecord* reverse=reverse_at(o.mono_ns,&latched);
    if(!speed||!reverse||speed->wheel_max>0.05)return false;
    const uint64_t anchor_utc=core_.anchor.utc_ns/1000000000ULL;
    if(o.position.utc_seconds<anchor_utc ||
       o.position.utc_seconds-anchor_utc>
           (o.mono_ns-core_.anchor.measured_ns)/1000000000ULL+2)
        return false;
    double longitude=o.position.longitude_deg-core_.estimate.longitude_deg;
    if(longitude>180)longitude-=360;
    if(longitude< -180)longitude+=360;
    const double north=(o.position.latitude_deg-core_.estimate.latitude_deg)*111320;
    const double east=longitude*111320*std::cos((o.position.latitude_deg+
        core_.estimate.latitude_deg)*0.5*PI/180);
    return std::sqrt(north*north+east*east)<=
        core_.estimate.error_budget_m+profile_.anchor_error_m;
}
PipelineResult Pipeline::apply_position(const adapter::Observation& o) {
    const int mode=o.position.mode;
    if(!model_&&owns_qualified_revoker()) {
        if(!o.call_sequence||o.call_sequence<=qualified_observed_position_call_sequence_)
            return fault(PIPELINE_BAD_INPUT);
        qualified_observed_position_call_sequence_=o.call_sequence;
        if(mode==1||mode==2) {
            // A mode generation may stay unchanged across many GPS fixes.
            // Only the verified anchor explicitly tied to this callback can
            // authorize its next gap; an earlier READY seed cannot do so.
            const bool paired=qualified_anchor_call_sequence_==o.call_sequence&&
                o.call_sequence>last_qualified_position_call_sequence_&&
                core_.seeded&&core_.estimate.state==MX5_DR_READY&&
                o.prediction_generation==context().generation;
            if(!paired) {
                ++status_.rejected;++status_.unpaired_positions;
                if(core_.seeded) {
                    if(!retire_qualified())return PIPELINE_BAD_INPUT;
                }
                status_.result=PIPELINE_NO_ANCHOR;
                return PIPELINE_NO_ANCHOR;
            }
            last_qualified_position_call_sequence_=o.call_sequence;
            qualified_anchor_call_sequence_=0;
            qualified_anchor_paired_=true;
        } else if(mode==0&&core_.seeded&&!qualified_anchor_paired_) {
            // An anchor by itself is not an adapter GPS decision. Never
            // activate it merely because a later GAP has a newer generation.
            ++status_.rejected;++status_.unpaired_positions;
            if(!retire_qualified())return PIPELINE_BAD_INPUT;
            status_.result=PIPELINE_NO_ANCHOR;
            return PIPELINE_NO_ANCHOR;
        }
    }
    // After invalidate(), the adapter keeps its new generation through
    // same-mode callbacks. A reset qualified calculator is already unseeded;
    // do not ask the core for another transition with that same generation.
    if (!model_&&owns_qualified_revoker()&&!core_.seeded&&position_mode_==-1&&
        (mode==0||mode==3)&&o.prediction_generation==context().generation) {
        position_mode_=mode;return PIPELINE_OK;
    }
    if (mode==3) {
        gps_wheel_.unavailable(); have_fix_=false;
        if (position_mode_!=3) {
            const uint64_t faults=status_.resets;
            const PipelineResult r=control(MX5_DR_NATIVE_POSITION,o.prediction_generation);
            if (status_.resets!=faults) return r;
        }
        position_mode_=3; return PIPELINE_OK;
    }
    if (mode==0) {
        gps_wheel_.unavailable();
        if (position_mode_!=0) { position_mode_=0; return control(MX5_DR_GAP,o.prediction_generation); }
        return PIPELINE_OK;
    }
    // Select the samples for this GPS decision before GPS_RETURN discards the
    // old prediction history. Only these values survive that single decision;
    // no sample time/lease is rewritten or copied into the next history.
    GpsAnchorSupport support=GpsAnchorSupport();
    if(model_&&good_fix(o)) {
        SensorRecord latched;
        const SensorRecord* wheel=causal(wheel_history_,o.mono_ns);
        const SensorRecord* reverse=reverse_at(o.mono_ns,&latched);
        if(wheel) {
            support.wheel_time_ns=wheel->time;support.wheel_received_ns=wheel->received;
            support.wheel_lease_ns=wheel->lease;support.wheel_speed_mps=wheel->value;
            support.wheel_spread_mps=wheel->spread;
        }
        if(reverse) {
            support.reverse_time_ns=reverse->time;support.reverse_received_ns=reverse->received;
            support.reverse_lease_ns=reverse->lease;support.reverse=int(reverse->value);
        }
    }
    if (position_mode_==0||position_mode_==3) {
        const uint64_t faults=status_.resets;
        // A qualified anchor can already have applied this GPS_RETURN (anchors
        // sort before a POSITION with the same time, or carry an earlier
        // measurement time). Its READY seed must survive the matching observed
        // control. A different generation or an ACTIVE estimate still revokes.
        const bool anchored_return=!model_&&(mode==1||mode==2)&&core_.seeded&&
            core_.estimate.state==MX5_DR_READY&&o.prediction_generation&&
            o.prediction_generation==context().generation;
        if(!anchored_return) {
            const PipelineResult r=control(MX5_DR_GPS_RETURN,o.prediction_generation);
            if (status_.resets!=faults) return r;
        }
        have_fix_=false; gps_wheel_.unavailable();
    }
    position_mode_=mode;
    if (!model_) return PIPELINE_OK;
    if (!good_fix(o)) {
        if(can_keep_stationary_heading(o)) {
            gps_wheel_.unavailable(); have_fix_=false;
            return PIPELINE_WAITING;
        }
        gps_wheel_.unavailable(GPS_GATE_BAD_FIX);
        have_fix_=false; control(MX5_DR_DISABLE); return PIPELINE_NO_ANCHOR;
    }
    // GPS travel bearing can be converted to body heading only with reverse
    // evidence already received by this fix and still within its original
    // bounded lease. A later reverse callback cannot repair a prior anchor.
    if (!reverse_known() || !support.reverse_time_ns) {
        gps_wheel_.unavailable(GPS_GATE_REVERSE);
        have_fix_=false; control(MX5_DR_DISABLE); return PIPELINE_NO_ANCHOR;
    }
    bool consistent=false;
    if (gps_wheel_.status().enabled) {
        consistent=gps_wheel_.fix(o,support);
        if (!consistent) {
            // Faster callbacks retain the one-second pair baseline. Actual
            // rejection revokes READY before a subsequent GPS gap.
            if (gps_wheel_.gate()!=GPS_GATE_WAITING) control(MX5_DR_DISABLE);
            return PIPELINE_NO_ANCHOR;
        }
    } else {
        if (!have_fix_) {
            previous_fix_=o; have_fix_=true;
            return PIPELINE_NO_ANCHOR;
        }
        if (o.mono_ns>previous_fix_.mono_ns &&
            o.mono_ns-previous_fix_.mono_ns<=2000000000ULL &&
            o.position.utc_seconds>=previous_fix_.position.utc_seconds) {
            const double dt=double(o.mono_ns-previous_fix_.mono_ns)/1e9;
            const double n=(o.position.latitude_deg-previous_fix_.position.latitude_deg)*111320;
            const double e=(o.position.longitude_deg-previous_fix_.position.longitude_deg)*
                           111320*std::cos(o.position.latitude_deg*PI/180);
            consistent=std::sqrt(n*n+e*e)<=100*dt+20;
        }
        previous_fix_=o;
        if (!consistent) {
            // A rejected pair must not leave an older READY seed available
            // for the next gap, or reuse this rejected endpoint as a baseline.
            have_fix_=false; control(MX5_DR_DISABLE);
            return PIPELINE_NO_ANCHOR;
        }
    }
    if (position_seq_==UINT64_MAX) return fault(PIPELINE_BAD_INPUT);
    mx5_dr_anchor a=mx5_dr_anchor(); a.context=context(); a.anchor_id=++position_seq_;
    a.position_seq=position_seq_; a.measured_ns=o.mono_ns;
    a.utc_ns=o.position.utc_seconds*1000000000ULL;
    a.latitude_deg=o.position.latitude_deg; a.longitude_deg=o.position.longitude_deg;
    a.body_heading_rad=o.position.heading_deg*PI/180;
    // Travel heading becomes body heading only under this stated model. Reverse
    // evidence was checked against the anchor above; reverse rotates by pi.
    if (support.reverse==1) a.body_heading_rad=std::fmod(a.body_heading_rad+PI,2*PI);
    a.position_error_m=profile_.anchor_error_m; a.heading_error_rad=profile_.heading_error_rad;
    a.quality=MX5_DR_MODEL;
    status_.uncertainties|=GPS_TIME_HEADING_MODEL;
    status_.core_result=mx5_dr_seed(&core_,&a);
    if (status_.core_result==MX5_DR_OK) {
        wheel_conflict_since_=0;
        gyro_bias_.apply_at_anchor(o.mono_ns); gps_wheel_.apply_at_anchor(o.mono_ns);
    }
    return status_.core_result==MX5_DR_OK?PIPELINE_OK:PIPELINE_CORE_REJECTED;
}
PipelineResult Pipeline::advance(uint64_t end) {
    // The BETA core never changes the MODEL result: when only BETA is seeded,
    // every obstacle disables BETA and returns the MODEL's original OK.
    const bool beta_only=!core_.seeded&&beta_enabled_&&beta_core_.seeded;
    if (!core_.seeded) wheel_conflict_since_=0;
    if (!core_.seeded&&!beta_only) return PIPELINE_OK;
    if (core_.seeded&&beta_enabled_&&beta_core_.seeded&&
        beta_core_.estimate.frontier_ns!=core_.estimate.frontier_ns) beta_control(MX5_DR_DISABLE);
    const uint64_t frontier=core_.seeded?core_.estimate.frontier_ns:beta_core_.estimate.frontier_ns;
    if(end<=frontier)return PIPELINE_OK;
    uint64_t begin=frontier;
    if (!status_.have_speed||!status_.have_yaw||!reverse_known()||
        yaw_.time>begin || yaw_.window_end<end) {
        if (beta_only) { beta_control(MX5_DR_DISABLE); return PIPELINE_OK; }
        return PIPELINE_WAITING;
    }
    const double yaw_rate=model_?(double(yaw_.raw)-gyro_bias_.status().active_zero)*
        profile_.yaw_rad_per_count:yaw_.value;
    // BETA rule 4: the fixed profile zero, never the stationary auto-bias.
    const double beta_rate=(double(yaw_.raw)-beta_.yaw_zero)*profile_.yaw_rad_per_count;
    // Do not apply the straight GPS-training wheel-spread gate to cornering.
    // This narrower MODEL contradiction needs exactly one stopped wheel, three
    // agreeing moving wheels, and the yaw window for this integration interval.
    // Require fresh conflicting wheel events across a full allowed sensor-age
    // interval, so a brief staggered update while braking does not revoke DR.
    if (beta_only) {
        if(speed_.wheel_zero_conflict&&std::fabs(beta_rate)<=0.03) {
            if(!beta_conflict_since_)beta_conflict_since_=begin;
            if(speed_.time>beta_conflict_since_&&
               speed_.time-beta_conflict_since_>beta_core_.config.sample_age_max_ns) {
                beta_control(MX5_DR_DISABLE); return PIPELINE_OK;
            }
        } else beta_conflict_since_=0;
    } else {
        beta_conflict_since_=0;
        if(model_&&speed_.wheel_zero_conflict&&std::fabs(yaw_rate)<=0.03) {
            if(!wheel_conflict_since_)wheel_conflict_since_=begin;
            if(speed_.time>wheel_conflict_since_&&
               speed_.time-wheel_conflict_since_>core_.config.sample_age_max_ns)
                return fault(PIPELINE_BAD_INPUT);
        } else wheel_conflict_since_=0;
    }
    while (begin<end) {
        if (interval_seq_==UINT64_MAX) {
            if (beta_only) { beta_control(MX5_DR_DISABLE); return PIPELINE_OK; }
            status_.core_result=MX5_DR_E_SEQUENCE;
            return reject_core(PIPELINE_CORE_REJECTED);
        }
        mx5_dr_interval i=mx5_dr_interval(); i.context=context(); i.interval_seq=++interval_seq_;
        const uint64_t step_max=core_.seeded?core_.config.interval_max_ns:beta_core_.config.interval_max_ns;
        i.start_ns=begin; i.end_ns=min64(end,add(begin,step_max));
        mx5_dr_evidence reverse=reverse_.evidence;
        if (reverse_latch_&&!latch_evidence(i.start_ns,&reverse)) {
            if (beta_only) { beta_control(MX5_DR_DISABLE); return PIPELINE_OK; }
            return PIPELINE_WAITING;
        }
        i.received_ns=max64(i.end_ns,max64(speed_.received,max64(yaw_.received,
            reverse_latch_?reverse.received_ns:reverse_.received)));
        if (core_.have_interval) i.received_ns=max64(i.received_ns,core_.last_interval.received_ns);
        i.speed=speed_.evidence; i.yaw=yaw_.evidence; i.reverse=reverse;
        // Without the latch, reverse is held only within the original event's
        // bounded lease. Successful wheel/yaw traffic never refreshes reverse
        // evidence. The MODEL latch restates its unchanged state per interval.
        // Convert at consumption: a fresh anchor may switch zero while this
        // window (or a future queued window) was received with the old zero.
        i.speed_mps=speed_.value*(model_?gps_wheel_.status().active_scale:1.0);
        i.yaw_rad_s=yaw_rate;
        i.reverse_active=reverse_latch_?latch_value_:int(reverse_.value);
        i.raw_yaw=yaw_.raw; i.yaw_count=yaw_.count;
        i.yaw_is_mean=1; i.yaw_window_start_ns=yaw_.time; i.yaw_window_end_ns=yaw_.window_end;
        beta_step(i,beta_rate);
        if (core_.seeded) {
            status_.core_result=mx5_dr_step(&core_,&i);
            if (status_.core_result!=MX5_DR_OK) return reject_core(PIPELINE_CORE_REJECTED);
            ++status_.intervals;
        } else if (!(beta_enabled_&&beta_core_.seeded)) return PIPELINE_OK;
        begin=i.end_ns;
    }
    return PIPELINE_OK;
}
void Pipeline::clear_latch() {
    latch_valid_=false; latch_value_=0;
    latch_time_=latch_received_=latch_epoch_=0;
}
bool Pipeline::reverse_known() const {
    return reverse_latch_?latch_valid_:status_.have_reverse;
}
const Pipeline::SensorRecord* Pipeline::reverse_at(uint64_t time,SensorRecord* latched) const {
    if (!reverse_latch_) return causal(reverse_history_,time);
    if (!latch_valid_||latch_time_>time||latch_received_>time) return 0;
    latched->time=latch_time_; latched->received=latch_received_;
    latched->lease=UINT64_MAX; latched->value=latch_value_;
    latched->spread=0; latched->wheel_max=0;
    return latched;
}
bool Pipeline::latch_evidence(uint64_t start,mx5_dr_evidence* out) {
    // MODEL claim (REVERSE_LATCH_MODEL): the change-only producer's last state
    // still holds at this interval start. A fresh local sequence per interval
    // keeps the core's ordering, age and lease checks unchanged.
    if (!latch_valid_||latch_time_>start||latch_seq_==UINT64_MAX) return false;
    const uint64_t received=max64(start,latch_received_);
    if (received-start>core_.config.sample_age_max_ns) return false;
    mx5_dr_evidence e=mx5_dr_evidence();
    e.source_id=uint64_t(REVERSE); e.source_epoch=latch_epoch_; e.producer_seq=++latch_seq_;
    e.measured_ns=start; e.received_ns=received; e.lease_until_ns=UINT64_MAX;
    e.quality=MX5_DR_MODEL; e.freshness=MX5_DR_MODEL_TIME;
    *out=e; return true;
}
bool Pipeline::enable_beta(const runtime::BetaProfile& p) {
    if (!configured_||!model_||!reverse_latch_) return false;
    const double values[]={p.anchor_error_m,p.heading_error_rad,p.yaw_error_rad_s,p.speed_error_mps,
        p.anchor_speed_min_kmh,p.anchor_speed_max_kmh,p.previous_speed_min_kmh,p.course_step_max_deg,
        p.yaw_quiet_max_rad_s,p.wheel_gps_speed_max_diff_kmh,p.yaw_zero,p.rotation_budget_per_rad,
        p.heading_budget_max_rad,p.accuracy_max_m,p.utc_mono_tolerance_s,p.anchor_hdop_max,
        p.displacement_ratio_min,p.displacement_ratio_max,p.reverse_suspect_kmh};
    for (size_t j=0;j<sizeof values/sizeof values[0];++j)
        if (!finite(values[j])||values[j]<0) return false;
    if (p.anchor_speed_min_kmh<1.8||p.anchor_speed_max_kmh<p.anchor_speed_min_kmh||
        !p.yaw_quiet_window_ns||!p.fix_pair_max_ns||!p.lease_ns||!p.utc_step_max_s||
        !(p.anchor_hdop_max>0)||!(p.displacement_ratio_min>0)||
        p.displacement_ratio_max<p.displacement_ratio_min||!p.reverse_suspect_ns) return false;
    const mx5_dr_config c=runtime::beta_core_config(p);
    mx5_dr_core probe;
    if (mx5_dr_init_model(&probe,&c,context())!=MX5_DR_OK) return false;
    beta_=p; beta_enabled_=true; reset_beta(context());
    return beta_core_.configured!=0;
}
void Pipeline::fence_beta() {
    if (!beta_enabled_) return;
    // Next generation of the BETA core's own context (identity only grows).
    mx5_dr_context x=beta_core_.configured?beta_core_.estimate.context:context();
    if (x.generation<UINT64_MAX) ++x.generation;
    reset_beta(x);
}
void Pipeline::reset_beta(mx5_dr_context x) {
    beta_have_prev_=false; beta_prev_=adapter::Observation(); beta_mode_=-1; beta_core_failure_=MX5_DR_OK;
    beta_position_seq_=0; beta_conflict_since_=0; beta_rotation_rad_=0; beta_rotation_budget_m_=0;
    beta_streak_=false; beta_streak_mono_=beta_streak_utc_=0;
    beta_reverse_suspect_=false; beta_reverse_fast_since_=0;
    beta_yaw_size_=beta_yaw_next_=0;
    if (!beta_enabled_) return;
    const mx5_dr_config c=runtime::beta_core_config(beta_);
    beta_core_result_=mx5_dr_init_model(&beta_core_,&c,x);
    beta_gate_=beta_core_result_==MX5_DR_OK?BETA_GATE_WAITING:BETA_GATE_CORE;
}
mx5_dr_result Pipeline::beta_control(mx5_dr_control_kind kind) {
    if (!beta_enabled_||!beta_core_.configured) return MX5_DR_E_CONFIG;
    mx5_dr_context x=beta_core_.estimate.context;
    if (x.generation==UINT64_MAX||beta_position_seq_==UINT64_MAX) {
        // Exhausted local identity: keep the core configured but unseeded.
        beta_core_.seeded=0; beta_core_.estimate.state=MX5_DR_INVALID;
        beta_core_result_=MX5_DR_E_SEQUENCE; return beta_core_result_;
    }
    ++x.generation; ++beta_position_seq_;
    beta_core_result_=mx5_dr_control(&beta_core_,kind,x,beta_position_seq_);
    return beta_core_result_;
}
void Pipeline::beta_step(const mx5_dr_interval& base,double rate) {
    if (!beta_enabled_||!beta_core_.seeded) return;
    // 3.4: a latched reverse while the wheels exceed 15 km/h for more than
    // 2 s contradicts the latch. Withdraw until a new gated anchor.
    if (base.reverse_active==1&&speed_.value*3.6>beta_.reverse_suspect_kmh) {
        if (!beta_reverse_fast_since_) beta_reverse_fast_since_=base.start_ns;
        if (base.end_ns>beta_reverse_fast_since_&&
            base.end_ns-beta_reverse_fast_since_>beta_.reverse_suspect_ns) {
            beta_reverse_suspect_=true; beta_reverse_fast_since_=0;
            beta_control(MX5_DR_DISABLE); return;
        }
    } else beta_reverse_fast_since_=0;
    mx5_dr_interval b=base; b.context=beta_core_.estimate.context;
    // Rule 4 zero and unscaled wheel speed: no learned calibration in BETA.
    b.speed_mps=speed_.value; b.yaw_rad_s=rate;
    if (beta_core_.have_interval)
        b.received_ns=max64(b.received_ns,beta_core_.last_interval.received_ns);
    beta_core_result_=mx5_dr_step(&beta_core_,&b);
    if (beta_core_result_!=MX5_DR_OK) beta_core_failure_=beta_core_result_;
    if (beta_core_result_==MX5_DR_OK) {
        const double dt=double(b.end_ns-b.start_ns)/1e9;
        beta_rotation_rad_+=std::fabs(rate)*dt;
        // 3.3: the rotation part of hb(tau) in the position budget. R at the
        // interval end bounds R(tau) inside it (R only grows).
        beta_rotation_budget_m_+=b.speed_mps*beta_.rotation_budget_per_rad*beta_rotation_rad_*dt;
    }
}
bool Pipeline::beta_pair_continues(const adapter::Observation& o) const {
    if (!beta_have_prev_) return false;
    const adapter::Observation& q=beta_prev_;
    if (o.mono_ns<=q.mono_ns||o.mono_ns-q.mono_ns>beta_.fix_pair_max_ns||
        o.position.utc_seconds<=q.position.utc_seconds||
        o.position.utc_seconds-q.position.utc_seconds>beta_.utc_step_max_s) return false;
    const double utc_step=double(o.position.utc_seconds-q.position.utc_seconds);
    const double mono_step=double(o.mono_ns-q.mono_ns)/1e9;
    return std::fabs(utc_step-mono_step)<=beta_.utc_mono_tolerance_s;
}
void Pipeline::beta_record(const adapter::Observation& o,BetaAnchorGate gate,double ratio) {
    BetaAnchorRecord& r=beta_records_[beta_record_seq_%BETA_RECORD_CAPACITY];
    r.seq=++beta_record_seq_; r.mono_ns=o.mono_ns; r.utc_s=o.position.utc_seconds;
    r.mode=o.position.mode; r.gate=gate; r.hdop=o.position.horizontal;
    r.kmh=o.position.velocity_kmh; r.displacement_ratio=ratio;
    r.streak_s=beta_streak_&&o.mono_ns>=beta_streak_mono_?double(o.mono_ns-beta_streak_mono_)/1e9:
        std::numeric_limits<double>::quiet_NaN();
}
bool Pipeline::beta_anchor_record(uint64_t seq,BetaAnchorRecord* out) const {
    if (!out||!seq||seq>beta_record_seq_||beta_record_seq_-seq>=BETA_RECORD_CAPACITY) return false;
    const BetaAnchorRecord& r=beta_records_[(seq-1)%BETA_RECORD_CAPACITY];
    if (r.seq!=seq) return false;
    *out=r; return true;
}
void Pipeline::exclude_reverse(LatchClear why) {
    if (reverse_latch_) drop_latch(why);
}
void Pipeline::reset_keep_reverse(mx5_dr_context x) {
    keep_latch_on_reset_=true; reset(x); keep_latch_on_reset_=false;
}
void Pipeline::drop_latch(LatchClear why) {
    if (latch_valid_&&why<LATCH_CLEAR_COUNT) { ++latch_clears_[why]; last_latch_clear_=why; }
    clear_latch();
}
uint64_t Pipeline::latch_clears_total() const {
    uint64_t n=0;
    for (unsigned i=0;i<LATCH_CLEAR_COUNT;++i) n+=latch_clears_[i];
    return n;
}
const char* latch_clear_name(LatchClear why) {
    static const char* const names[]={"reset","source_epoch","rejected_reverse","excluded_reverse","input_gap"};
    return unsigned(why)<sizeof names/sizeof names[0]?names[why]:"unknown";
}
BetaAnchorGate Pipeline::evaluate_beta_gate(const adapter::Observation& o,double* ratio) const {
    const adapter::PositionInput& p=o.position;
    *ratio=std::numeric_limits<double>::quiet_NaN();
    if (!good_fix(o)) return BETA_GATE_BAD_FIX;
    if (!beta_have_prev_) return BETA_GATE_PREVIOUS;
    const adapter::PositionInput& q=beta_prev_.position;
    // 3.1: a pair needs a strictly increasing utc; the same second is not a
    // new measurement and keeps the baseline.
    if (p.utc_seconds==q.utc_seconds) return BETA_GATE_UTC;
    if (o.mono_ns<=beta_prev_.mono_ns||o.mono_ns-beta_prev_.mono_ns>beta_.fix_pair_max_ns||
        p.utc_seconds<q.utc_seconds||p.utc_seconds-q.utc_seconds>beta_.utc_step_max_s)
        return BETA_GATE_PREVIOUS;
    if (!beta_pair_continues(o)) return BETA_GATE_UTC_MONO;
    // 3.2: HDOP of this fix (position horizontal field).
    if (!finite(p.horizontal)||!(p.horizontal>0)||p.horizontal>beta_.anchor_hdop_max)
        return BETA_GATE_HDOP;
    // 3.2: 10 s of consecutive increasing fixes since the first fix or GPS
    // return, on both the receipt clock and the utc clock.
    if (!beta_streak_||o.mono_ns<beta_streak_mono_||
        o.mono_ns-beta_streak_mono_<beta_.anchor_settle_ns||p.utc_seconds<beta_streak_utc_||
        (p.utc_seconds-beta_streak_utc_)*1000000000ULL<beta_.anchor_settle_ns)
        return BETA_GATE_SETTLING;
    if (p.velocity_kmh<beta_.anchor_speed_min_kmh||p.velocity_kmh>beta_.anchor_speed_max_kmh)
        return BETA_GATE_SPEED;
    if (!finite(q.heading_deg)||q.heading_deg<0||q.heading_deg>=360||
        q.velocity_kmh<beta_.previous_speed_min_kmh) return BETA_GATE_PREVIOUS;
    double course=std::fabs(p.heading_deg-q.heading_deg);
    if (course>180) course=360-course;
    if (!(course<=beta_.course_step_max_deg)) return BETA_GATE_COURSE;
    // 3.2: the pair displacement must match the GPS speeds over the utc step.
    {
        double dlon=p.longitude_deg-q.longitude_deg;
        if (dlon>180) dlon-=360;
        if (dlon< -180) dlon+=360;
        const double north=(p.latitude_deg-q.latitude_deg)*111320;
        const double east=dlon*111320*std::cos((p.latitude_deg+q.latitude_deg)*0.5*PI/180);
        const double expected=(p.velocity_kmh+q.velocity_kmh)/2/3.6*
            double(p.utc_seconds-q.utc_seconds);
        *ratio=expected>0?std::sqrt(north*north+east*east)/expected:
            std::numeric_limits<double>::quiet_NaN();
        if (!(*ratio>=beta_.displacement_ratio_min&&*ratio<=beta_.displacement_ratio_max))
            return BETA_GATE_DISPLACEMENT;
    }
    // Contiguous closed yaw windows must cover [fix-window, fix], all quiet.
    if (!beta_yaw_size_||o.mono_ns<beta_.yaw_quiet_window_ns) return BETA_GATE_YAW;
    const uint64_t from=o.mono_ns-beta_.yaw_quiet_window_ns;
    uint64_t covered=0; bool reached=false;
    for (size_t j=0;j<beta_yaw_size_;++j) {
        const YawRecord& y=beta_yaw_[(beta_yaw_next_+HISTORY_CAPACITY-1-j)%HISTORY_CAPACITY];
        if (j==0) { if (y.end<o.mono_ns||y.begin>o.mono_ns) return BETA_GATE_YAW; }
        else if (y.end!=covered) return BETA_GATE_YAW;
        if (!finite(y.rate)||std::fabs(y.rate)>beta_.yaw_quiet_max_rad_s) return BETA_GATE_YAW;
        covered=y.begin;
        if (covered<=from) { reached=true; break; }
    }
    if (!reached) return BETA_GATE_YAW;
    const SensorRecord* wheel=causal(wheel_history_,o.mono_ns);
    if (!wheel||!finite(wheel->value)||
        std::fabs(p.velocity_kmh-wheel->value*3.6)>beta_.wheel_gps_speed_max_diff_kmh)
        return BETA_GATE_WHEEL;
    // Forward only: the GPS course is the body heading. Unknown never seeds.
    SensorRecord latched;
    const SensorRecord* reverse=reverse_at(o.mono_ns,&latched);
    if (!reverse||reverse->value!=0) return BETA_GATE_REVERSE;
    // 3.4: never trust a forward latch before this producer was seen leaving
    // reverse (a 1->0 transition, or a forward first message in this epoch).
    if (!reverse_exit_seen_) return BETA_GATE_REVERSE_UNPROVEN;
    return BETA_GATE_ACCEPTED;
}
void Pipeline::beta_position(const adapter::Observation& o) {
    if (!beta_enabled_||!beta_core_.configured) return;
    const int mode=o.position.mode;
    if (mode==0||mode==3) {
        if (beta_mode_!=mode) beta_control(mode==0?MX5_DR_GAP:MX5_DR_NATIVE_POSITION);
        beta_mode_=mode; beta_have_prev_=false; beta_streak_=false; return;
    }
    if (mode!=1&&mode!=2) {
        beta_control(MX5_DR_DISABLE); beta_gate_=BETA_GATE_BAD_FIX;
        beta_mode_=mode; beta_have_prev_=false; beta_streak_=false;
        beta_record(o,beta_gate_,std::numeric_limits<double>::quiet_NaN()); return;
    }
    if (beta_mode_==0||beta_mode_==3) beta_control(MX5_DR_GPS_RETURN);
    beta_mode_=mode;
    // Rule 3: only a gated fix anchors. A failing fix leaves the previous
    // anchor and its clock untouched; it never re-anchors or disables it.
    double ratio;
    beta_gate_=evaluate_beta_gate(o,&ratio);
    // 3.1-3.2 bookkeeping: a no-fix value (utc 0) ends the run; the same utc
    // second keeps the baseline; any other valid fix becomes the baseline and
    // either continues the run or starts a new one.
    if (!valid_gps_position(o)) { beta_have_prev_=false; beta_streak_=false; }
    else if (beta_gate_!=BETA_GATE_UTC) {
        if (!beta_pair_continues(o)) {
            beta_streak_=true; beta_streak_mono_=o.mono_ns; beta_streak_utc_=o.position.utc_seconds;
        }
        beta_prev_=o; beta_have_prev_=true;
    }
    beta_record(o,beta_gate_,ratio);
    if (beta_gate_!=BETA_GATE_ACCEPTED) return;
    if (beta_position_seq_==UINT64_MAX) { beta_gate_=BETA_GATE_CORE; return; }
    mx5_dr_anchor a=mx5_dr_anchor(); a.context=beta_core_.estimate.context;
    a.anchor_id=a.position_seq=++beta_position_seq_; a.measured_ns=o.mono_ns;
    a.utc_ns=o.position.utc_seconds*1000000000ULL;
    a.latitude_deg=o.position.latitude_deg; a.longitude_deg=o.position.longitude_deg;
    a.body_heading_rad=o.position.heading_deg*PI/180;
    a.position_error_m=beta_.anchor_error_m; a.heading_error_rad=beta_.heading_error_rad;
    a.quality=MX5_DR_MODEL;
    beta_core_result_=mx5_dr_seed(&beta_core_,&a);
    if (beta_core_result_==MX5_DR_OK) {
        beta_core_failure_=MX5_DR_OK;
        beta_rotation_rad_=0; beta_rotation_budget_m_=0; beta_conflict_since_=0;
        beta_reverse_suspect_=false; beta_reverse_fast_since_=0;
    }
    else beta_gate_=BETA_GATE_CORE;
}
PipelineResult Pipeline::drain(uint64_t watermark) {
    if (!configured_) return PIPELINE_BAD_INPUT;
    if (watermark<watermark_) return fault(PIPELINE_CLOCK_RESET);
    while (size_ && queue_[0].time<=watermark) {
        Event e=queue_[0];
        // The first yaw callback supplies only the opening boundary. Until
        // it arrives, even an unseeded model cannot safely commit later raw
        // or GPS times: the next callback may create an earlier mean window.
        if(model_&&!last_yaw_time_) {
            if(watermark>e.time&&
               watermark-e.time>core_.config.sample_age_max_ns)
                return fault(PIPELINE_MISSING_SENSOR);
            break;
        }
        // A mean yaw callback closes the window that began at the previous
        // callback. Keep later wheel/GPS events, including mode transitions,
        // queued until that window arrives. This boundary also applies to a
        // seeded model: a pending GPS revocation must not commit later input
        // before an earlier yaw window. Pending non-gap positions hide the
        // old output; an ACTIVE model's repeated GAP is already mode 0.
        if(model_ && last_yaw_time_ && e.time>last_yaw_time_) {
            if(!core_.seeded && sensor_timeout_due(watermark))
                return fault(PIPELINE_MISSING_SENSOR);
            break;
        }
        const uint64_t faults=status_.resets;
        PipelineResult r=advance(e.time);
        if (status_.resets!=faults) return status_.result;
        // A READY stationary observation may await its closed mean window
        // within the original sensor-age deadline. Pending GPS already hides
        // diagnostics. Other GPS/anchor revocation must drain past missing
        // coverage so a fresh sequence can establish a new anchor.
        if (r!=PIPELINE_OK) {
            bool revoke=false;
            const bool stopped_ready_wait=r==PIPELINE_WAITING&&model_&&core_.seeded&&
                core_.estimate.state==MX5_DR_READY;
            for (size_t j=0;j<size_&&queue_[j].time<=watermark;++j) {
                if(queue_[j].kind==ANCHOR_EVENT)revoke=true;
                if(queue_[j].kind==POSITION_EVENT&&queue_[j].observation.position.mode!=0) {
                    const adapter::Observation& o=queue_[j].observation;
                    if(!stopped_ready_wait||!valid_gps_position(o)||o.position.velocity_kmh>=1.8)
                        revoke=true;
                }
            }
            if (revoke) {
                // Qualified anchors/controls below already revoke explicitly.
                // An extra local DISABLE would consume their generation before
                // the captured adapter transition can be applied. No snapshot
                // escapes this single-owner drain; pending controls also suppress
                // diagnostic/publication before it runs. Keep MODEL semantics.
                if(model_)control(MX5_DR_DISABLE);
                if (status_.resets!=faults) return status_.result;
                have_fix_=false;
            }
            else {
                if (core_.seeded&&watermark>core_.estimate.frontier_ns&&
                    watermark-core_.estimate.frontier_ns>core_.config.sample_age_max_ns)
                    return fault(PIPELINE_MISSING_SENSOR);
                status_.result=r; return r;
            }
        }
        for (size_t j=1;j<size_;++j) queue_[j-1]=queue_[j];
        --size_; watermark_=e.time;
        gps_wheel_.advance(e.time);
        switch(e.kind) {
        case SPEED_EVENT:
            speed_=e; status_.have_speed=true;
            if(model_)remember(wheel_history_,e);
            gps_wheel_.wheels(e.time,e.received,e.value,e.wheel_spread); break;
        case REVERSE_EVENT:
            reverse_=e; status_.have_reverse=true;
            if(model_)remember(reverse_history_,e);
            if(reverse_latch_) {
                // 3.4: proof that this change-only producer reports leaving reverse.
                // Or the first message of this source epoch says forward.
                if(latch_valid_&&latch_value_==1&&int(e.value)==0)reverse_exit_seen_=true;
                if(!reverse_any_seen_&&int(e.value)==0)reverse_exit_seen_=true;
                reverse_any_seen_=true;
                latch_valid_=true; latch_value_=int(e.value); latch_time_=e.time;
                latch_received_=e.received; latch_epoch_=e.evidence.source_epoch;
            }
            gps_wheel_.reverse(e.time,e.received,int(e.value)); break;
        case YAW_EVENT:
            yaw_=e; status_.have_yaw=true;
            if(beta_enabled_) {
                YawRecord& y=beta_yaw_[beta_yaw_next_];
                y.begin=e.time; y.end=e.window_end;
                y.rate=(double(e.raw)-beta_.yaw_zero)*profile_.yaw_rad_per_count;
                beta_yaw_next_=(beta_yaw_next_+1)%HISTORY_CAPACITY;
                if(beta_yaw_size_<HISTORY_CAPACITY)++beta_yaw_size_;
            }
            gps_wheel_.yaw(e.time,e.window_end,e.received,
                (double(e.raw)-gyro_bias_.status().active_zero)*profile_.yaw_rad_per_count); break;
        case ANCHOR_EVENT:
            if (core_.estimate.state==MX5_DR_ACTIVE||core_.estimate.state==MX5_DR_NATIVE||
                e.anchor.context.generation>context().generation) {
                mx5_dr_result cr=mx5_dr_control(&core_,MX5_DR_GPS_RETURN,e.anchor.context,
                    e.anchor.position_seq?e.anchor.position_seq-1:0);
                if (cr!=MX5_DR_OK) {
                    status_.core_result=cr;
                    if(!model_&&owns_qualified_revoker())return reject_core(PIPELINE_CORE_REJECTED);
                    control(MX5_DR_DISABLE);
                    if (status_.resets!=faults) return status_.result;
                    status_.core_result=cr;
                    status_.result=PIPELINE_CORE_REJECTED; return status_.result;
                }
                position_seq_=e.anchor.position_seq-1;
            }
            status_.core_result=mx5_dr_seed(&core_,&e.anchor);
            position_seq_=max64(position_seq_,e.anchor.position_seq);
            if (status_.core_result!=MX5_DR_OK) {
                return reject_core(PIPELINE_CORE_REJECTED);
            }
            // A directly owned qualified Pipeline may recover through a new
            // verified anchor after a fault. It owns the new candidate again,
            // so its next fault/destructor must revoke that generation.
            if(!model_&&qualified_retired_) {
                qualified_retired_=false;retired_from_generation_=0;
            }
            if(!model_&&owns_qualified_revoker()) {
                qualified_anchor_call_sequence_=e.anchor_call_sequence;
                qualified_anchor_paired_=false;
            }
            break;
        case POSITION_EVENT:
            // BETA first: MODEL controls below clear the causal wheel history.
            beta_position(e.observation);
            r=apply_position(e.observation);
            if (status_.resets!=faults) return status_.result;
            if(!model_&&owns_qualified_revoker()&&r!=PIPELINE_OK)return r;
            break;
        }
    }
    uint64_t through=watermark;
    if (status_.have_yaw) through=min64(through,yaw_.window_end);
    PipelineResult r=advance(through);
    // An unseeded MODEL has no core frontier. Silence after a closed yaw
    // window still needs a bounded source-fault reason even with an empty
    // queue; otherwise warmup can appear healthy indefinitely.
    if (model_ && !core_.seeded && sensor_timeout_due(watermark))
        return fault(PIPELINE_MISSING_SENSOR);
    if (core_.seeded && sensor_timeout_due(watermark))
        return fault(PIPELINE_MISSING_SENSOR);
    // A future mean window may begin at the current frontier. Do not commit
    // unused wall time as a measurement watermark.
    if (core_.seeded) watermark_=max64(watermark_,core_.estimate.frontier_ns);
    status_.result=r; return r;
}
Diagnostic Pipeline::diagnostic(uint64_t now) const {
    Diagnostic d=Diagnostic(); d.status=status_;
    d.result=model_?mx5_dr_get_model_snapshot(&core_,now,context(),&d.snapshot):
                    mx5_dr_get_snapshot(&core_,now,context(),&d.snapshot);
    for (size_t i=0;i<size_;++i) {
        if (((queue_[i].kind==POSITION_EVENT&&queue_[i].observation.position.mode!=0)||
             queue_[i].kind==ANCHOR_EVENT)&&queue_[i].time<=now) {
            d.snapshot.valid=0; d.snapshot.model_valid=0;
            d.result=MX5_DR_E_NO_SEED; break;
        }
    }
    return d;
}
bool Pipeline::pending_position(uint64_t mono_ns) const {
    for(size_t i=0;i<size_;++i)
        if(queue_[i].kind==POSITION_EVENT&&queue_[i].time==mono_ns)return true;
    return false;
}
bool Pipeline::sensor_timeout_due(uint64_t watermark_ns) const {
    if(!configured_)return false;
    const uint64_t last=core_.seeded?core_.estimate.frontier_ns:
        (model_?last_yaw_time_:0);
    return last&&watermark_ns>last&&
        watermark_ns-last>core_.config.sample_age_max_ns;
}
bool Pipeline::yaw_source_timeout_due(uint64_t observed_ns) const {
    return configured_&&model_&&last_yaw_time_&&observed_ns>last_yaw_time_&&
        observed_ns-last_yaw_time_>core_.config.sample_age_max_ns;
}
runtime::CoreBridgeResult Pipeline::qualified_snapshot(uint64_t now,
        const runtime::CoreBridgeQualification& q,adapter::DrSnapshot* out) const {
    // Never expose the core sensor lease as an adapter-ready snapshot. The
    // caller may publish this result directly, so it is valid only now.
    return qualified_publication(now,q,now,out);
}
runtime::CoreBridgeResult Pipeline::qualified_publication(uint64_t now,
        const runtime::CoreBridgeQualification& q,uint64_t requested_until,
        adapter::DrSnapshot* out) const {
    if (!out) return runtime::CORE_BRIDGE_NO_OUTPUT;
    *out=adapter::DrSnapshot();
    if (model_||!owns_qualified_revoker()||!qualified_anchor_paired_||
        q.now_mono_ns!=now||diagnostic(now).result!=MX5_DR_OK)
        return runtime::CORE_BRIDGE_UNQUALIFIED;
    for (size_t i=0;i<size_;++i) {
        if ((queue_[i].kind==POSITION_EVENT&&queue_[i].observation.position.mode!=0)||
             queue_[i].kind==ANCHOR_EVENT) {
            // diagnostic already rejected events due now. Do not let a lease
            // cross a known future revocation while the event awaits drain.
            requested_until=min64(requested_until,queue_[i].time-1);
        }
    }
    return runtime::prepare_core_publication(core_,q,requested_until,out);
}
runtime::BetaModelInput Pipeline::model_publication(uint64_t now) const {
    runtime::BetaModelInput in;
    std::memset(&in,0,sizeof in);
    in.now_mono_ns=now; in.lease_cap_mono_ns=UINT64_MAX; in.result=MX5_DR_E_CONFIG;
    if (!configured_||!model_||!beta_enabled_||!beta_core_.configured) return in;
    // Decision 4: query at the frontier (age 0). The core admits a query only
    // after the last interval's receipt, so use that earliest admissible time;
    // the core then adds (v+sv)*age to the budget itself.
    uint64_t query=beta_core_.estimate.frontier_ns;
    if (beta_core_.have_interval) query=max64(query,beta_core_.last_interval.received_ns);
    in.query_mono_ns=query;
    in.result=mx5_dr_get_model_snapshot(&beta_core_,query,beta_core_.estimate.context,&in.snapshot);
    if (in.result==MX5_DR_OK&&now<query) in.result=MX5_DR_E_TIME;
    for (size_t i=0;i<size_;++i) {
        if ((queue_[i].kind==POSITION_EVENT&&queue_[i].observation.position.mode!=0)||
             queue_[i].kind==ANCHOR_EVENT) {
            // A due GPS decision hides the output (as diagnostic() does); a
            // queued one caps the lease before it can apply.
            if (queue_[i].time<=now||queue_[i].time<=query) { in.result=MX5_DR_E_NO_SEED; break; }
            in.lease_cap_mono_ns=min64(in.lease_cap_mono_ns,queue_[i].time-1);
        }
    }
    if (in.result!=MX5_DR_OK) { in.snapshot.valid=0; in.snapshot.model_valid=0; }
    in.heading_budget_rad=in.snapshot.heading_budget_rad+beta_.rotation_budget_per_rad*beta_rotation_rad_;
    in.rotation_rad=beta_rotation_rad_; in.rotation_budget_m=beta_rotation_budget_m_;
    return in;
}
SpeedPublication Pipeline::speed_publication(uint64_t now) const {
    SpeedPublication out;
    std::memset(&out,0,sizeof out);
    if (!configured_||!model_||!beta_enabled_||!status_.have_speed) return out;
    const Event& e=speed_;
    if (!e.time||e.time>now||e.received>now||now-e.time>beta_.lease_ns) return out;
    if (!finite(e.value)||e.value<0||!finite(e.wheel_max)||e.wheel_zero_conflict) return out;
    out.stopped=e.wheel_max<=0.05;
    out.speed_mps=out.stopped?0.0:e.value;
    out.measured_ns=e.time; out.received_ns=e.received; out.ok=true;
    return out;
}
const char* beta_anchor_gate_name(BetaAnchorGate gate) {
    static const char* const names[]={"DISABLED","WAITING","ACCEPTED","BAD_FIX","SPEED",
        "PREVIOUS","COURSE","YAW","WHEEL","REVERSE","CORE","UTC","UTC_MONO","HDOP","SETTLING",
        "DISPLACEMENT","REVERSE_UNPROVEN"};
    return unsigned(gate)<sizeof names/sizeof names[0]?names[gate]:"UNKNOWN";
}
const char* pipeline_result_name(PipelineResult r) {
    static const char* const names[]={"OK","WAITING","BAD_INPUT","LATE","CLOCK_RESET",
        "SOURCE_RESET","OVERFLOW","MISSING_SENSOR","CORE_REJECTED","NO_ANCHOR",
        "STALE_INPUT"};
    return unsigned(r)<sizeof names/sizeof names[0]?names[r]:"UNKNOWN";
}
} }
