// Run the real journal/MODEL worker and motion socket. Lifecycle originals and
// input data are authored fixtures; this does not qualify an OEM request/phone.
#include "../../src/runtime/runtime.cpp"
#include <assert.h>
#include <fstream>
#include <string>
#include "model_bus_fixture.h"

namespace S=mx5::runtime::session_trace;
static A::SessionCallbacks callbacks;
static const A::SessionCallbacks* wrapped;
static bool fail_create;
static std::atomic<unsigned> status_block(0);
static int handle;
static int32_t next_send(void*,A::VehicleData*) { return 0; }
static void next_status(void*,void*) {
    if(!status_block.load())return;
    status_block.store(2);while(status_block.load()!=3)usleep(1000);
}
static void* status_inflight(void*) {
    int32_t info[2]={0,0};reinterpret_cast<A::SessionStatus>(wrapped->entry[1])(0,info);return 0;
}
static int32_t next_create(const char*,void*,const A::SessionCallbacks* cb,void** out) {
    if(fail_create)return 260;
    wrapped=cb;*out=&handle;return 0;
}
static int32_t next_destroy(void** out) { *out=0;return 0; }
static char motion_channel[80];
static void* run(void* root) { return worker_at(static_cast<const char*>(root),motion_channel); }
static void until(uint64_t ns) {
    struct timespec t={static_cast<time_t>(ns/1000000000ULL),static_cast<long>(ns%1000000000ULL)};
    int result;do { result=clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&t,0); } while(result==EINTR);
    assert(!result);
}
static void position(uint64_t time,unsigned ms,int mode,const S::Snapshot& issue,
                     const bus_fixture::B::Snapshot& bus) {
    A::Observation o=A::Observation();o.kind=A::Observation::POSITION;o.mono_ns=time;
    o.call_sequence=ms+1;o.position.mode=mode;o.position.utc_seconds=1700000000+ms/1000;
    o.position.latitude_deg=35+double(ms)*0.01/111320;o.position.longitude_deg=135;
    o.position.velocity_kmh=36;o.position.heading_deg=0;
    o.request_result=mx5::runtime::request_trace::OK;
    o.request_trace.request.id=ms+1;o.request_trace.request.epoch=1;
    o.request_trace.worker=o.request_trace.request;o.request_trace.issue.session_context=issue;
    o.request_trace.issue.observed_ns=time;o.request_trace.reply.observed_ns=time;
    o.request_trace.issue.connection=bus;o.request_trace.reply.connection=bus;
    if(bus.result==bus_fixture::B::CONNECTED)o.request_trace.issue.known=mx5::runtime::request_trace::ISSUE_BUS_LIFETIME;
    o.request_trace.issue.bus_lifetime=bus.lifetime;
    sink(&o,0);
}
static uint64_t number(const std::string& line,const char* name) {
    const std::string key=std::string("\"")+name+"\":";
    const size_t at=line.find(key);assert(at!=std::string::npos);
    return strtoull(line.c_str()+at+key.size(),0,10);
}
static void* bus_inflight(void*) { mx5_bus_disconnect(&bus_fixture::handles[0]);return 0; }
int main(int argc,char** argv) {
    assert(argc==2);const std::string scenario=argv[1];alarm(20);
    const bool bus_case=scenario.compare(0,4,"bus_")==0;
    assert(scenario=="destroy"||scenario=="recreate"||scenario=="status"||
           scenario=="failed_create"||scenario=="ambiguous"||scenario=="inflight"||
           scenario=="bus_disconnect"||scenario=="bus_reconnect"||scenario=="bus_reuse"||
           scenario=="bus_closed"||scenario=="bus_signal"||scenario=="bus_ambiguous"||scenario=="bus_inflight");
    A::Options options=A::Options();assert(A::configure(next_send,options));
    const A::SessionBindings bindings={next_create,next_destroy,next_status};
    assert(A::prepare_session_hooks(bindings));
    callbacks.entry[1]=reinterpret_cast<uintptr_t>(next_status);
    void* storage=0;void* other=0;
    assert(!mx5_session_create("fixture",0,&callbacks,&storage));
    const S::Snapshot old=A::read_issue_session();assert(old.result==S::OBSERVED);
    bus_fixture::prepare();
    const bus_fixture::B::Snapshot old_bus=A::read_bus_connection(&bus_fixture::handles[0]);
    char root[]="/tmp/mx5dr-worker-session-XXXXXX";assert(mkdtemp(root));
    const std::string logs=std::string(root)+"/logs";
    assert(!mkdir(logs.c_str(),0700));
    config.mode=4;config.max_log_bytes=8388608;hook_installed=true;
    snprintf(motion_channel,sizeof motion_channel,"mx5dr.worker.session.%ld",(long)getpid());
    pthread_t worker_thread;assert(!pthread_create(&worker_thread,0,run,root));
    // Wait for the actual worker's flushed boot record, not a guessed startup sleep.
    for(unsigned n=0;;++n) {
        std::ifstream f((logs+"/trace.0.jsonl").c_str());std::string line;bool active=false;
        while(std::getline(f,line))if(line.find("\"kind\":\"shadow_boot\",\"active\":true")!=std::string::npos)active=true;
        if(active)break;
        assert(n<200);usleep(10000);
    }
    N::MotionSender sender;assert(sender.open_channel(motion_channel));uint64_t sequence=0;
    pthread_t status_thread;
    const uint64_t start=clock_ns(0);
    for(unsigned ms=0;ms<=7500;ms+=20) {
        until(start+uint64_t(ms)*1000000ULL);
        if(ms==2000) {
            if(bus_case) {
                void* p=&bus_fixture::handles[0];
                if(scenario=="bus_reuse") { mx5_bus_free(p);bus_fixture::open(); }
                else if(scenario=="bus_closed")bus_fixture::callbacks[0](p,0);
                else if(scenario=="bus_signal") { int message=0;assert(!mx5_bus_signal(p,&message)); }
                else if(scenario=="bus_ambiguous")bus_fixture::open(1);
                else if(scenario=="bus_inflight") {
                    bus_fixture::blocked.store(1);assert(!pthread_create(&status_thread,0,bus_inflight,0));
                    while(bus_fixture::blocked.load()!=2)usleep(1000);
                } else {
                    mx5_bus_disconnect(p);
                    if(scenario=="bus_reconnect")assert(mx5_bus_connect(p,"authored",0,0)==1);
                }
            } else if(scenario=="destroy"||scenario=="recreate") {
                assert(!mx5_session_destroy(&storage));
                if(scenario=="recreate")assert(!mx5_session_create("fixture",0,&callbacks,&storage));
            } else if(scenario=="status") {
                int32_t info[2]={0,0};reinterpret_cast<A::SessionStatus>(wrapped->entry[1])(0,info);
            } else if(scenario=="failed_create") {
                fail_create=true;assert(mx5_session_create("fixture",0,&callbacks,&other)==260);fail_create=false;
                // The surviving lifetime and status event are unchanged. A
                // worker that misses the short transition still must reset.
                assert(A::read_issue_session().lifetime==old.lifetime);
                assert(A::read_issue_session().event==old.event);
            } else if(scenario=="inflight") {
                status_block.store(1);assert(!pthread_create(&status_thread,0,status_inflight,0));
                while(status_block.load()!=2)usleep(1000);
            } else assert(!mx5_session_create("fixture",0,&callbacks,&other));
        }
        if(ms==2500 && scenario=="inflight") { status_block.store(3);assert(!pthread_join(status_thread,0)); }
        if(ms==2500 && scenario=="bus_inflight") { bus_fixture::blocked.store(3);assert(!pthread_join(status_thread,0)); }
        if(ms==5000) {
            if(scenario=="destroy")assert(!mx5_session_create("fixture",0,&callbacks,&storage));
            if(scenario=="ambiguous")assert(!mx5_session_destroy(&other));
            if(bus_case) {
                if(scenario=="bus_ambiguous")mx5_bus_free(&bus_fixture::handles[1]);
                else if(scenario!="bus_reuse"&&scenario!="bus_reconnect")
                    assert(mx5_bus_connect(&bus_fixture::handles[0],"authored",0,0)==1);
                bus_fixture::mark();
            }
        }
        const uint64_t now=clock_ns(0);
        for(unsigned kind=1;kind<=3;++kind) {
            if(kind!=N::WHEELS && ms%100)continue;
            N::RawEvent r=N::RawEvent();r.kind=static_cast<N::SensorKind>(kind);
            r.epoch=1;r.receive_seq=++sequence;r.received_ns=now;r.count=1;
            for(unsigned i=0;i<4;++i)r.raw[i]=kind==N::WHEELS?13600:2047;
            assert(sender.send_event(r));
        }
        // Fresh anchors follow post-boundary motion receipt coverage. The
        // lifecycle action at 5000 ms does not make earlier raw inputs current.
        const bus_fixture::B::Snapshot current_bus=A::read_bus_connection(&bus_fixture::handles[0]);
        if(ms==0||ms==1100||ms==5200||ms==6300)position(now,ms,1,A::read_issue_session(),current_bus);
        if(ms==2020||ms==6500)position(now,ms,0,A::read_issue_session(),current_bus);
        if(ms==3000||ms==4100)position(now,ms,1,old,old_bus);
        if(ms==4300)position(now,ms,0,old,old_bus);
    }
    assert(!mkdir((logs+"/capture.stop").c_str(),0700));
    assert(!pthread_join(worker_thread,0));
    assert(access((logs+"/capture.done").c_str(),F_OK)==0);
    std::ifstream f((logs+"/trace.0.jsonl").c_str());std::string line;
    unsigned positions=0,begins=0,reset_aborts=0,invalid=0,recovered=0,stale_valid=0;
    while(std::getline(f,line)) {
        if(line.find("\"kind\":\"position\"")!=std::string::npos)++positions;
        if(line.find("\"kind\":\"shadow_holdout\"")!=std::string::npos) {
            if(line.find("\"event\":\"BEGIN\"")!=std::string::npos)++begins;
            if(line.find(bus_case?"\"reason\":\"bus_reset\"":"\"reason\":\"session_reset\"")!=std::string::npos)++reset_aborts;
        }
        if(line.find("\"kind\":\"shadow\"")==std::string::npos)continue;
        const uint64_t t=number(line,"mono_ns")-start;
        const bool valid=line.find("\"model_valid\":true")!=std::string::npos;
        if(t>=2200000000ULL&&t<5000000000ULL) { if(valid)++stale_valid;else ++invalid; }
        if(t>=6700000000ULL&&t<7500000000ULL&&valid)++recovered;
    }
    printf("%s: raw_positions=%u holdout_begin=%u session_abort=%u invalid=%u stale_valid=%u recovered=%u log=%s\n",
           argv[1],positions,begins,reset_aborts,invalid,stale_valid,recovered,logs.c_str());fflush(stdout);
    assert(positions==9 && begins>=1 && recovered>=2); // Positive controls, including GAP continuation.
    assert(!stale_valid && invalid>=10 && reset_aborts>=1);
    if(const char* results=getenv("MX5DR_SESSION_RESULTS")) {
        std::ifstream input((logs+"/trace.0.jsonl").c_str(),std::ios::binary);
        std::ofstream output((std::string(results)+"/"+scenario+".jsonl").c_str(),std::ios::binary);
        assert(input && output);output<<input.rdbuf();output.close();assert(output);
    }
    unlink((logs+"/capture.done").c_str());rmdir((logs+"/capture.stop").c_str());
    for(unsigned i=0;i<3;++i)unlink((logs+"/trace."+char('0'+i)+".jsonl").c_str());
    assert(!rmdir(logs.c_str())&&!rmdir(root));alarm(0);
}
