#ifndef MX5_ADAPTER_SESSION_HOOKS_H
#define MX5_ADAPTER_SESSION_HOOKS_H
#include "runtime/session_trace.h"
#include <stddef.h>

namespace mx5 { namespace adapter {
namespace S = runtime::session_trace;
struct SessionCallbacks { uintptr_t entry[19]; };
typedef void (*SessionStatus)(void* userdata, void* full_info);
typedef int32_t (*SessionCreate)(const char*, void*, const SessionCallbacks*, void**);
typedef int32_t (*SessionDestroy)(void**);
struct SessionBindings {
    SessionCreate create;
    SessionDestroy destroy;
    SessionStatus status; // Exact original BLM callback; other tables pass through.
};
enum { SESSION_CONTEXT_CAPACITY = 64 };
enum SessionFault {
    SESSION_CAPACITY = 1, SESSION_CONTENTION = 2, SESSION_CALLBACK = 4,
    SESSION_UNWIND = 8, SESSION_EVENT_EXHAUSTED = 16, SESSION_UNTRACKED = 32
};
// One cold initialization. All callback contexts/tables live until process exit.
// They are never recycled: a late callback must still call its original target.
bool prepare_session_hooks(const SessionBindings&);
struct SessionHealth { bool prepared; unsigned contexts, faults; };
SessionHealth session_hook_health();
// At issue: a unique live observed context, NOT proof of request ownership.
S::Snapshot read_issue_session();
// At send: lookup the actual storage argument without retaining/dereferencing it.
void read_send_session(const void* storage, S::Snapshot*, void*);
} }
extern "C" int32_t mx5_session_create(const char*,void*,const mx5::adapter::SessionCallbacks*,void**);
extern "C" int32_t mx5_session_destroy(void**);
#endif
