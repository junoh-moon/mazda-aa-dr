// Test-only access to our own unmodified DSO, never to OEM objects.
#include "unwind_offsets.h"
#include <cstdlib>
#include <dlfcn.h>
#include <link.h>
static uintptr_t request_dso_base;
static void initialize_request_test_dso() {
    const char* path=std::getenv("MX5_UNWIND_LIBRARY");assert(path && *path);
    void* h=dlopen(path,RTLD_NOW|RTLD_NOLOAD);link_map* map=0;
    assert(h && !dlinfo(h,RTLD_DI_LINKMAP,&map) && map && map->l_addr);
    assert(!strcmp(path,map->l_name));request_dso_base=uintptr_t(map->l_addr);
}
namespace mx5 { namespace adapter {
static bool dso_prepare(const RequestBindings& b,R::ObservationClock c,void* u) {
    typedef bool (*F)(const RequestBindings&,R::ObservationClock,void*);
    return reinterpret_cast<F>(request_dso_base+TEST_PREPARE)(b,c,u);
}
static RequestHookHealth dso_health() {
    return reinterpret_cast<RequestHookHealth(*)()>(request_dso_base+TEST_HEALTH)();
}
static R::Result dso_read(const void* p,R::Trace* t,void* u) {
    return reinterpret_cast<R::Result(*)(const void*,R::Trace*,void*)>(request_dso_base+TEST_READ)(p,t,u);
}
} }
#define prepare_request_hooks dso_prepare
#define request_hook_health dso_health
#define read_request_trace dso_read
#define mx5_request_submit (reinterpret_cast<A::RequestSubmit>(request_dso_base+TEST_SUBMIT))
#define mx5_request_free (reinterpret_cast<A::RequestFree>(request_dso_base+TEST_FREE))
#define mx5_request_free_only (reinterpret_cast<A::RequestFree>(request_dso_base+TEST_FREE_ONLY))
#define mx5_request_post_enter (reinterpret_cast<void(*)(const void*)>(request_dso_base+TEST_POST_ENTER))
#define mx5_request_post_veneer (reinterpret_cast<void(*)()>(request_dso_base+TEST_POST))
#define mx5_request_work_veneer (reinterpret_cast<void(*)()>(request_dso_base+TEST_WORK))
#define mx5_request_destroy_veneer (reinterpret_cast<void(*)()>(request_dso_base+TEST_DESTROY))
