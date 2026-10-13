// Drive data rows (v1.0.0-beta.14 candidate, runtime/drive_record_log.h,
// validation/RELEASE_V100_BETA14_CANDIDATE_2026-10-13.md): the row builders
// (exit_jump geometry and Naver predicates, fix1, yaw_learn, rec_drop), the
// gate/drop rule and the independence of every other persistent-profile row.
// Synthetic input; record only.
#include "runtime/log_profile.h"
#include "runtime/journal_ring.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using mx5::runtime::DriveRecordLog;
using mx5::runtime::PersistentLog;
namespace A=mx5::adapter;
namespace N=mx5::navigation;

static const uint64_t SEC=1000000000ULL,MS=1000000ULL;
static const double PI=3.14159265358979323846,M_PER_DEG=6371008.8*PI/180.0;
static N::ModelProfile model() {
    N::ModelProfile p={2047.0,-0.000658615,0.01,-100.0,0,1,100000000ULL,10.0,0.15};
    return p;
}
static std::vector<std::string> out;
static std::vector<unsigned> classes;
static bool gate_open=true;
static unsigned gate_reason=DriveRecordLog::GATE_LAG;   // reason while the gate is closed
static unsigned sink(void*,const char* row) { if(!gate_open)return gate_reason;out.push_back(row);return DriveRecordLog::GATE_OPEN; }
static void emit(void*,const char* row,unsigned cls) { out.push_back(row);classes.push_back(cls); }
static bool has(const std::string& s,const char* part) { return s.find(part)!=std::string::npos; }
static std::vector<std::string> rows(const char* kind,const std::vector<std::string>& from=out) {
    std::vector<std::string> r;const std::string k=std::string("{\"kind\":\"")+kind+"\"";
    for(size_t i=0;i<from.size();++i)if(from[i].compare(0,k.size(),k)==0)r.push_back(from[i]);
    return r;
}
static double field(const std::string& row,const char* key) {
    const std::string k=std::string("\"")+key+"\":";
    const size_t at=row.find(k);assert(at!=std::string::npos);
    return atof(row.c_str()+at+k.size());
}
static void put32(uint8_t* p,uint32_t v) { for(unsigned i=0;i<4;++i)p[i]=uint8_t(v>>(8*i)); }

// ---- jump geometry and the Naver predicates ----
static void jump_rules() {
    // DR at (37.5, 127) heading east; the fix 100 m east and 20 m north.
    const double lat=37.5,lon=127.0,north=20.0/M_PER_DEG,east=100.0/(M_PER_DEG*cos((lat+north/2)*PI/180.0));
    DriveRecordLog::Jump j=DriveRecordLog::jump(lat,lon,lat+north,lon+east,true,90.0,100.0,25.0,10.0,11.0,1000);
    assert(fabs(j.jump_m-101.98)<0.05 && fabs(j.along_m-100.0)<0.05 && fabs(j.cross_m+20.0)<0.05);
    assert(fabs(j.dbrg_deg-10.0)<1e-9 && j.r1 && j.r1_acc && j.r1_dt && j.r1_dist && j.r1_speed);
    assert(j.r2 && fabs(j.implied_kmh-367.1)<0.1);
    // Each R1 input at its boundary.
    assert(!DriveRecordLog::jump(lat,lon,lat+north,lon+east,true,90,100,30.0,10,11,1000).r1_acc);
    assert(DriveRecordLog::jump(lat,lon,lat+north,lon+east,true,90,100,0.0,10,11,1000).r1_acc);
    assert(!DriveRecordLog::jump(lat,lon,lat+north,lon+east,true,90,100,-1,10,11,1000).r1_acc);
    assert(!DriveRecordLog::jump(lat,lon,lat+north,lon+east,true,90,100,25,10,11,499).r1_dt);
    assert(DriveRecordLog::jump(lat,lon,lat+north,lon+east,true,90,100,25,10,11,500).r1_dt);
    assert(DriveRecordLog::jump(lat,lon,lat+north,lon+east,true,90,100,25,10,11,1500).r1_dt);
    assert(!DriveRecordLog::jump(lat,lon,lat+north,lon+east,true,90,100,25,10,11,1501).r1);
    const DriveRecordLog::Jump fast=DriveRecordLog::jump(lat,lon,lat+north,lon+east,true,90,100,25,51.0,10,1000);
    assert(!fast.r1_speed && !fast.r1 && fast.r1_dist);              // 2 x 51 m/s > 102 m
    const DriveRecordLog::Jump fix_fast=DriveRecordLog::jump(lat,lon,lat+north,lon+east,true,90,100,25,10,51.0,1000);
    assert(!fix_fast.r1_speed);                                        // the larger speed counts
    const double near=49.0/(M_PER_DEG*cos(lat*PI/180.0));
    const DriveRecordLog::Jump small=DriveRecordLog::jump(lat,lon,lat,lon+near,true,90,90,25,0,0,1000);
    assert(fabs(small.jump_m-49.0)<0.05 && !small.r1_dist && !small.r1);
    // R2: 200 km/h over at most 3 s.
    assert(!DriveRecordLog::jump(lat,lon,lat+north,lon+east,true,90,100,25,10,11,2000).r2);   // 184 km/h
    const double far=180.0/(M_PER_DEG*cos(lat*PI/180.0));
    assert(DriveRecordLog::jump(lat,lon,lat,lon+far,true,90,90,25,0,0,3000).r2);             // 216 km/h
    assert(!DriveRecordLog::jump(lat,lon,lat,lon+far,true,90,90,25,0,0,3001).r2);
    assert(!DriveRecordLog::jump(lat,lon,lat,lon+far,true,90,90,25,0,0,0).r2);
    // Without a DR bearing: no along/cross split; wrap across north.
    const DriveRecordLog::Jump nob=DriveRecordLog::jump(lat,lon,lat+north,lon,false,0,0,25,0,0,1000);
    assert(!nob.has_brg && nob.along_m==0 && nob.cross_m==0);
    assert(fabs(DriveRecordLog::jump(lat,lon,lat+north,lon,true,350,10,25,0,0,1000).dbrg_deg-20.0)<1e-9);
    assert(fabs(DriveRecordLog::jump(lat,lon,lat+north,lon,true,10,350,25,0,0,1000).dbrg_deg+20.0)<1e-9);
    puts("drive rows: jump split, R1 inputs at their boundaries, R2 window and speed passed");
}

// ---- a synthetic drive through the recorder ----
struct Drive {
    DriveRecordLog log;uint64_t t,utc;uint32_t call;double lat,lon,heading,kmh,yaw_mean;double honest;bool yaw_on;
    Drive():t(100*SEC),utc(1000),call(0),lat(37.5),lon(127.0),heading(90.0),kmh(36.0),yaw_mean(2048.0),honest(-1),
            yaw_on(true) {
        log.set_model(model());log.set_zero_in_use(2048.0);
    }
    void motion(double seconds) {   // 10 Hz wheels and yaw windows of 5 samples
        const uint64_t end=t+uint64_t(seconds*1e9+0.5);
        while(t<end) {
            N::RawEvent w=N::RawEvent();w.kind=N::WHEELS;w.received_ns=t;
            for(unsigned i=0;i<4;++i)w.raw[i]=uint16_t(lround(kmh*100+10000));
            log.motion(w,model());
            N::RawEvent y=N::RawEvent();y.kind=N::YAW;y.received_ns=t+MS;y.count=5;
            y.raw[0]=uint16_t(lround(yaw_mean*5));
            if(yaw_on)log.motion(y,model());
            log.poll(t,sink,0);
            t+=100*MS;
        }
    }
    A::Observation position(int mode,A::PositionClass cls,uint64_t utc) {
        A::Observation o=A::Observation();o.kind=A::Observation::POSITION;o.position_class=cls;
        o.original_mode=mode;o.mono_ns=t;o.call_sequence=++call;o.position.mode=mode;o.position.utc_seconds=utc;
        o.position.latitude_deg=lat;o.position.longitude_deg=lon;o.position.heading_deg=heading;
        o.position.velocity_kmh=kmh;o.position.horizontal=1.0;
        return o;
    }
    // A stock call: POSITION then its ORIGINAL LOCATION send (accuracy in mm).
    void fix(uint32_t accuracy_mm=4000,bool send=true) {
        A::Observation p=position(1,A::POSITION_FIX,++utc);
        log.observation(p,honest,t,sink,0);
        if(!send)return;
        A::Observation s=A::Observation();s.kind=A::Observation::SEND;s.type=1;s.has_payload=true;
        s.call_sequence=p.call_sequence;s.mono_ns=t+2*MS;s.choice=A::ORIGINAL;
        put32(s.original+8,uint32_t(int32_t(lround(lat*1e7))));put32(s.original+12,uint32_t(int32_t(lround(lon*1e7))));
        s.original[16]=1;put32(s.original+20,accuracy_mm);memcpy(s.outgoing,s.original,48);
        log.observation(s,honest,t+2*MS,sink,0);
    }
    // A GPS-lost call whose stock LOCATION goes out unchanged (BETA withdrawn):
    // frozen coordinates, accuracy cleared, no speed or bearing.
    void lost_stock(double frozen_lat,double frozen_lon) {
        A::Observation p=position(0,A::POSITION_LOST,utc);
        log.observation(p,honest,t,sink,0);
        A::Observation s=A::Observation();s.kind=A::Observation::SEND;s.type=1;s.has_payload=true;
        s.call_sequence=p.call_sequence;s.mono_ns=t+2*MS;s.choice=A::ORIGINAL;
        put32(s.original+8,uint32_t(int32_t(lround(frozen_lat*1e7))));
        put32(s.original+12,uint32_t(int32_t(lround(frozen_lon*1e7))));memcpy(s.outgoing,s.original,48);
        log.observation(s,honest,t+2*MS,sink,0);
    }
    // A GPS-lost call: POSITION mode 0 then a BETA replacement send at (dlat, dlon).
    void lost(double dr_lat,double dr_lon,double bearing,double mps,uint32_t accuracy_mm) {
        A::Observation p=position(0,A::POSITION_LOST,utc);
        log.observation(p,honest,t,sink,0);
        A::Observation s=A::Observation();s.kind=A::Observation::SEND;s.type=1;s.has_payload=true;
        s.call_sequence=p.call_sequence;s.mono_ns=t+2*MS;s.choice=A::BETA_REPLACEMENT;
        put32(s.outgoing+8,uint32_t(int32_t(lround(dr_lat*1e7))));put32(s.outgoing+12,uint32_t(int32_t(lround(dr_lon*1e7))));
        s.outgoing[16]=1;put32(s.outgoing+20,accuracy_mm);s.outgoing[32]=1;put32(s.outgoing+36,uint32_t(lround(mps*1000)));
        s.outgoing[40]=1;put32(s.outgoing+44,uint32_t(lround(bearing*1e6)));
        log.observation(s,honest,t+2*MS,sink,0);
    }
    void east(double meters) { lon+=meters/(M_PER_DEG*cos(lat*PI/180.0)); }
};

static void exit_jump_row() {
    out.clear();gate_open=true;
    Drive d;
    for(unsigned k=0;k<5;++k) { d.fix();d.motion(1.0);d.east(10); }
    // Tunnel: 4 replaced sends; the DR runs 30 m short and 10 m left of the road.
    const double north=10.0/M_PER_DEG;d.honest=31.5;
    for(unsigned k=0;k<4;++k) {
        d.lost(d.lat+north,d.lon-30.0/(M_PER_DEG*cos(d.lat*PI/180.0)),90.0,10.0,25000);
        d.motion(1.0);d.east(10);
    }
    d.honest=-1;   // the controller resets it after the episode: the row keeps the episode's value
    assert(rows("exit_jump").empty());
    d.heading=95.0;d.fix(6000);
    const std::vector<std::string> e=rows("exit_jump");
    assert(e.size()==1);
    const std::string& r=e[0];
    char fix_send[64];snprintf(fix_send,sizeof fix_send,"\"fix_send_ms\":%llu,",(unsigned long long)(d.t/MS+2));
    assert(has(r,"\"ep\":1,") && has(r,"\"reps\":4,") && has(r,"\"dt_ms\":998,") && has(r,"\"rep_ms\":3000,"));
    assert(has(r,"\"stock\":0,\"stock_after\":0,\"sent_choice\":3,") && has(r,fix_send));
    assert(has(r,"\"lost_ms\":4000,") && has(r,"\"sent_brg\":90.0,") && has(r,"\"fix_brg\":95.0,") && has(r,"\"dbrg\":5.0,"));
    assert(has(r,"\"sent_acc\":25.00,") && has(r,"\"fix_acc\":6.00,") && has(r,"\"honest\":31.50,"));
    assert(has(r,"\"sent_kmh\":36.0,") && has(r,"\"mode\":1,"));
    // Last DR at -30 m east of the road point 10 m behind the fix: 40 m along, 10 m to the right (south).
    assert(fabs(field(r,"along_m")-40.0)<0.2 && fabs(field(r,"cross_m")-10.0)<0.2 && fabs(field(r,"jump_m")-41.2)<0.2);
    // 41 m < 50 m: R1 off for the distance only; R2 off (148 km/h).
    assert(has(r,"\"r1\":false,\"r1_acc\":true,\"r1_dt\":true,\"r1_dist\":false,\"r1_speed\":true,\"r2\":false,"));
    // The next fixes end nothing; a later episode without replacement gives no row.
    d.motion(1.0);d.fix();d.motion(1.0);
    A::Observation p=d.position(0,A::POSITION_LOST,d.utc);d.log.observation(p,-1,d.t,sink,0);
    d.motion(1.0);d.fix();d.motion(2.0);
    assert(rows("exit_jump").size()==1);
    // An unpaired stock fix (no LOCATION send) is written after PAIR_NS from its own values.
    out.clear();
    d.honest=20;d.lost(d.lat,d.lon-80.0/(M_PER_DEG*cos(d.lat*PI/180.0)),90.0,12.0,20000);d.motion(1.0);
    d.fix(0,false);
    assert(rows("exit_jump").empty());
    d.motion(2.0);
    const std::vector<std::string> u=rows("exit_jump");
    assert(u.size()==1 && has(u[0],"\"ep\":2,") && has(u[0],"\"fix_acc\":null,") && has(u[0],"\"fix_send_ms\":null,") &&
           has(u[0],"\"r1\":true,"));
    puts("drive rows: exit_jump at the first stock fix after a replacement episode, paired and unpaired, passed");
}

// The 111 s tunnel case: BETA withdrew after 3 replacements, the stock
// LOCATION kept going out with the frozen entry coordinates; the phone last
// received one of those, so the jump and dt are measured from it.
static void exit_after_withdrawal() {
    out.clear();gate_open=true;
    Drive d;
    for(unsigned k=0;k<5;++k) { d.fix();d.motion(1.0);d.east(10); }
    const double lat0=d.lat,lon0=d.lon;
    d.honest=40;
    for(unsigned k=0;k<3;++k) { d.lost(d.lat,d.lon,90.0,10.0,25000);d.motion(1.0);d.east(10); }
    d.honest=-1;
    for(unsigned k=0;k<5;++k) { d.lost_stock(lat0,lon0);d.motion(1.0);d.east(10); }
    d.lon=lon0+280.0/(M_PER_DEG*cos(lat0*PI/180.0));
    d.fix();
    const std::vector<std::string> e=rows("exit_jump");
    assert(e.size()==1);
    const std::string& r=e[0];
    fprintf(stderr,"exit_jump after withdrawal: %s\n",r.c_str());
    assert(has(r,"\"reps\":3,") && has(r,"\"stock\":5,\"stock_after\":5,\"sent_choice\":0,") &&
           has(r,"\"dt_ms\":998,") && has(r,"\"sent_acc\":null,") && has(r,"\"sent_brg\":null,") &&
           has(r,"\"honest\":40.00,"));
    assert(fabs(field(r,"jump_m")-280.0)<0.2 && fabs(field(r,"r2_kmh")-1010.0)<1.0);
    assert(has(r,"\"r1\":false,\"r1_acc\":false,\"r1_dt\":true,\"r1_dist\":true,") && has(r,"\"r2\":true,"));
    // A LOST episode in which nothing was sent gives no row.
    out.clear();d.motion(1.0);
    A::Observation p=d.position(0,A::POSITION_LOST,d.utc);d.log.observation(p,-1,d.t,sink,0);
    d.motion(1.0);d.fix();d.motion(2.0);
    assert(rows("exit_jump").empty());
    puts("drive rows: exit_jump anchored on the last LOCATION sent (frozen stock after a withdrawal), R2, passed");
}

static void fix1_rows() {
    out.clear();gate_open=true;
    Drive d;d.kmh=3.0;
    d.motion(1.0);d.fix();d.motion(1.0);d.fix();
    assert(rows("fix1").empty());                  // standstill/crawl: no rows
    d.kmh=36.0;d.yaw_mean=2049.0;d.motion(1.0);   // wheels now 36 km/h; the fix still says 36 too
    d.lat=37.5123456;d.lon=127.0654321;d.heading=123.456789;
    d.fix(4321);
    std::vector<std::string> f=rows("fix1");
    assert(f.size()==1);
    // t_ms, mode, lat/lon e7, speed mm/s, bearing e6, accuracy mm, satellites,
    // window ms, yaw sum (10 windows x 5 samples x +1), samples, wheel km/h e1, wheel events.
    char want[256];
    snprintf(want,sizeof want,"{\"kind\":\"fix1\",\"d\":[%llu,1,375123456,1270654321,10000,123456789,4321,-1,1000,50,50,360,10]}",
             (unsigned long long)(d.t/MS));
    if(f[0]!=want)fprintf(stderr,"got  %s\nwant %s\n",f[0].c_str(),want);
    assert(f[0]==want);
    // A re-poll of the same fix: no row; the next new fix covers both seconds.
    d.motion(1.0);
    A::Observation again=d.position(1,A::POSITION_FIX,d.utc);again.position.latitude_deg=37.5123456;
    again.position.longitude_deg=127.0654321;again.position.heading_deg=123.456789;
    d.log.observation(again,-1,d.t,sink,0);
    d.motion(1.0);d.fix(1000);
    f=rows("fix1");
    assert(f.size()==2 && has(f[1],",2000,100,100,360,20]}"));
    // A bearing that rounds to 360 deg is 0 (as the adapter encodes it).
    d.motion(1.0);d.heading=359.9999996;d.fix();
    assert(has(rows("fix1").back(),",11111,0,4000,") || has(rows("fix1").back(),",10000,0,4000,"));
    d.heading=123.456789;
    // Satellites: the diagnostic GPS_SATELLITE (type 21) observation of the
    // same call; once one was seen, a fix waits for it after its LOCATION.
    A::Observation sat=A::Observation();sat.kind=A::Observation::SEND;sat.type=21;sat.diag_sat=true;
    sat.diag_sat_before=7;sat.mono_ns=d.t;sat.call_sequence=0;d.log.observation(sat,-1,d.t,sink,0);
    d.motion(1.0);d.fix();
    size_t n=rows("fix1").size();
    sat.call_sequence=d.call;sat.mono_ns=d.t+3*MS;d.log.observation(sat,-1,d.t+3*MS,sink,0);
    assert(rows("fix1").size()==n+1 && has(rows("fix1").back(),",4000,7,1000,"));
    d.motion(1.0);d.fix();                 // no type 21 for this call: written after PAIR_NS
    n=rows("fix1").size();d.motion(2.0);
    assert(rows("fix1").size()==n+1 && has(rows("fix1").back(),",4000,-1,1000,"));
    // Rate limit: 12 new fixes within 0.12 s write the burst (8) and count the rest.
    Drive r;r.motion(1.0);
    for(unsigned k=0;k<12;++k) { r.t+=10*MS;r.fix(1000); }
    assert(rows("fix1").size()-n-1==8 && r.log.rate_limited()==4);
    puts("drive rows: fix1 moving gate, exact fields, re-poll, satellites and rate limit passed");
}

static void yaw_learn_rows() {
    out.clear();gate_open=true;
    Drive d;d.yaw_mean=2050.0;
    // 45 s straight at 36 km/h: the estimate is the mean (2050) once 40 s are covered.
    for(unsigned k=0;k<45;++k) { d.motion(1.0);d.fix(); }
    std::vector<std::string> l=rows("yaw_learn");
    assert(l.size()==1 && has(l[0],"\"ev\":\"periodic\",\"z\":200,\"used\":0,\"mean\":200,") &&
           has(l[0],"\"dc\":0.0,\"dev\":0.0,") && has(l[0],"\"age_ms\":0,"));
    assert(fabs(d.log.learned().zero-2050.0)<1e-9 && d.log.learned().span_s>=40.0);
    // A constant left turn of 0.2 deg/s: the rate correction recovers the zero.
    out.clear();
    Drive c;c.yaw_mean=2048.0+(0.2*PI/180.0)/(-0.000658615);   // 2042.70: the yaw a 0.2 deg/s right turn gives
    for(unsigned k=0;k<45;++k) { c.motion(1.0);c.fix();c.heading+=0.2; }
    // (the yaw sums are integers: the mean is 2042.6, 0.1 count below the exact value)
    assert(c.log.learned().have && fabs(c.log.learned().zero-2048.0)<0.15 && fabs(c.log.learned().dc_deg-8.0)<0.25);
    // Episode start: one row with the estimate and its age; the periodic cadence is 60 s.
    out.clear();c.motion(5.0);
    A::Observation p=c.position(0,A::POSITION_LOST,c.utc);c.log.observation(p,-1,c.t,sink,0);
    l=rows("yaw_learn");
    assert(l.size()==1 && has(l[0],"\"ev\":\"lost\",\"z\":") && fabs(field(l[0],"z"))<=15 &&
           field(l[0],"age_ms")>=5000);
    c.log.observation(p,-1,c.t+SEC,sink,0);assert(rows("yaw_learn").size()==1);   // still lost: no row
    // Too slow or a curve over 10 deg: no estimate; the lost row says so.
    out.clear();
    Drive s;s.kmh=8.0;
    for(unsigned k=0;k<45;++k) { s.motion(1.0);s.fix(); }
    Drive v;v.heading=0;
    for(unsigned k=0;k<45;++k) { v.motion(1.0);v.fix();v.heading+=0.5; }
    assert(!s.log.learned().have && !v.log.learned().have);
    p=s.position(0,A::POSITION_LOST,s.utc);s.log.observation(p,-1,s.t,sink,0);
    l=rows("yaw_learn");
    assert(l.size()==1 && has(l[0],"\"ev\":\"lost\",\"z\":null,\"used\":0,\"mean\":null,"));
    // A 5 s yaw gap inside every 40 s window leaves < 90 % coverage: no estimate.
    Drive g;
    for(unsigned k=0;k<45;++k) { g.yaw_on=k<15 || k>=20;g.motion(1.0);g.fix(); }
    assert(!g.log.learned().have);
    // Contiguity: a 5 s gap between fixes restarts the window.
    Drive h;
    for(unsigned k=0;k<45;++k) { if(k==20)h.t+=5*SEC;h.motion(1.0);h.fix(); }
    assert(!h.log.learned().have);
    // One fix without a valid course keeps the chain (it is not compared).
    Drive v2;
    for(unsigned k=0;k<45;++k) { v2.heading=k==20?NAN:90.0;v2.motion(1.0);v2.fix(); }
    assert(v2.log.learned().have && v2.log.learned().fixes==41);
    // |zero - 2048| > 10 counts is rejected: the previous estimate stays.
    Drive z;z.yaw_mean=2050.0;
    for(unsigned k=0;k<45;++k) { z.motion(1.0);z.fix(); }
    assert(z.log.learned().have && fabs(z.log.learned().zero-2050.0)<1e-9);
    z.t+=5*SEC;z.yaw_mean=2058.4;   // a new chain 10.4 counts off: rejected, 2050 stays
    for(unsigned k=0;k<45;++k) { z.motion(1.0);z.fix(); }
    assert(fabs(z.log.learned().zero-2050.0)<1e-9);
    z.t+=5*SEC;z.yaw_mean=2057.6;   // 9.6 counts off: accepted
    for(unsigned k=0;k<45;++k) { z.motion(1.0);z.fix(); }
    assert(fabs(z.log.learned().zero-2057.6)<1e-9);
    puts("drive rows: yaw_learn straight, rate-corrected turn, episode start, speed/curve/coverage/deviation gates passed");
}

static void gate_and_drops() {
    out.clear();gate_open=true;
    Drive d;
    d.motion(1.0);d.fix();assert(rows("fix1").size()==1);
    gate_open=false;
    for(unsigned k=0;k<3;++k) { d.motion(1.0);d.fix(); }
    assert(rows("fix1").size()==1 && d.log.dropped(DriveRecordLog::FIX1)==3);
    assert(rows("rec_drop").empty());
    gate_open=true;d.motion(1.1);
    std::vector<std::string> r=rows("rec_drop");
    // Refused counter rows (retried every second) count themselves.
    assert(r.size()==1 && has(r[0],"\"fix1\":3,\"exit_jump\":0,\"yaw_learn\":0,\"rec_drop\":") &&
           field(r[0],"rec_drop")>=1 && has(r[0],"\"fix1_rate\":0,\"exit_rate\":0,\"lost_rate\":0,") &&
           has(r[0],"\"why\":{\"journal\":0,\"no_writer\":0,\"lag\":") && field(r[0],"lag")>=4 &&
           has(r[0],"\"queue\":0}}"));
    assert(d.log.dropped(DriveRecordLog::FIX1)==0 && d.log.dropped(DriveRecordLog::DROP)==0);
    // The next counter row waits 60 s after a written one.
    gate_open=false;d.motion(1.0);d.fix();gate_open=true;d.motion(1.0);
    assert(rows("rec_drop").size()==1);
    d.motion(60.0);
    r=rows("rec_drop");
    assert(r.size()==2 && has(r[1],"\"fix1\":1,\"exit_jump\":0,\"yaw_learn\":0,\"rec_drop\":0,"));
    // The reason is kept per gate (no writer thread).
    gate_reason=DriveRecordLog::GATE_NO_WRITER;gate_open=false;d.motion(1.0);d.fix();
    assert(d.log.dropped_why(DriveRecordLog::GATE_NO_WRITER)==1);
    gate_reason=DriveRecordLog::GATE_LAG;gate_open=true;
    puts("drive rows: refused rows are dropped and counted; rec_drop cadence passed");
}

// ---- inside the persistent profile ----
struct Feed {
    PersistentLog log;std::vector<unsigned char> storage;uint64_t t;uint32_t call;
    explicit Feed(bool record):storage(PersistentLog::WINDOW_BYTES+PersistentLog::ROW_BYTES),t(10*SEC),call(0) {
        log.init(&storage[0],model());log.set_record_rows(record);log.tick(t,emit,0);
    }
    void step(double kmh,int mode,uint64_t utc,bool replaced) {
        for(unsigned k=0;k<10;++k) {
            N::RawEvent w=N::RawEvent();w.kind=N::WHEELS;w.epoch=1;w.received_ns=t;
            for(unsigned i=0;i<4;++i)w.raw[i]=uint16_t(lround(kmh*100+10000));
            log.motion(w);
            N::RawEvent y=N::RawEvent();y.kind=N::YAW;y.epoch=1;y.received_ns=t+MS;y.count=5;y.raw[0]=uint16_t(5*2049);
            log.motion(y);log.tick(t,emit,0);t+=100*MS;
        }
        A::Observation o=A::Observation();o.kind=A::Observation::POSITION;o.mono_ns=t;o.call_sequence=++call;
        o.original_mode=mode;o.position.mode=mode;o.position.utc_seconds=utc;o.position.latitude_deg=35+utc*1e-4;
        o.position.longitude_deg=135;o.position.heading_deg=90;o.position.velocity_kmh=kmh;
        o.position_class=mode?A::POSITION_FIX:A::POSITION_LOST;
        log.observation("{\"kind\":\"position\"}",o,t,emit,0);
        A::Observation s=A::Observation();s.kind=A::Observation::SEND;s.type=1;s.has_payload=true;
        s.call_sequence=o.call_sequence;s.mono_ns=t+MS;s.original_mode=mode;s.position_class=o.position_class;
        s.choice=replaced?A::BETA_REPLACEMENT:A::ORIGINAL;
        s.original[16]=1;put32(s.original+20,5000);memcpy(s.outgoing,s.original,48);
        if(replaced) { put32(s.outgoing+8,uint32_t(lround((35+utc*1e-4-0.001)*1e7)));put32(s.outgoing+12,1350000000U);
                       s.outgoing[40]=1;put32(s.outgoing+44,90000000U); }
        log.observation(replaced?"{\"kind\":\"send\",\"choice\":3}":"{\"kind\":\"send\",\"choice\":0}",s,t+MS,emit,0);
    }
    void drive() {
        uint64_t utc=1000;
        for(unsigned k=0;k<70;++k)step(40,1,++utc,false);
        for(unsigned k=0;k<10;++k)step(40,0,utc,true);
        for(unsigned k=0;k<20;++k)step(40,1,++utc,false);
        log.row("{\"kind\":\"capture_end\"}",t,emit,0);
    }
};
static bool starts(const std::string& r,const char* prefix) { return !r.compare(0,strlen(prefix),prefix); }
static bool drive_kind(const std::string& r) {
    return starts(r,"{\"kind\":\"fix1\"") || starts(r,"{\"kind\":\"exit_jump\"") ||
           starts(r,"{\"kind\":\"yaw_learn\"") || starts(r,"{\"kind\":\"rec_drop\"");
}
static unsigned closed(void*) { return DriveRecordLog::GATE_LAG; }
static void profile_independence() {
    out.clear();classes.clear();
    { Feed off(false);off.drive(); }
    const std::vector<std::string> base=out;const std::vector<unsigned> base_classes=classes;
    for(size_t i=0;i<base.size();++i)assert(!drive_kind(base[i]));
    out.clear();classes.clear();
    { Feed on(true);on.drive(); }
    std::vector<std::string> kept;std::vector<unsigned> kept_classes;unsigned drive=0;
    for(size_t i=0;i<out.size();++i) {
        if(drive_kind(out[i])) { ++drive;assert(classes[i]==PersistentLog::ROW_RAW);continue; }
        kept.push_back(out[i]);kept_classes.push_back(classes[i]);
    }
    // Every other row is byte-identical, in the same order and class.
    if(kept!=base || kept_classes!=base_classes) {
        fprintf(stderr,"sizes %zu %zu\n",kept.size(),base.size());
        for(size_t i=0;i<kept.size() && i<base.size();++i) if(kept[i]!=base[i] || kept_classes[i]!=base_classes[i]) {
            fprintf(stderr,"first diff %zu\n on %u %.300s\n off %u %.300s\n",i,kept_classes[i],kept[i].c_str(),base_classes[i],base[i].c_str());break; }
    }
    assert(kept==base && kept_classes==base_classes);
    assert(rows("fix1").size()>=85 && rows("exit_jump").size()==1 && rows("yaw_learn").size()>=2);
    // The drive rows follow the rows of their own observation: exit_jump is
    // written at the fix's LOCATION send (in the raw period after the class
    // change), right after that send's own row, which follows the fix row.
    unsigned ordered=0;
    for(size_t i=2;i<out.size();++i)
        if(starts(out[i],"{\"kind\":\"exit_jump\"")) {
            assert(out[i-1]=="{\"kind\":\"send\",\"choice\":0}" && out[i-2]=="{\"kind\":\"position\"}");
            ++ordered;
        }
    assert(ordered==1);
    // A closed gate: no drive row at all, the rest unchanged.
    out.clear();classes.clear();
    { Feed gated(true);gated.log.set_record_gate(closed,0);gated.drive(); }
    assert(out==base && classes==base_classes);
    // Drive kinds are never journal evidence.
    const char* kinds[]={"{\"kind\":\"fix1\",\"d\":[]}","{\"kind\":\"exit_jump\",\"schema\":1}",
                         "{\"kind\":\"yaw_learn\",\"schema\":1}","{\"kind\":\"rec_drop\",\"schema\":1}"};
    for(unsigned i=0;i<4;++i)assert(!mx5::runtime::journal_evidence_row(kinds[i],strlen(kinds[i])));
    printf("drive rows in the persistent profile: %u rows, every other row byte-identical (on, off, gate closed)\n",drive);
}

static void sizes() {
    out.clear();gate_open=true;
    // Extreme values still give complete rows.
    Drive d;d.t=9999999*SEC;d.lat=-89.9999999;d.lon=-179.9999999;d.heading=359.999999;d.kmh=399.0;d.honest=99999.0;
    d.yaw_mean=0.2;d.motion(30.0);d.fix();   // 116 days of uptime, a yaw rail and a 30 s window
    d.lost(89.9999999,179.9999999,359.999999,999.0,4000000000U);d.motion(0.1);
    d.fix(4000000000U);d.motion(2.0);
    const std::vector<std::string> e=rows("exit_jump"),f=rows("fix1");
    assert(e.size()==1 && f.size()==2 && e[0][e[0].size()-1]=='}' && f[0][f[0].size()-1]=='}');
    for(size_t i=0;i<f.size();++i)assert(f[i].size()+1<=DriveRecordLog::max_row_bytes(DriveRecordLog::FIX1));
    for(size_t i=0;i<out.size();++i)
        if(starts(out[i],"{\"kind\":\"yaw_learn\""))assert(out[i].size()+1<=DriveRecordLog::max_row_bytes(DriveRecordLog::LEARN));
    printf("drive rows: longest synthetic exit_jump %zu B, fix1 %zu B (buffer %zu); sizeof(DriveRecordLog) %zu B\n",
           e[0].size(),f[0].size(),DriveRecordLog::ROW_CAPACITY,sizeof(DriveRecordLog));
    assert(e[0].size()<DriveRecordLog::ROW_CAPACITY && sizeof(DriveRecordLog)<=32768);
}

// GPS flapping every second (LOST/FIX) for an hour with a LOCATION every
// call: the rate limits keep the drive rows under the stated budget.
static void flapping_budget() {
    out.clear();gate_open=true;
    Drive d;d.kmh=60.0;
    for(unsigned k=0;k<45;++k) { d.motion(1.0);d.fix();d.east(16.7); }   // a learned zero exists
    out.clear();
    for(unsigned k=0;k<3600;++k) {
        if(k%2)d.lost(d.lat,d.lon,90.0,16.7,25000);else d.fix();
        d.motion(1.0);d.east(16.7);
    }
    d.log.flush(d.t,sink,0);
    size_t bytes=0;unsigned n[DriveRecordLog::ROWS]={0,0,0,0};
    const char* kinds[]={"{\"kind\":\"fix1\"","{\"kind\":\"exit_jump\"","{\"kind\":\"yaw_learn\"","{\"kind\":\"rec_drop\""};
    for(size_t i=0;i<out.size();++i) {
        bytes+=out[i].size()+1;
        for(unsigned r=0;r<DriveRecordLog::ROWS;++r)
            if(starts(out[i],kinds[r])) { ++n[r];assert(out[i].size()+1<=DriveRecordLog::max_row_bytes(r)); }
    }
    const double worst=DriveRecordLog::worst_case_bytes_per_hour();
    printf("drive rows, 1 h of 1 s GPS flapping: %zu B (fix1 %u, exit_jump %u, yaw_learn %u, rec_drop %u); "
           "worst case from the limits %.0f B/h\n",bytes,n[0],n[1],n[2],n[3],worst);
    // The limited rows are counted (rec_drop exit_rate/lost_rate, reset at each written row);
    // 1799 episodes end (the first call is a fix, the last one is lost).
    uint64_t exit_rate=d.log.rate_limited(DriveRecordLog::L_EXIT),lost_rate=d.log.rate_limited(DriveRecordLog::L_LOST);
    for(size_t i=0;i<out.size();++i)
        if(starts(out[i],kinds[3])) { exit_rate+=uint64_t(field(out[i],"exit_rate"));lost_rate+=uint64_t(field(out[i],"lost_rate")); }
    printf("drive rows, flapping: exit_jump limited %llu, yaw_learn at loss limited %llu\n",
           (unsigned long long)exit_rate,(unsigned long long)lost_rate);
    fflush(stdout);
    assert(n[1]<=63 && n[2]<=63+61 && n[1]+exit_rate==1799 && lost_rate>=1700);
    assert(bytes<=worst && worst<600000.0);
}

int main() {
    jump_rules();
    exit_jump_row();
    exit_after_withdrawal();
    fix1_rows();
    yaw_learn_rows();
    gate_and_drops();
    profile_independence();
    sizes();
    flapping_budget();
    puts("drive row tests passed");
    return 0;
}
