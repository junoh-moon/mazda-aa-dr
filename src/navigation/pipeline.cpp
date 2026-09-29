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
    last_yaw_time_(0), interval_seq_(0), position_seq_(0), position_mode_(-1),
    configured_(false), model_(false), have_fix_(false) {
    std::memset(&core_,0,sizeof core_); std::memset(&status_,0,sizeof status_);
    std::memset(raw_seq_,0,sizeof raw_seq_); std::memset(raw_time_,0,sizeof raw_time_);
    profile_=research_model_profile();
}
bool Pipeline::init_model(const ModelProfile& p,const mx5_dr_config& c,mx5_dr_context x) {
    if (!finite(p.yaw_zero)||!finite(p.yaw_rad_per_count)||p.yaw_rad_per_count==0 ||
        !finite(p.wheel_kmh_per_count)||p.wheel_kmh_per_count<=0 ||
        !finite(p.wheel_zero_kmh)||p.reorder_ns>c.sample_age_max_ns ||
        !finite(p.anchor_error_m)||p.anchor_error_m<0 ||
        !finite(p.heading_error_rad)||p.heading_error_rad<0) return false;
    model_=true; profile_=p;
    configured_=mx5_dr_init_model(&core_,&c,x)==MX5_DR_OK;
    if (configured_) reset(x);
    return configured_;
}
bool Pipeline::init_qualified(const mx5_dr_config& c,mx5_dr_context x) {
    model_=false; configured_=mx5_dr_init(&core_,&c,x)==MX5_DR_OK;
    if (configured_) reset(x);
    return configured_;
}
void Pipeline::reset(mx5_dr_context x) {
    if (!configured_) return;
    mx5_dr_reset(&core_,x); size_=0; watermark_=0; raw_epoch_=0;
    std::memset(raw_seq_,0,sizeof raw_seq_); std::memset(raw_time_,0,sizeof raw_time_);
    last_yaw_time_=0; interval_seq_=0; position_seq_=0; position_mode_=-1;
    have_fix_=false; status_.have_speed=status_.have_yaw=status_.have_reverse=false;
    status_.result=PIPELINE_WAITING; status_.core_result=MX5_DR_E_NO_SEED;
}
PipelineResult Pipeline::fault(PipelineResult r) {
    mx5_dr_context x=context();
    if (x.generation!=UINT64_MAX) ++x.generation;
    else { configured_=false; core_.estimate.valid=0; core_.estimate.model_valid=0; }
    if (configured_) reset(x);
    ++status_.resets; ++status_.rejected; status_.result=r;
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
    raw_seq_[r.kind]=r.receive_seq; raw_time_[r.kind]=time;
    Event e=Event(); e.time=time; e.received=r.received_ns;
    e.evidence.source_id=uint64_t(r.kind); e.evidence.source_epoch=r.epoch;
    e.evidence.producer_seq=r.receive_seq; // MODEL sequence, never physical claim.
    e.evidence.measured_ns=time; e.evidence.received_ns=r.received_ns;
    e.evidence.lease_until_ns=add(time,core_.config.sample_age_max_ns);
    e.evidence.quality=MX5_DR_MODEL; e.evidence.freshness=MX5_DR_MODEL_TIME;
    status_.uncertainties|=PHYSICAL_CALIBRATION_MODEL;
    if (r.kind==WHEELS) {
        e.kind=SPEED_EVENT; double kmh=0;
        for (unsigned i=0;i<4;++i) {
            if (r.raw[i]>40000) return fault(PIPELINE_BAD_INPUT);
            double wheel=r.raw[i]*profile_.wheel_kmh_per_count+profile_.wheel_zero_kmh;
            if (wheel<0) return fault(PIPELINE_BAD_INPUT);
            kmh+=wheel*0.25;
        }
        e.value=kmh/3.6;
    } else if (r.kind==REVERSE) {
        e.kind=REVERSE_EVENT;
        if (profile_.reverse_forward_value<0 || profile_.reverse_reverse_value<0 ||
            profile_.reverse_forward_value==profile_.reverse_reverse_value)
            return fault(PIPELINE_BAD_INPUT);
        if (r.reverse==profile_.reverse_forward_value) e.value=0;
        else if (r.reverse==profile_.reverse_reverse_value) e.value=1;
        else return fault(PIPELINE_BAD_INPUT);
        status_.uncertainties|=REVERSE_ENUM_MODEL|REVERSE_LATCH_MODEL;
    } else {
        if (!r.count) return fault(PIPELINE_BAD_INPUT);
        unsigned mean=unsigned(r.raw[0])/r.count;
        if (mean>=4094) return fault(PIPELINE_BAD_INPUT);
        if (!last_yaw_time_) { last_yaw_time_=time; return PIPELINE_WAITING; }
        if (time==last_yaw_time_) return PIPELINE_WAITING;
        if (time-last_yaw_time_>core_.config.sample_age_max_ns) return fault(PIPELINE_MISSING_SENSOR);
        e.kind=YAW_EVENT; e.time=last_yaw_time_; e.window_end=time;
        e.value=(double(mean)-profile_.yaw_zero)*profile_.yaw_rad_per_count;
        e.raw=uint16_t(mean); e.count=r.count; last_yaw_time_=time;
        status_.uncertainties|=YAW_WINDOW_MODEL;
    }
    return insert(e);
}
PipelineResult Pipeline::enqueue_position(const adapter::Observation& o) {
    if (o.kind!=adapter::Observation::POSITION) return PIPELINE_BAD_INPUT;
    Event e=Event(); e.kind=POSITION_EVENT; e.time=e.received=o.mono_ns; e.observation=o;
    return insert(e);
}
PipelineResult Pipeline::enqueue_anchor(const mx5_dr_anchor& a,uint64_t received) {
    if (model_) return PIPELINE_BAD_INPUT;
    Event e=Event(); e.kind=ANCHOR_EVENT; e.time=a.measured_ns; e.received=received; e.anchor=a;
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
PipelineResult Pipeline::control(mx5_dr_control_kind kind) {
    mx5_dr_context x=context();
    if (x.generation==UINT64_MAX || position_seq_==UINT64_MAX) return fault(PIPELINE_BAD_INPUT);
    ++x.generation; ++position_seq_;
    status_.core_result=mx5_dr_control(&core_,kind,x,position_seq_);
    return status_.core_result==MX5_DR_OK?PIPELINE_OK:PIPELINE_NO_ANCHOR;
}
bool Pipeline::good_fix(const adapter::Observation& o) const {
    const adapter::PositionInput& p=o.position;
    return (p.mode==1||p.mode==2)&&p.utc_seconds&&p.utc_seconds<=UINT64_MAX/1000000000ULL &&
        finite(p.latitude_deg)&&std::fabs(p.latitude_deg)<85 &&
        finite(p.longitude_deg)&&std::fabs(p.longitude_deg)<=180 &&
        finite(p.heading_deg)&&p.heading_deg>=0&&p.heading_deg<360 &&
        finite(p.velocity_kmh)&&p.velocity_kmh>=1.8&&p.velocity_kmh<=360;
}
PipelineResult Pipeline::apply_position(const adapter::Observation& o) {
    const int mode=o.position.mode;
    if (mode==3) {
        have_fix_=false;
        if (position_mode_!=3) control(MX5_DR_NATIVE_POSITION);
        position_mode_=3; return PIPELINE_OK;
    }
    if (mode==0) {
        if (position_mode_!=0) { position_mode_=0; return control(MX5_DR_GAP); }
        return PIPELINE_OK;
    }
    if (position_mode_==0||position_mode_==3) {
        control(MX5_DR_GPS_RETURN); have_fix_=false;
    }
    position_mode_=mode;
    if (!model_) return PIPELINE_OK;
    if (!good_fix(o)) { have_fix_=false; control(MX5_DR_DISABLE); return PIPELINE_NO_ANCHOR; }
    bool consistent=false;
    if (have_fix_ && o.mono_ns>previous_fix_.mono_ns &&
        o.mono_ns-previous_fix_.mono_ns<=2000000000ULL &&
        o.position.utc_seconds>=previous_fix_.position.utc_seconds) {
        const double dt=double(o.mono_ns-previous_fix_.mono_ns)/1e9;
        const double n=(o.position.latitude_deg-previous_fix_.position.latitude_deg)*111320;
        const double e=(o.position.longitude_deg-previous_fix_.position.longitude_deg)*
                       111320*std::cos(o.position.latitude_deg*PI/180);
        consistent=std::sqrt(n*n+e*e)<=100*dt+20;
    }
    previous_fix_=o; have_fix_=true;
    if (!consistent) return PIPELINE_NO_ANCHOR;
    mx5_dr_anchor a=mx5_dr_anchor(); a.context=context(); a.anchor_id=++position_seq_;
    a.position_seq=position_seq_; a.measured_ns=o.mono_ns;
    a.utc_ns=o.position.utc_seconds*1000000000ULL;
    a.latitude_deg=o.position.latitude_deg; a.longitude_deg=o.position.longitude_deg;
    a.body_heading_rad=o.position.heading_deg*PI/180;
    // Travel heading becomes body heading only under this stated model. Reverse
    // anchors require an actual observed reverse state, then rotate by pi.
    if (!status_.have_reverse) return PIPELINE_NO_ANCHOR;
    if (reverse_.value==1) a.body_heading_rad=std::fmod(a.body_heading_rad+PI,2*PI);
    a.position_error_m=profile_.anchor_error_m; a.heading_error_rad=profile_.heading_error_rad;
    a.quality=MX5_DR_MODEL;
    status_.uncertainties|=GPS_TIME_HEADING_MODEL;
    status_.core_result=mx5_dr_seed(&core_,&a);
    return status_.core_result==MX5_DR_OK?PIPELINE_OK:PIPELINE_CORE_REJECTED;
}
PipelineResult Pipeline::advance(uint64_t end) {
    if (!core_.seeded||end<=core_.estimate.frontier_ns) return PIPELINE_OK;
    uint64_t begin=core_.estimate.frontier_ns;
    if (!status_.have_speed||!status_.have_yaw||!status_.have_reverse||
        yaw_.time>begin || yaw_.window_end<end) return PIPELINE_WAITING;
    while (begin<end) {
        if (interval_seq_==UINT64_MAX) return PIPELINE_CORE_REJECTED;
        mx5_dr_interval i=mx5_dr_interval(); i.context=context(); i.interval_seq=++interval_seq_;
        i.start_ns=begin; i.end_ns=min64(end,add(begin,core_.config.interval_max_ns));
        i.received_ns=max64(i.end_ns,max64(speed_.received,max64(yaw_.received,reverse_.received)));
        if (core_.have_interval) i.received_ns=max64(i.received_ns,core_.last_interval.received_ns);
        i.speed=speed_.evidence; i.yaw=yaw_.evidence; i.reverse=reverse_.evidence;
        // Reverse is held only within the original event's bounded lease.
        // Successful wheel/yaw traffic never refreshes reverse evidence.
        i.speed_mps=speed_.value; i.yaw_rad_s=yaw_.value;
        i.reverse_active=int(reverse_.value); i.raw_yaw=yaw_.raw; i.yaw_count=yaw_.count;
        i.yaw_is_mean=1; i.yaw_window_start_ns=yaw_.time; i.yaw_window_end_ns=yaw_.window_end;
        status_.core_result=mx5_dr_step(&core_,&i);
        if (status_.core_result!=MX5_DR_OK) return PIPELINE_CORE_REJECTED;
        ++status_.intervals; begin=i.end_ns;
    }
    return PIPELINE_OK;
}
PipelineResult Pipeline::drain(uint64_t watermark) {
    if (!configured_) return PIPELINE_BAD_INPUT;
    if (watermark<watermark_) return fault(PIPELINE_CLOCK_RESET);
    while (size_ && queue_[0].time<=watermark) {
        Event e=queue_[0];
        PipelineResult r=advance(e.time);
        // Revocation is never held behind absent sensor coverage or earlier
        // queued speed events. Once revoked, drain the chronological backlog
        // unseeded so a new GPS sequence can establish a fresh anchor.
        if (r!=PIPELINE_OK) {
            bool revoke=false;
            for (size_t j=0;j<size_&&queue_[j].time<=watermark;++j)
                if ((queue_[j].kind==POSITION_EVENT&&queue_[j].observation.position.mode!=0)||
                    queue_[j].kind==ANCHOR_EVENT)
                    revoke=true;
            if (revoke) { control(MX5_DR_DISABLE); have_fix_=false; }
            else {
                if (core_.seeded&&watermark>core_.estimate.frontier_ns&&
                    watermark-core_.estimate.frontier_ns>core_.config.sample_age_max_ns)
                    return fault(PIPELINE_MISSING_SENSOR);
                status_.result=r; return r;
            }
        }
        for (size_t j=1;j<size_;++j) queue_[j-1]=queue_[j];
        --size_; watermark_=e.time;
        switch(e.kind) {
        case SPEED_EVENT: speed_=e; status_.have_speed=true; break;
        case REVERSE_EVENT: reverse_=e; status_.have_reverse=true; break;
        case YAW_EVENT: yaw_=e; status_.have_yaw=true; break;
        case ANCHOR_EVENT:
            if (core_.estimate.state==MX5_DR_ACTIVE||core_.estimate.state==MX5_DR_NATIVE||
                e.anchor.context.generation>context().generation) {
                mx5_dr_result cr=mx5_dr_control(&core_,MX5_DR_GPS_RETURN,e.anchor.context,
                    e.anchor.position_seq?e.anchor.position_seq-1:0);
                if (cr!=MX5_DR_OK) {
                    control(MX5_DR_DISABLE); status_.core_result=cr;
                    status_.result=PIPELINE_CORE_REJECTED; return status_.result;
                }
                position_seq_=e.anchor.position_seq-1;
            }
            status_.core_result=mx5_dr_seed(&core_,&e.anchor);
            position_seq_=max64(position_seq_,e.anchor.position_seq);
            if (status_.core_result!=MX5_DR_OK) {
                status_.result=PIPELINE_CORE_REJECTED; return status_.result;
            }
            break;
        case POSITION_EVENT: apply_position(e.observation); break;
        }
    }
    uint64_t through=watermark;
    if (status_.have_yaw) through=min64(through,yaw_.window_end);
    PipelineResult r=advance(through);
    if (core_.seeded && watermark>core_.estimate.frontier_ns &&
        watermark-core_.estimate.frontier_ns>core_.config.sample_age_max_ns)
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
runtime::CoreBridgeResult Pipeline::qualified_snapshot(uint64_t now,
        const runtime::CoreBridgeQualification& q,adapter::DrSnapshot* out) const {
    Diagnostic d=diagnostic(now);
    if (out) std::memset(out,0,sizeof *out);
    if (model_||d.result!=MX5_DR_OK||q.now_mono_ns!=now) return runtime::CORE_BRIDGE_UNQUALIFIED;
    return runtime::map_core_snapshot(d.snapshot,q,out);
}
const char* pipeline_result_name(PipelineResult r) {
    static const char* const names[]={"OK","WAITING","BAD_INPUT","LATE","CLOCK_RESET",
        "SOURCE_RESET","OVERFLOW","MISSING_SENSOR","CORE_REJECTED","NO_ANCHOR"};
    return unsigned(r)<sizeof names/sizeof names[0]?names[r]:"UNKNOWN";
}
} }
