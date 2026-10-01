#include "adapter/bus_hooks.h"
#include "adapter/adapter.h"
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
static bool block_create,block_closed,block_signal;
static bool block_predicate;
static std::atomic<unsigned> predicate_gate(0);
static void wait_block();
static int32_t is_signal(void* message,const char* interface_name,const char* member) {
    assert(message==&signal_message && !strcmp(interface_name,"org.freedesktop.DBus.Local") &&
           !strcmp(member,"Disconnected"));
    if(block_predicate) { predicate_gate.store(1);while(predicate_gate.load()!=2)sched_yield(); }
    errno=EIO;return disconnected_signal;
}
static int32_t signal(void* p,void* message) {
    assert(p==address && message==&signal_message && errno==EDOM);
    if(block_signal)wait_block();
    ++signals;errno=ERANGE;if(throw_signal)throw 41;return -57;
}
static int32_t connect_result=7;
static bool fail_create,nested_free,early_close,throw_create,throw_connect,throw_disconnect,throw_free,throw_closed;
static bool cancellation;
static std::atomic<unsigned> blocked(0);
static int32_t closed(void* p,void* user) {
    assert(p==address && user==expected_user && errno==EDOM);
    ++closed_calls;if(block_closed)wait_block();errno=ERANGE;
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
    if(block_create)wait_block();
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
    uint64_t last=0,last_source=0;
    while(reading.load()) {
        const B::Snapshot s=read();
        if(s.result==B::CONNECTED) {
            assert(s.object==1 && s.lifetime && s.lifetime>=last);last=s.lifetime;
        } else if(s.result==B::DISCONNECTED)assert(s.object==1 && !s.lifetime);
        else assert(s.result==B::TRANSITION && !s.object && !s.lifetime);
        const B::Boundary source=A::read_position_bus();
        if(source.connection.result==B::CONNECTED) {
            assert(source.revision && source.connection.object==1 &&
                   source.connection.lifetime && source.connection.lifetime>=last_source);
            last_source=source.connection.lifetime;
        } else {
            assert(source.connection.result==B::UNOBSERVED || source.connection.result==B::NONE ||
                   source.connection.result==B::TRANSITION);
            assert(!source.connection.object && !source.connection.lifetime);
        }
        reads.fetch_add(1);
    }
    return 0;
}
static void concurrent_readers() {
    open();pthread_t threads[2];
    for(unsigned i=0;i<2;++i)assert(!pthread_create(&threads[i],0,reader,0));
    while(reads.load()<1000)sched_yield();
    for(unsigned i=0;i<200;++i) { attach();A::observe_position_bus(address);callback(0);end(false); }
    reading.store(false);
    for(unsigned i=0;i<2;++i)assert(!pthread_join(threads[i],0));
    assert(read().result==B::DISCONNECTED && !A::bus_hook_health().faults);
    assert(connects==200 && disconnects==200 && closed_calls==200);
}
// Authored qualification is injected ONLY into this fixture. The product's
// live ASSIST gate remains closed. Check actual selection, not just a counter.
static const uint64_t sample_time=1000000000;
static unsigned char input[72],payload[48],sent[48];
static int send_storage;
static A::Observation last_send;
static A::VehicleData* input_wrapper;
static unsigned sends;
static bool original_wrapper;
static uint64_t clock_fn(void*) { return sample_time; }
static bool provenance(void*,const A::PositionInput*,A::Provenance* out,void*) {
    out->source_epoch=123;out->session_epoch=456;
    out->exact_request=out->verified_lds=out->legacy_receiver=true;return true;
}
static void sink(const A::Observation* event,void*) { if(event->kind==A::Observation::SEND)last_send=*event; }
static int32_t next_send(void* storage,A::VehicleData* data) {
    assert(storage==&send_storage && data && data->payload && data->type==1 && data->length==48 && errno==EDOM);
    ++sends;original_wrapper=data==input_wrapper;memcpy(sent,data->payload,48);errno=EINPROGRESS;return -717;
}
static A::DrSnapshot snapshot;
static void* publish(void*) { assert(A::publish_snapshot(snapshot));return 0; }
static void publish_candidate() {
    snapshot=A::DrSnapshot();snapshot.source_epoch=123;snapshot.session_epoch=456;
    snapshot.prediction_generation=A::generation();snapshot.frontier_mono_ns=sample_time;
    snapshot.valid_until_mono_ns=sample_time+100000000;snapshot.derived_utc_ns=1700000000000000000ULL;
    snapshot.latitude_deg=37;snapshot.longitude_deg=127;snapshot.speed_mps=4;snapshot.travel_bearing_deg=45;
    snapshot.ready=snapshot.profile_verified=snapshot.input_quality_verified=snapshot.limits_ok=true;
    pthread_t worker;assert(!pthread_create(&worker,0,publish,0));assert(!pthread_join(worker,0));
}
static void send_candidate(bool replacement) {
    A::VehicleData data={1,payload,48};input_wrapper=&data;const unsigned before=sends;
    errno=EDOM;assert(A::send_vehicle_data(&send_storage,&data)==-717 && errno==EINPROGRESS && sends==before+1);
    assert(last_send.choice==(replacement?A::DR_REPLACEMENT:A::ORIGINAL));
    assert(original_wrapper==!replacement);
    if(!replacement)assert(!memcmp(sent,payload,48) && last_send.reason==A::EPOCH_MISMATCH);
    for(unsigned i=0;i<48;++i)assert(payload[i]==i+1);
}
static void* boundary_call(void* opaque) {
    const char* operation=static_cast<const char*>(opaque);
    if(!strcmp(operation,"create"))open();
    else if(!strcmp(operation,"connect"))attach();
    else if(!strcmp(operation,"disconnect"))end(false);
    else if(!strcmp(operation,"free"))end(true);
    else if(!strcmp(operation,"closed"))callback(0);
    else { assert(!strcmp(operation,"signal"));errno=EDOM;
           assert(mx5_bus_signal(address,&signal_message)==-57 && errno==ERANGE); }
    return 0;
}
static void prediction_boundary(const char* name) {
    const bool entry=!strncmp(name,"entry_",6);
    assert(entry || !strncmp(name,"exit_",5));const char* operation=name+(entry?6:5);
    A::Options options=A::Options();options.allow_assist=true;options.clock=clock_fn;
    options.provenance=provenance;options.sink=sink;options.max_snapshot_age_ns=150000000;
    assert(A::configure(next_send,options) && A::set_mode(A::ASSIST));
    for(unsigned i=0;i<48;++i)payload[i]=i+1;
    if(strcmp(operation,"create")) { open();attach(); }
    disconnected_signal=true;
    A::position_enter(0,input);publish_candidate();send_candidate(true);A::position_leave();
    if(entry)A::position_enter(0,input); // Preserve the pre-boundary POSITION generation.
    block_create=!strcmp(operation,"create");block_closed=!strcmp(operation,"closed");block_signal=!strcmp(operation,"signal");
    blocked.store(1);pthread_t thread;
    assert(!pthread_create(&thread,0,boundary_call,const_cast<char*>(operation)));
    while(blocked.load()!=2)sched_yield();
    assert(read().result==B::TRANSITION);
    if(entry) { send_candidate(false);A::position_leave(); }
    else { A::position_enter(0,input);publish_candidate();A::position_leave(); }
    blocked.store(3);assert(!pthread_join(thread,0));
    A::position_enter(0,input);send_candidate(false);A::position_leave();
    A::position_enter(0,input);publish_candidate();send_candidate(true);A::position_leave();
    assert(!A::bus_hook_health().faults);
}
static void signal_reused_address() {
    // Authored observer race: this is not evidence that concurrent OEM
    // free/dispatch is supported. A message classification cannot switch slots.
    open();attach();const B::Snapshot old=read();
    disconnected_signal=block_predicate=true;pthread_t thread;
    assert(!pthread_create(&thread,0,boundary_call,const_cast<char*>("signal")));
    while(predicate_gate.load()!=1)sched_yield();
    end(true);open();attach();const B::Snapshot before=read();
    assert(before.result==B::CONNECTED && before.object!=old.object);
    predicate_gate.store(2);assert(!pthread_join(thread,0));
    const B::Snapshot after=read();
    assert(after.result==B::CONNECTED && after.object==before.object && after.lifetime==before.lifetime);
    assert(creates==2 && connects==2 && frees==1 && signals==1 && !A::bus_hook_health().faults);
}
// No lifecycle is concurrent here: two already ACTIVE observed connections
// each submit their first source mark for the current connect lifetime.
static std::atomic<unsigned> source_round(0),source_done(0);
static std::atomic<bool> source_stop(false);
static void* mark_source(void* connection) {
    for(unsigned round=1;;++round) {
        while(source_round.load()<round)sched_yield();
        if(source_stop.load())return 0;
        errno=E2BIG;A::observe_position_bus(connection);assert(errno==E2BIG);
        source_done.fetch_add(1);
    }
}
static void position_sources_concurrent() {
    const unsigned rounds=20000;
    void* const first=address;open();attach();
    void* const second=reinterpret_cast<void*>(0x2230);address=second;open();
    // A different connection's blocked original connect cannot erase the
    // stable first connection's source mark. This catches the global reader
    // guard deterministically before the two-marker scheduling stress below.
    blocked.store(1);pthread_t connecting_thread;
    assert(!pthread_create(&connecting_thread,0,connecting,0));
    while(blocked.load()!=2)sched_yield();
    errno=E2BIG;A::observe_position_bus(first);assert(errno==E2BIG);
    assert(A::read_position_bus().connection.result==B::TRANSITION);
    blocked.store(3);assert(!pthread_join(connecting_thread,0));blocked.store(0);
    const B::Boundary only=A::read_position_bus();
    assert(only.connection.result==B::CONNECTED && only.connection.object==1 && only.connection.lifetime==1);
    pthread_t markers[2];
    assert(!pthread_create(&markers[0],0,mark_source,first));
    assert(!pthread_create(&markers[1],0,mark_source,second));
    for(unsigned round=1;round<=rounds;++round) {
        address=first;end(false);attach();const B::Snapshot one=read();
        address=second;end(false);attach();const B::Snapshot two=read();
        assert(one.result==B::CONNECTED && two.result==B::CONNECTED && one.object!=two.object);
        source_done.store(0);source_round.store(round);
        while(source_done.load()!=2)sched_yield();
        const B::Boundary both=A::read_position_bus();
        assert(both.connection.result==B::AMBIGUOUS && !both.connection.object && !both.connection.lifetime);
        assert(!A::bus_hook_health().faults);
    }
    source_stop.store(true);source_round.store(rounds+1);
    assert(!pthread_join(markers[0],0));assert(!pthread_join(markers[1],0));
    assert(creates==2 && connects==2*rounds+2 && disconnects==2*rounds);
}
static void position_source() {
    assert(A::read_position_bus().connection.result==B::UNOBSERVED);
    open();attach();assert(A::read_position_bus().connection.result==B::UNOBSERVED);
    errno=E2BIG;A::observe_position_bus(address);assert(errno==E2BIG);
    const B::Boundary first=A::read_position_bus();
    assert(first.connection.result==B::CONNECTED && first.connection.object==1 && first.connection.lifetime==1);
    assert(first.revision==3); // create, connect, first observed LDS submission
    A::observe_position_bus(address);assert(A::read_position_bus().revision==first.revision);
    callback(0);assert(A::read_position_bus().connection.result==B::NONE);
    attach();assert(A::read_position_bus().connection.result==B::NONE); // new lifetime needs submission
    A::observe_position_bus(address);
    const B::Boundary second=A::read_position_bus();
    assert(second.connection.object==1 && second.connection.lifetime==2 && second.revision>first.revision);
    // An unmarked HMI/other bus is not mistaken for the LDS source.
    void* primary=address;address=reinterpret_cast<void*>(0x2230);open();attach();
    assert(A::read_position_bus().connection.object==1);
    A::observe_position_bus(address);assert(A::read_position_bus().connection.result==B::AMBIGUOUS);
    end(true);assert(A::read_position_bus().connection.object==1);
    address=primary;end(true);assert(A::read_position_bus().connection.result==B::NONE);
    open();attach();assert(A::read_position_bus().connection.result==B::NONE);
    A::observe_position_bus(address);const B::Boundary reused=A::read_position_bus();
    assert(reused.connection.object==3 && reused.connection.lifetime==4);
    callback(0);assert(A::read_position_bus().connection.object==3);
    assert(!A::bus_hook_health().faults);
}
int main(int argc,char** argv) {
    assert(argc==2);alarm(20);
#ifdef MX5_BUS_DSO_TEST
    initialize_bus_test_dso();
#endif
#if defined(__APPLE__)
    if(!strcmp(argv[1],"cancel")) { puts("SKIP Darwin forced unwind");return 77; }
#endif
    const A::BusBindings b={create,connect,disconnect,free_connection,signal,is_signal,A::BusEndpointApi()};assert(A::prepare_bus_hooks(b));
    const char* c=argv[1];
    if(!strncmp(c,"prediction_",11))prediction_boundary(c+11);
    else if(!strcmp(c,"normal"))normal();
    else if(!strcmp(c,"position_source"))position_source();
    else if(!strcmp(c,"position_sources_concurrent"))position_sources_concurrent();
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
    else if(!strcmp(c,"signal_reuse"))signal_reused_address();
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
