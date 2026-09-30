#ifndef MX5_ADAPTER_BUS_HOOKS_H
#define MX5_ADAPTER_BUS_HOOKS_H
#include "runtime/bus_trace.h"
namespace mx5 { namespace adapter {
typedef int32_t (*BusClosed)(void* connection,void* closure);
typedef void* (*BusCreate)(BusClosed,void*);
// The final argument is an opaque original callback word; never invoked here.
typedef int32_t (*BusConnect)(void*,const char*,int32_t,uintptr_t);
typedef void (*BusEnd)(void*);
typedef int32_t (*BusSignal)(void* connection,void* message);
typedef int32_t (*BusIsSignal)(void* message,const char* interface_name,const char* member);
struct BusBindings {
    BusCreate create; BusConnect connect; BusEnd disconnect,free;
    BusSignal signal; BusIsSignal is_signal;
};
enum { BUS_CONTEXT_CAPACITY=64 };
enum BusFault { BUS_CAPACITY=1, BUS_CONTENTION=2, BUS_CALLBACK=4, BUS_UNWIND=8,
                BUS_EXHAUSTED=16, BUS_COLLISION=32 };
struct BusHealth { bool prepared; unsigned contexts,faults; };
bool prepare_bus_hooks(const BusBindings&);
runtime::bus_trace::Snapshot read_bus_connection(const void*);
BusHealth bus_hook_health();
} }
extern "C" void* mx5_bus_create(mx5::adapter::BusClosed,void*);
extern "C" int32_t mx5_bus_connect(void*,const char*,int32_t,uintptr_t);
extern "C" void mx5_bus_disconnect(void*);
extern "C" void mx5_bus_free(void*);
extern "C" int32_t mx5_bus_signal(void*,void*);
#endif
