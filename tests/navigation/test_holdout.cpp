// Synthetic MODEL receipt-time holdout checks; no physical GPS accuracy claim.
#include "navigation/holdout.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
static void independent_and_exact() {
    Fixture a,b;unsigned compared=0,ended=0;bool turned=false,reversed=false,stopped=false;
    a.prime();b.prime();
    for(unsigned ms=0;ms<=2300;ms+=50) {
        if(ms==0||ms==100|| (ms>=150&&ms%150==0)) {
            A::Observation oa=gps(ms),ob=oa;
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
    independent_and_exact();aborts_and_reset();future_input_and_output_bound();calibration_survives_complete_only();
    wheel_training_outside_holdout_only();
    std::printf("MODEL GPS holdout: %u synthetic checks\n",checks);return 0;
}
