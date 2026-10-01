// Authored-only integration: include request wrapper to access its authored
// Observer for queue dispatch; no OEM object, binary or original API is used.
#include "../../src/adapter/request_hooks.cpp"
#include "adapter/adapter.h"
#include "adapter/bus_hooks.h"
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
namespace S=mx5::runtime::session_trace;
static A::SessionCallbacks callbacks[2];
static int handles[2], session_creates;
static int method, worker, connection, notify_context, reply_value;
alignas(8) static unsigned char position[72];
static A::RequestNotify queued_notify;
static A::Observation position_event, send_event;
static unsigned positions, sends;
static int32_t bus_closed(void*,void*) { return 0; }
static void* bus_create(A::BusClosed,void*) { return &connection; }
static int32_t bus_connect(void*,const char*,int32_t,uintptr_t) { return 1; }
static void bus_end(void*) {}
static int32_t bus_signal(void*,void*) { return 0; }
static int32_t bus_is_signal(void*,const char*,const char*) { return 0; }
static int32_t session_create(const char*,void*,const A::SessionCallbacks* cb,void** out) {
    assert(session_creates<2);callbacks[session_creates]=*cb;
    *out=&handles[session_creates++];return 0;
}
static int32_t session_destroy(void** out) { *out=0;return 0; }
static void session_status(void*,void*) {}
static void status_for(unsigned i,int32_t state) {
    int32_t info[2]={state,0};
    reinterpret_cast<A::SessionStatus>(callbacks[i].entry[1])(0,info);
}
static void* get_reply(void*) { return &reply_value; }
static int get_type(void*) { return 2; }
static const char* get_sender(void*) { return ":1.7"; }
static const char* get_error(void*) { return 0; }
static const char* get_destination(void*) { return "com.jci.lds.data"; }
static const char* get_path(void*) { return "/com/jci/lds/data"; }
static const char* get_interface(void*) { return "org.example.RouteInterface"; }
static const char* get_member(void*) { return "GetPosition"; }
static int get_serial(void*,uint32_t* out) { *out=7;return 0; }
static void original_notify(void*,void*,void*) {
    R::Token t;assert(observer->worker_post(&worker,position,&t)==R::OK);
}
static int32_t original_submit(void*,void*,A::RequestNotify callback,void*,int) {
    queued_notify=callback;return 42;
}
static int32_t original_free(void*) { return 13; }
static void trampoline() {}
static uint64_t clock_value(void*) { return 100; }
static void sink_copy(const A::Observation* e,void*) {
    if(e->kind==A::Observation::POSITION) { position_event=*e;++positions; }
    else { send_event=*e;++sends; }
}
static int32_t original_send(void*,A::VehicleData*) { return 29; }
int main(int argc,char** argv) {
    assert(argc==1 || (argc==2 && !strcmp(argv[1],"bus_recreated")));
    alarm(15);
    const A::BusBindings bb={bus_create,bus_connect,bus_end,bus_end,bus_signal,bus_is_signal,{}};
    assert(A::prepare_bus_hooks(bb));
    assert(mx5_bus_create(bus_closed,0)==&connection);
    assert(mx5_bus_connect(&connection,"authored",0,0)==1);
    A::SessionBindings sb={session_create,session_destroy,session_status};
    assert(A::prepare_session_hooks(sb));
    A::SessionCallbacks cb=A::SessionCallbacks();cb.entry[1]=reinterpret_cast<uintptr_t>(&session_status);
    void* storage=0;
    assert(mx5_session_create("authored",0,&cb,&storage)==0);status_for(0,-7);
    A::RequestBindings rb=A::RequestBindings();
    rb.reply={get_reply,get_type,get_sender,get_error,get_serial};rb.submit=original_submit;
    rb.method={get_destination,get_path,get_interface,get_member};
    rb.notify=original_notify;rb.free_method=rb.free_method_only=original_free;rb.position_vptr=1;
    rb.post_trampoline=rb.work_trampoline=rb.destroy_trampoline=reinterpret_cast<void*>(&trampoline);
    assert(A::prepare_request_hooks(rb,clock_value,0));
    A::Options options=A::Options();options.sink=sink_copy;options.clock=clock_value;
    options.request_reader=A::read_request_trace;options.session_reader=A::read_send_session;
    assert(A::configure(original_send,options));assert(A::set_mode(A::OBSERVE));
    assert(mx5_request_submit(&connection,&method,original_notify,&notify_context,-1)==42);
    assert(A::read_position_bus().connection.result==mx5::runtime::bus_trace::CONNECTED);
    assert(A::read_position_bus().connection.object==1 && A::read_position_bus().connection.lifetime==1);
    status_for(0,99);
    assert(mx5_session_destroy(&storage)==0);
    assert(mx5_session_create("authored",0,&cb,&storage)==0);status_for(1,3);
    if(argc==2) {
        mx5_bus_free(&connection);
        assert(mx5_bus_create(bus_closed,0)==&connection);
        assert(mx5_bus_connect(&connection,"authored",0,0)==1);
    }
    queued_notify(&connection,&method,&notify_context);
    if(argc==2)assert(A::read_position_bus().connection.result==mx5::runtime::bus_trace::NONE);
    assert(mx5_request_free(&method)==13);
    {
        R::WorkerScope scope(*observer,&worker);
        A::position_enter(0,position);
        unsigned char payload[48]={};A::VehicleData data={1,payload,sizeof payload};
        assert(A::send_vehicle_data(&storage,&data)==29);
        A::position_leave();
    }
    assert(positions==1 && sends==1);
    assert(position_event.request_result==R::OK && send_event.request_result==R::OK);
    const S::Snapshot& old=position_event.request_trace.issue.session_context;
    const S::Snapshot& carried=send_event.request_trace.issue.session_context;
    const S::Snapshot& actual=send_event.send_session;
    assert(old.result==S::OBSERVED && old.lifetime==1 && old.event==1 && old.state==-7 && old.state_known);
    assert(carried.result==old.result && carried.lifetime==old.lifetime && carried.event==old.event && carried.state==old.state && carried.state_known);
    assert(actual.result==S::OBSERVED && actual.lifetime==2 && actual.event==1 && actual.state==3 && actual.state_known);
    assert(old.revision==2 && carried.revision==old.revision && actual.revision==6);
    const R::Route& route=send_event.request_trace.issue.route;
    assert(route.destination.known && route.destination.complete &&
           !strcmp(route.destination.bytes,"com.jci.lds.data"));
    assert(route.path.known && route.path.complete &&
           !strcmp(route.path.bytes,"/com/jci/lds/data"));
    assert(route.interface_name.known && route.interface_name.complete &&
           !strcmp(route.interface_name.bytes,"org.example.RouteInterface"));
    assert(route.member.known && route.member.complete && !strcmp(route.member.bytes,"GetPosition"));
    assert(!memcmp(&route,&position_event.request_trace.issue.route,sizeof route));
    assert(send_event.request_trace.issue.known==R::ISSUE_BUS_LIFETIME);
    assert(send_event.request_trace.issue.bus_lifetime==1 && !send_event.request_trace.issue.session_lifetime);
    namespace B=mx5::runtime::bus_trace;
    const B::Snapshot& issue=send_event.request_trace.issue.connection;
    const B::Snapshot& reply=send_event.request_trace.reply.connection;
    assert(issue.result==B::CONNECTED && issue.object==1 && issue.lifetime==1);
    assert(reply.result==B::CONNECTED && reply.object==unsigned(argc) && reply.lifetime==unsigned(argc));
    assert(position_event.request_trace.reply.connection.lifetime==reply.lifetime);
    assert(!A::bus_hook_health().faults);
    assert(!send_event.provenance.exact_request);
    assert(!A::request_hook_health().ledger.requests && !A::request_hook_health().ledger.workers);
    assert(!A::session_hook_health().faults);
    puts("PASS actual submit hook retains issue lifetime 1/event 1/state -7 across status change, destroy/recreate, reply, owned worker, free and send to lifetime 2/event 1/state 3");
}
