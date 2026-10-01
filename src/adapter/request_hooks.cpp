#include "request_hooks.h"
#include "session_hooks.h"
#include "bus_hooks.h"
#include <atomic>
#include <errno.h>
#include <new>
#include <string.h>

namespace {
namespace A = mx5::adapter;
namespace R = mx5::runtime::request_trace;
R::Observer* observer;
A::RequestBindings original = A::RequestBindings();
std::atomic<unsigned> prepared(0), abi_fault(0);
bool attempted;
R::ObservationClock wire_clock;
void* wire_clock_user;
void ready() {
    // Pointers are published only after preparation. The acquire also pairs
    // with initialization for JCIDBUS threads that predate the BLM load.
    if(!prepared.load(std::memory_order_acquire))__builtin_trap();
}
struct PreserveErrno {
    const int value;
    PreserveErrno() : value(errno) {}
    ~PreserveErrno() { errno=value; }
};
struct SubmitFrame;
struct PendingFrame;
__thread SubmitFrame* submit_frame __attribute__((tls_model("initial-exec")));
__thread PendingFrame* pending_frame __attribute__((tls_model("initial-exec")));
struct SubmitFrame {
    SubmitFrame* previous;
    void* method;
    R::Token token;
    void* message;
    void* connection;
    mx5::runtime::bus_trace::Snapshot bus;
    uintptr_t raw_key;
    unsigned builds,sends;
    explicit SubmitFrame(void* value) : previous(submit_frame),method(value),token(),message(0),connection(0),
        bus(),raw_key(0),builds(0),sends(0) { submit_frame=this; }
    ~SubmitFrame() { const PreserveErrno saved;submit_frame=previous; }
};
struct PendingFrame {
    PendingFrame* previous;
    void *method,*pending;
    R::WireReply reply;
    bool consumed;
    PendingFrame(void* value,void* node) : previous(pending_frame),method(0),pending(value),reply(),consumed(false) {
        // Exact NA 74.00.324A original callback node; read the same fields as
        // its handler, before calling it. Never write node storage or refs.
        if(node) {
            uint32_t stored_pending=0,stored_method=0;
            memcpy(&stored_pending,static_cast<char*>(node)+24,4);
            if(uintptr_t(stored_pending)==uintptr_t(value)) {
                memcpy(&stored_method,static_cast<char*>(node)+4,4);
                method=reinterpret_cast<void*>(uintptr_t(stored_method));
            }
        }
        pending_frame=this; // Mask outer replies, including unobserved nodes.
    }
    ~PendingFrame() { const PreserveErrno saved;pending_frame=previous; }
};
uint64_t wire_now() { return wire_clock?wire_clock(wire_clock_user):0; }
bool wire_complete(const A::RequestWireApi& api) {
    return api.build && api.send && api.pending && api.steal && api.serial &&
        api.reply_serial && api.type && api.sender && api.error;
}
bool wire_present(const A::RequestWireApi& api) {
    return api.build || api.send || api.pending || api.steal || api.serial ||
        api.reply_serial || api.type || api.sender || api.error;
}
void notify(void* connection, void* method, void* context) {
    ready();
    R::WireReply wire=R::WireReply();
    if(pending_frame && pending_frame->method==method && !pending_frame->consumed) {
        wire=pending_frame->reply;pending_frame->consumed=true;
    }
    R::ReplyScope scope(*observer, method,A::read_bus_connection(connection),wire);
    original.notify(connection, method, context);
    // RAII runs on normal return and unwinding; it preserves original errno.
}
}

extern "C" {
__attribute__((visibility("hidden"))) void* mx5_request_post_trampoline;
__attribute__((visibility("hidden"))) void* mx5_request_work_trampoline;
__attribute__((visibility("hidden"))) void* mx5_request_destroy_trampoline;
}

namespace mx5 { namespace adapter {
bool prepare_request_hooks(const RequestBindings& bindings, R::ObservationClock clock, void* user) {
    const PreserveErrno saved;
    if(attempted || !bindings.submit || !bindings.notify || !bindings.free_method ||
       !bindings.free_method_only || !bindings.position_vptr ||
       !bindings.method.get_destination || !bindings.method.get_path ||
       !bindings.method.get_interface || !bindings.method.get_name ||
       !bindings.post_trampoline || !bindings.work_trampoline || !bindings.destroy_trampoline)
        return false;
    if(wire_present(bindings.wire) && !wire_complete(bindings.wire))return false;
    attempted=true;
    alignas(R::Observer) static unsigned char storage[sizeof(R::Observer)];
    observer=new(storage) R::Observer(bindings.reply, clock, user, bindings.method);
    R::Status status;
    if(!observer->valid() || observer->status(&status)!=R::OK) return false;
    original=bindings;
    wire_clock=clock;wire_clock_user=user;
    mx5_request_post_trampoline=bindings.post_trampoline;
    mx5_request_work_trampoline=bindings.work_trampoline;
    mx5_request_destroy_trampoline=bindings.destroy_trampoline;
    prepared.store(1,std::memory_order_release);
    return true;
}
R::Result read_request_trace(const void* position, R::Trace* out, void*) {
    if(!prepared.load(std::memory_order_acquire)) {
        if(out)*out=R::Trace();
        return R::NOT_READY;
    }
    return observer->position_take(position,out);
}
RequestHookHealth request_hook_health() {
    const PreserveErrno saved;
    RequestHookHealth health=RequestHookHealth();
    health.prepared=prepared.load(std::memory_order_acquire)!=0;
    health.abi_fault=abi_fault.load(std::memory_order_acquire)!=0;
    health.result=health.prepared?observer->status(&health.ledger):R::NOT_READY;
    return health;
}
} }

extern "C" int32_t mx5_request_submit(void* connection, void* method,
                                      A::RequestNotify callback, void* context, int timeout) {
    ready();
    SubmitFrame frame(method); // Unobserved/nested submissions mask the outer token.
    if(callback!=original.notify)
        return original.submit(connection,method,callback,context,timeout);
    A::observe_position_bus(connection);
    R::Endpoint endpoint=R::Endpoint();frame.connection=connection;
    frame.bus=A::read_bus_endpoint(connection,&endpoint,&frame.raw_key);
    observer->request_begin(method,&frame.token,A::read_issue_session(),frame.bus,endpoint);
    // Register before submission; another thread can notify before it returns.
    // Neither a failure status nor elapsed time substitutes for method end.
    return original.submit(connection,method,notify,context,timeout);
}
extern "C" int32_t mx5_request_free(void* method) {
    ready();
    observer->request_end(method);
    return original.free_method(method);
}
extern "C" int32_t mx5_request_free_only(void* method) {
    ready();
    observer->request_end(method);
    return original.free_method_only(method);
}

extern "C" void* mx5_request_message(void* method) {
    ready();
    void* result=original.wire.build(method);
    const PreserveErrno saved;
    if(submit_frame && submit_frame->token.id && submit_frame->method==method) {
        submit_frame->message=result;++submit_frame->builds;
    }
    return result;
}
extern "C" int32_t mx5_request_wire_send(void* connection,void* message,void** pending,int timeout) {
    ready();
    SubmitFrame* frame=submit_frame;
    const bool matched=frame && frame->token.id && message && frame->message==message;
    if(matched)++frame->sends;
    A::EndpointMatch endpoint=A::ENDPOINT_UNAVAILABLE;
    if(matched && frame->raw_key) {
        const PreserveErrno saved;
        endpoint=frame->raw_key!=uintptr_t(connection)?A::ENDPOINT_MISMATCH:
            A::bus_endpoint_matches(frame->connection,frame->bus,frame->raw_key);
    }
    const int32_t result=original.wire.send(connection,message,pending,timeout);
    const PreserveErrno saved;
    // Original JCIDBUS holds its owner mutex across this raw call. Still keep
    // an authored/reentrant lifetime change from claiming an exact join. A
    // change AFTER sending is uncertainty, not proof it sent on another raw
    // connection; retain the header without inventing an endpoint conflict.
    if(endpoint==A::ENDPOINT_MATCH &&
       A::bus_endpoint_matches(frame->connection,frame->bus,frame->raw_key)!=A::ENDPOINT_MATCH)
        endpoint=A::ENDPOINT_UNAVAILABLE;
    // The original registers notify only after this returns. Read the live
    // builder result, never a later method address or coincident routing text.
    if(matched) {
        R::WireIssue issue=R::WireIssue();
        if(result && pending && *pending) {
            issue.serial=original.wire.serial(message);
            issue.known=issue.serial!=0;issue.observed_ns=wire_now();
        }
        issue.conflict=frame->builds!=1 || frame->sends!=1 || endpoint==A::ENDPOINT_MISMATCH;
        issue.endpoint_matched=issue.known && !issue.conflict && endpoint==A::ENDPOINT_MATCH;
        // A second send attempt is ambiguous even when it fails. Preserve the
        // first header as diagnostic evidence but never present it as unique.
        if(issue.known || issue.conflict)observer->wire_issue(frame->token,issue);
    }
    return result;
}
extern "C" void mx5_request_pending(void* pending,void* node) {
    ready();PendingFrame frame(pending,node);
    original.wire.pending(pending,node);
}
extern "C" void* mx5_request_steal(void* pending) {
    ready();
    void* result=original.wire.steal(pending);
    const PreserveErrno saved;
    if(result && pending_frame && pending_frame->method &&
       pending_frame->pending==pending && !pending_frame->consumed) {
        R::WireReply& reply=pending_frame->reply;
        reply=R::WireReply();reply.observed_ns=wire_now();
        reply.serial=original.wire.serial(result);
        reply.reply_serial=original.wire.reply_serial(result);
        reply.type=original.wire.type(result);
        reply.sender=R::copy_text(original.wire.sender(result));
        reply.error_name=R::copy_text(original.wire.error(result));
        reply.known=true;
    }
    return result; // Original message ownership/reference count is unchanged.
}

#if defined(__arm__) && !defined(__ARM_PCS_VFP)
#if !defined(__EXCEPTIONS)
#error "ARM request wrappers require exception cleanup support"
#endif
extern "C" void mx5_arm_invoke(uint32_t* registers, void* target);
extern "C" void mx5_request_post_enter(const void* indirect_shared_ptr) {
    const PreserveErrno saved;
    ready();
    // Exact original static PostWorker: r0 points to the indirect shared_ptr.
    // The refcount is untouched. Inspect only the live original argument.
    if(!indirect_shared_ptr)return;
    uintptr_t worker=0,vptr=0,position=0;
    memcpy(&worker,indirect_shared_ptr,4);
    if(!worker)return;
    memcpy(&vptr,reinterpret_cast<const void*>(worker),4);
    if(vptr!=original.position_vptr)return;
    memcpy(&position,reinterpret_cast<const void*>(worker+80),4);
    if(position!=worker+8) {
        abi_fault.store(1,std::memory_order_release);
        return; // Preserve OEM forwarding; report the violated observation ABI.
    }
    R::Token token;
    observer->worker_post(reinterpret_cast<void*>(worker),reinterpret_cast<void*>(position),&token);
}
extern "C" void mx5_request_work_call(uint32_t* registers) {
    ready();
    R::WorkerScope scope(*observer,reinterpret_cast<void*>(registers[0]));
    mx5_arm_invoke(registers,mx5_request_work_trampoline);
}
extern "C" void mx5_request_destroy_enter(void* worker) {
    ready();
    observer->worker_destroy(worker);
}
#endif
