#include "adapter/session_hooks.h"
#include <cassert>
#include <cstdio>

namespace A=mx5::adapter;
namespace S=mx5::runtime::session_trace;
static int handle;
static void* storage;
static unsigned callbacks;
static void status(void* user,void* info) {
    assert(!user && *static_cast<int*>(info)==7);++callbacks;
}
static int32_t create(const char*,void* user,const A::SessionCallbacks* table,void** out) {
    int info=7;
    reinterpret_cast<A::SessionStatus>(table->entry[1])(user,&info);
    *out=&handle;return 0;
}
static int32_t destroy(void** out) { *out=0;return 0; }

// A preload interposer may run before its ordinary global constructors.
// Observe a complete early creation, then verify it survives those constructors.
__attribute__((constructor(101))) static void early_create() {
    const A::SessionBindings bindings={create,destroy,status};
    assert(A::prepare_session_hooks(bindings));
    A::SessionCallbacks table=A::SessionCallbacks();
    table.entry[1]=reinterpret_cast<uintptr_t>(status);
    assert(mx5_session_create("early",0,&table,&storage)==0);
    assert(A::read_issue_session().result==S::OBSERVED);
}
int main() {
    const S::Snapshot observed=A::read_issue_session();
    assert(observed.result==S::OBSERVED && observed.lifetime==1);
    assert(observed.state_known && observed.state==7 && observed.event==1);
    assert(callbacks==1 && storage==&handle);
    assert(mx5_session_destroy(&storage)==0 && !storage);
    assert(A::read_issue_session().result==S::NONE);
    assert(!A::session_hook_health().faults);
    puts("session before global constructors: ok");
}
