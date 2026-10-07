// PersistentLog (log_profile=persistent, 2026-10-06): which rows are kept,
// rate-limited, digested or held in the RAW window, and the order in which an
// event writes the window. Synthetic rows only.
#include "runtime/log_profile.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using mx5::runtime::PersistentLog;
namespace A=mx5::adapter;
namespace N=mx5::navigation;

static std::vector<std::string> out;
static std::vector<bool> raw_flags;
static void emit(void*,const char* row,bool raw) { out.push_back(row);raw_flags.push_back(raw); }
static const uint64_t S=1000000000ULL;
static N::ModelProfile model() {
    N::ModelProfile p={2047.0,-0.000658615,0.01,-100.0,0,1,100000000ULL,10.0,0.15};
    return p;
}
static std::string row(const char* kind,const char* extra="") {
    return std::string("{\"kind\":\"")+kind+"\""+extra+"}";
}
static size_t count(const char* kind) {
    size_t n=0;const std::string k=std::string("{\"kind\":\"")+kind+"\"";
    for(size_t i=0;i<out.size();++i)if(!out[i].compare(0,k.size(),k))++n;
    return n;
}
static A::Observation send(uint32_t type,A::Choice choice,uint32_t call) {
    A::Observation o=A::Observation();o.kind=A::Observation::SEND;o.type=type;o.choice=choice;o.call_sequence=call;
    return o;
}
static A::Observation position(A::PositionClass cls,int mode,uint32_t call) {
    A::Observation o=A::Observation();o.kind=A::Observation::POSITION;o.position_class=cls;
    o.original_mode=mode;o.call_sequence=call;return o;
}
static void observe(PersistentLog& log,const A::Observation& o,uint64_t now,const char* tag) {
    char line[200];
    snprintf(line,sizeof line,"{\"kind\":\"%s\",\"call\":%u,\"tag\":\"%s\",\"choice\":%u}",
             o.kind==A::Observation::SEND?"send":"position",o.call_sequence,tag,unsigned(o.choice));
    log.observation(line,o,now,emit,0);
}
static N::RawEvent wheel(uint64_t seq,uint64_t at,unsigned kmh) {
    N::RawEvent e=N::RawEvent();e.kind=N::WHEELS;e.epoch=3;e.receive_seq=seq;e.received_ns=at;
    for(unsigned i=0;i<4;++i)e.raw[i]=uint16_t(kmh*100+10000);
    return e;
}

static void rate_limits() {
    static unsigned char storage[PersistentLog::WINDOW_BYTES+PersistentLog::ROW_BYTES];
    PersistentLog log;log.init(storage,model());out.clear();
    log.row(row("boot").c_str(),1*S,emit,0);
    for(unsigned t=1;t<=25;++t)log.row(row("health",",\"capture_active\":true").c_str(),t*S,emit,0);
    assert(count("boot")==1 && count("health")==3);                 // t=1, 11, 21
    log.row(row("health",",\"capture_active\":false").c_str(),26*S,emit,0);
    assert(count("health")==4);                                     // terminal row always kept
    for(unsigned t=0;t<30;++t) {
        log.row(row("shadow").c_str(),30*S+t*100000000ULL,emit,0);
        log.row(row("shadow_calibration").c_str(),30*S+t*100000000ULL,emit,0);
    }
    assert(count("shadow")==1 && count("shadow_calibration")==1);
    // beta_summary: 1/s while live, 1/10 s otherwise, terminal states always.
    out.clear();
    for(unsigned t=0;t<20;++t)log.row(row("beta_summary",",\"state\":\"ARMED\"").c_str(),40*S+t*S,emit,0);
    assert(count("beta_summary")==2);
    out.clear();
    for(unsigned t=0;t<20;++t)log.row(row("beta_summary",",\"state\":\"ENGAGED\"").c_str(),70*S+t*S,emit,0);
    assert(count("beta_summary")==20);
    out.clear();
    log.row(row("beta_summary",",\"state\":\"DISABLED\"").c_str(),89*S+1,emit,0);
    assert(count("beta_summary")==1);
    // Always-kept evidence rows, whatever their rate.
    out.clear();
    const char* always[]={"beta_anchor","beta_reverse_latch","shadow_session","shadow_bus","unknown_future_kind"};
    for(unsigned t=0;t<10;++t)for(unsigned k=0;k<5;++k)log.row(row(always[k]).c_str(),90*S+t,emit,0);
    for(unsigned k=0;k<5;++k)assert(count(always[k])==10);
    // Faults: 5 per kind per 10 s; the rest are counted in the digest.
    out.clear();
    for(unsigned i=0;i<8;++i)log.row(row("shadow_position_rejected").c_str(),100*S+i,emit,0);
    assert(count("shadow_position_rejected")==5);
    log.row(row("shadow_position_rejected").c_str(),111*S,emit,0);   // new window (and a digest)
    assert(count("shadow_position_rejected")==6 && count("log_digest")>=1);
    const std::string& d=out[out.size()-2];
    assert(d.find("\"suppressed\":{")!=std::string::npos &&
           d.find("\"shadow_position_rejected\":3")!=std::string::npos);   // 3 = 8 - 5
    puts("persistent log: health/shadow/summary/fault rate limits and always-kept rows passed");
}

static void window_and_events() {
    static unsigned char storage[PersistentLog::WINDOW_BYTES+PersistentLog::ROW_BYTES];
    PersistentLog log;log.init(storage,model());out.clear();
    // First POSITION is a class transition: a (empty) raw period starts.
    observe(log,position(A::POSITION_FIX,1,1),1*S,"first_fix");
    assert(count("raw_window")==1 && count("position")==1);
    // During the 30 s post period raw rows are written directly...
    observe(log,position(A::POSITION_FIX,1,2),2*S,"fix_direct");
    observe(log,send(1,A::ORIGINAL,2),2*S,"loc_direct");
    assert(count("position")==2 && count("send")==1);
    // ...afterwards they are held in memory, and non-LOCATION sends digested.
    out.clear();
    std::vector<std::string> expected;
    for(unsigned i=0;i<70;++i) {
        const uint64_t t=40*S+i*S;
        char tag[32];snprintf(tag,sizeof tag,"w%u",i);
        observe(log,position(A::POSITION_FIX,1,100+i),t,tag);
        observe(log,send(1,A::ORIGINAL,100+i),t,tag);
        observe(log,send(3,A::ORIGINAL,100+i),t,tag);
        char batch[96];snprintf(batch,sizeof batch,"{\"kind\":\"motion_batch\",\"tag\":\"%s\"}",tag);
        log.row(batch,t,emit,0);
        log.motion(wheel(i+1,t,36));
    }
    assert(count("position")==0 && count("send")==0 && count("motion_batch")==0);
    assert(count("log_digest")>=6);
    // A BETA state change writes the last 60 s (oldest first) behind a
    // marker, then itself; rows older than 60 s are not written.
    const uint64_t event=110*S;
    log.tick(event,emit,0);   // a due periodic digest first, then only the event
    out.clear();
    log.row(row("beta_state",",\"to\":\"GPS_LOST\"").c_str(),event,emit,0);
    assert(!out.empty() && out[0].find("\"kind\":\"raw_window\"")==1 &&
           out[0].find("\"trigger\":\"beta_state\"")!=std::string::npos);
    assert(out.back().find("\"kind\":\"beta_state\"")!=std::string::npos);
    std::string first_tag;
    for(size_t i=1;i+1<out.size();++i) {
        const size_t at=out[i].find("\"tag\":\"w");assert(at!=std::string::npos);
        const unsigned n=unsigned(atoi(out[i].c_str()+at+8));
        assert(40+n>=110-60);                                        // within the 60 s window
        if(i>1) { const size_t p=out[i-1].find("\"tag\":\"w");assert(unsigned(atoi(out[i-1].c_str()+p+8))<=n); }
    }
    assert(out.size()==1+3*60+1 || out.size()==1+3*61+1);           // 60-61 s of position+location+batch
    // Window rows are RAW context (diagnostic class in the journal ring)
    // while BETA was not live; the marker and the event row are not.
    assert(raw_flags.size()>=out.size());
    {
        const size_t base=raw_flags.size()-out.size();
        assert(!raw_flags[base] && !raw_flags.back());
        for(size_t i=1;i+1<out.size();++i)assert(raw_flags[base+i]);
    }
    assert(out[0].find("\"overwritten_rows\":")!=std::string::npos);
    // The marker states how far back the written rows really reach.
    assert(out[0].find("\"span_ms\":60000,\"pre_limit_ms\":60000")!=std::string::npos);
    // While BETA is live (the GPS_LOST row above), raw-context POSITION rows
    // are evidence class; LOCATION sends and batches stay diagnostic.
    assert(log.beta_live());
    out.clear();
    const size_t flags_before=raw_flags.size();
    observe(log,position(A::POSITION_FIX,1,700),event+500000000ULL,"live_position");
    observe(log,send(1,A::ORIGINAL,700),event+500000000ULL,"live_location");
    assert(out.size()==2 && !raw_flags[flags_before] && raw_flags[flags_before+1]);
    // During the post period rows are direct; a second event inside it has
    // no new window to write.
    out.clear();
    observe(log,send(1,A::ORIGINAL,500),event+S,"post");
    log.row(row("beta_hold").c_str(),event+2*S,emit,0);
    assert(out.size()==2 && count("raw_window")==0);
    // Changed sends and candidate-class positions are always kept; a class
    // change is an event; a CONTEXT_UNAVAILABLE position is kept.
    out.clear();
    observe(log,send(1,A::BETA_REPLACEMENT,600),event+100*S,"replaced");
    observe(log,position(A::POSITION_LOST,0,601),event+101*S,"lost_transition");
    observe(log,position(A::POSITION_LOST,0,602),event+102*S,"lost_kept");
    observe(log,position(A::POSITION_NO_FIX_STALE,1,603),event+103*S,"nofix_transition");
    A::Observation unavailable=position(A::POSITION_NO_FIX_STALE,1,604);unavailable.reason=A::CONTEXT_UNAVAILABLE;
    observe(log,unavailable,event+104*S,"context");
    assert(count("send")==1 && count("position")==4);
    // capture_end: window, final digest, then the terminal row.
    log.tick(event+200*S,emit,0);
    out.clear();
    for(unsigned i=0;i<5;++i)log.row("{\"kind\":\"motion_batch\",\"tag\":\"tail\"}",event+200*S+i,emit,0);
    log.row(row("capture_end").c_str(),event+201*S,emit,0);
    assert(out.size()==8 && out[0].find("\"trigger\":\"capture_end\"")!=std::string::npos &&
           out[0].find("\"rows\":5")!=std::string::npos);
    assert(out[out.size()-2].find("\"digest\":\"final\"")!=std::string::npos);
    assert(out.back()=="{\"kind\":\"capture_end\"}");
    puts("persistent log: RAW window order, 60 s limit, post period, kept evidence and capture end passed");
}

static void digest_contents() {
    static unsigned char storage[PersistentLog::WINDOW_BYTES+PersistentLog::ROW_BYTES];
    PersistentLog log;log.init(storage,model());out.clear();
    log.tick(1*S,emit,0);
    for(unsigned i=0;i<10;++i)log.motion(wheel(i+1,1*S+i*100000000ULL,30+i));
    log.motion(wheel(13,3*S,50));                                   // 2-event sequence gap, 1.1 s receipt gap
    N::RawEvent y=N::RawEvent();y.kind=N::YAW;y.epoch=3;y.receive_seq=14;y.received_ns=3*S;y.raw[0]=4094;y.count=2;
    log.motion(y);
    observe(log,send(3,A::ORIGINAL,1),2*S,"x");
    observe(log,send(8,A::ORIGINAL,1),2*S,"x");
    log.tick(11*S+1,emit,0);
    assert(out.size()==1);
    const std::string& d=out[0];
    const char* needles[]={"\"kind\":\"log_digest\",\"schema\":1,\"digest\":\"periodic\",\"profile\":\"persistent\"",
        "\"motion_events\":12","\"wheels\":11","\"yaw\":1","\"seq_gaps\":1","\"max_receipt_gap_ms\":1100",
        "\"speed_kmh_min\":30","\"speed_kmh_max\":50","\"yaw_raw_min\":2047","\"wheels_last_ns\":3000000000",
        "\"reverse_last_ns\":null","\"send_types\":{\"3\":1,\"8\":1}","\"sends\":2","\"assist_ready\":false"};
    for(size_t i=0;i<sizeof needles/sizeof needles[0];++i)
        if(d.find(needles[i])==std::string::npos) { fprintf(stderr,"missing %s in %s\n",needles[i],d.c_str());assert(0); }
    // The next period starts empty.
    out.clear();log.tick(22*S,emit,0);
    assert(out.size()==1 && out[0].find("\"motion_events\":0")!=std::string::npos &&
           out[0].find("\"speed_kmh_min\":null")!=std::string::npos &&
           out[0].find("\"wheels_last_ns\":3000000000")!=std::string::npos);   // latest receipt persists
    puts("persistent log: digest statistics and period reset passed");
}

static void window_bounds() {
    static unsigned char storage[PersistentLog::WINDOW_BYTES+PersistentLog::ROW_BYTES];
    PersistentLog log;log.init(storage,model());out.clear();
    // 1000 rows of 1 KiB within 10 s: more than the window holds.
    std::string pad(1000,'x');
    for(unsigned i=0;i<1000;++i) {
        char head[64];snprintf(head,sizeof head,"{\"kind\":\"motion_batch\",\"n\":%u,\"p\":\"",i);
        log.row((head+pad+"\"}").c_str(),100*S+i*10000000ULL,emit,0);
    }
    log.tick(111*S,emit,0);
    out.clear();
    log.row(row("shadow_disabled").c_str(),111*S,emit,0);
    assert(out.front().find("\"trigger\":\"shadow_disabled\"")!=std::string::npos);
    const size_t kept=out.size()-2;
    assert(kept>370 && kept<400);                                   // about 400 KiB of 1 KiB rows
    for(size_t i=1;i<=kept;++i) {                                   // the newest rows, contiguous
        const unsigned n=unsigned(atoi(out[i].c_str()+out[i].find("\"n\":")+4));
        assert(n==1000-kept+(i-1));
    }
    // Without storage the profile still filters; raw rows are only counted.
    PersistentLog bare;bare.init(0,model());out.clear();
    bare.row("{\"kind\":\"motion_batch\"}",S,emit,0);
    bare.row(row("beta_state").c_str(),2*S,emit,0);
    assert(out.size()==2 && out[0].find("\"window\":\"unavailable\"")!=std::string::npos &&
           out[0].find("\"rows\":0")!=std::string::npos);
    out.clear();bare.tick(20*S,emit,0);
    assert(out.size()==1 && out[0].find("\"raw_window_dropped\":1")!=std::string::npos);
    puts("persistent log: window capacity keeps the newest rows; no-storage fallback passed");
}

// Journal lag guard engaged (2026-10-07): no replacement can be selected, so
// raw-context FIX POSITION rows are diagnostic class again even while BETA is
// live, both written directly and from the window; evidence again after.
static void lag_guard_classes() {
    static unsigned char storage[PersistentLog::WINDOW_BYTES+PersistentLog::ROW_BYTES];
    PersistentLog log;log.init(storage,model());out.clear();raw_flags.clear();
    observe(log,position(A::POSITION_FIX,1,1),1*S,"first");          // transition: raw period to 31 s
    log.row(row("beta_state",",\"to\":\"GPS_LOST\"").c_str(),2*S,emit,0);
    assert(log.beta_live());
    out.clear();raw_flags.clear();
    observe(log,position(A::POSITION_FIX,1,2),3*S,"direct_current");
    log.set_journal_current(false);
    observe(log,position(A::POSITION_FIX,1,3),4*S,"direct_lagging");
    observe(log,position(A::POSITION_LOST,0,4),5*S,"lost_lagging");    // class change: kept, evidence
    observe(log,position(A::POSITION_LOST,0,5),6*S,"lost_kept");       // candidate class: evidence
    log.set_journal_current(true);
    observe(log,position(A::POSITION_FIX,1,6),7*S,"fix_transition");   // class change: kept
    observe(log,position(A::POSITION_FIX,1,7),8*S,"direct_restored");
    assert(out.size()==6 && raw_flags.size()==6);
    assert(!raw_flags[0] && raw_flags[1] && !raw_flags[2] && !raw_flags[3] && !raw_flags[4] && !raw_flags[5]);
    // Window rows buffered while current but written while lagging follow
    // the guard at the time they are written.
    for(unsigned i=0;i<5;++i)observe(log,position(A::POSITION_FIX,1,10+i),(50+i)*S,"window");
    log.set_journal_current(false);
    out.clear();raw_flags.clear();
    log.row(row("beta_hold").c_str(),56*S,emit,0);
    assert(out.size()==7 && out[0].find("\"kind\":\"raw_window\"")==1 && count("position")==5);
    assert(!raw_flags[0] && !raw_flags[6]);
    for(size_t i=1;i<6;++i)assert(raw_flags[i]);
    // Recovered: the next window is evidence again.
    log.set_journal_current(true);
    for(unsigned i=0;i<3;++i)observe(log,position(A::POSITION_FIX,1,20+i),(90+i)*S,"window2");
    out.clear();raw_flags.clear();
    log.row(row("beta_hold").c_str(),94*S,emit,0);
    assert(out.size()==5 && count("position")==3);
    for(size_t i=1;i<4;++i)assert(!raw_flags[i]);
    // BETA not live: diagnostic regardless of the guard (unchanged).
    log.row(row("beta_state",",\"to\":\"DISABLED\"").c_str(),95*S,emit,0);
    out.clear();raw_flags.clear();
    observe(log,position(A::POSITION_FIX,1,30),96*S,"not_live");
    assert(out.size()==1 && raw_flags[0]);
    puts("persistent log: FIX POSITION rows are diagnostic while the journal lag guard is engaged");
}

int main() {
    rate_limits();
    window_and_events();
    digest_contents();
    window_bounds();
    lag_guard_classes();
    puts("persistent log profile tests passed");
    return 0;
}
