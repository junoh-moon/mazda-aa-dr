// Access hidden symbols in our unmodified product DSO, never OEM internals.
#include "unwind_offsets.h"
#include <cstdlib>
#include <dlfcn.h>
#include <link.h>
static uintptr_t session_dso_base;
static void initialize_session_test_dso() {
    const char* path=std::getenv("MX5_UNWIND_LIBRARY");assert(path && *path);
    void* h=dlopen(path,RTLD_NOW|RTLD_NOLOAD);link_map* map=0;
    assert(h && !dlinfo(h,RTLD_DI_LINKMAP,&map) && map && map->l_addr);
    assert(!strcmp(path,map->l_name));session_dso_base=uintptr_t(map->l_addr);
}
namespace mx5 { namespace adapter {
static bool dso_session_prepare(const SessionBindings& b) {
    return reinterpret_cast<bool(*)(const SessionBindings&)>(session_dso_base+TEST_PREPARE)(b);
}
static SessionHealth dso_session_health() {
    return reinterpret_cast<SessionHealth(*)()>(session_dso_base+TEST_HEALTH)();
}
static S::Snapshot dso_session_issue() {
    return reinterpret_cast<S::Snapshot(*)()>(session_dso_base+TEST_ISSUE)();
}
static void dso_session_send(const void* p,S::Snapshot* s,void* u) {
    reinterpret_cast<void(*)(const void*,S::Snapshot*,void*)>(session_dso_base+TEST_SEND)(p,s,u);
}
} }
#define prepare_session_hooks dso_session_prepare
#define session_hook_health dso_session_health
#define read_issue_session dso_session_issue
#define read_send_session dso_session_send
#define mx5_session_create (reinterpret_cast<A::SessionCreate>(session_dso_base+TEST_CREATE))
#define mx5_session_destroy (reinterpret_cast<A::SessionDestroy>(session_dso_base+TEST_DESTROY))
