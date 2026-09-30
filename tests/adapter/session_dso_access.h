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
static bool dso_configure(SendFunction f,const Options& o) {
    return reinterpret_cast<bool(*)(SendFunction,const Options&)>(session_dso_base+TEST_CONFIGURE)(f,o);
}
static bool dso_set_mode(Mode m) {
    return reinterpret_cast<bool(*)(Mode)>(session_dso_base+TEST_MODE)(m);
}
static uint32_t dso_generation() {
    return reinterpret_cast<uint32_t(*)()>(session_dso_base+TEST_GENERATION)();
}
static bool dso_publish(const DrSnapshot& s) {
    return reinterpret_cast<bool(*)(const DrSnapshot&)>(session_dso_base+TEST_PUBLISH)(s);
}
static void dso_position_enter(void* manager,const void* p) {
    reinterpret_cast<void(*)(void*,const void*)>(session_dso_base+TEST_POSITION_ENTER)(manager,p);
}
static void dso_position_leave() {
    reinterpret_cast<void(*)()>(session_dso_base+TEST_POSITION_LEAVE)();
}
static int32_t dso_send_vehicle_data(void* storage,VehicleData* data) {
    return reinterpret_cast<SendFunction>(session_dso_base+TEST_VEHICLE_SEND)(storage,data);
}
} }
#define prepare_session_hooks dso_session_prepare
#define session_hook_health dso_session_health
#define read_issue_session dso_session_issue
#define read_send_session dso_session_send
#define configure dso_configure
#define set_mode dso_set_mode
#define generation dso_generation
#define publish_snapshot dso_publish
#define position_enter dso_position_enter
#define position_leave dso_position_leave
#define send_vehicle_data dso_send_vehicle_data
#define mx5_session_create (reinterpret_cast<A::SessionCreate>(session_dso_base+TEST_CREATE))
#define mx5_session_destroy (reinterpret_cast<A::SessionDestroy>(session_dso_base+TEST_DESTROY))
