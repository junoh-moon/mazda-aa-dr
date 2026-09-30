#ifndef MX5_ADAPTER_REQUEST_HOOKS_H
#define MX5_ADAPTER_REQUEST_HOOKS_H

#include "runtime/request_observer.h"

namespace mx5 { namespace adapter {
namespace R = runtime::request_trace;
typedef void (*RequestNotify)(void* connection, void* method, void* context);
typedef int32_t (*RequestSubmit)(void*, void*, RequestNotify, void*, int);
typedef int32_t (*RequestFree)(void* method);

// Pinned and checked by the installer before any pointer becomes reachable.
// Context and OEM ownership are unchanged by every forwarding wrapper.
struct RequestBindings {
    R::ReplyApi reply;
    R::MethodApi method;
    RequestSubmit submit;
    RequestNotify notify;
    RequestFree free_method, free_method_only;
    uintptr_t position_vptr;
    void *post_trampoline, *work_trampoline, *destroy_trampoline;
};
// One initialization before installation. Storage and original targets remain
// alive for this process, including after an installer rollback: an existing
// JCIDBUS thread may already have entered a pointer wrapper during publication.
bool prepare_request_hooks(const RequestBindings&, R::ObservationClock, void*);
R::Result read_request_trace(const void* position, R::Trace*, void*);
struct RequestHookHealth {
    bool prepared, abi_fault;
    R::Result result;
    R::Status ledger;
};
RequestHookHealth request_hook_health();
} }

extern "C" int32_t mx5_request_submit(void*, void*, mx5::adapter::RequestNotify, void*, int);
extern "C" int32_t mx5_request_free(void*);
extern "C" int32_t mx5_request_free_only(void*);
extern "C" void mx5_request_post_veneer();
extern "C" void mx5_request_work_veneer();
extern "C" void mx5_request_destroy_veneer();

#endif
