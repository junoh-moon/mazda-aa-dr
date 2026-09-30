#ifndef MX5_RUNTIME_SESSION_TRACE_H
#define MX5_RUNTIME_SESSION_TRACE_H
#include <stdint.h>

namespace mx5 { namespace runtime { namespace session_trace {
// Observation of create's return 0, not handle validity or a connected phone.
enum Result { UNOBSERVED = 0, OBSERVED, NONE, TRANSITION, AMBIGUOUS, FAULT };
struct Snapshot {
    Result result;
    uint32_t lifetime, event; // Process-local; zero is unknown, never reused.
    int32_t state;           // Raw first word of the actual status callback.
    bool state_known;
};
inline const char* result_name(Result r) {
    switch(r) {
    case UNOBSERVED:return "unobserved";
    case OBSERVED:return "observed";
    case NONE:return "no_live_session";
    case TRANSITION:return "transition";
    case AMBIGUOUS:return "ambiguous";
    case FAULT:return "observation_fault";
    }
    return "invalid_result";
}
} } }
#endif
