#include "navigation/pipeline.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
using namespace mx5;
using namespace mx5::navigation;
static unsigned checks;
#define CHECK(x) do { ++checks; if(!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::exit(1); } } while(0)
static uint64_t T(unsigned ms) { return 1000000000ULL+uint64_t(ms)*1000000ULL; }
static adapter::Observation fix(unsigned ms,double scale=1.0,int mode=1) {
    adapter::Observation o=adapter::Observation(); o.kind=adapter::Observation::POSITION;
    o.mono_ns=T(ms); o.position.mode=mode; o.position.utc_seconds=1700000000+ms/1000;
    o.position.latitude_deg=double(ms)*0.01*scale/111320;
    o.position.longitude_deg=135; o.position.heading_deg=0;
    o.position.velocity_kmh=36*scale; return o;
}
static void helper_feed(GpsWheel& g,unsigned ms,int condition=0) {
    if (ms) g.yaw(T(ms-100),T(ms),T(ms),condition==1?0.04:0);
    g.advance(T(ms));
    if (condition!=2 || ms==0)
        g.wheels(T(ms),T(ms)+(condition==3?300000000ULL:0),
            condition==4?(ms%200?11:10):10,condition==5?2:0);
    g.reverse(T(ms),T(ms),condition==6?1:0);
}
static void helper_train(GpsWheel& g,unsigned end,double scale=1.03,int condition=0) {
    for (unsigned ms=0;ms<=end;ms+=100) {
        helper_feed(g,ms,condition);
        if (ms%1000==0) g.fix(fix(ms,scale));
    }
}
static void learning() {
    GpsWheel g; g.configure(true,250000000ULL);
    helper_train(g,9000); CHECK(!g.status().candidate_ready); CHECK(g.status().segments==3);
    for(unsigned ms=9100;ms<=12000;ms+=100) {
        helper_feed(g,ms); if(ms%1000==0) CHECK(g.fix(fix(ms,1.03)));
    }
    CHECK(g.status().candidate_ready); CHECK(g.status().segments==4);
    CHECK(std::fabs(g.status().candidate_scale-1.03)<1e-9);
    CHECK(g.status().active_scale==1); g.apply_at_anchor(T(12000));
    CHECK(g.status().active_scale==1); g.apply_at_anchor(T(13000));
    CHECK(std::fabs(g.status().active_scale-1.03)<1e-9);
    CHECK(g.status().calibration_version==1); CHECK(!g.status().candidate_ready);
    g.restart_prediction(); CHECK(g.status().calibration_version==1);
    CHECK(std::fabs(g.status().active_scale-1.03)<1e-9); CHECK(!g.status().segments);
    g.reset(); CHECK(g.status().active_scale==1); CHECK(!g.status().calibration_version);
    // Never clip a larger discrepancy into a plausible scale.
    for(unsigned k=0;k<4;++k) {
        g.configure(true,250000000ULL); helper_train(g,20000,k==0?1.08:k==1?0.92:k==2?1.049:0.951);
        CHECK(g.status().candidate_ready==(k>=2));
        if(k>=2) CHECK(g.status().candidate_scale>=0.95&&g.status().candidate_scale<=1.05);
    }
    for(int condition=1;condition<=6;++condition) {
        g.configure(true,250000000ULL); helper_train(g,20000,1.03,condition);
        CHECK(!g.status().candidate_ready); CHECK(g.status().active_scale==1);
    }
    g.configure(true,250000000ULL); helper_train(g,12000); CHECK(g.status().candidate_ready);
    g.unavailable(); CHECK(!g.status().candidate_ready); CHECK(!g.status().segments);
    g.configure(true,250000000ULL); helper_train(g,12000); g.apply_at_anchor(T(43000));
    CHECK(g.status().active_scale==1); CHECK(!g.status().candidate_ready);
}
static RawEvent raw(SensorKind kind,unsigned ms,unsigned wheel=13600,int reverse=0,unsigned yaw=2047) {
    RawEvent r=RawEvent(); r.kind=kind; r.epoch=1; r.receive_seq=ms+1;
    r.received_ns=T(ms); r.count=1; r.reverse=reverse;
    for(unsigned j=0;j<4;++j) r.raw[j]=uint16_t(kind==YAW?yaw:wheel);
    return r;
}
static void feed(Pipeline& p,unsigned ms,int reverse=0,unsigned wheel=13600,unsigned yaw=2047) {
    CHECK(p.enqueue_raw(raw(WHEELS,ms,wheel))==PIPELINE_OK);
    CHECK(p.enqueue_raw(raw(REVERSE,ms,wheel,reverse))==PIPELINE_OK);
    PipelineResult r=p.enqueue_raw(raw(YAW,ms,wheel,reverse,yaw));
    CHECK(r==PIPELINE_OK||r==PIPELINE_WAITING);
}
static void init(Pipeline& p) {
    mx5_dr_context x={1,1,1}; CHECK(p.init_model(research_model_profile(),mx5_dr_default_config(),x,true,true));
}
static void drain(Pipeline& p,unsigned ms) {
    CHECK(p.drain(T(ms)-p.reorder_ns())==PIPELINE_OK);
}
static void gates() {
    for(unsigned bad=0;bad<7;++bad) {
        Pipeline p; init(p);
        for(unsigned ms=0;ms<=1100;ms+=100) {
            feed(p,ms,bad==6?1:0);
            if(ms==0||ms==1000) {
                adapter::Observation o=fix(ms);
                if(ms==1000) {
                    if(bad==1) o.position.latitude_deg+=100.0/111320;
                    if(bad==2) o.position.heading_deg=180;
                    if(bad==3) o.position.velocity_kmh=100;
                    if(bad==4) o.position.latitude_deg=0;
                    if(bad==5) o.position.latitude_deg=std::numeric_limits<double>::quiet_NaN();
                }
                CHECK(p.enqueue_position(o)==PIPELINE_OK);
            }
            drain(p,ms);
        }
        CHECK((p.diagnostic(T(1100)).snapshot.state==MX5_DR_READY)==(bad==0||bad==6));
        if(bad==6) CHECK(std::fabs(p.diagnostic(T(1100)).snapshot.body_heading_rad-3.14159265358979323846)<1e-8);
    }
    // A fast pair and stationary ambiguity cannot establish heading.
    GpsWheel g; g.configure(true,250000000ULL); helper_feed(g,0); CHECK(!g.fix(fix(0)));
    helper_feed(g,100); CHECK(!g.fix(fix(100))); CHECK(g.gate()==GPS_GATE_WAITING);
    g.wheels(T(100),T(100),0,0); CHECK(!g.fix(fix(100))); CHECK(g.gate()==GPS_GATE_SPEED);
    // Longitude wrapping at the date line is a short eastward displacement.
    g.configure(true,250000000ULL);
    for(unsigned ms=0;ms<=1000;ms+=100) {
        helper_feed(g,ms);
        if(ms==0||ms==1000) {
            adapter::Observation o=fix(ms); o.position.latitude_deg=0; o.position.heading_deg=90;
            o.position.longitude_deg=179.99996+double(ms)*0.01/111320;
            if(o.position.longitude_deg>180) o.position.longitude_deg-=360;
            CHECK(g.fix(o)==(ms==1000));
        }
    }
}
static void evidence_resets() {
    // No fresh four-wheel support, frozen UTC, or discontinuity may train or
    // retain READY. GPS clocks are still receipt-domain MODEL assumptions.
    for(unsigned condition=0;condition<6;++condition) {
        GpsWheel g; g.configure(true,250000000ULL);
        for(unsigned ms=0;ms<=3000;ms+=100) {
            helper_feed(g,ms);
            if(ms==0||ms==1000||ms==2000||ms==3000) {
                adapter::Observation o=fix(ms);
                if(condition==0) o.position.utc_seconds=1700000000;
                if(ms==3000) {
                    if(condition==1) o.position.utc_seconds-=10;
                    if(condition==2) o.position.utc_seconds+=10;
                    if(condition==3) g.wheels(T(ms),T(ms),10,3);
                    if(condition==4) g.reverse(T(ms),T(ms)+100000000ULL,0);
                    if(condition==5) g.wheels(T(ms),T(ms)+100000000ULL,10,0);
                }
                bool accepted=g.fix(o);
                if(ms==3000) CHECK(!accepted);
            }
        }
        CHECK(!g.status().candidate_ready); CHECK(!g.status().segments);
    }
    GpsWheel g; g.configure(true,250000000ULL); helper_train(g,12000);
    CHECK(g.status().candidate_ready); g.restart_prediction();
    CHECK(!g.status().candidate_ready); CHECK(!g.status().segments);
    helper_train(g,12000); CHECK(g.status().candidate_ready);
    helper_feed(g,12100,5); CHECK(!g.status().candidate_ready);
    // Training needs continuous raw source-event coverage, even if GPS speed
    // agrees with the most recently refreshed wheel endpoint.
    g.configure(true,250000000ULL);
    for(unsigned ms=0;ms<=20000;ms+=100) {
        if(ms%1000==0) helper_feed(g,ms);
        else { g.yaw(T(ms-100),T(ms),T(ms),0); g.advance(T(ms)); }
        if(ms%1000==0) g.fix(fix(ms,1.03));
    }
    CHECK(!g.status().candidate_ready); CHECK(!g.status().segments);
}
static void pipeline_learning_and_reacquisition() {
    Pipeline p; init(p);
    for(unsigned ms=0;ms<=13100;ms+=100) {
        feed(p,ms);
        if(ms%1000==0) CHECK(p.enqueue_position(fix(ms,1.03))==PIPELINE_OK);
        drain(p,ms);
        if(ms==12100) { CHECK(p.wheel_calibration().candidate_ready); CHECK(p.wheel_calibration().active_scale==1); }
    }
    CHECK(std::fabs(p.wheel_calibration().active_scale-1.03)<1e-9);
    CHECK(p.wheel_calibration().calibration_version==1);
    CHECK(p.enqueue_position(fix(13100,1.03,0))==PIPELINE_OK);
    for(unsigned ms=13200;ms<=16100;ms+=100) { feed(p,ms); drain(p,ms); }
    Diagnostic d=p.diagnostic(T(16100)); CHECK(d.result==MX5_DR_OK);
    CHECK(std::fabs(d.snapshot.accumulated_north_m-30.9)<0.01);
    CHECK(p.wheel_calibration().calibration_version==1); CHECK(!p.wheel_calibration().segments);
    // Reacquisition is checked against new GPS pairs, not old DR location.
    for(unsigned ms=16200;ms<=17300;ms+=100) {
        feed(p,ms);
        if(ms==16200||ms==17200) {
            adapter::Observation o=fix(ms,1.03); o.position.latitude_deg+=1;
            CHECK(p.enqueue_position(o)==PIPELINE_OK);
        }
        drain(p,ms);
    }
    CHECK(p.diagnostic(T(17300)).snapshot.state==MX5_DR_READY);
    CHECK(p.restart_model_prediction(p.context()));
    CHECK(p.wheel_calibration().calibration_version==1); CHECK(!p.wheel_calibration().segments);
    CHECK(std::fabs(p.wheel_calibration().active_scale-1.03)<1e-9);
    RawEvent changed=raw(WHEELS,17400); changed.epoch=2;
    CHECK(p.enqueue_raw(changed)==PIPELINE_SOURCE_RESET);
    CHECK(p.wheel_calibration().active_scale==1); CHECK(!p.wheel_calibration().calibration_version);
    mx5_dr_context x={1,1,1}; CHECK(p.init_qualified(mx5_dr_default_config(),x));
    CHECK(!p.wheel_calibration().enabled);
}
static void rejected_anchor_revokes() {
    Pipeline p; init(p);
    for(unsigned ms=0;ms<=2200;ms+=100) {
        feed(p,ms);
        if(ms==0||ms==1000||ms==2000) {
            adapter::Observation o=fix(ms); if(ms==2000) o.position.heading_deg=180;
            CHECK(p.enqueue_position(o)==PIPELINE_OK);
        }
        if(ms==2100) CHECK(p.enqueue_position(fix(ms,1,0))==PIPELINE_OK);
        drain(p,ms);
    }
    CHECK(!p.diagnostic(T(2200)).snapshot.model_valid);
}
static void fast_outlier_revokes() {
    for(unsigned scenario=0;scenario<2;++scenario) {
        Pipeline p; init(p);
        const unsigned outlier=scenario?1600:1100;
        for(unsigned ms=0;ms<=outlier+200;ms+=100) {
            feed(p,ms);
            if(ms==0||ms==1000||ms==outlier) {
                adapter::Observation o=fix(ms);
                if(ms==outlier) {
                    if(scenario) o.position.heading_deg=180; // 6m: bearing meaningful
                    else o.position.latitude_deg+=1000.0/111320;
                }
                CHECK(p.enqueue_position(o)==PIPELINE_OK);
            }
            if(ms==outlier+100) CHECK(p.enqueue_position(fix(ms,1,0))==PIPELINE_OK);
            drain(p,ms);
        }
        CHECK(!p.diagnostic(T(outlier+200)).snapshot.model_valid);
    }
}
static void asynchronous_windows() {
    // Realistic unequal periods, with GPS inside the completed yaw window.
    Pipeline p; init(p);
    for(unsigned ms=0;ms<=14200;ms+=50) {
        CHECK(p.enqueue_raw(raw(WHEELS,ms))==PIPELINE_OK);
        CHECK(p.enqueue_raw(raw(REVERSE,ms))==PIPELINE_OK);
        if(ms%100==0) {
            PipelineResult r=p.enqueue_raw(raw(YAW,ms));
            CHECK(r==PIPELINE_OK||r==PIPELINE_WAITING);
        }
        if(ms%1000==50) CHECK(p.enqueue_position(fix(ms,1.03))==PIPELINE_OK);
        drain(p,ms);
    }
    CHECK(p.wheel_calibration().calibration_version==1);
    CHECK(std::fabs(p.wheel_calibration().active_scale-1.03)<1e-9);
    // Candidate receipt can extend past its GPS endpoint. A later anchor that
    // still predates that receipt cannot apply it; the exact receipt can.
    GpsWheel g; g.configure(true,250000000ULL);
    for(unsigned ms=0;ms<=12050;ms+=50) {
        g.advance(T(ms));
        if(ms%100==0) g.yaw(T(ms),T(ms+100),T(ms+200),0);
        g.wheels(T(ms),T(ms),10,0); g.reverse(T(ms),T(ms),0);
        if(ms%1000==50) g.fix(fix(ms,1.03));
    }
    CHECK(g.status().candidate_ready); g.apply_at_anchor(T(12100));
    CHECK(g.status().active_scale==1); CHECK(g.status().candidate_ready);
    g.apply_at_anchor(T(12200)); CHECK(std::fabs(g.status().active_scale-1.03)<1e-9);
}
int main() {
    learning(); gates(); evidence_resets(); pipeline_learning_and_reacquisition(); rejected_anchor_revokes();
    fast_outlier_revokes(); asynchronous_windows();
    std::printf("MODEL GPS/wheel consistency and scale: %u checks (synthetic)\n",checks);
}
