// Authored diagnostic caller for the exact NA 74.00.324A OEM libraries.
// Run only in the isolated OEM VM. Not installed in the USB/product bundle.
// The original RaceAap constructs its session and callback table. This probe
// observes its create GOT call and forwards status callbacks exactly once,
// retaining NULL userdata and the original full SessionInfo pointer.
#include "runtime/sha256.h"
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <time.h>
#include <unistd.h>

#if !defined(__arm__) || defined(__ARM_PCS_VFP)
#error "Build with the pinned ARM32 softfp compiler"
#endif

namespace {
typedef void (*StatusCallback)(void*, void*);
struct Callbacks { uintptr_t entry[19]; };
typedef int32_t (*Create)(const char*, void*, const Callbacks*, void**);
typedef int32_t (*Destroy)(void**);
typedef int32_t (*Start)(void**, const void*);
typedef int32_t (*Stop)(void**);
struct VehicleData { uint32_t type; void* payload; uint32_t length; };
typedef int32_t (*Send)(void**, const VehicleData*);
static_assert(sizeof(Callbacks)==76, "Exact callback table");
static_assert(sizeof(VehicleData)==12, "Exact vehicle wrapper");
struct Context {
    StatusCallback next;
    void* userdata;
    void** storage;
};
Context contexts[2]; // Immutable after each create begins; never recycled.
std::atomic<unsigned> next_event(0), creates(0);
Create create_original;
Destroy destroy_original;
uintptr_t original_status;

uint64_t now() {
    const int saved=errno;
    timespec t;
    bool ok=clock_gettime(CLOCK_MONOTONIC,&t)==0;
    errno=saved;
    return ok?uint64_t(t.tv_sec)*1000000000ULL+t.tv_nsec:0;
}
void record(const char* format, ...) {
    const int saved=errno;
    char line[768];
    va_list args;va_start(args,format);
    int n=vsnprintf(line,sizeof line,format,args);va_end(args);
    if(n<0 || size_t(n)>=sizeof line)_exit(92);
    // One stdio operation keeps callback/main records whole in this diagnostic.
    // This blocking logger is deliberately not a production callback sink.
    printf("MX5_SESSION %s\n",line);
    fflush(stdout);
    errno=saved;
}
void require(bool ok,const char* step) {
    if(ok)return;
    record("{\"kind\":\"failure\",\"step\":\"%s\"}",step);
    _exit(91);
}
template<unsigned index> void status(void* user,void* info) {
    const Context& context=contexts[index];
    unsigned event=next_event.fetch_add(1)+1;
    int32_t state=0, detail=0;
    if(info) {
        memcpy(&state,info,4);
        memcpy(&detail,static_cast<char*>(info)+4,4);
    }
    record("{\"kind\":\"status\",\"cycle\":%u,\"event\":%u,\"mono_ns\":%llu,"
           "\"state\":%d,\"detail\":%d,\"data_nonnull\":%s,\"userdata_unchanged\":%s}",
           index+1,event,(unsigned long long)now(),state,detail,
           info?"true":"false",user==context.userdata?"true":"false");
    context.next(user,info);
    record("{\"kind\":\"status_return\",\"cycle\":%u,\"event\":%u}",index+1,event);
}
int32_t create(const char* xml,void* user,const Callbacks* callbacks,void** storage) {
    unsigned index=creates.fetch_add(1);
    require(index<2 && callbacks && storage,"create_arguments");
    require(callbacks->entry[1]==original_status,"original_status_callback");
    Context& context=contexts[index];
    context.next=reinterpret_cast<StatusCallback>(callbacks->entry[1]);
    context.userdata=user;context.storage=storage;
    Callbacks copy=*callbacks;
    copy.entry[1]=index?reinterpret_cast<uintptr_t>(&status<1>):reinterpret_cast<uintptr_t>(&status<0>);
    record("{\"kind\":\"create_begin\",\"cycle\":%u,\"userdata_null\":%s,\"mono_ns\":%llu}",
           index+1,user?"false":"true",(unsigned long long)now());
    int32_t result=create_original(xml,user,&copy,storage);
    record("{\"kind\":\"create_end\",\"cycle\":%u,\"result\":%d,\"handle_nonnull\":%s,\"mono_ns\":%llu}",
           index+1,result,*storage?"true":"false",(unsigned long long)now());
    return result;
}
int32_t destroy(void** storage) {
    unsigned cycle=creates.load();
    require(cycle && cycle<=2 && contexts[cycle-1].storage==storage,"destroy_storage");
    int32_t result=destroy_original(storage);
    record("{\"kind\":\"destroy_end\",\"cycle\":%u,\"result\":%d,\"handle_null\":%s}",
           cycle,result,*storage?"false":"true");
    return result;
}
uintptr_t module(void* handle,const char* symbol,const char* path,uintptr_t offset) {
    void* address=dlsym(handle,symbol);
    Dl_info info;
    require(address && dladdr(address,&info) && info.dli_fbase && info.dli_fname,
            "loaded_module");
    require(strcmp(info.dli_fname,path)==0,"loaded_path");
    uintptr_t base=reinterpret_cast<uintptr_t>(info.dli_fbase);
    require(reinterpret_cast<uintptr_t>(address)==base+offset,"export_offset");
    return base;
}
template<class F> F function(uintptr_t base,uintptr_t offset,uint32_t first) {
    uint32_t actual;memcpy(&actual,reinterpret_cast<void*>(base+offset),4);
    require(actual==first,"function_entry");
    return reinterpret_cast<F>(base+offset);
}
}

int main(int argc,char** argv) {
    require(argc==2 && !strcmp(argv[1],"--isolated-oem-vm"),"explicit_vm_scope");
    alarm(45); // A missing completion is a failed/inconclusive probe, never PASS.
    record("{\"kind\":\"scope\",\"physical_phone\":false,\"oem_queue_started\":false,"
           "\"start_input\":\"synthetic_zero_304_bytes\"}");
    const char* blm="/jci/aapa/blmjciaapa.so";
    const char* interface="/usr/lib/libaap_interface.so";
    const char* common="/jci/lib/libjcicommon_util.so";
    require(mx5_verify_file_sha256(blm,"10e7235bfce075b44c1a8ffc99bbc9b63af85d9262df868ca1874ec36d8d3b71") &&
            mx5_verify_file_sha256(interface,"e9eb5e0d42719c98efc5ef86a270b4b3c86467aced55d4c8158bd99e06bbd436") &&
            mx5_verify_file_sha256(common,"c44304ad0e493eb172e0e298ba5ce6d35ee5d021563dae97af5877852dc98e58"),"firmware_hashes");
    require(dlopen(common,RTLD_NOW|RTLD_GLOBAL)!=0,"common_load");
    void* handle=dlopen(blm,RTLD_NOW|RTLD_LOCAL);
    require(handle!=0,"blm_load");
    uintptr_t b=module(handle,"GetServiceInterfaces",blm,0x61768);
    uintptr_t a=module(handle,"aap_create_session",interface,0x1740c);
    create_original=function<Create>(a,0x1740c,0xe92d4ff0);
    destroy_original=function<Destroy>(a,0x16e00,0xe92d40f0);
    Start start=function<Start>(a,0x18794,0xe92d40f0);
    Stop stop=function<Stop>(a,0x18cdc,0xe92d4070);
    Send send=function<Send>(a,0x1a538,0xe92d4070);
    original_status=b+0x8b128;
    (void)function<StatusCallback>(b,0x8b128,0xe92d4810);
    uintptr_t* slot=reinterpret_cast<uintptr_t*>(b+0xf8988);
    require(*slot==reinterpret_cast<uintptr_t>(create_original),"create_got_chain");
    uintptr_t* destroy_slot=reinterpret_cast<uintptr_t*>(b+0xf7f6c);
    require(*destroy_slot==reinterpret_cast<uintptr_t>(destroy_original),"destroy_got_chain");
    // Only this private caller has loaded the BLM; no OEM producers started.
    *slot=reinterpret_cast<uintptr_t>(&create);
    *destroy_slot=reinterpret_cast<uintptr_t>(&destroy);
    typedef void* (*Instance)();
    typedef void* (*Getter)(void*);
    typedef bool (*Init)(void*,bool);
    typedef void (*Uninit)(void*);
    Instance instance=function<Instance>(b,0x62670,0xe92d4830);
    Getter get_race=function<Getter>(b,0x7f614,0xe52db004);
    Init init=function<Init>(b,0x8c64c,0xe92d4800);
    Uninit uninit=function<Uninit>(b,0x8c988,0xe92d4810);
    void* race=get_race(instance());require(race!=0,"original_singleton");
    uintptr_t previous_handle=0;
    for(unsigned cycle=1;cycle<=2;++cycle) {
        bool result=init(race,true);
        require(creates.load()==cycle && contexts[cycle-1].storage,"original_init_create");
        void** storage=contexts[cycle-1].storage;
        require(result && *storage,"original_init_result");
        uintptr_t current_handle=reinterpret_cast<uintptr_t>(*storage);
        record("{\"kind\":\"identity\",\"cycle\":%u,\"same_handle_address_as_previous\":%s,"
               "\"same_storage_as_previous\":%s}",
               cycle,cycle>1 && current_handle==previous_handle?"true":"false",
               cycle>1 && storage==contexts[cycle-2].storage?"true":"false");
        uint8_t location[48]={0};VehicleData data={1,location,sizeof location};
        record("{\"kind\":\"send\",\"cycle\":%u,\"result\":%d}",cycle,send(storage,&data));
        alignas(8) uint8_t start_info[304]={0};
        int32_t started=start(storage,start_info);
        record("{\"kind\":\"start\",\"cycle\":%u,\"result\":%d}",cycle,started);
        usleep(300000);
        int32_t stopped=stop(storage);
        record("{\"kind\":\"stop\",\"cycle\":%u,\"result\":%d}",cycle,stopped);
        usleep(300000);
        // The original wrapper destroys once and cleans up its semaphore.
        uninit(race);
        previous_handle=current_handle;
        usleep(300000);
    }
    record("{\"kind\":\"complete\",\"cycles\":2,\"status_callbacks\":%u}",next_event.load());
    // AapProc's original queue was never started. Pending original workers
    // and singleton shutdown are explicitly outside this diagnostic's scope.
    return 0;
}
