// Access hidden symbols of our unmodified product DSO, never OEM internals.
#include "unwind_offsets.h"
#include <cstdlib>
#include <dlfcn.h>
#include <link.h>
static uintptr_t bus_dso_base;
static void initialize_bus_test_dso() {
    const char* path=std::getenv("MX5_UNWIND_LIBRARY");assert(path && *path);
    void* h=dlopen(path,RTLD_NOW|RTLD_NOLOAD);link_map* map=0;
    assert(h && !dlinfo(h,RTLD_DI_LINKMAP,&map) && map && map->l_addr);
    assert(!strcmp(path,map->l_name));bus_dso_base=uintptr_t(map->l_addr);
}
namespace mx5 { namespace adapter {
static bool dso_bus_prepare(const BusBindings& b) {
    return reinterpret_cast<bool(*)(const BusBindings&)>(bus_dso_base+TEST_PREPARE)(b);
}
static BusHealth dso_bus_health() {
    return reinterpret_cast<BusHealth(*)()>(bus_dso_base+TEST_HEALTH)();
}
static B::Snapshot dso_bus_read(const void* p) {
    return reinterpret_cast<B::Snapshot(*)(const void*)>(bus_dso_base+TEST_READ)(p);
}
} }
#define prepare_bus_hooks dso_bus_prepare
#define bus_hook_health dso_bus_health
#define read_bus_connection dso_bus_read
#define mx5_bus_create (reinterpret_cast<A::BusCreate>(bus_dso_base+TEST_CREATE))
#define mx5_bus_connect (reinterpret_cast<A::BusConnect>(bus_dso_base+TEST_CONNECT))
#define mx5_bus_disconnect (reinterpret_cast<A::BusEnd>(bus_dso_base+TEST_DISCONNECT))
#define mx5_bus_free (reinterpret_cast<A::BusEnd>(bus_dso_base+TEST_FREE))
#define mx5_bus_signal (reinterpret_cast<A::BusSignal>(bus_dso_base+TEST_SIGNAL))
