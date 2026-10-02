// Authored callback fixture; every adapter call enters the unchanged product.
#include "unwind_offsets.h"
#include <cstdlib>
#include <dlfcn.h>
#include <link.h>
static uintptr_t association_context_base;
static void initialize_association_context_dso() {
    const char* path=std::getenv("MX5_UNWIND_LIBRARY");assert(path&&*path);
    void* handle=dlopen(path,RTLD_NOW|RTLD_NOLOAD);link_map* map=0;
    assert(handle&&!dlinfo(handle,RTLD_DI_LINKMAP,&map)&&map&&map->l_addr);
    assert(!std::strcmp(path,map->l_name));association_context_base=uintptr_t(map->l_addr);
}
template<class Function> static Function association_function(uintptr_t offset) {
    return reinterpret_cast<Function>(association_context_base+offset);
}
namespace mx5 { namespace adapter {
static bool association_configure(SendFunction f,const Options& o) {
    return association_function<bool(*)(SendFunction,const Options&)>(TEST_CONFIGURE)(f,o);
}
static uint32_t association_generation() { return association_function<uint32_t(*)()>(TEST_GENERATION)(); }
static uint32_t association_invalidate() { return association_function<uint32_t(*)()>(TEST_INVALIDATE)(); }
static void association_enter(void* manager,const void* position) {
    association_function<void(*)(void*,const void*)>(TEST_POSITION_ENTER)(manager,position);
}
static void association_leave() { association_function<void(*)()>(TEST_POSITION_LEAVE)(); }
static int32_t association_send(void* storage,VehicleData* data) {
    return association_function<SendFunction>(TEST_VEHICLE_SEND)(storage,data);
}
} }
#define configure association_configure
#define generation association_generation
#define invalidate association_invalidate
#define position_enter association_enter
#define position_leave association_leave
#define send_vehicle_data association_send
