// Authored fixtures exercise the real wrappers. Host builds use -no-pie so
// these static objects fit the pinned 32-bit pending-node layout; ARM is exact.
#include "adapter/request_hooks.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdexcept>
#include <string.h>
namespace A=mx5::adapter;
namespace R=mx5::runtime::request_trace;
#if defined(MX5_REQUEST_WIRE_DSO_TEST)
#include "request_wire_dso_access.h"
#else
#include "../../src/adapter/request_hooks.cpp"
namespace mx5 { namespace adapter {
S::Snapshot read_issue_session() { return S::Snapshot(); }
runtime::bus_trace::Snapshot read_bus_connection(const void*) { return runtime::bus_trace::Snapshot(); }
runtime::bus_trace::Snapshot read_bus_endpoint(const void*,R::Endpoint* endpoint,uintptr_t* raw) {
    if(endpoint)*endpoint=R::Endpoint();
    if(raw)*raw=0;
    return runtime::bus_trace::Snapshot();
}
EndpointMatch bus_endpoint_matches(const void*,const runtime::bus_trace::Snapshot&,uintptr_t) { return ENDPOINT_UNAVAILABLE; }
void observe_position_bus(const void*) {}
} }
#if defined(__arm__)
extern "C" void mx5_arm_invoke(uint32_t*,void*) { assert(false); }
#endif
#endif
struct Method;
struct Message {
    Method* method;uint32_t serial,reply_serial;int type;
    char sender[20],error[40];
};
struct Method {
    Message request,reply;A::RequestNotify callback;void* context;
    int pending;alignas(8) unsigned char worker[88];
    unsigned calls,frees;R::Trace trace;
};
static Method methods[4];static int connection;
static Message unrelated;
static uint32_t next_serial=80;
static unsigned builds,raw_sends,handlers,steals;
static bool inline_reply,throw_send,send_failure,no_pending,auxiliary_send;
static bool repeated_send,repeated_failure,repeated_no_pending,repeated_build;
static bool nested_notify,throw_notify,throw_steal;
static unsigned nested_submit;
static void reply(Method& m,bool mismatch=false);
static uint64_t tick(void*) { errno=EIO;return 1234; }
static void* get_reply(void* p) { return &static_cast<Method*>(p)->reply; }
static int get_type(void*) { return 1; }
static const char* get_sender(void*) { return ":1.9"; }
static const char* get_error(void*) { return 0; }
static int get_public_serial(void*,uint32_t* out) { *out=0;return -1; }
static const char* route(void*) { return "authored"; }
static void* raw_build(void* p) { ++builds;errno=E2BIG;return &static_cast<Method*>(p)->request; }
static int32_t raw_send(void* c,void* p,void** pending,int timeout) {
    assert(c==&connection&&timeout==72);++raw_sends;errno=EBUSY;
    if(throw_send)throw 17;
    Message& msg=*static_cast<Message*>(p);msg.serial=next_serial++;
    if(pending)*pending=no_pending?0:&msg.method->pending;
    return send_failure?0:1;
}
static void* raw_steal(void* p) {
    ++steals;errno=EAGAIN;
    if(throw_steal)throw 23;
    for(unsigned i=0;i<4;++i)if(p==&methods[i].pending)return &methods[i].reply;
    return 0;
}
static uint32_t raw_serial(void* p) { errno=EIO;return static_cast<Message*>(p)->serial; }
static uint32_t raw_reply_serial(void* p) { errno=EIO;return static_cast<Message*>(p)->reply_serial; }
static int32_t raw_type(void* p) { errno=EIO;return static_cast<Message*>(p)->type; }
static const char* raw_sender(void* p) { errno=EIO;return static_cast<Message*>(p)->sender; }
static const char* raw_error(void* p) { errno=EIO;return static_cast<Message*>(p)->error[0]?static_cast<Message*>(p)->error:0; }
static int32_t method_free(void* p) { ++static_cast<Method*>(p)->frees;errno=ENOTTY;return 29; }
#if defined(MX5_REQUEST_WIRE_DSO_TEST)
static __thread R::Trace* active_trace;
static void work_trampoline(void* worker) {
    assert(active_trace);
    assert(A::read_request_trace(static_cast<char*>(worker)+8,active_trace,0)==R::OK);
}
static void take_worker(Method& m) {
    struct ActiveTrace {
        R::Trace* previous;
        explicit ActiveTrace(R::Trace* trace) : previous(active_trace) { active_trace=trace; }
        ~ActiveTrace() { active_trace=previous; }
    } scope(&m.trace);
    uint32_t shared[2]={uint32_t(uintptr_t(m.worker)),0x11223344};
    mx5_request_post_enter(shared);
    assert(shared[0]==uintptr_t(m.worker)&&shared[1]==0x11223344);
    uint32_t registers[4]={uint32_t(uintptr_t(m.worker)),0,0,0};
    mx5_request_work_call(registers);
}
#else
static void take_worker(Method& m) {
    R::Token work;assert(observer->worker_post(m.worker,m.worker+8,&work)==R::OK);
    R::WorkerScope worker(*observer,m.worker);
    assert(A::read_request_trace(m.worker+8,&m.trace,0)==R::OK);
}
#endif
static void original_notify(void* c,void* p,void* context) {
    assert(c==&connection&&p==context);Method& m=*static_cast<Method*>(p);++m.calls;
    if(throw_notify){errno=ERANGE;throw 31;}
    if(nested_notify&&p==&methods[0])reply(methods[1]);
    take_worker(m);errno=ERANGE;
}
static void unobserved_notify(void* c,void* p,void* context) {
    assert(c==&connection&&p==context);++static_cast<Method*>(p)->calls;errno=ERANGE;
}
static void raw_pending(void* p,void* node) {
    ++handlers;uint32_t address;memcpy(&address,static_cast<char*>(node)+4,4);
    Method& m=*reinterpret_cast<Method*>(uintptr_t(address));assert(p==&m.pending);
    void* result=mx5_request_steal(p);assert(result==&m.reply&&errno==EAGAIN);
    m.callback(&connection,&m,m.context);assert(errno==ERANGE);
    assert(mx5_request_free(&m)==29&&errno==ENOTTY);
}
static void reply(Method& m,bool mismatch) {
    assert(uintptr_t(&m)<=UINT32_MAX&&uintptr_t(&m.pending)<=UINT32_MAX);
    uint32_t node[7]={};node[1]=uint32_t(uintptr_t(&m));node[6]=uint32_t(uintptr_t(&m.pending));
    if(mismatch)node[6]=uint32_t(uintptr_t(&connection));
    m.reply.serial=next_serial++;m.reply.reply_serial=m.request.serial;m.reply.type=2;
    strcpy(m.reply.sender,":1.9");
    mx5_request_pending(&m.pending,node);assert(errno==ENOTTY);
}
static int32_t original_submit(void* c,void* p,A::RequestNotify cb,void* context,int timeout) {
    Method& m=*static_cast<Method*>(p);m.callback=cb;m.context=context;
    void* message=mx5_request_message(p);assert(message==&m.request&&errno==E2BIG);
    if(repeated_build)assert(mx5_request_message(p)==message&&errno==E2BIG);
    if(nested_submit&&p==&methods[0]) {
        const A::RequestNotify inner=nested_submit==2?unobserved_notify:original_notify;
        if(nested_submit==3) {
            throw_send=true;
            try { mx5_request_submit(c,&methods[1],inner,&methods[1],timeout);assert(false); }
            catch(int error) { assert(error==17&&errno==EBUSY); }
            throw_send=false;assert(mx5_request_free_only(&methods[1])==29&&errno==ENOTTY);
        } else {
            assert(mx5_request_submit(c,&methods[1],inner,&methods[1],timeout)==37&&errno==ENOSPC);
            reply(methods[1]);
        }
    }
    void* pending=0;
    if(auxiliary_send)assert(mx5_request_wire_send(c,&unrelated,&pending,timeout)==1);
    int32_t result=mx5_request_wire_send(c,message,&pending,timeout);assert(errno==EBUSY);
    assert(result==(send_failure?0:1));
    if(repeated_send) {
        send_failure=repeated_failure;no_pending=repeated_no_pending;
        assert(mx5_request_wire_send(c,message,&pending,timeout)==(send_failure?0:1));
        assert(errno==EBUSY);send_failure=no_pending=false;
    }
    if(inline_reply)reply(m);
    errno=ENOSPC;return 37;
}
static void trampoline() {}
static void setup() {
    for(unsigned i=0;i<4;++i) {
        methods[i].request.method=&methods[i];
        uint32_t vptr=1,position=uint32_t(uintptr_t(methods[i].worker+8));
        memcpy(methods[i].worker,&vptr,4);memcpy(methods[i].worker+80,&position,4);
    }
    unrelated.method=&methods[3];
    A::RequestBindings b=A::RequestBindings();b.reply={get_reply,get_type,get_sender,get_error,get_public_serial};
    b.method={route,route,route,route};b.submit=original_submit;b.notify=original_notify;
    b.free_method=b.free_method_only=method_free;b.position_vptr=1;
    b.post_trampoline=b.work_trampoline=b.destroy_trampoline=reinterpret_cast<void*>(&trampoline);
#if defined(MX5_REQUEST_WIRE_DSO_TEST)
    b.work_trampoline=reinterpret_cast<void*>(&work_trampoline);
#endif
    b.wire={raw_build,raw_send,raw_pending,raw_steal,raw_serial,raw_reply_serial,raw_type,raw_sender,raw_error};
    assert(A::prepare_request_hooks(b,tick,0));
}
static void issue(Method& m) {
    assert(mx5_request_submit(&connection,&m,original_notify,&m,72)==37&&errno==ENOSPC);
}
static void check(const Method& m) {
    assert(m.trace.issue.wire.known&&m.trace.issue.wire.serial==m.request.serial);
    assert(!m.trace.issue.wire.conflict&&m.trace.issue.wire.observed_ns==1234);
    assert(m.trace.reply.wire.known&&m.trace.reply.wire.reply_serial==m.request.serial);
    assert(m.trace.reply.wire.serial==m.reply.serial&&m.trace.reply.wire.type==2);
    assert(!strcmp(m.trace.reply.wire.sender.bytes,":1.9")&&m.trace.reply.wire.sender.complete);
    assert(!m.trace.reply.wire_serial_known&&!m.trace.reply.wire_serial);
    assert(!m.trace.issue.known); // Headers never imply qualified provenance.
}
static void empty() {
    const A::RequestHookHealth health=A::request_hook_health();
    assert(health.prepared&&!health.abi_fault&&health.result==R::OK);
    assert(!health.ledger.requests&&!health.ledger.workers&&!health.ledger.loss_reasons);
}
int main(int argc,char** argv) {
    assert(argc==1||(argc==2&&!strcmp(argv[1],"contract")));
#if defined(MX5_REQUEST_WIRE_DSO_TEST)
    initialize_request_wire_test_dso();
#endif
    setup();auxiliary_send=true;
    issue(methods[0]);issue(methods[1]);reply(methods[1]);reply(methods[0]);
    check(methods[0]);check(methods[1]);
    assert(methods[0].trace.request.id!=methods[1].trace.request.id);
    const uint64_t previous=methods[0].trace.request.id;
    inline_reply=true;issue(methods[0]);check(methods[0]);
    assert(methods[0].trace.request.id!=previous);inline_reply=false;auxiliary_send=false;
    throw_send=true;
    try{issue(methods[2]);assert(false);}catch(int error){assert(error==17&&errno==EBUSY);}
    assert(mx5_request_free(&methods[2])==29);throw_send=false;
    issue(methods[2]);reply(methods[2]);check(methods[2]);
    for(unsigned i=0;i<2;++i) {
        send_failure=i==0;no_pending=i==1;issue(methods[3]);
        // Authored callback after a failed/null-pending send must not inherit
        // a previous issue header. OEM forwarding is still attempted once.
        reply(methods[3]);assert(!methods[3].trace.issue.wire.known);
        assert(methods[3].trace.reply.wire.known);send_failure=no_pending=false;
    }
    empty();
    assert(builds==7&&raw_sends==10&&handlers==6&&steals==6);
    repeated_send=repeated_failure=true;
    issue(methods[0]);reply(methods[0]);
    assert(methods[0].trace.issue.wire.conflict);
    assert(methods[0].trace.reply.wire.known);
    repeated_send=repeated_failure=false;
    empty();
    for(unsigned mode=0;mode<3;++mode) {
        repeated_send=mode!=2;repeated_no_pending=mode==1;repeated_build=mode==2;
        issue(methods[0]);reply(methods[0]);
        assert(methods[0].trace.issue.wire.conflict&&methods[0].trace.reply.wire.known);
        repeated_send=repeated_no_pending=repeated_build=false;empty();
    }
    for(nested_submit=1;nested_submit<=3;++nested_submit) {
        methods[1].trace=R::Trace();
        issue(methods[0]);reply(methods[0]);check(methods[0]);
        if(nested_submit==1) {
            check(methods[1]);assert(methods[0].trace.request.id!=methods[1].trace.request.id);
        } else assert(!methods[1].trace.request.id);
        empty();
    }
    nested_submit=0;
    issue(methods[0]);issue(methods[1]);nested_notify=true;reply(methods[0]);nested_notify=false;
    check(methods[0]);check(methods[1]);empty();
    issue(methods[0]);reply(methods[0],true);
    assert(methods[0].trace.issue.wire.known&&!methods[0].trace.reply.wire.known);empty();
    for(unsigned i=0;i<2;++i) {
        issue(methods[0]);throw_notify=i==0;throw_steal=i==1;
        try { reply(methods[0]);assert(false); }
        catch(int error) { assert(error==(throw_notify?31:23)&&errno==(throw_notify?ERANGE:EAGAIN)); }
        throw_notify=throw_steal=false;
        assert(mx5_request_free(&methods[0])==29&&errno==ENOTTY);empty();
        issue(methods[0]);reply(methods[0]);check(methods[0]);empty();
    }
    empty();
    puts("PASS raw request wrappers contract: reversed identical values, inline callback, reuse, auxiliary send, failed/null/duplicate send, duplicate build, nested/unobserved submit, nested reply, pending mismatch, submit/notify/steal unwind and errno");
}
