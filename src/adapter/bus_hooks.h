#ifndef MX5_ADAPTER_BUS_HOOKS_H
#define MX5_ADAPTER_BUS_HOOKS_H
#include "runtime/bus_trace.h"
#include "runtime/request_trace.h"
namespace mx5 { namespace adapter {
typedef int32_t (*BusClosed)(void* connection,void* closure);
typedef void* (*BusCreate)(BusClosed,void*);
// The final argument is an opaque original callback word; never invoked here.
typedef int32_t (*BusConnect)(void*,const char*,int32_t,uintptr_t);
typedef void (*BusEnd)(void*);
typedef int32_t (*BusSignal)(void* connection,void* message);
typedef int32_t (*BusIsSignal)(void* message,const char* interface_name,const char* member);
struct BusEndpointApi {
    int32_t (*registration)(void* raw_connection,void* error);
    char* (*get_server_id)(void* raw_connection);
    const char* (*get_unique_name)(void* raw_connection);
    void (*free_guid)(void*);
    uintptr_t register_caller; // Verified return PC in original conn_by_env_name.
};
struct BusBindings {
    BusCreate create; BusConnect connect; BusEnd disconnect,free;
    BusSignal signal; BusIsSignal is_signal;
    BusEndpointApi endpoint;
};
enum { BUS_CONTEXT_CAPACITY=64 };
enum BusFault { BUS_CAPACITY=1, BUS_CONTENTION=2, BUS_CALLBACK=4, BUS_UNWIND=8,
                BUS_EXHAUSTED=16, BUS_COLLISION=32 };
struct BusHealth { bool prepared; unsigned contexts,faults; };
bool prepare_bus_hooks(const BusBindings&);
runtime::bus_trace::Snapshot read_bus_connection(const void*);
// The raw address is an opaque comparison key only. Never dereference it or
// place it in an owned Trace/journal. Unknown metadata leaves raw capture live.
runtime::bus_trace::Snapshot read_bus_endpoint(const void*,runtime::request_trace::Endpoint*,uintptr_t*);
enum EndpointMatch { ENDPOINT_UNAVAILABLE, ENDPOINT_MATCH, ENDPOINT_MISMATCH };
EndpointMatch bus_endpoint_matches(const void*,const runtime::bus_trace::Snapshot&,uintptr_t);
void observe_position_bus(const void*);
runtime::bus_trace::Boundary read_position_bus();
BusHealth bus_hook_health();
} }
extern "C" void* mx5_bus_create(mx5::adapter::BusClosed,void*);
extern "C" int32_t mx5_bus_connect(void*,const char*,int32_t,uintptr_t);
extern "C" void mx5_bus_disconnect(void*);
extern "C" void mx5_bus_free(void*);
extern "C" int32_t mx5_bus_signal(void*,void*);
extern "C" int32_t mx5_bus_register(void*,void*);
#endif
