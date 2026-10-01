// Fixture access to OUR unchanged production DSO, including its real journal
// worker. Offsets come from this selected ELF; no OEM offsets are involved.
#include "unwind_offsets.h"
#include "runtime/config.h"
#include <cstdlib>
#include <dlfcn.h>
#include <link.h>
#include <type_traits>

static uintptr_t runtime_assist_dso_base;
static void initialize_runtime_assist_test_dso() {
    const char* path=std::getenv("MX5_UNWIND_LIBRARY");assert(path&&*path);
    void* handle=dlopen(path,RTLD_NOW|RTLD_NOLOAD);link_map* map=0;
    assert(handle&&!dlinfo(handle,RTLD_DI_LINKMAP,&map)&&map&&map->l_addr);
    assert(!std::strcmp(path,map->l_name));runtime_assist_dso_base=uintptr_t(map->l_addr);
}
template<class Function> static Function runtime_assist_function(uintptr_t offset) {
    return reinterpret_cast<Function>(runtime_assist_dso_base+offset);
}
class ProductAssistWorker {
    alignas(R::AssistWorker) unsigned char storage_[sizeof(R::AssistWorker)];
public:
    ProductAssistWorker(const mx5_dr_config& config,const R::AssistSource& source) {
        static_assert(std::is_trivially_destructible<R::AssistWorker>::value,
                      "Add product destruction if needed");
        runtime_assist_function<void(*)(R::AssistWorker*,const mx5_dr_config&,
            const R::AssistSource&)>(TEST_ASSIST_CONSTRUCT)(self(),config,source);
    }
    R::AssistWorker* self() { return reinterpret_cast<R::AssistWorker*>(storage_); }
    // Like the product API, this is single-owner data. Inspect after joining
    // the worker; do not poll non-atomic product state from the fixture thread.
    const R::AssistStatus& status() const {
        return reinterpret_cast<const R::AssistWorker*>(storage_)->status();
    }
};
static void configure_runtime(unsigned mode,bool hooks) {
    // Set startup-only inputs before creating the worker thread.
    reinterpret_cast<R::Config*>(runtime_assist_dso_base+TEST_RUNTIME_CONFIG)->mode=mode;
    *reinterpret_cast<bool*>(runtime_assist_dso_base+TEST_HOOK_INSTALLED)=hooks;
}
static void record_raw(const A::Observation* observation) {
    runtime_assist_function<void(*)(const A::Observation*,void*)>(TEST_RUNTIME_SINK)(observation,0);
}
static void audit_failure() {
    runtime_assist_function<void(*)()>(TEST_AUDIT_FAILURE)();
}
static void run_product_worker(const char* root,ProductAssistWorker* assist) {
    runtime_assist_function<void*(*)(const char*,const char*,R::AssistWorker*)>(TEST_RUN_WORKER)(
        root,"mx5dr.assist.test",assist?assist->self():0);
}
static mx5_dr_config dso_default_config() {
    return runtime_assist_function<mx5_dr_config(*)()>(TEST_DEFAULT_CONFIG)();
}
namespace mx5 { namespace adapter {
static bool dso_configure(SendFunction next,const Options& options) {
    return runtime_assist_function<bool(*)(SendFunction,const Options&)>(TEST_CONFIGURE)(next,options);
}
static bool dso_set_mode(Mode mode) {
    return runtime_assist_function<bool(*)(Mode)>(TEST_MODE)(mode);
}
// Keep the real function spelling: a macro named generation would also
// rewrite mx5_dr_context::generation accesses in this fixture.
inline uint32_t generation() {
    return runtime_assist_function<uint32_t(*)()>(TEST_GENERATION)();
}
static void dso_position_enter(void* manager,const void* position) {
    runtime_assist_function<void(*)(void*,const void*)>(TEST_POSITION_ENTER)(manager,position);
}
static void dso_position_leave() {
    runtime_assist_function<void(*)()>(TEST_POSITION_LEAVE)();
}
static int32_t dso_send(void* storage,VehicleData* data) {
    return runtime_assist_function<SendFunction>(TEST_VEHICLE_SEND)(storage,data);
}
} }
#define mx5_dr_default_config dso_default_config
#define configure dso_configure
#define set_mode dso_set_mode
#define position_enter dso_position_enter
#define position_leave dso_position_leave
#define send_vehicle_data dso_send
