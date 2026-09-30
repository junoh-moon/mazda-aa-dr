// Test-only access to hidden functions in the exact, unmodified production
// DSO. Offsets come from its own ELF symbol table; no OEM ELF is used here.
#include "unwind_offsets.h"
#include <cstdlib>
#include <dlfcn.h>
#include <link.h>
static uintptr_t test_dso_base;
static void initialize_test_dso() {
    const char* path=std::getenv("MX5_UNWIND_LIBRARY");
    assert(path && *path);
    void* handle=dlopen(path,RTLD_NOW|RTLD_NOLOAD);
    link_map* map=0;
    assert(handle && !dlinfo(handle,RTLD_DI_LINKMAP,&map) && map && map->l_addr);
    assert(!std::strcmp(path,map->l_name));
    test_dso_base=static_cast<uintptr_t>(map->l_addr);
}
static bool dso_configure(SendFunction next,const Options& options) {
    typedef bool (*Function)(SendFunction,const Options&);
    return reinterpret_cast<Function>(test_dso_base+TEST_CONFIGURE)(next,options);
}
static bool dso_mode(Mode mode) {
    return reinterpret_cast<bool(*)(Mode)>(test_dso_base+TEST_MODE)(mode);
}
static int32_t dso_send(void* owner,VehicleData* data) {
    return reinterpret_cast<SendFunction>(test_dso_base+TEST_SEND)(owner,data);
}
static void dso_position(void* owner,const void* input) {
    reinterpret_cast<void(*)(void*,const void*)>(test_dso_base+TEST_POSITION)(owner,input);
}
#define configure dso_configure
#define set_mode dso_mode
#define send_vehicle_data dso_send
#define mx5_position_veneer dso_position
#define mx5_position_trampoline (*reinterpret_cast<void**>(test_dso_base+TEST_TRAMPOLINE))
