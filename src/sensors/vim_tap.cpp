#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "vim_source.h"
#include "runtime/config.h"
#include "runtime/sha256.h"
#include <dlfcn.h>
#include <errno.h>
#include <new>
#include <pthread.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

extern "C" __attribute__((visibility("default"))) int VIMC_AddClient(
    uint64_t,uint64_t,const mx5::sensors::VimCallbacks*);
namespace {
using namespace mx5::sensors;
using namespace mx5::navigation;
const unsigned ROUTES=8;
struct Route { uint64_t client,server;VimCallback original;VimCallbacks wrapper;bool used; };
Route routes[ROUTES]={};
pthread_mutex_t registry_lock=PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t sender_lock=PTHREAD_MUTEX_INITIALIZER;
VimAddClient real_add=0;
uint32_t init_phase=0;
bool observation_enabled=false;
uint64_t epoch=0;
uint32_t sequence=0;
alignas(MotionSender) unsigned char sender_storage[sizeof(MotionSender)];
MotionSender* sender=0;
#ifdef MX5_VIM_TAP_TESTING
bool test_accept_tables=false;
bool (*test_send)(const RawEvent&)=0;
#endif
uint64_t now_ns() {
    timespec ts;
    if(clock_gettime(CLOCK_MONOTONIC,&ts) || ts.tv_sec<0)return 0;
    return uint64_t(ts.tv_sec)*1000000000ull+uint64_t(ts.tv_nsec);
}
// Fixed routes are immutable after registration. An old queued OEM callback
// never changes ownership when a later registration installs a different one.
void receive(unsigned index,uint64_t client,uint64_t server,const VimMessage* msg) {
    Route& route=routes[index];
    const int incoming_errno=errno;
    if(observation_enabled && msg && client==route.client && server==route.server &&
       (msg->id==0x100 || msg->id==0x116 || msg->id==0x118)) {
        const uint32_t seq=__sync_add_and_fetch(&sequence,1);
        RawEvent event;
        // The observation sequence is never a physical producer sequence.
        if(seq && decode_vim_message(*msg,epoch,seq,now_ns(),&event)==VIM_DECODED &&
           pthread_mutex_trylock(&sender_lock)==0) {
#ifdef MX5_VIM_TAP_TESTING
            if(test_send)test_send(event);else
#endif
            if(sender)sender->send_event(event); // bounded nonblocking send; no retry
            pthread_mutex_unlock(&sender_lock);
        }
        // Missing datagrams/invalid payloads retain the consumed sequence gap.
    }
    errno=incoming_errno;
    route.original(client,server,msg); // exact original pointer and exactly once
    // Preserve errno left by the OEM callback, not our preceding observation.
}
template<unsigned N> void wrapped(uint64_t c,uint64_t s,const VimMessage* m) { receive(N,c,s,m); }
VimCallback wrappers[ROUTES]={wrapped<0>,wrapped<1>,wrapped<2>,wrapped<3>,wrapped<4>,wrapped<5>,wrapped<6>,wrapped<7>};
bool stock_function(void* fn,const char* expected,uint32_t offset) {
    Dl_info info;
    if(!fn || !dladdr(fn,&info) || !info.dli_fbase || !info.dli_fname)return false;
    return uintptr_t(fn)-uintptr_t(info.dli_fbase)==offset &&
        mx5_verify_file_sha256(info.dli_fname,expected);
}
bool stock_table(const VimCallbacks* table) {
#ifdef MX5_VIM_TAP_TESTING
    if(test_accept_tables)return table && table->app_event;
#endif
    if(!table || !table->app_event)return false;
    Dl_info info;
    if(!dladdr(reinterpret_cast<void*>(table->app_event),&info) || !info.dli_fbase)return false;
    return uintptr_t(table)==uintptr_t(info.dli_fbase)+0x4625c &&
        stock_function(reinterpret_cast<void*>(table->app_event),
            "bc478f2409e473ef3da8084ebce3b699643b5ee355485122521099d47c78ef22",0xa8b0);
}
VimAddClient resolve_original() {
#ifdef MX5_VIM_TAP_TESTING
    return real_add;
#else
    // Loader calls are deliberately outside every tap mutex.
    VimAddClient result=reinterpret_cast<VimAddClient>(dlsym(RTLD_NEXT,"VIMC_AddClient"));
    if(result==VIMC_AddClient)result=0;
    if(!result) {
        void* h=dlopen("/jci/lib/libjcivim_api.so",RTLD_LAZY|RTLD_NOLOAD|RTLD_LOCAL);
        if(h)result=reinterpret_cast<VimAddClient>(dlsym(h,"VIMC_AddClient"));
        // Keep the owner mapped: callbacks can outlive initialization scope.
        if(result==VIMC_AddClient)result=0;
    }
    return result;
#endif
}
void initialize(VimAddClient forward) {
    real_add=forward;
    mx5::runtime::Config config;
    if(sizeof(void*)!=4 || !real_add ||
       !mx5::runtime::startup_enabled("/data_persist/mx5-aa-dr/mx5dr.conf",
           "/data_persist/mx5-aa-dr/logs/disable-next-start",&config) || config.mode!=4)return;
    if(!stock_function(reinterpret_cast<void*>(real_add),
        "c9a8409743e304fc096336f90f9b262a93bc996f3e56a3a0bf5012672da0256b",0x1070) ||
       !mx5_verify_file_sha256("/jci/vim/vim_app",
        "1d3657cfe451dcdc02920b682bee3196fefaa431c3516f774d0ba603cf118605"))return;
    epoch=now_ns();if(!epoch)return;
    sender=new(sender_storage) MotionSender();
    observation_enabled=sender->open_channel();
}
}
extern "C" __attribute__((visibility("default"))) int VIMC_AddClient(
    uint64_t client,uint64_t server,const mx5::sensors::VimCallbacks* callbacks) {
    const int incoming_errno=errno;
    mx5::sensors::VimAddClient forward=resolve_original();
    if(__sync_bool_compare_and_swap(&init_phase,0,1)) {
        initialize(forward);
        __sync_synchronize();
        __sync_lock_test_and_set(&init_phase,2);
    }
    // Recursive/concurrent calls during initialization do not wait. They use
    // their resolved original with the original callback table unchanged.
    const bool ready=__sync_fetch_and_add(&init_phase,0)==2;
    const bool observe=ready && observation_enabled && forward==real_add &&
        client==(uint64_t(0x80000068u)<<32) && server==(uint64_t(0x80000067u)<<32) &&
        stock_table(callbacks); // dladdr/hash outside registry lock
    const mx5::sensors::VimCallbacks* chosen=callbacks;
    if(observe) {
        pthread_mutex_lock(&registry_lock);
        unsigned free_index=ROUTES;
        for(unsigned i=0;i<ROUTES;++i) {
            if(routes[i].used && routes[i].client==client && routes[i].server==server &&
               routes[i].original==callbacks->app_event) {chosen=&routes[i].wrapper;break;}
            if(!routes[i].used && free_index==ROUTES)free_index=i;
        }
        if(chosen==callbacks && free_index<ROUTES) {
            Route& r=routes[free_index];r.client=client;r.server=server;
            r.original=callbacks->app_event;r.wrapper.app_event=wrappers[free_index];
            r.used=true;chosen=&r.wrapper;
        }
        // Capacity exhausted: pass the original table through unchanged.
        pthread_mutex_unlock(&registry_lock);
    }
    errno=incoming_errno;
    // 102 is VIM's existing generic API failure status (Open/AddClient map
    // IPC errors to it). Missing resolver is our failure, not an OEM call result.
    if(!forward)return 102;
    return forward(client,server,chosen);
}
