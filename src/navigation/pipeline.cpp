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
    last_yaw_time_(0), interval_seq_(0), position_seq_(0), wheel_conflict_since_(0), position_mode_(-1),
    configured_(false), model_(false), have_fix_(false) {
    std::memset(&core_,0,sizeof core_); std::memset(&status_,0,sizeof status_);
    std::memset(raw_seq_,0,sizeof raw_seq_); std::memset(raw_time_,0,sizeof raw_time_);
    for (unsigned i=0;i<4;++i) raw_transport_[i]=-1;
    clear_history();
    profile_=research_model_profile();
}
bool Pipeline::init_model(const ModelProfile& p,const mx5_dr_config& c,mx5_dr_context x,bool auto_bias,bool gps_wheel) {
    if (!finite(p.yaw_zero)||!finite(p.yaw_rad_per_count)||p.yaw_rad_per_count==0 ||
        !finite(p.wheel_kmh_per_count)||p.wheel_kmh_per_count<=0 ||
        !finite(p.wheel_zero_kmh)||p.reorder_ns>c.sample_age_max_ns ||
        !finite(p.anchor_error_m)||p.anchor_error_m<0 ||
        !finite(p.heading_error_rad)||p.heading_error_rad<0) return false;
    model_=true; profile_=p;
    gyro_bias_.configure(auto_bias,p.yaw_zero,c.sample_age_max_ns);
    gps_wheel_.configure(gps_wheel,c.sample_age_max_ns);
    configured_=mx5_dr_init_model(&core_,&c,x)==MX5_DR_OK;
    if (configured_) reset(x);
    return configured_;
}
bool Pipeline::init_qualified(const mx5_dr_config& c,mx5_dr_context x) {
    model_=false; gyro_bias_.configure(false,profile_.yaw_zero,c.sample_age_max_ns);
    gps_wheel_.configure(false,c.sample_age_max_ns);
    configured_=mx5_dr_init(&core_,&c,x)==MX5_DR_OK;
    if (configured_) reset(x);
    return configured_;
}
void Pipeline::reset(mx5_dr_context x) {
    if (!configured_) return;
    mx5_dr_reset(&core_,x); gyro_bias_.reset(); gps_wheel_.reset(); size_=0; watermark_=0; raw_epoch_=0;
    std::memset(raw_seq_,0,sizeof raw_seq_); std::memset(raw_time_,0,sizeof raw_time_);
    for (unsigned i=0;i<4;++i) raw_transport_[i]=-1;
    last_yaw_time_=0; interval_seq_=0; position_seq_=0; position_mode_=-1;
    wheel_conflict_since_=0;clear_history();
    have_fix_=false; status_.have_speed=status_.have_yaw=status_.have_reverse=false;
    status_.result=PIPELINE_WAITING; status_.core_result=MX5_DR_E_NO_SEED;
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
    } else {
        if (!r.count) return fault(PIPELINE_BAD_INPUT);
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
    clear_history();
    return status_.core_result==MX5_DR_OK?PIPELINE_OK:PIPELINE_NO_ANCHOR;
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
       !status_.have_speed||!status_.have_yaw||!status_.have_reverse ||
       std::fabs(core_.last_interval.yaw_rad_s)>core_.config.stop_yaw_max_rad_s)
        return false;
    // The worker can already have consumed newer transport samples received
    // after GPS. Use retained causal evidence, including a causal braking
    // endpoint at GPS time; never refresh its original measurement or lease.
    const SensorRecord* speed=causal(wheel_history_,o.mono_ns);
    const SensorRecord* reverse=causal(reverse_history_,o.mono_ns);
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
    if (mode==3) {
        gps_wheel_.unavailable(); have_fix_=false;
        if (position_mode_!=3) control(MX5_DR_NATIVE_POSITION);
        position_mode_=3; return PIPELINE_OK;
    }
    if (mode==0) {
        gps_wheel_.unavailable();
        if (position_mode_!=0) { position_mode_=0; return control(MX5_DR_GAP); }
        return PIPELINE_OK;
    }
    // Select the samples for this GPS decision before GPS_RETURN discards the
    // old prediction history. Only these values survive that single decision;
    // no sample time/lease is rewritten or copied into the next history.
    GpsAnchorSupport support=GpsAnchorSupport();
    if(model_&&good_fix(o)) {
        const SensorRecord* wheel=causal(wheel_history_,o.mono_ns);
        const SensorRecord* reverse=causal(reverse_history_,o.mono_ns);
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
        control(MX5_DR_GPS_RETURN); have_fix_=false; gps_wheel_.unavailable();
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
    if (!status_.have_reverse || !support.reverse_time_ns) {
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
    }
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
    if (!core_.seeded) { wheel_conflict_since_=0; return PIPELINE_OK; }
    if(end<=core_.estimate.frontier_ns)return PIPELINE_OK;
    uint64_t begin=core_.estimate.frontier_ns;
    if (!status_.have_speed||!status_.have_yaw||!status_.have_reverse||
        yaw_.time>begin || yaw_.window_end<end) return PIPELINE_WAITING;
    const double yaw_rate=model_?(double(yaw_.raw)-gyro_bias_.status().active_zero)*
        profile_.yaw_rad_per_count:yaw_.value;
    // Do not apply the straight GPS-training wheel-spread gate to cornering.
    // This narrower MODEL contradiction needs exactly one stopped wheel, three
    // agreeing moving wheels, and the yaw window for this integration interval.
    // Require fresh conflicting wheel events across a full allowed sensor-age
    // interval, so a brief staggered update while braking does not revoke DR.
    if(model_&&speed_.wheel_zero_conflict&&std::fabs(yaw_rate)<=0.03) {
        if(!wheel_conflict_since_)wheel_conflict_since_=begin;
        if(speed_.time>wheel_conflict_since_&&
           speed_.time-wheel_conflict_since_>core_.config.sample_age_max_ns)
            return fault(PIPELINE_BAD_INPUT);
    } else wheel_conflict_since_=0;
    while (begin<end) {
        if (interval_seq_==UINT64_MAX) return PIPELINE_CORE_REJECTED;
        mx5_dr_interval i=mx5_dr_interval(); i.context=context(); i.interval_seq=++interval_seq_;
        i.start_ns=begin; i.end_ns=min64(end,add(begin,core_.config.interval_max_ns));
        i.received_ns=max64(i.end_ns,max64(speed_.received,max64(yaw_.received,reverse_.received)));
        if (core_.have_interval) i.received_ns=max64(i.received_ns,core_.last_interval.received_ns);
        i.speed=speed_.evidence; i.yaw=yaw_.evidence; i.reverse=reverse_.evidence;
        // Reverse is held only within the original event's bounded lease.
        // Successful wheel/yaw traffic never refreshes reverse evidence.
        // Convert at consumption: a fresh anchor may switch zero while this
        // window (or a future queued window) was received with the old zero.
        i.speed_mps=speed_.value*(model_?gps_wheel_.status().active_scale:1.0);
        i.yaw_rad_s=yaw_rate;
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
        gps_wheel_.advance(e.time);
        switch(e.kind) {
        case SPEED_EVENT:
            speed_=e; status_.have_speed=true;
            if(model_)remember(wheel_history_,e);
            gps_wheel_.wheels(e.time,e.received,e.value,e.wheel_spread); break;
        case REVERSE_EVENT:
            reverse_=e; status_.have_reverse=true;
            if(model_)remember(reverse_history_,e);
            gps_wheel_.reverse(e.time,e.received,int(e.value)); break;
        case YAW_EVENT:
            yaw_=e; status_.have_yaw=true;
            gps_wheel_.yaw(e.time,e.window_end,e.received,
                (double(e.raw)-gyro_bias_.status().active_zero)*profile_.yaw_rad_per_count); break;
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
