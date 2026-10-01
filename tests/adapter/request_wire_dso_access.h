// Test-only access to our own unmodified DSO, never to OEM objects.
#include "unwind_offsets.h"
#include <cstdlib>
#include <dlfcn.h>
#include <link.h>
static uintptr_t request_wire_dso_base;
static void initialize_request_wire_test_dso() {
    const char* path=std::getenv("MX5_UNWIND_LIBRARY");assert(path&&*path);
    void* handle=dlopen(path,RTLD_NOW|RTLD_NOLOAD);link_map* map=0;
    assert(handle&&!dlinfo(handle,RTLD_DI_LINKMAP,&map)&&map&&map->l_addr);
    assert(!strcmp(path,map->l_name));request_wire_dso_base=uintptr_t(map->l_addr);
}
namespace mx5 { namespace adapter {
static bool dso_wire_prepare(const RequestBindings& bindings,R::ObservationClock clock,void* user) {
    typedef bool (*Function)(const RequestBindings&,R::ObservationClock,void*);
    return reinterpret_cast<Function>(request_wire_dso_base+TEST_PREPARE)(bindings,clock,user);
}
static RequestHookHealth dso_wire_health() {
    return reinterpret_cast<RequestHookHealth(*)()>(request_wire_dso_base+TEST_HEALTH)();
}
static R::Result dso_wire_read(const void* position,R::Trace* trace,void* user) {
    typedef R::Result (*Function)(const void*,R::Trace*,void*);
    return reinterpret_cast<Function>(request_wire_dso_base+TEST_READ)(position,trace,user);
}
} }
#define prepare_request_hooks dso_wire_prepare
#define request_hook_health dso_wire_health
#define read_request_trace dso_wire_read
#define mx5_request_submit (reinterpret_cast<A::RequestSubmit>(request_wire_dso_base+TEST_SUBMIT))
#define mx5_request_free (reinterpret_cast<A::RequestFree>(request_wire_dso_base+TEST_FREE))
#define mx5_request_free_only (reinterpret_cast<A::RequestFree>(request_wire_dso_base+TEST_FREE_ONLY))
#define mx5_request_message (reinterpret_cast<void*(*)(void*)>(request_wire_dso_base+TEST_MESSAGE))
#define mx5_request_wire_send (reinterpret_cast<int32_t(*)(void*,void*,void**,int)>(request_wire_dso_base+TEST_WIRE_SEND))
#define mx5_request_pending (reinterpret_cast<void(*)(void*,void*)>(request_wire_dso_base+TEST_PENDING))
#define mx5_request_steal (reinterpret_cast<void*(*)(void*)>(request_wire_dso_base+TEST_STEAL))
#define mx5_request_post_enter (reinterpret_cast<void(*)(const void*)>(request_wire_dso_base+TEST_POST_ENTER))
#define mx5_request_work_call (reinterpret_cast<void(*)(uint32_t*)>(request_wire_dso_base+TEST_WORK_CALL))
