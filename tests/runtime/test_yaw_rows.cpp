// Yaw-zero data rows of the persistent profile (2026-10-09, yaw_study_log.h,
// validation/YAW_DATA_COLLECTION_2026-10-09.md): yaw_stop, yaw_edge,
// yaw_reinit and the extra log_digest fields, their rules, rate limits,
// row class and size bounds. Synthetic events only; logging, no decisions.
#include "runtime/log_profile.h"
#include "runtime/journal_ring.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using mx5::runtime::PersistentLog;
using mx5::runtime::YawStudyLog;
namespace A=mx5::adapter;
namespace N=mx5::navigation;

static std::vector<std::string> out;
static std::vector<unsigned> classes;
static void emit(void*,const char* row,unsigned cls) { out.push_back(row);classes.push_back(cls); }
static const uint64_t S=1000000000ULL,MS=1000000ULL;
static N::ModelProfile model() {
    N::ModelProfile p={2047.0,-0.000658615,0.01,-100.0,0,1,100000000ULL,10.0,0.15};
    return p;
}
static std::vector<std::string> rows(const char* kind) {
    std::vector<std::string> r;const std::string k=std::string("{\"kind\":\"")+kind+"\"";
    for(size_t i=0;i<out.size();++i)if(!out[i].compare(0,k.size(),k))r.push_back(out[i]);
    return r;
}
static bool has(const std::string& row,const char* needle) {
    if(row.find(needle)!=std::string::npos)return true;
    fprintf(stderr,"missing %s in %s\n",needle,row.c_str());return false;
}
// long long: mono_ns values exceed a 32-bit long on the ARM target.
static long long field(const std::string& row,const char* name) {
    const std::string k=std::string("\"")+name+"\":";
    const size_t at=row.find(k);assert(at!=std::string::npos);
    return strtoll(row.c_str()+at+k.size(),0,10);
}

// A synthetic drive at the vehicle cadence: wheels at t%100 ms == 0, a yaw
// window (count 5) at +50 ms, a worker turn (tick/poll) every 50 ms.
struct Feed {
    PersistentLog log;uint64_t seq,t;bool polls;   // polls: a worker turn per step
    std::vector<unsigned char> storage;
    // phase: offset of the 100 ms wheel/yaw cadence from the history bins.
    explicit Feed(uint64_t phase=0,const N::ModelProfile& m=model())
        :seq(0),t(10*S+phase),polls(true),storage(PersistentLog::WINDOW_BYTES+PersistentLog::ROW_BYTES) {
        log.init(&storage[0],m);log.tick(t,emit,0);
    }
    void wheels(double kmh,double left_right=0) {
        N::RawEvent e=N::RawEvent();e.kind=N::WHEELS;e.epoch=1;e.receive_seq=++seq;e.received_ns=t;
        for(unsigned i=0;i<4;++i)e.raw[i]=uint16_t(lround(kmh*100+10000+((i%2)?-left_right:left_right)*50));
        log.motion(e);
    }
    void yaw(unsigned sum,unsigned count=5) {
        N::RawEvent e=N::RawEvent();e.kind=N::YAW;e.epoch=1;e.receive_seq=++seq;e.received_ns=t;
        e.raw[0]=uint16_t(sum);e.count=uint16_t(count);log.motion(e);
    }
    // seconds of driving at kmh with a yaw mean (counts) per sample.
    void run(double seconds,double kmh,double yaw_mean,double left_right=0) {
        const uint64_t end=t+uint64_t(seconds*1e9+0.5);
        while(t<end) {
            if((t/(50*MS))%2==0)wheels(kmh,left_right);
            else yaw(unsigned(lround(yaw_mean*5)));
            if(polls)log.tick(t,emit,0);
            t+=50*MS;
        }
    }
    void idle(double seconds) {   // worker turns without motion
        const uint64_t end=t+uint64_t(seconds*1e9+0.5);
        while(t<end) { log.tick(t,emit,0);t+=50*MS; }
    }
    void position(A::PositionClass cls,double heading,double kmh,double hacc,uint64_t utc) {
        A::Observation o=A::Observation();o.kind=A::Observation::POSITION;o.position_class=cls;
        o.original_mode=cls==A::POSITION_FIX?1:0;o.mono_ns=t;o.position.utc_seconds=utc;
        o.position.heading_deg=heading;o.position.velocity_kmh=kmh;o.position.horizontal=hacc;
        o.position.latitude_deg=35+utc*1e-5;
        log.observation("{\"kind\":\"position\"}",o,t,emit,0);
    }
};

static void stop_row() {
    out.clear();classes.clear();
    Feed f;
    f.run(3,30,2044);              // earlier: -4 counts at 30 km/h
    f.run(2,20,2050);              // the 2 s before the stop: +2 counts at 20 km/h
    const uint64_t stop=f.t;
    f.run(1,0,2047);               // first second -1
    f.run(2,0,2049);               // then +1
    f.run(5.2,20,2048);            // moving again: merge window + 0.3 s slack not yet over
    assert(rows("yaw_stop").empty());
    f.run(1,20,2048);              // 5.3 s after the last zero wheel event (17.9 s)
    const std::vector<std::string> r=rows("yaw_stop");
    assert(r.size()==1 && field(r[0],"mono_ns")==23200000000LL);
    // Zero wheel events at stop .. stop+2.9 s. Yaw windows within 0.2 s of
    // the first zero event (15.05, 15.15) still hold moving samples and the
    // one after the last zero event (17.95) may: excluded. Left: 8 x -1
    // (15.25-15.95) and 19 x +1 (16.05-17.85) windows of 5 samples.
    assert(has(r[0],"\"dur_ms\":2900,") && has(r[0],"\"yn\":135,") && has(r[0],"\"ys\":55,"));
    // first: 15.2-16.2 s (8 x -1, 2 x +1); last: 16.9-17.9 s; pre: 13.0-15.0 s.
    assert(has(r[0],"\"first\":-60,") && has(r[0],"\"last\":100,") && has(r[0],"\"pre\":200,") &&
           has(r[0],"\"pre_kmh\":20.0"));
    assert(field(r[0],"sd")==91 && r[0].find("pre_bad")==std::string::npos);
    char boot[40];snprintf(boot,sizeof boot,"\"boot_s\":%.1f,",double(stop/(100*MS))/10.0);
    assert(has(r[0],boot) && r[0].find("\"merged\"")==std::string::npos && r[0].find("\"bad\"")==std::string::npos);
    for(size_t i=0;i<out.size();++i)
        if(out[i].find("{\"kind\":\"yaw_")==0)assert(classes[i]==PersistentLog::ROW_RAW);
    // A standstill shorter than 1 s is not an episode.
    out.clear();
    f.run(0.9,0,2040);f.run(10,30,2048);
    assert(rows("yaw_stop").empty());
    puts("yaw rows: standstill episode statistics, merge wait, 1 s minimum and diagnostic class passed");
}

static void merge_rule() {
    out.clear();
    Feed f;
    f.run(3,20,2048);
    f.run(2,0,2046);f.run(3,8,2060);f.run(2,0,2050);   // 3 s apart: merged, moving samples excluded
    f.run(8,20,2048);
    f.run(2,0,2048);f.run(6,8,2048);f.run(2,0,2048);   // 6 s apart: two episodes
    f.run(8,20,2048);
    const std::vector<std::string> r=rows("yaw_stop");
    assert(r.size()==3);
    assert(has(r[0],"\"merged\":2") && has(r[0],"\"yn\":170,") && has(r[0],"\"ys\":0,") &&
           has(r[0],"\"first\":-200,") && has(r[0],"\"last\":200,") && has(r[0],"\"dur_ms\":6900,"));
    assert(r[1].find("merged")==std::string::npos && r[2].find("merged")==std::string::npos);
    puts("yaw rows: episodes closer than 5 s merge (stationary samples only), farther ones do not");
}

static std::string last_digest() {
    const std::vector<std::string> d=rows("log_digest");assert(!d.empty());return d.back();
}

static void digest_fields() {
    out.clear();
    Feed f;   // since = 10 s
    f.idle(10.0);out.clear();                              // digest at 20 s; period from 20 s
    f.run(3,30,2049.2,1.0);                                 // moving: +1.2 per sample, wheel pairs differ
    f.position(A::POSITION_FIX,181.4,31,2.2,100);
    f.position(A::POSITION_FIX,181.4,31,2.2,100);           // same fix again: not new
    f.run(1,30,2048,1.0);
    f.position(A::POSITION_FIX,359.7,12.9,6.5,101);
    f.run(2,0,2045);                                        // stationary
    f.run(4,30,2048,1.0);
    f.idle(0.1);
    const std::string d=last_digest();
    // 10 yaw windows per second, 5 samples each: +6 per window while moving.
    assert(has(d,"\"y1\":[60,60,60,0,0,0,0,0,0,0],\"y1n\":[50,50,50,50,0,0,50,50,50,50]"));
    assert(has(d,"\"yst\":-300,\"ystn\":100,"));
    // Fixes at 3.0 s (course 181) and 4.0 s (course 0 for 359.7).
    assert(has(d,"\"gc\":[30181,40000],\"gq\":7,\"gv\":12"));
    assert(has(d,"\"dw01\":1.00,\"dw23\":1.00}"));
    // The next period starts empty.
    f.idle(10.0);
    const std::string e=last_digest();
    assert(has(e,"\"y1\":[0,0,0,0,0,0,0,0,0,0],\"y1n\":[0,0,0,0,0,0,0,0,0,0],\"yst\":0,\"ystn\":0,\"gc\":[]") &&
           e.find("\"gq\"")==std::string::npos && e.find("\"dw01\"")==std::string::npos);
    puts("yaw rows: digest 1 s moving bins, stationary sum, new-fix courses and wheel pair differences passed");
}

static void edge_rows() {
    out.clear();
    Feed f;
    for(unsigned i=0;i<6;++i) { f.position(A::POSITION_FIX,90+i,40,1.5,200+i);f.run(1,40,2049); }
    f.position(A::POSITION_LOST,0,0,0,0);                   // GPS loss
    f.run(4,40,2047);
    assert(rows("yaw_edge").empty());
    f.run(2,40,2047);
    std::vector<std::string> r=rows("yaw_edge");
    assert(r.size()==1);
    assert(has(r[0],"\"ev\":\"loss\",\"from\":\"FIX\",\"to\":\"LOST\"") && has(r[0],"\"yb\":100,\"nb\":250,\"vb\":40.0,") &&
           has(r[0],"\"cb\":[91,95,40,5]") && has(r[0],"\"ya\":-100,\"na\":250,") && has(r[0],"\"ca\":null,\"cut\":0"));
    // Return, then a loss after 2 s: the first edge is written with a cut window.
    out.clear();
    f.position(A::POSITION_FIX,120,40,1.5,300);f.run(1,40,2048);
    f.position(A::POSITION_FIX,121,40,1.5,301);f.run(1,40,2048);
    f.position(A::POSITION_LOST,0,0,0,0);f.run(6,40,2048);
    r=rows("yaw_edge");
    assert(r.size()==2 && has(r[0],"\"ev\":\"return\",\"from\":\"LOST\",\"to\":\"FIX\"") &&
           has(r[0],"\"ca\":[120,121,10,2],\"cut\":1") && has(r[0],"\"na\":100,") && has(r[1],"\"ev\":\"loss\""));
    puts("yaw rows: GPS loss/return edges with 5 s before/after windows passed");
}

static void reinit_rows() {
    out.clear();
    Feed f;
    f.run(3,20,2048);
    f.run(3,0,2046);f.run(8,20,2048);                       // stop A: -2
    assert(rows("yaw_stop").size()==1);
    // 1.5 s without yaw (wheels continue): reinit trigger.
    const uint64_t end=f.t+1500*MS;
    while(f.t<end) { if((f.t/(50*MS))%2==0)f.wheels(20);f.log.tick(f.t,emit,0);f.t+=50*MS; }
    f.run(5,20,2048);
    assert(rows("yaw_reinit").empty());                     // waits for the next standstill
    f.run(3,0,2051);f.run(8,20,2048);                       // stop B: +3
    std::vector<std::string> r=rows("yaw_reinit");
    assert(r.size()==1);
    assert(has(r[0],"\"cause\":1,\"n\":1,\"gap_ms\":1600,\"still\":0,\"before\":-200,") &&
           has(r[0],"\"after\":300,"));
    // An invalid window (4095 marker) is cause 2 and ends a standstill.
    out.clear();
    f.run(2,0,2048);
    f.yaw(4095*5);f.log.tick(f.t,emit,0);f.t+=50*MS;
    f.log.tick(f.t,emit,0);f.t+=50*MS;                      // back on the wheel/yaw phase
    f.run(2,0,2049);f.run(8,20,2048);
    r=rows("yaw_reinit");
    const std::vector<std::string> stops=rows("yaw_stop");
    assert(r.size()==1 && has(r[0],"\"cause\":2,") && has(r[0],"\"still\":1,") && has(r[0],"\"before\":0,") &&
           has(r[0],"\"after\":100,"));
    assert(stops.size()==2 && has(stops[0],"\"bad\":1"));    // split at the invalid window, never merged
    puts("yaw rows: reinit on a yaw gap or an invalid window with stationary means before/after passed");
}

static void rate_limit_and_flush() {
    out.clear();
    Feed f;
    // Permanent stop-and-go: a 1.5 s standstill every 7 s for 140 s.
    for(unsigned i=0;i<20;++i) { f.run(1.5,0,2048);f.run(5.5,15,2048); }
    f.run(6,15,2048);
    const size_t written=rows("yaw_stop").size();
    // Emissions at 16.7 s + 7 s k (k=0..19): the burst of 3 plus one token
    // per 10 s since 10 s gives 3 + 13 = 16 rows by the last one at 149.7 s.
    assert(written==16);
    f.idle(10);
    long suppressed=0;
    const std::vector<std::string> d=rows("log_digest");
    for(size_t i=0;i<d.size();++i) {
        const size_t at=d[i].find("\"yaw_stop\":");
        if(at!=std::string::npos)suppressed+=atol(d[i].c_str()+at+11);
    }
    assert(written+size_t(suppressed)==20);
    // Capture stop during a standstill: written at once, marked open, before the final digest.
    out.clear();
    f.idle(60);out.clear();
    f.run(4,0,2047);
    f.log.row("{\"kind\":\"capture_end\"}",f.t,emit,0);
    const std::vector<std::string> r=rows("yaw_stop");
    assert(r.size()==1 && has(r[0],"\"open\":1") && has(r[0],"\"first\":-100,"));
    size_t stop_at=0,digest_at=0;
    for(size_t i=0;i<out.size();++i) {
        if(out[i].find("yaw_stop")!=std::string::npos)stop_at=i;
        if(out[i].find("\"digest\":\"final\"")!=std::string::npos)digest_at=i;
    }
    assert(stop_at<digest_at);
    puts("yaw rows: per-kind rate limit with suppressed counts and capture-stop flush passed");
}

static void off_switch_and_class() {
    out.clear();
    Feed f;f.log.set_yaw_rows(false);
    f.position(A::POSITION_FIX,10,30,1,1);f.run(3,30,2048);f.run(3,0,2048);
    f.position(A::POSITION_LOST,0,0,0,0);f.run(20,30,2048);
    for(size_t i=0;i<out.size();++i)assert(out[i].find("{\"kind\":\"yaw_")!=0 && out[i].find("\"y1\"")==std::string::npos);
    // Diagnostic class: the journal never queues a yaw row as evidence.
    const char* kinds[]={"{\"kind\":\"yaw_stop\",\"schema\":1}","{\"kind\":\"yaw_edge\",\"schema\":1}",
                         "{\"kind\":\"yaw_reinit\",\"schema\":1}"};
    for(unsigned i=0;i<3;++i)assert(!mx5::runtime::journal_evidence_row(kinds[i],strlen(kinds[i])));
    puts("yaw rows: off switch writes none; yaw kinds are never journal evidence");
}

static void size_bounds() {
    printf("yaw rows: sizeof(YawStudyLog)=%zu, sizeof(PersistentLog)=%zu bytes\n",sizeof(YawStudyLog),
           sizeof(PersistentLog));
    assert(sizeof(YawStudyLog)<=8192);
    // Extreme values still produce complete rows within their buffers.
    out.clear();
    Feed f;f.t=(UINT64_MAX/2)/S*S;f.log.tick(f.t,emit,0);
    f.run(3,555,4093.8);                                   // top wheel and yaw values
    f.run(3,0,0.2);
    f.position(A::POSITION_FIX,359.99,999999,1e9,7);f.run(1,555,4093.8);
    f.position(A::POSITION_LOST,0,0,0,0);f.run(12,555,4093.8);
    f.log.row("{\"kind\":\"capture_end\"}",f.t,emit,0);
    const std::vector<std::string> s=rows("yaw_stop"),e=rows("yaw_edge"),d=rows("log_digest");
    assert(!s.empty() && !e.empty() && !d.empty());
    size_t longest=0;
    for(size_t i=0;i<out.size();++i) {
        if(out[i].find("{\"kind\":\"yaw_")==0 && out[i].size()>longest)longest=out[i].size();
        assert(out[i][out[i].size()-1]=='}');
    }
    assert(longest<YawStudyLog::ROW_CAPACITY);
    assert(has(d.back(),"\"y1\":[") && has(d.back(),"\"gc\":"));
    printf("yaw rows: longest extreme yaw row %zu bytes (capacity %zu); digest %zu bytes\n",longest,
           size_t(YawStudyLog::ROW_CAPACITY),d.back().size());
    puts("yaw rows: memory and row size bounds passed");
}

// pre is null with a reason when its 2 s are not clean driving; a creeping
// car (0.1 km/h, below the product's 0.05 m/s stationary tolerance) is no
// standstill here: these rows require all four wheels at exactly zero.
static void pre_validity_and_creep() {
    out.clear();
    Feed f;
    f.run(1.5,0,2048);f.run(10,20,2048);                    // boot: no history before -> 8
    f.run(5,20,2048);
    f.run(0.5,0,2048);f.run(1.2,10,2048);f.run(2,0,2048);   // a 0.5 s blip 1.2 s before -> 4
    f.run(10,20,2048);
    f.run(1,20,2048);
    { const uint64_t end=f.t+400*MS; while(f.t<end) { if((f.t/(50*MS))%2==0)f.wheels(20);f.log.tick(f.t,emit,0);f.t+=50*MS; } }
    f.run(1,20,2048);f.run(2,0,2048);f.run(10,20,2048);     // a 0.4 s yaw gap 1 s before -> 1
    f.run(3,20,2048);f.run(2,0,2048);f.run(10,20,2048);     // clean
    f.run(5,0.1,2040);f.run(8,20,2048);                      // creeping: no row
    f.run(1,20,2048);f.yaw(4095*5);f.log.tick(f.t,emit,0);f.t+=50*MS;   // a 4095 window 1 s before -> 2
    f.log.tick(f.t,emit,0);f.t+=50*MS;
    f.run(1,20,2048);f.run(2,0,2048);f.run(10,20,2048);
    std::vector<std::string> r=rows("yaw_stop");
    assert(r.size()==5 && has(r[4],"\"pre_bad\":2"));
    r.pop_back();
    assert(has(r[0],"\"pre\":null,\"pre_kmh\":null") && has(r[0],"\"pre_bad\":8"));
    assert(has(r[1],"\"pre\":null,") && has(r[1],"\"pre_bad\":4"));
    assert(has(r[2],"\"pre\":null,") && has(r[2],"\"pre_bad\":1"));
    assert(has(r[3],"\"pre\":0,\"pre_kmh\":20.0") && r[3].find("pre_bad")==std::string::npos);
    // The bin holding the first zero wheel event is not part of pre: with
    // the cadence 30 ms off the bins, it would add that zero event.
    out.clear();
    Feed g(30*MS);
    g.run(3,20,2048);g.run(2,0,2048);g.run(10,20,2048);
    const std::vector<std::string> q=rows("yaw_stop");
    assert(q.size()==1 && has(q[0],"\"pre\":0,\"pre_kmh\":20.0"));
    // Wheel differences use the model wheel scale (0.02 km/h per count here).
    out.clear();
    N::ModelProfile coarse=model();coarse.wheel_kmh_per_count=0.02;coarse.wheel_zero_kmh=-200.0;
    Feed c(0,coarse);
    c.idle(10.0);out.clear();
    for(unsigned i=0;i<100;++i) {
        N::RawEvent e=N::RawEvent();e.kind=N::WHEELS;e.epoch=1;e.receive_seq=++c.seq;e.received_ns=c.t;
        e.raw[0]=11525;e.raw[1]=11475;e.raw[2]=11510;e.raw[3]=11490;   // 30 km/h, 1.0 and 0.4 km/h apart
        c.log.motion(e);c.log.tick(c.t,emit,0);c.t+=100*MS;
    }
    c.idle(0.1);
    assert(has(rows("log_digest").back(),"\"dw01\":1.00,\"dw23\":0.40"));
    puts("yaw rows: pre-stop window validity (boot, earlier standstill, gap, reinit, start bin), creeping case "
         "and model wheel scale passed");
}

// Events in a burst without worker turns (no poll between two standstills):
// the merge window is still applied in end_run(); a late edge close marks
// the lost front of its after window.
static void without_polls() {
    out.clear();
    Feed f;f.polls=false;
    f.run(3,20,2048);f.run(2,0,2046);f.run(6,20,2048);f.run(2,0,2050);f.run(8,20,2048);
    f.log.tick(f.t,emit,0);
    const std::vector<std::string> r=rows("yaw_stop");
    assert(r.size()==2 && r[0].find("merged")==std::string::npos && has(r[0],"\"first\":-200,") &&
           has(r[1],"\"first\":200,"));
    out.clear();
    f.polls=true;f.position(A::POSITION_FIX,10,20,1,1);f.run(2,20,2048);
    f.position(A::POSITION_LOST,0,0,0,0);
    f.polls=false;f.run(8,20,2048);f.log.tick(f.t,emit,0);
    const std::vector<std::string> e=rows("yaw_edge");
    assert(e.size()==1 && has(e[0],"\"hist_lost\":1"));
    puts("yaw rows: merge window without polls and late edge close flag passed");
}

// The yaw digest fields give way before the digest itself is lost.
static void digest_overflow() {
    char line[64];
    strcpy(line,"{\"kind\":\"log_digest\",\"n\":1");
    const size_t n=strlen(line);
    size_t m=PersistentLog::close_digest(line,sizeof line,n,",\"y1\":[1,2]");
    assert(m && std::string(line)=="{\"kind\":\"log_digest\",\"n\":1,\"y1\":[1,2]}");
    line[n]=0;
    m=PersistentLog::close_digest(line,sizeof line,n,",\"y1\":[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19]");
    assert(m && std::string(line)=="{\"kind\":\"log_digest\",\"n\":1,\"yaw_dropped\":1}");
    line[n]=0;
    m=PersistentLog::close_digest(line,n+2,n,",\"y1\":[1]");
    assert(m && std::string(line)=="{\"kind\":\"log_digest\",\"n\":1}");
    assert(!PersistentLog::close_digest(line,n+1,n,""));
    puts("yaw rows: digest keeps itself when the yaw fields do not fit passed");
}

int main() {
    stop_row();
    merge_rule();
    digest_fields();
    edge_rows();
    reinit_rows();
    pre_validity_and_creep();
    without_polls();
    digest_overflow();
    rate_limit_and_flush();
    off_switch_and_class();
    size_bounds();
    puts("yaw row tests passed");
    return 0;
}
