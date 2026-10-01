// Run the real journal/MODEL worker and motion socket. Lifecycle originals and
// input data are authored fixtures; this does not qualify an OEM request/phone.
#include "../../src/runtime/runtime.cpp"
#include <assert.h>
#include <fstream>
#include <map>
#include <string>
#include <vector>
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

// Fixture-only delivery policy. Never change product MotionSender loss behavior.
// Retry only while the observed elapsed time is below 100 ms. Scheduling can
// extend wall time; the receiver still enforces its 250 ms age limit. Preserve
// the same datagram and record retries; never refresh timestamps to hide delay.
static bool send_fixture_motion(N::MotionSender& sender,const N::RawEvent& raw) {
    const uint64_t start=clock_ns(0);unsigned retries=0;int last_errno=0;
    for(;;) {
        if(retries) {
            const uint64_t now=clock_ns(0);
            if(now<start||now-start>=100000000ULL)break;
        }
        errno=0;
        if(sender.send_event(raw)) {
            if(retries)fprintf(stderr,"fixture_motion_backpressure seq=%llu retries=%u elapsed_ns=%llu\n",
                (unsigned long long)raw.receive_seq,retries,(unsigned long long)(clock_ns(0)-start));
            return true;
        }
        last_errno=errno;const uint64_t now=clock_ns(0);
        if((last_errno!=EAGAIN&&last_errno!=EWOULDBLOCK&&last_errno!=EINTR)||
           now<start||now-start>=100000000ULL)break;
        ++retries;const timespec pause={0,1000000};nanosleep(&pause,0);
    }
    fprintf(stderr,"fixture_motion_failure seq=%llu sensor=%u errno=%d retries=%u received_ns=%llu now_ns=%llu\n",
        (unsigned long long)raw.receive_seq,unsigned(raw.kind),last_errno,retries,
        (unsigned long long)raw.received_ns,(unsigned long long)clock_ns(0));
    errno=last_errno;return false;
}
static std::string expected_motion(const N::RawEvent& raw) {
    char line[256];const int n=snprintf(line,sizeof line,"[%u,%llu,%llu,%lld,%u,%u,%u,%u,%u,%d]",
        unsigned(raw.kind),(unsigned long long)raw.receive_seq,(unsigned long long)raw.received_ns,
        (long long)raw.source_mono_ms,unsigned(raw.raw[0]),unsigned(raw.raw[1]),unsigned(raw.raw[2]),
        unsigned(raw.raw[3]),unsigned(raw.count),raw.reverse);
    assert(n>0&&size_t(n)<sizeof line);return line;
}

static void position(uint64_t time,unsigned ms,int mode,const S::Snapshot& issue,
                     const bus_fixture::B::Snapshot& bus,uint64_t issued=0) {
    A::Observation o=A::Observation();o.kind=A::Observation::POSITION;o.mono_ns=time;
    o.call_sequence=ms+1;o.position.mode=mode;o.position.utc_seconds=1700000000+ms/1000;
    o.position.latitude_deg=35+double(ms)*0.01/111320;o.position.longitude_deg=135;
    o.position.velocity_kmh=36;o.position.heading_deg=0;
    o.request_result=mx5::runtime::request_trace::OK;
    o.request_trace.request.id=ms+1;o.request_trace.request.epoch=1;
    o.request_trace.worker=o.request_trace.request;o.request_trace.issue.session_context=issue;
    o.request_trace.issue.observed_ns=issued?issued:time;o.request_trace.reply.observed_ns=time;
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
static void* bus_free_inflight(void*) { mx5_bus_free(&bus_fixture::handles[0]);return 0; }
int main(int argc,char** argv) {
    assert(argc==2);const std::string scenario=argv[1];alarm(20);
    const bool bus_case=scenario.compare(0,4,"bus_")==0;
    const bool stale_raw_case=getenv("MX5DR_TEST_STALE_RAW")!=0;
    const bool slow_yaw=getenv("MX5DR_TEST_SLOW_YAW")!=0;
    const bool pre_gap=getenv("MX5DR_TEST_PREGAP")!=0;assert(!pre_gap||bus_case);
    assert(scenario=="destroy"||scenario=="recreate"||scenario=="status"||
           scenario=="failed_create"||scenario=="ambiguous"||scenario=="inflight"||
           scenario=="bus_disconnect"||scenario=="bus_reconnect"||scenario=="bus_reuse"||
           scenario=="bus_closed"||scenario=="bus_signal"||scenario=="bus_ambiguous"||scenario=="bus_inflight"||scenario=="bus_free_inflight"||scenario=="bus_late_same");
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
    // The boot row precedes initial fences. Wait for both flushed input-ready fences.
    for(unsigned n=0;;++n) {
        std::ifstream f((logs+"/trace.0.jsonl").c_str());std::string line;
        bool active=false,session_ready=false,bus_ready=false;
        while(std::getline(f,line)) {
            if(line.find("\"kind\":\"shadow_boot\",\"active\":true")!=std::string::npos)active=true;
            if(line.find("\"input_available\":true")!=std::string::npos) {
                if(line.find("\"kind\":\"shadow_session\"")!=std::string::npos)session_ready=true;
                if(line.find("\"kind\":\"shadow_bus\"")!=std::string::npos)bus_ready=true;
            }
        }
        if(active&&session_ready&&bus_ready)break;
        assert(n<200);usleep(10000);
    }
    N::MotionSender sender;assert(sender.open_channel(motion_channel));uint64_t sequence=0;
    std::vector<std::string> raw_expected;
    uint64_t old_receipt_seq=0,old_transport_seq=0;
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
                else if(scenario=="bus_late_same") { bus_fixture::selected=1;void* unrelated=mx5_bus_create(bus_fixture::closed,0);mx5_bus_free(unrelated); }
                else if(scenario=="bus_free_inflight") {
                    bus_fixture::blocked.store(1);assert(!pthread_create(&status_thread,0,bus_free_inflight,0));
                    while(bus_fixture::blocked.load()!=2)usleep(1000);
                }
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
        if(ms==2500 && (scenario=="bus_inflight"||scenario=="bus_free_inflight")) { bus_fixture::blocked.store(3);assert(!pthread_join(status_thread,0)); }
        if(ms==5000) {
            if(scenario=="destroy")assert(!mx5_session_create("fixture",0,&callbacks,&storage));
            if(scenario=="ambiguous")assert(!mx5_session_destroy(&other));
            if(bus_case) {
                if(scenario=="bus_free_inflight")bus_fixture::open();
                else if(scenario=="bus_ambiguous")mx5_bus_free(&bus_fixture::handles[1]);
                else if(scenario!="bus_reuse"&&scenario!="bus_reconnect"&&scenario!="bus_late_same")
                    assert(mx5_bus_connect(&bus_fixture::handles[0],"authored",0,0)==1);
                bus_fixture::mark();
            }
        }
        const uint64_t now=clock_ns(0);
        if(stale_raw_case && ms==2120) {
            assert(scenario=="bus_reuse");
            for(unsigned which=0;which<2;++which) {
                N::RawEvent late=N::RawEvent();late.kind=N::WHEELS;late.epoch=1;
                late.receive_seq=++sequence;late.received_ns=which?now:start+1990000000ULL;
                late.source_mono_ms=which?int64_t((start+1990000000ULL)/1000000ULL):0;
                late.count=1;for(unsigned i=0;i<4;++i)late.raw[i]=13600;
                if(which)old_transport_seq=late.receive_seq;else old_receipt_seq=late.receive_seq;
                assert(send_fixture_motion(sender,late));raw_expected.push_back(expected_motion(late));
            }
        }
        for(unsigned kind=1;kind<=3;++kind) {
            if(kind!=N::WHEELS && ms%(kind==N::YAW&&slow_yaw?160:100))continue;
            N::RawEvent r=N::RawEvent();r.kind=static_cast<N::SensorKind>(kind);
            r.epoch=1;r.receive_seq=++sequence;r.received_ns=now;r.count=1;
            for(unsigned i=0;i<4;++i)r.raw[i]=kind==N::WHEELS?13600:2047;
            assert(send_fixture_motion(sender,r));raw_expected.push_back(expected_motion(r));
        }
        // Fresh anchors follow post-boundary motion receipt coverage. The
        // lifecycle action at 5000 ms does not make earlier raw inputs current.
        const bus_fixture::B::Snapshot current_bus=A::read_bus_connection(&bus_fixture::handles[0]);
        if(ms==0||ms==1100||ms==5200||ms==6300)position(now,ms,1,A::read_issue_session(),current_bus);
        if(ms==2020||ms==6500||(pre_gap&&ms==1600))position(now,ms,0,A::read_issue_session(),current_bus);
        if(ms==3000||ms==4100)position(now,ms,1,old,old_bus,scenario=="bus_late_same"?start:0);
        if(ms==4300)position(now,ms,0,old,old_bus,scenario=="bus_late_same"?start:0);
    }
    assert(!mkdir((logs+"/capture.stop").c_str(),0700));
    assert(!pthread_join(worker_thread,0));
    assert(access((logs+"/capture.done").c_str(),F_OK)==0);
    uint64_t fresh_anchor_ns=0;
    {
        std::ifstream anchors((logs+"/trace.0.jsonl").c_str());std::string row;
        while(std::getline(anchors,row)) {
            if(row.find("\"kind\":\"position\"")!=std::string::npos&&number(row,"call")==5201) {
                assert(!fresh_anchor_ns);fresh_anchor_ns=number(row,"mono_ns");
            }
        }
        assert(fresh_anchor_ns>=start+5200000000ULL);
    }
    std::ifstream f((logs+"/trace.0.jsonl").c_str());std::string line;
    unsigned positions=0,begins=0,reset_aborts=0,invalid=0,recovered=0,stale_valid=0;
    unsigned pre_ready=0,pre_begin=0,pre_valid=0,bus_boundary=0;size_t raw_rows=0;
    unsigned excluded_receipt=0,excluded_transport=0,late_request_rejected=0;
    uint64_t reset_since=0,last_drain_calls=0;
    typedef std::pair<uint64_t,uint64_t> PositionKey;
    std::map<PositionKey,uint64_t> recorded_positions;
    unsigned linked_holdout_references=0;
    while(std::getline(f,line)) {
        if(line.find("\"kind\":\"shadow_motion_excluded\"")!=std::string::npos) {
            const uint64_t seq=number(line,"receive_seq");
            if(seq==old_receipt_seq&&line.find("receipt_before_bus")!=std::string::npos)++excluded_receipt;
            if(seq==old_transport_seq&&line.find("transport_before_bus")!=std::string::npos)++excluded_transport;
        }
        if(line.find("request_before_bus_boundary")!=std::string::npos)++late_request_rejected;
        if(line.find(bus_case?"\"kind\":\"shadow_bus\"":"\"kind\":\"shadow_session\"")!=std::string::npos&&line.find("\"reset\":true")!=std::string::npos) {
            const uint64_t t=number(line,"mono_ns")-start;
            if(t>=1900000000ULL&&t<2200000000ULL) {
                if(!reset_since)reset_since=number(line,"mono_ns");
                if(bus_case)++bus_boundary;
            }
        }
        if(line.find("\"kind\":\"motion_batch\"")!=std::string::npos) {
            assert(line.find("\"schema\":1,")!=std::string::npos);
            assert(line.find("\"epoch\":1,")!=std::string::npos);
            assert(line.find("\"producer_time_status\":\"unknown\"")!=std::string::npos);
            size_t at=line.find("\"events\":[");assert(at!=std::string::npos);at+=10;
            while(at<line.size()&&line[at]=='[') {
                const size_t end=line.find(']',at);assert(end!=std::string::npos&&raw_rows<raw_expected.size());
                assert(line.substr(at,end-at+1)==raw_expected[raw_rows++]);
                at=end+1;if(at<line.size()&&line[at]==',')++at;
            }
            assert(line.substr(at)=="]}");
        }
        assert(line.find("\"kind\":\"motion_rejected\"")==std::string::npos);
        // Once the authored stream ends at 7500 ms, the unchanged age limit
        // may expire while the worker waits for its capture-stop check.
        if(slow_yaw&&line.find("\"kind\":\"shadow_pipeline_reset\"")!=std::string::npos)
            assert(number(line,"mono_ns")>=start+7500000000ULL);
        if(line.find("\"kind\":\"position\"")!=std::string::npos) {
            ++positions;
            const PositionKey key(number(line,"call"),number(line,"generation"));
            assert(recorded_positions.insert(std::make_pair(key,number(line,"mono_ns"))).second);
        }
        if(line.find("\"kind\":\"shadow_holdout\"")!=std::string::npos) {
            const bool has_reference=line.find("\"event\":\"BEGIN\"")!=std::string::npos||
                line.find("\"event\":\"COMPARED\"")!=std::string::npos||
                line.find("\"event\":\"SKIPPED\"")!=std::string::npos;
            if(has_reference) {
                assert(line.find("\"reference_call\":null")==std::string::npos);
                assert(line.find("\"reference_generation\":null")==std::string::npos);
                const PositionKey key(number(line,"reference_call"),number(line,"reference_generation"));
                const std::map<PositionKey,uint64_t>::const_iterator p=recorded_positions.find(key);
                assert(p!=recorded_positions.end()&&p->second==number(line,"reference_ns"));
                ++linked_holdout_references;
            } else {
                // This authored worker run exercises ordinary reset/capture
                // aborts, which have no single reference Observation.
                assert(line.find("\"reference_call\":null")!=std::string::npos);
                assert(line.find("\"reference_generation\":null")!=std::string::npos);
            }
            if(line.find("\"event\":\"BEGIN\"")!=std::string::npos) {
                ++begins;if(number(line,"mono_ns")<start+2000000000ULL)++pre_begin;
            }
            if(line.find(bus_case?"\"reason\":\"bus_reset\"":"\"reason\":\"session_reset\"")!=std::string::npos)++reset_aborts;
        }
        if(line.find("\"kind\":\"shadow\"")==std::string::npos)continue;
        const uint64_t drain_calls=number(line,"drain_calls_total");
        assert(drain_calls>last_drain_calls);last_drain_calls=drain_calls;
        const uint64_t t=number(line,"mono_ns")-start;
        const bool valid=line.find("\"model_valid\":true")!=std::string::npos;
        if(t<2000000000ULL&&number(line,"state")==MX5_DR_READY&&number(line,"frontier_ns"))++pre_ready;
        if(t<2000000000ULL&&valid)++pre_valid;
        // Use the recorded fresh GPS time, not its scheduled 5200 ms deadline.
        // Include the immediate reset and the 5000 ms lifecycle recovery itself.
        if(reset_since&&number(line,"mono_ns")>=reset_since&&number(line,"mono_ns")<fresh_anchor_ns) { if(valid)++stale_valid;else ++invalid; }
        if(t>=6700000000ULL&&t<7500000000ULL&&valid)++recovered;
    }
    printf("%s: raw_positions=%u holdout_begin=%u session_abort=%u invalid=%u stale_valid=%u recovered=%u log=%s\n",
           argv[1],positions,begins,reset_aborts,invalid,stale_valid,recovered,logs.c_str());fflush(stdout);
    assert(positions==(pre_gap?10u:9u) && begins>=1 && recovered>=2); // Positive controls, including GAP continuation.
    printf("pre_boundary_ready=%u pre_boundary_begin=%u raw_rows=%zu accepted=%zu\n",pre_ready,pre_begin,raw_rows,raw_expected.size());fflush(stdout);
    assert(pre_ready>=2&&pre_begin>=1);
    assert(linked_holdout_references>=begins);
    assert(raw_rows==raw_expected.size()&&raw_rows==sequence);
    assert(reset_since && reset_since<fresh_anchor_ns && !stale_valid && invalid>=10 && (pre_gap||reset_aborts>=1));
    printf("boundary checks: receipt_excluded=%u transport_excluded=%u late_request_rejected=%u pre_valid=%u bus_boundary=%u\n",excluded_receipt,excluded_transport,late_request_rejected,pre_valid,bus_boundary);fflush(stdout);
    if(stale_raw_case)assert(excluded_receipt==1&&excluded_transport==1);
    if(scenario=="bus_late_same")assert(late_request_rejected==3);
    if(pre_gap)assert(pre_valid>=2&&bus_boundary>=1);
    if(const char* results=getenv("MX5DR_SESSION_RESULTS")) {
        std::ifstream input((logs+"/trace.0.jsonl").c_str(),std::ios::binary);
        std::ofstream output((std::string(results)+"/"+scenario+(pre_gap?"-pregap":"")+(stale_raw_case?"-stale-raw":"")+".jsonl").c_str(),std::ios::binary);
        assert(input && output);output<<input.rdbuf();output.close();assert(output);
    }
    unlink((logs+"/capture.done").c_str());rmdir((logs+"/capture.stop").c_str());
    for(unsigned i=0;i<3;++i)unlink((logs+"/trace."+char('0'+i)+".jsonl").c_str());
    assert(!rmdir(logs.c_str())&&!rmdir(root));alarm(0);
}
