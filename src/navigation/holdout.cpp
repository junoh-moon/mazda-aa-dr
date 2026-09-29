#include "holdout.h"
#include <cmath>

namespace mx5 { namespace navigation {
namespace {
const double PI=3.14159265358979323846;
uint64_t maximum(uint64_t a,uint64_t b) { return a>b?a:b; }
uint64_t minimum(uint64_t a,uint64_t b) { return a<b?a:b; }
uint64_t add(uint64_t a,uint64_t b) { return UINT64_MAX-a<b?UINT64_MAX:a+b; }
double distance(const adapter::PositionInput& a,const adapter::PositionInput& b) {
    const double north=(a.latitude_deg-b.latitude_deg)*111320.0;
    double longitude=a.longitude_deg-b.longitude_deg;
    if(longitude>180)longitude-=360;
    if(longitude< -180)longitude+=360;
    const double east=longitude*111320.0*std::cos((a.latitude_deg+b.latitude_deg)*PI/360);
    return std::sqrt(north*north+east*east);
}
}
HoldoutConfig default_holdout_config() {
    HoldoutConfig c={10000000000ULL,30000000000ULL,2000000000ULL};return c;
}
GpsHoldout::GpsHoldout() : phase_(HOLDOUT_WARMUP), reference_count_(0),
    result_head_(0), result_count_(0), window_id_(0), anchor_ns_(0), end_ns_(0),
    cooldown_until_(0), last_gps_ns_(0), latest_received_ns_(0), watermark_(0), utc_progress_ns_(0), sample_age_ns_(0),
    configured_(false), have_previous_(false) { config_=default_holdout_config(); }
bool GpsHoldout::init_model(const ModelProfile& p,const mx5_dr_config& c,
                            mx5_dr_context x,const HoldoutConfig& h) {
    if(!h.duration_ns||h.duration_ns>60000000000ULL||!h.cooldown_ns||
       !h.gps_timeout_ns||h.gps_timeout_ns>2000000000ULL)return false;
    configured_=pipeline_.init_model(p,c,x,true,true);
    if(!configured_)return false;
    config_=h;sample_age_ns_=c.sample_age_max_ns;phase_=HOLDOUT_WARMUP;reference_count_=result_head_=result_count_=0;
    window_id_=anchor_ns_=end_ns_=cooldown_until_=last_gps_ns_=0;
    latest_received_ns_=watermark_=utc_progress_ns_=0;have_previous_=false;return true;
}
bool GpsHoldout::eligible(const adapter::Observation& o,bool moving) const {
    const adapter::PositionInput& p=o.position;
    return o.kind==adapter::Observation::POSITION&&o.mono_ns&&
        (p.mode==1||p.mode==2)&&p.utc_seconds&&p.utc_seconds<=UINT64_MAX/1000000000ULL&&
        std::isfinite(p.latitude_deg)&&std::fabs(p.latitude_deg)<85&&
        std::isfinite(p.longitude_deg)&&std::fabs(p.longitude_deg)<=180&&
        std::isfinite(p.velocity_kmh)&&p.velocity_kmh>=(moving?1.8:0)&&p.velocity_kmh<=360&&
        std::isfinite(p.heading_deg)&&p.heading_deg>=0&&p.heading_deg<360;
}
bool GpsHoldout::consistent(const adapter::Observation& o) const {
    if(!have_previous_)return true;
    if(o.mono_ns<=previous_.mono_ns||o.mono_ns-previous_.mono_ns>config_.gps_timeout_ns||
       o.position.utc_seconds<previous_.position.utc_seconds)return false;
    const double dt=double(o.mono_ns-previous_.mono_ns)/1e9;
    return double(o.position.utc_seconds-previous_.position.utc_seconds)<=dt+2&&
           distance(o.position,previous_.position)<=100*dt+20;
}
bool GpsHoldout::source_ok(PipelineResult r) const {
    return r==PIPELINE_OK||r==PIPELINE_WAITING||r==PIPELINE_NO_ANCHOR;
}
void GpsHoldout::restart(uint64_t now,bool complete) {
    mx5_dr_context x=pipeline_.context();
    if(x.generation==UINT64_MAX) { configured_=false;return; }
    ++x.generation;
    if(complete)pipeline_.restart_model_prediction(x);
    else pipeline_.reset(x);
    reference_count_=0;have_previous_=false;
    last_gps_ns_=0;phase_=HOLDOUT_COOLDOWN;cooldown_until_=add(now,config_.cooldown_ns);
}
void GpsHoldout::emit(HoldoutEvent event,HoldoutReason reason,
                      const adapter::Observation* o,const mx5_dr_snapshot* s) {
    HoldoutResult r=HoldoutResult();r.event=event;r.reason=reason;
    r.window_id=phase_==HOLDOUT_RUNNING?window_id_:0;
    r.anchor_ns=phase_==HOLDOUT_RUNNING?anchor_ns_:0;
    r.applied_yaw_zero=pipeline_.calibration().active_zero;
    r.calibration_version=pipeline_.calibration().calibration_version;
    r.applied_wheel_scale=pipeline_.wheel_calibration().active_scale;
    r.wheel_scale_version=pipeline_.wheel_calibration().calibration_version;
    if(o) { r.reference=o->position;r.reference_ns=o->mono_ns; }
    if(s) { r.prediction=*s;r.prediction_frontier_ns=s->frontier_ns; }
    if(event==HOLDOUT_COMPARED&&o&&s) {
        adapter::PositionInput p=adapter::PositionInput();
        p.latitude_deg=s->latitude_deg;p.longitude_deg=s->longitude_deg;
        r.position_error_m=distance(p,o->position);
        r.has_heading_error=s->has_bearing&&o->position.velocity_kmh>=1.8;
        if(r.has_heading_error) {
            r.heading_error_rad=::remainder(s->travel_bearing_rad-o->position.heading_deg*PI/180,2*PI);
        }
    }
    // Leave an observable terminal record even if the consumer stops draining.
    if(result_count_==RESULT_CAPACITY) {
        r.event=HOLDOUT_ABORT;r.reason=HOLDOUT_OUTPUT_OVERFLOW;
        results_[(result_head_+result_count_-1)%RESULT_CAPACITY]=r;
        restart(maximum(latest_received_ns_,watermark_));return;
    }
    results_[(result_head_+result_count_)%RESULT_CAPACITY]=r;++result_count_;
}
void GpsHoldout::abort(HoldoutReason reason,uint64_t now) {
    if(phase_!=HOLDOUT_COOLDOWN)emit(HOLDOUT_ABORT,reason,0,0);
    restart(now);
}
void GpsHoldout::reset(mx5_dr_context x,HoldoutReason reason) {
    if(!configured_)return;
    if(phase_!=HOLDOUT_COOLDOWN)emit(HOLDOUT_ABORT,reason,0,0);
    pipeline_.reset(x);reference_count_=0;have_previous_=false;last_gps_ns_=0;
    phase_=HOLDOUT_WARMUP;anchor_ns_=end_ns_=cooldown_until_=watermark_=latest_received_ns_=0;
}
PipelineResult GpsHoldout::enqueue_raw(const RawEvent& r) {
    if(!configured_)return PIPELINE_BAD_INPUT;
    latest_received_ns_=maximum(latest_received_ns_,r.received_ns);
    PipelineResult result=pipeline_.enqueue_raw(r);
    if(!source_ok(result))abort(HOLDOUT_SOURCE_FAULT,r.received_ns);
    return result;
}
PipelineResult GpsHoldout::enqueue_position(const adapter::Observation& o) {
    if(!configured_||o.kind!=adapter::Observation::POSITION)return PIPELINE_BAD_INPUT;
    latest_received_ns_=maximum(latest_received_ns_,o.mono_ns);
    if(o.position.mode==0||o.position.mode==3) {
        abort(o.position.mode==0?HOLDOUT_REAL_GAP:HOLDOUT_NATIVE,o.mono_ns);
        return PIPELINE_NO_ANCHOR;
    }
    // Cooldown is GPS-visible training time. Only RUNNING excludes references
    // from the predictor and its calibration; cooldown still cannot start a
    // new holdout before the configured deadline.
    if(!eligible(o,false)) { abort(HOLDOUT_BAD_GPS,o.mono_ns);return PIPELINE_BAD_INPUT; }
    if(last_gps_ns_&&o.mono_ns<=last_gps_ns_) {
        abort(HOLDOUT_TIME_ORDER,o.mono_ns);return PIPELINE_LATE;
    }
    if(!consistent(o)|| (have_previous_&&o.position.utc_seconds==previous_.position.utc_seconds&&
       o.mono_ns>utc_progress_ns_&&o.mono_ns-utc_progress_ns_>config_.gps_timeout_ns)) {
        abort(HOLDOUT_BAD_GPS,o.mono_ns);return PIPELINE_BAD_INPUT;
    }
    if(!have_previous_||o.position.utc_seconds!=previous_.position.utc_seconds)utc_progress_ns_=o.mono_ns;
    if(reference_count_==REFERENCE_CAPACITY) {
        abort(HOLDOUT_REFERENCE_OVERFLOW,o.mono_ns);return PIPELINE_OVERFLOW;
    }
    references_[reference_count_++]=o;previous_=o;have_previous_=true;last_gps_ns_=o.mono_ns;
    return PIPELINE_OK;
}
void GpsHoldout::remove_reference() {
    for(size_t i=1;i<reference_count_;++i)references_[i-1]=references_[i];
    --reference_count_;
}
void GpsHoldout::drain(uint64_t watermark) {
    if(!configured_)return;
    if(watermark<watermark_) { abort(HOLDOUT_TIME_ORDER,latest_received_ns_);return; }
    watermark_=watermark;
    while(reference_count_&&references_[0].mono_ns<=watermark) {
        const adapter::Observation o=references_[0];
        // A delayed drain may contain older cooldown fixes. The reference's
        // time, not the later worker watermark, determines eligibility.
        if(phase_==HOLDOUT_COOLDOWN&&o.mono_ns>=cooldown_until_)phase_=HOLDOUT_WARMUP;
        if(phase_==HOLDOUT_RUNNING&&o.mono_ns>end_ns_)break;
        if(phase_!=HOLDOUT_RUNNING) {
            // Stationary fixes remain references only; a moving pair is needed
            // to establish the unverified GPS travel-heading/body model.
            if(!eligible(o,true)) { remove_reference();continue; }
            if(!source_ok(pipeline_.enqueue_position(o))) {
                abort(HOLDOUT_SOURCE_FAULT,watermark);return;
            }
        }
        PipelineResult result=pipeline_.drain(o.mono_ns);
        if(!source_ok(result)) { abort(HOLDOUT_SOURCE_FAULT,watermark);return; }
        // A completed mean yaw window may arrive after the reference. Query at
        // actual receipt frontier, while retaining the EXACT prediction time.
        Diagnostic d=pipeline_.diagnostic(maximum(latest_received_ns_,watermark));
        if(phase_!=HOLDOUT_RUNNING) {
            remove_reference();
            if(phase_==HOLDOUT_COOLDOWN)continue;
            if(pipeline_.anchor_gate()!=GPS_GATE_ACCEPTED||
               d.snapshot.state!=MX5_DR_READY||d.snapshot.frontier_ns!=o.mono_ns)continue;
            adapter::Observation gap=adapter::Observation();gap.kind=adapter::Observation::POSITION;
            gap.mono_ns=o.mono_ns;gap.position.mode=0;
            if(!source_ok(pipeline_.enqueue_position(gap))||!source_ok(pipeline_.drain(o.mono_ns))) {
                abort(HOLDOUT_SOURCE_FAULT,watermark);return;
            }
            if(window_id_==UINT64_MAX) { abort(HOLDOUT_TIME_ORDER,watermark);return; }
            ++window_id_;anchor_ns_=o.mono_ns;end_ns_=add(anchor_ns_,config_.duration_ns);
            phase_=HOLDOUT_RUNNING;emit(HOLDOUT_BEGIN,HOLDOUT_NONE,&o,&d.snapshot);
        } else if(phase_==HOLDOUT_RUNNING) {
            if(d.snapshot.frontier_ns<o.mono_ns&&d.snapshot.state==MX5_DR_ACTIVE) {
                if(watermark>d.snapshot.frontier_ns&&watermark-d.snapshot.frontier_ns>sample_age_ns_)
                    abort(HOLDOUT_SOURCE_FAULT,watermark);
                return;
            }
            if(d.snapshot.frontier_ns!=o.mono_ns||d.result!=MX5_DR_OK||
               !d.snapshot.model_valid||d.snapshot.valid||d.snapshot.domain!=MX5_DR_MODEL_DOMAIN) {
                abort(HOLDOUT_PREDICTION_INVALID,watermark);return;
            }
            if(result_count_>=RESULT_CAPACITY-1) { abort(HOLDOUT_OUTPUT_OVERFLOW,watermark);return; }
            emit(HOLDOUT_COMPARED,HOLDOUT_NONE,&o,&d.snapshot);remove_reference();
        } else break;
    }
    if(phase_==HOLDOUT_RUNNING&&last_gps_ns_&&watermark>last_gps_ns_&&
       watermark-last_gps_ns_>config_.gps_timeout_ns) {
        abort(HOLDOUT_GPS_TIMEOUT,watermark);return;
    }
    const uint64_t through=phase_==HOLDOUT_RUNNING?minimum(watermark,end_ns_):watermark;
    PipelineResult result=pipeline_.drain(through);
    if(!source_ok(result)) { abort(HOLDOUT_SOURCE_FAULT,watermark);return; }
    if(phase_==HOLDOUT_RUNNING&&watermark>=end_ns_) {
        Diagnostic d=pipeline_.diagnostic(maximum(latest_received_ns_,watermark));
        if(d.snapshot.frontier_ns<end_ns_&&d.snapshot.state==MX5_DR_ACTIVE) {
            if(watermark>d.snapshot.frontier_ns&&watermark-d.snapshot.frontier_ns>sample_age_ns_)
                abort(HOLDOUT_SOURCE_FAULT,watermark);
            return;
        }
        if(d.snapshot.frontier_ns!=end_ns_||d.result!=MX5_DR_OK) {
            abort(HOLDOUT_PREDICTION_INVALID,watermark);return;
        }
        emit(HOLDOUT_END,HOLDOUT_COMPLETE,0,&d.snapshot);
        // Normal completion starts a new prediction while retaining the same
        // applied MODEL calibration and source binding. Any abort/reset clears
        // it; no candidate evidence crosses this boundary.
        if(phase_==HOLDOUT_RUNNING)restart(watermark,true);
    }
}
bool GpsHoldout::pop(HoldoutResult* out) {
    if(!out||!result_count_)return false;
    *out=results_[result_head_];result_head_=(result_head_+1)%RESULT_CAPACITY;--result_count_;return true;
}
const char* holdout_event_name(HoldoutEvent e) {
    static const char* const n[]={"BEGIN","COMPARED","END","ABORT"};
    return unsigned(e)<sizeof n/sizeof n[0]?n[e]:"unknown";
}
const char* holdout_reason_name(HoldoutReason r) {
    static const char* const n[]={"none","complete","bad_gps","gps_timeout","real_gap","native",
        "source_fault","audit_reset","reference_overflow","output_overflow","time_order","prediction_invalid","capture_stop"};
    return unsigned(r)<sizeof n/sizeof n[0]?n[r]:"unknown";
}
} }
