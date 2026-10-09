// Journal byte-rate harness for the full and persistent log profiles
// (validation/PERSISTENT_LOGGING_2026-10-06.md). SYNTHETIC drive only.
//
// Runs the product journal path in simulated time: the real adapter
// POSITION/SEND logic with the product runtime Options (BETA, session
// observation declined as with the vehicle's libpatch), the real row
// formatters, Pipeline/GpsHoldout/BetaController, drain_motion with the real
// channel check, the Journal and the PersistentLog filter. The worker turn
// below mirrors run_worker_association's order (observations, motion, MODEL
// tick, BETA tick, 1 s health); it is not the worker thread itself.
// Authored: the OEM send endpoint, the request trace and bus fixture (as in
// test_worker_beta), the drive (speed/yaw profile in the style of the
// tests/replay synthetic fixture: stops, acceleration, cruise, curves, a
// boot-time NO_FIX period and optional GPS outages/tunnels) and AA traffic
// (1 Hz POSITION + LOCATION, about 11 Hz non-LOCATION sends as measured on
// the 2026-10-05 drive). Not reproduced: LDS sideband/association rows, the
// collector journal, the writer thread (bytes are identical), real timing.
// Synthetic receipt-time data: not vehicle, phone or DHU evidence.
#define MX5DR_SIMULATED_CLOCK 1
#include "../../src/runtime/runtime.cpp"
#include <assert.h>
#include <deque>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include "model_bus_fixture.h"

namespace {
const double PI=3.14159265358979323846;
uint8_t sent[48];uint32_t sent_type;
int32_t next_send(void*,A::VehicleData* d) {
    sent_type=d?d->type:0;
    if(d && d->payload && d->length==48)memcpy(sent,d->payload,48);
    return 0;
}
uint64_t request_id;
mx5::runtime::request_trace::Result authored_request(const void*,mx5::runtime::request_trace::Trace* t,void*) {
    *t=mx5::runtime::request_trace::Trace();
    const bus_fixture::B::Snapshot bus=A::read_bus_connection(&bus_fixture::handles[0]);
    t->request.id=++request_id;t->request.epoch=1;t->worker=t->request;
    t->issue.session_context=A::read_issue_session();
    t->issue.observed_ns=t->reply.observed_ns=clock_ns(0);
    t->issue.connection=t->reply.connection=bus;
    if(bus.result==bus_fixture::B::CONNECTED)t->issue.known=mx5::runtime::request_trace::ISSUE_BUS_LIFETIME;
    t->issue.bus_lifetime=bus.lifetime;
    return mx5::runtime::request_trace::OK;
}
void put32(uint8_t* p,uint32_t v) { for(unsigned i=0;i<4;++i)p[i]=uint8_t(v>>(8*i)); }
void put64(uint8_t* p,uint64_t v) { put32(p,uint32_t(v));put32(p+4,uint32_t(v>>32)); }
void putd(uint8_t* p,double v) { uint64_t b;memcpy(&b,&v,8);put64(p,b); }

// ---- synthetic drive (10-minute cycle) ----
double cycle_kmh(double c) {
    if(c<5)return 0;
    if(c<15)return 4*(c-5);
    if(c<190)return 40;
    if(c<200)return 40-4*(c-190);
    if(c<215)return 0;                       // a stop
    if(c<227.5)return 4*(c-215);
    if(c<540)return 50;
    if(c<552.5)return 50-4*(c-540);
    if(c<570)return 0;                       // another stop
    return 4*(c-570)<40?4*(c-570):40;
}
double cycle_yaw(double c) {                  // rad/s, clockwise positive
    if(c>=100 && c<110)return PI/2/10;        // 90 deg right
    if(c>=300 && c<312)return -PI/2/12;       // 90 deg left
    if(c>=450 && c<455)return PI/4/5;         // 45 deg right
    return 0;
}
struct Truth { double lat,lon,heading,kmh; };
struct Drive {
    double north,east,heading,t;
    Drive():north(0),east(0),heading(PI/2),t(0) {}
    void step(double dt) {
        const double c=fmod(t,600.0),v=cycle_kmh(c)/3.6,r=cycle_yaw(c);
        const double mid=heading+r*dt/2;
        north+=v*cos(mid)*dt;east+=v*sin(mid)*dt;heading+=r*dt;t+=dt;
    }
    Truth truth() const {
        Truth x;x.lat=35+north/111000.0;x.lon=135+east/(111000.0*cos(35*PI/180));
        double h=fmod(heading*180/PI,360.0);if(h<0)h+=360;x.heading=h;x.kmh=cycle_kmh(fmod(t,600.0));
        return x;
    }
};

struct SimReceiver {
    std::deque<N::RawEvent> pending;
    N::MotionCursor cursor;
    N::ReceiveResult receive(N::RawEvent* out,N::ReceiveDiagnostic* d) {
        if(pending.empty() || pending.front().received_ns>clock_ns(0))return N::CHANNEL_EMPTY;
        const N::RawEvent e=pending.front();pending.pop_front();
        unsigned char bytes[N::MOTION_RECORD_SIZE];assert(N::encode_motion(e,bytes));
        const N::MotionDatagram packet={bytes,sizeof bytes,false,true,4242,0};
        return N::inspect_motion_datagram(packet,0,clock_ns(0),cursor,out,d);
    }
};

int manager_object;
// One OEM POSITION callback with one LOCATION send.
void oem_position(const Truth& x,int mode,uint64_t utc,bool stored,const Truth& frozen) {
    uint8_t position[72];memset(position,0,sizeof position);
    const Truth& p=mode==0?frozen:x;
    const double lat=stored?35.0005:p.lat,lon=stored?135.0005:p.lon;
    put32(position,uint32_t(mode));put64(position+8,stored?0:utc);
    putd(position+16,lat);putd(position+24,lon);put32(position+32,30);
    putd(position+40,stored?335.0:p.heading);putd(position+48,stored?4.0:p.kmh);
    const double hdop=stored?4.4:mode?1.0:99.0;
    putd(position+56,hdop);putd(position+64,stored?9.7:hdop);
    uint8_t original[48];
    for(unsigned i=0;i<48;++i)original[i]=uint8_t(0xA0+i);
    put64(original,1700000000000ULL+clock_ns(0)/1000000ULL);
    put32(original+8,uint32_t(int32_t(lround(lat*1e7))));put32(original+12,uint32_t(int32_t(lround(lon*1e7))));
    original[16]=mode?1:0;put32(original+20,mode?uint32_t(hdop*2000):0);
    original[32]=1;put32(original+36,uint32_t(lround((stored?4.0:p.kmh)/3.6*1000)));
    original[40]=1;put32(original+44,uint32_t(lround((stored?335.0:p.heading)*1e6)));
    A::VehicleData data={1,original,48};
    A::position_enter(&manager_object,position);
    A::send_vehicle_data(&manager_object,&data);
    A::position_leave();
}
void oem_other_send(uint32_t type,unsigned counter) {
    uint8_t payload[8];put32(payload,counter);put32(payload+4,0x01020304);
    A::VehicleData data={type,payload,8};
    A::send_vehicle_data(&manager_object,&data);
}

struct KindStat { uint64_t rows,bytes; };
}

int main(int argc,char** argv) {
    std::string profile="full",out;unsigned seconds=600,event_every=0,nofix_s=45,outage_s=25;
    // motion_ms: wheels and yaw each every motion_ms (vehicle 2026-10-05:
    // about 100 ms each); turn_ms: worker receive turn (the real worker wakes
    // per datagram, so a turn per datagram is turn_ms = motion_ms/2).
    unsigned motion_ms=100,turn_ms=50;
    for(int i=1;i+1<argc;i+=2) {
        const std::string k=argv[i],v=argv[i+1];
        if(k=="--profile")profile=v;else if(k=="--seconds")seconds=unsigned(atoi(v.c_str()));
        else if(k=="--event-every")event_every=unsigned(atoi(v.c_str()));
        else if(k=="--nofix")nofix_s=unsigned(atoi(v.c_str()));
        else if(k=="--outage")outage_s=unsigned(atoi(v.c_str()));
        else if(k=="--motion-ms")motion_ms=unsigned(atoi(v.c_str()));
        else if(k=="--turn-ms")turn_ms=unsigned(atoi(v.c_str()));
        else if(k=="--out")out=v;
        else { fprintf(stderr,"unknown option %s\n",k.c_str());return 64; }
    }
    if(out.empty() || (profile!="full" && profile!="persistent") || !motion_ms || motion_ms%20 ||
       !turn_ms || turn_ms%10) {
        fprintf(stderr,"usage: log_rate --profile full|persistent --out DIR [--seconds N] "
                       "[--event-every S] [--nofix S] [--outage S] [--motion-ms 20k] [--turn-ms 10k]\n");return 64;
    }
    const std::string logs=out+"/logs";
    if(mkdir(out.c_str(),0700) && errno!=EEXIST)return 73;
    if(mkdir(logs.c_str(),0700) && errno!=EEXIST)return 73;
    for(unsigned i=0;i<3;++i)unlink((logs+"/trace."+char('0'+i)+".jsonl").c_str());
    unlink((logs+"/capture.done").c_str());
    const bool quiet_profile=profile=="persistent";
    config.mode=5;config.valid=true;config.max_log_files=3;
    config.max_log_bytes=41943040;config.log_profile=quiet_profile?mx5::runtime::LOG_PROFILE_PERSISTENT:
                                                     mx5::runtime::LOG_PROFILE_FULL;
    hook_installed=true;boot_result="ok";
    install_report.sessions_declined=true;install_report.declined_stage=3;
    simulated_clock_ns=1000000000ULL;
    A::Options options=product_options();
    options.request_reader=authored_request;options.sessions_declined=true;
    if(!A::configure(next_send,options) || !A::set_mode(A::OBSERVE))return 70;
    bus_fixture::prepare();

    Journal j(out.c_str());
    unsigned char* window=0;
    mx5::runtime::PersistentLog quiet;
    if(quiet_profile) {
        window=new unsigned char[mx5::runtime::PersistentLog::WINDOW_BYTES+mx5::runtime::PersistentLog::ROW_BYTES];
        quiet.init(window,N::research_model_profile());j.filter=&quiet;
    }
    char line[mx5::runtime::OBSERVATION_JSON_CAPACITY];
    char boot_id[37];mx5::runtime::read_boot_id(boot_id);
    snprintf(line,sizeof line,
        "{\"kind\":\"boot\",\"schema\":1,\"pid\":%ld,\"mono_ns\":%llu,\"boot_id\":\"%s\",\"mode\":5,"
        "\"install\":\"ok\",\"assist_ready\":false,\"assist_block\":\"synthetic_log_rate_harness\","
        "\"wire_timestamp_modified\":false,\"session_hooks\":\"declined_third_party_interposer\","
        "\"journal_writer\":\"inline\",\"log_profile\":\"%s\",\"raw_window\":\"%s\","
        "\"beta\":{\"mode\":\"BETA\",\"enabled\":true,\"reason\":\"adapter_opt_in\","
        "\"session_fence\":\"declined_send_storage_counter\"}}",
        (long)getpid(),(unsigned long long)clock_ns(0),boot_id,profile.c_str(),
        quiet_profile?"available":"none");
    j.line(line);
    const N::ModelProfile model=N::research_model_profile();
    snprintf(line,sizeof line,
        "{\"kind\":\"shadow_boot\",\"active\":true,\"capture_active\":true,\"domain\":\"model\","
        "\"source\":\"existing_vbs_vim_callback\",\"assist_ready\":false,"
        "\"motion_log_format\":\"motion_batch_v1\",\"motion_sampling\":false,"
        "\"wheel_kmh_per_count\":%.9g,\"wheel_zero_kmh\":%.9g}",model.wheel_kmh_per_count,model.wheel_zero_kmh);
    j.line(line);
    N::Pipeline navigation;N::GpsHoldout holdout;
    const mx5_dr_context context={1,1,1};
    if(!navigation.init_model(model,mx5_dr_default_config(),context,true,true,true) ||
       !holdout.init_model(model,mx5_dr_default_config(),context) ||
       !navigation.enable_beta(mx5::runtime::beta_profile_tunnel()))return 70;
    mx5::runtime::BetaController beta(beta_shared);
    mx5::runtime::ModelSession model_session;mx5::runtime::ModelBus model_bus;
    if(!beta.enable(j,clock_ns(0),0) || A::mode()!=A::BETA)return 70;
    mx5::runtime::MotionBatch batch;SimReceiver receiver;
    Drive drive;Truth frozen=drive.truth();
    uint64_t sequence=0,last_health=0,last_calibration=0,last_shadow=0,drain_calls=0;
    unsigned other_counter=0;
    const uint64_t start=clock_ns(0),step=10000000ULL; // 10 ms simulation step
    const uint64_t end=start+uint64_t(seconds)*1000000000ULL;
    unsigned outages=0;
    for(uint64_t now=start;now<=end;now+=step) {
        simulated_clock_ns=now;
        const uint64_t ms=(now-start)/1000000ULL;
        drive.step(0.01);
        const Truth x=drive.truth();
        // Motion producer: wheels and yaw every motion_ms each (offset by
        // half a period), reverse 1 then 0 at boot.
        if(ms%motion_ms==0 || ms%motion_ms==motion_ms/2) {
            N::RawEvent e=N::RawEvent();e.epoch=7;e.receive_seq=++sequence;e.received_ns=now;
            if(ms%motion_ms==0) {
                e.kind=N::WHEELS;
                for(unsigned i=0;i<4;++i)e.raw[i]=uint16_t(lround(x.kmh*100+10000));
            } else {
                e.kind=N::YAW;e.count=1;
                e.raw[0]=uint16_t(lround(2048-cycle_yaw(fmod(drive.t,600.0))/0.000658615));
            }
            receiver.pending.push_back(e);
        }
        if(ms==1000 || ms==1100) {
            N::RawEvent e=N::RawEvent();e.kind=N::REVERSE;e.epoch=7;e.receive_seq=++sequence;
            e.received_ns=now;e.reverse=ms==1000?1:0;receiver.pending.push_back(e);
        }
        // AA traffic: POSITION + LOCATION at 1 Hz, other sends at about 11 Hz.
        if(ms%1000==500) {
            const unsigned second=unsigned(ms/1000);
            const bool stored=second<nofix_s;
            // One GPS outage (tunnel, mode 0) of outage_s per event_every
            // seconds, two thirds into each period, after the NO_FIX start.
            const unsigned offset=event_every*2/3;
            const bool outage=event_every && second>=nofix_s+60 &&
                second%event_every>=offset && second%event_every<offset+outage_s;
            if(!outage)frozen=x;
            if(outage && second%event_every==offset)++outages;
            oem_position(x,outage?0:1,1700000000ULL+second,stored,frozen);
        }
        if(ms%90==0)oem_other_send(3,++other_counter);
        // One worker turn every 50 ms, in run_worker_association order.
        if(ms%turn_ms)continue;
        A::Observation o;
        while(pop(&o)) {
            if(!format_observation(line,sizeof line,o)) { j.fail();continue; }
            j.observation_line(line,o);
            if(o.kind==A::Observation::SEND)beta.send(o);
            if(o.kind==A::Observation::POSITION) {
                const uint64_t resets=navigation.status().resets;
                navigation.enqueue_position(o);
                journal_pipeline_reset(j,navigation,resets,"position",o.mono_ns,0,0,o.call_sequence);
                holdout.enqueue_position(o);
                beta.position(j,o,now);
            }
        }
        drain_motion(j,batch,receiver,navigation,holdout,true);
        if(now>navigation.reorder_ns()) {
            const uint64_t resets=navigation.status().resets,watermark=now-navigation.reorder_ns();
            navigation.drain(watermark);++drain_calls;
            journal_pipeline_reset(j,navigation,resets,"drain",watermark);
            holdout.drain(watermark);
        }
        journal_holdout(j,holdout,now);
        if(now-last_calibration>=1000000000ULL) {
            last_calibration=now;
            if(mx5::runtime::format_shadow_calibration(line,sizeof line,now,navigation.calibration(),
                navigation.wheel_calibration(),N::anchor_gate_name(navigation.anchor_gate())))j.line(line);
        }
        if(now-last_shadow>=100000000ULL) {
            last_shadow=now;journal_shadow(j,now,navigation,model_session,model_bus,drain_calls);
        }
        if(beta.cadence_fence(j,now))navigation.fence_beta();
        beta.tick(j,now,navigation,true,navigation.status().last_received_ns,1,0);
        j.pump(now);   // paced RAW window drain (runtime worker turn)
        if(now-last_health>=1000000000ULL) {
            last_health=now;j.tick(now);journal_health(j,now,true,true);j.flush();
        }
        if(j.failed)break;
    }
    // Parked capture stop: final drain, terminal rows, durable close.
    simulated_clock_ns+=step;
    freeze_capture();
    if(drain_capture_tail(j))finish_capture(j,boot_id,simulated_clock_ns-1,simulated_clock_ns);
    const uint64_t total=j.total_bytes;
    // Per-kind breakdown from the files (complete unless rotation discarded).
    std::map<std::string,KindStat> kinds;uint64_t file_bytes=0;
    for(int i=2;i>=0;--i) {
        std::ifstream f((logs+"/trace."+char('0'+i)+".jsonl").c_str());std::string row;
        while(std::getline(f,row)) {
            file_bytes+=row.size()+1;
            std::string kind="?";
            const size_t a=row.find("\"kind\":\"");
            if(a!=std::string::npos) { const size_t b=row.find('"',a+8);kind=row.substr(a+8,b-a-8); }
            if(kind=="send") {
                const size_t c=row.find("\"choice\":");
                const size_t t=row.find("\"type\":");
                kind+=std::string(":type")+(t!=std::string::npos?row.substr(t+7,row.find(',',t)-t-7):"?");
                if(c!=std::string::npos && row[c+9]!='0')kind+=":changed";
            }
            KindStat& k=kinds[kind];++k.rows;k.bytes+=row.size()+1;
        }
    }
    printf("{\"profile\":\"%s\",\"seconds\":%u,\"event_every_s\":%u,\"outages\":%u,\"nofix_s\":%u,"
           "\"total_bytes\":%llu,\"file_bytes\":%llu,\"bytes_per_s\":%.1f,\"bytes_per_hour\":%.0f,"
           "\"journal_failed\":%s,\"beta_state\":\"%s\",\"kinds\":{",
           profile.c_str(),seconds,event_every,outages,nofix_s,(unsigned long long)total,
           (unsigned long long)file_bytes,double(total)/seconds,double(total)/seconds*3600.0,
           j.failed?"true":"false",mx5::runtime::beta_state_name(beta.state()));
    bool first=true;
    for(std::map<std::string,KindStat>::const_iterator it=kinds.begin();it!=kinds.end();++it) {
        printf("%s\"%s\":[%llu,%llu]",first?"":",",it->first.c_str(),
               (unsigned long long)it->second.rows,(unsigned long long)it->second.bytes);
        first=false;
    }
    printf("}}\n");
    delete[] window;
    return j.failed?1:0;
}
