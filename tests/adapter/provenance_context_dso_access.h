// Authored fixture access to the selected, unmodified product ELF. Its adapter
// is not linked into the caller; only the fixture's request Ledger is linked.
#include "unwind_offsets.h"
#include <cstdlib>
#include <dlfcn.h>
#include <link.h>

static uintptr_t provenance_context_dso_base;
static void initialize_provenance_context_test_dso() {
    const char* path=std::getenv("MX5_UNWIND_LIBRARY");assert(path&&*path);
    void* handle=dlopen(path,RTLD_NOW|RTLD_NOLOAD);link_map* map=0;
    assert(handle&&!dlinfo(handle,RTLD_DI_LINKMAP,&map)&&map&&map->l_addr);
    assert(!std::strcmp(path,map->l_name));
    provenance_context_dso_base=uintptr_t(map->l_addr);
}
template<class Function> static Function provenance_context_function(uintptr_t offset) {
    return reinterpret_cast<Function>(provenance_context_dso_base+offset);
}
namespace mx5 { namespace adapter {
static bool provenance_context_configure(SendFunction f,const Options& o) {
    return provenance_context_function<bool(*)(SendFunction,const Options&)>(TEST_CONFIGURE)(f,o);
}
static bool provenance_context_set_mode(Mode value) {
    return provenance_context_function<bool(*)(Mode)>(TEST_MODE)(value);
}
static uint32_t provenance_context_generation() {
    return provenance_context_function<uint32_t(*)()>(TEST_GENERATION)();
}
static uint32_t provenance_context_invalidate() {
    return provenance_context_function<uint32_t(*)()>(TEST_INVALIDATE)();
}
static bool provenance_context_publish(const DrSnapshot& value) {
    return provenance_context_function<bool(*)(const DrSnapshot&)>(TEST_PUBLISH)(value);
}
static void provenance_context_position_enter(void* manager,const void* position) {
    provenance_context_function<void(*)(void*,const void*)>(TEST_POSITION_ENTER)(manager,position);
}
static void provenance_context_position_leave() {
    provenance_context_function<void(*)()>(TEST_POSITION_LEAVE)();
}
static int32_t provenance_context_send(void* storage,VehicleData* data) {
    return provenance_context_function<SendFunction>(TEST_VEHICLE_SEND)(storage,data);
}
} }
#define configure provenance_context_configure
#define set_mode provenance_context_set_mode
#define generation provenance_context_generation
#define invalidate provenance_context_invalidate
#define publish_snapshot provenance_context_publish
#define position_enter provenance_context_position_enter
#define position_leave provenance_context_position_leave
#define send_vehicle_data provenance_context_send
