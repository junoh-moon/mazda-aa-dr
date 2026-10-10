// BETA domain host tests: MODEL reverse latch, BETA anchor gate, BETA core
// budget, map_model_publication accuracy/heading limits, and isolation of the
// existing MODEL/SHADOW diagnostic. Synthetic receipt-time streams only; this
// is not vehicle or phone evidence.
//
// 2026-10-05 (validation/BETA_DECISIONS_2026-10-05.md 3.1-3.5): fixture
// streams changed with the rules, not to fit them. A gated anchor now needs a
// strictly increasing utc pair (a same-second pair such as the former
// 2410/2510 ms fixes is no longer a pair), 10 s of consecutive increasing
// fixes, HDOP <= 3 and a pair displacement consistent with v*dt; a reverse
// 1->0 transition must have been seen; the BETA yaw zero is 2048. The plans
// therefore send 1 Hz fixes from 410 ms (anchor at 11410 ms), HDOP 1, a
// reverse 1 then 0 at boot, and the straight-line yaw raw 2048.
#include "navigation/pipeline.h"
#include "runtime/core_bridge.h"
#include "runtime/motion_gap.h"
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
static uint64_t T(unsigned ms) { return 1000000000ULL+uint64_t(ms)*1000000ULL; }
static unsigned wheel_raw(double kmh) { return unsigned(::lround(kmh*100+10000)); }
// BETA rate = (mean-2048) * -0.000658615 rad/s (fixed BETA zero, rule 3.5).
static const unsigned STRAIGHT=2048;
static unsigned yaw_raw(double rad_s) { return unsigned(::lround(2048-rad_s/0.000658615)); }
// The straight plans: 1 Hz fixes 410..11410 ms, GPS lost at 11620 ms.
static const unsigned ANCHOR_MS=11410, LOST_MS=11620;

struct Fix { unsigned ms; int mode; double kmh, heading, lat, hdop; uint64_t utc; double lon; };
struct Plan {
    unsigned end_ms;
    std::function<double(unsigned)> wheel;      // km/h at ms
    std::function<unsigned(unsigned)> yaw;      // raw mean at ms
    std::vector<std::pair<unsigned,int> > reverse;
    std::vector<Fix> fixes;
    std::function<void(Pipeline&,unsigned)> each;
    uint64_t epoch;
    unsigned yaw_count;                       // samples per yaw window (raw[0] is their sum)
    Plan():end_ms(0),epoch(1),yaw_count(1) {
        wheel=[](unsigned){return 36.0;};
        yaw=[](unsigned){return STRAIGHT;};
    }
    // A change-only producer that has been seen leaving reverse (3.4).
    void boot_reverse() { reverse.push_back(std::make_pair(0u,1)); reverse.push_back(std::make_pair(100u,0)); }
};
static uint64_t utc_at(unsigned ms) { return 1700000000ULL+ms/1000; }
static Fix fix(unsigned ms,int mode=1,double kmh=36,double heading=0) {
    Fix f; f.ms=ms; f.mode=mode; f.kmh=kmh; f.heading=heading; f.hdop=1.0; f.utc=utc_at(ms); f.lon=135;
    f.lat=35+kmh/3.6*(ms/1000.0)/111320; return f;
}
static Fix fix_at(unsigned ms,double lat,double kmh=36,double heading=0) {
    Fix f=fix(ms,1,kmh,heading); f.lat=lat; return f;
}
static RawEvent raw(SensorKind kind,unsigned ms,uint64_t seq,uint64_t epoch) {
    RawEvent r=RawEvent(); r.kind=kind; r.epoch=epoch; r.receive_seq=seq;
    r.received_ns=T(ms); r.source_mono_ms=0; r.count=1; r.reverse=0;
    return r;
}
static adapter::Observation position(const Fix& f,unsigned seq) {
    adapter::Observation o=adapter::Observation(); o.kind=adapter::Observation::POSITION;
    o.call_sequence=seq; o.mono_ns=T(f.ms); o.original_mode=f.mode;
    o.position.mode=f.mode; o.position.utc_seconds=f.utc;
    o.position.latitude_deg=f.lat; o.position.longitude_deg=f.lon;
    o.position.heading_deg=f.heading; o.position.velocity_kmh=f.kmh;
    o.position.horizontal=f.hdop; o.position.vertical=f.hdop;
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
        RawEvent y=raw(YAW,ms,++seq,plan.epoch); y.raw[0]=uint16_t(plan.yaw(ms)); y.count=uint16_t(plan.yaw_count);
        const PipelineResult yr=p.enqueue_raw(y);
        CHECK(yr==PIPELINE_OK||yr==PIPELINE_WAITING);
        for(size_t j=0;j<plan.fixes.size();++j)
            if(plan.fixes[j].ms>=ms&&plan.fixes[j].ms<ms+100)
                CHECK(p.enqueue_position(position(plan.fixes[j],++call))==PIPELINE_OK);
        if(ms>=100) p.drain(T(ms)-100000000ULL);
        if(plan.each) plan.each(p,ms);
    }
}
// Straight north, 1 Hz fixes 410..11410 ms (gated anchor at 11410), GPS lost
// at 11620 ms.
static Plan straight(unsigned end_ms=ANCHOR_MS+1600,double kmh=36) {
    Plan plan; plan.end_ms=end_ms; plan.boot_reverse();
    for(unsigned ms=410;ms<=ANCHOR_MS;ms+=1000) plan.fixes.push_back(fix(ms,1,kmh));
    plan.fixes.push_back(fix(LOST_MS,0,kmh));
    return plan;
}
static runtime::CoreBridgeResult publish(const Pipeline& p,unsigned ms,adapter::DrSnapshot* out,
                                         BetaModelInput* input=0) {
    const BetaModelInput in=p.model_publication(T(ms));
    if(input)*input=in;
    return runtime::map_model_publication(in,runtime::beta_profile(),out);
}

static void latch_seeds_from_change_only_stream() {
    // Reverse 1 then 0 at boot, a 1 s standstill, then motion: the latch is
    // not released at wheel speed 0 and both MODEL and BETA cores seed.
    Pipeline p; init(p);
    Plan plan=straight();
    plan.wheel=[](unsigned ms){return ms>=500&&ms<1500?0.0:36.0;};
    bool latched_throughout=true;
    plan.each=[&](Pipeline& q,unsigned ms){ if(ms>=200&&!q.reverse_latched()){ if(latched_throughout)std::fprintf(stderr,"unlatched at %u\n",ms); latched_throughout=false; } };
    run(p,plan);
    const unsigned end=plan.end_ms;
    CHECK(latched_throughout && p.reverse_exit_seen());
    CHECK(!p.status().resets);
    CHECK((p.status().uncertainties&REVERSE_LATCH_MODEL)!=0);
    CHECK(p.diagnostic(T(end)).snapshot.model_valid);
    CHECK(p.beta_gate()==BETA_GATE_ACCEPTED);
    adapter::DrSnapshot s; BetaModelInput in;
    CHECK(publish(p,end,&s,&in)==runtime::CORE_BRIDGE_OK);
    CHECK(s.ready&&s.beta&&!s.profile_verified&&!s.input_quality_verified&&!s.speed_only);
    CHECK(std::fabs(s.speed_mps-10)<1e-6 && std::fabs(s.travel_bearing_deg)<1e-6);
    CHECK(s.valid_until_mono_ns==in.snapshot.frontier_ns+500000000ULL);
    CHECK(in.rotation_rad==0 && in.rotation_budget_m==0); // yaw 2048 is straight for BETA

    // The same stream without the latch keeps the original 250 ms lease.
    Pipeline lease; init(lease,false,false);
    run(lease,straight());
    CHECK(!lease.diagnostic(T(end)).snapshot.model_valid);
    Pipeline refused; init(refused,false,false);
    CHECK(!refused.enable_beta(runtime::beta_profile()));
}
static void unknown_reverse_never_seeds() {
    Pipeline p; init(p);
    Plan plan=straight(); plan.reverse.clear();
    run(p,plan);
    CHECK(!p.reverse_latched());
    CHECK(p.beta_gate()==BETA_GATE_REVERSE);
    CHECK(!p.diagnostic(T(plan.end_ms)).snapshot.model_valid);
    adapter::DrSnapshot s;
    CHECK(publish(p,plan.end_ms,&s)!=runtime::CORE_BRIDGE_OK && !s.ready);
    // 3.4 (as clarified by the coordinator on 2026-10-05): a forward FIRST
    // message of the source epoch counts like a 1->0 transition (the
    // 2026-10-04 drive has only forward messages).
    Pipeline f; init(f);
    Plan first_forward=straight(); first_forward.reverse.clear();
    first_forward.reverse.push_back(std::make_pair(0u,0));
    run(f,first_forward);
    CHECK(f.reverse_latched() && f.reverse_exit_seen() && f.beta_gate()==BETA_GATE_ACCEPTED);
    // A forward latch after a first REVERSE message, without a 1->0
    // transition (the latch was cleared by a reset in between), does not
    // seed BETA; the MODEL core is unchanged and still seeds.
    Pipeline q; init(q);
    Plan forward=straight(); forward.reverse.clear();
    forward.reverse.push_back(std::make_pair(0u,1)); forward.reverse.push_back(std::make_pair(300u,0));
    forward.each=[](Pipeline& x,unsigned ms){ if(ms==200) x.reset(x.context()); };
    run(q,forward);
    CHECK(q.reverse_latched() && !q.reverse_exit_seen());
    CHECK(q.latch_clears(LATCH_CLEAR_RESET)==1 && q.last_latch_clear()==LATCH_CLEAR_RESET);
    CHECK(q.beta_gate()==BETA_GATE_REVERSE_UNPROVEN);
    CHECK(publish(q,forward.end_ms,&s)!=runtime::CORE_BRIDGE_OK && !s.ready);
    CHECK(q.diagnostic(T(forward.end_ms)).snapshot.model_valid);
}
static void epoch_change_and_lost_reverse_unseed() {
    // Source epoch change: the latch is gone and nothing seeds in epoch 2.
    Pipeline p; init(p);
    Plan first; first.end_ms=1000; first.boot_reverse();
    run(p,first);
    CHECK(p.reverse_latched() && p.reverse_exit_seen());
    RawEvent other=raw(WHEELS,1100,1,2);
    for(unsigned i=0;i<4;++i) other.raw[i]=uint16_t(wheel_raw(36));
    CHECK(p.enqueue_raw(other)==PIPELINE_SOURCE_RESET);
    CHECK(!p.reverse_latched() && !p.reverse_exit_seen());
    Plan second=straight(); second.reverse.clear(); second.epoch=2;
    for(size_t j=0;j<second.fixes.size();++j)
        second.fixes[j]=fix(second.fixes[j].ms+1200,second.fixes[j].mode);
    second.end_ms=straight().end_ms+1200;
    run(p,second,1200);
    CHECK(p.beta_gate()==BETA_GATE_REVERSE);
    adapter::DrSnapshot s;
    CHECK(publish(p,second.end_ms,&s)!=runtime::CORE_BRIDGE_OK);

    // 3.4: every reset clears the latch (formerly a fault that dropped no
    // queued REVERSE message kept it). A REVERSE message the pipeline
    // rejects, or one the runtime excludes, also clears it.
    Pipeline q; init(q);
    run(q,first);
    RawEvent bad=raw(WHEELS,1100,100000,1); bad.raw[0]=50000;
    CHECK(q.enqueue_raw(bad)==PIPELINE_BAD_INPUT);
    CHECK(q.status().resets==1 && !q.reverse_latched());
    CHECK(q.reverse_exit_seen()); // same source epoch: the producer property stays
    Pipeline r; init(r);
    run(r,first);
    RawEvent invalid=raw(REVERSE,1200,100001,1); invalid.reverse=7;
    CHECK(r.enqueue_raw(invalid)==PIPELINE_BAD_INPUT);
    CHECK(!r.reverse_latched());
    Pipeline x; init(x);
    run(x,first);
    CHECK(x.reverse_latched());
    x.exclude_reverse();
    CHECK(!x.reverse_latched());
    Pipeline y; init(y);
    run(y,first);
    y.reset(y.context());
    CHECK(!y.reverse_latched() && y.latch_clears(LATCH_CLEAR_RESET)==1);
    // Task E: the runtime's small-gap rejection reset keeps the latch (and
    // the producer history) even with a source epoch bump...
    Pipeline k; init(k);
    run(k,first);
    mx5_dr_context bumped=k.context(); ++bumped.source_epoch; ++bumped.generation;
    k.reset_keep_reverse(bumped);
    CHECK(k.reverse_latched() && k.reverse_exit_seen() && !k.latch_clears_total());
    CHECK(k.status().resets==0 && !k.diagnostic(T(1100)).snapshot.model_valid);
    // ...unless the reset discards a queued REVERSE message.
    Pipeline l; init(l);
    run(l,first);
    RawEvent queued=raw(REVERSE,1100,200000,1); queued.reverse=1;
    CHECK(l.enqueue_raw(queued)==PIPELINE_OK);
    l.reset_keep_reverse(bumped);
    CHECK(!l.reverse_latched() && l.latch_clears(LATCH_CLEAR_RESET)==1);
    // A genuine source reset (plain reset with a new epoch) clears both.
    Pipeline m; init(m);
    run(m,first);
    m.reset(bumped);
    CHECK(!m.reverse_latched() && !m.reverse_exit_seen() && m.latch_clears(LATCH_CLEAR_SOURCE_EPOCH)==1);
    m.exclude_reverse(LATCH_CLEAR_INPUT_GAP);
    CHECK(m.latch_clears(LATCH_CLEAR_INPUT_GAP)==0); // nothing valid to clear
    CHECK(std::strcmp(latch_clear_name(LATCH_CLEAR_INPUT_GAP),"input_gap")==0);
}
struct GateCase {
    double gps_kmh, wheel_kmh, prev_kmh, prev_heading, heading;
    unsigned yaw_spike_ms; int reverse; unsigned prev_ms, streak_from_ms;
    double hdop, displacement_scale; int utc_offset; unsigned fix_ms;
    bool reverse_exit; unsigned gap_ms;
    GateCase(double g,double w,double pk,double ph,double h)
        : gps_kmh(g),wheel_kmh(w),prev_kmh(pk),prev_heading(ph),heading(h),yaw_spike_ms(0),reverse(0),
          prev_ms(ANCHOR_MS-1000),streak_from_ms(410),hdop(1.0),displacement_scale(1.0),utc_offset(0),
          fix_ms(ANCHOR_MS),reverse_exit(true),gap_ms(0) {}
};
// The tested pair is (prev_ms, fix_ms); the fixes before prev_ms only build
// the consecutive run (36 km/h, 1 Hz) from streak_from_ms. Coordinates are
// cumulative so that the pair displacement matches the GPS speeds.
static BetaAnchorGate gate_case(const GateCase& c,Pipeline* out=0) {
    Pipeline local; Pipeline& p=out?*out:local; init(p);
    Plan plan; plan.end_ms=c.fix_ms+300;
    if(c.reverse_exit) plan.boot_reverse();
    if(c.reverse || !c.reverse_exit) plan.reverse.push_back(std::make_pair(200u,c.reverse));
    plan.wheel=[c](unsigned){return c.wheel_kmh;};
    plan.yaw=[c](unsigned ms){return c.yaw_spike_ms&&ms==c.yaw_spike_ms?yaw_raw(0.06):STRAIGHT;};
    double lat=35;
    unsigned last=0;
    for(unsigned ms=c.streak_from_ms;ms<c.prev_ms;ms+=1000) {
        if(last) lat+=36/3.6*(ms-last)/1000.0/111320;
        if(c.gap_ms && ms==c.gap_ms) plan.fixes.push_back(fix(ms,0));
        else plan.fixes.push_back(fix_at(ms,lat));
        last=ms;
    }
    if(last) lat+=36/3.6*(c.prev_ms-last)/1000.0/111320;
    plan.fixes.push_back(fix_at(c.prev_ms,lat,c.prev_kmh,c.prev_heading));
    const double step=(c.prev_kmh+c.gps_kmh)/2/3.6*double(utc_at(c.fix_ms)-utc_at(c.prev_ms))*c.displacement_scale;
    Fix f=fix_at(c.fix_ms,lat+step/111320,c.gps_kmh,c.heading);
    f.hdop=c.hdop; f.utc=uint64_t(int64_t(f.utc)+c.utc_offset);
    plan.fixes.push_back(f);
    run(p,plan);
    CHECK(!p.status().resets);
    return p.beta_gate();
}
static GateCase G(double g,double w,double pk,double ph,double h) { return GateCase(g,w,pk,ph,h); }
static void anchor_gate() {
    CHECK(gate_case(G(36,36,36,0,0))==BETA_GATE_ACCEPTED);
    CHECK(gate_case(G(20,20,15,0,0))==BETA_GATE_ACCEPTED);       // both boundaries
    CHECK(gate_case(G(19,19,36,0,0))==BETA_GATE_SPEED);
    CHECK(gate_case(G(61,61,61,0,0))==BETA_GATE_SPEED);          // validated range ends at 60
    CHECK(gate_case(G(36,36,14,0,0))==BETA_GATE_PREVIOUS);
    CHECK(gate_case(G(36,36,36,0,4))==BETA_GATE_COURSE);
    CHECK(gate_case(G(36,36,36,0,3))==BETA_GATE_ACCEPTED);
    CHECK(gate_case(G(36,36,36,359,1.5))==BETA_GATE_ACCEPTED);   // course wraps at north
    { GateCase c=G(36,36,36,0,0); c.yaw_spike_ms=ANCHOR_MS-910; CHECK(gate_case(c)==BETA_GATE_YAW); }      // inside the last 2 s
    { GateCase c=G(36,36,36,0,0); c.yaw_spike_ms=ANCHOR_MS-3110; CHECK(gate_case(c)==BETA_GATE_ACCEPTED); } // before the last 2 s
    CHECK(gate_case(G(36,41,36,0,0))==BETA_GATE_WHEEL);
    CHECK(gate_case(G(36,39.5,36,0,0))==BETA_GATE_ACCEPTED);
    { GateCase c=G(36,36,36,0,0); c.reverse=1; CHECK(gate_case(c)==BETA_GATE_REVERSE); }    // forward only
    // Pair limit 2.5 s on the receipt clock (2026-10-08, was 2.0 s): 2.6 s
    // is too far apart; 2.2 s with a 2 s utc step is a pair now.
    { GateCase c=G(36,36,36,0,0); c.prev_ms=ANCHOR_MS-2600; CHECK(gate_case(c)==BETA_GATE_PREVIOUS); } // 2.6 s
    { GateCase c=G(36,36,36,0,0); c.prev_ms=ANCHOR_MS-2200; CHECK(gate_case(c)==BETA_GATE_ACCEPTED); } // 2.2 s
    // 3.1: the same utc second is not a pair (it formerly passed: 2410/2510 ms).
    { GateCase c=G(36,36,36,0,0); c.fix_ms=ANCHOR_MS-900; c.prev_ms=ANCHOR_MS-1000;
      CHECK(utc_at(c.fix_ms)==utc_at(c.prev_ms)); CHECK(gate_case(c)==BETA_GATE_UTC); }
    // 3.1: utc step and receipt step must agree (2 s of utc in 0.9 s).
    { GateCase c=G(36,36,36,0,0); c.utc_offset=1; c.fix_ms=ANCHOR_MS-100;
      CHECK(gate_case(c)==BETA_GATE_UTC_MONO); }
    { GateCase c=G(36,36,36,0,0); c.utc_offset=-1; CHECK(gate_case(c)==BETA_GATE_UTC); }
    // 3.2: HDOP <= 3.0.
    { GateCase c=G(36,36,36,0,0); c.hdop=3.0; CHECK(gate_case(c)==BETA_GATE_ACCEPTED); }
    { GateCase c=G(36,36,36,0,0); c.hdop=3.1; CHECK(gate_case(c)==BETA_GATE_HDOP); }
    { GateCase c=G(36,36,36,0,0); c.hdop=0; CHECK(gate_case(c)==BETA_GATE_HDOP); }       // unknown
    { GateCase c=G(36,36,36,0,0); c.hdop=4.4; CHECK(gate_case(c)==BETA_GATE_HDOP); }     // the stored no-fix HDOP
    // 3.2: 10 s of consecutive increasing fixes (on both clocks).
    { GateCase c=G(36,36,36,0,0); c.streak_from_ms=1410; CHECK(gate_case(c)==BETA_GATE_ACCEPTED); }
    { GateCase c=G(36,36,36,0,0); c.streak_from_ms=2410; CHECK(gate_case(c)==BETA_GATE_SETTLING); }
    // GPS return: a mode-0 callback inside the run restarts the wait.
    { GateCase c=G(36,36,36,0,0); c.gap_ms=4410; CHECK(gate_case(c)==BETA_GATE_SETTLING); }
    // 3.2: pair displacement within [0.5, 1.5] x v*dt.
    { GateCase c=G(36,36,36,0,0); c.displacement_scale=0.51; CHECK(gate_case(c)==BETA_GATE_ACCEPTED); }
    { GateCase c=G(36,36,36,0,0); c.displacement_scale=1.49; CHECK(gate_case(c)==BETA_GATE_ACCEPTED); }
    { GateCase c=G(36,36,36,0,0); c.displacement_scale=0.49; CHECK(gate_case(c)==BETA_GATE_DISPLACEMENT); }
    { GateCase c=G(36,36,36,0,0); c.displacement_scale=1.51; CHECK(gate_case(c)==BETA_GATE_DISPLACEMENT); }
    // A single forward message (first of the epoch) is enough (3.4).
    { GateCase c=G(36,36,36,0,0); c.reverse_exit=false; CHECK(gate_case(c)==BETA_GATE_ACCEPTED); }
    // Every evaluation is recorded for the journal, rejections included.
    { Pipeline p; GateCase c=G(36,36,36,0,0); c.hdop=3.1; CHECK(gate_case(c,&p)==BETA_GATE_HDOP);
      const uint64_t last=p.beta_anchor_sequence();
      CHECK(last==12); // 11 run fixes + the tested one
      BetaAnchorRecord r;
      CHECK(p.beta_anchor_record(last,&r) && r.gate==BETA_GATE_HDOP && r.hdop==3.1 && r.mode==1);
      CHECK(r.utc_s==utc_at(ANCHOR_MS) && std::fabs(r.streak_s-11.0)<1e-9);
      CHECK(p.beta_anchor_record(1,&r) && r.gate==BETA_GATE_PREVIOUS);
      CHECK(p.beta_anchor_record(2,&r) && r.gate==BETA_GATE_SETTLING);
      CHECK(!p.beta_anchor_record(0,&r) && !p.beta_anchor_record(last+1,&r));
      CHECK(std::strcmp(beta_anchor_gate_name(BETA_GATE_REVERSE_UNPROVEN),"REVERSE_UNPROVEN")==0); }
}
// The OEM POSITION cadence the hook saw on the first persistent BETA drive
// (beta.3, mono 1852-1878 s): polls about every 1.0 s, the fix utc on its
// own 1 Hz phase, so a poll repeats the previous second and the next jumps
// by 2 s (deltas 1,0,2,0,2,1,1,0,2,...). Synthetic rows built from that
// pattern with 1005 ms poll intervals: a repeat-then-jump pair spans 2010 ms.
struct CadenceRun { unsigned accepted, previous, utc_mono, settling, utc; bool utc_kept_streak; };
static CadenceRun oem_cadence_run(const BetaProfile& profile,const std::vector<int>& deltas,
                                  unsigned interval_ms=1005) {
    Pipeline p; mx5_dr_context x={1,1,1};
    CHECK(p.init_model(research_model_profile(),mx5_dr_default_config(),x,false,false,true));
    CHECK(p.enable_beta(profile));
    Plan plan; plan.boot_reverse();
    uint64_t utc=1700000000ULL; double lat=35;
    unsigned ms=410;
    for(size_t k=0;k<deltas.size();++k) {
        if(k) { utc+=uint64_t(deltas[k]); lat+=36/3.6*deltas[k]/111320; ms+=interval_ms; }
        Fix f=fix(ms); f.utc=utc; f.lat=lat;   // the position belongs to its utc second
        plan.fixes.push_back(f);
    }
    plan.end_ms=ms+300;
    CadenceRun r=CadenceRun(); r.utc_kept_streak=true;
    double last_streak=-1;
    uint64_t seen=0;
    plan.each=[&](Pipeline& q,unsigned){
        for(uint64_t seq=seen+1;seq<=q.beta_anchor_sequence();++seq) {
            BetaAnchorRecord a; CHECK(q.beta_anchor_record(seq,&a));
            if(seq>1) {
                if(a.gate==BETA_GATE_ACCEPTED)++r.accepted;
                if(a.gate==BETA_GATE_PREVIOUS)++r.previous;
                if(a.gate==BETA_GATE_UTC_MONO)++r.utc_mono;
                if(a.gate==BETA_GATE_SETTLING)++r.settling;
            }
            if(a.gate==BETA_GATE_UTC) {
                ++r.utc;
                // A repeated second neither resets nor restarts the streak.
                if(!(std::isfinite(a.streak_s) && a.streak_s>last_streak)) r.utc_kept_streak=false;
            }
            if(std::isfinite(a.streak_s)) last_streak=a.streak_s;
            seen=seq;
        }
    };
    run(p,plan);
    CHECK(!p.status().resets);
    return r;
}
static void anchor_gate_oem_cadence() {
    const int pattern[]={0,1,0,2,0,2,1,1,0,2,0,2,1,0,2,1,1,0,2,0,2,1,0,2,1,0,2,1};
    const std::vector<int> deltas(pattern,pattern+sizeof pattern/sizeof pattern[0]);
    // The product profile: one run from the first fix, settled after 10 s on
    // both clocks; every later non-repeated fix is an anchor.
    const CadenceRun now=oem_cadence_run(runtime::beta_profile(),deltas);
    std::printf("OEM cadence (2.5 s pair): accepted=%u previous=%u utc_mono=%u settling=%u utc=%u\n",
                now.accepted,now.previous,now.utc_mono,now.settling,now.utc);
    CHECK(now.previous==0 && now.utc_mono==0 && now.utc_kept_streak);
    CHECK(now.utc==9 && now.accepted>=9 && now.settling<=9);
    // The beta.3/beta.4 rule (2.0 s) on the same rows: every repeat-then-jump
    // pair (2010 ms) failed as PREVIOUS and restarted the settle: no anchor.
    BetaProfile old=runtime::beta_profile(); old.fix_pair_max_ns=2000000000ULL;
    const CadenceRun before=oem_cadence_run(old,deltas);
    std::printf("OEM cadence (2.0 s pair, beta.4): accepted=%u previous=%u settling=%u\n",
                before.accepted,before.previous,before.settling);
    CHECK(before.accepted==0 && before.previous==9);
    // A real utc stall still breaks the run: two repeats (3015 ms to the
    // baseline) fail as PREVIOUS and restart the 10 s settle.
    std::vector<int> stall(deltas.begin(),deltas.begin()+13);
    const CadenceRun head=oem_cadence_run(runtime::beta_profile(),stall);
    stall.push_back(0); stall.push_back(0); stall.push_back(3);
    for(int k=0;k<6;++k) stall.push_back(1);
    const CadenceRun stalled=oem_cadence_run(runtime::beta_profile(),stall);
    std::printf("OEM cadence utc stall: accepted %u -> %u, previous=%u settling %u -> %u\n",
                head.accepted,stalled.accepted,stalled.previous,head.settling,stalled.settling);
    CHECK(stalled.previous==1 && stalled.utc==head.utc+2);
    // Nothing after the stall is an anchor: the 6 later fixes settle again.
    CHECK(stalled.accepted==head.accepted && stalled.settling==head.settling+6);
    // A 2 s utc step after only one poll interval (980 ms, as at mono
    // 1144.9 s of the drive) still disagrees with the receipt clock
    // (|2 - 0.98| > 1.0): UTC_MONO, unchanged.
    std::vector<int> skip(deltas.begin(),deltas.begin()+13); skip.push_back(2);
    const CadenceRun skipped=oem_cadence_run(runtime::beta_profile(),skip,980);
    CHECK(skipped.utc_mono==1 && skipped.previous==0);
}
static void anchor_records_ring() {
    // More than the ring capacity of evaluations: old records are reported
    // as unavailable (the journal counts them as dropped), never stale.
    Pipeline p; init(p);
    Plan plan; plan.end_ms=40000; plan.boot_reverse();
    for(unsigned ms=410;ms<=40000;ms+=1000) plan.fixes.push_back(fix(ms));
    run(p,plan);
    const uint64_t last=p.beta_anchor_sequence();
    CHECK(last==40);
    BetaAnchorRecord r;
    CHECK(!p.beta_anchor_record(1,&r) && !p.beta_anchor_record(last-32,&r));
    CHECK(p.beta_anchor_record(last-31,&r) && r.seq==last-31);
    CHECK(p.beta_anchor_record(last,&r) && r.gate==BETA_GATE_ACCEPTED);
}
static void no_reanchor_from_bad_fix() {
    // A later fix whose GPS speed disagrees with the wheels re-anchors the
    // MODEL core but never the BETA core; the BETA clock stays at ANCHOR_MS.
    Pipeline p; init(p);
    Plan plan; plan.end_ms=ANCHOR_MS+3600; plan.boot_reverse();
    double lat=35;
    for(unsigned ms=410;ms<=ANCHOR_MS;ms+=1000) { plan.fixes.push_back(fix_at(ms,lat)); lat+=10.0/111320; }
    lat-=10.0/111320;
    lat+=(36+25)/2/3.6/111320; plan.fixes.push_back(fix_at(ANCHOR_MS+1000,lat,25));
    lat+=25/3.6/111320; plan.fixes.push_back(fix_at(ANCHOR_MS+2000,lat,25));
    plan.fixes.push_back(fix(ANCHOR_MS+2210,0));
    run(p,plan);
    CHECK(p.beta_gate()==BETA_GATE_WHEEL);
    adapter::DrSnapshot s; BetaModelInput in;
    CHECK(publish(p,plan.end_ms,&s,&in)==runtime::CORE_BRIDGE_OK);
    const Diagnostic d=p.diagnostic(T(plan.end_ms));
    CHECK(d.snapshot.model_valid);
    CHECK(std::fabs(in.snapshot.elapsed_s-double(in.snapshot.frontier_ns-T(ANCHOR_MS))/1e9)<1e-9);
    CHECK(std::fabs(d.snapshot.elapsed_s-double(d.snapshot.frontier_ns-T(ANCHOR_MS+2000))/1e9)<1e-9);
    CHECK(in.snapshot.elapsed_s>d.snapshot.elapsed_s+1.9);
}
static void budget_formula_and_limit() {
    // Straight 10 m/s: core budget = e0 + sv t + v (h0 t + k t^2/2) (+ the
    // core's own age term), accuracy adds (v+sv)*0.5 s, and the BETA window
    // closes at 40 m without ever reporting a clamped value.
    Pipeline p; init(p);
    Plan plan=straight(ANCHOR_MS+21000);
    const BetaProfile b=runtime::beta_profile();
    bool closed=false; unsigned published=0, first_closed=0;
    plan.each=[&](Pipeline& q,unsigned ms) {
        if(ms<LOST_MS+200) return;
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
            CHECK(in.rotation_rad==0 && in.rotation_budget_m==0);
            CHECK(s.accuracy_m==in.snapshot.error_budget_m+in.rotation_budget_m+
                  (s.speed_mps+b.speed_error_mps)*lease+
                  s.speed_mps*b.rotation_budget_per_rad*in.rotation_rad*(age+lease));
            CHECK(s.accuracy_m<=40.0);
        } else {
            if(!closed) first_closed=ms;
            closed=true; CHECK(!s.ready && s.accuracy_m==0);
        }
    };
    run(p,plan);
    CHECK(published>100 && closed);
    // 20+0.6t+0.01t^2+5.15 = 40 at t ~ 18.8 s after the anchor.
    CHECK(first_closed>ANCHOR_MS+18000 && first_closed<ANCHOR_MS+19500);
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
    // 3.3 rotation term: the accumulated part plus v*c*R over age and lease.
    { BetaModelInput in=synthetic(20,0.1); in.rotation_rad=1.0; in.rotation_budget_m=5.0;
      in.query_mono_ns=T(1100); in.now_mono_ns=T(1100);
      CHECK(runtime::map_model_publication(in,b,&s)==runtime::CORE_BRIDGE_OK);
      CHECK(std::fabs(s.accuracy_m-(20+5+lease_term+10*b.rotation_budget_per_rad*1.0*(0.1+0.5)))<1e-9);
      in.rotation_budget_m=40-20-lease_term-10*0.1*0.6+1e-6;
      CHECK(runtime::map_model_publication(in,b,&s)==runtime::CORE_BRIDGE_LIMIT && !s.ready);
      in.rotation_rad=::nan("");
      CHECK(runtime::map_model_publication(in,b,&s)==runtime::CORE_BRIDGE_NUMERIC); }
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

    // Pipeline: a steady 0.3 rad/s turn at 20 km/h after the gap withdraws
    // BETA through the heading budget (0.03+0.002t+0.1*rotation > 20 deg near
    // t=10 s) while the position budget, now including the rotation term
    // integral(v*0.1*R), is still below 40 m. (At the former 25 km/h the
    // rotation term reaches the 40 m limit first; 3.3.)
    Pipeline p; init(p);
    Plan plan=straight(ANCHOR_MS+14000,20);
    plan.wheel=[](unsigned){return 20.0;};
    plan.yaw=[](unsigned ms){return ms>=LOST_MS+80?yaw_raw(0.3):STRAIGHT;};
    runtime::CoreBridgeResult last=runtime::CORE_BRIDGE_OK; unsigned withdrawn=0;
    double rotation_budget=0;
    plan.each=[&](Pipeline& q,unsigned ms) {
        if(ms<LOST_MS+200) return;
        adapter::DrSnapshot out; BetaModelInput bin;
        const runtime::CoreBridgeResult r=publish(q,ms,&out,&bin);
        if(r==runtime::CORE_BRIDGE_BEARING&&last==runtime::CORE_BRIDGE_OK) {
            withdrawn=ms; rotation_budget=bin.rotation_budget_m;
            CHECK(bin.snapshot.error_budget_m<35);
            const double tau=double(bin.snapshot.frontier_ns-T(LOST_MS-20))/1e9;
            CHECK(std::fabs(q.beta_rotation_rad()-0.3*tau)<0.01);
            // integral(v*0.1*R) with R=0.3*tau: v*0.1*0.3*tau^2/2.
            CHECK(std::fabs(bin.rotation_budget_m-20/3.6*0.1*0.3*tau*tau/2)<0.3);
        }
        if(r==runtime::CORE_BRIDGE_OK) CHECK(out.accuracy_m<=40);
        if(last==runtime::CORE_BRIDGE_BEARING) CHECK(r!=runtime::CORE_BRIDGE_OK);
        last=r;
    };
    run(p,plan);
    CHECK(withdrawn>ANCHOR_MS+9000 && withdrawn<ANCHOR_MS+11000);
    CHECK(rotation_budget>5);
}
// Tunnel mode (v1.0.0-beta.6): the honest budget and heading are computed and
// reported separately, the sent accuracy is clamped at accuracy_max_m and the
// bounded profile still refuses.
static void unbounded_profile_clamps_reported_accuracy() {
    const BetaProfile t=runtime::beta_profile_tunnel(),b=runtime::beta_profile();
    CHECK(t.unbounded && !b.unbounded && t.accuracy_max_m==40.0);
    const double lease_term=(10+t.speed_error_mps)*0.5;
    adapter::DrSnapshot s; double honest=-1;
    // Honest budget 400 m, heading 1.0 rad (57 deg): published at 40 m.
    CHECK(runtime::map_model_publication(synthetic(400-lease_term,1.0),t,&s,&honest)==runtime::CORE_BRIDGE_OK);
    CHECK(s.ready && s.beta && s.accuracy_m==40.0 && std::fabs(honest-400.0)<1e-9);
    CHECK(runtime::map_model_publication(synthetic(400-lease_term,1.0),b,&s,&honest)==runtime::CORE_BRIDGE_LIMIT);
    CHECK(!s.ready && s.accuracy_m==0 && std::fabs(honest-400.0)<1e-9);
    // Below the clamp the honest value is reported unchanged (never raised).
    CHECK(runtime::map_model_publication(synthetic(12-lease_term,0.1),t,&s,&honest)==runtime::CORE_BRIDGE_OK);
    CHECK(std::fabs(s.accuracy_m-12.0)<1e-9 && std::fabs(honest-12.0)<1e-9);
    // Non-finite and non-model inputs are still refused in tunnel mode.
    BetaModelInput in=synthetic(25,0.1); in.snapshot.domain=MX5_DR_QUALIFIED_DOMAIN;
    CHECK(runtime::map_model_publication(in,t,&s,&honest)==runtime::CORE_BRIDGE_UNQUALIFIED && !s.ready);
    in=synthetic(25,0.1); in.rotation_rad=::nan("");
    CHECK(runtime::map_model_publication(in,t,&s,&honest)==runtime::CORE_BRIDGE_NUMERIC);
    // The sanity caps are not behavioural limits but still exist.
    BetaProfile bad=t; bad.duration_max_s=21601.0;
    CHECK(runtime::map_model_publication(synthetic(25,0.1),bad,&s)==runtime::CORE_BRIDGE_LIMIT);
    // Core configuration: tunnel mode requests the extended limits.
    CHECK(runtime::beta_core_config(t).extended_limits==1 && runtime::beta_core_config(b).extended_limits==0);
}
// The continuous policy replaces the b1ec29a stale-anchor entry veto with an
// entry decision at the GPS loss (review M3, 2026-10-09): FRESH when the last
// accepted position is at most BETA_POSITION_MAX_AGE_NS old; otherwise FALLBACK
// from the dead-reckoned estimate only inside the bounded envelope (60 s since
// that position, honest budget <= 40 m) and when every rejected data-valid fix
// in between agreed with the estimate; otherwise REFUSED (stock outage).
static double angle_error(double a,double b) { return std::fabs(std::remainder(a-b,360.0)); }
static bool last_entry(const Pipeline& p,BetaAnchorRecord* out) {
    for(uint64_t q=p.beta_anchor_sequence();q>0;--q)
        if(p.beta_anchor_record(q,out)&&(out->gate==BETA_GATE_ENTRY_FRESH||
           out->gate==BETA_GATE_ENTRY_FALLBACK||out->gate==BETA_GATE_ENTRY_REFUSED)) return true;
    return false;
}
static void stale_position_does_not_start_a_tunnel_episode() {
    struct Case { unsigned gap; double offset_m; BetaAnchorGate want; const char* why; };
    const Case cases[]={
        {2000,0,BETA_GATE_ENTRY_FRESH,"fresh"},
        {5000,0,BETA_GATE_ENTRY_FALLBACK,"fallback"},     // HDOP 5 fixes agree with the estimate
        {5000,100,BETA_GATE_ENTRY_REFUSED,"disagree"},    // ... or are 100 m off it
        {30000,0,BETA_GATE_ENTRY_REFUSED,"budget"},       // honest budget > 40 m
        {70000,0,BETA_GATE_ENTRY_REFUSED,"age"}};         // older than 60 s
    for(unsigned g=0;g<sizeof cases/sizeof cases[0];++g) {
        const Case& c=cases[g];
        Pipeline p; init(p,true,false); CHECK(p.enable_beta(runtime::beta_profile_tunnel()));
        Plan plan; plan.boot_reverse();
        for(unsigned ms=410;ms<=ANCHOR_MS;ms+=1000) plan.fixes.push_back(fix(ms,1,36));
        for(unsigned ms=ANCHOR_MS+1000;ms<=ANCHOR_MS+c.gap;ms+=1000) {
            Fix f=fix(ms,1,36); f.hdop=5.0; // fails the HDOP check: position ages
            f.lon+=c.offset_m/(111320*std::cos(35*3.14159265358979/180));
            plan.fixes.push_back(f);
        }
        const unsigned lost=ANCHOR_MS+c.gap+210;
        plan.fixes.push_back(fix(lost,0,36));
        plan.end_ms=lost+1600;
        unsigned published=0;
        plan.each=[&](Pipeline& q,unsigned ms) {
            if(ms<lost+200) return;
            adapter::DrSnapshot s;
            if(runtime::map_model_publication(q.model_publication(T(ms)),runtime::beta_profile_tunnel(),&s)==runtime::CORE_BRIDGE_OK) ++published;
        };
        run(p,plan);
        BetaAnchorRecord r; CHECK(last_entry(p,&r));
        if(r.gate!=c.want||std::strcmp(r.entry_reason,c.why)!=0)
            std::fprintf(stderr,"gap %u offset %.0f: %s/%s age %.1f budget %.1f\n",c.gap,c.offset_m,
                         beta_anchor_gate_name(r.gate),r.entry_reason,r.entry_age_s,r.entry_budget_m);
        CHECK(r.gate==c.want && std::strcmp(r.entry_reason,c.why)==0);
        CHECK(std::fabs(r.entry_age_s-(c.gap+210)/1000.0)<0.05);
        CHECK((published>0)==(c.want!=BETA_GATE_ENTRY_REFUSED));
    }
    // A cold outage (never seeded) is journaled as refused/unseeded and stays stock.
    Pipeline cold; init(cold,true,false); CHECK(cold.enable_beta(runtime::beta_profile_tunnel()));
    Plan none; none.boot_reverse(); none.end_ms=3000; none.fixes.push_back(fix(410,1,36)); none.fixes.push_back(fix(1620,0,36));
    run(cold,none);
    BetaAnchorRecord r; CHECK(last_entry(cold,&r) && r.gate==BETA_GATE_ENTRY_REFUSED && std::strcmp(r.entry_reason,"unseeded")==0);
}
// Review M2 (2026-10-09): back-to-back tunnels. The yaw heading and the honest
// budgets are carried across the GPS return, so the first fix that passes the
// pair/displacement/HDOP checks after the return refreshes only the position
// (source "yaw"/"blend", never a new "seed" from the GPS course) and a second
// outage 2 s or more after the return starts FRESH. A 1 s exit has no such fix
// and starts from the dead-reckoned estimate only inside the bounded envelope.
static void back_to_back_tunnels_keep_the_heading() {
    const unsigned exits[]={1,2,3,5};
    for(unsigned i=0;i<4;++i) for(unsigned long_first=0;long_first<2;++long_first) {
        const unsigned g=exits[i];
        Pipeline p; init(p,true,false); CHECK(p.enable_beta(runtime::beta_profile_tunnel()));
        Plan plan=straight();                         // first loss at 11620 ms
        const unsigned back=long_first?71410:16410;  // a 60 s or a 5 s first tunnel
        for(unsigned k=0;k<g;++k) plan.fixes.push_back(fix(back+1000*k,1,36));
        const unsigned lost=back+1000*g-790;
        plan.fixes.push_back(fix(lost,0,36));
        plan.end_ms=lost+2500;
        unsigned published=0, seeds_after_return=0;
        plan.each=[&](Pipeline& q,unsigned ms) {
            if(ms<lost+200) return;
            adapter::DrSnapshot s;
            if(runtime::map_model_publication(q.model_publication(T(ms)),runtime::beta_profile_tunnel(),&s)==runtime::CORE_BRIDGE_OK) {
                ++published; CHECK(angle_error(s.travel_bearing_deg,0)<2.0);
            }
        };
        run(p,plan);
        BetaAnchorRecord r;
        for(uint64_t q=1;q<=p.beta_anchor_sequence();++q)
            if(p.beta_anchor_record(q,&r)&&r.gate==BETA_GATE_ACCEPTED&&r.mono_ns>=T(back)&&
               std::strcmp(r.heading_source,"seed")==0) ++seeds_after_return;
        CHECK(seeds_after_return==0);
        CHECK(last_entry(p,&r));
        if(g>=2) CHECK(r.gate==BETA_GATE_ENTRY_FRESH && published>0);
        else if(!long_first) CHECK(r.gate==BETA_GATE_ENTRY_FALLBACK && published>0);
        else CHECK(r.gate==BETA_GATE_ENTRY_REFUSED && published==0);
    }
}
static void tunnel_entry_has_no_speed_gate() {
    const double speeds[]={0.5,5,19,61,180};
    for(unsigned i=0;i<sizeof speeds/sizeof speeds[0];++i) {
        const double v=speeds[i];
        // A cold start needs a GPS displacement of >= 3 m that confirms the
        // course (observability, not a speed limit): more fixes when slow.
        const unsigned n=3+unsigned(std::ceil(3.0/(v/3.6)));
        Pipeline p; init(p,true,false); CHECK(p.enable_beta(runtime::beta_profile_tunnel()));
        Plan plan; plan.boot_reverse();
        plan.wheel=[v](unsigned){return v;};
        for(unsigned k=0;k<n;++k) plan.fixes.push_back(fix(410+1000*k,1,v));
        const unsigned lost=410+1000*(n-1)+210;
        plan.fixes.push_back(fix(lost,0,v)); plan.end_ms=lost+1900;
        run(p,plan);
        adapter::DrSnapshot s;
        CHECK(runtime::map_model_publication(p.model_publication(T(plan.end_ms)),runtime::beta_profile_tunnel(),&s)==runtime::CORE_BRIDGE_OK);
        CHECK(std::fabs(s.speed_mps-v/3.6)<0.001 && s.beta);
    }
}
// Head-to-head finding 2026-10-09: a wrong first GPS course must not be
// carried for minutes. The course must agree with the GPS displacement to
// seed, and a stable chord-confirmed disagreement reseeds the heading.
static double last_heading(const Pipeline& p) {
    for(uint64_t q=p.beta_anchor_sequence();q>0;--q) {
        BetaAnchorRecord r;
        if(p.beta_anchor_record(q,&r)&&r.gate==BETA_GATE_ACCEPTED) return r.heading_deg;
    }
    return -1;
}
static void tunnel_wrong_course_does_not_stick() {
    // Course 90 deg while the fixes move north: never seeds from it.
    {
        Pipeline p; init(p,true,false); CHECK(p.enable_beta(runtime::beta_profile_tunnel()));
        Plan plan; plan.boot_reverse(); plan.end_ms=8000; plan.wheel=[](unsigned){return 12.0;};
        for(unsigned ms=410;ms<=7410;ms+=1000) plan.fixes.push_back(fix(ms,1,12,90));
        run(p,plan);
        CHECK(p.beta_gate()==BETA_GATE_COURSE && last_heading(p)<0);
    }
    // A 25 deg wrong course passes the chord check and seeds; five consistent
    // true courses reseed (C-style resync, but five fixes, not three).
    {
        Pipeline p; init(p,true,false); CHECK(p.enable_beta(runtime::beta_profile_tunnel()));
        Plan plan; plan.boot_reverse(); plan.end_ms=30000; plan.wheel=[](unsigned){return 30.0;};
        unsigned k=0; double at_bad=-1;
        for(unsigned ms=410;ms<=29410;ms+=1000,++k) plan.fixes.push_back(fix(ms,1,30,k<6?25.0:0.0));
        plan.each=[&](Pipeline& q,unsigned ms){ if(ms==6000) at_bad=last_heading(q); };
        run(p,plan);
        CHECK(angle_error(at_bad,25)<1.0);
        BetaAnchorRecord r; CHECK(p.beta_anchor_record(p.beta_anchor_sequence(),&r));
        CHECK(angle_error(r.heading_deg,0)<1.0);
        bool resynced=false;
        for(uint64_t q=1;q<=p.beta_anchor_sequence();++q)
            if(p.beta_anchor_record(q,&r)&&std::strcmp(r.heading_source,"resync")==0) resynced=true;
        CHECK(resynced);
    }
}
static void tunnel_short_multipath_burst_does_not_resync() {
    // Four fixes whose course AND positions swing 30 deg without yaw (a
    // consistent multipath burst at a tunnel entrance), then the GPS is lost.
    Pipeline p; init(p,true,false); CHECK(p.enable_beta(runtime::beta_profile_tunnel()));
    Plan plan; plan.boot_reverse(); plan.wheel=[](unsigned){return 30.0;};
    for(unsigned ms=410;ms<=20410;ms+=1000) plan.fixes.push_back(fix(ms,1,30,0));
    double lat=fix(20410,1,30).lat, lon=135;
    for(unsigned j=1;j<=4;++j) {
        lat+=30/3.6*std::cos(30*3.14159265358979/180)/111320;
        lon+=30/3.6*std::sin(30*3.14159265358979/180)/(111320*std::cos(35*3.14159265358979/180));
        Fix f=fix_at(20410+1000*j,lat,30,30); f.lon=lon; plan.fixes.push_back(f);
    }
    plan.fixes.push_back(fix(24620,0,30)); plan.end_ms=26000;
    run(p,plan);
    adapter::DrSnapshot s;
    CHECK(runtime::map_model_publication(p.model_publication(T(plan.end_ms)),runtime::beta_profile_tunnel(),&s)==runtime::CORE_BRIDGE_OK);
    CHECK(angle_error(s.travel_bearing_deg,0)<3.0);
}
// trip4 garage: the car reversed for 3 s before the GPS went. A carried
// estimate keeps refreshing its position in reverse (no course use), so the
// 3 s freshness rule does not refuse the entry; a fast latched reverse is
// still a contradiction even though the position keeps refreshing.
static void tunnel_reverse_refreshes_position_only() {
    for(unsigned fast=0;fast<2;++fast) {
        Pipeline p; init(p,true,false); CHECK(p.enable_beta(runtime::beta_profile_tunnel()));
        Plan plan=straight(20000); plan.fixes.pop_back();
        const double v=fast?36.0:5.0;
        plan.reverse.push_back(std::make_pair(12000u,1));
        plan.wheel=[v](unsigned ms){return ms<12000?36.0:v;};
        double lat=fix(ANCHOR_MS).lat;
        for(unsigned ms=12410;ms<=16410;ms+=1000) {
            lat-=v/3.6/111320; plan.fixes.push_back(fix_at(ms,lat,v,180));
        }
        plan.fixes.push_back(fix(16620,0,v));
        unsigned reversed=0;
        plan.each=[&](Pipeline& q,unsigned ms){
            if(ms%1000!=500||ms<12000||ms>16500) return;
            BetaAnchorRecord r;
            if(q.beta_anchor_record(q.beta_anchor_sequence(),&r)&&r.gate==BETA_GATE_ACCEPTED&&
               std::strcmp(r.heading_source,"reverse")==0) ++reversed;
        };
        run(p,plan);
        adapter::DrSnapshot s;
        const bool ok=runtime::map_model_publication(p.model_publication(T(plan.end_ms)),runtime::beta_profile_tunnel(),&s)==runtime::CORE_BRIDGE_OK;
        if(!fast) { CHECK(reversed>=3 && ok); }
        else CHECK(p.beta_reverse_suspect() && !ok);
    }
}
// Mutation guard (review 2026-10-09): a 1 Hz reverse position refresh must not
// restart the 3.4 reverse-contradiction timer. The contradiction must fire
// while the GPS is still present (the reverse fixes keep coming), not only
// after the GPS is lost and the refreshes stop.
static void tunnel_reverse_refresh_keeps_contradiction_timer() {
    Pipeline p; init(p,true,false); CHECK(p.enable_beta(runtime::beta_profile_tunnel()));
    Plan plan=straight(20000); plan.fixes.pop_back();
    plan.reverse.push_back(std::make_pair(12000u,1));      // latch says reverse ...
    plan.wheel=[](unsigned){return 36.0;};                 // ... at 36 km/h: contradiction
    double lat=fix(ANCHOR_MS).lat;
    for(unsigned ms=12410;ms<=18410;ms+=1000) {
        lat-=36/3.6/111320; plan.fixes.push_back(fix_at(ms,lat,36,180));
    }
    bool suspect_with_gps=false; unsigned refreshed_after=0;
    plan.each=[&](Pipeline& q,unsigned ms){
        if(ms==15000) suspect_with_gps=q.beta_reverse_suspect();
        if(ms==18900) {
            BetaAnchorRecord r;
            for(uint64_t s=1;s<=q.beta_anchor_sequence();++s)
                if(q.beta_anchor_record(s,&r)&&r.gate==BETA_GATE_ACCEPTED&&r.mono_ns>T(14600)) ++refreshed_after;
        }
    };
    run(p,plan);
    CHECK(suspect_with_gps);          // fired ~2 s after 12000 ms despite refreshes at 12410/13410/14410
    CHECK(refreshed_after==0);        // a suspected latch is not refreshed again by reverse fixes
}
// Review H1 (2026-10-09): after boot, a reverse then forward drive must not
// seed from a course chord spanning the reverse manoeuvre (the chord from the
// pre-reverse reference points backwards: a heading 180 deg wrong).
static void reverse_then_forward_does_not_seed_backwards() {
    Pipeline p; init(p,true,false); CHECK(p.enable_beta(runtime::beta_profile_tunnel()));
    Plan plan; plan.boot_reverse(); plan.end_ms=13000;
    plan.reverse.push_back(std::make_pair(3000u,1)); plan.reverse.push_back(std::make_pair(8000u,0));
    plan.wheel=[](unsigned ms){return ms<3000?0.0:7.2;};
    const double lat0=35.0;
    for(unsigned ms=410;ms<=2410;ms+=1000) plan.fixes.push_back(fix_at(ms,lat0,0,0));
    double lat=lat0;
    for(unsigned ms=3410;ms<=7410;ms+=1000){ lat-=2.0/111320; plan.fixes.push_back(fix_at(ms,lat,7.2,180)); }
    // Forward (true heading 0); the first course still lags the reverse.
    lat+=0.8/111320; plan.fixes.push_back(fix_at(8410,lat,7.2,180));
    for(unsigned ms=9410;ms<=11410;ms+=1000){ lat+=2.0/111320; plan.fixes.push_back(fix_at(ms,lat,7.2,0)); }
    plan.fixes.push_back(fix(11620,0,7.2));
    run(p,plan);
    BetaAnchorRecord r;
    for(uint64_t q=1;q<=p.beta_anchor_sequence();++q)
        if(p.beta_anchor_record(q,&r)&&r.gate==BETA_GATE_ACCEPTED) CHECK(angle_error(r.heading_deg,0)<30);
    adapter::DrSnapshot s;
    if(runtime::map_model_publication(p.model_publication(T(plan.end_ms)),runtime::beta_profile_tunnel(),&s)==runtime::CORE_BRIDGE_OK)
        CHECK(angle_error(s.travel_bearing_deg,0)<30);
}
static void tunnel_low_speed_course_does_not_replace_carried_heading() {
    Pipeline p; init(p,true,false); CHECK(p.enable_beta(runtime::beta_profile_tunnel()));
    Plan plan=straight(16000);
    plan.fixes.pop_back();
    plan.wheel=[](unsigned ms){return ms<12000?36.0:5.0;};
    Fix low=fix_at(12410,fix(ANCHOR_MS).lat+5.0/111320,5,150);
    plan.fixes.push_back(low); plan.fixes.push_back(fix(12620,0,5));
    run(p,plan);
    adapter::DrSnapshot s;
    CHECK(runtime::map_model_publication(p.model_publication(T(plan.end_ms)),runtime::beta_profile_tunnel(),&s)==runtime::CORE_BRIDGE_OK);
    CHECK(std::fabs(s.latitude_deg-low.lat)<10.0/111320); // recent position, not old fix
    CHECK(s.travel_bearing_deg<5 || s.travel_bearing_deg>355); // reject large low-speed innovation
}
static void tunnel_multipath_course_step_keeps_yaw() {
    Pipeline p; init(p,true,false); CHECK(p.enable_beta(runtime::beta_profile_tunnel()));
    Plan plan=straight(15000); plan.fixes.pop_back();
    plan.wheel=[](unsigned ms){return ms<12000?36.0:21.0;};
    // Trip4-like entrance: course jumps ~18 degrees while yaw stays quiet.
    // Position still updates, but one suspect fix must not drag heading 3 deg.
    plan.fixes.push_back(fix_at(12410,fix(ANCHOR_MS).lat+6.0/111320,21,18));
    plan.fixes.push_back(fix(12620,0,21)); run(p,plan);
    adapter::DrSnapshot s;
    CHECK(runtime::map_model_publication(p.model_publication(T(plan.end_ms)),runtime::beta_profile_tunnel(),&s)==runtime::CORE_BRIDGE_OK);
    CHECK(s.travel_bearing_deg<1.0);
}
// Review M1 (2026-10-09): a GPS course step the yaw does not show (the
// positions stay straight, so the chord check alone accepts a 12 deg step)
// must not be blended into the carried heading.
static void course_step_without_yaw_turn_is_not_blended() {
    Pipeline p; init(p,true,false); CHECK(p.enable_beta(runtime::beta_profile_tunnel()));
    Plan plan=straight(14000); plan.fixes.pop_back();
    plan.fixes.push_back(fix(12410,1,36,12));
    plan.fixes.push_back(fix(12620,0,36)); run(p,plan);
    adapter::DrSnapshot s;
    CHECK(runtime::map_model_publication(p.model_publication(T(plan.end_ms)),runtime::beta_profile_tunnel(),&s)==runtime::CORE_BRIDGE_OK);
    CHECK(angle_error(s.travel_bearing_deg,0)<0.3);
}
static void tunnel_stationary_fixes_preserve_stop_confirmation() {
    Pipeline p; init(p,true,false); CHECK(p.enable_beta(runtime::beta_profile_tunnel()));
    Plan plan=straight(23000); plan.fixes.pop_back();
    plan.wheel=[](unsigned ms){return ms<12000?36.0:0.0;};
    plan.yaw=[](unsigned ms){return ms<12000?STRAIGHT:yaw_raw(0.01);};
    for(unsigned ms=12410;ms<=20410;ms+=1000)
        plan.fixes.push_back(fix_at(ms,fix(ANCHOR_MS).lat+6.0/111320,0,180));
    plan.fixes.push_back(fix(20620,0,0));
    double held=-1;
    plan.each=[&](Pipeline& q,unsigned ms) {
        if(ms==16000) {
            BetaAnchorRecord r; CHECK(q.beta_anchor_record(q.beta_anchor_sequence(),&r));
            CHECK(r.gate==BETA_GATE_ACCEPTED && std::strcmp(r.heading_source,"yaw")==0);
            held=r.heading_deg;
        }
    };
    run(p,plan);
    adapter::DrSnapshot s;
    CHECK(runtime::map_model_publication(p.model_publication(T(plan.end_ms)),runtime::beta_profile_tunnel(),&s)==runtime::CORE_BRIDGE_OK);
    CHECK(s.speed_mps==0 && held>=0);
    CHECK(std::fabs(s.travel_bearing_deg-held)<0.05);
    // A stationary cold boot has no absolute heading to carry.
    Pipeline cold; init(cold,true,false); CHECK(cold.enable_beta(runtime::beta_profile_tunnel()));
    Plan zero=straight(13000,0); zero.wheel=[](unsigned){return 0.0;}; run(cold,zero);
    CHECK(cold.beta_gate()==BETA_GATE_COURSE);
    CHECK(runtime::map_model_publication(cold.model_publication(T(zero.end_ms)),runtime::beta_profile_tunnel(),&s)!=runtime::CORE_BRIDGE_OK);
}
static void tunnel_keeps_data_validity_and_accepts_turns() {
    for(unsigned c=0;c<6;++c) {
        Pipeline p; init(p,true,false); CHECK(p.enable_beta(runtime::beta_profile_tunnel()));
        // A 5 km/h cold start seeds once the GPS displacement (>= 3 m since
        // the first pair-checked fix) confirms the course: the fifth fix.
        Plan plan; plan.boot_reverse(); plan.end_ms=6000;
        plan.wheel=[](unsigned){return 5.0;};
        plan.yaw=[](unsigned){return yaw_raw(0.2);}; // turning is not a veto
        plan.fixes.push_back(fix(410,1,5,350));
        plan.fixes.push_back(fix(1410,1,5,354));
        plan.fixes.push_back(fix(2410,1,5,356));
        plan.fixes.push_back(fix(3410,1,5,358));
        Fix f=fix(4410,1,5,1);
        if(c==1)f.utc=utc_at(3410); // no new measurement
        if(c==2)f.lat=::nan("");
        if(c==3)f.hdop=5;
        if(c==4)f.lat+=0.01; // gross jump, not low-speed jitter
        if(c==5)plan.reverse.clear();
        plan.fixes.push_back(f); plan.fixes.push_back(fix(4620,0,5));run(p,plan);
        adapter::DrSnapshot s;
        const bool ok=runtime::map_model_publication(p.model_publication(T(plan.end_ms)),runtime::beta_profile_tunnel(),&s)==runtime::CORE_BRIDGE_OK;
        CHECK(ok==(c==0));
        if(ok)CHECK(s.travel_bearing_deg>10 && s.travel_bearing_deg<25);
    }
}
static void standstill_in_a_tunnel_episode_keeps_a_bearing() {
    Pipeline p; init(p,true,false); CHECK(p.enable_beta(runtime::beta_profile_tunnel()));
    Plan plan=straight(ANCHOR_MS+9000);
    plan.wheel=[](unsigned ms){return ms<ANCHOR_MS+3000?36.0:0.0;};
    unsigned moving=0,still=0,dropped=0;
    plan.each=[&](Pipeline& q,unsigned ms) {
        if(ms<LOST_MS+200) return;
        adapter::DrSnapshot s;
        if(runtime::map_model_publication(q.model_publication(T(ms)),runtime::beta_profile_tunnel(),&s)!=
           runtime::CORE_BRIDGE_OK) { if(ms>ANCHOR_MS+3000+300) ++dropped; return; }
        uint8_t out[48];
        CHECK(adapter::encode_location(s,out));
        CHECK(out[40]==1);                       // bearing present, also at standstill
        if(s.speed_mps>0.0) ++moving; else { ++still; CHECK(s.accuracy_m>0 && s.accuracy_m<=40.0); }
    };
    run(p,plan);
    // The stop-confirmation wait (speed 0, not yet stopped) and the stopped state both publish.
    CHECK(moving>0 && still>0 && dropped==0);
}
// Stopped accuracy (validation/STOPPED_ACCURACY_2026-10-10.md; owner decision
// from DHU experiment 9 Tier 1: tunnel profile A*=25 m, H_max=150 m, crawl
// off). The neutral values (beta_profile()) change nothing; with A* set, the
// hold-bearing form (and, with v_c, a crawl) reports min(honest, A*) while the
// honest budget is <= H_max.
static BetaProfile neutral_tunnel() {
    BetaProfile p=runtime::beta_profile_tunnel();
    const BetaProfile b=runtime::beta_profile();
    p.stopped_accuracy_m=b.stopped_accuracy_m; p.stopped_honest_max_m=b.stopped_honest_max_m;
    p.crawl_speed_mps=b.crawl_speed_mps;
    return p;
}
static BetaModelInput still_input(double honest,bool stopped=false) {
    // Speed 0, no bearing: the 1.5 s stop-confirmation wait (or the stopped
    // state). Lease term (0+sv)*0.5.
    BetaModelInput in=synthetic(honest-0.3*0.5,0.1);
    in.snapshot.speed_mps=0; in.snapshot.has_bearing=0; in.snapshot.stopped=stopped?1:0;
    return in;
}
static BetaModelInput moving_input(double honest,double v) {
    BetaModelInput in=synthetic(honest-(v+0.3)*0.5,0.1);
    in.snapshot.speed_mps=v;
    return in;
}
static double reported(const BetaModelInput& in,const BetaProfile& p,int* rule,double* honest=0,
                       runtime::CoreBridgeResult want=runtime::CORE_BRIDGE_OK) {
    adapter::DrSnapshot s; double h=-1; *rule=-1;
    const runtime::CoreBridgeResult r=runtime::map_model_publication(in,p,&s,&h,rule);
    CHECK(r==want);
    if(honest)*honest=h;
    if(r!=runtime::CORE_BRIDGE_OK) { CHECK(!s.ready && *rule==runtime::BETA_ACC_RULE_NORMAL); return -1; }
    // The BETA serializer: hasAccuracy and ceil(accuracy*1000) in (0, 40000].
    uint8_t original[48]={0},out[48];
    CHECK(adapter::encode_beta_location(s,original,out) && out[16]==1);
    const uint32_t e3=uint32_t(out[20])|(uint32_t(out[21])<<8)|(uint32_t(out[22])<<16)|(uint32_t(out[23])<<24);
    CHECK(e3>=1 && e3<=40000 && e3==uint32_t(std::ceil(s.accuracy_m*1000.0)));
    return s.accuracy_m;
}
static void stopped_accuracy_rules() {
    int rule; double honest;
    // Production tunnel profile: 25 m while stopped up to an honest 150 m.
    { const BetaProfile t=runtime::beta_profile_tunnel();
      CHECK(t.stopped_accuracy_m==25.0 && t.stopped_honest_max_m==150.0 && t.crawl_speed_mps==0.0 &&
            t.accuracy_max_m==40.0 && runtime::beta_stopped_rule_enabled(t));
      CHECK(reported(still_input(50),t,&rule,&honest)==25.0 && rule==runtime::BETA_ACC_RULE_STOPPED &&
            std::fabs(honest-50)<1e-9);
      CHECK(reported(still_input(150,true),t,&rule)==25.0 && rule==runtime::BETA_ACC_RULE_STOPPED);
      CHECK(reported(still_input(150.5),t,&rule)==40.0 && rule==runtime::BETA_ACC_RULE_NORMAL);
      CHECK(reported(still_input(600,true),t,&rule)==40.0 && rule==runtime::BETA_ACC_RULE_NORMAL);
      CHECK(std::fabs(reported(still_input(21),t,&rule)-21)<1e-9 && rule==runtime::BETA_ACC_RULE_STOPPED);
      // Crawl off: any movement reports the normal min(honest, 40).
      CHECK(reported(moving_input(50,0.0007),t,&rule)==40.0 && rule==runtime::BETA_ACC_RULE_NORMAL);
      CHECK(reported(moving_input(50,0.833),t,&rule)==40.0 && rule==runtime::BETA_ACC_RULE_NORMAL);
      CHECK(std::fabs(reported(moving_input(30,10),t,&rule)-30)<1e-9 && rule==runtime::BETA_ACC_RULE_NORMAL); }
    const BetaProfile d=neutral_tunnel();
    // Neutral (beta_profile()): A* == accuracy_max == 40, no H_max, crawl off; rule disabled.
    CHECK(d.stopped_accuracy_m==40.0 && d.stopped_accuracy_m==d.accuracy_max_m &&
          std::isinf(d.stopped_honest_max_m) && d.crawl_speed_mps==0.0);
    CHECK(!runtime::beta_stopped_rule_enabled(d) && !runtime::beta_stopped_rule_enabled(runtime::beta_profile()));
    CHECK(reported(still_input(50),d,&rule,&honest)==40.0 && rule==runtime::BETA_ACC_RULE_NORMAL &&
          std::fabs(honest-50)<1e-9);
    CHECK(std::fabs(reported(still_input(12),d,&rule)-12)<1e-9 && rule==runtime::BETA_ACC_RULE_NORMAL);
    CHECK(reported(moving_input(50,1.0),d,&rule)==40.0 && rule==runtime::BETA_ACC_RULE_NORMAL);

    BetaProfile a=d; a.stopped_accuracy_m=20.0;     // H_max infinite, crawl off
    CHECK(runtime::beta_stopped_rule_enabled(a));
    // Stopped at an honest budget of 50 m: A* is reported, honest kept.
    CHECK(reported(still_input(50),a,&rule,&honest)==20.0 && rule==runtime::BETA_ACC_RULE_STOPPED &&
          std::fabs(honest-50)<1e-9);
    CHECK(reported(still_input(500,true),a,&rule)==20.0 && rule==runtime::BETA_ACC_RULE_STOPPED);
    // Never raised above the honest budget.
    CHECK(std::fabs(reported(still_input(12),a,&rule)-12)<1e-9 && rule==runtime::BETA_ACC_RULE_STOPPED);
    // Moving: unchanged (40 m clamp or honest); crawl is off.
    CHECK(reported(moving_input(50,10),a,&rule)==40.0 && rule==runtime::BETA_ACC_RULE_NORMAL);
    CHECK(reported(moving_input(50,0.3),a,&rule)==40.0 && rule==runtime::BETA_ACC_RULE_NORMAL);
    CHECK(std::fabs(reported(moving_input(30,10),a,&rule)-30)<1e-9);

    // H_max (rule D): above it the normal 40 m clamp stays.
    BetaProfile h=a; h.stopped_honest_max_m=189.0;
    CHECK(reported(still_input(189),h,&rule)==20.0 && rule==runtime::BETA_ACC_RULE_STOPPED);
    CHECK(reported(still_input(189.5),h,&rule)==40.0 && rule==runtime::BETA_ACC_RULE_NORMAL);
    h.stopped_honest_max_m=40.0;
    CHECK(reported(still_input(39),h,&rule)==20.0 && reported(still_input(41),h,&rule)==40.0);
    h.stopped_honest_max_m=0.0;
    CHECK(reported(still_input(25),h,&rule)==25.0 && rule==runtime::BETA_ACC_RULE_NORMAL);

    // Crawl (rule C): 0 < speed < v_c, also subject to H_max.
    BetaProfile c=a; c.crawl_speed_mps=4.7;
    CHECK(reported(moving_input(50,0.833),c,&rule)==20.0 && rule==runtime::BETA_ACC_RULE_CRAWL);
    CHECK(reported(moving_input(50,4.69),c,&rule)==20.0 && rule==runtime::BETA_ACC_RULE_CRAWL);
    CHECK(reported(moving_input(50,4.7),c,&rule)==40.0 && rule==runtime::BETA_ACC_RULE_NORMAL);
    CHECK(reported(moving_input(50,14),c,&rule)==40.0 && rule==runtime::BETA_ACC_RULE_NORMAL);
    CHECK(reported(still_input(50),c,&rule)==20.0 && rule==runtime::BETA_ACC_RULE_STOPPED);
    c.stopped_honest_max_m=100.0;
    CHECK(reported(moving_input(150,1),c,&rule)==40.0 && rule==runtime::BETA_ACC_RULE_NORMAL);
    { adapter::DrSnapshot s; double hh; int rr;
      CHECK(runtime::map_model_publication(moving_input(50,1),c,&s,&hh,&rr)==runtime::CORE_BRIDGE_OK);
      CHECK(s.travel_bearing_deg==0.0 && s.speed_mps==1.0 && !s.stopped); }

    // Clamps and profile validation.
    BetaProfile x=a; x.stopped_accuracy_m=60.0;             // above accuracy_max: rule disabled
    CHECK(!runtime::beta_stopped_rule_enabled(x));
    CHECK(reported(still_input(50),x,&rule)==40.0 && rule==runtime::BETA_ACC_RULE_NORMAL);
    x.stopped_accuracy_m=40.0;                              // equal: disabled (the default)
    CHECK(reported(still_input(50),x,&rule)==40.0 && rule==runtime::BETA_ACC_RULE_NORMAL);
    x.stopped_accuracy_m=5.0;                               // below the 20 m floor: expressible (owner decision)
    CHECK(reported(still_input(50),x,&rule)==5.0);
    x.stopped_accuracy_m=0.5;                               // still a valid (0,40] payload
    CHECK(reported(still_input(50),x,&rule)==0.5);
    const double bad_a[]={0.0,-1.0,::nan(""),HUGE_VAL};
    for(unsigned i=0;i<4;++i) { x=a; x.stopped_accuracy_m=bad_a[i];
        reported(still_input(50),x,&rule,0,runtime::CORE_BRIDGE_LIMIT); }
    x=a; x.stopped_honest_max_m=-1.0; reported(still_input(50),x,&rule,0,runtime::CORE_BRIDGE_LIMIT);
    x=a; x.stopped_honest_max_m=::nan(""); reported(still_input(50),x,&rule,0,runtime::CORE_BRIDGE_LIMIT);
    x=a; x.crawl_speed_mps=-0.1; reported(still_input(50),x,&rule,0,runtime::CORE_BRIDGE_LIMIT);
    x=a; x.crawl_speed_mps=HUGE_VAL; reported(still_input(50),x,&rule,0,runtime::CORE_BRIDGE_LIMIT);
    // Other refusals keep rule NORMAL.
    { BetaModelInput in=still_input(50); in.snapshot.domain=MX5_DR_QUALIFIED_DOMAIN;
      reported(in,a,&rule,0,runtime::CORE_BRIDGE_UNQUALIFIED); }
    // The bounded profile never under-reports (and never sends the hold form).
    BetaProfile b=runtime::beta_profile(); b.stopped_accuracy_m=20.0; b.crawl_speed_mps=4.7;
    CHECK(!runtime::beta_stopped_rule_enabled(b));
    CHECK(std::fabs(reported(still_input(30,true),b,&rule)-30)<1e-9 && rule==runtime::BETA_ACC_RULE_NORMAL);
    CHECK(std::fabs(reported(moving_input(30,1),b,&rule)-30)<1e-9 && rule==runtime::BETA_ACC_RULE_NORMAL);
    reported(still_input(30),b,&rule,0,runtime::CORE_BRIDGE_BEARING);
}
// Review H1 (2026-10-10): the honest budget keeps growing at standstill
// (sv*t), so the H_max decision is latched at the first hold-bearing
// publication of a stop and kept for the whole stop.
static double latched(const BetaModelInput& in,const BetaProfile& p,runtime::BetaStopLatch* l,int* rule) {
    adapter::DrSnapshot s; double h; *rule=-1;
    CHECK(runtime::map_model_publication(in,p,&s,&h,rule,l)==runtime::CORE_BRIDGE_OK);
    return s.accuracy_m;
}
static void stopped_accuracy_latch() {
    const BetaProfile t=runtime::beta_profile_tunnel();
    int rule;
    // A long stop whose honest budget crosses 150 m keeps 25 m throughout.
    { runtime::BetaStopLatch l;
      CHECK(latched(still_input(125.7),t,&l,&rule)==25.0 && rule==runtime::BETA_ACC_RULE_STOPPED);
      CHECK(l.active && l.allowed && std::fabs(l.honest0_m-125.7)<1e-9 && l.anchor_id==1);
      CHECK(latched(still_input(149.0,true),t,&l,&rule)==25.0);
      CHECK(latched(still_input(150.1,true),t,&l,&rule)==25.0 && rule==runtime::BETA_ACC_RULE_STOPPED);
      CHECK(latched(still_input(900,true),t,&l,&rule)==25.0 && std::fabs(l.honest0_m-125.7)<1e-9);
      // Without the latch the same input would be 40 m (stateless decision).
      CHECK(reported(still_input(150.1,true),t,&rule)==40.0); }
    // A stop that starts above 150 m stays at 40 m for the whole stop.
    { runtime::BetaStopLatch l;
      CHECK(latched(still_input(150.5),t,&l,&rule)==40.0 && rule==runtime::BETA_ACC_RULE_NORMAL);
      CHECK(l.active && !l.allowed && std::fabs(l.honest0_m-150.5)<1e-9);
      CHECK(latched(still_input(30,true),t,&l,&rule)==30.0 && rule==runtime::BETA_ACC_RULE_NORMAL); // honest < 40: honest
      CHECK(latched(still_input(170,true),t,&l,&rule)==40.0 && rule==runtime::BETA_ACC_RULE_NORMAL); }
    // Boundary: 149.9 and exactly 150 start an allowed stop.
    { runtime::BetaStopLatch l;
      CHECK(latched(still_input(149.9),t,&l,&rule)==25.0 && latched(still_input(150.2),t,&l,&rule)==25.0);
      runtime::BetaStopLatch e;
      CHECK(latched(still_input(150.0),t,&e,&rule)==25.0 && e.allowed); }
    // stop -> move -> stop re-evaluates at the second stop.
    { runtime::BetaStopLatch l;
      CHECK(latched(still_input(140),t,&l,&rule)==25.0);
      CHECK(latched(moving_input(145,10),t,&l,&rule)==40.0 && !l.active);
      CHECK(latched(still_input(155),t,&l,&rule)==40.0 && l.active && !l.allowed);
      CHECK(latched(moving_input(160,0.0007),t,&l,&rule)==40.0 && !l.active);   // one wheel tick moves
      CHECK(latched(still_input(100),t,&l,&rule)==25.0 && l.allowed && std::fabs(l.honest0_m-100)<1e-9); }
    // A new core anchor (new outage, GPS or carried return) is a new stop.
    { runtime::BetaStopLatch l;
      CHECK(latched(still_input(160),t,&l,&rule)==40.0 && !l.allowed);
      BetaModelInput in=still_input(60); in.snapshot.anchor_id=2;
      CHECK(latched(in,t,&l,&rule)==25.0 && l.allowed && l.anchor_id==2);
      in=still_input(60); in.snapshot.anchor_id=2; in.snapshot.stopped=1;
      CHECK(latched(in,t,&l,&rule)==25.0); }
    // A refused publication leaves the latch unchanged; the neutral profile never latches.
    { runtime::BetaStopLatch l; CHECK(latched(still_input(100),t,&l,&rule)==25.0);
      BetaModelInput in=still_input(500); in.snapshot.domain=MX5_DR_QUALIFIED_DOMAIN;
      adapter::DrSnapshot s; double h;
      CHECK(runtime::map_model_publication(in,t,&s,&h,&rule,&l)==runtime::CORE_BRIDGE_UNQUALIFIED);
      CHECK(l.active && l.allowed && std::fabs(l.honest0_m-100)<1e-9);
      runtime::BetaStopLatch n;
      CHECK(latched(still_input(100),neutral_tunnel(),&n,&rule)==40.0 && !n.active); }
}
// Pipeline (review H1): a 6 min stop inside a tunnel. The honest budget
// grows past 150 m during the stop; with the controller's latch the whole
// stop stays at 25 m (without it the stop would switch to the rejected 40 m).
static void long_stop_keeps_the_stopped_accuracy() {
    const BetaProfile t=runtime::beta_profile_tunnel();
    Pipeline p; init(p,true,false); CHECK(p.enable_beta(t));
    const unsigned stop=ANCHOR_MS+25000,go=ANCHOR_MS+385000;
    Plan plan=straight(go+5000);
    plan.wheel=[&](unsigned ms){ return ms>=stop&&ms<go?0.0:36.0; };
    runtime::BetaStopLatch latch;
    unsigned still=0,still_25=0,stateless_40=0,after_40=0; double first=-1,last=-1;
    plan.each=[&](Pipeline& q,unsigned ms) {
        if(ms<stop-2000) return;
        const BetaModelInput in=q.model_publication(T(ms));
        adapter::DrSnapshot s; double honest=-1; int rule=-1;
        CHECK(runtime::map_model_publication(in,t,&s,&honest,&rule,&latch)==runtime::CORE_BRIDGE_OK);
        if(s.speed_mps==0.0) {
            ++still; if(first<0) first=honest; last=honest;
            if(s.accuracy_m==25.0 && rule==runtime::BETA_ACC_RULE_STOPPED) ++still_25;
            adapter::DrSnapshot u; int r2;
            if(runtime::map_model_publication(in,t,&u,0,&r2)==runtime::CORE_BRIDGE_OK && u.accuracy_m==40.0) ++stateless_40;
        } else if(ms>=go) { CHECK(s.accuracy_m==40.0 && !latch.active); ++after_40; }
    };
    run(p,plan);
    if(!(first>40 && first<150 && last>150)) std::fprintf(stderr,"long stop honest %.1f -> %.1f\n",first,last);
    CHECK(first>40 && first<150 && last>150);
    CHECK(still>3500 && still_25==still && stateless_40>0 && after_40>30);
}
// Pipeline: 40 m while moving, A* from the stop (including the 1.5 s
// confirmation wait), back to 40 m at the restart; repeated stops; H_max.
static void stopped_accuracy_switches_at_stop_and_restart() {
    struct Variant { double a, hmax, crawl; bool stop_rule; };
    // 0 neutral, 1 A*=20, 2 H_max below the honest budget, 3 crawl, 4 production.
    const Variant variants[]={{40,HUGE_VAL,0,false},{20,HUGE_VAL,0,true},{25,30,0,false},{20,HUGE_VAL,2.0,true},
                              {25,150,0,true}};
    for(unsigned v=0;v<5;++v) {
        BetaProfile prof=neutral_tunnel();
        prof.stopped_accuracy_m=variants[v].a; prof.stopped_honest_max_m=variants[v].hmax;
        prof.crawl_speed_mps=variants[v].crawl;
        if(v==4) { const BetaProfile t=runtime::beta_profile_tunnel();
            CHECK(t.stopped_accuracy_m==prof.stopped_accuracy_m && t.stopped_honest_max_m==prof.stopped_honest_max_m &&
                  t.crawl_speed_mps==prof.crawl_speed_mps); prof=t; }
        double honest_max=0;
        Pipeline p; init(p,true,false); CHECK(p.enable_beta(prof));
        const unsigned stop1=ANCHOR_MS+25000,go1=ANCHOR_MS+35000,stop2=ANCHOR_MS+40000,go2=ANCHOR_MS+45000;
        Plan plan=straight(ANCHOR_MS+50000);
        // 36 km/h with two stops; a 3 km/h crawl between the stops.
        plan.wheel=[&](unsigned ms){ return (ms>=stop1&&ms<go1)||(ms>=stop2&&ms<go2)?0.0:
                                             (ms>=go1+2000&&ms<stop2-1000)?3.0:36.0; };
        unsigned still=0,still_a=0,moving_after=0,crawl_a=0,fast_40=0,dropped=0;
        int last_rule=-1; unsigned switches=0;
        plan.each=[&](Pipeline& q,unsigned ms) {
            if(ms<ANCHOR_MS+20000) return;   // honest budget > 40 m from about 19 s
            adapter::DrSnapshot s; double honest=-1; int rule=-1;
            if(runtime::map_model_publication(q.model_publication(T(ms)),prof,&s,&honest,&rule)!=runtime::CORE_BRIDGE_OK) {
                ++dropped; return;
            }
            CHECK(honest>40.0); if(honest>honest_max) honest_max=honest;
            CHECK(s.accuracy_m>0 && s.accuracy_m<=40.0 && s.accuracy_m<=honest+1e-9);
            if(rule!=last_rule) { ++switches; last_rule=rule; }
            if(s.speed_mps==0.0) {
                ++still;
                CHECK(rule==(variants[v].stop_rule?runtime::BETA_ACC_RULE_STOPPED:runtime::BETA_ACC_RULE_NORMAL));
                if(s.accuracy_m==variants[v].a && variants[v].stop_rule) ++still_a;
                if(!variants[v].stop_rule) CHECK(s.accuracy_m==40.0);
            } else if(s.speed_mps<2.0) {
                if(variants[v].crawl>0) { CHECK(rule==runtime::BETA_ACC_RULE_CRAWL && s.accuracy_m==variants[v].a); ++crawl_a; }
                else CHECK(rule==runtime::BETA_ACC_RULE_NORMAL && s.accuracy_m==40.0);
            } else {
                CHECK(rule==runtime::BETA_ACC_RULE_NORMAL && s.accuracy_m==40.0);
                if(s.speed_mps>9.0) ++fast_40;
                if(ms>=go2) ++moving_after;
            }
        };
        run(p,plan);
        if(!(dropped==0 && still>=140 && moving_after>30 && fast_40>100))
            std::fprintf(stderr,"variant %u: dropped %u still %u after %u fast %u crawl %u switches %u\n",
                         v,dropped,still,moving_after,fast_40,crawl_a,switches);
        CHECK(dropped==0 && still>=140 && moving_after>30 && fast_40>100);
        if(variants[v].stop_rule) CHECK(still_a==still);
        if(variants[v].crawl>0) CHECK(crawl_a>=15);
        // Default and H_max variants never leave rule NORMAL; the A* variant
        // switches NORMAL -> STOPPED -> NORMAL twice (plus crawl phases).
        // The production variant stays below its 150 m H_max here.
        if(v==4) CHECK(honest_max<150.0);
        if(!variants[v].stop_rule) CHECK(switches==1);
        else if(variants[v].crawl==0) CHECK(switches==5);
        else CHECK(switches>=5);
    }
}
// G2 investigation 2026-10-09: the yaw window mean is the exact sum/count, not
// the integer-truncated value (a 2048.5 mean must turn the heading).
static void yaw_window_mean_is_exact() {
    Pipeline p; init(p,true,false); CHECK(p.enable_beta(runtime::beta_profile_tunnel()));
    Plan plan=straight(ANCHOR_MS+9000);
    plan.yaw_count=2; plan.yaw=[](unsigned){return 4097u;};   // mean 2048.5, truncated 2048
    double heading=-1;
    plan.each=[&](Pipeline& q,unsigned ms) {
        if(ms<ANCHOR_MS+8000 || heading>=0) return;
        adapter::DrSnapshot s; BetaModelInput in;
        CHECK(publish(q,ms,&s,&in)==runtime::CORE_BRIDGE_OK);
        heading=in.snapshot.body_heading_rad;
    };
    run(p,plan);
    const double expected=0.5*0.000658615*(8.0+0.01);   // 0.5 count * rad/s per count * ~8 s
    // Positive yaw counts turn the heading negative (wraps to just below 2 pi).
    const double turned=std::fabs(heading>3.14159265358979?heading-6.28318530717959:heading);
    CHECK(turned>0.95*expected && turned<1.05*expected);
}
static void reverse_latch_contradiction_withdraws() {
    // 3.4: after a forward anchor the latch reads reverse while the wheels
    // keep 36 km/h: after more than 2 s the BETA core is disabled.
    Pipeline p; init(p);
    Plan plan=straight(ANCHOR_MS+5000);
    plan.reverse.push_back(std::make_pair(LOST_MS+380,1));
    unsigned first_bad=0;
    plan.each=[&](Pipeline& q,unsigned ms) {
        if(ms<LOST_MS+200) return;
        adapter::DrSnapshot s;
        const runtime::CoreBridgeResult r=publish(q,ms,&s);
        if(r!=runtime::CORE_BRIDGE_OK&&!first_bad) first_bad=ms;
    };
    run(p,plan);
    CHECK(p.beta_reverse_suspect());
    CHECK(first_bad>LOST_MS+380+2000 && first_bad<LOST_MS+380+2500);
    adapter::DrSnapshot s;
    CHECK(publish(p,plan.end_ms,&s)!=runtime::CORE_BRIDGE_OK && !s.ready);
    // Slow genuine reverse (10 km/h) is not a contradiction.
    Pipeline q; init(q);
    Plan slow=straight(ANCHOR_MS+5000);
    slow.reverse.push_back(std::make_pair(LOST_MS+380,1));
    slow.wheel=[](unsigned ms){return ms>=LOST_MS+380?10.0:36.0;};
    run(q,slow);
    CHECK(!q.beta_reverse_suspect());
}
static void pending_gps_caps_and_hides() {
    Pipeline p; init(p);
    const unsigned end=ANCHOR_MS+1100;
    run(p,straight(end));
    adapter::DrSnapshot s;
    CHECK(publish(p,end,&s)==runtime::CORE_BRIDGE_OK);
    // A queued GPS return caps the lease; once due it hides the output.
    CHECK(p.enqueue_position(position(fix(end+50),99))==PIPELINE_OK);
    CHECK(publish(p,end,&s)==runtime::CORE_BRIDGE_OK);
    CHECK(s.valid_until_mono_ns==T(end+50)-1);
    BetaModelInput in=p.model_publication(T(end+50));
    CHECK(in.result==MX5_DR_E_NO_SEED && !in.snapshot.model_valid);
}
static void shadow_diagnostic_unchanged_by_beta() {
    // The MODEL core (SHADOW diagnostic) is identical with and without the
    // BETA core for the same input, including a fix the BETA gate rejects.
    Pipeline with; init(with,true,true);
    Pipeline without; init(without,true,false);
    Plan plan; plan.end_ms=9000; plan.boot_reverse();
    plan.yaw=[](unsigned ms){return ms>=5000&&ms<6000?yaw_raw(0.2):STRAIGHT;};
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
    // The MODEL zero stays the research profile (2047): only BETA is fixed at 2048.
    CHECK(research_model_profile().yaw_zero==2047.0 && runtime::beta_profile().yaw_zero==2048.0);
}
static void speed_publication_without_anchor() {
    // BETA_DECISIONS 2: the NO_FIX overlay speed comes from the last drained
    // wheel SPEED event. No GPS fix, anchor or core is needed.
    Pipeline p; init(p);
    Plan plan; plan.end_ms=3000; plan.boot_reverse();
    plan.wheel=[](unsigned ms){return ms<2000?36.0:0.0;};
    std::vector<SpeedPublication> seen(31);
    plan.each=[&](Pipeline& q,unsigned ms){ seen[ms/100]=q.speed_publication(T(ms)); };
    run(p,plan);
    CHECK(!p.diagnostic(T(3000)).snapshot.model_valid); // nothing seeded
    CHECK(seen[15].ok && !seen[15].stopped && std::fabs(seen[15].speed_mps-10)<1e-9);
    CHECK(seen[15].measured_ns<=T(1500) && T(1500)-seen[15].measured_ns<=runtime::beta_profile().lease_ns);
    CHECK(seen[29].ok && seen[29].stopped && seen[29].speed_mps==0);
    // Stale: no newer SPEED within the BETA lease.
    CHECK(!p.speed_publication(T(3000)+600000000ULL).ok);
    // Not before the measurement.
    CHECK(!p.speed_publication(T(0)).ok);
    // Without the BETA core the accessor never answers.
    Pipeline plain; init(plain,true,false);
    run(plain,plan);
    CHECK(!plain.speed_publication(T(1500)).ok && !plain.speed_publication(T(3000)).ok);
    // One stopped wheel with three agreeing moving wheels is a contradiction.
    Pipeline q; init(q);
    Plan one; one.end_ms=1500; one.boot_reverse();
    run(q,one);
    CHECK(q.speed_publication(T(1500)).ok);
    RawEvent w=raw(WHEELS,1600,900000,1);
    w.raw[0]=uint16_t(wheel_raw(0));for(unsigned i=1;i<4;++i)w.raw[i]=uint16_t(wheel_raw(36));
    CHECK(q.enqueue_raw(w)==PIPELINE_OK);
    RawEvent y=raw(YAW,1700,900001,1); y.raw[0]=STRAIGHT;
    CHECK(q.enqueue_raw(y)==PIPELINE_OK);
    q.drain(T(1700));
    const SpeedPublication c=q.speed_publication(T(1700));
    CHECK(!c.ok);
}
static void motion_gap_rule() {
    // Task E (2026-10-05): which input-rejection resets may keep the latch.
    typedef runtime::MotionGapTracker G;
    RawEvent e=raw(WHEELS,0,10,7); uint64_t missing,span;
    {   G g; CHECK(!g.reject(true,RECEIVE_STALE,e,&missing,&span));   // nothing accepted yet
        CHECK(g.accept(e,&missing,&span)==G::NO_GAP);
        RawEvent r=raw(YAW,100,11,7);
        CHECK(g.reject(true,RECEIVE_STALE,r,&missing,&span) && missing==1);
        r=raw(WHEELS,1900,26,7);                                      // 16 missing, 1.9 s
        CHECK(g.reject(true,RECEIVE_SEQUENCE,r,&missing,&span) && missing==16 && span==1900000000ULL);
        RawEvent next=raw(YAW,1950,27,7);
        CHECK(g.accept(next,&missing,&span)==G::KEPT && missing==16); }
    {   G g; CHECK(g.accept(e,&missing,&span)==G::NO_GAP);
        RawEvent r=raw(WHEELS,500,28,7);                              // 18 missing
        CHECK(!g.reject(true,RECEIVE_SEQUENCE,r,&missing,&span) && missing==18); }
    {   G g; CHECK(g.accept(e,&missing,&span)==G::NO_GAP);
        RawEvent r=raw(WHEELS,2100,12,7);                             // > 2 s of data
        CHECK(!g.reject(true,RECEIVE_STALE,r,&missing,&span)); }
    {   G g; CHECK(g.accept(e,&missing,&span)==G::NO_GAP);
        RawEvent r=raw(REVERSE,50,11,7);                              // the change itself is lost
        CHECK(!g.reject(true,RECEIVE_STALE,r,&missing,&span));
        r=raw(WHEELS,50,10,7);
        CHECK(!g.reject(true,RECEIVE_SEQUENCE,r,&missing,&span)); } // rewind/replay
    {   G g; CHECK(g.accept(e,&missing,&span)==G::NO_GAP);
        RawEvent r=raw(WHEELS,50,11,8);                               // another producer epoch
        CHECK(!g.reject(true,RECEIVE_SOURCE_CHANGED,r,&missing,&span));
        CHECK(!g.reject(true,RECEIVE_STALE,r,&missing,&span));
        CHECK(!g.reject(false,RECEIVE_STALE,e,&missing,&span)); }
    {   G g; CHECK(g.accept(e,&missing,&span)==G::NO_GAP);
        RawEvent r=raw(WHEELS,100,11,7);
        CHECK(g.reject(true,RECEIVE_STALE,r,&missing,&span));
        RawEvent late=raw(YAW,2500,30,7);                             // the gap grew: clear on accept
        CHECK(g.accept(late,&missing,&span)==G::TOO_LARGE); }
}
// 2026-10-06 late-arrival tolerance (channel.cpp): a worker stall delivers
// the queued records late, all at once, with their unchanged producer receipt
// times. The pipeline orders and integrates them by that time, so the
// publication after the stall equals the on-time one: the late arrival adds
// nothing to the position error budget. Output freshness stays bounded by
// the lease against now, and an event behind the drain watermark stays LATE.
static void late_arrival_output_stays_bounded() {
    const Plan plan=straight(LOST_MS+400);
    Pipeline p,q; init(p); init(q);
    run(p,plan); run(q,plan);
    const unsigned stall_from=LOST_MS+480,stall_to=LOST_MS+1980;   // 12100..13600 ms
    adapter::DrSnapshot sp=adapter::DrSnapshot(),sq=adapter::DrSnapshot();
    CHECK(publish(p,stall_from-100,&sp)==runtime::CORE_BRIDGE_OK);   // engaged before the stall
    uint64_t pseq=500000,qseq=600000; unsigned call=900;
    std::vector<RawEvent> backlog; std::vector<adapter::Observation> positions;
    for(unsigned ms=stall_from;ms<=stall_to;ms+=100) {
        RawEvent w=raw(WHEELS,ms,0,plan.epoch); for(unsigned i=0;i<4;++i) w.raw[i]=uint16_t(wheel_raw(36));
        RawEvent y=raw(YAW,ms,0,plan.epoch); y.raw[0]=STRAIGHT;
        const adapter::Observation gps=position(fix(ms,0),++call);
        // On time: every 100 ms, drained like the worker tick.
        w.receive_seq=++pseq; CHECK(p.enqueue_raw(w)==PIPELINE_OK);
        y.receive_seq=++pseq; CHECK(p.enqueue_raw(y)==PIPELINE_OK);
        if(ms%1000==620) CHECK(p.enqueue_position(gps)==PIPELINE_OK);
        p.drain(T(ms)-100000000ULL);
        // Stalled worker: nothing received or drained until stall_to.
        w.receive_seq=++qseq; y.receive_seq=++qseq; backlog.push_back(w); backlog.push_back(y);
        if(ms%1000==620) positions.push_back(gps);
    }
    for(size_t i=0;i<positions.size();++i) CHECK(q.enqueue_position(positions[i])==PIPELINE_OK);
    for(size_t i=0;i<backlog.size();++i) {
        const PipelineResult r=q.enqueue_raw(backlog[i]);
        CHECK(r==PIPELINE_OK||r==PIPELINE_WAITING);
    }
    q.drain(T(stall_to)-100000000ULL);
    CHECK(q.status().resets==p.status().resets && q.status().last_received_ns==p.status().last_received_ns);
    CHECK(q.status().last_received_ns==T(stall_to));                 // producer time, not the late receipt
    BetaModelInput ip,iq;
    const runtime::CoreBridgeResult rp=publish(p,stall_to,&sp,&ip),rq=publish(q,stall_to,&sq,&iq);
    CHECK(rp==runtime::CORE_BRIDGE_OK && rq==rp);
    CHECK(sq.frontier_mono_ns==sp.frontier_mono_ns && sq.valid_until_mono_ns==sp.valid_until_mono_ns);
    CHECK(sq.accuracy_m==sp.accuracy_m && sq.latitude_deg==sp.latitude_deg && sq.longitude_deg==sp.longitude_deg);
    CHECK(iq.snapshot.error_budget_m==ip.snapshot.error_budget_m);
    // The newest event is still bounded by the lease against now: with no
    // newer input the same estimate expires, late or not.
    CHECK(publish(q,stall_to+1000,&sq)!=runtime::CORE_BRIDGE_OK && !sq.ready);
    // A record that arrives after the drain watermark passed its time is LATE.
    RawEvent behind=raw(WHEELS,stall_to-300,++qseq,plan.epoch);
    for(unsigned i=0;i<4;++i) behind.raw[i]=uint16_t(wheel_raw(36));
    const uint64_t resets=q.status().resets;
    CHECK(q.enqueue_raw(behind)!=PIPELINE_OK && q.status().resets==resets+1);
}
static void tunnel_zero_wheel_yaw_holds_heading() {
    // Synthetic warm-up/anchor, followed by the trip6 stop/restart interval
    // inputs (no vehicle position). The mean yaw window precedes the first
    // nonzero wheel event by 99.296 ms; reverse is a valid latched 1.
    const BetaProfile profile=runtime::beta_profile_tunnel();
    mx5_dr_config cfg=runtime::beta_core_config(profile);
    mx5_dr_context x={1,1,1};
    mx5_dr_core c;
    CHECK(mx5_dr_init_model(&c,&cfg,x)==MX5_DR_OK);
    mx5_dr_anchor a=mx5_dr_anchor(); a.context=x; a.anchor_id=1; a.position_seq=1;
    a.measured_ns=3356860805736ULL; a.utc_ns=1700000000000000000ULL;
    a.quality=MX5_DR_MODEL; a.heading_error_rad=profile.heading_error_rad;
    a.position_error_m=profile.anchor_error_m;
    CHECK(mx5_dr_seed(&c,&a)==MX5_DR_OK);
    ++x.generation;
    CHECK(mx5_dr_control(&c,MX5_DR_GAP,x,2)==MX5_DR_OK);
    const auto step=[&](uint64_t end,double speed,double rate) {
        mx5_dr_interval i=mx5_dr_interval(); i.context=x;
        i.interval_seq=c.last_interval.interval_seq+1;
        i.start_ns=c.estimate.frontier_ns; i.end_ns=end; i.received_ns=end;
        i.speed_mps=speed; i.yaw_rad_s=rate; i.reverse_active=1;
        i.raw_yaw=2048; i.yaw_count=5;
        mx5_dr_evidence* ev[]={&i.speed,&i.yaw,&i.reverse};
        for(unsigned n=0;n<3;++n) {
            ev[n]->source_id=n+1; ev[n]->source_epoch=1;
            ev[n]->producer_seq=i.interval_seq;
            ev[n]->measured_ns=ev[n]->received_ns=i.start_ns;
            ev[n]->lease_until_ns=i.start_ns+250000000ULL;
            ev[n]->quality=MX5_DR_MODEL; ev[n]->freshness=MX5_DR_MODEL_TIME;
        }
        return i;
    };
    for(unsigned n=0;n<20;++n) {
        const mx5_dr_interval i=step(c.estimate.frontier_ns+100000000ULL,0,0);
        CHECK(mx5_dr_step(&c,&i)==MX5_DR_OK);
    }
    CHECK(c.estimate.stopped && c.estimate.frontier_ns==3358860805736ULL);
    const double heading=c.estimate.body_heading_rad, budget=c.estimate.heading_budget_rad;
    const double rate=(10043.0/5-2048)*research_model_profile().yaw_rad_per_count;
    CHECK(std::fabs(rate-0.025949431)<1e-12);
    mx5_dr_interval i=step(3358960101736ULL,0,rate);
    i.raw_yaw=2008; i.yaw_is_mean=1;
    i.yaw_window_start_ns=3358860805736ULL; i.yaw_window_end_ns=3358962515402ULL;
    i.received_ns=i.yaw.received_ns=i.yaw.measured_ns=i.yaw_window_end_ns;
    // raw wheels [10000,10000,10000,10000], yaw sum/count 10043/5.
    const mx5_dr_result result=mx5_dr_step(&c,&i);
    if(result!=MX5_DR_OK) std::fprintf(stderr,"zero-wheel interval: %s\n",mx5_dr_result_name(result));
    CHECK(result==MX5_DR_OK);
    CHECK(c.estimate.stopped && c.estimate.model_valid && !c.estimate.valid);
    CHECK(c.estimate.speed_mps==0 && !c.estimate.has_bearing);
    CHECK(c.estimate.body_heading_rad==heading && c.estimate.distance_m==0);
    CHECK(c.estimate.heading_budget_rad>=budget+(std::fabs(rate)+cfg.yaw_error_rad_s)*0.099296-1e-12);
    // The same yaw window, now with [10000,10015,10000,10013]. Motion
    // releases the stop immediately and reverse rotates travel bearing by pi.
    mx5_dr_interval moving=i;
    ++moving.interval_seq; moving.start_ns=i.end_ns; moving.end_ns=i.yaw_window_end_ns;
    moving.speed_mps=(0.15+0.13)/4/3.6;
    ++moving.speed.producer_seq;
    moving.speed.measured_ns=moving.speed.received_ns=moving.start_ns;
    moving.speed.lease_until_ns=moving.start_ns+250000000ULL;
    CHECK(mx5_dr_step(&c,&moving)==MX5_DR_OK);
    CHECK(!c.estimate.stopped && c.estimate.has_bearing);
    CHECK(std::fabs(c.estimate.body_heading_rad-heading-rate*0.002413666)<1e-12);
    CHECK(std::fabs(c.estimate.travel_bearing_rad-c.estimate.body_heading_rad-std::acos(-1.0))<1e-12);

    // Exercise the real navigation path as well. Only the tunnel profile
    // opts in; the bounded BETA test below keeps its strict E_FRAME check.
    Pipeline p; init(p); CHECK(p.enable_beta(profile));
    Plan plan=straight(ANCHOR_MS+6000);
    plan.wheel=[](unsigned ms){return ms<LOST_MS+400?36.0:0.0;};
    plan.yaw=[](unsigned ms){return ms<LOST_MS+3000?STRAIGHT:yaw_raw(0.2);};
    run(p,plan);
    CHECK(p.beta_core_failure()==MX5_DR_OK);
    adapter::DrSnapshot out;
    CHECK(runtime::map_model_publication(p.model_publication(T(plan.end_ms)),profile,&out)==runtime::CORE_BRIDGE_OK);
    CHECK(out.speed_mps==0 && out.travel_bearing_deg==0);
    CHECK(p.status().core_result==MX5_DR_E_FRAME); // default SHADOW unchanged
}
// Body heading of the BETA core estimate (degrees, 0..360).
static double live_heading(const Pipeline& p,unsigned ms) {
    return p.model_publication(T(ms)).snapshot.body_heading_rad*180.0/3.14159265358979323846;
}
static void tunnel_restart_with_yaw_leading_the_wheels_stays_engaged() {
    // The 2026-10-10 drive (validation/FRAME_REJECT_RESTART_2026-10-10.md):
    // a tunnel episode, a 10 s stop with all four wheels zero, reverse
    // engaged during the stop, then a start while steering where the yaw
    // rises 300 ms (or 0 ms) before the first wheel pulse. The episode
    // publishes throughout, the lead rotation is held (not integrated) and
    // integration resumes with the first wheel movement.
    for(unsigned lead=0;lead<2;++lead) {
        Pipeline p; init(p,true,false); CHECK(p.enable_beta(runtime::beta_profile_tunnel()));
        const unsigned stop_ms=LOST_MS+2000, go_ms=LOST_MS+12000, yaw_ms=go_ms-(lead?300:0);
        Plan plan=straight(LOST_MS+16000);
        plan.reverse.push_back(std::make_pair(LOST_MS+9000,1));
        plan.wheel=[=](unsigned ms){return ms<stop_ms?36.0:ms<go_ms?0.0:2.0;};
        plan.yaw=[=](unsigned ms){return ms<yaw_ms?STRAIGHT:yaw_raw(0.2);};
        unsigned dropped=0,still=0,moving=0;
        double before=-1,at_go=-1;
        plan.each=[&](Pipeline& q,unsigned ms) {
            if(ms<LOST_MS+300) return;
            if(before<0&&ms>=yaw_ms-200) before=live_heading(q,ms);
            if(at_go<0&&ms>=go_ms-100) at_go=live_heading(q,ms);
            adapter::DrSnapshot s;
            if(runtime::map_model_publication(q.model_publication(T(ms)),runtime::beta_profile_tunnel(),&s)!=
               runtime::CORE_BRIDGE_OK) { ++dropped; return; }
            if(s.speed_mps>0.0) ++moving; else ++still;
        };
        run(p,plan);
        CHECK(dropped==0 && still>90 && moving>20);
        CHECK(p.beta_core_failure()==MX5_DR_OK);
        CHECK(before>=0 && at_go==before);                         // lead held
        CHECK(angle_error(live_heading(p,plan.end_ms),before)>30); // then integrated
    }
}
static void tunnel_standstill_yaw_bias_holds_heading() {
    // A sustained 0.03 rad/s standstill bias for 30 s inside a tunnel
    // episode: no withdrawal, heading held, honest budget grows.
    Pipeline p; init(p,true,false); CHECK(p.enable_beta(runtime::beta_profile_tunnel()));
    const unsigned stop_ms=LOST_MS+2000, bias_ms=LOST_MS+5000;
    Plan plan=straight(bias_ms+30000);
    plan.wheel=[=](unsigned ms){return ms<stop_ms?36.0:0.0;};
    plan.yaw=[=](unsigned ms){return ms<bias_ms?STRAIGHT:yaw_raw(0.03);};
    unsigned dropped=0;
    double held=-1,honest0=-1,honest1=-1,hb0=-1,hb1=-1;
    plan.each=[&](Pipeline& q,unsigned ms) {
        if(ms<LOST_MS+300) return;
        adapter::DrSnapshot s; double honest=0;
        if(runtime::map_model_publication(q.model_publication(T(ms)),runtime::beta_profile_tunnel(),&s,&honest)!=
           runtime::CORE_BRIDGE_OK) { ++dropped; return; }
        const double hb=q.model_publication(T(ms)).snapshot.heading_budget_rad; // core part
        if(held<0&&ms>=bias_ms-100) { held=live_heading(q,ms); honest0=honest; hb0=hb; }
        if(ms>bias_ms) { CHECK(live_heading(q,ms)==held && s.speed_mps==0.0); honest1=honest; hb1=hb; }
    };
    run(p,plan);
    CHECK(dropped==0 && p.beta_core_failure()==MX5_DR_OK && held>=0);
    // The held rotation |yaw|*dt (raw 2002: 0.0303 rad/s) plus the yaw error
    // rate over the 30 s is budgeted.
    const double expected=((2048-2002)*0.000658615+runtime::beta_profile_tunnel().yaw_error_rad_s)*30.0;
    CHECK(honest1>honest0 && std::fabs((hb1-hb0)-expected)<0.03);
    // Bounded BETA keeps the strict stationary guard.
    Pipeline b; init(b);
    run(b,plan);
    CHECK(b.beta_core_failure()==MX5_DR_E_FRAME);
    CHECK(!runtime::beta_core_config(runtime::beta_profile()).hold_stopped_yaw &&
          runtime::beta_core_config(runtime::beta_profile_tunnel()).hold_stopped_yaw==1 &&
          !mx5_dr_default_config().hold_stopped_yaw);
}
static void creeping_turn_is_not_a_frame_fault() {
    // Decision F: after the outage the car stops (all wheels zero, quiet yaw)
    // and then creeps at 1 km/h while turning at 0.2 rad/s. The BETA core
    // leaves its stopped state on the first wheel movement and keeps going;
    // the MODEL core (default config, SHADOW unchanged) fails E_FRAME.
    Pipeline p; init(p);
    Plan plan=straight(ANCHOR_MS+6000);
    plan.wheel=[](unsigned ms){return ms<LOST_MS+400?36.0:ms<LOST_MS+2900?0.0:1.0;};
    plan.yaw=[](unsigned ms){return ms<LOST_MS+3000?STRAIGHT:yaw_raw(0.2);};
    unsigned published_after_creep=0;
    plan.each=[&](Pipeline& q,unsigned ms) {
        adapter::DrSnapshot out;
        if(ms>=LOST_MS+3300&&publish(q,ms,&out)==runtime::CORE_BRIDGE_OK) ++published_after_creep;
    };
    run(p,plan);
    CHECK(published_after_creep>20);
    CHECK(p.beta_core_failure()==MX5_DR_OK);
    CHECK(p.status().core_result==MX5_DR_E_FRAME && !p.diagnostic(T(plan.end_ms)).snapshot.model_valid);
    // All four wheels zero while the yaw turns: the strict freeze still applies.
    Pipeline q; init(q);
    Plan still=straight(ANCHOR_MS+6000);
    still.wheel=[](unsigned ms){return ms<LOST_MS+400?36.0:0.0;};
    still.yaw=[](unsigned ms){return ms<LOST_MS+3000?STRAIGHT:yaw_raw(0.2);};
    run(q,still);
    CHECK(q.beta_core_failure()==MX5_DR_E_FRAME);
    adapter::DrSnapshot out;
    CHECK(publish(q,still.end_ms,&out)!=runtime::CORE_BRIDGE_OK);
    const mx5_dr_config c=runtime::beta_core_config(runtime::beta_profile());
    const mx5_dr_config d=mx5_dr_default_config();
    CHECK(c.stop_enter_mps==0.0 && c.stop_exit_mps==0.0005 && d.stop_enter_mps==0.2 && d.stop_exit_mps==0.5);
}
int main() {
    tunnel_zero_wheel_yaw_holds_heading();
    tunnel_restart_with_yaw_leading_the_wheels_stays_engaged();
    tunnel_standstill_yaw_bias_holds_heading();
    tunnel_multipath_course_step_keeps_yaw();
    course_step_without_yaw_turn_is_not_blended();
    tunnel_keeps_data_validity_and_accepts_turns();
    tunnel_stationary_fixes_preserve_stop_confirmation();
    tunnel_entry_has_no_speed_gate();
    tunnel_low_speed_course_does_not_replace_carried_heading();
    creeping_turn_is_not_a_frame_fault();
    motion_gap_rule();
    late_arrival_output_stays_bounded();
    speed_publication_without_anchor();
    latch_seeds_from_change_only_stream();
    unknown_reverse_never_seeds();
    epoch_change_and_lost_reverse_unseed();
    anchor_gate();
    anchor_gate_oem_cadence();
    anchor_records_ring();
    no_reanchor_from_bad_fix();
    budget_formula_and_limit();
    accuracy_boundary_and_heading_withdrawal();
    unbounded_profile_clamps_reported_accuracy();
    stale_position_does_not_start_a_tunnel_episode();
    tunnel_wrong_course_does_not_stick();
    tunnel_short_multipath_burst_does_not_resync();
    tunnel_reverse_refreshes_position_only();
    tunnel_reverse_refresh_keeps_contradiction_timer();
    reverse_then_forward_does_not_seed_backwards();
    back_to_back_tunnels_keep_the_heading();
    standstill_in_a_tunnel_episode_keeps_a_bearing();
    stopped_accuracy_rules();
    stopped_accuracy_latch();
    long_stop_keeps_the_stopped_accuracy();
    stopped_accuracy_switches_at_stop_and_restart();
    yaw_window_mean_is_exact();
    reverse_latch_contradiction_withdraws();
    pending_gps_caps_and_hides();
    shadow_diagnostic_unchanged_by_beta();
    std::printf("beta tests passed (%u checks)\n",checks);
    return 0;
}
