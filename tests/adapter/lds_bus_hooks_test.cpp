#include "adapter/bus_hooks.h"
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>

namespace A=mx5::adapter;
namespace B=mx5::runtime::bus_trace;

// Link this fixture without adapter.cpp: LDS must not need AA prediction TLS.
static int connection_storage,message_storage;
static void* const connection=&connection_storage;
static A::BusClosed on_close;
static void* close_user;
static unsigned creates,connects,disconnects,frees,signals,closed;
static bool disconnected_signal;
static bool throw_signal;

static int32_t original_closed(void* value,void* user) {
    assert(value==connection && user==&close_user && errno==EDOM);
    ++closed;errno=EINPROGRESS;return -73;
}
static void* original_create(A::BusClosed callback,void* user) {
    assert(callback && user==&close_user && errno==EDOM);
    on_close=callback;close_user=user;++creates;errno=ERANGE;return connection;
}
static int32_t original_connect(void* value,const char* name,int32_t type,uintptr_t callback) {
    assert(value==connection && name && name[0]=='L' && !name[1] && type==7 && callback==0x1234 && errno==EDOM);
    ++connects;errno=ERANGE;return 9;
}
static void original_disconnect(void* value) {
    assert(value==connection && errno==EDOM);++disconnects;errno=ERANGE;
}
static void original_free(void* value) {
    assert(value==connection && errno==EDOM);++frees;errno=ERANGE;
}
static int32_t original_signal(void* value,void* message) {
    assert(value==connection && message==&message_storage && errno==EDOM);
    ++signals;errno=ERANGE;if(throw_signal)throw 41;return -57;
}
static int32_t original_is_signal(void* message,const char* interface_name,const char* member) {
    assert(message==&message_storage && interface_name && member &&
           !std::strcmp(interface_name,"org.freedesktop.DBus.Local") &&
           !std::strcmp(member,"Disconnected"));
    errno=EIO;return disconnected_signal;
}
static void create() {
    errno=EDOM;assert(mx5_bus_create(original_closed,&close_user)==connection && errno==ERANGE);
}
static void connect() {
    errno=EDOM;assert(mx5_bus_connect(connection,"L",7,0x1234)==9 && errno==ERANGE);
}
static void expect_position(B::Result expected) {
    errno=E2BIG;const B::Boundary result=A::read_position_bus();
    assert(errno==E2BIG && result.connection.result==expected);
}
int main() {
    const A::BusBindings b={original_create,original_connect,original_disconnect,
                            original_free,original_signal,original_is_signal,A::BusEndpointApi()};
    assert(A::prepare_bus_hooks(b,0));
    expect_position(B::UNOBSERVED);
    create();connect();expect_position(B::UNOBSERVED);
    errno=E2BIG;A::observe_position_bus(connection);assert(errno==E2BIG);
    expect_position(B::CONNECTED);
    const B::Snapshot first=A::read_bus_connection(connection);
    assert(first.result==B::CONNECTED && first.object && first.lifetime);

    errno=EDOM;assert(mx5_bus_signal(connection,&message_storage)==-57 && errno==ERANGE);
    expect_position(B::CONNECTED);
    disconnected_signal=true;
    errno=EDOM;assert(mx5_bus_signal(connection,&message_storage)==-57 && errno==ERANGE);
    expect_position(B::NONE);
    connect();expect_position(B::NONE); // A new lifetime requires a new source mark.
    A::observe_position_bus(connection);expect_position(B::CONNECTED);
    errno=EDOM;assert(on_close(connection,close_user)==-73 && errno==EINPROGRESS);
    expect_position(B::NONE);
    errno=EDOM;mx5_bus_disconnect(connection);assert(errno==ERANGE);
    errno=EDOM;mx5_bus_free(connection);assert(errno==ERANGE);
    expect_position(B::NONE);
    assert(creates==1 && connects==2 && signals==2 && closed==1 &&
           disconnects==1 && frees==1 && !A::bus_hook_health().faults);
    // A foreign exception must keep its original errno and mark the local
    // observation boundary faulty without changing OEM forwarding.
    throw_signal=true;errno=EDOM;
    try { mx5_bus_signal(connection,&message_storage);assert(false); }
    catch(int code) { assert(code==41 && errno==ERANGE); }
    assert(A::bus_hook_health().faults&A::BUS_UNWIND);
    std::puts("PASS LDS bus hooks without AA adapter: OEM forwarding and source lifetime");
}
