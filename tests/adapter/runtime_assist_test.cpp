// Authored qualified source and OEM endpoint, driven by the actual runtime
// worker. Neither this source nor its provenance qualifies vehicle inputs.
#include "runtime/assist_worker.h"
#include "runtime/worker.h"
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <pthread.h>
#include <string>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
namespace A=mx5::adapter;
namespace R=mx5::runtime;
#ifdef MX5_RUNTIME_ASSIST_DSO_TEST
#include "runtime_assist_dso_access.h"
#else
#include "../../src/runtime/runtime.cpp"
#ifdef MX5_ASSIST_RUNTIME_BASELINE
// Compile the same test against the previous runtime with no qualified pump.
struct ProductAssistWorker { ProductAssistWorker(const mx5_dr_config&,const R::AssistSource&) {} };
#else
typedef R::AssistWorker ProductAssistWorker;
#endif
static void configure_runtime(unsigned mode,bool hooks) { config.mode=mode;hook_installed=hooks; }
static void record_raw(const A::Observation* o) { sink(o,0); }
static void audit_failure() { disable_mutation(); }
static void run_product_worker(const char* root,ProductAssistWorker* assist) {
#ifdef MX5_ASSIST_RUNTIME_BASELINE
    (void)assist;worker_at(root,"mx5dr.assist.test");
#else
    R::run_worker(root,"mx5dr.assist.test",assist);
#endif
}
#endif
static const uint64_t MS=1000000ULL;
static uint64_t monotonic(void*) {
    timespec t;assert(!clock_gettime(CLOCK_MONOTONIC,&t));
    return uint64_t(t.tv_sec)*1000000000ULL+uint64_t(t.tv_nsec);
}
static void until(uint64_t ns) {
    timespec t={time_t(ns/1000000000ULL),long(ns%1000000000ULL)};
    int result;do { result=clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&t,0); } while(result==EINTR);
    assert(!result);
}
struct AuthoredSource {
    R::AssistInput events[256];
    std::atomic<unsigned> head,tail,reads;
    std::atomic<uint64_t> watermark;
    std::atomic<bool> qualified,fault;
    AuthoredSource():head(0),tail(0),reads(0),watermark(0),qualified(true),fault(false) {}
    void push(const R::AssistInput& input) {
        const unsigned h=head.load();assert(h-tail.load()<256);
        events[h%256]=input;head.store(h+1);
    }
    static R::AssistPoll pop(void* user,R::AssistInput* out) {
        AuthoredSource& s=*static_cast<AuthoredSource*>(user);
        if(s.fault.load())return R::ASSIST_FAULT;
        const unsigned t=s.tail.load();if(t==s.head.load())return R::ASSIST_EMPTY;
        *out=s.events[t%256];s.tail.store(t+1);return R::ASSIST_INPUT;
    }
    static bool readiness(void* user,uint64_t now,R::AssistReadiness* out) {
        AuthoredSource& s=*static_cast<AuthoredSource*>(user);s.reads.fetch_add(1);
        *out=R::AssistReadiness();const uint64_t complete=s.watermark.load();
        // The producer can complete another window after the worker sampled
        // now. Report only the complete-through boundary requested by this
        // query; never change an event's original measurement or receipt time.
        out->watermark_ns=complete<now?complete:now;
        out->requested_until_ns=now+150*MS;
        R::CoreBridgeQualification& q=out->qualification;
        q.expected_context=mx5_dr_context{11,12,A::generation()};
        q.now_mono_ns=q.limits_verified_until_mono_ns=now;
        q.max_snapshot_age_ns=150*MS;
        q.duration_max_s=60;q.distance_max_m=1500;q.error_max_m=100;
        q.profile_verified=q.input_quality_verified=s.qualified.load();return true;
    }
    R::AssistSource interface() { return R::AssistSource{pop,readiness,this}; }
};
static AuthoredSource source;
static A::Observation position,selected;
static unsigned sends,replacements;
static uint8_t original[48],sent[48];
static A::VehicleData* borrowed;
static bool used_original;
static bool authored_provenance(void*,const A::PositionContext&,A::Provenance* out,void*) {
    *out=A::Provenance{11,12,true,true,true};return true;
}
static void observe(const A::Observation* o,void*) {
    record_raw(o);
    if(o->kind==A::Observation::POSITION)position=*o;else selected=*o;
    errno=E2BIG;
}
static int32_t endpoint(void* session,A::VehicleData* data) {
    assert(session==&sends&&errno==EDOM&&data->length==48);
    used_original=data==borrowed;std::memcpy(sent,data->payload,48);++sends;
    errno=ERANGE;return -731;
}
static void put32(uint8_t* p,uint32_t x) { for(unsigned i=0;i<4;++i)p[i]=uint8_t(x>>(i*8)); }
static void put64(uint8_t* p,uint64_t x) { for(unsigned i=0;i<8;++i)p[i]=uint8_t(x>>(i*8)); }
static int32_t get32(const uint8_t* p) {
    uint32_t x=0;for(unsigned i=0;i<4;++i)x|=uint32_t(p[i])<<(i*8);return int32_t(x);
}
static uint64_t get64(const uint8_t* p) {
    uint64_t x=0;for(unsigned i=0;i<8;++i)x|=uint64_t(p[i])<<(i*8);return x;
}
static bool callback(int mode) {
    uint8_t raw[72]={};put32(raw,uint32_t(mode));put64(raw+8,1700000000ULL);
    const double lat=37,lon=127;std::memcpy(raw+16,&lat,8);std::memcpy(raw+24,&lon,8);
    A::VehicleData data={1,original,48};borrowed=&data;const unsigned before=sends;errno=EDOM;
    A::position_enter(0,raw);assert(errno==EDOM);
    assert(A::send_vehicle_data(&sends,&data)==-731&&errno==ERANGE);
    A::position_leave();assert(errno==ERANGE&&sends==before+1);
    const bool replaced=selected.choice==A::DR_REPLACEMENT;
    assert(used_original!=replaced);
    if(replaced)++replacements;else assert(!std::memcmp(sent,original,48));
    for(unsigned i=0;i<48;++i)assert(original[i]==uint8_t(i+1));
    return replaced;
}
static R::AssistInput input(R::AssistInputKind kind,uint32_t generation) {
    R::AssistInput e=R::AssistInput();e.kind=kind;e.context=mx5_dr_context{11,12,generation};return e;
}
static void enqueue_position() {
    R::AssistInput e=input(R::ASSIST_POSITION,position.prediction_generation);e.observation=position;source.push(e);
}
static uint64_t seed() {
    assert(!callback(1));const A::Observation gps=position;
    R::AssistInput begin=input(R::ASSIST_BEGIN,gps.prediction_generation);
    begin.received_ns=gps.mono_ns;source.push(begin);
    R::AssistInput e=input(R::ASSIST_ANCHOR,gps.prediction_generation);
    e.position_call_sequence=gps.call_sequence;
    mx5_dr_anchor& a=e.anchor;a.context=e.context;a.anchor_id=gps.call_sequence;
    a.position_seq=uint64_t(gps.call_sequence)*4;a.measured_ns=gps.mono_ns;a.utc_ns=1700000000000000000ULL;
    a.latitude_deg=37;a.longitude_deg=127;a.position_error_m=1;a.heading_error_rad=.01;
    a.validated=a.heading_valid=a.calibration_verified=1;a.quality=MX5_DR_VALID;e.received_ns=gps.mono_ns;
    source.push(e);enqueue_position();assert(!callback(0));enqueue_position();return gps.mono_ns;
}
static void window(uint64_t start,uint64_t end,unsigned sequence) {
    R::AssistInput e=input(R::ASSIST_SPEED,A::generation());
    mx5_dr_evidence& q=e.evidence;q.source_id=1;q.source_epoch=1;q.producer_seq=sequence;
    q.measured_ns=start;q.received_ns=end;q.lease_until_ns=start+250*MS;
    q.quality=MX5_DR_VALID;q.freshness=MX5_DR_PRODUCER_TIME;e.value=10;source.push(e);
    e.kind=R::ASSIST_REVERSE;q.source_id=3;e.reverse=0;source.push(e);
    e.kind=R::ASSIST_YAW;q.source_id=2;q.measured_ns=end;q.lease_until_ns=end+250*MS;
    e.value=0;e.raw_yaw=2047;e.yaw_count=1;e.window_start_ns=start;e.window_end_ns=end;source.push(e);
    source.watermark.store(end);
}
static uint64_t drive(unsigned windows) {
    const uint64_t start=seed();
    for(unsigned i=0;i<windows;++i) {
        const uint64_t end=start+(i+1)*100*MS;until(end);window(end-100*MS,end,i+1);
        bool replacement=false;
        for(unsigned tries=0;tries<16;++tries) {
            if(callback(0)&&get64(sent)==1700000000000000000ULL+(i+1)*100*MS) { replacement=true;break; }
            usleep(5000);
        }
        if(!replacement)std::fprintf(stderr,"missing runtime output: reads=%u reason=%u window=%u\n",source.reads.load(),unsigned(selected.reason),i);
        assert(replacement);
        assert(get32(sent+36)==10000&&sent[32]==1&&sent[40]==1);
        assert(get32(sent+8)>370000000&&get32(sent+12)==1270000000);
    }
    return start+windows*100*MS;
}
static void withdrawn_before_expiry(uint64_t frontier,A::Reason reason) {
    bool withdrawn=false;
    A::Reason last_reason=A::PASS;
    // QEMU scheduling is not a real-time timing oracle. Host execution keeps
    // the strict pre-lease bound; product-DSO emulation must still observe the
    // explicit withdrawal reason, not merely passive expiry.
#ifdef MX5_RUNTIME_ASSIST_DSO_TEST
    const uint64_t deadline=monotonic(0)+500*MS;
#else
    const uint64_t deadline=frontier+140*MS;
#endif
    while(monotonic(0)<deadline) {
        if(!callback(0)&&selected.reason==reason) { withdrawn=true;break; }
        last_reason=selected.reason;
        usleep(3000);
    }
    if(!withdrawn)std::fprintf(stderr,"withdraw miss: wanted=%u seen=%u now=%llu frontier=%llu reads=%u gen=%u\n",
        unsigned(reason),unsigned(last_reason),(unsigned long long)monotonic(0),
        (unsigned long long)frontier,source.reads.load(),A::generation());
#ifdef MX5_RUNTIME_ASSIST_DSO_TEST
    assert(withdrawn);
#else
    assert(withdrawn&&monotonic(0)<frontier+150*MS);
#endif
}
struct Running { const char* root;ProductAssistWorker* assist; };
static void* run(void* raw) { Running& r=*static_cast<Running*>(raw);run_product_worker(r.root,r.assist);return 0; }
static std::string read_file(const std::string& path) {
    std::ifstream f(path.c_str());return std::string(std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>());
}
int main(int argc,char** argv) {
    assert(argc==2);alarm(15);const std::string scenario=argv[1];
    assert(scenario=="publication"||scenario=="source_fault"||scenario=="unqualified"||
           scenario=="recovery"||scenario=="audit"||scenario=="journal_failure"||
           scenario=="pre_stopped"||scenario=="unhooked"||scenario=="shadow");
#ifdef MX5_RUNTIME_ASSIST_DSO_TEST
    initialize_runtime_assist_test_dso();
#endif
    for(unsigned i=0;i<48;++i)original[i]=uint8_t(i+1);
    A::Options options=A::Options();options.clock=monotonic;options.provenance=authored_provenance;
    options.sink=observe;options.allow_assist=true;options.max_snapshot_age_ns=150*MS;
    assert(A::configure(endpoint,options)&&A::set_mode(A::ASSIST));
    configure_runtime(scenario=="shadow"?4:3,scenario!="unhooked"); // Fixture injection only.
    ProductAssistWorker assist(mx5_dr_default_config(),source.interface());
    char root[]="/tmp/mx5dr-runtime-assist-XXXXXX";assert(mkdtemp(root));
    const std::string logs=std::string(root)+"/logs";assert(!mkdir(logs.c_str(),0700));
    if(scenario=="pre_stopped")assert(!mkdir((logs+"/capture.stop").c_str(),0700));
    Running running={root,&assist};pthread_t thread;assert(!pthread_create(&thread,0,run,&running));
    const std::string trace=logs+"/trace.0.jsonl";
    if(scenario!="pre_stopped") {
        for(unsigned i=0;i<200&&read_file(trace).find("\"kind\":\"boot\"")==std::string::npos;++i)usleep(5000);
        assert(read_file(trace).find("\"kind\":\"boot\"")!=std::string::npos);
        if(scenario=="unhooked"||scenario=="shadow") {
            const uint64_t start=seed();until(start+100*MS);window(start,start+100*MS,1);usleep(70000);
            assert(!callback(0)&&source.reads.load()==0);
        } else {
            const uint64_t frontier=drive(scenario=="publication"?6:scenario=="journal_failure"?2:1);
            const uint32_t generation=A::generation();
            if(scenario=="source_fault"||scenario=="recovery")source.fault.store(true);
            if(scenario=="unqualified")source.qualified.store(false);
            if(scenario=="audit")audit_failure();
            if(scenario=="journal_failure")assert(!rename(logs.c_str(),(logs+"-retained").c_str()));
            if(scenario=="source_fault"||scenario=="unqualified"||scenario=="recovery") {
                withdrawn_before_expiry(frontier,A::NOT_READY);assert(A::generation()==generation+1);
                source.fault.store(false);source.qualified.store(true);
                for(unsigned i=0;i<20;++i) { assert(!callback(0));usleep(5000); }
                if(scenario=="recovery")drive(3); // New GPS, BEGIN and normalized evidence.
            }
            if(scenario=="audit"||scenario=="journal_failure")withdrawn_before_expiry(frontier,A::DISABLED);
            if(scenario=="journal_failure")assert(!rename((logs+"-retained").c_str(),logs.c_str()));
        }
        assert(!mkdir((logs+"/capture.stop").c_str(),0700));
    }
    assert(!pthread_join(thread,0));
#ifndef MX5_ASSIST_RUNTIME_BASELINE
    assert(assist.status().state==R::ASSIST_STOPPED);
#endif
    assert(!callback(0));
    const std::string journal=read_file(trace);
    if(scenario=="pre_stopped")assert(journal.empty()&&source.reads.load()==0);
    else assert(journal.find("\"kind\":\"position\"")!=std::string::npos);
    if(replacements)assert(journal.find("\"choice\":2")!=std::string::npos);
    if(replacements&&scenario!="journal_failure")
        assert(journal.find("\"kind\":\"assist_worker\"")!=std::string::npos);
    assert(!rmdir((logs+"/capture.stop").c_str()));
    DIR* dir=opendir(logs.c_str());assert(dir);dirent* item;
    while((item=readdir(dir)))if(item->d_name[0]!='.')assert(!unlink((logs+"/"+item->d_name).c_str()));
    closedir(dir);assert(!rmdir(logs.c_str())&&!rmdir(root));
    std::printf("PASS runtime assist %s: %u sends, %u replacements; authored source, actual worker\n",argv[1],sends,replacements);
}
