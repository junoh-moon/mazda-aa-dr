// Synthetic MODEL receipt-time holdout checks; no physical GPS accuracy claim.
#include "navigation/holdout.h"
#include "runtime/shadow_log.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
using namespace mx5;
namespace N=mx5::navigation;
namespace A=mx5::adapter;
static unsigned checks;
#define CHECK(x) do { ++checks; if(!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::exit(1); } } while(0)
static uint64_t T(int ms) { return uint64_t(1000000000LL+int64_t(ms)*1000000LL); }
static A::Observation gps(int ms) {
    A::Observation o=A::Observation();o.kind=A::Observation::POSITION;o.mono_ns=T(ms);
    o.position.mode=1;o.position.utc_seconds=1700000000ULL+(ms+1000)/1000;
    o.position.latitude_deg=35+0.01*ms/111320.0;o.position.longitude_deg=135;
    o.position.heading_deg=0;o.position.velocity_kmh=36;return o;
}
struct Fixture {
    N::GpsHoldout h;
    uint64_t seq;
    Fixture(uint64_t duration=2000000000ULL,uint64_t cooldown=500000000ULL) : seq(0) {
        N::HoldoutConfig c=N::default_holdout_config();c.duration_ns=duration;
        c.cooldown_ns=cooldown;
        mx5_dr_context x={1,1,1};
        mx5_dr_config core=mx5_dr_default_config();core.stop_hold_s=0.5; // short synthetic stop fixture
        CHECK(h.init_model(N::research_model_profile(),core,x,c));
    }
    void raw(int ms,unsigned wheel=13600,unsigned yaw=2047,int reverse=0,uint64_t epoch=1) {
        N::RawEvent r=N::RawEvent();r.epoch=epoch;r.received_ns=T(ms);
        r.kind=N::WHEELS;r.receive_seq=++seq;
        for(unsigned i=0;i<4;++i)r.raw[i]=uint16_t(wheel);
        CHECK(h.enqueue_raw(r)==N::PIPELINE_OK);
        r.kind=N::REVERSE;r.receive_seq=++seq;r.reverse=reverse;
        CHECK(h.enqueue_raw(r)==N::PIPELINE_OK);
        r.kind=N::YAW;r.receive_seq=++seq;r.raw[0]=uint16_t(yaw);r.count=1;
        N::PipelineResult result=h.enqueue_raw(r);
        CHECK(result==N::PIPELINE_OK||result==N::PIPELINE_WAITING);
    }
    void wheel_reverse(int ms) {
        N::RawEvent r=N::RawEvent();r.epoch=1;r.received_ns=T(ms);
        r.kind=N::WHEELS;r.receive_seq=++seq;
        for(unsigned i=0;i<4;++i)r.raw[i]=13600;
        CHECK(h.enqueue_raw(r)==N::PIPELINE_OK);
        r.kind=N::REVERSE;r.receive_seq=++seq;r.reverse=0;
        CHECK(h.enqueue_raw(r)==N::PIPELINE_OK);
    }
    void prime() {
        // Give the production GPS gate a full second/10m of moving evidence.
        // Keep later scenario times unchanged, with the anchor still at 100ms.
        for(int ms=-900;ms<0;ms+=100) {
            CHECK(h.enqueue_position(gps(ms))==N::PIPELINE_OK);
            raw(ms);h.drain(T(ms)-100000000ULL);
        }
    }
    void warm() {
        prime();
        CHECK(h.enqueue_position(gps(0))==N::PIPELINE_OK);raw(0);h.drain(T(0)-100000000ULL);
        CHECK(h.enqueue_position(gps(100))==N::PIPELINE_OK);raw(100);h.drain(T(0));
        raw(200);h.drain(T(100));
        CHECK(h.phase()==N::HOLDOUT_RUNNING);
        N::HoldoutResult r;CHECK(h.pop(&r));CHECK(r.event==N::HOLDOUT_BEGIN);
        CHECK(r.anchor_ns==T(100)&&r.reference_ns==T(100));CHECK(!h.pop(&r));
    }
};
static void reference_json(const N::HoldoutResult& result,bool present,
                           uint32_t call=0,uint32_t generation=0) {
    char line[2200];
    CHECK(runtime::format_shadow_holdout(line,sizeof line,T(10000),result));
    const char* names[]={"reference_call","reference_generation"};
    const uint32_t values[]={call,generation};
    for(unsigned i=0;i<2;++i) {
        char expected[80];
        if(present)::snprintf(expected,sizeof expected,"\"%s\":%u",names[i],values[i]);
        else ::snprintf(expected,sizeof expected,"\"%s\":null",names[i]);
        const char* field=std::strstr(line,expected);
        if(!field)std::fprintf(stderr,"Expected %s in actual holdout JSON: %s\n",expected,line);
        CHECK(field&&(field[std::strlen(expected)]==','||field[std::strlen(expected)]=='}'));
    }
}
static void reference_identity_survives_delayed_queue() {
    Fixture f;f.prime();
    CHECK(f.h.enqueue_position(gps(0))==N::PIPELINE_OK);f.raw(0);f.h.drain(T(-100));
    A::Observation anchor=gps(100);anchor.call_sequence=101;anchor.prediction_generation=7;
    CHECK(f.h.enqueue_position(anchor)==N::PIPELINE_OK);
    f.wheel_reverse(100);f.h.drain(T(100));
    N::HoldoutResult result;CHECK(!f.h.pop(&result));
    A::Observation first=gps(150);first.call_sequence=202;first.prediction_generation=8;
    A::Observation second=gps(200);second.call_sequence=303;second.prediction_generation=9;
    CHECK(f.h.enqueue_position(first)==N::PIPELINE_OK);
    CHECK(f.h.enqueue_position(second)==N::PIPELINE_OK);
    // Queued observations must own their original identity and values. The
    // latest callback and the caller's reused storage cannot retag the anchor.
    anchor=first=second=gps(900);
    anchor.call_sequence=first.call_sequence=second.call_sequence=999;
    anchor.prediction_generation=first.prediction_generation=second.prediction_generation=99;
    f.h.drain(T(150));CHECK(!f.h.pop(&result));
    f.raw(200);f.h.drain(T(200));
    CHECK(f.h.pop(&result)&&result.event==N::HOLDOUT_BEGIN);
    CHECK(result.reference_ns==T(100)&&result.reference.latitude_deg==gps(100).position.latitude_deg);
    reference_json(result,true,101,7);
    CHECK(f.h.pop(&result)&&result.event==N::HOLDOUT_COMPARED);
    CHECK(result.reference_ns==T(150)&&result.prediction_frontier_ns==T(150));
    CHECK(result.reference.latitude_deg==gps(150).position.latitude_deg);
    reference_json(result,true,202,8);
    CHECK(f.h.pop(&result)&&result.event==N::HOLDOUT_COMPARED);
    CHECK(result.reference_ns==T(200)&&result.prediction_frontier_ns==T(200));
    reference_json(result,true,303,9);CHECK(!f.h.pop(&result));
}
static void reference_identity_absence_and_overflow() {
    // A zero/wrapped call and a maximal generation are still actual IDs.
    Fixture skipped;A::Observation old=gps(100);
    old.call_sequence=0;old.prediction_generation=UINT32_MAX;
    CHECK(skipped.h.enqueue_position(old)==N::PIPELINE_OK);old=gps(900);
    skipped.h.drain(T(351));N::HoldoutResult result;
    CHECK(skipped.h.pop(&result)&&result.event==N::HOLDOUT_SKIPPED);
    reference_json(result,true,0,UINT32_MAX);CHECK(!skipped.h.pop(&result));
    Fixture reverse_ids;old=gps(100);old.call_sequence=UINT32_MAX;old.prediction_generation=0;
    CHECK(reverse_ids.h.enqueue_position(old)==N::PIPELINE_OK);reverse_ids.h.drain(T(351));
    CHECK(reverse_ids.h.pop(&result)&&result.event==N::HOLDOUT_SKIPPED);
    reference_json(result,true,UINT32_MAX,0);
    // Terminal results do not acquire a last-seen or triggering callback ID.
    Fixture complete(100000000ULL);complete.warm();complete.raw(300);complete.h.drain(T(200));
    CHECK(complete.h.pop(&result)&&result.event==N::HOLDOUT_END);reference_json(result,false);
    Fixture fault;fault.warm();old=gps(250);old.position.mode=0;
    old.call_sequence=444;old.prediction_generation=12;
    CHECK(fault.h.enqueue_position(old)==N::PIPELINE_NO_ANCHOR);
    CHECK(fault.h.pop(&result)&&result.event==N::HOLDOUT_ABORT);reference_json(result,false);
    const mx5_dr_context next={1,2,3};fault.h.reset(next);
    CHECK(fault.h.pop(&result)&&result.event==N::HOLDOUT_ABORT);reference_json(result,false);
    // A full result queue rewrites the final event to output_overflow. Preserve
    // the actual emit argument, which can be a skipped observation or null.
    for(unsigned with_observation=0;with_observation<2;++with_observation) {
        Fixture full;
        const unsigned count=N::GpsHoldout::RESULT_CAPACITY+with_observation;
        for(unsigned i=0;i<count;++i) {
            A::Observation o=gps(100+int(i));o.call_sequence=17+i;o.prediction_generation=31+i;
            CHECK(full.h.enqueue_position(o)==N::PIPELINE_OK);
            full.h.drain(T(351+int(i)));
        }
        if(!with_observation)full.h.reset(next);
        unsigned read=0;
        while(full.h.pop(&result)) {
            ++read;
            if(read<N::GpsHoldout::RESULT_CAPACITY)CHECK(result.event==N::HOLDOUT_SKIPPED);
            else {
                CHECK(result.event==N::HOLDOUT_ABORT&&result.reason==N::HOLDOUT_OUTPUT_OVERFLOW);
                CHECK(result.window_id==0&&result.anchor_ns==0&&result.prediction_frontier_ns==0);
                CHECK(result.reference_ns==(with_observation?T(100+int(count)-1):0));
                reference_json(result,with_observation!=0,17+count-1,31+count-1);
            }
        }
        CHECK(read==N::GpsHoldout::RESULT_CAPACITY);
    }
}
static void warmup_waits_for_delayed_yaw() {
    Fixture f;f.prime();
    CHECK(f.h.enqueue_position(gps(0))==N::PIPELINE_OK);
    f.raw(0);f.h.drain(T(-100));
    CHECK(f.h.enqueue_position(gps(100))==N::PIPELINE_OK);
    f.wheel_reverse(100);f.h.drain(T(100));
    N::HoldoutResult result;CHECK(!f.h.pop(&result));
    f.h.drain(T(150));CHECK(!f.h.pop(&result));
    f.raw(200);f.h.drain(T(200));
    CHECK(f.h.pop(&result));
    CHECK(result.event==N::HOLDOUT_BEGIN);
    CHECK(result.anchor_ns==T(100));
    CHECK(f.h.phase()==N::HOLDOUT_RUNNING);

    // The timeout belongs to the last yaw boundary, not to the later GPS
    // reference. A reference at 200 ms cannot defer a missing 0 ms window.
    Fixture missing;missing.raw(0);
    CHECK(missing.h.enqueue_position(gps(200))==N::PIPELINE_OK);
    missing.wheel_reverse(200);missing.h.drain(T(200));
    CHECK(!missing.h.pop(&result));
    missing.h.drain(T(250));CHECK(!missing.h.pop(&result));
    missing.h.drain(T(251));
    CHECK(missing.h.pop(&result));
    CHECK(result.event==N::HOLDOUT_ABORT&&result.reason==N::HOLDOUT_SOURCE_FAULT);

    Fixture delayed;delayed.raw(0);
    CHECK(delayed.h.enqueue_position(gps(100))==N::PIPELINE_OK);
    delayed.wheel_reverse(100);delayed.h.drain(T(100));
    delayed.raw(200);delayed.h.drain(T(351));
    CHECK(delayed.h.pop(&result));
    CHECK(result.event==N::HOLDOUT_ABORT&&result.reason==N::HOLDOUT_STALE_REFERENCE);

    // A worker may drain an older watermark after later raw receipt. Do not
    // turn that stale GPS into a retrospective holdout BEGIN.
    Fixture future;future.prime();
    CHECK(future.h.enqueue_position(gps(0))==N::PIPELINE_OK);
    future.raw(0);future.h.drain(T(-100));
    CHECK(future.h.enqueue_position(gps(100))==N::PIPELINE_OK);
    future.raw(100);future.raw(200);
    future.wheel_reverse(600);
    future.h.drain(T(100));
    CHECK(future.h.phase()==N::HOLDOUT_WARMUP);
    bool skipped_zero=false,skipped_hundred=false;
    while(future.h.pop(&result)) {
        CHECK(result.event==N::HOLDOUT_SKIPPED);
        if(result.reference_ns==T(0))skipped_zero=true;
        if(result.reference_ns==T(100))skipped_hundred=true;
    }
    CHECK(skipped_zero&&skipped_hundred);
}
static void stale_unsubmitted_reference_does_not_extend_cooldown() {
    Fixture warm;
    CHECK(warm.h.enqueue_position(gps(100))==N::PIPELINE_OK);
    warm.h.drain(T(351));
    CHECK(warm.h.phase()==N::HOLDOUT_WARMUP);
    N::HoldoutResult result;
    CHECK(warm.h.pop(&result));
    CHECK(result.event==N::HOLDOUT_SKIPPED&&result.reason==N::HOLDOUT_STALE_REFERENCE);
    CHECK(result.reference_ns==T(100)&&!warm.h.pop(&result));

    Fixture pending_limit;
    CHECK(pending_limit.h.enqueue_position(gps(100))==N::PIPELINE_OK);
    pending_limit.h.drain(T(350));
    CHECK(!pending_limit.h.pop(&result));
    pending_limit.h.drain(T(351));
    CHECK(pending_limit.h.pop(&result));
    CHECK(result.event==N::HOLDOUT_ABORT&&result.reason==N::HOLDOUT_STALE_REFERENCE);

    Fixture cooldown;
    A::Observation gap=gps(1000);gap.position.mode=0;
    CHECK(cooldown.h.enqueue_position(gap)==N::PIPELINE_NO_ANCHOR);
    CHECK(cooldown.h.pop(&result)&&result.event==N::HOLDOUT_ABORT);
    CHECK(cooldown.h.phase()==N::HOLDOUT_COOLDOWN);
    CHECK(cooldown.h.enqueue_position(gps(1100))==N::PIPELINE_OK);
    cooldown.h.drain(T(1500));
    CHECK(cooldown.h.phase()==N::HOLDOUT_COOLDOWN);
    CHECK(cooldown.h.pop(&result));
    CHECK(result.event==N::HOLDOUT_SKIPPED&&result.reason==N::HOLDOUT_STALE_REFERENCE);
    CHECK(result.reference_ns==T(1100)&&!cooldown.h.pop(&result));
    CHECK(cooldown.h.enqueue_position(gps(1600))==N::PIPELINE_OK);
    cooldown.h.drain(T(1600));
    CHECK(cooldown.h.phase()==N::HOLDOUT_WARMUP);
    CHECK(!cooldown.h.pop(&result));

    Fixture submitted;
    A::Observation submitted_gap=gps(1000);submitted_gap.position.mode=0;
    CHECK(submitted.h.enqueue_position(submitted_gap)==N::PIPELINE_NO_ANCHOR);
    CHECK(submitted.h.pop(&result)&&result.event==N::HOLDOUT_ABORT);
    submitted.raw(1000);
    CHECK(submitted.h.enqueue_position(gps(1100))==N::PIPELINE_OK);
    submitted.wheel_reverse(1100);submitted.h.drain(T(1100));
    CHECK(!submitted.h.pop(&result));
    submitted.raw(1200);submitted.h.drain(T(1360));
    CHECK(submitted.h.pop(&result));
    CHECK(result.event==N::HOLDOUT_ABORT&&result.reason==N::HOLDOUT_STALE_REFERENCE);
    CHECK(!submitted.h.pop(&result));
    CHECK(submitted.h.phase()==N::HOLDOUT_COOLDOWN);
    CHECK(submitted.h.enqueue_position(gps(1600))==N::PIPELINE_OK);
    submitted.h.drain(T(1600));
    CHECK(submitted.h.phase()==N::HOLDOUT_WARMUP);

    Fixture repeated_faults;
    A::Observation repeated_gap=gps(1000);repeated_gap.position.mode=0;
    CHECK(repeated_faults.h.enqueue_position(repeated_gap)==N::PIPELINE_NO_ANCHOR);
    CHECK(repeated_faults.h.pop(&result)&&result.event==N::HOLDOUT_ABORT);
    for(unsigned i=0;i<N::GpsHoldout::RESULT_CAPACITY+2;++i) {
        N::RawEvent bad=N::RawEvent();bad.kind=N::YAW;bad.epoch=1;
        bad.receive_seq=i+1;bad.received_ns=T(1001+int(i));bad.count=0;
        CHECK(repeated_faults.h.enqueue_raw(bad)==N::PIPELINE_BAD_INPUT);
    }
    CHECK(repeated_faults.h.pop(&result));
    CHECK(result.event==N::HOLDOUT_ABORT&&result.reason==N::HOLDOUT_SOURCE_FAULT);
    CHECK(result.window_id==0&&!repeated_faults.h.pop(&result));
    // Repeated cooldown faults must not silently postpone its original end.
    CHECK(repeated_faults.h.enqueue_position(gps(1600))==N::PIPELINE_OK);
    repeated_faults.h.drain(T(1600));
    CHECK(repeated_faults.h.phase()==N::HOLDOUT_WARMUP);

    Fixture completed(100000000ULL);
    completed.warm();completed.raw(300);completed.h.drain(T(200));
    CHECK(completed.h.pop(&result));
    CHECK(result.event==N::HOLDOUT_END&&result.reason==N::HOLDOUT_COMPLETE);
    CHECK(completed.h.phase()==N::HOLDOUT_COOLDOWN);
    A::Observation bad_fix=gps(210);bad_fix.position.mode=9;
    CHECK(completed.h.enqueue_position(bad_fix)==N::PIPELINE_BAD_INPUT);
    CHECK(completed.h.pop(&result));
    CHECK(result.event==N::HOLDOUT_ABORT&&result.reason==N::HOLDOUT_BAD_GPS);
    CHECK(result.window_id==0&&result.anchor_ns==0);
    bad_fix.mono_ns=T(220);
    CHECK(completed.h.enqueue_position(bad_fix)==N::PIPELINE_BAD_INPUT);
    CHECK(!completed.h.pop(&result));

    Fixture expired;
    A::Observation expired_gap=gps(1000);expired_gap.position.mode=0;
    CHECK(expired.h.enqueue_position(expired_gap)==N::PIPELINE_NO_ANCHOR);
    CHECK(expired.h.pop(&result)&&result.reason==N::HOLDOUT_REAL_GAP);
    N::RawEvent expired_bad=N::RawEvent();expired_bad.kind=N::YAW;expired_bad.epoch=1;
    expired_bad.receive_seq=1;expired_bad.received_ns=T(1001);expired_bad.count=0;
    CHECK(expired.h.enqueue_raw(expired_bad)==N::PIPELINE_BAD_INPUT);
    CHECK(expired.h.pop(&result)&&result.reason==N::HOLDOUT_SOURCE_FAULT);
    expired_bad.receive_seq=2;expired_bad.received_ns=T(1600);
    CHECK(expired.h.enqueue_raw(expired_bad)==N::PIPELINE_BAD_INPUT);
    CHECK(expired.h.pop(&result)&&result.reason==N::HOLDOUT_SOURCE_FAULT);
    CHECK(!expired.h.pop(&result));
    A::Observation before_deadline=gps(1800);before_deadline.position.velocity_kmh=0;
    CHECK(expired.h.enqueue_position(before_deadline)==N::PIPELINE_OK);
    expired.h.drain(T(1800));CHECK(expired.h.phase()==N::HOLDOUT_COOLDOWN);
    CHECK(expired.h.enqueue_position(gps(2200))==N::PIPELINE_OK);
    expired.h.drain(T(2200));CHECK(expired.h.phase()==N::HOLDOUT_WARMUP);

    Fixture boundary;
    A::Observation boundary_gap=gps(1000);boundary_gap.position.mode=0;
    CHECK(boundary.h.enqueue_position(boundary_gap)==N::PIPELINE_NO_ANCHOR);
    CHECK(boundary.h.pop(&result)&&result.reason==N::HOLDOUT_REAL_GAP);
    expired_bad.receive_seq=1;expired_bad.received_ns=T(1001);
    CHECK(boundary.h.enqueue_raw(expired_bad)==N::PIPELINE_BAD_INPUT);
    CHECK(boundary.h.pop(&result)&&result.reason==N::HOLDOUT_SOURCE_FAULT);
    const mx5_dr_context next_session={1,2,1};
    boundary.h.reset(next_session,N::HOLDOUT_BUS_RESET);
    CHECK(boundary.h.pop(&result));
    CHECK(result.event==N::HOLDOUT_ABORT&&result.reason==N::HOLDOUT_BUS_RESET);
    CHECK(!boundary.h.pop(&result)&&boundary.h.phase()==N::HOLDOUT_WARMUP);
}
static void independent_and_exact() {
    Fixture a,b;unsigned compared=0,ended=0;bool turned=false,reversed=false,stopped=false;
    a.prime();b.prime();
    for(unsigned ms=0;ms<=2300;ms+=50) {
        if(ms==0||ms==100|| (ms>=150&&ms%150==0)) {
            A::Observation oa=gps(ms),ob=oa;
            oa.call_sequence=ms+101;oa.prediction_generation=7;
            ob.call_sequence=ms+202;ob.prediction_generation=8;
            if(ms>=150) {
                ob.position.latitude_deg+=1.0/111320.0;
                ob.position.longitude_deg+=1.0/111320.0;
                ob.position.heading_deg=73;ob.position.velocity_kmh=18;
                if(ms>=1200)oa.position.velocity_kmh=ob.position.velocity_kmh=0;
            }
            CHECK(a.h.enqueue_position(oa)==b.h.enqueue_position(ob));
        }
        if(ms%100==0) {
            unsigned wheel=ms>=1000?10000:13600,yaw=ms>=400&&ms<700?1800:2047;
            int reverse=ms>=600?1:0;
            a.raw(ms,wheel,yaw,reverse);b.raw(ms,wheel,yaw,reverse);
        }
        a.h.drain(T(ms)-100000000ULL);b.h.drain(T(ms)-100000000ULL);
        N::HoldoutResult ra,rb;
        while(a.h.pop(&ra)) {
            CHECK(b.h.pop(&rb));CHECK(ra.event==rb.event);CHECK(ra.reason==rb.reason);
            CHECK(ra.event!=N::HOLDOUT_ABORT);
            if(ra.event==N::HOLDOUT_BEGIN||ra.event==N::HOLDOUT_COMPARED) {
                const unsigned reference_ms=unsigned((ra.reference_ns-T(0))/1000000ULL);
                reference_json(ra,true,reference_ms+101,7);
                reference_json(rb,true,reference_ms+202,8);
            } else {reference_json(ra,false);reference_json(rb,false);}
            if(ra.event==N::HOLDOUT_COMPARED) {
                ++compared;CHECK(ra.reference_ns==ra.prediction_frontier_ns);
                CHECK(ra.prediction_frontier_ns==rb.prediction_frontier_ns);
                CHECK(ra.prediction.latitude_deg==rb.prediction.latitude_deg);
                CHECK(ra.prediction.longitude_deg==rb.prediction.longitude_deg);
                CHECK(ra.prediction.body_heading_rad==rb.prediction.body_heading_rad);
                CHECK(ra.prediction.speed_mps==rb.prediction.speed_mps);
                CHECK(ra.applied_wheel_scale==rb.applied_wheel_scale);
                CHECK(ra.wheel_scale_version==rb.wheel_scale_version);
                CHECK(ra.prediction.model_valid&&!ra.prediction.valid);
                CHECK(ra.prediction.domain==MX5_DR_MODEL_DOMAIN);
                if(ra.reference_ns==T(150)) {
                    CHECK(std::fabs(ra.prediction.accumulated_north_m-0.5)<0.001);
                    CHECK(ra.position_error_m<0.01);CHECK(rb.position_error_m>1);
                }
                if(ra.reference_ns==T(450))turned=ra.prediction.body_heading_rad>0;
                if(ra.reference_ns==T(750))reversed=ra.prediction.travel_bearing_rad>3;
                if(ra.reference_ns==T(2100))stopped=ra.prediction.stopped;
            }
            if(ra.event==N::HOLDOUT_END)++ended;
        }
        CHECK(!b.h.pop(&rb));
    }
    CHECK(compared==14&&ended==1&&turned&&reversed&&stopped);
    CHECK(a.h.phase()==N::HOLDOUT_COOLDOWN);
}
static void expect_abort(Fixture& f,N::HoldoutReason reason) {
    N::HoldoutResult r;CHECK(f.h.pop(&r));CHECK(r.event==N::HOLDOUT_ABORT);
    CHECK(r.reason==reason);CHECK(f.h.phase()==N::HOLDOUT_COOLDOWN);
}
static void aborts_and_reset() {
    {Fixture f;f.warm();mx5_dr_context x={1,1,1};f.h.reset(x,N::HOLDOUT_CAPTURE_STOP);
     N::HoldoutResult r;CHECK(f.h.pop(&r));CHECK(r.event==N::HOLDOUT_ABORT);
     CHECK(r.reason==N::HOLDOUT_CAPTURE_STOP && r.anchor_ns==T(100));
     CHECK(!f.h.pop(&r));}
    {Fixture f;f.warm();A::Observation o=gps(250);o.position.mode=0;
     f.h.enqueue_position(o);expect_abort(f,N::HOLDOUT_REAL_GAP);}
    {Fixture f;f.warm();A::Observation o=gps(250);o.position.mode=3;
     f.h.enqueue_position(o);expect_abort(f,N::HOLDOUT_NATIVE);}
    {Fixture f;f.warm();A::Observation o=gps(250);o.position.latitude_deg=std::numeric_limits<double>::quiet_NaN();
     f.h.enqueue_position(o);expect_abort(f,N::HOLDOUT_BAD_GPS);}
    {Fixture f;f.warm();A::Observation o=gps(250);o.position.latitude_deg+=1;
     f.h.enqueue_position(o);expect_abort(f,N::HOLDOUT_BAD_GPS);}
    {Fixture f;f.warm();f.h.enqueue_position(gps(100));expect_abort(f,N::HOLDOUT_TIME_ORDER);}
    {Fixture f;f.warm();A::Observation o=gps(250);--o.position.utc_seconds;
     f.h.enqueue_position(o);expect_abort(f,N::HOLDOUT_BAD_GPS);}
    {Fixture f;f.warm();N::RawEvent r=N::RawEvent();r.kind=N::REVERSE;r.epoch=2;
     r.receive_seq=++f.seq;r.received_ns=T(250);
     CHECK(f.h.enqueue_raw(r)==N::PIPELINE_SOURCE_RESET);expect_abort(f,N::HOLDOUT_SOURCE_FAULT);}
    {Fixture f;f.warm();mx5_dr_context x={2,2,2};f.h.reset(x);
     N::HoldoutResult r;CHECK(f.h.pop(&r));CHECK(r.event==N::HOLDOUT_ABORT&&r.reason==N::HOLDOUT_AUDIT_RESET);
     CHECK(f.h.phase()==N::HOLDOUT_WARMUP);f.warm();}
    {Fixture f(10000000000ULL);f.warm();for(unsigned ms=300;ms<=2300;ms+=100) {f.raw(ms);f.h.drain(T(ms)-100000000ULL);}
     expect_abort(f,N::HOLDOUT_GPS_TIMEOUT);}
    {Fixture f;f.warm();f.h.drain(T(1000));expect_abort(f,N::HOLDOUT_SOURCE_FAULT);}
    {Fixture f;f.warm();for(unsigned ms=201;ms<=233;++ms)f.h.enqueue_position(gps(ms));
     expect_abort(f,N::HOLDOUT_REFERENCE_OVERFLOW);}
    // Waiting for a yaw window must not hide an eventual source outage.
    {Fixture f;f.warm();f.h.enqueue_position(gps(350));f.h.drain(T(350));
     N::HoldoutResult r;CHECK(!f.h.pop(&r));f.h.drain(T(1000));
     expect_abort(f,N::HOLDOUT_SOURCE_FAULT);}
    // A late reference must not compare against a later frontier.
    {Fixture f;f.warm();f.raw(300);f.h.drain(T(250));f.h.enqueue_position(gps(225));
     f.h.drain(T(250));expect_abort(f,N::HOLDOUT_SOURCE_FAULT);}
}
static void future_input_and_output_bound() {
    // A queued speed at 200ms cannot affect a prediction at 150ms. The yaw
    // window that covers 150ms is an explicit completed MODEL mean window.
    {Fixture f;f.prime();f.h.enqueue_position(gps(0));f.raw(0);f.h.drain(T(0)-100000000ULL);
     f.h.enqueue_position(gps(100));f.raw(100);f.h.drain(T(0));
     f.h.enqueue_position(gps(150));f.raw(200,17200);f.h.drain(T(150));
     N::HoldoutResult r;CHECK(f.h.pop(&r)&&r.event==N::HOLDOUT_BEGIN);
     CHECK(f.h.pop(&r)&&r.event==N::HOLDOUT_COMPARED);
     CHECK(r.prediction_frontier_ns==T(150));CHECK(r.prediction.speed_mps==10);
     CHECK(std::fabs(r.prediction.accumulated_north_m-0.5)<0.001);}
    {Fixture f(10000000000ULL);f.warm();
     for(unsigned ms=250;ms<=3600;ms+=50) {
         f.h.enqueue_position(gps(ms));if(ms%100==0)f.raw(ms);
         f.h.drain(T(ms)-100000000ULL);
     }
     N::HoldoutResult r;unsigned count=0,overflow=0;
     while(f.h.pop(&r)) {++count;if(r.event==N::HOLDOUT_ABORT&&r.reason==N::HOLDOUT_OUTPUT_OVERFLOW)++overflow;}
     CHECK(count<=N::GpsHoldout::RESULT_CAPACITY);CHECK(overflow==1);}
    {Fixture f(10000000000ULL);f.warm();
     for(unsigned ms=300;ms<=2200;ms+=100) {
         A::Observation o=gps(ms);o.position.utc_seconds=gps(100).position.utc_seconds;
         f.h.enqueue_position(o);f.raw(ms);f.h.drain(T(ms)-100000000ULL);
         N::HoldoutResult r;
         while(f.h.pop(&r))if(r.event==N::HOLDOUT_ABORT) {
             CHECK(r.reason==N::HOLDOUT_BAD_GPS);CHECK(ms==2100);return;
         }
     }
     CHECK(false);}
}
static void calibration_survives_complete_only() {
    Fixture f(500000000ULL);unsigned begins=0,ends=0,comparisons=0;
    // Learn while stationary, then keep moving through both windows and the
    // cooldown: no second stationary segment may hide an accidental reset.
    for(unsigned ms=0;ms<=7200;ms+=100) {
        if(ms>=3600)f.h.enqueue_position(gps(ms));
        f.raw(ms,ms<3600?10000:13600,2050);
        f.h.drain(T(ms)-100000000ULL);
        N::HoldoutResult r;
        while(f.h.pop(&r)) {
            CHECK(r.event!=N::HOLDOUT_ABORT);
            CHECK(r.applied_yaw_zero==2050&&r.calibration_version==1);
            if(r.event==N::HOLDOUT_BEGIN)++begins;
            if(r.event==N::HOLDOUT_END)++ends;
            if(r.event==N::HOLDOUT_COMPARED) {
                ++comparisons;CHECK(std::fabs(r.prediction.body_heading_rad)<1e-12);
            }
        }
    }
    CHECK(begins==2&&ends==2&&comparisons==10);
    // Audit resets must still discard the retained applied calibration.
    mx5_dr_context context={2,2,100};f.h.reset(context);
    N::HoldoutResult r;while(f.h.pop(&r)) {}
    for(unsigned ms=7300;ms<=8600;ms+=100) {
        f.h.enqueue_position(gps(ms));f.raw(ms,13600,2050);f.h.drain(T(ms)-100000000ULL);
        while(f.h.pop(&r)) {
            CHECK(r.applied_yaw_zero==2047&&r.calibration_version==0);
            if(r.event==N::HOLDOUT_BEGIN) return;
        }
    }
    CHECK(false);
}
static void cooldown_relearned_calibration_fault_is_recorded() {
    for(unsigned cause=0;cause<3;++cause) {
        Fixture f(500000000ULL,10000000000ULL);
        A::Observation bad=gps(1000);bad.position.mode=9;
        CHECK(f.h.enqueue_position(bad)==N::PIPELINE_BAD_INPUT);
        N::HoldoutResult result;
        CHECK(f.h.pop(&result)&&result.event==N::HOLDOUT_ABORT);
        CHECK(result.reason==N::HOLDOUT_BAD_GPS&&result.calibration_version==0);
        for(unsigned ms=1100;ms<=6500;ms+=100) {
            if(ms>=4700)CHECK(f.h.enqueue_position(gps(ms))==N::PIPELINE_OK);
            f.raw(ms,ms<4700?10000:13600,2050);
            f.h.drain(T(ms)-100000000ULL);
            CHECK(!f.h.pop(&result));
        }
        if(cause==0) {
            bad=gps(6600);bad.position.mode=9;
            CHECK(f.h.enqueue_position(bad)==N::PIPELINE_BAD_INPUT);
        } else if(cause==1) {
            N::RawEvent malformed=N::RawEvent();malformed.kind=N::YAW;
            malformed.epoch=1;malformed.receive_seq=++f.seq;
            malformed.received_ns=T(6600);malformed.count=0;
            CHECK(f.h.enqueue_raw(malformed)==N::PIPELINE_BAD_INPUT);
        } else {
            f.h.drain(T(7000));
        }
        bool aborted=false;
        while(f.h.pop(&result)) {
            if(result.event==N::HOLDOUT_SKIPPED)continue;
            CHECK(result.event==N::HOLDOUT_ABORT&&!aborted);
            CHECK(result.reason==(cause==0?N::HOLDOUT_BAD_GPS:N::HOLDOUT_SOURCE_FAULT));
            CHECK(result.calibration_version==1&&result.applied_yaw_zero==2050);
            aborted=true;
        }
        CHECK(aborted);
    }
    Fixture partial(500000000ULL,10000000000ULL);
    A::Observation bad=gps(1000);bad.position.mode=9;
    CHECK(partial.h.enqueue_position(bad)==N::PIPELINE_BAD_INPUT);
    N::HoldoutResult result;CHECK(partial.h.pop(&result));
    for(unsigned ms=1100;ms<=2000;ms+=100) {
        partial.raw(ms,10000,2050);partial.h.drain(T(ms)-100000000ULL);
        CHECK(!partial.h.pop(&result));
    }
    bad=gps(2100);bad.position.mode=9;
    CHECK(partial.h.enqueue_position(bad)==N::PIPELINE_BAD_INPUT);
    CHECK(partial.h.pop(&result)&&result.event==N::HOLDOUT_ABORT);
    CHECK(result.reason==N::HOLDOUT_BAD_GPS&&!partial.h.pop(&result));

    // A moving wheel-calibration attempt can be partway through its first
    // segment while both completed segments and gyro collection remain zero.
    Fixture wheel_partial(500000000ULL,10000000000ULL);
    bad=gps(1000);bad.position.mode=9;
    CHECK(wheel_partial.h.enqueue_position(bad)==N::PIPELINE_BAD_INPUT);
    CHECK(wheel_partial.h.pop(&result)&&result.event==N::HOLDOUT_ABORT);
    for(unsigned ms=1100;ms<=3200;ms+=100) {
        CHECK(wheel_partial.h.enqueue_position(gps(ms))==N::PIPELINE_OK);
        wheel_partial.raw(ms);wheel_partial.h.drain(T(ms)-100000000ULL);
        while(wheel_partial.h.pop(&result))CHECK(result.event!=N::HOLDOUT_ABORT);
    }
    N::RawEvent malformed=N::RawEvent();malformed.kind=N::YAW;
    malformed.epoch=1;malformed.receive_seq=++wheel_partial.seq;
    malformed.received_ns=T(3300);malformed.count=0;
    CHECK(wheel_partial.h.enqueue_raw(malformed)==N::PIPELINE_BAD_INPUT);
    bool wheel_abort=false;
    while(wheel_partial.h.pop(&result)) {
        if(result.event==N::HOLDOUT_SKIPPED)continue;
        CHECK(result.event==N::HOLDOUT_ABORT&&result.reason==N::HOLDOUT_SOURCE_FAULT);
        wheel_abort=true;
    }
    CHECK(wheel_abort);
}
static void cooldown_preserves_distinct_fault_reasons_and_new_references() {
    Fixture f(500000000ULL,500000000ULL);
    A::Observation gap=gps(1000);gap.position.mode=0;
    CHECK(f.h.enqueue_position(gap)==N::PIPELINE_NO_ANCHOR);
    N::HoldoutResult result;
    CHECK(f.h.pop(&result)&&result.event==N::HOLDOUT_ABORT);
    CHECK(result.reason==N::HOLDOUT_REAL_GAP&&!f.h.pop(&result));
    N::RawEvent malformed=N::RawEvent();malformed.kind=N::YAW;
    malformed.epoch=1;malformed.receive_seq=++f.seq;
    malformed.received_ns=T(1100);malformed.count=0;
    CHECK(f.h.enqueue_raw(malformed)==N::PIPELINE_BAD_INPUT);
    CHECK(f.h.pop(&result)&&result.event==N::HOLDOUT_ABORT);
    CHECK(result.reason==N::HOLDOUT_SOURCE_FAULT&&!f.h.pop(&result));
    A::Observation bad=gps(1200);bad.position.mode=9;
    CHECK(f.h.enqueue_position(bad)==N::PIPELINE_BAD_INPUT);
    CHECK(f.h.pop(&result)&&result.event==N::HOLDOUT_ABORT);
    CHECK(result.reason==N::HOLDOUT_BAD_GPS&&!f.h.pop(&result));
    bad=gps(1300);bad.position.mode=9;
    CHECK(f.h.enqueue_position(bad)==N::PIPELINE_BAD_INPUT);
    CHECK(!f.h.pop(&result)); // Identical fault with no new evidence coalesces.
    CHECK(f.h.enqueue_position(gps(1350))==N::PIPELINE_OK);
    bad=gps(1400);bad.position.mode=9;
    CHECK(f.h.enqueue_position(bad)==N::PIPELINE_BAD_INPUT);
    CHECK(f.h.pop(&result)&&result.event==N::HOLDOUT_ABORT);
    CHECK(result.reason==N::HOLDOUT_BAD_GPS&&!f.h.pop(&result));
    CHECK(f.h.enqueue_position(gps(1600))==N::PIPELINE_OK);
    f.h.drain(T(1600));
    CHECK(f.h.phase()==N::HOLDOUT_WARMUP);

    Fixture drained(500000000ULL,500000000ULL);
    bad=gps(1000);bad.position.mode=9;
    CHECK(drained.h.enqueue_position(bad)==N::PIPELINE_BAD_INPUT);
    CHECK(drained.h.pop(&result)&&result.event==N::HOLDOUT_ABORT);
    bad=gps(1050);bad.position.mode=9;
    CHECK(drained.h.enqueue_position(bad)==N::PIPELINE_BAD_INPUT);
    CHECK(drained.h.pop(&result)&&result.event==N::HOLDOUT_ABORT);
    CHECK(drained.h.enqueue_position(gps(1100))==N::PIPELINE_OK);
    drained.raw(1100);drained.h.drain(T(1100));
    CHECK(!drained.h.pop(&result));
    bad=gps(1200);bad.position.mode=9;
    CHECK(drained.h.enqueue_position(bad)==N::PIPELINE_BAD_INPUT);
    CHECK(drained.h.pop(&result)&&result.event==N::HOLDOUT_ABORT);
    CHECK(result.reason==N::HOLDOUT_BAD_GPS&&!drained.h.pop(&result));
}
static void wheel_training_outside_holdout_only() {
    Fixture a(2000000000ULL,30000000000ULL),b(2000000000ULL,30000000000ULL);
    a.warm();b.warm();
    unsigned learned_begins=0,learned_comparisons=0;
    double frozen_scale=1;uint64_t frozen_version=0;
    for(unsigned ms=300;ms<=37000;ms+=100) {
        A::Observation oa=gps(ms),ob;
        // Raw wheels say 10m/s; the GPS-visible straight segment says 10.2.
        oa.position.latitude_deg=35+0.0102*ms/111320.0;
        oa.position.velocity_kmh=36.72;ob=oa;
        if(b.h.phase()==N::HOLDOUT_RUNNING) {
            // Withheld coordinates/speed/heading cannot train the next gain.
            ob.position.latitude_deg+=1.0/111320.0;
            ob.position.longitude_deg+=1.0/111320.0;
            ob.position.velocity_kmh=20;ob.position.heading_deg=73;
        }
        CHECK(a.h.enqueue_position(oa)==b.h.enqueue_position(ob));
        a.raw(ms);b.raw(ms);
        a.h.drain(T(ms)-100000000ULL);b.h.drain(T(ms)-100000000ULL);
        N::HoldoutResult ra,rb;
        while(a.h.pop(&ra)) {
            CHECK(b.h.pop(&rb));CHECK(ra.event==rb.event);CHECK(ra.event!=N::HOLDOUT_ABORT);
            CHECK(ra.applied_wheel_scale==rb.applied_wheel_scale);
            CHECK(ra.wheel_scale_version==rb.wheel_scale_version);
            if(ra.event==N::HOLDOUT_BEGIN) {
                CHECK(ra.applied_wheel_scale>1.015&&ra.applied_wheel_scale<1.025);
                CHECK(ra.wheel_scale_version>0);
                frozen_scale=ra.applied_wheel_scale;frozen_version=ra.wheel_scale_version;
                ++learned_begins;
            }
            if(ra.event==N::HOLDOUT_COMPARED) {
                CHECK(ra.applied_wheel_scale==frozen_scale);
                CHECK(ra.wheel_scale_version==frozen_version);
                CHECK(ra.prediction.latitude_deg==rb.prediction.latitude_deg);
                CHECK(ra.prediction.longitude_deg==rb.prediction.longitude_deg);
                if(frozen_version)++learned_comparisons;
            }
        }
        CHECK(!b.h.pop(&rb));
    }
    CHECK(learned_begins==1&&learned_comparisons>0);
}
int main() {
    reference_identity_survives_delayed_queue();reference_identity_absence_and_overflow();
    warmup_waits_for_delayed_yaw();stale_unsubmitted_reference_does_not_extend_cooldown();
    independent_and_exact();aborts_and_reset();
    future_input_and_output_bound();calibration_survives_complete_only();
    cooldown_relearned_calibration_fault_is_recorded();
    cooldown_preserves_distinct_fault_reasons_and_new_references();
    wheel_training_outside_holdout_only();
    std::printf("MODEL GPS holdout: %u synthetic checks\n",checks);return 0;
}
