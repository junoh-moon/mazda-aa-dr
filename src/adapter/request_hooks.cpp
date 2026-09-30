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
void notify(void* connection, void* method, void* context) {
    ready();
    R::ReplyScope scope(*observer, method,A::read_bus_connection(connection));
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
    attempted=true;
    alignas(R::Observer) static unsigned char storage[sizeof(R::Observer)];
    observer=new(storage) R::Observer(bindings.reply, clock, user, bindings.method);
    R::Status status;
    if(!observer->valid() || observer->status(&status)!=R::OK) return false;
    original=bindings;
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
    if(callback!=original.notify)
        return original.submit(connection,method,callback,context,timeout);
    A::observe_position_bus(connection);
    R::Token token;
    observer->request_begin(method,&token,A::read_issue_session(),A::read_bus_connection(connection));
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
