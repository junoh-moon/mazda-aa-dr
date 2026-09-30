#ifndef MX5_RUNTIME_SESSION_TRACE_H
#define MX5_RUNTIME_SESSION_TRACE_H
#include <stdint.h>

namespace mx5 { namespace runtime { namespace session_trace {
// Observation only. A live API handle is not a connected/accepted phone.
enum Result { UNOBSERVED = 0, OBSERVED, NONE, TRANSITION, AMBIGUOUS, FAULT };
struct Snapshot {
    Result result;
    uint32_t lifetime, event; // Process-local; zero is unknown, never reused.
    int32_t state;           // Raw first word of the actual status callback.
    bool state_known;
    // Coherent completed lifecycle revision, including failed calls and late
    // callbacks. Only OBSERVED/NONE/AMBIGUOUS snapshots carry it. Observation
    // at issue is still NOT proof that this session owns the request.
    uint64_t revision;
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
