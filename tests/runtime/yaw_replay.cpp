// Replays recorded wheel/yaw events and POSITION values through the product
// PersistentLog (log_profile=persistent) and prints every row it emits, so
// the yaw-zero data rows (yaw_study_log.h) can be checked on real
// recordings. Input on stdin, one record per line, in time order:
//   W <received_ns> <raw0> <raw1> <raw2> <raw3>
//   Y <received_ns> <sum> <count>
//   P <mono_ns> <class> <mode> <utc_s> <heading> <kmh> <horizontal> <key>
//   E <mono_ns>                      (capture stop)
// <key> distinguishes fixes (the recording's position identity; no
// coordinates are needed). The worker clock is the record time; one worker
// turn (tick) per record. Offline tool: not vehicle, phone or DHU evidence.
#include "runtime/log_profile.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace A=mx5::adapter;
namespace N=mx5::navigation;

static void emit(void*,const char* row,unsigned) { puts(row); }

int main() {
    std::vector<unsigned char> storage(mx5::runtime::PersistentLog::WINDOW_BYTES+mx5::runtime::PersistentLog::ROW_BYTES);
    // research_model_profile() (src/navigation/pipeline.cpp); the yaw rows
    // use only the wheel scale/offset.
    const N::ModelProfile model={2047.0,-0.000658615,0.01,-100.0,0,1,100000000ULL,10.0,0.15};
    mx5::runtime::PersistentLog log;log.init(&storage[0],model);
    char line[512];uint64_t seq=0,records=0;
    while(fgets(line,sizeof line,stdin)) {
        unsigned long long t=0;unsigned a=0,b=0,c=0,d=0;
        if(line[0]=='W' && sscanf(line+1,"%llu %u %u %u %u",&t,&a,&b,&c,&d)==5) {
            N::RawEvent e=N::RawEvent();e.kind=N::WHEELS;e.epoch=1;e.receive_seq=++seq;e.received_ns=t;
            e.raw[0]=uint16_t(a);e.raw[1]=uint16_t(b);e.raw[2]=uint16_t(c);e.raw[3]=uint16_t(d);
            log.motion(e);
        } else if(line[0]=='Y' && sscanf(line+1,"%llu %u %u",&t,&a,&b)==3) {
            N::RawEvent e=N::RawEvent();e.kind=N::YAW;e.epoch=1;e.receive_seq=++seq;e.received_ns=t;
            e.raw[0]=uint16_t(a);e.count=uint16_t(b);
            log.motion(e);
        } else if(line[0]=='P') {
            int cls=0,mode=0;unsigned long long utc=0,key=0;double heading=0,kmh=0,hacc=0;
            if(sscanf(line+1,"%llu %d %d %llu %lf %lf %lf %llu",&t,&cls,&mode,&utc,&heading,&kmh,&hacc,&key)!=8 ||
               cls<0 || cls>5) { fprintf(stderr,"bad record: %s",line);return 65; }
            A::Observation o=A::Observation();o.kind=A::Observation::POSITION;o.mono_ns=t;
            o.position_class=A::PositionClass(cls);o.original_mode=mode;o.position.mode=mode;
            o.position.utc_seconds=utc;o.position.heading_deg=heading;o.position.velocity_kmh=kmh;
            o.position.horizontal=hacc;o.position.latitude_deg=double(key);
            log.observation("{\"kind\":\"position\"}",o,t,emit,0);
        } else if(line[0]=='E' && sscanf(line+1,"%llu",&t)==1) {
            log.row("{\"kind\":\"capture_end\"}",t,emit,0);
            continue;
        } else { fprintf(stderr,"bad record: %s",line);return 65; }
        log.tick(t,emit,0);++records;
    }
    fprintf(stderr,"yaw_replay: %llu records\n",(unsigned long long)records);
    return 0;
}
