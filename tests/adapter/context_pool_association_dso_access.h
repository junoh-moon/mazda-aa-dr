// Selected unmodified product ELF only; the fixture does not link adapter.cpp.
// Initialization occurs in the caller's constructor(101), after LD_PRELOAD DSO
// initialization. The runner pins the DSO, symbols, fixture, and link inputs.
#include "unwind_offsets.h"
#include <cstdlib>
#include <dlfcn.h>
#include <link.h>

static uintptr_t context_pool_association_dso_base;
static void initialize_context_pool_association_test_dso() {
    const char* path=std::getenv("MX5_UNWIND_LIBRARY");assert(path&&*path);
    void* handle=dlopen(path,RTLD_NOW|RTLD_NOLOAD);link_map* map=0;
    assert(handle&&!dlinfo(handle,RTLD_DI_LINKMAP,&map)&&map&&map->l_addr);
    assert(!std::strcmp(path,map->l_name));
    context_pool_association_dso_base=uintptr_t(map->l_addr);
}
template<class Function> static Function context_pool_association_function(uintptr_t offset) {
    return reinterpret_cast<Function>(context_pool_association_dso_base+offset);
}
namespace mx5 { namespace adapter {
static bool pool_association_configure(SendFunction f,const Options& o) {
    return context_pool_association_function<bool(*)(SendFunction,const Options&)>(TEST_CONFIGURE)(f,o);
}
static bool pool_association_set_mode(Mode value) {
    return context_pool_association_function<bool(*)(Mode)>(TEST_MODE)(value);
}
static uint32_t pool_association_generation() {
    return context_pool_association_function<uint32_t(*)()>(TEST_GENERATION)();
}
static bool pool_association_faulted() {
    return context_pool_association_function<bool(*)()>(TEST_FAULT)();
}
static void pool_association_enter(void* manager,const void* position) {
    context_pool_association_function<void(*)(void*,const void*)>(TEST_POSITION_ENTER)(manager,position);
}
static void pool_association_leave() {
    context_pool_association_function<void(*)()>(TEST_POSITION_LEAVE)();
}
static int32_t pool_association_send(void* storage,VehicleData* data) {
    return context_pool_association_function<SendFunction>(TEST_VEHICLE_SEND)(storage,data);
}
static bool pool_association_decode(const void* raw,PositionInput* out) {
    return context_pool_association_function<bool(*)(const void*,PositionInput*)>(TEST_DECODE_POSITION)(raw,out);
}
} }
#define configure pool_association_configure
#define set_mode pool_association_set_mode
#define generation pool_association_generation
#define faulted pool_association_faulted
#define position_enter pool_association_enter
#define position_leave pool_association_leave
#define send_vehicle_data pool_association_send
#define decode_position pool_association_decode
