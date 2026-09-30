#ifndef MX5_RUNTIME_BUS_TRACE_H
#define MX5_RUNTIME_BUS_TRACE_H
#include <stdint.h>
namespace mx5 { namespace runtime { namespace bus_trace {
// Process-local observation of the actual JCIDBUS connection argument.
// This is neither a bus daemon GUID nor provider/receiver qualification.
enum Result { UNOBSERVED, CONNECTED, DISCONNECTED, TRANSITION, FAULT, NONE, AMBIGUOUS };
struct Snapshot { Result result; uint32_t object; uint64_t lifetime; };
// Unique live connection observed at LDS submission. Revision also fences a
// completed lifecycle between reads, even if the surviving connection agrees.
struct Boundary { Snapshot connection; uint64_t revision; };
inline const char* result_name(Result r) {
    switch(r) {
    case CONNECTED:return "connected";
    case DISCONNECTED:return "disconnected";
    case TRANSITION:return "transition";
    case FAULT:return "observation_fault";
    case NONE:return "no_live_connection";
    case AMBIGUOUS:return "ambiguous";
    default:return "unobserved";
    }
}
} } }
#endif
