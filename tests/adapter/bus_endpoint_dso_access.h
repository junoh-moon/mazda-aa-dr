// Hidden symbols of the unmodified product DSO. The fixture's JSON formatter
// is header code; actual production formatter execution has a separate test.
#include "request_wire_dso_access.h"
namespace mx5 { namespace runtime { namespace request_trace {
const char* result_name(Result result) {
    return reinterpret_cast<const char*(*)(Result)>(request_wire_dso_base+TEST_RESULT_NAME)(result);
}
} } }
namespace mx5 { namespace adapter {
static bool dso_endpoint_prepare(const BusBindings& b) {
    return reinterpret_cast<bool(*)(const BusBindings&)>(request_wire_dso_base+TEST_BUS_PREPARE)(b);
}
static BusHealth dso_endpoint_health() {
    return reinterpret_cast<BusHealth(*)()>(request_wire_dso_base+TEST_BUS_HEALTH)();
}
static B::Snapshot dso_endpoint_read(const void* p) {
    return reinterpret_cast<B::Snapshot(*)(const void*)>(request_wire_dso_base+TEST_BUS_READ)(p);
}
static B::Snapshot dso_endpoint_copy(const void* p,R::Endpoint* endpoint,uintptr_t* raw) {
    typedef B::Snapshot (*Function)(const void*,R::Endpoint*,uintptr_t*);
    return reinterpret_cast<Function>(request_wire_dso_base+TEST_ENDPOINT_READ)(p,endpoint,raw);
}
static EndpointMatch dso_endpoint_matches(const void* p,const B::Snapshot& issue,uintptr_t raw) {
    typedef EndpointMatch (*Function)(const void*,const B::Snapshot&,uintptr_t);
    return reinterpret_cast<Function>(request_wire_dso_base+TEST_ENDPOINT_MATCH)(p,issue,raw);
}
} }
#define prepare_bus_hooks dso_endpoint_prepare
#define bus_hook_health dso_endpoint_health
#define read_bus_connection dso_endpoint_read
#define read_bus_endpoint dso_endpoint_copy
#define bus_endpoint_matches dso_endpoint_matches
#define mx5_bus_create (reinterpret_cast<A::BusCreate>(request_wire_dso_base+TEST_BUS_CREATE))
#define mx5_bus_connect (reinterpret_cast<A::BusConnect>(request_wire_dso_base+TEST_BUS_CONNECT))
#define mx5_bus_disconnect (reinterpret_cast<A::BusEnd>(request_wire_dso_base+TEST_BUS_DISCONNECT))
#define mx5_bus_free (reinterpret_cast<A::BusEnd>(request_wire_dso_base+TEST_BUS_FREE))
#define mx5_bus_register (reinterpret_cast<int32_t(*)(void*,void*)>(request_wire_dso_base+TEST_BUS_REGISTER))
