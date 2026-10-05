// BETA domain host tests: MODEL reverse latch, BETA anchor gate, BETA core
// budget, map_model_publication accuracy/heading limits, and isolation of the
// existing MODEL/SHADOW diagnostic. Synthetic receipt-time streams only; this
// is not vehicle or phone evidence.
#include "navigation/pipeline.h"
#include "runtime/core_bridge.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <vector>
using namespace mx5;
using namespace mx5::navigation;
using mx5::runtime::BetaProfile;
using mx5::runtime::BetaModelInput;
static unsigned checks;
#define CHECK(x) do { ++checks; if(!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::exit(1); } } while(0)
static const double PI=3.14159265358979323846;
static uint64_t T(unsigned ms) { return 1000000000ULL+uint64_t(ms)*1000000ULL; }
static unsigned wheel_raw(double kmh) { return unsigned(std::lround(kmh*100+10000)); }
// research profile: rate = (mean-2047) * -0.000658615 rad/s
static unsigned yaw_raw(double rad_s) { return unsigned(std::lround(2047-rad_s/0.000658615)); }

struct Fix { unsigned ms; int mode; double kmh, heading, lat; };
struct Plan {
    unsigned end_ms;
    std::function<double(unsigned)> wheel;      // km/h at ms
    std::function<unsigned(unsigned)> yaw;      // raw mean at ms
    std::vector<std::pair<unsigned,int> > reverse;
    std::vector<Fix> fixes;
    std::function<void(Pipeline&,unsigned)> each;
    uint64_t epoch;
    Plan():end_ms(0),epoch(1) {
        wheel=[](unsigned){return 36.0;};
        yaw=[](unsigned){return 2047u;};
    }
};
static Fix fix(unsigned ms,int mode=1,double kmh=36,double heading=0) {
    Fix f; f.ms=ms; f.mode=mode; f.kmh=kmh; f.heading=heading;
    f.lat=35+kmh/3.6*(ms/1000.0)/111320; return f;
}
static RawEvent raw(SensorKind kind,unsigned ms,uint64_t seq,uint64_t epoch) {
    RawEvent r=RawEvent(); r.kind=kind; r.epoch=epoch; r.receive_seq=seq;
    r.received_ns=T(ms); r.source_mono_ms=0; r.count=1; r.reverse=0;
    return r;
}
static adapter::Observation position(const Fix& f,unsigned seq) {
    adapter::Observation o=adapter::Observation(); o.kind=adapter::Observation::POSITION;
    o.call_sequence=seq; o.mono_ns=T(f.ms); o.original_mode=f.mode;
    o.position.mode=f.mode; o.position.utc_seconds=1700000000+f.ms/1000;
    o.position.latitude_deg=f.lat; o.position.longitude_deg=135;
    o.position.heading_deg=f.heading; o.position.velocity_kmh=f.kmh;
    return o;
}
static void init(Pipeline& p,bool latch=true,bool beta=true) {
    mx5_dr_context x={1,1,1};
    CHECK(p.init_model(research_model_profile(),mx5_dr_default_config(),x,false,false,latch));
    if(beta) CHECK(p.enable_beta(runtime::beta_profile()));
}
static void run(Pipeline& p,const Plan& plan,unsigned from=0) {
    static uint64_t seq=0;
    unsigned call=0;
    for(unsigned ms=from;ms<=plan.end_ms;ms+=100) {
        for(size_t j=0;j<plan.reverse.size();++j) if(plan.reverse[j].first==ms) {
            RawEvent r=raw(REVERSE,ms,++seq,plan.epoch); r.reverse=plan.reverse[j].second;
            CHECK(p.enqueue_raw(r)==PIPELINE_OK);
        }
        RawEvent w=raw(WHEELS,ms,++seq,plan.epoch);
        for(unsigned i=0;i<4;++i) w.raw[i]=uint16_t(wheel_raw(plan.wheel(ms)));
        CHECK(p.enqueue_raw(w)==PIPELINE_OK);
        RawEvent y=raw(YAW,ms,++seq,plan.epoch); y.raw[0]=uint16_t(plan.yaw(ms));
        const PipelineResult yr=p.enqueue_raw(y);
        CHECK(yr==PIPELINE_OK||yr==PIPELINE_WAITING);
        for(size_t j=0;j<plan.fixes.size();++j)
            if(plan.fixes[j].ms>=ms&&plan.fixes[j].ms<ms+100)
                CHECK(p.enqueue_position(position(plan.fixes[j],++call))==PIPELINE_OK);
        if(ms>=100) p.drain(T(ms)-100000000ULL);
        if(plan.each) plan.each(p,ms);
    }
}
// Straight 36 km/h north, gated fixes at 2410/2510 ms, GPS lost at 2620 ms.
static Plan straight(unsigned end_ms=4000) {
    Plan plan; plan.end_ms=end_ms; plan.reverse.push_back(std::make_pair(0u,0));
    plan.fixes.push_back(fix(2410)); plan.fixes.push_back(fix(2510));
    plan.fixes.push_back(fix(2620,0));
    return plan;
}
static runtime::CoreBridgeResult publish(const Pipeline& p,unsigned ms,adapter::DrSnapshot* out,
                                         BetaModelInput* input=0) {
    const BetaModelInput in=p.model_publication(T(ms));
    if(input)*input=in;
    return runtime::map_model_publication(in,runtime::beta_profile(),out);
}

static void latch_seeds_from_change_only_stream() {
    // One REVERSE message at boot, a 1 s standstill, then motion: the latch
    // is not released at wheel speed 0 and both MODEL and BETA cores seed.
    Pipeline p; init(p);
    Plan plan=straight();
    plan.wheel=[](unsigned ms){return ms>=500&&ms<1500?0.0:36.0;};
    bool latched_throughout=true;
    plan.each=[&](Pipeline& q,unsigned ms){ if(ms>=200&&!q.reverse_latched()){ if(latched_throughout)std::fprintf(stderr,"unlatched at %u\n",ms); latched_throughout=false; } };
    run(p,plan);
    CHECK(latched_throughout);
    CHECK(!p.status().resets);
    CHECK((p.status().uncertainties&REVERSE_LATCH_MODEL)!=0);
    CHECK(p.diagnostic(T(4000)).snapshot.model_valid);
    CHECK(p.beta_gate()==BETA_GATE_ACCEPTED);
    adapter::DrSnapshot s; BetaModelInput in;
    CHECK(publish(p,4000,&s,&in)==runtime::CORE_BRIDGE_OK);
    CHECK(s.ready&&s.beta&&!s.profile_verified&&!s.input_quality_verified);
    CHECK(std::fabs(s.speed_mps-10)<1e-6 && std::fabs(s.travel_bearing_deg)<1e-6);
    CHECK(s.valid_until_mono_ns==in.snapshot.frontier_ns+500000000ULL);

    // The same stream without the latch keeps the original 250 ms lease.
    Pipeline lease; init(lease,false,false);
    run(lease,straight());
    CHECK(!lease.diagnostic(T(4000)).snapshot.model_valid);
    Pipeline refused; init(refused,false,false);
    CHECK(!refused.enable_beta(runtime::beta_profile()));
}
static void unknown_reverse_never_seeds() {
    Pipeline p; init(p);
    Plan plan=straight(); plan.reverse.clear();
    run(p,plan);
    CHECK(!p.reverse_latched());
    CHECK(p.beta_gate()==BETA_GATE_REVERSE);
    CHECK(!p.diagnostic(T(4000)).snapshot.model_valid);
    adapter::DrSnapshot s;
    CHECK(publish(p,4000,&s)!=runtime::CORE_BRIDGE_OK && !s.ready);
}
static void epoch_change_and_lost_reverse_unseed() {
    // Source epoch change: the latch is gone and nothing seeds in epoch 2.
    Pipeline p; init(p);
    Plan first; first.end_ms=1000; first.reverse.push_back(std::make_pair(0u,0));
    run(p,first);
    CHECK(p.reverse_latched());
    RawEvent other=raw(WHEELS,1100,1,2);
    for(unsigned i=0;i<4;++i) other.raw[i]=uint16_t(wheel_raw(36));
    CHECK(p.enqueue_raw(other)==PIPELINE_SOURCE_RESET);
    CHECK(!p.reverse_latched());
    Plan second=straight(); second.reverse.clear(); second.epoch=2;
    for(size_t j=0;j<second.fixes.size();++j) second.fixes[j].ms+=1200;
    for(size_t j=0;j<second.fixes.size();++j) second.fixes[j]=fix(second.fixes[j].ms,second.fixes[j].mode);
    second.end_ms=5200;
    run(p,second,1200);
    CHECK(p.beta_gate()==BETA_GATE_REVERSE);
    adapter::DrSnapshot s;
    CHECK(publish(p,5200,&s)!=runtime::CORE_BRIDGE_OK);

    // A fault that drops no REVERSE message keeps the latch; a REVERSE
    // message the pipeline rejects clears it (its change would be lost).
    Pipeline q; init(q);
    run(q,first);
    RawEvent bad=raw(WHEELS,1100,100000,1); bad.raw[0]=50000;
    CHECK(q.enqueue_raw(bad)==PIPELINE_BAD_INPUT);
    CHECK(q.status().resets==1 && q.reverse_latched());
    RawEvent invalid=raw(REVERSE,1200,100001,1); invalid.reverse=7;
    CHECK(q.enqueue_raw(invalid)==PIPELINE_BAD_INPUT);
    CHECK(!q.reverse_latched());
}
static BetaAnchorGate gate_case(double gps_kmh,double wheel_kmh,double prev_kmh,double prev_heading,
                                double heading,unsigned yaw_spike_ms=0,int reverse=0,unsigned prev_ms=2410) {
    Pipeline p; init(p);
    Plan plan; plan.end_ms=2800; plan.reverse.push_back(std::make_pair(0u,reverse));
    plan.wheel=[wheel_kmh](unsigned){return wheel_kmh;};
    plan.yaw=[yaw_spike_ms](unsigned ms){return yaw_spike_ms&&ms==yaw_spike_ms?yaw_raw(0.06):2047u;};
    plan.fixes.push_back(fix(prev_ms,1,prev_kmh,prev_heading));
    plan.fixes.push_back(fix(2510,1,gps_kmh,heading));
    run(p,plan);
    CHECK(!p.status().resets);
    return p.beta_gate();
}
static void anchor_gate() {
    CHECK(gate_case(36,36,36,0,0)==BETA_GATE_ACCEPTED);
    CHECK(gate_case(20,20,15,0,0)==BETA_GATE_ACCEPTED);       // both boundaries
    CHECK(gate_case(19,19,36,0,0)==BETA_GATE_SPEED);
    CHECK(gate_case(61,61,61,0,0)==BETA_GATE_SPEED);          // validated range ends at 60
    CHECK(gate_case(36,36,14,0,0)==BETA_GATE_PREVIOUS);
    CHECK(gate_case(36,36,36,0,4)==BETA_GATE_COURSE);
    CHECK(gate_case(36,36,36,0,3)==BETA_GATE_ACCEPTED);
    CHECK(gate_case(36,36,36,359,1.5)==BETA_GATE_ACCEPTED);   // course wraps at north
    CHECK(gate_case(36,36,36,0,0,1000)==BETA_GATE_YAW);       // inside the last 2 s
    CHECK(gate_case(36,36,36,0,0,300)==BETA_GATE_ACCEPTED);   // before the last 2 s
    CHECK(gate_case(36,41,36,0,0)==BETA_GATE_WHEEL);
    CHECK(gate_case(36,39.5,36,0,0)==BETA_GATE_ACCEPTED);
    CHECK(gate_case(36,36,36,0,0,0,1)==BETA_GATE_REVERSE);    // forward only
    CHECK(gate_case(36,36,36,0,0,0,0,310)==BETA_GATE_PREVIOUS); // not consecutive (2.2 s)
}
static void no_reanchor_from_bad_fix() {
    // A later fix whose GPS speed disagrees with the wheels re-anchors the
    // MODEL core but never the BETA core; the BETA clock stays at 2510 ms.
    Pipeline p; init(p);
    Plan plan; plan.end_ms=5000; plan.reverse.push_back(std::make_pair(0u,0));
    plan.fixes.push_back(fix(2410)); plan.fixes.push_back(fix(2510));
    plan.fixes.push_back(fix(3510,1,25)); plan.fixes.push_back(fix(3610,1,25));
    plan.fixes.push_back(fix(3720,0));
    run(p,plan);
    CHECK(p.beta_gate()==BETA_GATE_WHEEL);
    adapter::DrSnapshot s; BetaModelInput in;
    CHECK(publish(p,5000,&s,&in)==runtime::CORE_BRIDGE_OK);
    const Diagnostic d=p.diagnostic(T(5000));
    CHECK(d.snapshot.model_valid);
    CHECK(std::fabs(in.snapshot.elapsed_s-double(in.snapshot.frontier_ns-T(2510))/1e9)<1e-9);
    CHECK(std::fabs(d.snapshot.elapsed_s-double(d.snapshot.frontier_ns-T(3610))/1e9)<1e-9);
    CHECK(in.snapshot.elapsed_s>d.snapshot.elapsed_s+0.9);
}
static void budget_formula_and_limit() {
    // Straight 10 m/s: core budget = e0 + sv t + v (h0 t + k t^2/2) (+ the
    // core's own age term), accuracy adds (v+sv)*0.5 s, and the BETA window
    // closes at 40 m without ever reporting a clamped value.
    Pipeline p; init(p);
    Plan plan=straight(30000);
    const BetaProfile b=runtime::beta_profile();
    bool closed=false; unsigned published=0, first_closed=0;
    plan.each=[&](Pipeline& q,unsigned ms) {
        if(ms<2800) return;
        adapter::DrSnapshot s; BetaModelInput in;
        const runtime::CoreBridgeResult r=publish(q,ms,&s,&in);
        if(r==runtime::CORE_BRIDGE_OK) {
            CHECK(!closed);
            ++published;
            const double t=in.snapshot.elapsed_s, v=10.0;
            const double age=double(in.query_mono_ns-in.snapshot.frontier_ns)/1e9;
            const double core=b.anchor_error_m+b.speed_error_mps*t+
                v*(b.heading_error_rad*t+b.yaw_error_rad_s*t*t/2)+(v+b.speed_error_mps)*age;
            CHECK(std::fabs(in.snapshot.error_budget_m-core)<0.05);
            const double lease=double(s.valid_until_mono_ns-in.snapshot.frontier_ns)/1e9;
            CHECK(std::fabs(lease-0.5)<1e-12);
            CHECK(s.accuracy_m==in.snapshot.error_budget_m+(s.speed_mps+b.speed_error_mps)*lease);
            CHECK(s.accuracy_m<=40.0);
        } else {
            if(!closed) first_closed=ms;
            closed=true; CHECK(!s.ready && s.accuracy_m==0);
        }
    };
    run(p,plan);
    CHECK(published>100 && closed);
    // 20+0.6t+0.01t^2+5.15 = 40 at t ~ 18.8 s after the 2510 ms anchor.
    CHECK(first_closed>2510+18000 && first_closed<2510+19500);
}
static BetaModelInput synthetic(double budget,double heading) {
    BetaModelInput in; std::memset(&in,0,sizeof in);
    mx5_dr_snapshot& s=in.snapshot;
    s.context.source_epoch=s.context.session_epoch=s.context.generation=1;
    s.anchor_id=s.processed_position_seq=s.solution_seq=1;
    s.frontier_ns=T(1000); s.derived_utc_ns=1700000000000000000ULL;
    s.latitude_deg=35; s.longitude_deg=135; s.speed_mps=10; s.has_bearing=1;
    s.elapsed_s=10; s.distance_m=100; s.error_budget_m=budget; s.heading_budget_rad=0.05;
    s.domain=MX5_DR_MODEL_DOMAIN; s.model_valid=1; s.state=MX5_DR_ACTIVE; s.reason=MX5_DR_OK;
    in.result=MX5_DR_OK; in.now_mono_ns=in.query_mono_ns=T(1000);
    in.lease_cap_mono_ns=UINT64_MAX; in.heading_budget_rad=heading;
    return in;
}
static void accuracy_boundary_and_heading_withdrawal() {
    const BetaProfile b=runtime::beta_profile();
    const double lease_term=(10+b.speed_error_mps)*0.5;
    adapter::DrSnapshot s;
    CHECK(runtime::map_model_publication(synthetic(39.9-lease_term,0.1),b,&s)==runtime::CORE_BRIDGE_OK);
    CHECK(s.ready && std::fabs(s.accuracy_m-39.9)<1e-9);
    CHECK(runtime::map_model_publication(synthetic(40.0-lease_term,0.1),b,&s)==runtime::CORE_BRIDGE_OK);
    CHECK(runtime::map_model_publication(synthetic(40.1-lease_term,0.1),b,&s)==runtime::CORE_BRIDGE_LIMIT);
    CHECK(!s.ready && !s.beta && s.accuracy_m==0);
    const double max_h=b.heading_budget_max_rad-b.yaw_error_rad_s*0.5;
    CHECK(runtime::map_model_publication(synthetic(25,max_h-1e-6),b,&s)==runtime::CORE_BRIDGE_OK);
    CHECK(runtime::map_model_publication(synthetic(25,max_h+1e-6),b,&s)==runtime::CORE_BRIDGE_BEARING);
    CHECK(!s.ready);
    // Not ACTIVE (GPS still present), qualified-domain or expired: never ready.
    BetaModelInput in=synthetic(25,0.1); in.snapshot.state=MX5_DR_READY;
    CHECK(runtime::map_model_publication(in,b,&s)==runtime::CORE_BRIDGE_UNQUALIFIED);
    in=synthetic(25,0.1); in.snapshot.domain=MX5_DR_QUALIFIED_DOMAIN;
    CHECK(runtime::map_model_publication(in,b,&s)==runtime::CORE_BRIDGE_UNQUALIFIED);
    in=synthetic(25,0.1); in.now_mono_ns=T(1501);
    CHECK(runtime::map_model_publication(in,b,&s)==runtime::CORE_BRIDGE_TIME);
    in=synthetic(25,0.1); in.lease_cap_mono_ns=T(1200);
    CHECK(runtime::map_model_publication(in,b,&s)==runtime::CORE_BRIDGE_OK);
    CHECK(s.valid_until_mono_ns==T(1200) && std::fabs(s.accuracy_m-(25+10.3*0.2))<1e-9);
    in=synthetic(25,0.1); in.snapshot.elapsed_s=59.8;
    CHECK(runtime::map_model_publication(in,b,&s)==runtime::CORE_BRIDGE_LIMIT);

    // Pipeline: a steady 0.3 rad/s turn after the gap withdraws BETA through
    // the heading budget (0.03+0.002t+0.1*rotation > 20 deg near t=10 s)
    // while the position budget is still below 40 m.
    Pipeline p; init(p);
    Plan plan=straight(16000);
    plan.wheel=[](unsigned){return 25.0;};
    for(size_t j=0;j<plan.fixes.size();++j) plan.fixes[j]=fix(plan.fixes[j].ms,plan.fixes[j].mode,25);
    plan.yaw=[](unsigned ms){return ms>=2700?yaw_raw(0.3):2047u;};
    runtime::CoreBridgeResult last=runtime::CORE_BRIDGE_OK; unsigned withdrawn=0;
    plan.each=[&](Pipeline& q,unsigned ms) {
        if(ms<2800) return;
        adapter::DrSnapshot out; BetaModelInput bin;
        const runtime::CoreBridgeResult r=publish(q,ms,&out,&bin);
        if(r==runtime::CORE_BRIDGE_BEARING&&last==runtime::CORE_BRIDGE_OK) {
            withdrawn=ms;
            CHECK(bin.snapshot.error_budget_m<35);
            CHECK(std::fabs(q.beta_rotation_rad()-0.3*(bin.snapshot.frontier_ns-T(2600))/1e9)<0.01);
        }
        if(last==runtime::CORE_BRIDGE_BEARING) CHECK(r!=runtime::CORE_BRIDGE_OK);
        last=r;
    };
    run(p,plan);
    CHECK(withdrawn>2510+9000 && withdrawn<2510+11000);
}
static void pending_gps_caps_and_hides() {
    Pipeline p; init(p);
    run(p,straight(3500));
    adapter::DrSnapshot s;
    CHECK(publish(p,3500,&s)==runtime::CORE_BRIDGE_OK);
    // A queued GPS return caps the lease; once due it hides the output.
    CHECK(p.enqueue_position(position(fix(3550),99))==PIPELINE_OK);
    CHECK(publish(p,3500,&s)==runtime::CORE_BRIDGE_OK);
    CHECK(s.valid_until_mono_ns==T(3550)-1);
    BetaModelInput in=p.model_publication(T(3550));
    CHECK(in.result==MX5_DR_E_NO_SEED && !in.snapshot.model_valid);
}
static void shadow_diagnostic_unchanged_by_beta() {
    // The MODEL core (SHADOW diagnostic) is identical with and without the
    // BETA core for the same input, including a fix the BETA gate rejects.
    Pipeline with; init(with,true,true);
    Pipeline without; init(without,true,false);
    Plan plan; plan.end_ms=9000; plan.reverse.push_back(std::make_pair(0u,0));
    plan.yaw=[](unsigned ms){return ms>=5000&&ms<6000?yaw_raw(0.2):2047u;};
    plan.fixes.push_back(fix(2410)); plan.fixes.push_back(fix(2510));
    plan.fixes.push_back(fix(3510,1,25)); plan.fixes.push_back(fix(3610,1,25));
    plan.fixes.push_back(fix(3720,0));
    unsigned compared=0;
    std::vector<Diagnostic> a,b;
    plan.each=[&](Pipeline& q,unsigned ms) { (&q==&with?a:b).push_back(q.diagnostic(T(ms))); };
    run(with,plan); run(without,plan);
    CHECK(a.size()==b.size());
    for(size_t j=0;j<a.size();++j) {
        const mx5_dr_snapshot& x=a[j].snapshot; const mx5_dr_snapshot& y=b[j].snapshot;
        CHECK(a[j].result==b[j].result && x.state==y.state && x.model_valid==y.model_valid);
        CHECK(x.latitude_deg==y.latitude_deg && x.longitude_deg==y.longitude_deg);
        CHECK(x.error_budget_m==y.error_budget_m && x.heading_budget_rad==y.heading_budget_rad);
        CHECK(x.body_heading_rad==y.body_heading_rad && x.frontier_ns==y.frontier_ns);
        CHECK(x.context.generation==y.context.generation && x.anchor_id==y.anchor_id);
        CHECK(a[j].status.intervals==b[j].status.intervals && a[j].status.resets==b[j].status.resets);
        if(x.model_valid) ++compared;
    }
    CHECK(compared>40);
}
int main() {
    latch_seeds_from_change_only_stream();
    unknown_reverse_never_seeds();
    epoch_change_and_lost_reverse_unseed();
    anchor_gate();
    no_reanchor_from_bad_fix();
    budget_formula_and_limit();
    accuracy_boundary_and_heading_withdrawal();
    pending_gps_caps_and_hides();
    shadow_diagnostic_unchanged_by_beta();
    std::printf("beta tests passed (%u checks)\n",checks);
    return 0;
}
