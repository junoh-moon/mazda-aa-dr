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
static unsigned wheel_raw(double kmh) { return unsigned(std::lround(kmh*100+10000)); }
// BETA rate = (mean-2048) * -0.000658615 rad/s (fixed BETA zero, rule 3.5).
static const unsigned STRAIGHT=2048;
static unsigned yaw_raw(double rad_s) { return unsigned(std::lround(2048-rad_s/0.000658615)); }
// The straight plans: 1 Hz fixes 410..11410 ms, GPS lost at 11620 ms.
static const unsigned ANCHOR_MS=11410, LOST_MS=11620;

struct Fix { unsigned ms; int mode; double kmh, heading, lat, hdop; uint64_t utc; };
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
    Fix f; f.ms=ms; f.mode=mode; f.kmh=kmh; f.heading=heading; f.hdop=1.0; f.utc=utc_at(ms);
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
    o.position.latitude_deg=f.lat; o.position.longitude_deg=135;
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
      in.rotation_rad=std::nan("");
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
    in=synthetic(25,0.1); in.rotation_rad=std::nan("");
    CHECK(runtime::map_model_publication(in,t,&s,&honest)==runtime::CORE_BRIDGE_NUMERIC);
    // The sanity caps are not behavioural limits but still exist.
    BetaProfile bad=t; bad.duration_max_s=21601.0;
    CHECK(runtime::map_model_publication(synthetic(25,0.1),bad,&s)==runtime::CORE_BRIDGE_LIMIT);
    // Core configuration: tunnel mode requests the extended limits.
    CHECK(runtime::beta_core_config(t).extended_limits==1 && runtime::beta_core_config(b).extended_limits==0);
}
// Review finding (b1ec29a): tunnel mode lifts the in-outage limits only. An
// anchor that integrated through a long GPS-present stretch must not start an
// episode, and a standstill inside a tunnel episode keeps a bearing.
static void stale_anchor_does_not_start_a_tunnel_episode() {
    const unsigned gaps[]={5000,30000,70000};
    for(unsigned g=0;g<3;++g) {
        Pipeline p; init(p,true,false); CHECK(p.enable_beta(runtime::beta_profile_tunnel()));
        Plan plan; plan.boot_reverse();
        for(unsigned ms=410;ms<=ANCHOR_MS;ms+=1000) plan.fixes.push_back(fix(ms,1,36));
        for(unsigned ms=ANCHOR_MS+1000;ms<=ANCHOR_MS+gaps[g];ms+=1000) {
            Fix f=fix(ms,1,36); f.hdop=5.0; plan.fixes.push_back(f); // fails the HDOP gate: anchor ages
        }
        const unsigned lost=ANCHOR_MS+gaps[g]+210;
        plan.fixes.push_back(fix(lost,0,36));
        plan.end_ms=lost+1600;
        unsigned published=0;
        plan.each=[&](Pipeline& q,unsigned ms) {
            if(ms<lost+200) return;
            adapter::DrSnapshot s;
            if(publish(q,ms,&s,0)==runtime::CORE_BRIDGE_OK) ++published;
        };
        run(p,plan);
        if(gaps[g]<=5000) CHECK(published>0); else CHECK(published==0);
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
    stale_anchor_does_not_start_a_tunnel_episode();
    standstill_in_a_tunnel_episode_keeps_a_bearing();
    yaw_window_mean_is_exact();
    reverse_latch_contradiction_withdraws();
    pending_gps_caps_and_hides();
    shadow_diagnostic_unchanged_by_beta();
    std::printf("beta tests passed (%u checks)\n",checks);
    return 0;
}
