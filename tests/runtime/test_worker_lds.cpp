// Authored input through the real AA worker/journal and credentialed socket.
// No OEM producer, physical qualification, or phone acceptance is inferred.
#include "../../src/runtime/runtime.cpp"
#include "runtime/lds_sideband.h"
#include <cassert>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
namespace L=mx5::runtime::lds_sideband;
static char lds_channel[80];
static bool wrong_uid;
static void* run(void* root) {
    return mx5::runtime::run_worker_channels(static_cast<const char*>(root),"unused.motion",lds_channel,
                                            geteuid()+(wrong_uid?1:0),0);
}
static std::string contents(const std::string& path) {
    std::ifstream file(path.c_str());std::ostringstream out;out<<file.rdbuf();return out.str();
}
static bool wait_for(const std::string& path,const char* text) {
    const uint64_t until=clock_ns(0)+3000000000ULL;
    while(clock_ns(0)<until) { if(contents(path).find(text)!=std::string::npos)return true;usleep(1000); }
    return false;
}
static void request_stop(const std::string& path) {
    // Product stop marker is a directory, exactly as packaging/finish creates.
    assert(!mkdir(path.c_str(),0700));
}
static size_t count(const std::string& text,const char* needle) {
    size_t n=0,pos=0;while((pos=text.find(needle,pos))!=std::string::npos) { ++n;pos+=strlen(needle); }return n;
}
struct AlwaysReady {
    unsigned calls;
    AlwaysReady():calls(0) {}
    L::ReceiveResult receive(L::Record* record,L::Diagnostic* diagnostic) {
        ++calls;*record=L::Record();record->source_instance=1;record->sequence=calls;
        *diagnostic=L::Diagnostic();return L::RECORD;
    }
    void close_channel() { assert(false); }
};
static void malformed() {
    const int fd=socket(AF_UNIX,SOCK_DGRAM|SOCK_NONBLOCK,0);assert(fd>=0);
    sockaddr_un address=sockaddr_un();address.sun_family=AF_UNIX;
    const size_t length=strlen(lds_channel);memcpy(address.sun_path+1,lds_channel,length);
    const socklen_t size=static_cast<socklen_t>(offsetof(sockaddr_un,sun_path)+1+length);
    assert(sendto(fd,"bad",3,MSG_DONTWAIT,reinterpret_cast<sockaddr*>(&address),size)==3);assert(!close(fd));
}
int main(int argc,char** argv) {
    assert(argc==2);alarm(15);const std::string scenario=argv[1];
    assert(scenario=="capture"||scenario=="occupied"||scenario=="pre_stopped"||
           scenario=="bounded"||scenario=="malformed"||scenario=="wrong_uid");
    char root[]="/tmp/mx5dr-worker-lds-XXXXXX";assert(mkdtemp(root));
    const std::string logs=std::string(root)+"/logs",trace=logs+"/trace.0.jsonl";
    assert(!mkdir(logs.c_str(),0700));
    config.mode=1;config.max_log_bytes=8388608;config.max_log_files=3;hook_installed=false;
    snprintf(lds_channel,sizeof lds_channel,"mx5dr.worker.lds.%ld",(long)getpid());
    if(scenario=="bounded") {
        AlwaysReady input;
        { Journal journal(root);assert(mx5::runtime::drain_lds(journal,input)==L::DRAIN_LIMIT);journal.flush(); }
        assert(input.calls==L::DRAIN_LIMIT);const std::string text=contents(trace);
        assert(count(text,"\"kind\":\"lds_sideband\"")==L::DRAIN_LIMIT);
        assert(count(text,"\"status\":\"drain_limit\"")==1);
        assert(!unlink(trace.c_str()));assert(!rmdir(logs.c_str()));assert(!rmdir(root));
        puts("worker LDS bounded PASS: exactly 16 observations per drain");return 0;
    }
    if(scenario=="pre_stopped") {
        request_stop(logs+"/capture.stop");run(root);assert(access(trace.c_str(),F_OK)!=0);
        assert(!rmdir((logs+"/capture.stop").c_str()));assert(!rmdir(logs.c_str()));assert(!rmdir(root));
        puts("worker LDS pre_stopped PASS");return 0;
    }
    L::Receiver occupied;
    wrong_uid=scenario=="wrong_uid";
    if(scenario=="occupied")assert(occupied.open_channel(lds_channel,geteuid()));
    pthread_t worker;assert(!pthread_create(&worker,0,run,root));
    assert(wait_for(trace,"\"kind\":\"health\""));
    assert(wait_for(trace,scenario=="occupied"?"\"status\":\"unavailable\"":"\"status\":\"opened\""));
    A::Observation raw=A::Observation();raw.kind=A::Observation::POSITION;raw.original_mode=1;
    raw.request_result=mx5::runtime::request_trace::NOT_FOUND;
    raw.call_sequence=17;raw.prediction_generation=4;raw.mono_ns=clock_ns(0);
    raw.position.mode=1;raw.position.latitude_deg=35;raw.position.longitude_deg=129;
    raw.position.altitude_m=12;raw.position.horizontal=1;raw.position.vertical=1.5;
    char expected[mx5::runtime::OBSERVATION_JSON_CAPACITY];assert(format_observation(expected,sizeof expected,raw));
    sink(&raw,0);
    L::Sender sender;assert(sender.open_channel(lds_channel,88));L::Record side=L::Record();
    side.flags=1;side.observed_ns=1;side.field_lineage.lifetime=1;side.field_lineage.write_sequence=1;
    side.position=raw.position;
    if(scenario=="malformed")malformed();
    assert(sender.try_send(side));
    assert(wait_for(trace,expected));
    const bool captured=scenario=="capture"||scenario=="malformed";
    if(captured)assert(wait_for(trace,"\"kind\":\"lds_sideband\""));
    if(scenario=="malformed"||wrong_uid)assert(wait_for(trace,"\"status\":\"rejected\""));
    request_stop(logs+"/capture.stop");assert(!pthread_join(worker,0));
    const std::string recorded=contents(trace);
    assert(recorded.find(expected)!=std::string::npos);assert(count(recorded,"\"kind\":\"position\"")==1);
    assert(count(recorded,"\"kind\":\"lds_sideband\"")==(captured?1U:0U));
    if(scenario=="malformed")assert(recorded.find("\"reason\":\"bad_record\"")!=std::string::npos);
    if(wrong_uid)assert(recorded.find("\"reason\":\"credentials_mismatch\"")!=std::string::npos);
    assert(recorded.find("\"kind\":\"capture_end\"")!=std::string::npos);
    assert(scenario=="occupied"||!sender.try_send(side));
    // Retain a copy only when the harness explicitly requests evidence.
    const char* output=getenv("MX5DR_WORKER_LDS_OUTPUT");
    if(output) { std::ofstream out(output);out<<recorded;assert(out.good()); }
    assert(!unlink(trace.c_str()));assert(!rmdir((logs+"/capture.stop").c_str()));
    assert(!unlink((logs+"/capture.done").c_str()));
    assert(!rmdir(logs.c_str()));assert(!rmdir(root));
    printf("worker LDS %s PASS: raw POSITION retained; association independent; capture closed\n",scenario.c_str());
}
