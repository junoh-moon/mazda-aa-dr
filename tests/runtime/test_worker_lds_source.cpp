// The actual worker supplies owned LDS/request evidence to a source callback.
// The authored peer supplies no physical qualification, so ASSIST stays idle.
#include "../../src/runtime/runtime.cpp"
#include "runtime/lds_request_source.h"
#include <atomic>
#include <cassert>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>

namespace R=mx5::runtime;
namespace L=R::lds_sideband;
namespace Q=R::request_trace;
namespace bus_fixture {
// Authored originals behind the real bus ownership wrappers.
static int handles[1];
static int32_t closed(void*,void*) { return 0; }
static void* create(A::BusClosed,void*) { return &handles[0]; }
static int32_t connect(void*,const char*,int32_t,uintptr_t) { return 1; }
static void end(void*) {}
static int32_t signal(void*,void*) { return 0; }
static int32_t is_signal(void*,const char*,const char*) { return 1; }
}
static char channel[80];
static A::Observation expected;
struct Consumer {
    R::LdsRequestSource source;
    std::atomic<unsigned> reads,matched,mismatched,conflicts,polls,pause,revoked;
    R::LdsRequestSource::JoinedReply owned;
    Consumer():reads(0),matched(0),mismatched(0),conflicts(0),polls(0),pause(0),revoked(0),owned() {}
    static R::AssistPoll pop(void* user,R::AssistInput*) {
        static_cast<Consumer*>(user)->polls.fetch_add(1);return R::ASSIST_EMPTY;
    }
    static bool readiness(void* user,uint64_t,R::AssistReadiness* out) {
        Consumer& c=*static_cast<Consumer*>(user);c.reads.fetch_add(1);
        // Hold the actual worker between boundary reads. The parent may then
        // complete the first request before the worker discovers its bus.
        if(c.pause.load()==1) {
            c.pause.store(2);while(c.pause.load()!=3)usleep(1000);c.pause.store(0);
        }
        if(c.owned.revision&&!c.source.current(c.owned))c.revoked.fetch_add(1);
        R::LdsRequestSource::JoinedReply joined;
        const R::LdsRequestSource::Result result=c.source.lookup(expected,&joined);
        if(result==R::LdsRequestSource::MATCHED) {
            assert(c.source.current(joined));
            assert(joined.observation.call_sequence==expected.call_sequence);
            assert(joined.observation.prediction_generation==expected.prediction_generation);
            assert(joined.observation.request_trace.request.id==expected.request_trace.request.id);
            assert(joined.observation.position.latitude_deg==expected.position.latitude_deg);
            assert(joined.record.field_lineage.fields[0].write_sequence==1);
            assert(joined.diagnostic.sender_pid==getpid());
            assert(joined.diagnostic.sender_uid==geteuid());
            assert(!joined.observation.provenance.exact_request);
            assert(!joined.observation.provenance.verified_lds);
            assert(!joined.observation.provenance.legacy_receiver);
            c.owned=joined;c.matched.fetch_add(1);
        }
        if(result==R::LdsRequestSource::PAYLOAD_MISMATCH)c.mismatched.fetch_add(1);
        if(result==R::LdsRequestSource::CONFLICT) {
            assert(!c.source.current(c.owned));c.conflicts.fetch_add(1);
        }
        *out=R::AssistReadiness();
        return false; // This fixture has no measured-time/profile/receiver evidence.
    }
};
static Consumer consumer;
static R::AssistWorker* controller;
static void* run(void* root) {
    return R::run_worker_inputs(static_cast<const char*>(root),"unused.motion",channel,
                                geteuid(),controller,&consumer.source);
}
static std::string contents(const std::string& path) {
    std::ifstream in(path.c_str());std::ostringstream out;out<<in.rdbuf();return out.str();
}
static bool wait_text(const std::string& path,const char* text) {
    const uint64_t until=clock_ns(0)+3000000000ULL;
    while(clock_ns(0)<until) { if(contents(path).find(text)!=std::string::npos)return true;usleep(1000); }
    return false;
}
static bool wait_count(const std::atomic<unsigned>& count) {
    const uint64_t until=clock_ns(0)+3000000000ULL;
    while(clock_ns(0)<until) { if(count.load())return true;usleep(1000); }
    return false;
}
static void wait_paused() {
    const uint64_t until=clock_ns(0)+3000000000ULL;
    while(consumer.pause.load()!=2&&clock_ns(0)<until)usleep(1000);
    assert(consumer.pause.load()==2);
}
static void prepare_bus() {
    const A::BusBindings bindings={bus_fixture::create,bus_fixture::connect,
        bus_fixture::end,bus_fixture::end,bus_fixture::signal,bus_fixture::is_signal,A::BusEndpointApi()};
    assert(A::prepare_bus_hooks(bindings));
    assert(mx5_bus_create(bus_fixture::closed,0)==&bus_fixture::handles[0]);
    assert(mx5_bus_connect(&bus_fixture::handles[0],"authored",0,0)==1);
    // No LDS submission has used this real registry entry yet.
    assert(A::read_position_bus().connection.result==R::bus_trace::UNOBSERVED);
}
static void send_malformed() {
    const int fd=socket(AF_UNIX,SOCK_DGRAM|SOCK_NONBLOCK,0);assert(fd>=0);
    sockaddr_un address=sockaddr_un();address.sun_family=AF_UNIX;
    const size_t length=strlen(channel);memcpy(address.sun_path+1,channel,length);
    const socklen_t size=static_cast<socklen_t>(offsetof(sockaddr_un,sun_path)+1+length);
    assert(sendto(fd,"bad",3,MSG_DONTWAIT,reinterpret_cast<sockaddr*>(&address),size)==3);
    assert(!close(fd));
}
static void prepare() {
    const uint64_t now=clock_ns(0);
    expected=A::Observation();expected.kind=A::Observation::POSITION;
    expected.call_sequence=17;expected.prediction_generation=4;expected.mono_ns=now;
    expected.original_mode=expected.position.mode=1;
    expected.position.utc_seconds=1700000000ULL;
    expected.position.latitude_deg=35;expected.position.longitude_deg=129;
    expected.position.altitude_m=12;expected.position.horizontal=1;expected.position.vertical=1.5;
    expected.request_result=Q::OK;
    Q::Trace& t=expected.request_trace;
    t.request=Q::Token{17,1};t.worker=Q::Token{21,1};
    t.issue.observed_ns=now-1000;t.reply.observed_ns=now-100;
    t.issue.connection=R::bus_trace::Snapshot{R::bus_trace::CONNECTED,11,3};
    t.reply.connection=t.issue.connection;t.issue.bus_lifetime=3;t.issue.known=Q::ISSUE_BUS_LIFETIME;
    t.issue.endpoint.server_guid=Q::copy_text("source-fixture-guid");
    t.issue.endpoint.unique_name=Q::copy_text(":1.20");
    t.issue.wire.observed_ns=now-900;t.issue.wire.serial=7;
    t.issue.wire.known=t.issue.wire.endpoint_matched=true;
    t.reply.wire.known=true;t.reply.wire.type=2;t.reply.wire.serial=8;
    t.reply.wire.reply_serial=7;t.reply.wire.observed_ns=now-200;
    t.reply.wire.sender=Q::copy_text(":1.10");
}
static L::Record sideband() {
    L::Record r=L::Record();r.flags=111;r.observed_ns=clock_ns(0);
    r.send_result=1;r.reply_type=2;r.position=expected.position;
    r.wire.server_guid=expected.request_trace.issue.endpoint.server_guid;
    r.wire.client_unique=r.wire.destination=expected.request_trace.issue.endpoint.unique_name;
    r.wire.server_unique=expected.request_trace.reply.wire.sender;
    r.wire.request_serial=r.wire.reply_serial=7;r.wire.response_serial=8;
    r.field_lineage.lifetime=r.field_lineage.write_sequence=1;
    for(unsigned i=0;i<9;++i) {
        r.field_lineage.fields[i].write_sequence=1;
        r.field_lineage.fields[i].observed_ns=expected.request_trace.issue.observed_ns;
    }
    return r;
}
int main(int argc,char** argv) {
    assert(argc==2);alarm(15);const std::string scenario=argv[1];
    const bool discovery=scenario=="first_bus"||scenario=="startup_bus"||scenario=="bus_reconnect";
    assert(scenario=="position_first"||scenario=="sideband_first"||scenario=="mismatch"||
           scenario=="late_conflict"||scenario=="pre_stopped"||scenario=="malformed_recovery"||discovery);
    char root[]="/tmp/mx5dr-worker-lds-source-XXXXXX";assert(mkdtemp(root));
    const std::string logs=std::string(root)+"/logs",trace=logs+"/trace.0.jsonl";
    assert(!mkdir(logs.c_str(),0700));
    config.mode=3;config.max_log_bytes=8388608;config.max_log_files=3;hook_installed=true;
    snprintf(channel,sizeof channel,"mx5dr.worker.source.%ld",(long)getpid());prepare();
    R::AssistSource source={Consumer::pop,Consumer::readiness,&consumer};
    R::AssistWorker assist(mx5_dr_default_config(),source);controller=&assist;
    if(discovery) {
        if(scenario!="startup_bus")prepare_bus();
        consumer.pause.store(1);
    }
    if(scenario=="pre_stopped") {
        L::Record side=sideband();side.source_instance=88;side.sequence=1;
        L::Diagnostic diagnostic=L::Diagnostic();diagnostic.sender_pid=getpid();
        diagnostic.sender_uid=geteuid();diagnostic.received_ns=clock_ns(0);
        consumer.source.position(expected,clock_ns(0));
        consumer.source.sideband(side,diagnostic,clock_ns(0));
        assert(consumer.source.lookup(expected,&consumer.owned)==R::LdsRequestSource::MATCHED);
        assert(!mkdir((logs+"/capture.stop").c_str(),0700));run(root);
        assert(!consumer.source.current(consumer.owned));assert(!consumer.reads.load());
        assert(assist.status().state==R::ASSIST_STOPPED);assert(access(trace.c_str(),F_OK)!=0);
        assert(!rmdir((logs+"/capture.stop").c_str()));assert(!rmdir(logs.c_str()));assert(!rmdir(root));
        puts("worker LDS source pre_stopped PASS: existing evidence retired without reopening capture");return 0;
    }
    pthread_t thread;assert(!pthread_create(&thread,0,run,root));
    if(discovery) {
        wait_paused();
        if(scenario=="startup_bus")prepare_bus();
        A::observe_position_bus(&bus_fixture::handles[0]);
        prepare();
        expected.request_trace.issue.connection=A::read_bus_connection(&bus_fixture::handles[0]);
        expected.request_trace.reply.connection=expected.request_trace.issue.connection;
        expected.request_trace.issue.bus_lifetime=expected.request_trace.issue.connection.lifetime;
        assert(A::read_position_bus().connection.result==R::bus_trace::CONNECTED);
    } else {
        assert(wait_text(trace,"\"status\":\"opened\""));assert(wait_count(consumer.reads));
    }
    L::Sender sender;assert(sender.open_channel(channel,88));L::Record side=sideband();
    char raw[R::OBSERVATION_JSON_CAPACITY];assert(format_observation(raw,sizeof raw,expected));
    if(discovery) {
        // Both halves predate the next worker poll, as in the original LDS run.
        sink(&expected,0);assert(sender.try_send(side));consumer.pause.store(3);
    } else if(scenario=="sideband_first") {
        assert(sender.try_send(side));assert(wait_text(trace,"\"kind\":\"lds_sideband\""));
        assert(!consumer.matched.load());sink(&expected,0);
    } else {
        sink(&expected,0);assert(wait_text(trace,raw));assert(!consumer.matched.load());
        if(scenario=="malformed_recovery") {
            // A rejected local packet cannot erase an already retained exact
            // request or require a new issue before its legitimate reply.
            send_malformed();assert(wait_text(trace,"\"reason\":\"bad_record\""));
            assert(!consumer.matched.load());
        }
        if(scenario=="mismatch")side.position.altitude_m+=1;
        assert(sender.try_send(side));
    }
    if(scenario=="mismatch")assert(wait_count(consumer.mismatched));
    else assert(wait_count(consumer.matched));
    if(scenario=="bus_reconnect") {
        consumer.pause.store(1);wait_paused();
        mx5_bus_disconnect(&bus_fixture::handles[0]);
        assert(mx5_bus_connect(&bus_fixture::handles[0],"authored",0,0)==1);
        A::observe_position_bus(&bus_fixture::handles[0]);
        // Replaying the old issue after a real observed lifetime change must
        // neither preserve the old lease nor create a new match.
        sink(&expected,0);assert(sender.try_send(side));consumer.pause.store(3);
        assert(wait_count(consumer.revoked));
        consumer.pause.store(1);wait_paused();
        assert(consumer.source.status().matches==1);
        assert(consumer.source.status().rejected==2);
        prepare();++expected.call_sequence;++expected.request_trace.request.id;
        ++expected.request_trace.worker.id;++expected.request_trace.issue.wire.serial;
        ++expected.request_trace.reply.wire.serial;++expected.request_trace.reply.wire.reply_serial;
        expected.request_trace.issue.connection=A::read_bus_connection(&bus_fixture::handles[0]);
        expected.request_trace.reply.connection=expected.request_trace.issue.connection;
        expected.request_trace.issue.bus_lifetime=expected.request_trace.issue.connection.lifetime;
        side=sideband();
        side.wire.request_serial=side.wire.reply_serial=expected.request_trace.issue.wire.serial;
        side.wire.response_serial=expected.request_trace.reply.wire.serial;
        consumer.matched.store(0);sink(&expected,0);assert(sender.try_send(side));consumer.pause.store(3);
        assert(wait_count(consumer.matched));
    }
    if(scenario=="late_conflict") {
        side.position.latitude_deg+=1;side.observed_ns=clock_ns(0);
        assert(sender.try_send(side));assert(wait_count(consumer.conflicts));
    }
    assert(wait_text(trace,raw));
    assert(!mkdir((logs+"/capture.stop").c_str(),0700));assert(!pthread_join(thread,0));
    assert(assist.status().state==R::ASSIST_STOPPED);
    assert(assist.status().published==0&&consumer.polls.load()==0);
    assert(A::mode()!=A::ASSIST);
    if(scenario=="mismatch")assert(!consumer.matched.load());
    else assert(!consumer.source.current(consumer.owned)); // Stop ends the source lifetime.
    const std::string text=contents(trace);assert(text.find(raw)!=std::string::npos);
    assert(text.find("\"kind\":\"lds_sideband\"")!=std::string::npos);
    assert(text.find("\"kind\":\"capture_end\"")!=std::string::npos);
    assert(text.find("\"lds_request_source\":{\"association_only\":true")!=std::string::npos);
    if(scenario=="malformed_recovery") {
        assert(text.find("\"status\":\"rejected\"")!=std::string::npos);
        assert(text.find("\"reason\":\"bad_record\"")!=std::string::npos);
        assert(consumer.source.status().positions==1&&consumer.source.status().records==1);
        assert(consumer.source.status().conflicts==0);
    }
    if(discovery) {
        assert(consumer.source.status().matches==(scenario=="bus_reconnect"?2u:1u));
        assert(consumer.source.status().rejected==(scenario=="bus_reconnect"?2u:0u));
    }
    assert(!unlink(trace.c_str()));assert(!unlink((logs+"/capture.done").c_str()));
    assert(!rmdir((logs+"/capture.stop").c_str()));assert(!rmdir(logs.c_str()));assert(!rmdir(root));
    printf("worker LDS source %s PASS: source consumes owned evidence; physical readiness remains unknown\n",scenario.c_str());
}
