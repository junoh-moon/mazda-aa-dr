#include "adapter/bus_hooks.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <atomic>
#include <sched.h>
namespace A=mx5::adapter;
namespace B=mx5::runtime::bus_trace;
#ifdef MX5_BUS_DSO_TEST
#include "bus_dso_access.h"
#endif
static A::BusClosed saved[80];
static void* closures[80];
static void* address=reinterpret_cast<void*>(0x1230);
static void* expected_user=reinterpret_cast<void*>(9);
static unsigned creates,connects,disconnects,frees,closed_calls;
static unsigned signals;
static bool disconnected_signal;
static bool throw_signal;
static int signal_message;
static int32_t is_signal(void* message,const char* interface_name,const char* member) {
    assert(message==&signal_message && !strcmp(interface_name,"org.freedesktop.DBus.Local") &&
           !strcmp(member,"Disconnected"));errno=EIO;return disconnected_signal;
}
static int32_t signal(void* p,void* message) {
    assert(p==address && message==&signal_message && errno==EDOM);
    ++signals;errno=ERANGE;if(throw_signal)throw 41;return -57;
}
static int32_t connect_result=7;
static bool fail_create,nested_free,early_close,throw_create,throw_connect,throw_disconnect,throw_free,throw_closed;
static bool cancellation;
static std::atomic<unsigned> blocked(0);
static int32_t closed(void* p,void* user) {
    assert(p==address && user==expected_user && errno==EDOM);
    ++closed_calls;errno=ERANGE;
    if(throw_closed)throw 41;
    return -73;
}
static void wait_block() {
    if(!blocked.load())return;
    blocked.store(2);
    while(blocked.load()!=3) { if(cancellation)pthread_testcancel();sched_yield(); }
}
static void* create(A::BusClosed callback,void* user) {
    assert(errno==EDOM);saved[creates]=callback;closures[creates++]=user;
    if(throw_create) { errno=ERANGE;throw 41; }
    errno=ERANGE;return fail_create||!callback?0:address;
}
static int32_t connect(void* p,const char* name,int32_t type,uintptr_t callback) {
    assert(p==address && !strcmp(name,"original") && type==-7 && callback==0x7654 && errno==EDOM);
    ++connects;wait_block();
    if(early_close) { errno=EDOM;assert(saved[creates-1](p,closures[creates-1])==-73); }
    errno=ERANGE;if(throw_connect)throw 41;return connect_result;
}
static void disconnect(void* p) {
    assert(p==address && errno==EDOM);++disconnects;wait_block();
    errno=ERANGE;if(throw_disconnect)throw 41;
}
static void free_connection(void* p) {
    assert(p==address && errno==EDOM);++frees;
    if(nested_free) { mx5_bus_disconnect(p);assert(errno==ERANGE); }
    wait_block();errno=ERANGE;if(throw_free)throw 41;
}
static void open() {
    errno=EDOM;assert(mx5_bus_create(closed,reinterpret_cast<void*>(9))==address);assert(errno==ERANGE);
}
static void attach() {
    errno=EDOM;assert(mx5_bus_connect(address,"original",-7,0x7654)==connect_result);assert(errno==ERANGE);
}
static void end(bool freeing) {
    errno=EDOM;if(freeing)mx5_bus_free(address);else mx5_bus_disconnect(address);assert(errno==ERANGE);
}
static B::Snapshot read() { errno=E2BIG;B::Snapshot s=A::read_bus_connection(address);assert(errno==E2BIG);return s; }
static void callback(unsigned index) {
    errno=EDOM;assert(saved[index](address,closures[index])==-73);assert(errno==ERANGE);
}
static void normal() {
    assert(read().result==B::UNOBSERVED);open();assert(read().result==B::DISCONNECTED);
    attach();B::Snapshot one=read();assert(one.result==B::CONNECTED && one.object && one.lifetime);
    callback(0);assert(read().result==B::DISCONNECTED);attach();
    B::Snapshot two=read();assert(two.object==one.object && two.lifetime>one.lifetime);
    end(false);assert(read().result==B::DISCONNECTED);attach();
    nested_free=true;end(true);assert(read().result==B::UNOBSERVED);
    open();attach();B::Snapshot three=read();
    assert(three.object!=two.object && three.lifetime>two.lifetime);
    callback(0);B::Snapshot after=read(); // Old callback must not close a reused address.
    assert(after.result==B::CONNECTED && after.object==three.object && after.lifetime==three.lifetime);
    assert(saved[0]!=saved[1] && creates==2 && connects==4 && disconnects==2 && frees==1 && closed_calls==2);
    assert(!A::bus_hook_health().faults);
}
static void failures() {
    errno=EDOM;assert(!mx5_bus_create(0,0));assert(saved[0]==0 && errno==ERANGE);
    fail_create=true;errno=EDOM;assert(!mx5_bus_create(closed,reinterpret_cast<void*>(9)));assert(errno==ERANGE);
    assert(read().result==B::UNOBSERVED);fail_create=false;open();
    connect_result=0;attach();assert(read().result==B::DISCONNECTED);
    connect_result=7;attach();const B::Snapshot old=read();assert(old.result==B::CONNECTED);
    connect_result=0;attach();assert(read().result==B::DISCONNECTED);end(true);
    assert(!A::bus_hook_health().faults);
}
static void* connecting(void*) { attach();return 0; }
static void overlap() {
    open();blocked.store(1);pthread_t t;assert(!pthread_create(&t,0,connecting,0));
    while(blocked.load()!=2)sched_yield();
    assert(read().result==B::TRANSITION);
    // Competing lifecycle must not be hidden by the original's return order.
    fail_create=true;errno=EDOM;assert(!mx5_bus_create(closed,reinterpret_cast<void*>(9)));
    blocked.store(3);assert(!pthread_join(t,0));
    assert(read().result==B::FAULT && (A::bus_hook_health().faults&A::BUS_CONTENTION));
}
static void cancel() {
    open();cancellation=true;blocked.store(1);pthread_t t;assert(!pthread_create(&t,0,connecting,0));
    while(blocked.load()!=2)sched_yield();
    assert(!pthread_cancel(t));void* out=0;
    assert(!pthread_join(t,&out) && out==PTHREAD_CANCELED);
    assert(read().result==B::FAULT && (A::bus_hook_health().faults&A::BUS_UNWIND));
}
static std::atomic<unsigned> reads(0);
static std::atomic<bool> reading(true);
static void* reader(void*) {
    uint64_t last=0;
    while(reading.load()) {
        const B::Snapshot s=read();
        if(s.result==B::CONNECTED) {
            assert(s.object==1 && s.lifetime && s.lifetime>=last);last=s.lifetime;
        } else if(s.result==B::DISCONNECTED)assert(s.object==1 && !s.lifetime);
        else assert(s.result==B::TRANSITION && !s.object && !s.lifetime);
        reads.fetch_add(1);
    }
    return 0;
}
static void concurrent_readers() {
    open();pthread_t threads[2];
    for(unsigned i=0;i<2;++i)assert(!pthread_create(&threads[i],0,reader,0));
    while(reads.load()<1000)sched_yield();
    for(unsigned i=0;i<200;++i) { attach();callback(0);end(false); }
    reading.store(false);
    for(unsigned i=0;i<2;++i)assert(!pthread_join(threads[i],0));
    assert(read().result==B::DISCONNECTED && !A::bus_hook_health().faults);
    assert(connects==200 && disconnects==200 && closed_calls==200);
}
int main(int argc,char** argv) {
    assert(argc==2);alarm(20);
#ifdef MX5_BUS_DSO_TEST
    initialize_bus_test_dso();
#endif
#if defined(__APPLE__)
    if(!strcmp(argv[1],"cancel")) { puts("SKIP Darwin forced unwind");return 77; }
#endif
    const A::BusBindings b={create,connect,disconnect,free_connection,signal,is_signal};assert(A::prepare_bus_hooks(b));
    const char* c=argv[1];
    if(!strcmp(c,"normal"))normal();
    else if(!strcmp(c,"signal")) {
        open();attach();const B::Snapshot before=read();
        errno=EDOM;assert(mx5_bus_signal(address,&signal_message)==-57 && errno==ERANGE);
        assert(read().result==B::CONNECTED && read().lifetime==before.lifetime);
        disconnected_signal=true;
        errno=EDOM;assert(mx5_bus_signal(address,&signal_message)==-57 && errno==ERANGE);
        assert(read().result==B::DISCONNECTED && read().object==before.object);
        assert(signals==2 && !closed_calls && !A::bus_hook_health().faults);
        attach();throw_signal=true;
        try { errno=EDOM;mx5_bus_signal(address,&signal_message);assert(false); }
        catch(int n) { assert(n==41 && errno==ERANGE); }
        assert(signals==3 && read().result==B::FAULT && (A::bus_hook_health().faults&A::BUS_UNWIND));
    }
    else if(!strcmp(c,"failure"))failures();
    else if(!strcmp(c,"early_close")) { open();early_close=true;attach();assert(read().result==B::DISCONNECTED && !A::bus_hook_health().faults); }
    else if(!strcmp(c,"unobserved")) { attach();assert(read().result==B::UNOBSERVED);end(false);end(true);assert(!A::bus_hook_health().faults); }
    else if(!strcmp(c,"overlap"))overlap();
    else if(!strcmp(c,"cancel"))cancel();
    else if(!strcmp(c,"readers"))concurrent_readers();
    else if(!strcmp(c,"capacity")) {
        for(unsigned i=0;i<A::BUS_CONTEXT_CAPACITY+1;++i) { open();end(true); }
        assert(read().result==B::FAULT && (A::bus_hook_health().faults&A::BUS_CAPACITY));
        assert(saved[A::BUS_CONTEXT_CAPACITY]==closed);
    } else if(!strcmp(c,"collision")) { open();open();assert(read().result==B::FAULT && (A::bus_hook_health().faults&A::BUS_COLLISION)); }
    else if(!strcmp(c,"bad_callback")) { open();attach();expected_user=0;errno=EDOM;assert(saved[0](address,0)==-73);assert(read().result==B::FAULT); }
    else if(!strncmp(c,"throw_",6)) {
        if(strcmp(c,"throw_create")) { open();attach(); }
        try {
            if(!strcmp(c,"throw_create")) { throw_create=true;open(); }
            else if(!strcmp(c,"throw_connect")) { throw_connect=true;attach(); }
            else if(!strcmp(c,"throw_disconnect")) { throw_disconnect=true;end(false); }
            else if(!strcmp(c,"throw_free")) { throw_free=true;end(true); }
            else { assert(!strcmp(c,"throw_closed"));throw_closed=true;callback(0); }
            assert(false);
        } catch(int n) { assert(n==41 && errno==ERANGE); }
        assert(read().result==B::FAULT && (A::bus_hook_health().faults&A::BUS_UNWIND));
    } else assert(false);
    printf("PASS bus connection %s: original forwarding and lifetime boundary\n",c);
}
