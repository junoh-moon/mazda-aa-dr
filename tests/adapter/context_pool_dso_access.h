// Only address offsets from the selected, unmodified product ELF are called.
#include "unwind_offsets.h"
#include <cstdlib>
#include <dlfcn.h>
#include <link.h>

static uintptr_t context_pool_dso_base;
static void initialize_context_pool_test_dso() {
    const char* path=std::getenv("MX5_UNWIND_LIBRARY");assert(path&&*path);
    void* handle=dlopen(path,RTLD_NOW|RTLD_NOLOAD);link_map* map=0;
    assert(handle&&!dlinfo(handle,RTLD_DI_LINKMAP,&map)&&map&&map->l_addr);
    assert(!std::strcmp(path,map->l_name));
    context_pool_dso_base=uintptr_t(map->l_addr);
}
template<class Function> static Function context_pool_function(uintptr_t offset) {
    return reinterpret_cast<Function>(context_pool_dso_base+offset);
}
namespace mx5 { namespace adapter {
static bool pool_configure(SendFunction f,const Options& o) {
    return context_pool_function<bool(*)(SendFunction,const Options&)>(TEST_CONFIGURE)(f,o);
}
static bool pool_set_mode(Mode value) {
    return context_pool_function<bool(*)(Mode)>(TEST_MODE)(value);
}
static uint32_t pool_generation() {
    return context_pool_function<uint32_t(*)()>(TEST_GENERATION)();
}
static bool pool_publish(const DrSnapshot& value) {
    return context_pool_function<bool(*)(const DrSnapshot&)>(TEST_PUBLISH)(value);
}
static void pool_enter(void* manager,const void* position) {
    context_pool_function<void(*)(void*,const void*)>(TEST_POSITION_ENTER)(manager,position);
}
static void pool_leave() {
    context_pool_function<void(*)()>(TEST_POSITION_LEAVE)();
}
static int32_t pool_send(void* storage,VehicleData* data) {
    return context_pool_function<SendFunction>(TEST_VEHICLE_SEND)(storage,data);
}
} }
#define configure pool_configure
#define set_mode pool_set_mode
#define generation pool_generation
#define publish_snapshot pool_publish
#define position_enter pool_enter
#define position_leave pool_leave
#define send_vehicle_data pool_send
