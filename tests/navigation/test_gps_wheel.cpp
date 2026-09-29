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
static void stationary_heading_continuity() {
    // A valid stopped GPS position has no new travel heading. It must not erase
    // the existing heading maintained by continuous sensors just before a gap.
    // Keep the original anchor/time budget; do not manufacture a stopped seed.
    Diagnostic preserved=Diagnostic();
    for(unsigned scenario=0;scenario<9;++scenario) {
        Pipeline p;init(p);uint64_t anchor_id=0;
        for(unsigned ms=0;ms<=5000;ms+=50) {
            const bool stopped=ms>=1500&&ms<3000;
            const unsigned wheel=stopped&&scenario!=3?10000:13600;
            const unsigned yaw=stopped&&scenario==4?2000:2047;
            feed(p,ms,0,wheel,yaw);
            if(ms==0||ms==1000)CHECK(p.enqueue_position(fix(ms))==PIPELINE_OK);
            if(scenario==5&&ms==1750) {
                RawEvent bad=raw(YAW,ms);++bad.receive_seq;bad.count=0;
                CHECK(p.enqueue_raw(bad)==PIPELINE_BAD_INPUT);
            }
            if((scenario==7||scenario==8)&&ms==1800)
                CHECK(p.enqueue_position(fix(ms,1,scenario==7?0:3))==PIPELINE_OK);
            if(ms==2000&&scenario!=6) {
                adapter::Observation o=fix(ms);o.position.latitude_deg=15.0/111320;
                o.position.velocity_kmh=0;
                o.position.heading_deg=std::numeric_limits<double>::quiet_NaN();
                if(scenario==1)o.position.latitude_deg+=0.01;
                if(scenario==2)o.position.utc_seconds=1700000000;
                CHECK(p.enqueue_position(o)==PIPELINE_OK);
            }
            if(ms==2500)CHECK(p.enqueue_position(fix(ms,1,0))==PIPELINE_OK);
            drain(p,ms);
            if(ms==1400)anchor_id=p.diagnostic(T(ms)).snapshot.anchor_id;
        }
        const Diagnostic d=p.diagnostic(T(5000));
        CHECK(bool(d.snapshot.model_valid)==(scenario==0||scenario==6));
        CHECK(!d.snapshot.valid);
        if(scenario==0||scenario==6) {
            CHECK(d.result==MX5_DR_OK&&d.snapshot.state==MX5_DR_ACTIVE);
            CHECK(anchor_id&&d.snapshot.anchor_id==anchor_id);
            CHECK(std::fabs(d.snapshot.elapsed_s-3.9)<1e-9);
            CHECK(std::fabs(d.snapshot.accumulated_north_m-24.0)<1e-8);
            CHECK(std::fabs(d.snapshot.body_heading_rad)<1e-9);
            if(!scenario)preserved=d;
            else {
                // Keeping a stationary observation changes none of the prior
                // anchor's position, heading, uncertainty, or lifetime budget.
                CHECK(d.snapshot.anchor_id==preserved.snapshot.anchor_id);
                CHECK(d.snapshot.frontier_ns==preserved.snapshot.frontier_ns);
                CHECK(d.snapshot.error_budget_m==preserved.snapshot.error_budget_m);
                CHECK(d.snapshot.heading_budget_rad==preserved.snapshot.heading_budget_rad);
            }
        }
    }
    Pipeline unseeded;init(unseeded);
    for(unsigned ms=0;ms<=3500;ms+=50) {
        feed(unseeded,ms,0,ms<3000?10000:13600);
        if(ms==0||ms==1000||ms==2000) {
            adapter::Observation o=fix(ms);o.position.latitude_deg=0;
            o.position.velocity_kmh=0;
            o.position.heading_deg=std::numeric_limits<double>::quiet_NaN();
            CHECK(unseeded.enqueue_position(o)==PIPELINE_OK);
        }
        if(ms==2500)CHECK(unseeded.enqueue_position(fix(ms,1,0))==PIPELINE_OK);
        drain(unseeded,ms);
    }
    CHECK(!unseeded.diagnostic(T(3500)).snapshot.model_valid);
    // Repeated stationary fixes neither apply a completed gyro candidate nor
    // extend the original 60-second anchor lifetime.
    Pipeline bounded;init(bounded);uint64_t anchor_id=0;
    for(unsigned ms=0;ms<=64000;ms+=50) {
        feed(bounded,ms,0,ms>=1500&&ms<63000?10000:13600);
        if(ms%1000==0&&ms<=62000) {
            adapter::Observation o=fix(ms);
            if(ms>=2000) {
                o.position.latitude_deg=15.0/111320;o.position.velocity_kmh=0;
                o.position.heading_deg=std::numeric_limits<double>::quiet_NaN();
            }
            CHECK(bounded.enqueue_position(o)==PIPELINE_OK);
        }
        if(ms==62500)CHECK(bounded.enqueue_position(fix(ms,1,0))==PIPELINE_OK);
        PipelineResult r=bounded.drain(T(ms)-bounded.reorder_ns());
        CHECK(r==PIPELINE_OK||r==PIPELINE_CORE_REJECTED);
        if(ms==1400)anchor_id=bounded.diagnostic(T(ms)).snapshot.anchor_id;
        if(ms==60150) {
            const Diagnostic d=bounded.diagnostic(T(ms));
            CHECK(d.snapshot.state==MX5_DR_READY&&d.snapshot.anchor_id==anchor_id);
            CHECK(d.snapshot.elapsed_s>59.0);
            CHECK(bounded.calibration().candidate_ready);
            CHECK(bounded.calibration().active_zero==2047);
            CHECK(!bounded.calibration().calibration_version);
        }
    }
    CHECK(!bounded.diagnostic(T(64000)).snapshot.model_valid);
    CHECK(!bounded.calibration().calibration_version);
}
static void stationary_sensor_boundaries() {
    bool all_preserved=true;
    for(unsigned scenario=0;scenario<3;++scenario) {
        Pipeline p;init(p);uint64_t anchor_id=0;
        for(unsigned ms=0;ms<=3500;ms+=50) {
            const bool stopped=ms>=1500&&ms<2050;
            RawEvent w=raw(WHEELS,ms,stopped?10000:13600);
            // Every wheel satisfies the same <=0.05 m/s stationary condition
            // as GyroBias, even though average + spread would exceed it.
            if(scenario==2&&stopped)w.raw[0]=10017;
            CHECK(p.enqueue_raw(w)==PIPELINE_OK);
            CHECK(p.enqueue_raw(raw(REVERSE,ms))==PIPELINE_OK);
            // This turn window starts at the stopped GPS fix, after the
            // heading being preserved. It must not judge the preceding stop.
            RawEvent y=raw(YAW,ms,13600,0,scenario==1&&ms==2050?2000:2047);
            const PipelineResult r=p.enqueue_raw(y);
            CHECK(r==PIPELINE_OK||r==PIPELINE_WAITING);
            if(ms==0||ms==1000)CHECK(p.enqueue_position(fix(ms))==PIPELINE_OK);
            if(ms==2000) {
                adapter::Observation o=fix(ms);o.position.latitude_deg=15.0/111320;
                o.position.velocity_kmh=0;
                o.position.heading_deg=std::numeric_limits<double>::quiet_NaN();
                CHECK(p.enqueue_position(o)==PIPELINE_OK);
            }
            if(ms==2100)CHECK(p.enqueue_position(fix(ms,1,0))==PIPELINE_OK);
            drain(p,ms);
            if(ms==1400)anchor_id=p.diagnostic(T(ms)).snapshot.anchor_id;
        }
        const Diagnostic d=p.diagnostic(T(3500));
        if(!d.snapshot.model_valid) {
            std::fprintf(stderr,"stationary sensor boundary %u lost its original anchor\n",scenario);
            all_preserved=false;
        } else {
            CHECK(d.snapshot.anchor_id==anchor_id&&!d.snapshot.valid);
            CHECK(std::fabs(d.snapshot.elapsed_s-2.4)<1e-9);
        }
    }
    CHECK(all_preserved);
}
static void stationary_receipt_boundaries() {
    // Same-time endpoint events can arrive after GPS. Retain heading from the
    // original causal evidence, even when the newest completed interval used
    // more recent transport samples that were received after that GPS fix.
    const unsigned delays[][3]={{0,0,0},{50,50,50},{50,0,0},{0,50,0},{0,0,50},{75,75,75}};
    for(unsigned timing=0;timing<6;++timing)
    for(unsigned motion=0;motion<4;++motion)
    for(unsigned with_fix=0;with_fix<2;++with_fix) {
        Pipeline p;init(p);uint64_t anchor_id=0;
        for(unsigned ms=0;ms<=4000;ms+=50) {
            for(unsigned j=0;j<3;++j) {
                if(j==2&&ms%100)continue;
                // Introduce latency without moving the producer clock back.
                const unsigned delay=ms<1500?0:ms<1550&&delays[timing][j]>50?50:delays[timing][j];
                const unsigned measured=ms-delay;
                const bool stopped=motion!=1&&measured>=(motion==2?2000:1500)&&measured<2500;
                RawEvent r=raw(j==0?WHEELS:j==1?REVERSE:YAW,measured,stopped?10000:13600);
                if(j==0&&stopped&&motion==3)r.raw[0]=10072; // One wheel moves at 0.2 m/s.
                r.source_mono_ms=int64_t(T(measured)/1000000);
                r.received_ns=T(ms);r.receive_seq=ms+1;
                const PipelineResult result=p.enqueue_raw(r);
                if(result!=PIPELINE_OK&&result!=PIPELINE_WAITING)
                    std::fprintf(stderr,"stationary receipt enqueue timing=%u motion=%u fix=%u ms=%u kind=%u result=%s\n",
                        timing,motion,with_fix,ms,unsigned(r.kind),pipeline_result_name(result));
                CHECK(result==PIPELINE_OK||result==PIPELINE_WAITING);
            }
            if(ms==0||ms==1000||ms==3000||(ms==2000&&with_fix)) {
                adapter::Observation o=fix(ms);
                if(ms==2000) {
                    o.position.latitude_deg=(motion==1||motion==2?20.0:15.0)/111320;
                    o.position.velocity_kmh=0;
                    o.position.heading_deg=std::numeric_limits<double>::quiet_NaN();
                }
                if(ms==3000)o.position.mode=0;
                CHECK(p.enqueue_position(o)==PIPELINE_OK);
            }
            const PipelineResult r=p.drain(T(ms)-p.reorder_ns());
            CHECK(r==PIPELINE_OK||r==PIPELINE_WAITING);
            if(ms==1400)anchor_id=p.diagnostic(T(ms)).snapshot.anchor_id;
        }
        const bool preserved=!with_fix||motion==0||(motion==2&&!delays[timing][0]);
        const Diagnostic d=p.diagnostic(T(4000));
        if(bool(d.snapshot.model_valid)!=preserved)
            std::fprintf(stderr,"stationary receipt timing=%u motion=%u fix=%u expected=%u actual=%u\n",
                timing,motion,with_fix,unsigned(preserved),unsigned(d.snapshot.model_valid));
        CHECK(bool(d.snapshot.model_valid)==preserved);
        CHECK(!d.snapshot.valid&&!p.status().resets);
        if(preserved) {
            CHECK(d.snapshot.anchor_id==anchor_id&&anchor_id);
            CHECK(std::fabs(d.snapshot.elapsed_s-2.9)<1e-9);
        }
    }
}
static void stationary_history_phases() {
    // GPS can fall between asynchronous sensor events. Keeping that stopped
    // observation must leave the same prediction as omitting it, including a
    // causal forward-to-reverse change while the vehicle is stationary.
    for(unsigned reverse=0;reverse<2;++reverse)
    for(unsigned phase=0;phase<4;++phase) {
        Diagnostic control=Diagnostic();
        for(unsigned with_fix=0;with_fix<2;++with_fix) {
            Pipeline p;init(p);
            for(unsigned ms=0;ms<=4000;ms+=25) {
                if(ms%50==0) {
                    const unsigned measured=ms>=1500?ms-50:ms;
                    RawEvent events[3]={raw(WHEELS,measured,measured>=1500&&measured<2500?10000:13600),
                        raw(REVERSE,measured,13600,reverse&&measured>=1750?1:0),raw(YAW,measured)};
                    for(unsigned j=0;j<3;++j) {
                        if(j==2&&ms%100)continue;
                        events[j].source_mono_ms=int64_t(T(measured)/1000000);
                        events[j].received_ns=T(ms);events[j].receive_seq=ms+1;
                        const PipelineResult r=p.enqueue_raw(events[j]);
                        if(r!=PIPELINE_OK&&r!=PIPELINE_WAITING)
                            std::fprintf(stderr,"stationary history phase=%u reverse=%u fix=%u ms=%u kind=%u result=%s\n",
                                phase,reverse,with_fix,ms,unsigned(events[j].kind),pipeline_result_name(r));
                        CHECK(r==PIPELINE_OK||r==PIPELINE_WAITING);
                    }
                }
                if(ms==0||ms==1000||ms==3000||(with_fix&&ms==2000+phase*25)) {
                    adapter::Observation o=fix(ms);
                    if(ms>=2000&&ms<3000) {
                        o.position.latitude_deg=15.0/111320;o.position.velocity_kmh=0;
                        o.position.heading_deg=std::numeric_limits<double>::quiet_NaN();
                    }
                    if(ms==3000)o.position.mode=0;
                    CHECK(p.enqueue_position(o)==PIPELINE_OK);
                }
                const PipelineResult r=p.drain(T(ms)-p.reorder_ns());
                CHECK(r==PIPELINE_OK||r==PIPELINE_WAITING);
            }
            const Diagnostic d=p.diagnostic(T(4000));
            CHECK(d.snapshot.model_valid&&!d.snapshot.valid);
            CHECK(std::fabs(d.snapshot.body_heading_rad)<1e-9);
            CHECK(reverse?d.snapshot.accumulated_north_m<0:d.snapshot.accumulated_north_m>0);
            if(!with_fix)control=d;
            else {
                CHECK(d.snapshot.anchor_id==control.snapshot.anchor_id);
                CHECK(d.snapshot.frontier_ns==control.snapshot.frontier_ns);
                CHECK(d.snapshot.elapsed_s==control.snapshot.elapsed_s);
                CHECK(d.snapshot.error_budget_m==control.snapshot.error_budget_m);
                CHECK(d.snapshot.accumulated_north_m==control.snapshot.accumulated_north_m);
            }
        }
    }
}
static void stationary_history_capacity() {
    // A bounded history must decline preservation when all causal records have
    // been evicted. Twice the wheel cadence exhausts 64 records here; it must
    // not promote a remaining future receipt or invent a fresh stopped sample.
    for(unsigned period=1;period<=2;++period)
    for(unsigned with_fix=0;with_fix<2;++with_fix) {
        Pipeline p;init(p);
        for(unsigned ms=0;ms<=4000;++ms) {
            if(ms%period==0) {
                const unsigned delay=ms<1500?0:ms<1574?ms-1499:75;
                const unsigned measured=ms-delay;
                RawEvent w=raw(WHEELS,measured,measured>=1500&&measured<2500?10000:13600);
                w.source_mono_ms=int64_t(T(measured)/1000000);
                w.received_ns=T(ms);w.receive_seq=ms+1;
                CHECK(p.enqueue_raw(w)==PIPELINE_OK);
            }
            if(ms%50==0) {
                RawEvent r=raw(REVERSE,ms),y=raw(YAW,ms);
                r.source_mono_ms=y.source_mono_ms=int64_t(T(ms)/1000000);
                CHECK(p.enqueue_raw(r)==PIPELINE_OK);
                const PipelineResult result=p.enqueue_raw(y);
                CHECK(result==PIPELINE_OK||result==PIPELINE_WAITING);
            }
            if(ms==0||ms==1000||ms==3000||(with_fix&&ms==2000)) {
                adapter::Observation o=fix(ms);
                if(ms==2000) {
                    o.position.latitude_deg=15.0/111320;o.position.velocity_kmh=0;
                    o.position.heading_deg=std::numeric_limits<double>::quiet_NaN();
                }
                if(ms==3000)o.position.mode=0;
                CHECK(p.enqueue_position(o)==PIPELINE_OK);
            }
            const PipelineResult r=p.drain(T(ms)-p.reorder_ns());
            CHECK(r==PIPELINE_OK||r==PIPELINE_WAITING);
        }
        const Diagnostic d=p.diagnostic(T(4000));
        CHECK(bool(d.snapshot.model_valid)==(!with_fix||period==2));
        CHECK(!d.snapshot.valid&&!p.status().resets);
    }
}
static void stationary_history_resets() {
    for(unsigned reset=0;reset<5;++reset) {
        Pipeline p;init(p);
        for(unsigned ms=0;ms<=4000;ms+=50) {
            CHECK(p.enqueue_raw(raw(WHEELS,ms,ms>=1500&&ms<2500?10000:13600))==PIPELINE_OK);
            if(reset!=4||ms<=1500||ms>=2100)CHECK(p.enqueue_raw(raw(REVERSE,ms))==PIPELINE_OK);
            const PipelineResult y=p.enqueue_raw(raw(YAW,ms));
            CHECK(y==PIPELINE_OK||y==PIPELINE_WAITING);
            if(ms==1800&&(reset==1||reset==2)) {
                mx5_dr_context x=p.context();++x.generation;
                if(reset==1)p.reset(x);
                else CHECK(p.restart_model_prediction(x));
            }
            if(ms==1800&&reset==3) {
                RawEvent changed=raw(WHEELS,ms);changed.epoch=2;
                CHECK(p.enqueue_raw(changed)==PIPELINE_SOURCE_RESET);
            }
            if(ms==0||ms==1000||ms==2000||ms==3000) {
                adapter::Observation o=fix(ms);
                if(ms==2000) {
                    o.position.latitude_deg=15.0/111320;o.position.velocity_kmh=0;
                    o.position.heading_deg=std::numeric_limits<double>::quiet_NaN();
                }
                if(ms==3000)o.position.mode=0;
                CHECK(p.enqueue_position(o)==PIPELINE_OK);
            }
            const PipelineResult r=p.drain(T(ms)-p.reorder_ns());
            CHECK(r==PIPELINE_OK||r==PIPELINE_WAITING||r==PIPELINE_CORE_REJECTED||r==PIPELINE_MISSING_SENSOR);
        }
        const Diagnostic d=p.diagnostic(T(4000));
        CHECK(bool(d.snapshot.model_valid)==(reset==0));CHECK(!d.snapshot.valid);
    }
}
static void stationary_pending_window() {
    for(unsigned scenario=0;scenario<7;++scenario) {
        Pipeline p;init(p);uint64_t initial_generation=0,waiting_events=0;
        const unsigned end=scenario==0?3200:scenario==1?4200:scenario<5?2200:2175;
        for(unsigned ms=0;ms<=end;ms+=25) {
            if(ms%50==0) {
                // Recovery supplies causal endpoints for the unchanged moving
                // GPS seed gates; the transport clock domain stays the same.
                const unsigned measured=ms>=1500&&!(scenario==1&&ms>=2500)?ms-50:ms;
                RawEvent events[3]={raw(WHEELS,measured,measured>=1500&&measured<2500?10000:13600),
                    raw(REVERSE,measured),raw(YAW,measured)};
                for(unsigned j=0;j<3;++j) {
                    if(j==2&&(ms%100||(scenario==1&&ms>2100&&ms<2500)||
                       (scenario>=2&&scenario<=4&&ms==2200)))continue;
                    events[j].source_mono_ms=int64_t(T(measured)/1000000);
                    events[j].received_ns=T(ms);events[j].receive_seq=ms+1;
                    const PipelineResult r=p.enqueue_raw(events[j]);
                    CHECK(r==PIPELINE_OK||r==PIPELINE_WAITING);
                }
            }
            if(ms==0||ms==1000||ms==2075||(scenario==1&&(ms==3000||ms==4000))) {
                adapter::Observation o=fix(ms);
                if(ms==2075) {
                    o.position.latitude_deg=15.0/111320;o.position.velocity_kmh=0;
                    o.position.heading_deg=std::numeric_limits<double>::quiet_NaN();
                }
                CHECK(p.enqueue_position(o)==PIPELINE_OK);
            }
            if(ms==2100&&scenario>=2&&scenario<=4) {
                adapter::Observation o=fix(ms,1,scenario==3?3:1);
                if(scenario==4)o.position.latitude_deg=std::numeric_limits<double>::quiet_NaN();
                CHECK(p.enqueue_position(o)==PIPELINE_OK);
            }
            if((ms==1800&&scenario>=5)||ms==2300||(scenario==1&&ms==4100))
                CHECK(p.enqueue_position(fix(ms,1,ms==1800&&scenario==6?3:0))==PIPELINE_OK);
            const PipelineResult r=p.drain(T(ms)-p.reorder_ns());
            CHECK(r==PIPELINE_OK||r==PIPELINE_WAITING||r==PIPELINE_MISSING_SENSOR);
            if(ms==1400)initial_generation=p.context().generation;
            if(ms==2175) {
                const Diagnostic d=p.diagnostic(T(ms));
                CHECK(!d.snapshot.model_valid&&!d.snapshot.valid);
                if(scenario<5) {
                    CHECK(r==PIPELINE_WAITING&&d.snapshot.state==MX5_DR_READY);
                    CHECK(d.snapshot.frontier_ns==T(2050));
                    runtime::CoreBridgeQualification q=runtime::CoreBridgeQualification();
                    q.expected_context=p.context();q.now_mono_ns=T(ms);
                    adapter::DrSnapshot out=adapter::DrSnapshot();
                    CHECK(p.qualified_snapshot(T(ms),q,&out)==runtime::CORE_BRIDGE_UNQUALIFIED);
                    CHECK(!out.ready);waiting_events=p.status().events;
                } else CHECK(d.snapshot.state!=MX5_DR_READY);
            }
            if(ms==2200) {
                if(scenario==0) {
                    CHECK(r==PIPELINE_OK);
                    CHECK(p.diagnostic(T(ms)).snapshot.frontier_ns==T(2100));
                } else if(scenario>=2&&scenario<=4)
                    CHECK(p.diagnostic(T(ms)).snapshot.state!=MX5_DR_READY);
            }
            if(scenario==1&&ms==2400) {
                CHECK(r==PIPELINE_WAITING&&!p.status().resets);
                CHECK(p.status().events>waiting_events); // Raw ingestion continues during the wait.
                CHECK(p.diagnostic(T(ms)).snapshot.frontier_ns==T(2050));
            }
            if(scenario==1&&ms==2425) {
                CHECK(r==PIPELINE_MISSING_SENSOR&&p.status().resets==1);
                CHECK(p.context().generation>initial_generation);
            }
            if(scenario==1&&ms==2500)CHECK(p.diagnostic(T(ms)).snapshot.state==MX5_DR_UNSEEDED);
        }
        const Diagnostic d=p.diagnostic(T(end));
        if(bool(d.snapshot.model_valid)!=(scenario<2))
            std::fprintf(stderr,"stationary pending scenario=%u state=%u result=%s resets=%llu\n",
                scenario,unsigned(d.snapshot.state),mx5_dr_result_name(d.result),
                static_cast<unsigned long long>(p.status().resets));
        CHECK(bool(d.snapshot.model_valid)==(scenario<2));CHECK(!d.snapshot.valid);
        if(scenario==0)CHECK(std::fabs(d.snapshot.elapsed_s-2.1)<1e-9);
        if(scenario==1) {
            CHECK(p.context().generation>initial_generation);
            CHECK(std::fabs(d.snapshot.elapsed_s-0.1)<1e-9); // New moving pair after the fault.
        }
    }
}
static void transport_anchor_receipts() {
    // Independent new-anchor failure reproduction, now with the success
    // criterion: preceding fresh causal wheel/reverse samples remain usable
    // when newer transport samples have already reached the worker queue.
    for(unsigned transport=0;transport<2;++transport)
    for(unsigned mask=0;mask<4;++mask)
    for(unsigned phase=0;phase<=25;phase+=25) {
        Pipeline p;init(p);uint64_t initial_anchor=0;
        for(unsigned ms=50;ms<=5000;ms+=25) {
            if(ms%50==0) {
                for(unsigned j=0;j<3;++j) {
                    if(j==2&&ms%100)continue;
                    const unsigned measured=ms-((mask&(1u<<j))?50:0);
                    RawEvent e=raw(j==0?WHEELS:j==1?REVERSE:YAW,measured);
                    e.source_mono_ms=transport?int64_t(T(measured)/1000000):0;
                    e.received_ns=T(ms);e.receive_seq=ms+1;
                    const PipelineResult r=p.enqueue_raw(e);
                    CHECK(r==PIPELINE_OK||r==PIPELINE_WAITING);
                }
            }
            if(ms==500+phase||ms==1500+phase||ms==2500+phase||ms==3500+phase||
               ms==4500+phase||ms==4750+phase)
                CHECK(p.enqueue_position(fix(ms,1,ms==2500+phase||ms==4750+phase?0:1))==PIPELINE_OK);
            const PipelineResult r=p.drain(T(ms)-p.reorder_ns());
            CHECK(r==PIPELINE_OK||r==PIPELINE_WAITING);
            if(ms==1700) {
                const Diagnostic d=p.diagnostic(T(ms));
                if(d.snapshot.state!=MX5_DR_READY)
                    std::fprintf(stderr,"transport anchor clock=%u delay-mask=%u phase=%u gate=%s\n",
                        transport,mask,phase,anchor_gate_name(p.anchor_gate()));
                CHECK(d.snapshot.state==MX5_DR_READY&&d.snapshot.anchor_id);
                initial_anchor=d.snapshot.anchor_id;
            }
            if(ms==3000)CHECK(p.diagnostic(T(ms)).snapshot.model_valid);
            if(ms==4700)CHECK(p.diagnostic(T(ms)).snapshot.state==MX5_DR_READY);
        }
        const Diagnostic d=p.diagnostic(T(5000));
        CHECK(d.snapshot.model_valid&&!d.snapshot.valid&&!p.status().resets);
        CHECK(std::fabs(d.snapshot.body_heading_rad)<1e-9);
        CHECK(d.snapshot.anchor_id!=initial_anchor);
        CHECK(std::fabs(d.snapshot.elapsed_s-double(4900-4500-phase)/1000)<1e-9);
    }
}
static void transport_anchor_direction() {
    for(unsigned initial_reverse=0;initial_reverse<2;++initial_reverse)
    for(unsigned causal_change=0;causal_change<2;++causal_change) {
        Pipeline p;init(p);
        for(unsigned ms=50;ms<=2300;ms+=25) {
            if(ms%50==0) {
                for(unsigned j=0;j<3;++j) {
                    if(j==2&&ms%100)continue;
                    const unsigned measured=ms-(j<2?50:0);
                    const int reverse=measured>=(causal_change?1450:1500)?1-int(initial_reverse):int(initial_reverse);
                    RawEvent e=raw(j==0?WHEELS:j==1?REVERSE:YAW,measured,
                        measured>=1500&&measured<1800?10000:13600,reverse);
                    e.source_mono_ms=int64_t(T(measured)/1000000);
                    e.received_ns=T(ms);e.receive_seq=ms+1;
                    const PipelineResult r=p.enqueue_raw(e);
                    CHECK(r==PIPELINE_OK||r==PIPELINE_WAITING);
                }
            }
            if(ms==500||ms==1500||ms==2000)
                CHECK(p.enqueue_position(fix(ms,1,ms==2000?0:1))==PIPELINE_OK);
            const PipelineResult r=p.drain(T(ms)-p.reorder_ns());
            CHECK(r==PIPELINE_OK||r==PIPELINE_WAITING);
            if(ms==1700) {
                CHECK((p.diagnostic(T(ms)).snapshot.state==MX5_DR_READY)==!causal_change);
                if(causal_change)CHECK(p.anchor_gate()==GPS_GATE_REVERSE);
            }
        }
        const Diagnostic d=p.diagnostic(T(2300));
        CHECK(bool(d.snapshot.model_valid)==!causal_change);CHECK(!d.snapshot.valid);
        if(!causal_change) {
            CHECK(std::fabs(d.snapshot.body_heading_rad-(initial_reverse?3.14159265358979323846:0))<1e-9);
            CHECK(d.snapshot.accumulated_north_m< -3.9);
        }
    }
}
static void explicit_anchor_support() {
    for(unsigned bad=0;bad<12;++bad) {
        GpsWheel g;g.configure(true,250000000ULL);
        for(unsigned ms=0;ms<=1000;ms+=100) {
            helper_feed(g,ms);
            if(!ms)CHECK(!g.fix(fix(ms)));
        }
        // The chronological learning inputs are newer and unavailable at GPS
        // time. Choosing original causal support must not rewrite those inputs.
        g.wheels(T(1000),T(1100),20,0);g.reverse(T(1000),T(1100),1);
        GpsAnchorSupport s=GpsAnchorSupport();
        s.wheel_time_ns=s.reverse_time_ns=T(900);
        s.wheel_received_ns=s.reverse_received_ns=T(950);
        s.wheel_lease_ns=s.reverse_lease_ns=T(1150);s.wheel_speed_mps=10;
        if(bad==1)s.wheel_received_ns=T(1001);
        if(bad==2)s.reverse_received_ns=T(1001);
        if(bad==3)s.wheel_lease_ns=T(999);
        if(bad==4)s.reverse_lease_ns=T(999);
        if(bad==5)s.wheel_time_ns=T(700);
        if(bad==6)s.reverse_time_ns=T(700);
        if(bad==7)s.wheel_spread_mps=1;
        if(bad==8)s.wheel_speed_mps=0;
        if(bad==9)s.reverse=1;
        if(bad==10)s.wheel_received_ns=T(800);
        if(bad==11)s.reverse_received_ns=T(800);
        CHECK(g.fix(fix(1000),s)==!bad);
        if(!bad) {
            CHECK(!g.fix(fix(1100)));CHECK(g.gate()==GPS_GATE_SPEED);
            CHECK(!g.status().candidate_ready&&g.status().active_scale==1);
        }
    }
}
static void transport_anchor_missing_evidence() {
    // Old causal evidence cannot be replaced with a newer future receipt, and
    // a source reset cannot reuse the GPS pair or sensor history from before it.
    for(unsigned missing=0;missing<4;++missing) {
        Pipeline p;init(p);unsigned resets=0;
        for(unsigned ms=50;ms<=3000;ms+=25) {
            if(ms%50==0) {
                for(unsigned j=0;j<3;++j) {
                    if(j==2&&ms%100)continue;
                    const unsigned measured=ms-(j<2?50:0);
                    if(missing<2&&j==missing&&measured>1100&&measured<1500)continue;
                    if(missing==2&&j==1&&measured<1500)continue;
                    RawEvent e=raw(j==0?WHEELS:j==1?REVERSE:YAW,measured);
                    e.source_mono_ms=int64_t(T(measured)/1000000);
                    e.received_ns=T(ms);e.receive_seq=ms+1;
                    if(missing==3&&ms>=1000)e.epoch=2;
                    const PipelineResult r=p.enqueue_raw(e);
                    if(r==PIPELINE_SOURCE_RESET)++resets;
                    CHECK(r==PIPELINE_OK||r==PIPELINE_WAITING||(missing==3&&ms==1000&&j==0&&r==PIPELINE_SOURCE_RESET));
                }
            }
            if(ms==500||ms==1500||ms==2500||ms==2800)
                CHECK(p.enqueue_position(fix(ms,1,ms==2800?0:1))==PIPELINE_OK);
            const PipelineResult r=p.drain(T(ms)-p.reorder_ns());
            CHECK(r==PIPELINE_OK||r==PIPELINE_WAITING);
            if(ms==1700) {
                CHECK(p.diagnostic(T(ms)).snapshot.state!=MX5_DR_READY);
                if(missing<3)CHECK(p.anchor_gate()==(missing==0?GPS_GATE_WHEELS:GPS_GATE_REVERSE));
            }
        }
        const Diagnostic d=p.diagnostic(T(3000));
        CHECK(bool(d.snapshot.model_valid)==(missing==3));
        CHECK(!d.snapshot.valid&&resets==unsigned(missing==3));
    }
}
static void transport_anchor_history_capacity() {
    for(unsigned period=1;period<=2;++period) {
        Pipeline p;init(p);
        for(unsigned ms=0;ms<=3000;++ms) {
            if(ms>=75&&ms%period==0) {
                RawEvent w=raw(WHEELS,ms-75);w.received_ns=T(ms);w.receive_seq=ms+1;
                w.source_mono_ms=int64_t(T(ms-75)/1000000);
                CHECK(p.enqueue_raw(w)==PIPELINE_OK);
            }
            if(ms%50==0) {
                RawEvent r=raw(REVERSE,ms),y=raw(YAW,ms);
                r.source_mono_ms=y.source_mono_ms=int64_t(T(ms)/1000000);
                CHECK(p.enqueue_raw(r)==PIPELINE_OK);
                const PipelineResult result=p.enqueue_raw(y);
                CHECK(result==PIPELINE_OK||result==PIPELINE_WAITING);
            }
            if(ms==500||ms==1500||ms==2500)
                CHECK(p.enqueue_position(fix(ms,1,ms==2500?0:1))==PIPELINE_OK);
            const PipelineResult r=p.drain(T(ms)-p.reorder_ns());
            CHECK(r==PIPELINE_OK||r==PIPELINE_WAITING);
        }
        const Diagnostic d=p.diagnostic(T(3000));
        CHECK(bool(d.snapshot.model_valid)==(period==2));
        CHECK(!d.snapshot.valid&&!p.status().resets);
    }
}
int main() {
    learning(); gates(); evidence_resets(); pipeline_learning_and_reacquisition(); rejected_anchor_revokes();
    fast_outlier_revokes(); asynchronous_windows(); stationary_heading_continuity();
    stationary_sensor_boundaries();stationary_receipt_boundaries();
    stationary_history_phases();stationary_history_capacity();stationary_history_resets();
    stationary_pending_window();
    transport_anchor_receipts();
    transport_anchor_direction();explicit_anchor_support();
    transport_anchor_missing_evidence();transport_anchor_history_capacity();
    std::printf("MODEL GPS/wheel consistency and scale: %u checks (synthetic)\n",checks);
}
