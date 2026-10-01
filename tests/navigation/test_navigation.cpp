#include "navigation/pipeline.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>
using namespace mx5;
using namespace mx5::navigation;
static_assert(!std::is_copy_assignable<Pipeline>::value,
              "a bound publication owner must not lose its revoker by assignment");
static_assert(!std::is_copy_constructible<Pipeline>::value,
              "a bound publication owner must not be copied");
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
static void init(Pipeline& p,uint64_t generation=1) {
    mx5_dr_context x={1,1,generation};
    CHECK(p.init_model(research_model_profile(),mx5_dr_default_config(),x));
}
static void feed(Pipeline& p,unsigned ms,unsigned seq,unsigned speed=13600,unsigned yaw=2047) {
    RawEvent w=raw(WHEELS,ms,seq,speed), y=raw(YAW,ms,seq); y.raw[0]=uint16_t(yaw);
    CHECK(p.enqueue_raw(w)==PIPELINE_OK);
    PipelineResult r=p.enqueue_raw(y);
    CHECK(r==PIPELINE_OK||r==PIPELINE_WAITING);
}
static void seeded(Pipeline& p,int direction=0,unsigned speed=13600,unsigned yaw=2047,uint64_t generation=1) {
    init(p,generation); RawEvent r=raw(REVERSE,0,1); r.reverse=direction;
    CHECK(p.enqueue_raw(r)==PIPELINE_OK);
    feed(p,0,1,speed,yaw); CHECK(p.enqueue_position(pos(0,1,1))==PIPELINE_OK);
    r=raw(REVERSE,100,2); r.reverse=direction; CHECK(p.enqueue_raw(r)==PIPELINE_OK);
    feed(p,100,2,speed,yaw); CHECK(p.enqueue_position(pos(100,1,2))==PIPELINE_OK);
    CHECK(p.enqueue_position(pos(110,0,3))==PIPELINE_OK);
    r=raw(REVERSE,200,3); r.reverse=direction; CHECK(p.enqueue_raw(r)==PIPELINE_OK);
    feed(p,200,3,speed,yaw); feed(p,300,4,speed,yaw);
    CHECK(p.drain(T(200))==PIPELINE_OK);
}
static void model_waits_for_closed_yaw_window() {
    // Wheel traffic can arrive before a slightly delayed, still-fresh mean
    // yaw window. Without an anchor, consuming those wheels must not make
    // that window's retained start time late before it can be submitted.
    for(unsigned transport=0;transport<2;++transport) {
        Pipeline p;mx5_dr_context context={1,1,1};
        CHECK(p.init_model(research_model_profile(),mx5_dr_default_config(),context,true,true));
        for(unsigned ms=0;ms<=100;ms+=100) {
            for(unsigned kind=WHEELS;kind<=REVERSE;++kind) {
                RawEvent r=raw(static_cast<SensorKind>(kind),ms,ms+1);
                if(!transport)r.source_mono_ms=0;
                const PipelineResult result=p.enqueue_raw(r);
                CHECK(result==PIPELINE_OK||result==PIPELINE_WAITING);
            }
        }
        CHECK(p.drain(T(100))==PIPELINE_OK);
        RawEvent wheel=raw(WHEELS,120,121),yaw=raw(YAW,220,221);
        if(!transport)wheel.source_mono_ms=yaw.source_mono_ms=0;
        CHECK(p.enqueue_raw(wheel)==PIPELINE_OK);
        CHECK(p.drain(T(120))==PIPELINE_OK);
        CHECK(p.enqueue_raw(yaw)==PIPELINE_OK);
        CHECK(!p.status().resets);
        CHECK(p.drain(T(220))==PIPELINE_OK);
        CHECK(!p.diagnostic(T(220)).snapshot.model_valid);
        // An actually missing interval retains the original age rejection.
        yaw=raw(YAW,480,481);if(!transport)yaw.source_mono_ms=0;
        CHECK(p.enqueue_raw(yaw)==PIPELINE_MISSING_SENSOR);
        CHECK(p.status().resets==1);

        Pipeline stopped;init(stopped);
        feed(stopped,0,1);feed(stopped,100,2);
        CHECK(stopped.enqueue_raw(raw(WHEELS,120,3))==PIPELINE_OK);
        CHECK(stopped.drain(T(350))==PIPELINE_OK);
        CHECK(!stopped.status().resets);
        CHECK(stopped.drain(T(351))==PIPELINE_MISSING_SENSOR);
        CHECK(stopped.status().resets==1);
        CHECK(!stopped.diagnostic(T(351)).snapshot.model_valid);
    }
}
static void model_position_waits_for_closed_yaw_window() {
    // A fresh GPS pair can be queued while a bounded yaw mean remains open.
    // Processing the position must not commit past that mean's start time and
    // reject its later arrival as LATE.
    const int transitions[]={-1,0,3};
    for (unsigned scenario=0;scenario<3;++scenario) {
        const int transition=transitions[scenario];
        Pipeline p;init(p);
        RawEvent reverse=raw(REVERSE,0,1);
        CHECK(p.enqueue_raw(reverse)==PIPELINE_OK);
        feed(p,0,1);
        CHECK(p.enqueue_position(pos(0,1,1))==PIPELINE_OK);
        reverse=raw(REVERSE,100,2);
        CHECK(p.enqueue_raw(reverse)==PIPELINE_OK);
        feed(p,100,2);
        CHECK(p.drain(T(100))==PIPELINE_OK);
        CHECK(p.enqueue_raw(raw(WHEELS,120,3))==PIPELINE_OK);
        CHECK(p.enqueue_position(pos(130,1,2))==PIPELINE_OK);
        if (transition>=0)
            CHECK(p.enqueue_position(pos(140,transition,3))==PIPELINE_OK);
        const PipelineResult waiting=p.drain(T(transition>=0?140:130));
        CHECK(waiting==PIPELINE_OK);
        CHECK(p.enqueue_raw(raw(YAW,220,3))==PIPELINE_OK);
        CHECK(p.drain(T(220))==PIPELINE_OK);
        CHECK(!p.status().resets);
        const Diagnostic d=p.diagnostic(T(220));
        CHECK(d.snapshot.state==(transition==3?MX5_DR_NATIVE:
                 transition==0?MX5_DR_ACTIVE:MX5_DR_READY));
        if (transition==0) CHECK(d.snapshot.model_valid);
        if (transition!=3) CHECK(d.snapshot.frontier_ns==T(220));
    }
    // During a GPS gap, repeated mode-0 callbacks are not fresh revocations.
    // Their 100 ms reorder residence must not continuously hide active DR.
    Pipeline gap;seeded(gap);
    CHECK(gap.enqueue_position(pos(310,0,4))==PIPELINE_OK);
    CHECK(gap.enqueue_position(pos(320,0,5))==PIPELINE_OK);
    CHECK(gap.drain(T(220))==PIPELINE_OK);
    CHECK(gap.diagnostic(T(320)).snapshot.model_valid);
    CHECK(gap.enqueue_raw(raw(YAW,400,5))==PIPELINE_OK);
    CHECK(gap.drain(T(400))==PIPELINE_OK);
    CHECK(!gap.status().resets);
    CHECK(gap.diagnostic(T(400)).snapshot.state==MX5_DR_ACTIVE);

    Pipeline missing;seeded(missing);
    CHECK(missing.enqueue_raw(raw(WHEELS,400,5))==PIPELINE_OK);
    CHECK(missing.drain(T(550))==PIPELINE_OK);
    CHECK(missing.drain(T(551))==PIPELINE_MISSING_SENSOR);
    CHECK(missing.status().resets==1);

    Pipeline queued_yaw;seeded(queued_yaw);
    CHECK(queued_yaw.enqueue_raw(raw(YAW,400,5))==PIPELINE_OK);
    CHECK(queued_yaw.enqueue_raw(raw(YAW,500,6))==PIPELINE_OK);
    CHECK(queued_yaw.sensor_timeout_due(T(551)));
    CHECK(!queued_yaw.yaw_source_timeout_due(T(551)));
    CHECK(queued_yaw.yaw_source_timeout_due(T(751)));

    Pipeline first;init(first);
    CHECK(first.enqueue_raw(raw(WHEELS,120,1))==PIPELINE_OK);
    CHECK(first.enqueue_position(pos(130,1,1))==PIPELINE_OK);
    CHECK(first.drain(T(130))==PIPELINE_OK);
    CHECK(first.pending_position(T(130)));
    RawEvent opening=raw(YAW,100,1);opening.received_ns=T(150);
    CHECK(first.enqueue_raw(opening)==PIPELINE_WAITING);
    CHECK(first.enqueue_raw(raw(YAW,220,2))==PIPELINE_OK);
    CHECK(first.drain(T(220))==PIPELINE_OK);
    CHECK(!first.pending_position(T(130))&&!first.status().resets);

    Pipeline silent;init(silent);
    CHECK(silent.enqueue_raw(raw(YAW,0,1))==PIPELINE_WAITING);
    CHECK(silent.enqueue_raw(raw(YAW,100,2))==PIPELINE_OK);
    CHECK(silent.drain(T(350))==PIPELINE_OK);
    CHECK(silent.drain(T(351))==PIPELINE_MISSING_SENSOR);
    CHECK(silent.status().resets==1);
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
    CHECK(p.qualified_publication(T(300),q,T(350),&out)==runtime::CORE_BRIDGE_UNQUALIFIED); CHECK(!out.ready);
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
    CHECK(!native.diagnostic(T(310)).snapshot.model_valid);
    CHECK(native.enqueue_raw(raw(YAW,400,5))==PIPELINE_OK);
    CHECK(native.drain(T(400))==PIPELINE_OK);
    CHECK(native.diagnostic(T(400)).snapshot.state==MX5_DR_NATIVE);
    CHECK(native.enqueue_position(pos(320,0,5))==PIPELINE_OK);
    CHECK(native.drain(T(320))==PIPELINE_OK);
    CHECK(!native.status().resets);
    CHECK(!native.diagnostic(T(320)).snapshot.model_valid);
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
static void raw_yaw_accumulator_rejection(unsigned scenario) {
    // Authored inputs exercised against the original VIP accumulator:
    // 17 * 4093 wraps to 4045; 33 * 2047 wraps to 2015. Dividing either
    // wrapped sum by count invents a small, apparently valid raw mean.
    const unsigned sums[]={4045,2015,4079,2047,2047,65504};
    const unsigned counts[]={17,33,17,256,65535,32};
    CHECK(scenario<sizeof sums/sizeof sums[0]);
    for(unsigned transport=0;transport<2;++transport) {
        Pipeline p;seeded(p);
        CHECK(p.diagnostic(T(300)).snapshot.model_valid);
        const uint64_t generation=p.context().generation;
        RawEvent r=raw(YAW,400,5);r.raw[0]=uint16_t(sums[scenario]);
        r.count=uint16_t(counts[scenario]);
        if(!transport)r.source_mono_ms=0;
        CHECK(p.enqueue_raw(r)==PIPELINE_BAD_INPUT);
        CHECK(r.raw[0]==sums[scenario]&&r.count==counts[scenario]);
        CHECK(p.context().generation==generation+1);
        const Diagnostic d=p.diagnostic(T(400));
        CHECK(!d.snapshot.model_valid&&!d.snapshot.valid);
        CHECK(d.snapshot.state==MX5_DR_UNSEEDED);
        CHECK(!d.status.have_yaw&&!d.status.have_speed&&!d.status.have_reverse);
        RawEvent next=raw(YAW,500,6);
        CHECK(p.enqueue_raw(next)==PIPELINE_WAITING);
        CHECK(!p.diagnostic(T(500)).snapshot.model_valid); // New anchor required.
    }
}
static void raw_yaw_accumulator_boundaries() {
    // A count above 16 is not by itself ambiguous: sum+65536 must still
    // fit count independent 12-bit samples. Preserve the boundary above it.
    const unsigned sums[]={2047,4094,32752,4080,65505};
    const unsigned counts[]={1,2,16,17,32};
    for(unsigned i=0;i<sizeof sums/sizeof sums[0];++i) {
        Pipeline p;init(p);
        RawEvent r=raw(YAW,0,1);r.raw[0]=uint16_t(sums[i]);r.count=uint16_t(counts[i]);
        CHECK(p.enqueue_raw(r)==PIPELINE_WAITING);
        r.receive_seq=2;r.received_ns=T(100);r.source_mono_ms=int64_t(T(100)/1000000);
        CHECK(p.enqueue_raw(r)==PIPELINE_OK);
        CHECK(p.status().resets==0);
    }
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
    // A delayed yaw mean leaves later speed/GPS and GAP queued. Pending GPS
    // immediately hides the old output; the mean must close before those
    // events can advance the watermark or establish a new sequence.
    RawEvent late_wheel=raw(WHEELS,900,10);late_wheel.source_mono_ms=0;
    CHECK(p.enqueue_raw(late_wheel)==PIPELINE_OK);
    CHECK(p.enqueue_position(pos(910,1,1000))==PIPELINE_OK);
    p.drain(T(910)); CHECK(p.diagnostic(T(910)).snapshot.model_valid==0);
    CHECK(p.enqueue_position(pos(920,0,1001))==PIPELINE_OK);
    p.drain(T(920)); CHECK(!p.diagnostic(T(920)).snapshot.model_valid);
    RawEvent closing=raw(YAW,950,11);closing.source_mono_ms=0;
    CHECK(p.enqueue_raw(closing)==PIPELINE_OK);
    CHECK(p.drain(T(950))==PIPELINE_OK);
    CHECK(!p.status().resets);
    const Diagnostic after_gap=p.diagnostic(T(950));
    CHECK(!after_gap.snapshot.model_valid);
    CHECK(after_gap.snapshot.state==MX5_DR_REACQUIRING);
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
static uint64_t test_revoke_generation(void* value) {
    return ++*static_cast<uint64_t*>(value);
}
static void seed_qualified(Pipeline&,uint64_t);
static void qualified() {
    uint64_t published_generation=0,gps_generation=0,anchor_generation=0,stale_generation=0;
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
    Pipeline missing;seed_qualified(missing,1);
    CHECK(missing.drain(T(350))==PIPELINE_OK);
    CHECK(missing.drain(T(351))==PIPELINE_MISSING_SENSOR);
    CHECK(missing.status().resets==1);
    CHECK(!missing.diagnostic(T(351)).snapshot.valid);
    Pipeline pending_gps,pending_anchor,stale_anchor;
    seed_qualified(pending_gps,1);
    seed_qualified(pending_anchor,1);
    seed_qualified(stale_anchor,1);
    runtime::CoreBridgeQualification q=runtime::CoreBridgeQualification(); q.expected_context=d.snapshot.context;
    q.now_mono_ns=T(100); q.max_snapshot_age_ns=150000000;
    q.limits_verified_until_mono_ns=T(100); q.duration_max_s=60;
    q.distance_max_m=1500; q.error_max_m=100; q.profile_verified=q.input_quality_verified=true;
    adapter::DrSnapshot out=adapter::DrSnapshot();
    CHECK(p.qualified_snapshot(T(100),q,&out)==runtime::CORE_BRIDGE_UNQUALIFIED&&!out.ready);
    CHECK(p.qualified_publication(T(100),q,T(150),&out)==runtime::CORE_BRIDGE_UNQUALIFIED&&!out.ready);
    published_generation=gps_generation=anchor_generation=stale_generation=p.context().generation;
    CHECK(p.bind_qualified_revoker(test_revoke_generation,&published_generation));
    CHECK(pending_gps.bind_qualified_revoker(test_revoke_generation,&gps_generation));
    CHECK(pending_anchor.bind_qualified_revoker(test_revoke_generation,&anchor_generation));
    CHECK(stale_anchor.bind_qualified_revoker(test_revoke_generation,&stale_generation));
    CHECK(p.qualified_snapshot(T(100),q,&out)==runtime::CORE_BRIDGE_OK&&out.ready);
    CHECK(out.valid_until_mono_ns==T(100));
    CHECK(published_generation==p.context().generation);
    CHECK(p.qualified_publication(T(100),q,T(150),&out)==runtime::CORE_BRIDGE_OK);
    CHECK(out.ready&&out.valid_until_mono_ns==T(150));
    CHECK(out.frontier_mono_ns==d.snapshot.frontier_ns&&out.derived_utc_ns==d.snapshot.derived_utc_ns);
    CHECK(p.diagnostic(T(100)).snapshot.solution_seq==d.snapshot.solution_seq);
    CHECK(p.qualified_publication(T(101),q,T(150),&out)==runtime::CORE_BRIDGE_UNQUALIFIED&&!out.ready);
    CHECK(p.qualified_publication(T(100),q,T(150),0)==runtime::CORE_BRIDGE_NO_OUTPUT);
    // A known future control bounds publication before drain consumes it.
    CHECK(pending_gps.enqueue_position(pos(130,1,3))==PIPELINE_OK);
    CHECK(pending_gps.qualified_snapshot(T(100),q,&out)==runtime::CORE_BRIDGE_OK);
    CHECK(out.valid_until_mono_ns==T(100));
    CHECK(pending_gps.qualified_publication(T(100),q,T(150),&out)==runtime::CORE_BRIDGE_OK);
    CHECK(out.valid_until_mono_ns==T(130)-1);
    runtime::CoreBridgeQualification later=q;later.now_mono_ns=later.limits_verified_until_mono_ns=T(130);
    CHECK(pending_gps.qualified_publication(T(130),later,T(150),&out)==runtime::CORE_BRIDGE_UNQUALIFIED&&!out.ready);
    mx5_dr_anchor future=a;
    future.context=p.context();++future.context.generation;
    future.anchor_id=2;future.position_seq=4;future.measured_ns=T(140);future.utc_ns+=140000000;
    CHECK(pending_anchor.enqueue_anchor(future,T(140))==PIPELINE_OK);
    CHECK(pending_anchor.qualified_publication(T(100),q,T(150),&out)==runtime::CORE_BRIDGE_OK);
    CHECK(out.valid_until_mono_ns==T(140)-1);
    mx5_dr_anchor returned=a; returned.context=p.context();
    returned.anchor_id=2; returned.position_seq=4; returned.measured_ns=T(100);
    returned.utc_ns+=100000000; returned.latitude_deg+=0.00001;
    CHECK(stale_anchor.enqueue_anchor(returned,T(100))==PIPELINE_OK);
    CHECK(!stale_anchor.diagnostic(T(100)).snapshot.valid);
    CHECK(stale_anchor.qualified_publication(T(100),q,T(150),&out)==runtime::CORE_BRIDGE_UNQUALIFIED&&!out.ready);
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
static void exhausted_model() {
    Pipeline p;seeded(p,0,13600,2047,UINT64_MAX-1);
    const Diagnostic before=p.diagnostic(T(300));
    CHECK(before.snapshot.model_valid);CHECK(before.result==MX5_DR_OK);
    CHECK(p.context().generation==UINT64_MAX);
    uint8_t preview[48];CHECK(runtime::encode_model_location_preview(before.snapshot,preview));
    RawEvent bad=raw(YAW,310,5);bad.count=0;
    CHECK(p.enqueue_raw(bad)==PIPELINE_BAD_INPUT);
    const Diagnostic after=p.diagnostic(T(310));
    std::printf("terminal MODEL: before_valid=%d after_valid=%d result=%s pipeline=%s\n",
                before.snapshot.model_valid,after.snapshot.model_valid,mx5_dr_result_name(after.result),
                pipeline_result_name(after.status.result));std::fflush(stdout);
    CHECK(!after.snapshot.model_valid&&!after.snapshot.valid);
    CHECK(!runtime::encode_model_location_preview(after.snapshot,preview));
    CHECK(p.context().generation==UINT64_MAX);
    CHECK(after.status.resets==before.status.resets+1&&after.status.rejected==before.status.rejected+1);
    CHECK(!after.status.have_speed&&!after.status.have_yaw&&!after.status.have_reverse);
    CHECK(p.drain(T(310))==PIPELINE_BAD_INPUT);
    CHECK(p.enqueue_raw(raw(WHEELS,320,6))==PIPELINE_BAD_INPUT);
    mx5_dr_context fresh={1,1,1};p.reset(fresh);
    CHECK(p.context().generation==UINT64_MAX); // reset cannot re-enable a terminal Pipeline.
    CHECK(!p.restart_model_prediction(fresh));
    CHECK(!p.diagnostic(T(320)).snapshot.model_valid);
    seeded(p);CHECK(p.diagnostic(T(300)).snapshot.model_valid); // Explicit init can recover.
}
static void seed_qualified(Pipeline& p,uint64_t generation) {
    mx5_dr_context x={1,1,generation};CHECK(p.init_qualified(mx5_dr_default_config(),x));
    mx5_dr_anchor a=mx5_dr_anchor();a.context=x;a.anchor_id=a.position_seq=1;
    a.measured_ns=T(0);a.utc_ns=1700000000000000000ULL;
    a.latitude_deg=35;a.longitude_deg=135;a.position_error_m=1;
    a.validated=a.heading_valid=a.calibration_verified=1;a.quality=MX5_DR_VALID;
    CHECK(p.enqueue_anchor(a,T(0))==PIPELINE_OK);
    CHECK(p.enqueue_speed(evidence(1,1,0),10)==PIPELINE_OK);
    CHECK(p.enqueue_reverse(evidence(3,1,0),0)==PIPELINE_OK);
    CHECK(p.enqueue_yaw(evidence(2,1,100),0,2047,1,T(0),T(100))==PIPELINE_OK);
    CHECK(p.enqueue_position(pos(10,0,2))==PIPELINE_OK);
    CHECK(p.drain(T(100))==PIPELINE_OK);
    CHECK(p.diagnostic(T(100)).snapshot.valid);
}
struct RevocationResponse { uint64_t value; unsigned calls; };
static uint64_t fixed_revoke_generation(void* opaque) {
    RevocationResponse& response=*static_cast<RevocationResponse*>(opaque);
    ++response.calls;return response.value;
}
static void qualified_revoker_failure_is_terminal() {
    const uint64_t invalid[]={0,2,uint64_t(UINT32_MAX)+1};
    for(unsigned i=0;i<sizeof invalid/sizeof invalid[0];++i) {
        RevocationResponse response={invalid[i],0};
        Pipeline p;seed_qualified(p,1);
        CHECK(p.bind_qualified_revoker(fixed_revoke_generation,&response));
        CHECK(p.enqueue_yaw(evidence(2,2,110),0,2047,1,T(110),T(110))==PIPELINE_BAD_INPUT);
        CHECK(response.calls==1&&p.context().generation==UINT64_MAX);
        CHECK(!p.diagnostic(T(110)).snapshot.valid);
        CHECK(p.drain(T(120))==PIPELINE_BAD_INPUT);
        CHECK(response.calls==1);
    }
    RevocationResponse response={3,0};
    Pipeline reset_pipeline;seed_qualified(reset_pipeline,1);
    CHECK(reset_pipeline.bind_qualified_revoker(fixed_revoke_generation,&response));
    mx5_dr_context requested={1,1,999};reset_pipeline.reset(requested);
    CHECK(response.calls==1&&reset_pipeline.context().generation==3);
    CHECK(!reset_pipeline.diagnostic(T(110)).snapshot.valid);

    RevocationResponse model_response={3,0};
    Pipeline model_switch;seed_qualified(model_switch,1);
    CHECK(model_switch.bind_qualified_revoker(fixed_revoke_generation,&model_response));
    mx5_dr_context model_context={1,1,3};
    CHECK(model_switch.init_model(research_model_profile(),mx5_dr_default_config(),model_context));
    CHECK(model_response.calls==1);
    CHECK(!model_switch.diagnostic(T(110)).snapshot.valid);
    for(unsigned i=0;i<sizeof invalid/sizeof invalid[0];++i) {
        RevocationResponse failed_model_response={invalid[i],0};
        Pipeline failed_model;seed_qualified(failed_model,1);
        CHECK(failed_model.bind_qualified_revoker(fixed_revoke_generation,&failed_model_response));
        CHECK(!failed_model.init_model(research_model_profile(),mx5_dr_default_config(),model_context));
        CHECK(failed_model_response.calls==1&&failed_model.context().generation==UINT64_MAX);
        CHECK(!failed_model.diagnostic(T(110)).snapshot.valid);
        CHECK(failed_model.drain(T(110))==PIPELINE_BAD_INPUT);
    }
    for(unsigned cause=0;cause<2;++cause) {
        RevocationResponse failed_profile_response={3,0};
        Pipeline failed_profile;seed_qualified(failed_profile,1);
        CHECK(failed_profile.bind_qualified_revoker(fixed_revoke_generation,&failed_profile_response));
        ModelProfile profile=research_model_profile();
        mx5_dr_config config=mx5_dr_default_config();
        if(cause==0)profile.yaw_rad_per_count=0;
        else config.sample_age_max_ns=0;
        CHECK(!failed_profile.init_model(profile,config,model_context));
        CHECK(failed_profile_response.calls==1&&failed_profile.context().generation==3);
        CHECK(!failed_profile.diagnostic(T(110)).snapshot.valid);
        CHECK(failed_profile.drain(T(110))==PIPELINE_BAD_INPUT);
    }

    RevocationResponse missing_generation={3,0};
    Pipeline untagged;seed_qualified(untagged,1);
    CHECK(untagged.bind_qualified_revoker(fixed_revoke_generation,&missing_generation));
    CHECK(untagged.enqueue_position(pos(110,3,3))==PIPELINE_OK);
    CHECK(untagged.drain(T(110))==PIPELINE_BAD_INPUT);
    CHECK(missing_generation.calls==1&&untagged.context().generation==3);
    CHECK(!untagged.diagnostic(T(110)).snapshot.valid);

    RevocationResponse same_mode_response={3,0};
    Pipeline same_mode;seed_qualified(same_mode,1);
    CHECK(same_mode.bind_qualified_revoker(fixed_revoke_generation,&same_mode_response));
    CHECK(same_mode.enqueue_yaw(evidence(2,2,110),0,2047,1,T(110),T(110))==PIPELINE_BAD_INPUT);
    CHECK(same_mode_response.calls==1&&same_mode.context().generation==3);
    adapter::Observation stale=pos(115,0,4);stale.prediction_generation=2;
    CHECK(same_mode.enqueue_position(stale)==PIPELINE_BAD_INPUT);
    mx5_dr_anchor stale_anchor=mx5_dr_anchor();stale_anchor.context=mx5_dr_context{1,1,2};
    CHECK(same_mode.enqueue_anchor(stale_anchor,T(115))==PIPELINE_BAD_INPUT);
    CHECK(same_mode_response.calls==1&&same_mode.status().resets==1);
    adapter::Observation gap=pos(120,0,4);gap.prediction_generation=3;
    CHECK(same_mode.enqueue_position(gap)==PIPELINE_OK);
    CHECK(same_mode.drain(T(120))==PIPELINE_OK);
    CHECK(same_mode_response.calls==1&&same_mode.status().resets==1);
    gap=pos(130,0,5);gap.prediction_generation=3;
    CHECK(same_mode.enqueue_position(gap)==PIPELINE_OK);
    CHECK(same_mode.drain(T(130))==PIPELINE_OK);
    CHECK(same_mode_response.calls==1&&same_mode.context().generation==3);

    uint64_t unseeded_generation=1;
    Pipeline unseeded;mx5_dr_context initial={1,1,1};
    CHECK(unseeded.init_qualified(mx5_dr_default_config(),initial));
    CHECK(unseeded.bind_qualified_revoker(test_revoke_generation,&unseeded_generation));
    adapter::Observation gps_first=pos(10,1,1);gps_first.prediction_generation=1;
    CHECK(unseeded.enqueue_position(gps_first)==PIPELINE_OK);
    CHECK(unseeded.drain(T(10))==PIPELINE_OK);
    adapter::Observation first_gap=pos(20,0,2);first_gap.prediction_generation=2;
    CHECK(unseeded.enqueue_position(first_gap)==PIPELINE_OK);
    CHECK(unseeded.drain(T(20))==PIPELINE_OK);
    CHECK(unseeded.status().resets==0&&unseeded.context().generation==2);
    CHECK(unseeded_generation==1&&unseeded.status().core_result==MX5_DR_E_NO_SEED);

    RevocationResponse lifetime_response={3,0};
    {
        Pipeline owned;seed_qualified(owned,1);
        CHECK(owned.bind_qualified_revoker(fixed_revoke_generation,&lifetime_response));
    }
    CHECK(lifetime_response.calls==1);
}
static void qualified_coverage(Pipeline& p) {
    CHECK(p.enqueue_speed(evidence(1,2,100),10)==PIPELINE_OK);
    CHECK(p.enqueue_reverse(evidence(3,2,100),0)==PIPELINE_OK);
    CHECK(p.enqueue_yaw(evidence(2,2,200),0,2047,1,T(100),T(200))==PIPELINE_OK);
}
static void qualified_anchor_before_observed_return() {
    for(unsigned variant=0;variant<3;++variant) {
        Pipeline p;seed_qualified(p,1);qualified_coverage(p);
        mx5_dr_anchor a=mx5_dr_anchor();a.context=p.context();++a.context.generation;
        a.anchor_id=2;a.position_seq=4;a.measured_ns=T(110);a.utc_ns=1700000000110000000ULL;
        a.latitude_deg=36;a.longitude_deg=136;a.position_error_m=1;
        a.validated=a.heading_valid=a.calibration_verified=1;a.quality=MX5_DR_VALID;
        adapter::Observation gps=pos(130,variant==1?2:1,3);
        gps.prediction_generation=uint32_t(a.context.generation)-(variant==2?1:0);
        // The verified fix was measured before the callback. Queue by original
        // time, without moving the anchor to receipt or rewriting the window.
        CHECK(p.enqueue_position(gps)==PIPELINE_OK);
        CHECK(p.enqueue_anchor(a,T(130))==PIPELINE_OK);
        adapter::Observation gap=pos(140,0,5);gap.prediction_generation=uint32_t(a.context.generation+1);
        CHECK(p.enqueue_position(gap)==PIPELINE_OK);
        const PipelineResult result=p.drain(T(200));
        const Diagnostic d=p.diagnostic(T(200));
        if(variant==2) {
            CHECK(result==PIPELINE_BAD_INPUT);CHECK(!d.snapshot.valid);CHECK(d.status.resets==1);
        } else {
            CHECK(result==PIPELINE_OK);CHECK(d.snapshot.valid);CHECK(d.status.resets==0);
            CHECK(d.snapshot.anchor_id==2);CHECK(d.snapshot.context.generation==gap.prediction_generation);
            CHECK(d.snapshot.frontier_ns==T(200));CHECK(d.snapshot.derived_utc_ns==a.utc_ns+90000000);
            CHECK(std::fabs(d.snapshot.accumulated_north_m-0.9)<1e-10);
            CHECK(std::fabs(d.snapshot.accumulated_east_m)<1e-10);
            CHECK(d.snapshot.latitude_deg>36&&d.snapshot.latitude_deg<36.00001);
        }
    }
}
static void exhausted_qualified(unsigned trigger) {
    Pipeline p;seed_qualified(p,UINT64_MAX-1);
    CHECK(p.context().generation==UINT64_MAX);
    const Diagnostic before=p.diagnostic(T(100));
    runtime::CoreBridgeQualification q=runtime::CoreBridgeQualification();
    q.expected_context=before.snapshot.context;q.now_mono_ns=T(110);q.max_snapshot_age_ns=150000000;
    q.limits_verified_until_mono_ns=T(110);q.duration_max_s=60;q.distance_max_m=1500;q.error_max_m=100;
    q.profile_verified=q.input_quality_verified=true;
    adapter::DrSnapshot out=adapter::DrSnapshot();
    CHECK(before.result==MX5_DR_OK&&before.snapshot.valid);
    // The core can calculate here, but no revoker can bind a generation above
    // UINT32_MAX. The bridge separately tests wire overflow.
    CHECK(p.qualified_snapshot(T(110),q,&out)==runtime::CORE_BRIDGE_UNQUALIFIED&&!out.ready);
    if(trigger==0) {
        CHECK(p.enqueue_yaw(evidence(2,2,110),0,2047,1,T(110),T(110))==PIPELINE_BAD_INPUT);
    } else {
        if(trigger==2) qualified_coverage(p); // Reach the position event's control call.
        // Without coverage, drain first revokes via control(DISABLE). A reset
        // there must not underflow the queue that drain is about to consume.
        CHECK(p.enqueue_position(pos(110,3,3))==PIPELINE_OK);
        const PipelineResult result=p.drain(T(110));
        std::printf("terminal qualified trigger=%u drain=%s\n",trigger,pipeline_result_name(result));std::fflush(stdout);
        CHECK(result==PIPELINE_BAD_INPUT);
    }
    const Diagnostic after=p.diagnostic(T(110));
    CHECK(!after.snapshot.valid&&!after.snapshot.model_valid&&after.result!=MX5_DR_OK);
    CHECK(after.snapshot.state==MX5_DR_UNSEEDED&&after.snapshot.frontier_ns==0);
    CHECK(!after.status.have_speed&&!after.status.have_yaw&&!after.status.have_reverse);
    CHECK(after.status.resets==before.status.resets+1&&after.status.rejected==before.status.rejected+1);
    CHECK(p.qualified_snapshot(T(110),q,&out)==runtime::CORE_BRIDGE_UNQUALIFIED&&!out.ready);
    CHECK(p.context().generation==UINT64_MAX);
    CHECK(p.drain(T(120))==PIPELINE_BAD_INPUT);
    mx5_dr_context fresh={1,1,1};p.reset(fresh);
    CHECK(p.context().generation==UINT64_MAX);
    CHECK(p.enqueue_speed(evidence(1,3,120),10)==PIPELINE_BAD_INPUT);
    CHECK(!p.diagnostic(T(120)).snapshot.valid);
    seed_qualified(p,1); // Explicit init also restores the independent qualified API.
}
static void exhausted_anchor_replacement() {
    Pipeline p;seed_qualified(p,UINT64_MAX-1);
    qualified_coverage(p);
    mx5_dr_anchor a=mx5_dr_anchor();a.context=p.context();a.anchor_id=2;a.position_seq=4;
    a.measured_ns=T(110);a.utc_ns=1700000000110000000ULL;
    a.latitude_deg=35;a.longitude_deg=135;a.position_error_m=1;
    a.validated=a.heading_valid=a.calibration_verified=1;a.quality=MX5_DR_VALID;
    CHECK(p.enqueue_anchor(a,T(110))==PIPELINE_OK);
    CHECK(p.drain(T(110))==PIPELINE_BAD_INPUT);
    const Diagnostic d=p.diagnostic(T(110));
    CHECK(!d.snapshot.valid&&d.snapshot.state==MX5_DR_UNSEEDED);
    CHECK(d.status.result==PIPELINE_BAD_INPUT&&d.status.resets==1);
    CHECK(d.status.core_result==MX5_DR_E_NO_SEED);
    CHECK(p.context().generation==UINT64_MAX);
}
static void exhausted_position_sequence() {
    Pipeline p;mx5_dr_context x={1,1,1};CHECK(p.init_qualified(mx5_dr_default_config(),x));
    mx5_dr_anchor a=mx5_dr_anchor();a.context=x;
    a.anchor_id=a.position_seq=UINT64_MAX;a.measured_ns=T(0);
    a.utc_ns=1700000000000000000ULL;a.latitude_deg=35;a.longitude_deg=135;
    a.position_error_m=1;a.validated=a.heading_valid=a.calibration_verified=1;
    a.quality=MX5_DR_VALID;
    CHECK(p.enqueue_anchor(a,T(0))==PIPELINE_OK);
    CHECK(p.drain(T(0))==PIPELINE_OK);
    CHECK(p.diagnostic(T(0)).snapshot.state==MX5_DR_READY);
    CHECK(p.enqueue_position(pos(100,3,2))==PIPELINE_OK);
    CHECK(p.drain(T(100))==PIPELINE_BAD_INPUT);
    const Diagnostic d=p.diagnostic(T(100));
    CHECK(d.status.result==PIPELINE_BAD_INPUT&&d.status.resets==1);
    CHECK(!d.snapshot.valid&&d.snapshot.state==MX5_DR_UNSEEDED&&d.snapshot.frontier_ns==0);
    CHECK(p.context().generation==2); // A nonterminal fault advances once; it does not wrap.
    CHECK(p.drain(T(100))==PIPELINE_OK);
    CHECK(p.status().resets==1);
    a.context=p.context();a.anchor_id=a.position_seq=1;a.measured_ns=T(200);
    a.utc_ns+=200000000ULL;
    CHECK(p.enqueue_anchor(a,T(200))==PIPELINE_OK);
    CHECK(p.drain(T(200))==PIPELINE_OK);
    CHECK(p.diagnostic(T(200)).snapshot.state==MX5_DR_READY);
}
int main(int argc,char** argv) {
    if(argc==2&&!std::strcmp(argv[1],"exhausted_model"))exhausted_model();
    else if(argc==2&&!std::strcmp(argv[1],"exhausted_qualified"))exhausted_qualified(0);
    else if(argc==2&&!std::strcmp(argv[1],"exhausted_revoke"))exhausted_qualified(1);
    else if(argc==2&&!std::strcmp(argv[1],"exhausted_position"))exhausted_qualified(2);
    else if(argc==2&&!std::strcmp(argv[1],"exhausted_anchor"))exhausted_anchor_replacement();
    else if(argc==2&&!std::strcmp(argv[1],"exhausted_sequence"))exhausted_position_sequence();
    else if(argc==2&&!std::strcmp(argv[1],"open_yaw_position"))model_position_waits_for_closed_yaw_window();
    else if(argc==2&&!std::strncmp(argv[1],"yaw_accumulator_",16))
        raw_yaw_accumulator_rejection(unsigned(std::atoi(argv[1]+16)));
    else { model_waits_for_closed_yaw_window();model_position_waits_for_closed_yaw_window();model_motion(); turning_reverse_stop(); rejection(); receipt_worker_and_reacquisition();
        for(unsigned i=0;i<6;++i)raw_yaw_accumulator_rejection(i);
        raw_yaw_accumulator_boundaries();
        rejected_gps_requires_new_pair();single_stopped_wheel_consistency();qualified();
        qualified_anchor_before_observed_return();qualified_revoker_failure_is_terminal();exhausted_model();
        exhausted_qualified(0);exhausted_qualified(1);exhausted_qualified(2);
        exhausted_anchor_replacement();exhausted_position_sequence(); }
    std::printf("navigation: %u checks passed\n",checks); return 0; }
