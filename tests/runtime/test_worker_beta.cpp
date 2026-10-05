// Runtime-level BETA wiring test (validation/ASSIST_BETA_DESIGN_2026-10-05.md
// decisions 5, 6, 8, 9). Runs the REAL journal/MODEL worker, motion socket,
// adapter POSITION/SEND logic and the product runtime Options (provenance,
// sink, counted session reader, beta_event, allow_beta) with config mode 5,
// an installed hook and session observation knowingly declined (libpatch case:
// session hooks never prepared, so every session read is UNOBSERVED).
// Authored parts: the OEM send endpoint, the bus/request originals
// (model_bus_fixture.h) and the request reader, which fills the request trace
// the real OEM request hooks would observe. Synthetic receipt-time streams
// only: this is not vehicle, phone or DHU evidence.
#include "../../src/runtime/runtime.cpp"
#include <assert.h>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include "model_bus_fixture.h"

namespace S=mx5::runtime::session_trace;
static char motion_channel[80];
static void* run(void* root) { return worker_at(static_cast<const char*>(root),motion_channel); }
static void until(uint64_t ns) {
    struct timespec t={static_cast<time_t>(ns/1000000000ULL),static_cast<long>(ns%1000000000ULL)};
    int result;do { result=clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&t,0); } while(result==EINTR);
    assert(!result);
}

// ---- authored OEM endpoint ------------------------------------------------
static uint8_t sent[48];
static uint32_t sent_type,sent_calls;
static int32_t fail_next_replaced; // result for the next replaced send (0 = success)
static bool replaced_payload(const uint8_t* p);
static uint8_t current_original[48];
static int32_t next_send(void*,A::VehicleData* d) {
    ++sent_calls;sent_type=d?d->type:0;
    if(d && d->payload && d->length==48)memcpy(sent,d->payload,48);
    if(d && d->type==1 && fail_next_replaced && memcmp(sent,current_original,48)) {
        const int32_t r=fail_next_replaced;fail_next_replaced=0;errno=EIO;return r;
    }
    return 0;
}
static uint64_t request_id;
// Stands in for the OEM request/reply observation of this POSITION (the
// product request reader needs the real OEM request hooks).
static mx5::runtime::request_trace::Result authored_request(const void*,
        mx5::runtime::request_trace::Trace* t,void*) {
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

static void put32(uint8_t* p,uint32_t v) { for(unsigned i=0;i<4;++i)p[i]=uint8_t(v>>(8*i)); }
static void put64(uint8_t* p,uint64_t v) { put32(p,uint32_t(v));put32(p+4,uint32_t(v>>32)); }
static void putd(uint8_t* p,double v) { uint64_t b;memcpy(&b,&v,8);put64(p,b); }
static uint32_t get32(const uint8_t* p) { return p[0]|(p[1]<<8)|(p[2]<<16)|(uint32_t(p[3])<<24); }

struct Call { unsigned ms;int mode;bool replaced;int32_t result;uint32_t accuracy_e3,speed_e3; };
static std::vector<Call> calls;
static int manager;
static double kmh=36;
static void* storage_a=&manager;
static int storage_b_object;
static void* storage=storage_a;
static double latitude(unsigned ms) { return 35+kmh/3.6*(ms/1000.0)/111320; }
// One OEM position callback: POSITION enter, one LOCATION send, leave.
static bool oem_call(unsigned ms,int mode) {
    uint8_t position[72];memset(position,0,sizeof position);
    put32(position,uint32_t(mode));put64(position+8,1700000000ULL+ms/1000);
    putd(position+16,latitude(ms));putd(position+24,135.0);put32(position+32,30);
    putd(position+40,0.0);putd(position+48,kmh);putd(position+56,mode?1.0:99.0);
    putd(position+64,mode?1.0:99.0);
    uint8_t original[48];
    for(unsigned i=0;i<48;++i)original[i]=uint8_t(0xA0+i);
    original[16]=0;original[32]=1;original[40]=1;put64(original,1700000000000ULL+ms);
    memcpy(current_original,original,48);
    A::VehicleData data={1,original,48};
    A::position_enter(&manager,position);
    const int32_t result=A::send_vehicle_data(storage,&data);
    A::position_leave();
    assert(sent_type==1);
    const bool replaced=memcmp(sent,original,48)!=0;
    Call c={ms,mode,replaced,result,get32(sent+20),get32(sent+36)};
    calls.push_back(c);
    if(replaced) {
        // Never on a non-mode-0 call; copy-then-overwrite keeps 0..7, 24..31.
        assert(mode==0);
        assert(replaced_payload(original));
    }
    return replaced;
}
static bool replaced_payload(const uint8_t* original) {
    for(unsigned i=0;i<8;++i)if(sent[i]!=original[i])return false;
    for(unsigned i=24;i<32;++i)if(sent[i]!=original[i])return false;
    const uint32_t accuracy=get32(sent+20);
    return sent[16]==1 && accuracy>=1 && accuracy<=40000 && sent[32]==1;
}

struct Rows {
    std::vector<std::string> lines;
    std::vector<std::string> transitions; // "FROM>TO:reason"
    unsigned hold_set,hold_cleared,storage_rows,summaries,boot_beta_ok,replaced_rows,replaced_bad_mode;
};
static std::string field(const std::string& line,const char* name) {
    const std::string key=std::string("\"")+name+"\":\"";
    const size_t at=line.find(key);if(at==std::string::npos)return "";
    const size_t end=line.find('"',at+key.size());return line.substr(at+key.size(),end-at-key.size());
}
static Rows read_rows(const std::string& logs) {
    Rows r=Rows();
    std::ifstream f((logs+"/trace.0.jsonl").c_str());std::string line;
    while(std::getline(f,line)) {
        r.lines.push_back(line);
        if(line.find("\"kind\":\"beta_state\"")!=std::string::npos) {
            assert(line.find("\"assist_ready\":false")!=std::string::npos);
            r.transitions.push_back(field(line,"from")+">"+field(line,"to")+":"+field(line,"reason"));
        }
        if(line.find("\"kind\":\"beta_hold\"")!=std::string::npos) {
            if(field(line,"event")=="hold_set")++r.hold_set;else ++r.hold_cleared;
        }
        if(line.find("\"kind\":\"beta_session_storage\"")!=std::string::npos)++r.storage_rows;
        if(line.find("\"kind\":\"beta_summary\"")!=std::string::npos)++r.summaries;
        if(line.find("\"kind\":\"boot\"")!=std::string::npos &&
           line.find("\"beta\":{\"mode\":\"BETA\",\"enabled\":true,\"reason\":\"adapter_opt_in\","
                     "\"session_fence\":\"declined_send_storage_counter\"}")!=std::string::npos &&
           line.find("\"session_hooks\":\"declined_third_party_interposer\"")!=std::string::npos &&
           line.find("\"assist_ready\":false")!=std::string::npos)++r.boot_beta_ok;
        if(line.find("\"kind\":\"send\"")!=std::string::npos && line.find("\"choice\":3,")!=std::string::npos) {
            ++r.replaced_rows;
            if(line.find("\"mode\":0,")==std::string::npos)++r.replaced_bad_mode;
        }
    }
    return r;
}
static bool has(const Rows& r,const char* t) {
    for(size_t i=0;i<r.transitions.size();++i)if(r.transitions[i]==t)return true;
    return false;
}
static bool ordered(const Rows& r,const std::vector<std::string>& expected) {
    size_t at=0;
    for(size_t i=0;i<r.transitions.size() && at<expected.size();++i)
        if(r.transitions[i]==expected[at])++at;
    return at==expected.size();
}
static bool wait_rows(const std::string& logs,const char* const* needles,unsigned count) {
    for(unsigned n=0;n<300;++n) {
        std::ifstream f((logs+"/trace.0.jsonl").c_str());std::string line;unsigned found=0;
        std::vector<bool> seen(count,false);
        while(std::getline(f,line))
            for(unsigned i=0;i<count;++i)
                if(!seen[i] && line.find(needles[i])!=std::string::npos) { seen[i]=true;++found; }
        if(found==count)return true;
        usleep(10000);
    }
    return false;
}

int main(int argc,char** argv) {
    assert(argc==2);const std::string scenario=argv[1];
    assert(scenario=="main"||scenario=="silence"||scenario=="disable"||scenario=="budget"||
           scenario=="fault"||scenario=="no_anchor");
    alarm(scenario=="budget"?60:40);
    if(scenario=="budget")kmh=59;
    char root[]="/tmp/mx5dr-worker-beta-XXXXXX";assert(mkdtemp(root));
    const std::string logs=std::string(root)+"/logs";
    assert(!mkdir(logs.c_str(),0700));
    // Startup-only fixture injection, as loader_enabled()/bootstrap set them.
    config.mode=5;config.valid=true;config.max_log_bytes=41943040;config.max_log_files=1;
    hook_installed=true;boot_result="ok";
    install_report.sessions_declined=true;install_report.declined_stage=3;
    A::Options options=product_options();
    // The product options for mode 5: counted send-session reader, BETA
    // opt-in with its event hook, qualified ASSIST closed.
    assert(options.allow_beta && !options.allow_assist && options.beta_event==beta_event);
    // The installer accepts only the product session reader; the BETA storage
    // fence is a separate send hook (the v74 installer rejected a wrapper).
    assert(options.session_reader==A::read_send_session && options.provenance==provenance);
    assert(options.send_storage==observe_send_storage_beta);
    assert(options.sink==sink && options.clock==clock_ns && options.max_snapshot_age_ns==500000000ULL);
    // BETA_DECISIONS 3.7 regression: the v74 installer (observe_requests)
    // accepts these product options in both SHADOW and BETA.
    assert(A::install_readers_supported(options));
    config.mode=4;
    { const A::Options shadow=product_options();
      assert(A::install_readers_supported(shadow) && !shadow.allow_beta && !shadow.send_storage); }
    config.mode=5;
    options.request_reader=authored_request;
    assert(!A::install_readers_supported(options));
    // install_v74 sets this from its own decline decision (stage 3, known shim).
    assert(!options.sessions_declined);options.sessions_declined=true;
    assert(A::configure(next_send,options));
    assert(A::set_mode(A::OBSERVE));
    // Declined case: session hooks are never prepared, the reader is UNOBSERVED.
    assert(A::read_issue_session().result==S::UNOBSERVED);
    bus_fixture::prepare();
    snprintf(motion_channel,sizeof motion_channel,"mx5dr.worker.beta.%ld",(long)getpid());
    pthread_t worker_thread;assert(!pthread_create(&worker_thread,0,run,root));
    const char* ready[]={"\"kind\":\"shadow_boot\",\"active\":true",
        "\"kind\":\"beta_state\",\"mono_ns\"","\"to\":\"ARMED\",\"reason\":\"enabled\""};
    assert(wait_rows(logs,ready,3));
    {
        std::ifstream f((logs+"/trace.0.jsonl").c_str());std::string line;bool session=false;
        while(std::getline(f,line))
            if(line.find("\"kind\":\"shadow_session\"")!=std::string::npos &&
               line.find("\"input_available\":true")!=std::string::npos &&
               line.find("\"result\":\"unobserved\"")!=std::string::npos)session=true;
        assert(session); // The declined fence admits MODEL input.
    }
    assert(A::mode()==A::BETA);
    N::MotionSender sender;assert(sender.open_channel(motion_channel));uint64_t sequence=0;
    const unsigned end_ms=scenario=="budget"?20000:scenario=="main"?9600:6400;
    const uint64_t start=clock_ns(0);
    bool motion=true;
    unsigned first_outage_replaced=0,second_outage_replaced=0,after_hold_replaced=0,after_storage_replaced=0;
    unsigned replaced_after_silence=0,replaced_after_disable=0,replaced_after_stop=0;
    bool failed_once=false,storage_changed=false;
    unsigned double_send_replaced=0;
    unsigned last_replaced_ms=0;
    for(unsigned ms=0;ms<=end_ms;ms+=20) {
        until(start+uint64_t(ms)*1000000ULL);
        const uint64_t now=clock_ns(0);
        if(scenario=="silence" && ms==4000)motion=false;
        for(unsigned kind=1;kind<=3 && motion;++kind) {
            if(kind==N::YAW && ms%100)continue;
            if(kind==N::REVERSE && ms)continue; // change-only producer: one message
            N::RawEvent r=N::RawEvent();r.kind=static_cast<N::SensorKind>(kind);
            r.epoch=1;r.receive_seq=++sequence;r.received_ns=now;r.count=1;r.reverse=0;
            for(unsigned i=0;i<4;++i)r.raw[i]=kind==N::WHEELS?uint16_t(lround(kmh*100+10000)):2047;
            if(!sender.send_event(r)) { fprintf(stderr,"motion send failed at %u errno=%d\n",ms,errno);assert(0); }
        }
        if(scenario=="disable" && ms==4000) {
            const std::string marker=logs+"/disable-next-start";
            const int fd=open(marker.c_str(),O_WRONLY|O_CREAT|O_EXCL,0600);assert(fd>=0);close(fd);
        }
        // GPS mode per scenario.
        int mode=-1;
        if(scenario=="no_anchor") {
            // GPS lost before any gated anchor (yaw history < 2 s): never replaced.
            if(ms<1000) { if(ms%500==0)mode=1; } else if(ms%200==0)mode=0;
        }
        else if(ms<=3000) { if(ms%500==0)mode=1; }
        else if(scenario=="main") {
            if(ms>=3200 && ms<5000 && ms%200==0)mode=0;
            else if(ms>=5000 && ms<=7500 && ms%500==0)mode=1;
            else if(ms>=7700 && ms<9000 && ms%200==0)mode=0;
            else if(ms>=9000 && ms%500==0)mode=1;
        } else if(ms>=3200 && ms%200==0)mode=0;
        if(mode<0)continue;
        if(scenario=="main" && ms==4400)fail_next_replaced=-7;
        if(scenario=="main" && ms==8600 && !storage_changed) { storage=&storage_b_object;storage_changed=true; }
        if(scenario=="fault" && ms==4600) {
            // A second LOCATION inside one POSITION is an adapter contract
            // fault: sticky, OBSERVE, and BETA goes to FAULT.
            uint8_t position[72];memset(position,0,sizeof position);
            putd(position+16,latitude(ms));putd(position+24,135.0);
            uint8_t a[48],b[48];memset(a,1,48);memset(b,2,48);
            A::VehicleData first={1,a,48},second={1,b,48};
            A::position_enter(&manager,position);
            // The first LOCATION cannot know a second will follow (existing
            // adapter contract); it may be replaced. The second never is.
            A::send_vehicle_data(storage,&first);
            if(memcmp(sent,a,48))++double_send_replaced;
            A::send_vehicle_data(storage,&second);assert(!memcmp(sent,b,48));
            A::position_leave();
            assert(A::faulted());
            continue;
        }
        const bool replaced=oem_call(ms,mode);
        if(scenario=="fault" && ms>4600 && replaced)++replaced_after_disable;
        if(replaced)last_replaced_ms=ms;
        if(scenario=="main") {
            if(ms>=3200 && ms<5000 && replaced) {
                ++first_outage_replaced;
                if(failed_once)++after_hold_replaced;
                if(calls.back().result==-7)failed_once=true;
            }
            if(ms==5000)assert(!replaced); // GPS return: the very next send is ORIGINAL.
            if(ms>=7700 && ms<9000 && replaced) {
                if(storage_changed)++after_storage_replaced;else ++second_outage_replaced;
            }
            if(ms>=9000)assert(!replaced);
        }
        if(scenario=="silence" && replaced && ms>=4400)++replaced_after_silence;
        if(scenario=="disable" && replaced && ms>=5200)++replaced_after_disable;
    }
    // capture.stop takes precedence: the adapter returns to OBSERVE.
    assert(!mkdir((logs+"/capture.stop").c_str(),0700));
    assert(!pthread_join(worker_thread,0));
    assert(A::mode()==A::OBSERVE);
    for(unsigned i=0;i<3;++i) if(oem_call(end_ms+200+i*200,0))++replaced_after_stop;
    const Rows rows=read_rows(logs);
    unsigned replaced_total=0;
    for(size_t i=0;i<calls.size();++i)if(calls[i].replaced) {
        ++replaced_total;assert(calls[i].mode==0);
        // speed is the DR wheel speed (unscaled BETA): within 5% of the stream.
        assert(fabs(double(calls[i].speed_e3)/1000.0-kmh/3.6)<0.05*kmh/3.6);
    }
    printf("%s: calls=%zu replaced=%u rows_replaced=%u transitions=%zu hold=%u/%u storage_rows=%u summaries=%u last_replaced_ms=%u\n",
           scenario.c_str(),calls.size(),replaced_total,rows.replaced_rows,rows.transitions.size(),
           rows.hold_set,rows.hold_cleared,rows.storage_rows,rows.summaries,last_replaced_ms);
    for(size_t i=0;i<rows.transitions.size();++i)printf("  %s\n",rows.transitions[i].c_str());
    fflush(stdout);
    assert(rows.boot_beta_ok==1);
    assert(rows.replaced_rows==replaced_total+double_send_replaced && !rows.replaced_bad_mode);
    assert(!replaced_after_stop);
    assert(rows.storage_rows>=1 && rows.summaries>=2);
    if(scenario=="main") {
        assert(first_outage_replaced>=2 && !after_hold_replaced && failed_once);
        assert(second_outage_replaced>=1 && !after_storage_replaced && storage_changed);
        assert(rows.hold_set==1 && rows.hold_cleared==1 && rows.storage_rows>=2);
        std::vector<std::string> expected;
        expected.push_back("DISABLED>ARMED:enabled");
        expected.push_back("ARMED>GPS_LOST:gps_lost");
        expected.push_back("GPS_LOST>ENGAGED:published");
        expected.push_back("ENGAGED>WITHDRAWN:send_result_hold");
        expected.push_back("WITHDRAWN>ARMED:gps_returned");
        expected.push_back("ARMED>GPS_LOST:gps_lost");
        expected.push_back("GPS_LOST>ENGAGED:published");
        expected.push_back("ENGAGED>WITHDRAWN:session_storage_changed");
        expected.push_back("WITHDRAWN>ARMED:gps_returned");
        expected.push_back("ARMED>DISABLED:capture_stop");
        assert(ordered(rows,expected) && rows.transitions.size()==expected.size());
    } else if(scenario=="silence") {
        assert(replaced_total>=2 && !replaced_after_silence);
        assert(has(rows,"ENGAGED>WITHDRAWN:sensor_silence") && has(rows,"WITHDRAWN>DISABLED:capture_stop"));
    } else if(scenario=="disable") {
        assert(replaced_total>=2 && !replaced_after_disable);
        assert(has(rows,"ENGAGED>DISABLED:disable_next_start"));
        assert(!has(rows,"DISABLED>DISABLED:capture_stop")); // disabled once, stays disabled
        unlink((logs+"/disable-next-start").c_str());
    } else if(scenario=="fault") {
        assert(replaced_total>=2 && !replaced_after_disable);
        bool fault=false;
        for(size_t i=0;i<rows.transitions.size();++i)
            if(rows.transitions[i].compare(0,13,"ENGAGED>FAULT")==0)fault=true;
        assert(fault && rows.transitions.back().compare(0,13,"ENGAGED>FAULT")==0); // sticky
    } else if(scenario=="no_anchor") {
        assert(!replaced_total && has(rows,"ARMED>GPS_LOST:gps_lost") && !has(rows,"GPS_LOST>ENGAGED:published"));
        assert(has(rows,"GPS_LOST>DISABLED:capture_stop"));
    } else {
        // Budget: replaced while the reported accuracy stays <= 40 m, then the
        // outage stays withdrawn; accuracy is never lowered to stay engaged.
        assert(replaced_total>=5 && has(rows,"ENGAGED>WITHDRAWN:budget_limit"));
        uint32_t previous=0;
        for(size_t i=0;i<calls.size();++i)if(calls[i].replaced) {
            assert(calls[i].accuracy_e3<=40000 && calls[i].accuracy_e3+500>=previous);
            previous=calls[i].accuracy_e3;
        }
        assert(previous>30000);
    }
    unlink((logs+"/capture.done").c_str());rmdir((logs+"/capture.stop").c_str());
    for(unsigned i=0;i<3;++i)unlink((logs+"/trace."+char('0'+i)+".jsonl").c_str());
    assert(!rmdir(logs.c_str())&&!rmdir(root));alarm(0);
    puts("BETA runtime wiring checks passed");
    return 0;
}
