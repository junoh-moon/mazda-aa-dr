// Authored ARM targets only: exception propagation and deferred cancellation
// must clean both position and send scopes without swallowing the unwind.
#include "adapter/adapter.h"
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
using namespace mx5::adapter;
#if defined(MX5_UNWIND_DSO_TEST)
#include "unwind_dso_access.h"
#else
extern "C" void* mx5_position_trampoline;
extern "C" void mx5_position_veneer(void*, const void*);
#endif
static unsigned behavior, depth, original_calls, events, cleanups;
static Observation last;
static unsigned char raw[72], payload[48];
static int owner;
enum { THROW_POSITION=1, THROW_SEND, NESTED_THROW, CANCEL_POSITION, CANCEL_SEND,
       THROW_ENTER, CANCEL_ENTER, NESTED_SEND };
static bool in_nested_send;
extern "C" void authored_target_entry();
static void* make_trampoline() {
    assert(sysconf(_SC_PAGESIZE)==4096);
    const uint32_t* entry=reinterpret_cast<const uint32_t*>(authored_target_entry);
    assert(entry[0]==0xe92d4810 && entry[1]==0xe28db008);
    uint32_t* code=static_cast<uint32_t*>(mmap(0,4096,PROT_READ|PROT_WRITE,
        MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    assert(code!=MAP_FAILED);
    code[0]=entry[0];code[1]=entry[1];
    code[2]=0xe51ff004;code[3]=reinterpret_cast<uint32_t>(entry+2);
    __builtin___clear_cache(reinterpret_cast<char*>(code),reinterpret_cast<char*>(code)+16);
    assert(!mprotect(code,4096,PROT_READ|PROT_EXEC));
    return code;
}
static void cancel_now() {
    assert(pthread_cancel(pthread_self())==0);
    pthread_testcancel();
    assert(false);
}
static int32_t original(void* manager, VehicleData* data) {
    assert(manager==&owner && data && (data->type==1 || data->type==3));
    ++original_calls;
    if (behavior==THROW_SEND) throw 27;
    if (behavior==CANCEL_SEND) cancel_now();
    if (behavior==NESTED_SEND && data->type==3) {
        if(in_nested_send)throw 31;
        in_nested_send=true;bool caught=false;
        try { send_vehicle_data(manager,data); }
        catch(int n) { assert(n==31);caught=true; }
        in_nested_send=false;assert(caught);
    }
    errno=EDOM; return -123;
}
static void observe(const Observation* e, void*) {
    ++events;last=*e;errno=EBUSY;
    if(e->kind==Observation::POSITION && behavior==THROW_ENTER)throw 73;
    if(e->kind==Observation::POSITION && behavior==CANCEL_ENTER)cancel_now();
}
static void outside() {
    behavior=0;
    VehicleData data={1,payload,48};
    const unsigned before=events, calls=original_calls;
    assert(send_vehicle_data(&owner,&data)==-123 && errno==EDOM);
    assert(original_calls==calls+1 && events==before+1);
    assert(last.kind==Observation::SEND && last.reason==NO_CONTEXT && !last.call_sequence);
}
extern "C" void authored_target(void* manager,const void* position) {
    assert(manager==&owner && position==raw && errno==EAGAIN);
    ++depth;
    if (behavior==THROW_POSITION || (behavior==NESTED_THROW && depth==2)) throw 19;
    if (behavior==CANCEL_POSITION) cancel_now();
    if (behavior==NESTED_THROW) {
        bool caught=false;
        try { mx5_position_veneer(manager,position); }
        catch(int n) { assert(n==19);caught=true;--depth; }
        assert(caught);
    }
    if (behavior==NESTED_SEND) {
        VehicleData other={3,payload,48};
        assert(send_vehicle_data(manager,&other)==-123);
    }
    VehicleData data={1,payload,48};
    assert(send_vehicle_data(manager,&data)==-123);
    assert(last.reason==PASS && last.choice==SCRUBBED);
    --depth;
}
static void cleanup(void*) { outside();++cleanups; }
static void* thread_main(void*) {
    pthread_cleanup_push(cleanup,0);
    errno=EAGAIN;
    mx5_position_veneer(&owner,raw);
    pthread_cleanup_pop(0);
    assert(false);return 0;
}
int main(int argc,char** argv) {
    assert(argc==2);
    alarm(10);
#if defined(MX5_UNWIND_DSO_TEST)
    initialize_test_dso();
#endif
    if(!strcmp(argv[1],"throw_position"))behavior=THROW_POSITION;
    else if(!strcmp(argv[1],"throw_send"))behavior=THROW_SEND;
    else if(!strcmp(argv[1],"nested_throw"))behavior=NESTED_THROW;
    else if(!strcmp(argv[1],"cancel_position"))behavior=CANCEL_POSITION;
    else if(!strcmp(argv[1],"cancel_send"))behavior=CANCEL_SEND;
    else if(!strcmp(argv[1],"throw_enter"))behavior=THROW_ENTER;
    else if(!strcmp(argv[1],"cancel_enter"))behavior=CANCEL_ENTER;
    else if(!strcmp(argv[1],"nested_send"))behavior=NESTED_SEND;
    else assert(false);
    Options options=Options();options.sink=observe;
    assert(configure(original,options) && set_mode(SCRUB_STALE));
    void* trampoline=make_trampoline();
    mx5_position_trampoline=trampoline;
    payload[32]=payload[40]=1;
    if(behavior==CANCEL_POSITION || behavior==CANCEL_SEND || behavior==CANCEL_ENTER) {
        pthread_t thread;void* result=0;
        assert(!pthread_create(&thread,0,thread_main,0));
        assert(!pthread_join(thread,&result));
        assert(result==PTHREAD_CANCELED && cleanups==1);
    } else {
        const unsigned requested=behavior;bool caught=false;
        errno=EAGAIN;
        try { mx5_position_veneer(&owner,raw); }
        catch(int n) { assert(n==(requested==THROW_SEND?27:requested==THROW_ENTER?73:19));caught=true; }
        assert(caught==(requested!=NESTED_THROW && requested!=NESTED_SEND));
        outside();
    }
    std::printf("PASS ARM unwind %s: propagation and empty post-unwind scopes\n",argv[1]);
    assert(!munmap(trampoline,4096));
}
