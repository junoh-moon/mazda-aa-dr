// Authored transport identities and OEM endpoint through the actual runtime,
// shared-map handoff, one-shot request Ledger and adapter. No physical source
// qualification, original firmware execution or ASSIST enablement is claimed.
#include "adapter/lds_hooks.h"
#include "adapter/bus_hooks.h"
#include "adapter/session_hooks.h"
#include "runtime/worker.h"
#include "runtime/worker_thread.h"
#include "runtime/lds_association_protocol.h"
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <limits>
#include <pthread.h>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
// These test-only scheduling seams preserve each real snapshot and sleep.
// Delay a completed outer read across a real lifecycle call, then let the
// unmodified worker's inner POSITION check consume the new boundary.
namespace mx5 { namespace adapter {
runtime::session_trace::Snapshot scheduled_issue_session();
runtime::bus_trace::Boundary scheduled_position_bus();
} }
static int scheduled_nanosleep(const timespec*,timespec*);
#define read_issue_session scheduled_issue_session
#define read_position_bus scheduled_position_bus
#define nanosleep scheduled_nanosleep
#include "../../src/runtime/runtime.cpp"
#undef nanosleep
#undef read_position_bus
#undef read_issue_session
#include "../runtime/model_bus_fixture.h"

#ifdef NDEBUG
#error Runtime LDS association regressions require assertions
#endif
namespace R=mx5::runtime;
namespace L=R::lds_association;
namespace Q=R::request_trace;
namespace C=mx5::sensors::nmea_course_token;
namespace {
const uint64_t MS=1000000ULL;
Q::Ledger requests;
Q::WorkerContext* current_request;
unsigned request_reads,sends,callbacks;
A::Observation last_position,last_send;
A::VehicleData* borrowed;
unsigned char original_payload[48];
enum BoundaryCase { NO_BOUNDARY, BUS_BOUNDARY, SESSION_BOUNDARY };
BoundaryCase boundary_case=NO_BOUNDARY;
thread_local bool runtime_owner=false;
std::atomic<unsigned> idle_gate(0),boundary_step(0);
const unsigned IDLE_ARMED=6; // Between the test's request (1) and the park (2).
uint64_t outer_revision,inner_revision;
unsigned lifecycle_calls;
int session_handle;
void* session_storage;
void session_status(void*,void*) {}
int32_t session_create(const char*,void*,const A::SessionCallbacks*,void** out) {
    *out=&session_handle;return 0;
}
int32_t session_destroy(void** out) {
    assert(*out==&session_handle);*out=0;++lifecycle_calls;return 0;
}
void wait_gate(unsigned expected) {
    const uint64_t deadline=clock_ns(0)+2000*MS;
    while(idle_gate.load(std::memory_order_acquire)!=expected&&clock_ns(0)<deadline)usleep(1000);
    assert(idle_gate.load(std::memory_order_acquire)==expected);
}

Q::Result request_reader(const void* raw,Q::Trace* out,void*) {
    ++request_reads;
    assert(current_request);
    const Q::Result result=requests.position_take(current_request,raw,out);
    errno=E2BIG;
    return result;
}
void observe(const A::Observation* o,void*) {
    sink(o,0); // Real queue consumed and journaled by run_worker_association.
    assert(!o->provenance.source_epoch&&!o->provenance.session_epoch);
    assert(!o->provenance.exact_request&&!o->provenance.verified_lds&&!o->provenance.legacy_receiver);
    if(o->kind==A::Observation::POSITION)last_position=*o;
    else last_send=*o;
    errno=ENOTTY;
}
int32_t original_send(void* session,A::VehicleData* data) {
    assert(session==&sends&&errno==EDOM&&data==borrowed);
    assert(data->type==1&&data->length==48&&data->payload==original_payload);
    const unsigned char* payload=static_cast<const unsigned char*>(data->payload);
    for(unsigned i=0;i<48;++i)assert(payload[i]==static_cast<unsigned char>(i+1));
    ++sends;
    errno=ERANGE;
    return -731;
}
void put32(unsigned char* p,uint32_t value) {
    for(unsigned i=0;i<4;++i)p[i]=static_cast<unsigned char>(value>>(i*8));
}
void put64(unsigned char* p,uint64_t value) {
    for(unsigned i=0;i<8;++i)p[i]=static_cast<unsigned char>(value>>(i*8));
}
void encode(unsigned char raw[72],const A::PositionInput& p) {
    std::memset(raw,0,72);
    put32(raw,uint32_t(p.mode));put64(raw+8,p.utc_seconds);
    std::memcpy(raw+16,&p.latitude_deg,8);std::memcpy(raw+24,&p.longitude_deg,8);
    put32(raw+32,uint32_t(p.altitude_m));
    std::memcpy(raw+40,&p.heading_deg,8);std::memcpy(raw+48,&p.velocity_kmh,8);
    std::memcpy(raw+56,&p.horizontal,8);std::memcpy(raw+64,&p.vertical,8);
}
A::LdsLockedSend record(unsigned sequence,C::Presence presence,C::RmcStatus status) {
    A::LdsLockedSend row=A::LdsLockedSend();
    row.stage=L::LOCKED_FOR_SEND;row.reply_type=2;
    row.wire.server_guid=Q::copy_text("runtime-transport-guid");
    row.wire.client_unique=Q::copy_text(":1.20");
    row.wire.server_unique=Q::copy_text(":1.10");
    row.wire.destination=row.wire.client_unique;
    row.wire.request_serial=row.wire.reply_serial=10+sequence;
    row.wire.response_serial=100+sequence;
    row.field_lineage.lifetime=51;row.field_lineage.write_sequence=sequence;
    row.position.mode=1;row.position.utc_seconds=1700000000ULL+sequence;
    row.position.latitude_deg=37;row.position.longitude_deg=127;
    row.position.altitude_m=-23;row.position.heading_deg=presence==C::UNKNOWN?42:0;
    row.position.velocity_kmh=36;row.position.horizontal=1.25;row.position.vertical=2.5;
    row.field_lineage.heading_presence=presence;row.field_lineage.heading_rmc_status=status;
    return row;
}
void check_owned(const A::Observation& o,const A::LdsLockedSend& row,
                 Q::Token request,Q::Token worker) {
    assert(o.request_result==Q::OK);
    assert(o.request_trace.request.id==request.id&&o.request_trace.request.epoch==request.epoch);
    assert(o.request_trace.worker.id==worker.id&&o.request_trace.worker.epoch==worker.epoch);
    const L::Owned& owned=o.lds_association;
    if(owned.result!=L::MATCHED_LOCKED_FOR_SEND) {
        assert(owned.result==L::UNAVAILABLE&&owned.stage==L::NO_STAGE);
        assert(!owned.call_sequence&&!owned.source_instance&&!owned.request_id&&!owned.write_sequence);
        return;
    }
    assert(owned.stage==L::LOCKED_FOR_SEND&&owned.layout_version==L::protocol::VERSION&&owned.view_revision);
    assert(owned.call_sequence==o.call_sequence&&owned.prediction_generation==o.prediction_generation);
    assert(owned.source_instance==41&&owned.record_sequence&&owned.map_loss_epoch);
    assert(owned.locked_observed_ns==row.observed_ns);
    assert(owned.request_id==request.id&&owned.request_epoch==request.epoch);
    assert(owned.worker_id==worker.id&&owned.worker_epoch==worker.epoch);
    assert(owned.cache_lifetime==51&&owned.write_sequence==row.field_lineage.write_sequence);
    for(unsigned i=0;i<9;++i) {
        assert(owned.fields[i].write_sequence==row.field_lineage.fields[i].write_sequence);
        assert(owned.fields[i].observed_ns==row.field_lineage.fields[i].observed_ns);
    }
    assert(owned.heading_presence==row.field_lineage.heading_presence);
    assert(owned.heading_rmc_status==row.field_lineage.heading_rmc_status);
}
bool callback(L::Publisher& publisher,bool publish=true,C::Presence presence=C::UNKNOWN,
              C::RmcStatus status=C::RMC_UNKNOWN) {
    const unsigned sequence=++callbacks;
    A::LdsLockedSend row=record(sequence,presence,status);
    Q::Issue issue=Q::Issue();
    issue.observed_ns=clock_ns(0);
    issue.connection=R::bus_trace::Snapshot{R::bus_trace::CONNECTED,11,3};
    issue.bus_lifetime=3;issue.known=Q::ISSUE_BUS_LIFETIME;
    issue.endpoint.server_guid=row.wire.server_guid;issue.endpoint.unique_name=row.wire.client_unique;
    issue.route.destination=Q::copy_text("com.jci.lds.data");
    issue.route.path=Q::copy_text("/com/jci/lds/data");
    issue.route.interface_name=Q::copy_text("com.jci.lds.data");
    issue.route.member=Q::copy_text("GetPosition");
    issue.wire.known=issue.wire.endpoint_matched=true;
    issue.wire.observed_ns=issue.observed_ns;issue.wire.serial=row.wire.request_serial;
    int method=0,worker_key=0;
    Q::Token request=Q::Token(),reply=Q::Token(),worker=Q::Token();
    assert(requests.request_begin(&method,issue,&request)==Q::OK);
    row.observed_ns=clock_ns(0);
    for(unsigned i=0;i<9;++i) {
        row.field_lineage.fields[i].write_sequence=sequence;
        row.field_lineage.fields[i].observed_ns=issue.observed_ns;
    }
    if(publish)assert(publisher.publish(row));
    Q::Reply response=Q::Reply();
    response.connection=issue.connection;response.observed_ns=clock_ns(0);
    response.wire.known=true;response.wire.type=2;
    response.wire.observed_ns=response.observed_ns;
    response.wire.serial=row.wire.response_serial;response.wire.reply_serial=row.wire.reply_serial;
    response.wire.sender=row.wire.server_unique;
    assert(requests.reply_enter(&method,response,&reply)==Q::OK&&reply.id==request.id);
    unsigned char raw[72];encode(raw,row.position);
    assert(requests.worker_post(&worker_key,raw,request,&worker)==Q::OK);
    assert(requests.request_end(&method)==Q::OK); // queued ownership survives original method end
    Q::WorkerContext context;
    assert(requests.worker_enter(&worker_key,&context)==Q::OK);
    current_request=&context;
    const unsigned before_reads=request_reads,before_sends=sends;
    A::VehicleData data={1,original_payload,48};borrowed=&data;
    errno=EDOM;A::position_enter(0,raw);assert(errno==EDOM);
    assert(A::send_vehicle_data(&sends,&data)==-731&&errno==ERANGE);
    A::position_leave();assert(errno==ERANGE);
    current_request=0;requests.worker_leave(&context);
    assert(requests.worker_destroy(&worker_key)==Q::NOT_FOUND); // worker_enter consumed it
    assert(request_reads==before_reads+1&&sends==before_sends+1);
    assert(last_position.kind==A::Observation::POSITION&&last_send.kind==A::Observation::SEND);
    assert(last_position.call_sequence==last_send.call_sequence);
    assert(!std::memcmp(&last_position.lds_association,&last_send.lds_association,sizeof(L::Owned)));
    assert(last_send.choice!=A::DR_REPLACEMENT&&A::mode()==A::OBSERVE);
    check_owned(last_position,row,request,worker);check_owned(last_send,row,request,worker);
    return last_position.lds_association.result==L::MATCHED_LOCKED_FOR_SEND;
}
std::string read_file(const std::string& path) {
    std::ifstream f(path.c_str());
    return std::string(std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>());
}
bool read_captured(const A::Observation& o,L::Owned* out) {
    // Read the immutable values already obtained from the actual callback;
    // this lookup does not re-consume its request Ledger context.
    const A::PositionContext context={o.position,o.request_result,o.request_trace,
                                     o.call_sequence,o.prediction_generation,0};
    return read_inline_association(context,out,0);
}
std::string journal_association(const std::string& journal,const char* kind,
                                const A::Observation& observation) {
    char prefix[160];
    ::snprintf(prefix,sizeof prefix,"{\"kind\":\"%s\",\"call\":%u,\"generation\":%u,",
               kind,observation.call_sequence,observation.prediction_generation);
    const size_t row=journal.find(prefix);assert(row!=std::string::npos);
    const size_t end=journal.find('\n',row);assert(end!=std::string::npos);
    const std::string line=journal.substr(row,end-row);
    const std::string key="\"lds_association\":";
    const size_t field=line.find(key);assert(field!=std::string::npos);
    const size_t begin=field+key.size();assert(line[begin]=='{');
    unsigned depth=0;
    for(size_t i=begin;i<line.size();++i) {
        if(line[i]=='{')++depth;
        else if(line[i]=='}'&&!--depth)return line.substr(begin,i-begin+1);
    }
    assert(false);return std::string();
}
void check_journal_association(const std::string& journal,const A::Observation& first,
                               const A::Observation& matched) {
    assert(journal_association(journal,"position",first)=="{\"result\":\"unavailable\"}");
    assert(journal_association(journal,"send",first)=="{\"result\":\"unavailable\"}");
    const std::string position=journal_association(journal,"position",matched);
    assert(position==journal_association(journal,"send",matched));
    assert(position.find("\"result\":\"matched_locked_for_send\"")!=std::string::npos);
    assert(position.find("\"stage\":\"locked_for_send\"")!=std::string::npos);
    const L::Owned& o=matched.lds_association;
    const char* presence=o.heading_presence==C::EMPTY?"empty":o.heading_presence==C::PRESENT?"present":"unknown";
    assert(position.find(std::string("\"heading_presence\":\"")+presence+"\"")!=std::string::npos);
    const char* status[]={"unknown","empty","a","v","other"};
    assert(o.heading_rmc_status<=C::RMC_OTHER);
    assert(position.find(std::string("\"heading_rmc_status\":\"")+status[o.heading_rmc_status]+"\"")!=std::string::npos);
    const char* names[]={"call","generation","revision","layout","source_instance",
        "record_sequence","locked_observed_ns","map_loss_epoch","cache_lifetime","write_sequence"};
    const uint64_t values[]={o.call_sequence,o.prediction_generation,o.view_revision,o.layout_version,
        o.source_instance,o.record_sequence,o.locked_observed_ns,o.map_loss_epoch,o.cache_lifetime,o.write_sequence};
    for(unsigned i=0;i<sizeof values/sizeof values[0];++i) {
        char expected[100];::snprintf(expected,sizeof expected,"\"%s\":%llu,",names[i],
                                    static_cast<unsigned long long>(values[i]));
        assert(position.find(expected)!=std::string::npos);
    }
    char tokens[180];::snprintf(tokens,sizeof tokens,"\"request\":[%llu,%llu],\"worker\":[%llu,%llu]",
        static_cast<unsigned long long>(o.request_id),static_cast<unsigned long long>(o.request_epoch),
        static_cast<unsigned long long>(o.worker_id),static_cast<unsigned long long>(o.worker_epoch));
    assert(position.find(tokens)!=std::string::npos);
    std::string fields="\"fields\":[";
    for(unsigned i=0;i<9;++i) {
        char pair[96];::snprintf(pair,sizeof pair,"%s[%llu,%llu]",i?",":"",
            static_cast<unsigned long long>(o.fields[i].write_sequence),
            static_cast<unsigned long long>(o.fields[i].observed_ns));
        fields+=pair;
    }
    fields+="]";assert(position.find(fields)!=std::string::npos);
}
struct Running { const char* root;char motion[96],sideband[96],association[96];R::LdsRequestSource* source; };
void* run(void* raw) {
    Running& r=*static_cast<Running*>(raw);
    runtime_owner=true;
    return R::run_worker_association(r.root,r.motion,r.sideband,geteuid(),0,r.source,r.association);
}
void wait_boot(const std::string& trace) {
    const uint64_t deadline=clock_ns(0)+2000*MS;
    while(clock_ns(0)<deadline&&read_file(trace).find("\"kind\":\"boot\"")==std::string::npos)usleep(5000);
    assert(read_file(trace).find("\"kind\":\"boot\"")!=std::string::npos);
}
void offer(L::Publisher& publisher,const char* channel) {
    bool offered=false;
    // The worker flushes the boot row before opening its optional channels.
    // Respect the real one-second offer limit; no special test-side retry API.
    for(unsigned attempt=0;attempt<3&&!offered;++attempt) {
        offered=publisher.offer(channel,clock_ns(0));
        if(!offered)usleep(1000000);
    }
    assert(offered); // A runnable failure here exposes missing worker channel adoption.
}
void wait_match(L::Publisher& publisher) {
    bool matched=false;
    // Each try is a new request/token/serial and real one-shot callback.
    // Keep the bounded probe below the 64-record map capacity.
    for(unsigned attempt=0;attempt<50&&!matched;++attempt) {
        usleep(10000);matched=callback(publisher);
    }
    assert(matched);
}
void remove_logs(const std::string& logs) {
    DIR* dir=opendir(logs.c_str());assert(dir);
    dirent* item;
    while((item=readdir(dir))) {
        if(item->d_name[0]=='.')continue;
        const std::string path=logs+"/"+item->d_name;
        struct stat st;assert(!lstat(path.c_str(),&st));
        if(S_ISDIR(st.st_mode))assert(!rmdir(path.c_str()));
        else assert(!unlink(path.c_str()));
    }
    closedir(dir);assert(!rmdir(logs.c_str()));
}
void formatter_bounds() {
    // Conservative serialization bounds, including diagnostic malformed Text
    // and numeric extremes; this is not an admissible physical/map record.
    A::Observation o=A::Observation();o.kind=A::Observation::POSITION;o.request_result=Q::OK;
    o.call_sequence=o.prediction_generation=o.type=o.length=UINT32_MAX;
    o.mono_ns=UINT64_MAX;o.original_mode=o.result=INT32_MIN;
    o.choice=A::DR_REPLACEMENT;o.reason=A::BAD_PROVENANCE;o.has_payload=true;
    std::memset(o.original,255,sizeof o.original);std::memset(o.outgoing,255,sizeof o.outgoing);
    Q::Trace& t=o.request_trace;
    t.request.id=t.request.epoch=t.worker.id=t.worker.epoch=UINT64_MAX;
    t.issue.observed_ns=t.reply.observed_ns=UINT64_MAX;
    t.issue.bus_lifetime=t.issue.session_lifetime=t.issue.session_event=UINT64_MAX;
    t.issue.known=7;t.issue.session_state=INT32_MIN;
    t.issue.connection=R::bus_trace::Snapshot{R::bus_trace::CONNECTED,UINT32_MAX,UINT64_MAX};
    t.reply.connection=t.issue.connection;
    t.issue.session_context=R::session_trace::Snapshot{
        R::session_trace::OBSERVED,UINT32_MAX,UINT32_MAX,INT32_MIN,true,UINT64_MAX};
    o.send_session=t.issue.session_context;
    Q::Text text=Q::Text();text.known=true;std::memset(text.bytes,1,sizeof text.bytes);
    t.issue.route.destination=t.issue.route.path=t.issue.route.interface_name=t.issue.route.member=text;
    t.issue.endpoint.server_guid=t.issue.endpoint.unique_name=text;
    t.reply.sender=t.reply.error_name=t.reply.wire.sender=t.reply.wire.error_name=text;
    t.reply.type_known=t.reply.wire_serial_known=true;t.reply.type=INT32_MIN;t.reply.wire_serial=UINT32_MAX;
    t.issue.wire.known=t.reply.wire.known=true;t.issue.wire.endpoint_matched=true;
    t.issue.wire.observed_ns=t.reply.wire.observed_ns=UINT64_MAX;
    t.issue.wire.serial=t.reply.wire.serial=t.reply.wire.reply_serial=UINT32_MAX;t.reply.wire.type=INT32_MIN;
    o.position.mode=INT32_MIN;o.position.utc_seconds=UINT64_MAX;o.position.altitude_m=INT32_MIN;
    o.position.latitude_deg=o.position.heading_deg=o.position.horizontal=std::numeric_limits<double>::max();
    o.position.longitude_deg=o.position.velocity_kmh=o.position.vertical=-std::numeric_limits<double>::max();
    L::Owned& a=o.lds_association;a.result=L::MATCHED_LOCKED_FOR_SEND;a.stage=L::LOCKED_FOR_SEND;
    a.call_sequence=a.prediction_generation=a.view_revision=a.layout_version=UINT32_MAX;
    a.source_instance=a.record_sequence=a.locked_observed_ns=a.map_loss_epoch=UINT64_MAX;
    a.request_id=a.request_epoch=a.worker_id=a.worker_epoch=a.cache_lifetime=a.write_sequence=UINT64_MAX;
    for(unsigned i=0;i<9;++i)a.fields[i].write_sequence=a.fields[i].observed_ns=UINT64_MAX;
    char full[2][16384];size_t required[2];
    for(unsigned kind=0;kind<2;++kind) {
        o.kind=kind?A::Observation::SEND:A::Observation::POSITION;
        assert(format_observation(full[kind],sizeof full[kind],o));
        required[kind]=std::strlen(full[kind])+1;
        std::fprintf(stderr,"maximum association %s JSON: %zu bytes; worker capacity %u\n",
                     kind?"SEND":"POSITION",required[kind],unsigned(R::OBSERVATION_JSON_CAPACITY));
    }
    for(unsigned kind=0;kind<2;++kind) {
        o.kind=kind?A::Observation::SEND:A::Observation::POSITION;
        unsigned char before[sizeof o];std::memcpy(before,&o,sizeof o);
        char worker[R::OBSERVATION_JSON_CAPACITY+2];std::memset(worker,0x5a,sizeof worker);
        assert(format_observation(worker+1,R::OBSERVATION_JSON_CAPACITY,o));
        assert(worker[0]==0x5a&&worker[sizeof worker-1]==0x5a);
        assert(!std::strcmp(worker+1,full[kind]));
        char exact[16384];std::memset(exact,0x5a,sizeof exact);
        assert(format_observation(exact+1,required[kind],o));
        assert(exact[0]==0x5a&&exact[required[kind]+1]==0x5a&&!std::strcmp(exact+1,full[kind]));
        std::memset(exact,0x5a,sizeof exact);
        assert(!format_observation(exact+1,required[kind]-1,o));
        assert(exact[0]==0x5a&&exact[required[kind]-1]==0&&exact[required[kind]]==0x5a);
        assert(!std::memcmp(before,&o,sizeof o));
    }
    std::puts("PASS runtime LDS association bounds: actual worker capacity, exact/N-1 and canaries");
}
}
static int scheduled_nanosleep(const timespec* delay,timespec* remaining) {
    if(runtime_owner) {
        // Park (2) only at the idle sleep that ends a complete turn begun
        // after the request (1 -> IDLE_ARMED here, IDLE_ARMED -> 2 at the
        // next idle sleep). The turn that was already past its queue drain
        // when the test asked could otherwise park with the last wait_match
        // callback's POSITION still queued, and the boundary turn then counted
        // two positions (2026-10-10, QEMU on a loaded host).
        unsigned pending=1;
        unsigned armed=IDLE_ARMED;
        if(idle_gate.compare_exchange_strong(pending,IDLE_ARMED)) {
        } else if(idle_gate.compare_exchange_strong(armed,2)) {
            while(idle_gate.load(std::memory_order_acquire)!=3)usleep(1000);
        } else if(idle_gate.load(std::memory_order_acquire)==3&&
                  boundary_step.load(std::memory_order_acquire)==3) {
            idle_gate.store(4,std::memory_order_release);
            while(idle_gate.load(std::memory_order_acquire)!=5)usleep(1000);
        }
    }
    return ::nanosleep(delay,remaining);
}
namespace mx5 { namespace adapter {
runtime::session_trace::Snapshot scheduled_issue_session() {
    const runtime::session_trace::Snapshot actual=read_issue_session();
    if(runtime_owner&&boundary_case==SESSION_BOUNDARY) {
        const unsigned step=boundary_step.load(std::memory_order_acquire);
        if(step==1) {
            outer_revision=actual.revision;
            assert(actual.result==runtime::session_trace::OBSERVED);
            assert(!mx5_session_destroy(&session_storage));
            boundary_step.store(2,std::memory_order_release);
        } else if(step==2) {
            inner_revision=actual.revision;assert(inner_revision!=outer_revision);
            boundary_step.store(3,std::memory_order_release);
        }
    }
    return actual;
}
runtime::bus_trace::Boundary scheduled_position_bus() {
    const runtime::bus_trace::Boundary actual=read_position_bus();
    if(runtime_owner&&boundary_case==BUS_BOUNDARY) {
        const unsigned step=boundary_step.load(std::memory_order_acquire);
        if(step==1) {
            outer_revision=actual.revision;
            assert(actual.connection.result==runtime::bus_trace::CONNECTED);
            mx5_bus_disconnect(&bus_fixture::handles[0]);++lifecycle_calls;
            boundary_step.store(2,std::memory_order_release);
        } else if(step==2) {
            inner_revision=actual.revision;assert(inner_revision!=outer_revision);
            assert(actual.connection.result==runtime::bus_trace::NONE);
            assert(read_bus_connection(&bus_fixture::handles[0]).result==runtime::bus_trace::DISCONNECTED);
            boundary_step.store(3,std::memory_order_release);
        }
    }
    return actual;
}
} }
int main(int argc,char** argv) {
    assert(argc==2);alarm(15);
    const std::string scenario=argv[1];
    if(scenario=="bounds") { formatter_bounds();return 0; }
    assert(scenario=="adopted"||scenario=="journal"||scenario=="freeze"||scenario=="audit"||
           scenario=="journal_failure"||scenario=="pre_stopped"||scenario=="fork"||
           scenario=="drain_bus"||scenario=="drain_session");
    if(scenario=="drain_bus")boundary_case=BUS_BOUNDARY;
    if(scenario=="drain_session")boundary_case=SESSION_BOUNDARY;
    for(unsigned i=0;i<48;++i)original_payload[i]=static_cast<unsigned char>(i+1);
    A::Options options=A::Options();options.clock=clock_ns;options.request_reader=request_reader;
    options.association_reader=read_inline_association;options.provenance=provenance;options.sink=observe;
    assert(!options.allow_assist&&A::configure(original_send,options)&&A::set_mode(A::OBSERVE));
    assert(!A::set_mode(A::ASSIST)); // Association alone never enables mutation.
    if(boundary_case==BUS_BOUNDARY)bus_fixture::prepare();
    if(boundary_case==SESSION_BOUNDARY) {
        const A::SessionBindings bindings={session_create,session_destroy,session_status};
        assert(A::prepare_session_hooks(bindings));
        A::SessionCallbacks callbacks=A::SessionCallbacks();
        callbacks.entry[1]=reinterpret_cast<uintptr_t>(session_status);
        assert(!mx5_session_create("fixture",0,&callbacks,&session_storage));
    }
    config.mode=1;hook_installed=true;
    char root[]="/tmp/mx5-runtime-association-XXXXXX";assert(mkdtemp(root));
    const std::string logs=std::string(root)+"/logs",trace=logs+"/trace.0.jsonl";
    assert(!mkdir(logs.c_str(),0700));
    L::Publisher publisher;assert(publisher.prepare(41,root));
    assert(!callback(publisher));
    const A::Observation before_adoption=last_position;
    A::Observation matched=A::Observation();
    A::Observation status_rows[5];
    A::Observation presence_rows[3]; // Filled by actual callbacks before adopted-case journal checks.
    Running running=Running();running.root=root;
    R::LdsRequestSource source;
    if(boundary_case!=NO_BOUNDARY)running.source=&source;
    bool boundary_rejected=true;
    ::snprintf(running.motion,sizeof running.motion,"mx5-assoc-motion-%ld",long(getpid()));
    ::snprintf(running.sideband,sizeof running.sideband,"mx5-assoc-side-%ld",long(getpid()));
    ::snprintf(running.association,sizeof running.association,"mx5-assoc-map-%ld",long(getpid()));
    if(scenario=="pre_stopped")assert(!mkdir((logs+"/capture.stop").c_str(),0700));
    pthread_t thread;assert(R::create_thread(&thread,run,&running,R::WORKER_STACK_BYTES,false));
    if(scenario!="pre_stopped") {
        wait_boot(trace);offer(publisher,running.association);wait_match(publisher);
        matched=last_position;
        assert(before_adoption.lds_association.result==L::UNAVAILABLE);
        assert(!before_adoption.provenance.exact_request); // Never retroactively mutated.
        const L::Owned retained=last_position.lds_association;
        if(scenario=="adopted") {
            const C::Presence values[]={C::EMPTY,C::PRESENT,C::UNKNOWN};
            for(unsigned i=0;i<3;++i) {
                assert(callback(publisher,true,values[i]));presence_rows[i]=last_position;
                assert(last_position.lds_association.heading_presence==values[i]);
                assert(last_send.lds_association.heading_presence==values[i]);
                if(i<2)assert(last_position.position.heading_deg==0);
            }
            for(unsigned i=0;i<5;++i) {
                assert(callback(publisher,true,C::PRESENT,C::RmcStatus(i)));status_rows[i]=last_position;
                assert(last_position.lds_association.heading_rmc_status==C::RmcStatus(i));
                assert(last_send.lds_association.heading_rmc_status==C::RmcStatus(i));
                assert(last_position.position.heading_deg==0);
            }
        } else if(boundary_case!=NO_BOUNDARY) {
            idle_gate.store(1,std::memory_order_release);wait_gate(2);
            // All prior rows drained before this idle boundary. This new real
            // callback queues exactly one POSITION/SEND pair for the next turn.
            L::Owned before=L::Owned();assert(read_captured(matched,&before));
            const uint64_t retirements=source.status().retirements;
            const uint64_t positions=source.status().positions;
            assert(callback(publisher));
            boundary_step.store(1,std::memory_order_release);
            idle_gate.store(3,std::memory_order_release);wait_gate(4);
            assert(boundary_step.load(std::memory_order_acquire)==3&&lifecycle_calls==1);
            // The pause is after the complete turn: the inner check really
            // consumed the boundary and reset the historical resolver.
            assert(source.status().retirements==retirements+1);
            assert(source.status().positions==positions+1);
            L::Owned after=L::Owned();boundary_rejected=!read_captured(matched,&after);
            std::fprintf(stderr,"worker drain %s: outer_actual_revision=%llu inner_actual_revision=%llu source_resets=1 positions=1 captured_before=%u captured_after=%u\n",
                scenario.c_str(),static_cast<unsigned long long>(outer_revision),
                static_cast<unsigned long long>(inner_revision),unsigned(before.result),unsigned(after.result));
            if(boundary_rejected)assert(after.result==L::UNAVAILABLE);
            idle_gate.store(5,std::memory_order_release);
        } else if(scenario=="freeze") {
            freeze_capture();assert(!callback(publisher));
        } else if(scenario=="audit") {
            disable_mutation();assert(!callback(publisher));
        } else if(scenario=="journal_failure") {
            // The matched callback queued its POSITION/SEND rows for the worker. Wait until they
            // are in the file before breaking the journal: renaming the directory first races the
            // worker (a slow machine, such as QEMU, loses it) and the rows never exist.
            const uint64_t rows_deadline=clock_ns(0)+5000*MS;
            while(clock_ns(0)<rows_deadline) {
                const std::string seen=read_file(trace);
                if(seen.find("\"kind\":\"position\"")!=std::string::npos&&
                   seen.find("\"kind\":\"send\"")!=std::string::npos)break;
                usleep(5000);
            }
            {
                const std::string seen=read_file(trace);
                assert(seen.find("\"kind\":\"position\"")!=std::string::npos);
                assert(seen.find("\"kind\":\"send\"")!=std::string::npos);
            }
            assert(!rename(logs.c_str(),(logs+"-retained").c_str()));
            callback(publisher); // Actual journal statvfs fails on the next queued raw row.
            const uint64_t deadline=clock_ns(0)+2000*MS;
            while(clock_ns(0)<deadline&&!__sync_fetch_and_add(&audit_fault,0))usleep(5000);
            assert(__sync_fetch_and_add(&audit_fault,0));
            assert(!callback(publisher));
            assert(!rename((logs+"-retained").c_str(),logs.c_str()));
        } else if(scenario=="fork") {
            L::Owned current=L::Owned();
            assert(read_captured(matched,&current)); // Same published key before fork.
            const pid_t child=fork();assert(child>=0);
            if(!child) {
                // libc's actual child handler must disable the inherited view.
                // No manual Registry/Publisher disable or worker call here.
                const bool inherited=read_captured(matched,&current);
                const bool fresh=callback(publisher,false);
                _exit(inherited||fresh?9:0);
            }
            int status=0;assert(waitpid(child,&status,0)==child);
            assert(WIFEXITED(status)&&WEXITSTATUS(status)==0);
            assert(read_captured(matched,&current));
            assert(callback(publisher)); // Child disable did not touch parent's map.
        }
        assert(retained.result==L::MATCHED_LOCKED_FOR_SEND&&retained.source_instance==41);
        assert(!mkdir((logs+"/capture.stop").c_str(),0700));
    }
    assert(!pthread_join(thread,0));
    assert(!callback(publisher)); // Process-lifetime view is retired when worker exits.
    if(scenario=="pre_stopped")assert(read_file(trace).empty());
    else {
        const std::string journal=read_file(trace);
        assert(journal.find("\"kind\":\"position\"")!=std::string::npos);
        assert(journal.find("\"kind\":\"send\"")!=std::string::npos);
        if(scenario=="journal")check_journal_association(journal,before_adoption,matched);
        if(scenario=="adopted") {
            for(unsigned i=0;i<3;++i)check_journal_association(journal,before_adoption,presence_rows[i]);
            for(unsigned i=0;i<5;++i)check_journal_association(journal,before_adoption,status_rows[i]);
        }
        if(scenario!="journal_failure") {
            assert(journal.find("\"kind\":\"capture_end\"")!=std::string::npos);
            assert(!read_file(logs+"/capture.done").empty());
        }
    }
    assert(request_reads==callbacks&&sends==callbacks);
    remove_logs(logs);assert(!rmdir(root));
    assert(boundary_rejected); // Real inner lifecycle consumption must also retire the adopted map.
    std::printf("PASS runtime LDS association %s: %u one-shot callbacks; actual worker, physical qualification unknown\n",argv[1],callbacks);
}
