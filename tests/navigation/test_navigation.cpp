#include "navigation/pipeline.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
using namespace mx5;
using namespace mx5::navigation;
static unsigned checks;
#define CHECK(x) do { ++checks; if(!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::exit(1); } } while(0)
static uint64_t T(unsigned ms) { return 1000000000ULL+uint64_t(ms)*1000000ULL; }
static RawEvent raw(SensorKind kind,unsigned ms,unsigned seq,unsigned value=13600) {
    RawEvent r=RawEvent(); r.kind=kind; r.epoch=1; r.receive_seq=seq;
    r.received_ns=T(ms); r.source_mono_ms=int64_t(r.received_ns/1000000);
    r.count=1; r.reverse=0;
    for(unsigned i=0;i<4;++i) r.raw[i]=uint16_t(kind==YAW?2047:value);
    return r;
}
static adapter::Observation pos(unsigned ms,int mode,unsigned seq) {
    adapter::Observation o=adapter::Observation(); o.kind=adapter::Observation::POSITION;
    o.call_sequence=seq; o.mono_ns=T(ms); o.original_mode=mode;
    o.position.mode=mode; o.position.utc_seconds=1700000000;
    o.position.latitude_deg=35; o.position.longitude_deg=135;
    o.position.heading_deg=0; o.position.velocity_kmh=36;
    return o;
}
static void init(Pipeline& p) {
    mx5_dr_context x={1,1,1};
    CHECK(p.init_model(research_model_profile(),mx5_dr_default_config(),x));
}
static void feed(Pipeline& p,unsigned ms,unsigned seq,unsigned speed=13600,unsigned yaw=2047) {
    RawEvent w=raw(WHEELS,ms,seq,speed), y=raw(YAW,ms,seq); y.raw[0]=uint16_t(yaw);
    CHECK(p.enqueue_raw(w)==PIPELINE_OK);
    PipelineResult r=p.enqueue_raw(y);
    CHECK(r==PIPELINE_OK||r==PIPELINE_WAITING);
}
static void seeded(Pipeline& p,int direction=0,unsigned speed=13600,unsigned yaw=2047) {
    init(p); RawEvent r=raw(REVERSE,0,1); r.reverse=direction;
    CHECK(p.enqueue_raw(r)==PIPELINE_OK);
    feed(p,0,1,speed,yaw); CHECK(p.enqueue_position(pos(0,1,1))==PIPELINE_OK);
    r=raw(REVERSE,100,2); r.reverse=direction; CHECK(p.enqueue_raw(r)==PIPELINE_OK);
    feed(p,100,2,speed,yaw); CHECK(p.enqueue_position(pos(100,1,2))==PIPELINE_OK);
    CHECK(p.enqueue_position(pos(110,0,3))==PIPELINE_OK);
    r=raw(REVERSE,200,3); r.reverse=direction; CHECK(p.enqueue_raw(r)==PIPELINE_OK);
    feed(p,200,3,speed,yaw); feed(p,300,4,speed,yaw);
    CHECK(p.drain(T(200))==PIPELINE_OK);
}
static void model_motion() {
    Pipeline p; seeded(p);
    Diagnostic d=p.diagnostic(T(300));
    CHECK(d.result==MX5_DR_OK); CHECK(d.snapshot.model_valid==1); CHECK(d.snapshot.valid==0);
    CHECK(d.snapshot.domain==MX5_DR_MODEL_DOMAIN);
    CHECK(std::fabs(d.snapshot.accumulated_north_m-1.0)<1e-6);
    CHECK(std::fabs(d.snapshot.accumulated_east_m)<1e-6);
    CHECK(d.snapshot.state==MX5_DR_ACTIVE);
    CHECK((d.status.uncertainties&REVERSE_LATCH_MODEL)!=0);
    CHECK((d.status.uncertainties&GPS_TIME_HEADING_MODEL)!=0);
    adapter::DrSnapshot out=adapter::DrSnapshot(); runtime::CoreBridgeQualification q=runtime::CoreBridgeQualification();
    q.expected_context=d.snapshot.context; q.now_mono_ns=T(300);
    q.profile_verified=q.input_quality_verified=true;
    q.max_snapshot_age_ns=150000000; q.limits_verified_until_mono_ns=T(300);
    q.duration_max_s=60; q.distance_max_m=1500; q.error_max_m=100;
    CHECK(p.qualified_snapshot(T(300),q,&out)==runtime::CORE_BRIDGE_UNQUALIFIED); CHECK(!out.ready);
    d.snapshot.valid=1; d.snapshot.model_valid=0;
    CHECK(runtime::map_core_snapshot(d.snapshot,q,&out)==runtime::CORE_BRIDGE_UNQUALIFIED);
    CHECK(p.diagnostic(T(500)).result==MX5_DR_E_STALE);
    // A queued GPS return revokes immediately, even before its watermark.
    CHECK(p.enqueue_position(pos(310,1,4))==PIPELINE_OK);
    CHECK(p.diagnostic(T(310)).result==MX5_DR_E_NO_SEED);
    p.drain(T(310)); CHECK(p.diagnostic(T(310)).snapshot.model_valid==0);
    Pipeline native; seeded(native);
    CHECK(native.enqueue_position(pos(310,3,4))==PIPELINE_OK);
    CHECK(!native.diagnostic(T(310)).snapshot.model_valid);
    native.drain(T(310));
    CHECK(native.diagnostic(T(310)).snapshot.state==MX5_DR_NATIVE);
    CHECK(native.enqueue_position(pos(320,0,5))==PIPELINE_OK);
    native.drain(T(320)); CHECK(!native.diagnostic(T(320)).snapshot.model_valid);
}
static void turning_reverse_stop() {
    Pipeline turning; seeded(turning,0,13600,2199);
    Diagnostic d=turning.diagnostic(T(300)); CHECK(d.result==MX5_DR_OK);
    CHECK(d.snapshot.accumulated_east_m<0); // Native positive raw turns left.
    Pipeline reverse; seeded(reverse,1);
    d=reverse.diagnostic(T(300)); CHECK(d.result==MX5_DR_OK);
    // Anchor's GPS travel bearing is north; body is south in reverse.
    CHECK(std::fabs(d.snapshot.body_heading_rad-3.141592653589793)<1e-9);
    CHECK(d.snapshot.accumulated_north_m>0);
    RawEvent change=raw(REVERSE,210,4); change.reverse=0;
    CHECK(reverse.enqueue_raw(change)==PIPELINE_OK);
    feed(reverse,400,5); CHECK(reverse.drain(T(300))==PIPELINE_OK);
    d=reverse.diagnostic(T(400)); CHECK(d.result==MX5_DR_OK);
    CHECK(d.snapshot.accumulated_north_m<0.3);
    Pipeline stop; seeded(stop);
    for(unsigned ms=400;ms<=2100;ms+=100) {
        CHECK(stop.enqueue_raw(raw(REVERSE,ms,ms/100+1))==PIPELINE_OK);
        feed(stop,ms,ms/100+1,10000);
        CHECK(stop.drain(T(ms-100))==PIPELINE_OK);
    }
    d=stop.diagnostic(T(2100)); CHECK(d.result==MX5_DR_OK);
    CHECK(d.snapshot.stopped==1); CHECK(d.snapshot.speed_mps==0); CHECK(!d.snapshot.has_bearing);
}
static void rejection() {
    Pipeline p; seeded(p);
    RawEvent r=raw(YAW,400,5); r.count=0;
    CHECK(p.enqueue_raw(r)==PIPELINE_BAD_INPUT); CHECK(!p.diagnostic(T(400)).snapshot.model_valid);
    seeded(p); r=raw(WHEELS,400,5); r.source_mono_ms=-1;
    CHECK(p.enqueue_raw(r)==PIPELINE_CLOCK_RESET);
    seeded(p); r=raw(REVERSE,400,4); r.reverse=2;
    CHECK(p.enqueue_raw(r)==PIPELINE_BAD_INPUT);
    seeded(p); r=raw(WHEELS,400,5); r.epoch=2;
    CHECK(p.enqueue_raw(r)==PIPELINE_SOURCE_RESET);
    seeded(p); CHECK(p.enqueue_position(pos(50,0,10))==PIPELINE_LATE);
    seeded(p); CHECK(p.drain(T(600))==PIPELINE_MISSING_SENSOR);
    Pipeline no_reverse; init(no_reverse);
    feed(no_reverse,0,1); no_reverse.enqueue_position(pos(0,1,1));
    feed(no_reverse,100,2); no_reverse.enqueue_position(pos(100,1,2));
    no_reverse.enqueue_position(pos(110,0,3)); feed(no_reverse,200,3);
    no_reverse.drain(T(200)); CHECK(!no_reverse.diagnostic(T(200)).snapshot.model_valid);
    Pipeline bounded; init(bounded);
    for(unsigned i=0;i<Pipeline::CAPACITY;++i) CHECK(bounded.enqueue_position(pos(i,1,i+1))==PIPELINE_OK);
    CHECK(bounded.enqueue_position(pos(200,1,999))==PIPELINE_OVERFLOW);
    CHECK(bounded.status().resets>0);
}
static void receipt_worker_and_reacquisition() {
    Pipeline p; init(p);
    for(unsigned ms=0;ms<=800;ms+=50) {
        if (ms%100==0) {
            RawEvent w=raw(WHEELS,ms,ms/100+1), y=raw(YAW,ms,ms/100+1);
            RawEvent r=raw(REVERSE,ms,ms/100+1);
            w.source_mono_ms=y.source_mono_ms=r.source_mono_ms=0;
            CHECK(p.enqueue_raw(w)==PIPELINE_OK);
            CHECK(p.enqueue_raw(r)==PIPELINE_OK);
            PipelineResult result=p.enqueue_raw(y);
            CHECK(result==PIPELINE_OK||result==PIPELINE_WAITING);
        }
        if(ms==0||ms==100) CHECK(p.enqueue_position(pos(ms,1,ms+1))==PIPELINE_OK);
        if(ms==150) CHECK(p.enqueue_position(pos(ms,0,151))==PIPELINE_OK);
        CHECK(p.drain(T(ms)-p.reorder_ns())==PIPELINE_OK);
        if(ms>=300) {
            Diagnostic d=p.diagnostic(T(ms)); CHECK(d.result==MX5_DR_OK);
            CHECK(d.snapshot.model_valid&&!d.snapshot.valid);
            CHECK((d.status.uncertainties&RECEIPT_TIME_MODEL)!=0);
            CHECK((d.status.uncertainties&TRANSPORT_TIME_MODEL)==0);
        }
    }
    // Missing yaw leaves speed at 800 queued before a GPS return. Reacquisition
    // must get past it and cannot use the pre-gap GPS fix as its first fix.
    CHECK(p.enqueue_raw(raw(WHEELS,900,10))==PIPELINE_OK);
    CHECK(p.enqueue_position(pos(910,1,1000))==PIPELINE_OK);
    p.drain(T(910)); CHECK(p.diagnostic(T(910)).snapshot.model_valid==0);
    CHECK(p.enqueue_position(pos(920,0,1001))==PIPELINE_OK);
    p.drain(T(920)); CHECK(!p.diagnostic(T(920)).snapshot.model_valid);
    CHECK(p.status().core_result==MX5_DR_E_NO_SEED);

    // Cross-stream delivery jitter is sorted by retained timestamps, not by
    // insertion order. It cannot move an already committed frontier backward.
    Pipeline jitter; seeded(jitter);
    RawEvent later=raw(WHEELS,400,5), earlier=raw(REVERSE,250,4);
    CHECK(jitter.enqueue_raw(later)==PIPELINE_OK);
    CHECK(jitter.enqueue_raw(earlier)==PIPELINE_OK);
    CHECK(jitter.enqueue_raw(raw(YAW,400,5))==PIPELINE_OK);
    CHECK(jitter.drain(T(300))==PIPELINE_OK);
    CHECK(jitter.diagnostic(T(400)).result==MX5_DR_OK);

    // Wheel/yaw alone do not keep a silent reverse source alive.
    Pipeline silent; seeded(silent);
    for(unsigned ms=400;ms<=700;ms+=100) {
        feed(silent,ms,ms/100+1);
        silent.drain(T(ms-100));
    }
    CHECK(!silent.diagnostic(T(700)).snapshot.model_valid);
}
static void rejected_gps_requires_new_pair() {
    // A rejected GPS pair cannot leave an older READY anchor available for
    // the next gap. Recovery needs two new fixes, not the rejected endpoint.
    for (int mode=1;mode<=2;++mode) {
        Pipeline p; init(p);
        for (unsigned ms=0;ms<=1200;ms+=100) {
            CHECK(p.enqueue_raw(raw(REVERSE,ms,ms/100+1))==PIPELINE_OK);
            feed(p,ms,ms/100+1);
            if (ms==0||ms==100||ms==200||ms==900||ms==1000) {
                adapter::Observation o=pos(ms,mode,ms/100+1);
                if (ms==200) o.position.latitude_deg=36;
                if (ms>=900) o.position.latitude_deg+=double(ms)*0.01/111320;
                CHECK(p.enqueue_position(o)==PIPELINE_OK);
            }
            if (ms==300||ms==1100)
                CHECK(p.enqueue_position(pos(ms-90,0,ms/100+20))==PIPELINE_OK);
            if (ms>=100) CHECK(p.drain(T(ms-100))==PIPELINE_OK);
            if (ms==400) {
                Diagnostic d=p.diagnostic(T(ms));
                CHECK(!d.snapshot.model_valid&&!d.snapshot.valid);
                CHECK(d.snapshot.state!=MX5_DR_READY&&d.snapshot.state!=MX5_DR_ACTIVE);
            }
            if (ms==1000) {
                // The first clean return cannot revive either old endpoint.
                CHECK(p.diagnostic(T(ms)).snapshot.state!=MX5_DR_READY);
                CHECK(!p.diagnostic(T(ms)).snapshot.model_valid);
            }
        }
        Diagnostic d=p.diagnostic(T(1200));
        CHECK(d.result==MX5_DR_OK&&d.snapshot.model_valid&&!d.snapshot.valid);
        CHECK(d.snapshot.state==MX5_DR_ACTIVE);
        CHECK(d.snapshot.latitude_deg>35&&d.snapshot.latitude_deg<35.001);
    }
}
static void single_stopped_wheel_consistency() {
    // A persistent stopped wheel among three agreeing moving wheels is not a
    // plausible straight-motion input. Do not average it into a slower car.
    // Tire differences, ordinary cornering, and brief staggered braking remain
    // distinct; use the completed yaw window, including delayed transport data.
    for(unsigned scenario=0;scenario<6;++scenario) {
        Pipeline p;mx5_dr_context x={1,1,1};
        CHECK(p.init_model(research_model_profile(),mx5_dr_default_config(),x,true,true));
        unsigned rejected=0;
        for(unsigned ms=0;ms<=3050;ms+=50) {
            RawEvent w=raw(WHEELS,ms,ms+1);
            if(ms>=1500) {
                if(scenario==0||scenario==4||scenario==5||
                   (scenario==3&&ms<=1700))w.raw[0]=10000;
                else if(scenario==1) {
                    w.raw[0]=w.raw[2]=13492;w.raw[1]=w.raw[3]=13708; // 9.7/10.3 m/s
                } else if(scenario==2) {
                    w.raw[0]=w.raw[2]=11008;w.raw[1]=w.raw[3]=11152; // 2.8/3.2 m/s
                } else if(scenario==3&&ms<=1900)
                    for(unsigned j=0;j<4;++j)w.raw[j]=10000;
            }
            CHECK(p.enqueue_raw(w)==PIPELINE_OK);
            CHECK(p.enqueue_raw(raw(REVERSE,ms,ms+1))==PIPELINE_OK);
            const bool delayed=(scenario==4||scenario==5)&&ms>=1500;
            if(!delayed||ms%100==50) {
                const unsigned measured=delayed?ms-50:ms;
                RawEvent y=raw(YAW,measured,ms+1);y.received_ns=T(ms);
                if(measured>=1500&&(scenario==2||scenario==4||
                   (scenario==5&&measured<2000)))y.raw[0]=2199;
                const PipelineResult r=p.enqueue_raw(y);
                CHECK(r==PIPELINE_OK||r==PIPELINE_WAITING);
            }
            if(ms==0||ms==1000) {
                adapter::Observation o=pos(ms,1,ms+1);
                o.position.latitude_deg+=double(ms)*0.01/111320;
                o.position.utc_seconds+=ms/1000;
                CHECK(p.enqueue_position(o)==PIPELINE_OK);
            }
            if(ms==1100)CHECK(p.enqueue_position(pos(ms,0,ms+1))==PIPELINE_OK);
            const PipelineResult r=p.drain(T(ms)-p.reorder_ns());
            CHECK(r==PIPELINE_OK||r==PIPELINE_BAD_INPUT);
            if(r==PIPELINE_BAD_INPUT)++rejected;
        }
        const bool bad=scenario==0||scenario==5;
        const Diagnostic d=p.diagnostic(T(3050));
        CHECK(bool(d.snapshot.model_valid)!=bad);
        CHECK(!d.snapshot.valid);
        CHECK(rejected==unsigned(bad));
        CHECK(p.status().resets==unsigned(bad));
    }
}
static mx5_dr_evidence evidence(uint64_t id,uint64_t seq,unsigned ms) {
    mx5_dr_evidence e=mx5_dr_evidence(); e.source_id=id; e.source_epoch=1; e.producer_seq=seq;
    e.measured_ns=e.received_ns=T(ms); e.lease_until_ns=T(ms+250);
    e.quality=MX5_DR_VALID; e.freshness=MX5_DR_PRODUCER_TIME; return e;
}
static void qualified() {
    Pipeline p; mx5_dr_context x={1,1,1}; CHECK(p.init_qualified(mx5_dr_default_config(),x));
    mx5_dr_anchor a=mx5_dr_anchor(); a.context=x; a.anchor_id=a.position_seq=1;
    a.measured_ns=T(0); a.utc_ns=1700000000000000000ULL;
    a.latitude_deg=35; a.longitude_deg=135; a.position_error_m=1;
    a.validated=a.heading_valid=a.calibration_verified=1; a.quality=MX5_DR_VALID;
    CHECK(p.enqueue_anchor(a,T(0))==PIPELINE_OK);
    CHECK(p.enqueue_speed(evidence(1,1,0),10)==PIPELINE_OK);
    CHECK(p.enqueue_reverse(evidence(3,1,0),0)==PIPELINE_OK);
    CHECK(p.enqueue_yaw(evidence(2,1,100),0,2047,1,T(0),T(100))==PIPELINE_OK);
    CHECK(p.enqueue_position(pos(10,0,2))==PIPELINE_OK);
    CHECK(p.drain(T(100))==PIPELINE_OK);
    Diagnostic d=p.diagnostic(T(100)); CHECK(d.result==MX5_DR_OK); CHECK(d.snapshot.valid==1);
    runtime::CoreBridgeQualification q=runtime::CoreBridgeQualification(); q.expected_context=d.snapshot.context;
    q.now_mono_ns=T(100); q.max_snapshot_age_ns=150000000;
    q.limits_verified_until_mono_ns=T(100); q.duration_max_s=60;
    q.distance_max_m=1500; q.error_max_m=100; q.profile_verified=q.input_quality_verified=true;
    adapter::DrSnapshot out=adapter::DrSnapshot(); CHECK(p.qualified_snapshot(T(100),q,&out)==runtime::CORE_BRIDGE_OK); CHECK(out.ready);
    Pipeline stale_anchor=p;
    mx5_dr_anchor returned=a; returned.context=p.context();
    returned.anchor_id=2; returned.position_seq=4; returned.measured_ns=T(100);
    returned.utc_ns+=100000000; returned.latitude_deg+=0.00001;
    CHECK(stale_anchor.enqueue_anchor(returned,T(100))==PIPELINE_OK);
    CHECK(!stale_anchor.diagnostic(T(100)).snapshot.valid);
    CHECK(stale_anchor.drain(T(100))==PIPELINE_CORE_REJECTED);
    CHECK(!stale_anchor.diagnostic(T(100)).snapshot.valid);
    ++returned.context.generation;
    CHECK(p.enqueue_anchor(returned,T(100))==PIPELINE_OK);
    CHECK(!p.diagnostic(T(100)).snapshot.valid);
    CHECK(p.drain(T(100))==PIPELINE_OK);
    CHECK(p.diagnostic(T(100)).snapshot.state==MX5_DR_READY);
    CHECK(p.diagnostic(T(100)).snapshot.anchor_id==2);
    CHECK(p.diagnostic(T(100)).snapshot.latitude_deg==returned.latitude_deg);
    Pipeline bad; CHECK(bad.init_qualified(mx5_dr_default_config(),x));
    CHECK(bad.enqueue_anchor(a,T(0))==PIPELINE_OK);
    mx5_dr_evidence fake=evidence(1,1,0); fake.quality=MX5_DR_MODEL; fake.freshness=MX5_DR_MODEL_TIME;
    CHECK(bad.enqueue_speed(fake,10)==PIPELINE_OK);
    bad.enqueue_reverse(evidence(3,1,0),0);
    bad.enqueue_yaw(evidence(2,1,100),0,2047,1,T(0),T(100));
    bad.enqueue_position(pos(10,0,2));
    CHECK(bad.drain(T(100))==PIPELINE_CORE_REJECTED);
    CHECK(!bad.diagnostic(T(100)).snapshot.valid);
    Pipeline overlap; CHECK(overlap.init_qualified(mx5_dr_default_config(),x));
    CHECK(overlap.enqueue_yaw(evidence(2,1,100),0,2047,1,T(0),T(100))==PIPELINE_OK);
    CHECK(overlap.enqueue_yaw(evidence(2,2,150),0,2047,1,T(50),T(150))==PIPELINE_BAD_INPUT);
    CHECK(!overlap.diagnostic(T(150)).snapshot.valid);
}
int main() { model_motion(); turning_reverse_stop(); rejection(); receipt_worker_and_reacquisition();
    rejected_gps_requires_new_pair();single_stopped_wheel_consistency();qualified();
    std::printf("navigation: %u checks passed\n",checks); return 0; }
