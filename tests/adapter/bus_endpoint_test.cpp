// Authored OEM functions, real bus/register/request wrappers and owned journal
// formatter. No real DBus transport or physical qualification is claimed here.
#include "adapter/bus_hooks.h"
#include "adapter/request_hooks.h"
#include "runtime/request_log.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
namespace B=mx5::runtime::bus_trace;
#if defined(MX5_ENDPOINT_DSO_TEST)
namespace A=mx5::adapter;
namespace R=mx5::runtime::request_trace;
#include "bus_endpoint_dso_access.h"
#else
#include "../../src/adapter/request_hooks.cpp"
namespace mx5 { namespace adapter {
S::Snapshot read_issue_session() { return S::Snapshot(); }
} }
#if defined(__arm__)
extern "C" void mx5_arm_invoke(uint32_t*,void*) { assert(false); }
extern "C" { void* mx5_position_trampoline; }
#endif
#endif
extern "C" int32_t endpoint_test_register_call(void*,void*);
extern "C" const char endpoint_test_register_return;
extern "C" { uintptr_t endpoint_test_register_target; }
struct Connection { unsigned char bytes[0x300];A::BusClosed closed;void* user; };
static Connection connections[2];
static unsigned char raw_storage[2];
static unsigned creates,registrations,guid_reads,unique_reads,guid_frees,sends,notifies;
static char guid[100]="0123456789abcdef0123456789abcdef",unique_name[100]=":1.7",allocated_guid[100];
static const char* scenario;
static unsigned raw_index;
static bool changed,reentering;
static std::atomic<bool> waiting(false),proceed(false),readers_done(false);
static std::atomic<unsigned> complete_reads(0);
static bool is(const char* name) { return !strcmp(scenario,name); }
static void* raw() { return &raw_storage[raw_index]; }
static int32_t closed_next(void*,void*) { errno=ENOTTY;return 17; }
static void* create_next(A::BusClosed callback,void* user) {
    Connection& c=connections[is("address_reuse")?0:creates];++creates;
    c.closed=callback;c.user=user;errno=E2BIG;return &c;
}
static int32_t register_next(void* value,void* error) {
    assert(value==raw() || is("wrong_raw"));assert(error==&registrations);++registrations;
    errno=ERANGE;if(is("register_throw"))throw 23;
    return is("failed_register")?0:1;
}
static char* get_guid(void* value) {
    assert(value==raw());++guid_reads;errno=ENOMEM;
    if(is("missing_guid"))return 0;
    strcpy(allocated_guid,guid);return allocated_guid;
}
static const char* get_unique(void* value) {
    assert(value==raw());++unique_reads;errno=EIO;
    if(is("getter_throw"))throw 29;
    if(is("getter_cancel")) {
        waiting.store(true);
        for(;;){pthread_testcancel();sched_yield();}
    }
    if(is("getter_nested")&&!reentering) {
        reentering=true;assert(endpoint_test_register_call(raw(),&registrations)==1&&errno==ERANGE);reentering=false;
    }
    return is("missing_unique")?0:unique_name;
}
static void free_guid(void* value) {
    assert(value==allocated_guid);++guid_frees;memset(allocated_guid,'x',sizeof allocated_guid);errno=ENOSPC;
}
static int32_t connect_next(void* value,const char* name,int32_t type,uintptr_t callback) {
    assert(!strcmp(name,"authored")&&type==19&&callback==0x1234);
    Connection& c=*static_cast<Connection*>(value);
    assert(uintptr_t(raw())<=UINT32_MAX);
    const uint32_t word=uint32_t(uintptr_t(raw()));memcpy(c.bytes+0x268,&word,4);
    if(is("nested")&&!reentering) {
        reentering=true;
        assert(mx5_bus_connect(&connections[1],name,type,callback)==31&&errno==EBUSY);
        reentering=false;
    }
    const int result=is("no_api")?1:is("wrong_caller")?mx5_bus_register(raw(),&registrations):
        endpoint_test_register_call(is("wrong_raw")?&raw_storage[1]:raw(),&registrations);
    assert(result==(is("failed_register")?0:1));if(!is("no_api"))assert(errno==ERANGE);
    if(is("duplicate"))assert(endpoint_test_register_call(raw(),&registrations)==1&&errno==ERANGE);
    if(is("early_close"))assert(c.closed(value,c.user)==17&&errno==ENOTTY);
    if(is("connect_throw")){errno=EBUSY;throw 31;}
    if(is("transition_send")&&changed) {
        waiting.store(true);while(!proceed.load())sched_yield();
    }
    errno=EBUSY;return is("failed_connect")?0:31;
}
static void end_next(void*) { errno=ENOTTY; /* Original may leave +0x268 non-NULL. */ }
static int32_t signal_next(void*,void*) { return 0; }
static int32_t is_signal(void*,const char*,const char*) { return 0; }
static int32_t connect() { return mx5_bus_connect(&connections[0],"authored",19,0x1234); }
static void* connect_thread(void*) { connect();return 0; }
static void* reader_thread(void*) {
    do {
        R::Endpoint e;uintptr_t key=0;errno=ERANGE;
        const B::Snapshot b=A::read_bus_endpoint(&connections[0],&e,&key);assert(errno==ERANGE);
        if(b.result==B::CONNECTED) {
            assert(key==uintptr_t(raw_storage)&&e.server_guid.known&&e.unique_name.known);
            assert(!strcmp(e.server_guid.bytes,e.unique_name.bytes));++complete_reads;
        } else assert(!e.server_guid.known&&!e.unique_name.known&&!key);
    } while(!readers_done.load());
    return 0;
}
struct Method { uint32_t serial;unsigned char worker[88];R::Trace trace; };
static Method method;
#if defined(MX5_ENDPOINT_DSO_TEST)
static void work_trampoline(void* worker) {
    assert(worker==method.worker);
    assert(A::read_request_trace(method.worker+8,&method.trace,0)==R::OK);
}
#endif
static void* get_reply(void* p) { return p; }
static int get_type(void*) { return 1; }
static const char* get_sender(void*) { return ":1.9"; }
static const char* get_error(void*) { return 0; }
static int get_serial(void*,uint32_t*) { return -1; }
static const char* route(void*) { return "authored"; }
static void* build_next(void* p) { return p; }
static int32_t send_next(void* connection,void* p,void** pending,int timeout) {
    assert(connection==(is("raw_mismatch")?&raw_storage[1]:raw()));assert(p==&method&&timeout==11);
    ++sends;method.serial=7;*pending=&method;
    if(is("reconnect_in_send")&&!changed) {
        changed=true;mx5_bus_disconnect(&connections[0]);strcpy(unique_name,":1.8");assert(connect()==31);
    }
    errno=EAGAIN;return 1;
}
static uint32_t raw_serial(void*) { errno=EIO;return method.serial; }
static void pending_next(void*,void*) { assert(false); }
static void* steal_next(void*) { assert(false);return 0; }
static int32_t raw_type(void*) { return 2; }
static uint64_t now(void*) { errno=EIO;return 100; }
static void notify_next(void* c,void* p,void* context) {
    assert(c==&connections[0]&&p==&method&&context==p);++notifies;
#if defined(MX5_ENDPOINT_DSO_TEST)
    uint32_t shared[2]={uint32_t(uintptr_t(method.worker)),0x11223344};mx5_request_post_enter(shared);
    assert(shared[0]==uintptr_t(method.worker)&&shared[1]==0x11223344);
    uint32_t registers[4]={uint32_t(uintptr_t(method.worker)),0,0,0};mx5_request_work_call(registers);
    errno=EDOM;
#else
    R::Token token;assert(observer->worker_post(method.worker,method.worker+8,&token)==R::OK);
    R::WorkerScope worker(*observer,method.worker);
    assert(A::read_request_trace(method.worker+8,&method.trace,0)==R::OK);errno=EDOM;
#endif
}
static int32_t free_method(void*) { errno=ENOTTY;return 13; }
static int32_t submit_next(void* c,void* p,A::RequestNotify callback,void* context,int timeout) {
    void* message=mx5_request_message(p);assert(message==p);
    if(is("reconnect_before_send")&&!changed) {
        changed=true;mx5_bus_disconnect(c);raw_index=1;strcpy(unique_name,":1.8");
        assert(connect()==31&&errno==EBUSY);
    }
    pthread_t thread;
    if(is("transition_send")) {
        changed=true;assert(!pthread_create(&thread,0,connect_thread,0));while(!waiting.load())sched_yield();
    }
    void* pending=0;
    assert(mx5_request_wire_send(is("raw_mismatch")?&raw_storage[1]:raw(),message,&pending,timeout)==1&&errno==EAGAIN);
    if(is("transition_send")){proceed.store(true);assert(!pthread_join(thread,0));}
    callback(c,p,context);assert(errno==EDOM);errno=ENOSPC;return 37;
}
static void trampoline() {}
static void setup() {
    endpoint_test_register_target=reinterpret_cast<uintptr_t>(mx5_bus_register);
    A::BusBindings b={create_next,connect_next,end_next,end_next,signal_next,is_signal,
        {register_next,get_guid,get_unique,free_guid,uintptr_t(&endpoint_test_register_return)}};
    A::BusBindings incomplete=b;incomplete.endpoint.free_guid=0;assert(!A::prepare_bus_hooks(incomplete));
    if(is("no_api"))b.endpoint=A::BusEndpointApi();
    assert(A::prepare_bus_hooks(b));
    assert(mx5_bus_create(closed_next,&connections[0])==&connections[0]&&errno==E2BIG);
    if(is("nested"))assert(mx5_bus_create(closed_next,&connections[1])==&connections[1]);
    A::RequestBindings q=A::RequestBindings();
    q.reply={get_reply,get_type,get_sender,get_error,get_serial};q.method={route,route,route,route};
    q.submit=submit_next;q.notify=notify_next;q.free_method=q.free_method_only=free_method;q.position_vptr=1;
    q.post_trampoline=q.work_trampoline=q.destroy_trampoline=reinterpret_cast<void*>(&trampoline);
#if defined(MX5_ENDPOINT_DSO_TEST)
    q.work_trampoline=reinterpret_cast<void*>(&work_trampoline);
    const uint32_t vptr=1,position=uint32_t(uintptr_t(method.worker+8));
    memcpy(method.worker,&vptr,4);memcpy(method.worker+80,&position,4);
#endif
    q.wire={build_next,send_next,pending_next,steal_next,raw_serial,raw_serial,raw_type,get_sender,get_error};
    assert(A::prepare_request_hooks(q,now,0));
}
static void issue() {
    assert(mx5_request_submit(&connections[0],&method,notify_next,&method,11)==37&&errno==ENOSPC);
    assert(mx5_request_free(&method)==13&&errno==ENOTTY);
    assert(method.trace.request.id&&method.trace.issue.wire.known&&method.trace.issue.wire.serial==7);
    const A::RequestHookHealth health=A::request_hook_health();
    assert(health.prepared&&!health.abi_fault&&!health.ledger.requests&&!health.ledger.workers);
}
int main(int argc,char** argv) {
    assert(argc==2);scenario=argv[1];
#if defined(MX5_ENDPOINT_DSO_TEST)
    initialize_request_wire_test_dso();
#endif
    if(is("long")){memset(guid,'g',99);guid[99]=0;memset(unique_name,'u',99);unique_name[99]=0;}
    if(is("empty")){guid[0]=unique_name[0]=0;}
    if(is("readers"))strcpy(guid,unique_name);
    setup();
    if(is("getter_cancel")) {
        pthread_t thread;assert(!pthread_create(&thread,0,connect_thread,0));while(!waiting.load())sched_yield();
        assert(!pthread_cancel(thread));void* result=0;assert(!pthread_join(thread,&result)&&result==PTHREAD_CANCELED);
        assert(guid_frees==1&&A::read_bus_connection(&connections[0]).result==B::FAULT);
        printf("PASS endpoint wrappers %s: cancellation contract\n",scenario);return 0;
    }
    if(is("register_throw")||is("getter_throw")||is("connect_throw")) {
        try { connect();assert(false); } catch(int n) { assert(n==(is("register_throw")?23:is("getter_throw")?29:31)); }
        assert(A::read_bus_connection(&connections[0]).result==B::FAULT);
        if(is("getter_throw"))assert(guid_frees==1);
        printf("PASS endpoint wrappers %s: unwind contract\n",scenario);return 0;
    }
    assert(connect()==(is("failed_connect")?0:31)&&errno==EBUSY);
    if(is("readers")) {
        pthread_t reader;assert(!pthread_create(&reader,0,reader_thread,0));
        for(unsigned n=0;n<300;++n) {
            mx5_bus_disconnect(&connections[0]);::snprintf(guid,sizeof guid,"identity-%u",n);strcpy(unique_name,guid);
            assert(connect()==31);sched_yield();
        }
        while(!complete_reads.load())sched_yield();
        readers_done.store(true);assert(!pthread_join(reader,0));
        assert(!A::bus_hook_health().faults);printf("PASS endpoint wrappers %s: concurrent snapshot contract\n",scenario);return 0;
    }
    if(is("normal")) { strcpy(guid,"overwritten");strcpy(unique_name,"overwritten"); }
    issue();
    const R::Endpoint& endpoint=method.trace.issue.endpoint;
    const bool unavailable=is("failed_register")||is("failed_connect")||is("early_close")||
        is("wrong_raw")||is("wrong_caller")||is("duplicate")||is("nested")||is("getter_nested")||is("no_api");
    assert(endpoint.server_guid.known==(!unavailable&&!is("missing_guid")));
    assert(endpoint.unique_name.known==(!unavailable&&!is("missing_unique")));
    if(!unavailable&&!is("missing_guid")) {
        assert(endpoint.server_guid.complete==!is("long"));
        assert(!strcmp(endpoint.server_guid.bytes,is("empty")?"":is("long")?"ggggggggggggggggggggggggggggggggggggggggggggggggggggggggggggggg":"0123456789abcdef0123456789abcdef"));
    }
    if(!unavailable&&!is("missing_unique")&&!is("long"))assert(!strcmp(endpoint.unique_name.bytes,is("empty")?"":":1.7"));
    assert(method.trace.issue.wire.conflict==(is("raw_mismatch")||is("reconnect_before_send")));
    assert(method.trace.issue.wire.endpoint_matched==(!unavailable&&!is("raw_mismatch")&&!is("reconnect_before_send")&&
        !is("transition_send")&&!is("reconnect_in_send")));
    char json[mx5::runtime::REQUEST_JSON_CAPACITY];
    assert(mx5::runtime::format_request_trace(json,sizeof json,R::OK,method.trace));
    assert(strstr(json,"\"endpoint\":{\"server_guid\":{")&&strstr(json,"\"unique_name\":{"));
    if(is("normal")) {
        assert(strstr(json,"0123456789abcdef0123456789abcdef"));
        assert(strstr(json,":1.7"));
        assert(guid_reads==1&&unique_reads==1&&guid_frees==1);
    }
    assert(notifies==1&&sends==1);
    if(is("reconnect")||is("address_reuse")) {
        const R::Trace old=method.trace;
        mx5_bus_disconnect(&connections[0]);assert(errno==ENOTTY);
        if(is("address_reuse")) {
            mx5_bus_free(&connections[0]);assert(mx5_bus_create(closed_next,&connections[0])==&connections[0]);
        }
        strcpy(unique_name,":1.8");assert(connect()==31);issue();
        assert(!strcmp(method.trace.issue.endpoint.unique_name.bytes,":1.8"));
        assert(!strcmp(old.issue.endpoint.unique_name.bytes,":1.7"));
        assert(old.issue.connection.lifetime!=method.trace.issue.connection.lifetime);
        assert((old.issue.connection.object!=method.trace.issue.connection.object)==is("address_reuse"));
        assert(A::bus_endpoint_matches(&connections[0],old.issue.connection,uintptr_t(raw()))==A::ENDPOINT_MISMATCH);
        assert(method.trace.issue.wire.endpoint_matched&&!method.trace.issue.wire.conflict);
    }
    printf("PASS endpoint wrappers %s: request and journal contract\n",scenario);return 0;
}
