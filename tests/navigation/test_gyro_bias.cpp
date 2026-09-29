#include "navigation/pipeline.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
using namespace mx5;
using namespace mx5::navigation;
static unsigned checks;
#define CHECK(x) do { ++checks; if(!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::exit(1); } } while(0)
static uint64_t T(unsigned ms) { return 1000000000ULL+uint64_t(ms)*1000000ULL; }
static RawEvent raw(SensorKind kind,unsigned ms,unsigned value=10000) {
    RawEvent r=RawEvent(); r.kind=kind;r.epoch=1;r.receive_seq=ms+1;r.received_ns=T(ms);
    r.count=2;for(unsigned j=0;j<4;++j)r.raw[j]=uint16_t(value);return r;
}
static adapter::Observation pos(unsigned ms,int mode) {
    adapter::Observation o=adapter::Observation();o.kind=adapter::Observation::POSITION;
    o.mono_ns=T(ms);o.position.mode=mode;o.position.utc_seconds=1700000000+ms/1000;
    o.position.latitude_deg=35;o.position.longitude_deg=135;o.position.heading_deg=0;
    o.position.velocity_kmh=36;return o;
}
static void feed(Pipeline& p,unsigned ms,unsigned wheel=10000,unsigned yaw=2067) {
    CHECK(p.enqueue_raw(raw(WHEELS,ms,wheel))==PIPELINE_OK);
    CHECK(p.enqueue_raw(raw(REVERSE,ms))==PIPELINE_OK);
    PipelineResult r=p.enqueue_raw(raw(YAW,ms,yaw*2));
    CHECK(r==PIPELINE_OK||r==PIPELINE_WAITING);
}
static void init(Pipeline& p,bool enabled=true) {
    mx5_dr_context x={1,1,1};CHECK(p.init_model(research_model_profile(),mx5_dr_default_config(),x,enabled));
}
static void train(Pipeline& p) {
    for(unsigned ms=0;ms<=3400;ms+=100) {
        feed(p,ms);CHECK(p.drain(T(ms)-p.reorder_ns())==PIPELINE_OK);
    }
}
static Diagnostic motion(bool enabled,unsigned yaw) {
    Pipeline p;init(p,enabled);train(p);
    CHECK(p.calibration().candidate_ready==enabled);
    CHECK(p.calibration().active_zero==2047);
    for(unsigned ms=3500;ms<=8700;ms+=100) {
        feed(p,ms,13600,ms<3700?2067:yaw);
        if(ms==3500||ms==3600)CHECK(p.enqueue_position(pos(ms,1))==PIPELINE_OK);
        if(ms==3700)CHECK(p.enqueue_position(pos(ms,0))==PIPELINE_OK);
        CHECK(p.drain(T(ms)-p.reorder_ns())==PIPELINE_OK);
    }
    CHECK(p.calibration().active_zero==(enabled?2067:2047));
    CHECK(p.calibration().calibration_version==(enabled?1u:0u));
    Diagnostic d=p.diagnostic(T(8700));CHECK(d.result==MX5_DR_OK);
    CHECK(d.snapshot.model_valid&&!d.snapshot.valid);return d;
}
static void drift_reduction() {
    Diagnostic fixed=motion(false,2067),cal=motion(true,2067);
    CHECK(std::fabs(cal.snapshot.accumulated_east_m)<1e-9);
    CHECK(std::fabs(fixed.snapshot.accumulated_east_m)>1.0);
    fixed=motion(false,2219);cal=motion(true,2219);
    const double expected=152*research_model_profile().yaw_rad_per_count*5.0;
    const double target=2*3.14159265358979323846+expected;
    CHECK(std::fabs(cal.snapshot.body_heading_rad-target)<1e-8);
    CHECK(std::fabs(fixed.snapshot.body_heading_rad-target)>0.06);
}
static void estimator_gates() {
    // Isolate evidence gates without GPS or DR output in this fixture.
    for(unsigned condition=0;condition<7;++condition) {
        GyroBias b;b.configure(true,2047,250000000ULL);
        for(unsigned ms=0;ms<=5000;ms+=100) {
            double wheels[4]={0,0,0,0};
            if(condition==0)for(unsigned j=0;j<4;++j)wheels[j]=0.2;
            if(condition==1)wheels[3]=0.19; // average < threshold, one wheel moving
            if(condition!=2||ms==0)b.wheels(T(ms),T(ms),false,wheels);
            unsigned sample=condition==3?(ms%200?2064:2070):2067;
            if(condition==4)sample=2140; // bounded nominal deviation
            if(ms)b.yaw(T(ms-100),T(ms),T(ms)+(condition==5?300000000ULL:0),false,sample);
            if(condition==6&&ms%1000==0)b.reset(); // unknown / audit evidence loss
        }
        CHECK(!b.status().candidate_ready);CHECK(b.status().active_zero==2047);
    }
    GyroBias b;b.configure(true,2047,250000000ULL);double zero[4]={0,0,0,0};
    for(unsigned ms=0;ms<=3200;ms+=100) {
        b.wheels(T(ms),T(ms),false,zero);if(ms)b.yaw(T(ms-100),T(ms),T(ms),false,2067);
    }
    CHECK(b.status().candidate_ready);CHECK(!b.apply_at_anchor(T(3100)));
    CHECK(!b.apply_at_anchor(T(34000)));CHECK(!b.status().candidate_ready);
    // A wheel receipt gap destroys a pending candidate, even if yaw continues.
    for(unsigned ms=35000;ms<=38200;ms+=100) {
        b.wheels(T(ms),T(ms),false,zero);if(ms>35000)b.yaw(T(ms-100),T(ms),T(ms),false,2067);
    }
    CHECK(b.status().candidate_ready);b.wheels(T(39000),T(39000),false,zero);
    CHECK(!b.status().candidate_ready);
}
static void freeze_reanchor_reset() {
    Pipeline p;init(p);train(p);
    for(unsigned ms=3500;ms<=7300;ms+=100) {
        // Learn a second stationary zero while GPS is withheld.
        feed(p,ms,ms<3800?13600:10000,ms<3800?2067:2077);
        if(ms==3500||ms==3600)CHECK(p.enqueue_position(pos(ms,1))==PIPELINE_OK);
        if(ms==3700)CHECK(p.enqueue_position(pos(ms,0))==PIPELINE_OK);
        CHECK(p.drain(T(ms)-p.reorder_ns())==PIPELINE_OK);
    }
    CHECK(p.calibration().active_zero==2067);CHECK(p.calibration().candidate_ready);
    CHECK(p.calibration().candidate_zero==2077);CHECK(p.calibration().calibration_version==1);
    Pipeline completed=p;
    CHECK(completed.restart_model_prediction(completed.context()));
    CHECK(completed.calibration().active_zero==2067);
    CHECK(completed.calibration().calibration_version==1);
    CHECK(!completed.calibration().candidate_ready);
    CHECK(!completed.diagnostic(T(7300)).snapshot.model_valid);
    // Prediction restart retains source, sequence, effective time and clock
    // identity guards, so old calibration cannot cross evidence discontinuity.
    for(unsigned bad_kind=0;bad_kind<4;++bad_kind) {
        Pipeline guarded=completed;RawEvent bad=raw(WHEELS,7400);
        PipelineResult expected=PIPELINE_BAD_INPUT;
        if(bad_kind==0) {bad.epoch=2;expected=PIPELINE_SOURCE_RESET;}
        if(bad_kind==1) {bad.received_ns=T(7200);expected=PIPELINE_CLOCK_RESET;}
        if(bad_kind==2)bad.receive_seq=7301;
        if(bad_kind==3) {bad.source_mono_ms=int64_t(T(7400)/1000000);expected=PIPELINE_CLOCK_RESET;}
        CHECK(guarded.enqueue_raw(bad)==expected);
        CHECK(guarded.calibration().active_zero==2047);
    }
    for(unsigned ms=7400;ms<=7700;ms+=100) {
        feed(p,ms,13600,2077);
        if(ms==7400||ms==7500)CHECK(p.enqueue_position(pos(ms,1))==PIPELINE_OK);
        if(ms==7600)CHECK(p.enqueue_position(pos(ms,0))==PIPELINE_OK);
        CHECK(p.drain(T(ms)-p.reorder_ns())==PIPELINE_OK);
    }
    CHECK(p.calibration().active_zero==2077);CHECK(p.calibration().calibration_version==2);
    CHECK(std::fabs(p.diagnostic(T(7700)).snapshot.body_heading_rad)<1e-9);
    RawEvent changed_clock=raw(WHEELS,7800);
    changed_clock.source_mono_ms=int64_t(T(7800)/1000000);
    CHECK(p.enqueue_raw(changed_clock)==PIPELINE_CLOCK_RESET);
    CHECK(p.calibration().active_zero==2047);
    CHECK(!p.diagnostic(T(7800)).snapshot.model_valid);
    train(p);
    RawEvent source=raw(WHEELS,3500);source.epoch=2;
    CHECK(p.enqueue_raw(source)==PIPELINE_SOURCE_RESET);
    CHECK(p.calibration().active_zero==2047);CHECK(!p.calibration().calibration_version);
    train(p);RawEvent bad=raw(YAW,3500);bad.count=0;
    CHECK(p.enqueue_raw(bad)==PIPELINE_BAD_INPUT);CHECK(!p.calibration().candidate_ready);
    train(p);mx5_dr_context x={2,2,2};p.reset(x);CHECK(!p.calibration().candidate_ready);
    train(p);RawEvent clock=raw(WHEELS,3500);clock.source_mono_ms=int64_t(T(3500)/1000000);
    CHECK(p.enqueue_raw(clock)==PIPELINE_CLOCK_RESET);CHECK(!p.calibration().candidate_ready);
    // Qualified path always exposes disabled MODEL calibration.
    CHECK(p.init_qualified(mx5_dr_default_config(),x));CHECK(!p.calibration().enabled);
    CHECK(!p.restart_model_prediction(x));CHECK(!p.calibration().enabled);
}
static void anchor_inside_received_window() {
    Pipeline p;init(p);train(p);
    feed(p,3500,13600);CHECK(p.enqueue_position(pos(3500,1))==PIPELINE_OK);
    CHECK(p.drain(T(3400))==PIPELINE_OK);
    feed(p,3600,13600);
    CHECK(p.enqueue_position(pos(3550,1))==PIPELINE_OK);
    CHECK(p.enqueue_position(pos(3560,0))==PIPELINE_OK);
    CHECK(p.drain(T(3500))==PIPELINE_OK);
    feed(p,3700,13600);CHECK(p.drain(T(3600))==PIPELINE_OK);
    Diagnostic d=p.diagnostic(T(3700));CHECK(d.result==MX5_DR_OK);
    CHECK(p.calibration().active_zero==2067);
    CHECK(std::fabs(d.snapshot.body_heading_rad)<1e-12);
    CHECK(std::fabs(d.snapshot.accumulated_east_m)<1e-12);
}
static void anchor_reverse_evidence() {
    for(unsigned scenario=0;scenario<4;++scenario) {
        Pipeline p;init(p);
        if(scenario!=3) {
            RawEvent r=raw(REVERSE,0);r.source_mono_ms=int64_t(T(0)/1000000);
            CHECK(p.enqueue_raw(r)==PIPELINE_OK);
        }
        for(unsigned ms=0;ms<=500;ms+=100) {
            CHECK(p.enqueue_raw(raw(WHEELS,ms,13600))==PIPELINE_OK);
            PipelineResult result=p.enqueue_raw(raw(YAW,ms,4094));
            CHECK(result==PIPELINE_OK||result==PIPELINE_WAITING);
            if(ms==400&&(scenario==1||scenario==2)) {
                RawEvent r=raw(REVERSE,ms);r.reverse=1;
                r.source_mono_ms=int64_t(T(ms)/1000000);
                if(scenario==1)r.received_ns=T(550); // future receipt cannot anchor
                CHECK(p.enqueue_raw(r)==PIPELINE_OK);
            }
            if(ms==400||ms==500)CHECK(p.enqueue_position(pos(ms,1))==PIPELINE_OK);
        }
        CHECK(p.drain(T(500))==PIPELINE_OK);
        if(scenario!=2)CHECK(p.diagnostic(T(500)).snapshot.state!=MX5_DR_READY);
        // A fresh reverse callback after a rejected anchor cannot resurrect it.
        RawEvent r=raw(REVERSE,500);r.reverse=1;
        r.source_mono_ms=int64_t(T(500)/1000000);
        CHECK(p.enqueue_raw(r)==PIPELINE_OK);
        CHECK(p.enqueue_position(pos(500,0))==PIPELINE_OK);
        CHECK(p.enqueue_raw(raw(WHEELS,600,13600))==PIPELINE_OK);
        CHECK(p.enqueue_raw(raw(YAW,600,4094))==PIPELINE_OK);
        CHECK(p.drain(T(600))==PIPELINE_OK);
        Diagnostic d=p.diagnostic(T(600));
        if(scenario==2) {
            CHECK(d.result==MX5_DR_OK&&d.snapshot.model_valid);
            CHECK(std::fabs(d.snapshot.accumulated_north_m-1.0)<1e-9);
            CHECK(std::fabs(d.snapshot.body_heading_rad-3.14159265358979323846)<1e-9);
        } else CHECK(!d.snapshot.model_valid);
    }
}
static void candidate_receipt_causality() {
    // A completed MODEL transport-time window may be received after the GPS
    // anchor it would otherwise calibrate. Both wheel and yaw evidence must
    // have arrived; declining that anchor must retain the previous calibration
    // and the pending candidate for a later causal anchor.
    const unsigned wheel_delay[] = {200,0,250};
    const unsigned yaw_delay[] = {0,200,100};
    for(unsigned scenario=0;scenario<3;++scenario) {
        GyroBias b;b.configure(true,2047,250000000ULL);
        double stopped[4]={0,0,0,0},moving[4]={1,1,1,1};
        for(unsigned ms=0;ms<=3200;ms+=100) {
            b.wheels(T(ms),T(ms),true,stopped);
            if(ms)b.yaw(T(ms-100),T(ms),T(ms),true,2050);
        }
        CHECK(b.apply_at_anchor(T(3300)));
        CHECK(b.status().active_zero==2050&&b.status().calibration_version==1);
        b.wheels(T(3300),T(3300),true,moving);
        b.yaw(T(3200),T(3300),T(3300),true,2050);
        for(unsigned ms=3400;ms<=6600;ms+=100) {
            b.wheels(T(ms),T(ms+wheel_delay[scenario]),true,stopped);
            b.yaw(T(ms-100),T(ms),T(ms+yaw_delay[scenario]),true,2067);
        }
        CHECK(b.status().candidate_ready);
        CHECK(b.status().candidate_zero==2067);
        CHECK(b.status().evidence_end_ns==T(6600));
        CHECK(!b.apply_at_anchor(T(6700)));
        CHECK(b.status().candidate_ready);
        CHECK(b.status().active_zero==2050&&b.status().calibration_version==1);
        const unsigned delay=wheel_delay[scenario]>yaw_delay[scenario]?
            wheel_delay[scenario]:yaw_delay[scenario];
        CHECK(b.apply_at_anchor(T(6600+delay)));
        CHECK(!b.status().candidate_ready);
        CHECK(b.status().active_zero==2067&&b.status().calibration_version==2);
    }
}
int main() {
    drift_reduction();estimator_gates();freeze_reanchor_reset();anchor_inside_received_window();anchor_reverse_evidence();
    candidate_receipt_causality();
    std::printf("MODEL stationary gyro bias: %u checks (synthetic)\n",checks);return 0;
}
