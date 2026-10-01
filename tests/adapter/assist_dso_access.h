// Fixture access to OUR unmodified product library. All offsets are extracted
// from the selected ELF, never from an OEM binary or a different build.
#include "unwind_offsets.h"
#include <cstdlib>
#include <dlfcn.h>
#include <link.h>
#include <type_traits>
static uintptr_t assist_dso_base;
static void initialize_assist_test_dso() {
    const char* path=std::getenv("MX5_UNWIND_LIBRARY");assert(path&&*path);
    void* handle=dlopen(path,RTLD_NOW|RTLD_NOLOAD);link_map* map=0;
    assert(handle&&!dlinfo(handle,RTLD_DI_LINKMAP,&map)&&map&&map->l_addr);
    assert(!std::strcmp(path,map->l_name));assist_dso_base=uintptr_t(map->l_addr);
}
template<class Function> static Function assist_function(uintptr_t offset) {
    return reinterpret_cast<Function>(assist_dso_base+offset);
}
static uint32_t current_generation() { return assist_function<uint32_t(*)()>(TEST_GENERATION)(); }
class ProductPipeline {
    alignas(N::Pipeline) unsigned char storage_[sizeof(N::Pipeline)];
    N::Pipeline* self() { return reinterpret_cast<N::Pipeline*>(storage_); }
public:
    ProductPipeline() {
        assist_function<void(*)(N::Pipeline*)>(TEST_PIPELINE_CONSTRUCT)(self());
    }
    ~ProductPipeline() { assist_function<void(*)(N::Pipeline*)>(TEST_PIPELINE_DESTRUCT)(self()); }
    bool init_qualified(const mx5_dr_config& c,mx5_dr_context x) {
        return assist_function<bool(*)(N::Pipeline*,const mx5_dr_config&,mx5_dr_context)>(TEST_PIPELINE_INIT)(self(),c,x);
    }
    bool init_model(const N::ModelProfile& p,const mx5_dr_config& c,mx5_dr_context x,bool auto_bias=false,bool gps_wheel=false) {
        return assist_function<bool(*)(N::Pipeline*,const N::ModelProfile&,const mx5_dr_config&,mx5_dr_context,bool,bool)>(TEST_PIPELINE_MODEL_INIT)(self(),p,c,x,auto_bias,gps_wheel);
    }
    bool bind_qualified_revoker(N::Pipeline::QualifiedRevoker revoke,void* user) {
        return assist_function<bool(*)(N::Pipeline*,N::Pipeline::QualifiedRevoker,void*)>(TEST_PIPELINE_BIND)(self(),revoke,user);
    }
    N::PipelineResult enqueue_anchor(const mx5_dr_anchor& a,uint64_t received,uint64_t call_sequence) {
        return assist_function<N::PipelineResult(*)(N::Pipeline*,const mx5_dr_anchor&,uint64_t,uint64_t)>(TEST_PIPELINE_ANCHOR)(self(),a,received,call_sequence);
    }
    N::PipelineResult enqueue_position(const A::Observation& o) {
        return assist_function<N::PipelineResult(*)(N::Pipeline*,const A::Observation&)>(TEST_PIPELINE_POSITION)(self(),o);
    }
    N::PipelineResult enqueue_speed(const mx5_dr_evidence& e,double speed) {
        return assist_function<N::PipelineResult(*)(N::Pipeline*,const mx5_dr_evidence&,double)>(TEST_PIPELINE_SPEED)(self(),e,speed);
    }
    N::PipelineResult enqueue_reverse(const mx5_dr_evidence& e,int reverse) {
        return assist_function<N::PipelineResult(*)(N::Pipeline*,const mx5_dr_evidence&,int)>(TEST_PIPELINE_REVERSE)(self(),e,reverse);
    }
    N::PipelineResult enqueue_yaw(const mx5_dr_evidence& e,double yaw,uint16_t raw,uint16_t count,uint64_t begin,uint64_t end) {
        return assist_function<N::PipelineResult(*)(N::Pipeline*,const mx5_dr_evidence&,double,uint16_t,uint16_t,uint64_t,uint64_t)>(TEST_PIPELINE_YAW)(self(),e,yaw,raw,count,begin,end);
    }
    N::PipelineResult drain(uint64_t watermark) {
        return assist_function<N::PipelineResult(*)(N::Pipeline*,uint64_t)>(TEST_PIPELINE_DRAIN)(self(),watermark);
    }
    N::Diagnostic diagnostic(uint64_t now) {
        return assist_function<N::Diagnostic(*)(const N::Pipeline*,uint64_t)>(TEST_PIPELINE_DIAGNOSTIC)(self(),now);
    }
    R::CoreBridgeResult qualified_publication(uint64_t now,const R::CoreBridgeQualification& q,uint64_t until,A::DrSnapshot* out) {
        return assist_function<R::CoreBridgeResult(*)(const N::Pipeline*,uint64_t,const R::CoreBridgeQualification&,uint64_t,A::DrSnapshot*)>(TEST_PIPELINE_PUBLICATION)(self(),now,q,until,out);
    }
};
static mx5_dr_config dso_default_config() {
    return assist_function<mx5_dr_config(*)()>(TEST_DEFAULT_CONFIG)();
}
namespace mx5 { namespace adapter {
static bool dso_configure(SendFunction f,const Options& o) {
    return assist_function<bool(*)(SendFunction,const Options&)>(TEST_CONFIGURE)(f,o);
}
static bool dso_set_mode(Mode mode) { return assist_function<bool(*)(Mode)>(TEST_MODE)(mode); }
static uint32_t dso_invalidate() { return assist_function<uint32_t(*)()>(TEST_INVALIDATE)(); }
static bool dso_publish(const DrSnapshot& s) {
    return assist_function<bool(*)(const DrSnapshot&)>(TEST_PUBLISH)(s);
}
static void dso_position_enter(void* manager,const void* position) {
    assist_function<void(*)(void*,const void*)>(TEST_POSITION_ENTER)(manager,position);
}
static void dso_position_leave() { assist_function<void(*)()>(TEST_POSITION_LEAVE)(); }
static int32_t dso_send(void* storage,VehicleData* data) {
    return assist_function<SendFunction>(TEST_VEHICLE_SEND)(storage,data);
}
} }
#define mx5_dr_default_config dso_default_config
#define configure dso_configure
#define set_mode dso_set_mode
#define invalidate dso_invalidate
#define publish_snapshot dso_publish
#define position_enter dso_position_enter
#define position_leave dso_position_leave
#define send_vehicle_data dso_send
