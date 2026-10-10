// VIM side-channel rows (2026-10-10, sensors/vim_channels.h,
// runtime/chan_digest_log.h, validation/VIM_CHANNEL_CAPTURE_2026-10-10.md):
// field parsers, the chan_digest row rules, its row class and its
// independence from every other row. Synthetic input; logging only.
#include "runtime/log_profile.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using mx5::runtime::ChanDigestLog;
using mx5::runtime::PersistentLog;
namespace N=mx5::navigation;
namespace S=mx5::sensors;

static const uint64_t SEC=1000000000ULL,MS=1000000ULL;
static N::ModelProfile model() {
    N::ModelProfile p={2047.0,-0.000658615,0.01,-100.0,0,1,100000000ULL,10.0,0.15};
    return p;
}
static std::vector<std::string> out;
static std::vector<unsigned> classes;
static void sink(void*,const char* row) { out.push_back(row); }
static void emit(void*,const char* row,unsigned cls) { out.push_back(row);classes.push_back(cls); }

static void parser_tests() {
    unsigned char d[16]={7,0xfa,0x0f,2,3,0xff,0x1f,0x00,0x00,3};
    S::Vim116Extra x;
    assert(S::parse_vim116_extra(d,10,&x)==S::CHANNEL_PARSED && x.qf_a==3 && x.accel_long==0x1fff &&
           x.brake==0 && x.qf_b==3);
    assert(S::parse_vim116_extra(d,16,&x)==S::CHANNEL_PARSED);
    for(size_t n=0;n<10;++n)assert(S::parse_vim116_extra(d,n,&x)==S::CHANNEL_SHORT);   // missing fields
    assert(S::parse_vim116_extra(d,17,&x)==S::CHANNEL_SHORT && S::parse_vim116_extra(0,10,&x)==S::CHANNEL_SHORT);
    d[6]=0x20;assert(S::parse_vim116_extra(d,10,&x)==S::CHANNEL_ODD && x.accel_long==0x20ff);   // > 13 bits
    d[6]=0x1f;d[8]=0x20;assert(S::parse_vim116_extra(d,10,&x)==S::CHANNEL_ODD && x.brake==0x2000);
    d[8]=0;d[4]=4;assert(S::parse_vim116_extra(d,10,&x)==S::CHANNEL_ODD);                      // Qf a > 3
    d[4]=0;d[9]=4;assert(S::parse_vim116_extra(d,10,&x)==S::CHANNEL_ODD);                      // Qf b > 3
    d[9]=0;assert(S::parse_vim116_extra(d,10,&x)==S::CHANNEL_PARSED && x.qf_a==0 && x.qf_b==0);
    unsigned char l[16]={2,3,0x34,0x12};S::Vim169 y;
    assert(S::parse_vim169(l,4,&y)==S::CHANNEL_PARSED && y.qf==3 && y.accel_lat==0x1234);
    for(size_t n=0;n<4;++n)assert(S::parse_vim169(l,n,&y)==S::CHANNEL_SHORT);
    l[3]=0x20;assert(S::parse_vim169(l,4,&y)==S::CHANNEL_ODD);
    l[3]=0x12;l[1]=4;assert(S::parse_vim169(l,4,&y)==S::CHANNEL_ODD);
    unsigned char v[16]={0x2f,0xe8,0x03,0xd0,0x07,0,0,0,0,0,0,3};S::Vim15b z;
    assert(S::parse_vim15b(v,12,&z)==S::CHANNEL_PARSED && z.speed==1000 && z.rpm==2000 && z.status==3);
    assert(S::parse_vim15b(v,5,&z)==S::CHANNEL_PARSED && z.status==-1);   // no status byte
    for(size_t n=0;n<5;++n)assert(S::parse_vim15b(v,n,&z)==S::CHANNEL_SHORT);
    v[4]=0x20;assert(S::parse_vim15b(v,12,&z)==S::CHANNEL_ODD);
    v[4]=0x07;v[11]=4;assert(S::parse_vim15b(v,12,&z)==S::CHANNEL_ODD);
    v[11]=0;v[1]=0xff;v[2]=0xff;assert(S::parse_vim15b(v,12,&z)==S::CHANNEL_PARSED && z.speed==65535);
    puts("chan parsers: offsets, short/oversized/null input, 13-bit and Qf limits passed");
}

// ---- synthetic side-channel input ----
static const uint64_t EPOCH=SEC;
static N::ChanRecord rec(uint16_t id,uint64_t t,const unsigned char* bytes,uint8_t length) {
    N::ChanRecord r;memset(&r,0,sizeof r);
    r.id=id;r.length=length;r.dt_ms=uint32_t((t-EPOCH)/MS);
    if(bytes && length<=N::CHAN_PAYLOAD)memcpy(r.data,bytes,length);
    return r;
}
static N::ChanRecord r116(uint64_t t,unsigned ax,unsigned bp,unsigned qa=3,unsigned qb=3) {
    const unsigned char b[10]={7,0,0,1,uint8_t(qa),uint8_t(ax),uint8_t(ax>>8),uint8_t(bp),uint8_t(bp>>8),uint8_t(qb)};
    return rec(0x116,t,b,10);
}
static N::ChanRecord r169(uint64_t t,unsigned ay,unsigned qf=3) {
    const unsigned char b[4]={2,uint8_t(qf),uint8_t(ay),uint8_t(ay>>8)};return rec(0x169,t,b,4);
}
static N::ChanRecord r15b(uint64_t t,unsigned speed,unsigned rpm,unsigned status=3) {
    const unsigned char b[12]={0x2f,uint8_t(speed),uint8_t(speed>>8),uint8_t(rpm),uint8_t(rpm>>8),0,0,0,0,0,0,uint8_t(status)};
    return rec(0x15b,t,b,12);
}
static N::ChanBatch batch(uint32_t number,uint32_t lost,const N::ChanRecord* r,unsigned n) {
    N::ChanBatch b;memset(&b,0,sizeof b);b.epoch=EPOCH;b.batch=number;b.lost=lost;b.count=n;
    for(unsigned i=0;i<n;++i)b.records[i]=r[i];
    return b;
}
static N::RawEvent wheel(uint64_t t,double kmh) {
    N::RawEvent e=N::RawEvent();e.kind=N::WHEELS;e.epoch=1;e.receive_seq=t/MS;e.received_ns=t;
    for(unsigned i=0;i<4;++i)e.raw[i]=uint16_t(kmh*100+10000);
    return e;
}

static void digest_rules() {
    ChanDigestLog log;out.clear();
    log.poll(5*SEC,sink,0);assert(out.empty() && !log.active());   // nothing before input
    log.flush(5*SEC,sink,0);assert(out.empty());
    // Wheels: zero 1.0-3.0 s, 40 km/h 3.1-5.0 s, zero at 6.0 s then silence;
    // batches arrive after the wheel events of their time (as on the worker).
    for(uint64_t t=1000;t<=3000;t+=100)log.wheels(wheel(t*MS,0),model());
    const N::ChanRecord a[]={
        r169(500*MS,4000),                    // before every kept wheel event: not stationary
        r169(1150*MS,4500),                   // zero run since 1.0 s: settling (< 200 ms), not stationary
        r116(1500*MS,4010,0),r169(1500*MS,4100),r15b(1500*MS,0,800),
        r169(3050*MS,4104),                   // newest wheel 3.0 s is zero, 50 ms old: stationary
    };
    N::ChanBatch b1=batch(1,2,a,6);log.batch(&b1);
    for(uint64_t t=3100;t<=5000;t+=100)log.wheels(wheel(t*MS,40),model());
    unsigned char shortp[4]={7,1,2,3};
    const N::ChanRecord c[]={
        r116(4000*MS,4050,600,3,1),r169(4000*MS,4300),r15b(4000*MS,4000,2000),r15b(4100*MS,4000,2100),
        rec(0x116,4100*MS,shortp,4),          // motion-only length: short
        r169(4100*MS,4200,7),                 // Qf 7: odd, excluded
        rec(0x116,4200*MS,0,N::CHAN_LENGTH_INVALID),
        rec(0x123,4200*MS,shortp,4),          // not a side-channel id
    };
    N::ChanBatch b2=batch(2,4,c,8);log.batch(&b2);
    log.wheels(wheel(6000*MS,0),model());
    N::RawEvent yaw=N::RawEvent();yaw.kind=N::YAW;yaw.received_ns=6050*MS;log.wheels(yaw,model());   // ignored
    const N::ChanRecord late[]={r169(6600*MS,4200)};   // zero wheel 600 ms old (> 500 ms): not stationary
    N::ChanBatch b2b=batch(3,5,late,1);log.batch(&b2b);
    log.batch(0);                             // a rejected datagram
    assert(log.active());
    log.poll(10*SEC,sink,0);assert(out.empty());   // period starts at the first worker turn
    log.poll(29*SEC,sink,0);assert(out.empty());
    log.poll(30*SEC,sink,0);assert(out.size()==1);
    const char* expected=
        "{\"kind\":\"chan_digest\",\"schema\":1,\"mono_ns\":30000000000,\"n\":[4,7,3],"
        "\"ax\":[4010,4050,4030.0,4010.0,1],\"bp\":[0,600,1],\"ay\":[4000,4500,4200.7,4102.0,2],"
        "\"q\":[8,10,8],\"v\":[4000,1,3],\"rpm\":[2100,2],\"bad\":[1,1,2,1],\"lost\":3,\"lost0\":2}";
    if(out[0]!=expected)fprintf(stderr,"got  %s\nwant %s\n",out[0].c_str(),expected);
    assert(out[0]==expected);
    // The next period: no input, counters reset, last speed/rpm persist.
    log.poll(50*SEC,sink,0);assert(out.size()==2);
    assert(out[1]=="{\"kind\":\"chan_digest\",\"schema\":1,\"mono_ns\":50000000000,\"n\":[0,0,0],"
                   "\"ax\":null,\"bp\":null,\"ay\":null,\"q\":[0,0,0],\"v\":[4000,0,3],\"rpm\":[2100,0]}");
    // lost: the first cumulative value is a baseline (lost0, drops before
    // this worker listened), later rows carry only the increase; a restarted
    // tap (smaller counter) is a new baseline.
    const N::ChanRecord one[]={r169(7000*MS,8100)};
    N::ChanBatch b3=batch(4,9,one,1);log.batch(&b3);
    N::ChanBatch b4=batch(1,1,one,1);log.batch(&b4);
    N::ChanBatch b5=batch(2,4,one,1);log.batch(&b5);
    log.flush(55*SEC,sink,0);assert(out.size()==3);
    assert(out[2].find("\"lost\":7,\"lost0\":1}")!=std::string::npos && out[2].find("\"n\":[0,3,0]")!=std::string::npos);
    // Wrap hint: a signed 13-bit value crossing 8191 <-> 0.
    ChanDigestLog wrap;const N::ChanRecord w2[]={r116(1500*MS,100,0),r116(1600*MS,8100,0),r169(1600*MS,4000)};
    N::ChanBatch bw=batch(1,0,w2,3);wrap.batch(&bw);wrap.flush(58*SEC,sink,0);
    assert(out.back().find("\"ax\":[100,8100,4100.0,")!=std::string::npos &&
           out.back().find(",\"wrap\":1,\"lost0\":0}")!=std::string::npos);
    // Without any 0x15B: speed/rpm null.
    ChanDigestLog fresh;const N::ChanRecord only[]={r116(1500*MS,8000,0)};
    N::ChanBatch b6=batch(1,0,only,1);fresh.batch(&b6);fresh.flush(60*SEC,sink,0);
    assert(out.back().find("\"v\":null,\"rpm\":null,\"lost0\":0}")!=std::string::npos);
    assert(out.back().find("\"ax\":[8000,8000,8000.0,null,0]")!=std::string::npos);   // no stationary sample
    // Worst-case widths stay inside the row buffer.
    ChanDigestLog wide;
    for(uint64_t t=1000;t<=3000;t+=100)wide.wheels(wheel(t*MS,0),model());
    for(unsigned i=0;i<200000;++i) {
        const N::ChanRecord w[]={r116(2000*MS,i%2?8191:0,i%2?8191:0,i%4,3-i%4),r169(2000*MS,i%2?8191:0,i%4),
                                 r15b(2000*MS,i%2?65535:0,i%2?8191:0,i%4),rec(0x200,2000*MS,0,4)};
        N::ChanBatch wb=batch(i+1,4000000000U,w,4);wide.batch(&wb);wide.batch(0);
    }
    out.clear();wide.flush(70*SEC,sink,0);assert(out.size()==1 && out[0].size()<ChanDigestLog::ROW_CAPACITY);
    printf("chan rows: longest synthetic row %u B (buffer %u), sizeof(ChanDigestLog) %u B, sizeof(PersistentLog) %u B\n",
           unsigned(out[0].size()),unsigned(ChanDigestLog::ROW_CAPACITY),unsigned(sizeof(ChanDigestLog)),
           unsigned(sizeof(PersistentLog)));
    puts("chan digest: stationary rule, gaps, odd/short/invalid, changes, masks, lost delta, persistence passed");
}

// PersistentLog integration: no input -> byte-identical; with input the
// rows are ROW_RAW and every other row is unchanged.
struct Run {
    std::vector<std::string> rows;std::vector<unsigned> cls;
};
static Run drive(bool channels,bool chan_rows) {
    out.clear();classes.clear();
    std::vector<unsigned char> storage(PersistentLog::WINDOW_BYTES+PersistentLog::ROW_BYTES);
    PersistentLog log;log.init(&storage[0],model());log.set_chan_rows(chan_rows);
    uint64_t seq=0;
    for(uint64_t t=10*SEC;t<75*SEC;t+=50*MS) {
        const double kmh=(t/SEC)%30<10?0:40;
        if((t/(50*MS))%2==0) { N::RawEvent e=wheel(t,kmh);e.receive_seq=++seq;log.motion(e); }
        else {
            N::RawEvent e=N::RawEvent();e.kind=N::YAW;e.epoch=1;e.receive_seq=++seq;e.received_ns=t;
            e.raw[0]=2048*5;e.count=5;log.motion(e);
        }
        if(channels && (t/(50*MS))%10==1) {
            const N::ChanRecord r[]={r116(t,8000+unsigned(kmh),kmh>0?0:300),r169(t,8100),r15b(t,unsigned(kmh*100),800)};
            N::ChanBatch b;memset(&b,0,sizeof b);b.epoch=EPOCH;b.batch=uint32_t(t/MS);b.count=3;
            for(unsigned i=0;i<3;++i) { b.records[i]=r[i];b.records[i].dt_ms=uint32_t((t-EPOCH)/MS); }
            log.channels(&b);
        }
        log.tick(t,emit,0);
    }
    log.row("{\"kind\":\"capture_end\",\"schema\":1}",76*SEC,emit,0);
    Run run;run.rows=out;run.cls=classes;return run;
}
static void profile_integration() {
    const Run off=drive(false,true),disabled=drive(true,false),on=drive(true,true);
    // Without side-channel input, or with the rows turned off, the profile
    // is the same byte for byte.
    assert(off.rows==disabled.rows && off.cls==disabled.cls);
    for(size_t i=0;i<off.rows.size();++i)assert(off.rows[i].find("chan_digest")==std::string::npos);
    // With input: chan_digest rows (ROW_RAW) are added, everything else equal.
    Run rest;unsigned chan=0;std::vector<std::string> chan_rows;
    for(size_t i=0;i<on.rows.size();++i) {
        if(!on.rows[i].compare(0,22,"{\"kind\":\"chan_digest\",")) {
            assert(on.cls[i]==PersistentLog::ROW_RAW);++chan;chan_rows.push_back(on.rows[i]);continue;
        }
        rest.rows.push_back(on.rows[i]);rest.cls.push_back(on.cls[i]);
    }
    assert(rest.rows==off.rows && rest.cls==off.cls);
    // 65 s of input: first worker turn after the first batch starts the
    // period; rows every 20 s and the open period at capture stop, before
    // the final log_digest.
    assert(chan==4);
    size_t final_digest=0,final_chan=0;
    for(size_t i=0;i<on.rows.size();++i) {
        if(on.rows[i].find("\"digest\":\"final\"")!=std::string::npos)final_digest=i;
        if(!on.rows[i].compare(0,22,"{\"kind\":\"chan_digest\","))final_chan=i;
    }
    assert(final_chan<final_digest && final_chan+1==final_digest);
    // Stationary samples exist (0 km/h for 10 s of every 30 s) and carry
    // the stationary longitudinal value 8000.
    // 30-40 s is a standstill (batches at 2 Hz): the second row's
    // stationary mean is the stationary value 8000 from the 19 samples
    // after the 200 ms settling time; the overall mean includes 40 km/h.
    assert(chan_rows[1].find("\"ax\":[8000,8040,8021.0,8000.0,19]")!=std::string::npos);
    assert(chan_rows[0].find("\"ax\":[8000,8040,8039.0,null,0]")!=std::string::npos);   // moving only
    puts("chan rows in the persistent profile: no input/off byte-identical, ROW_RAW, cadence, capture stop passed");
}

int main() {
    parser_tests();
    digest_rules();
    profile_integration();
    puts("chan row tests passed");
    return 0;
}
