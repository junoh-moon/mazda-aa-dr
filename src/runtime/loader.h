#ifndef MX5_RUNTIME_LOADER_H
#define MX5_RUNTIME_LOADER_H
namespace mx5 { namespace runtime {
// Called only for the first target load, before any extra loader operation.
// Production implementation reads startup configuration and the disable marker.
bool loader_enabled();
void loader_bootstrap(void* handle);
// Called after all symbol/segment/hash checks, around only the memory patch.
// No dlopen/dlsym/dl_iterate_phdr calls are allowed while this lease is held.
bool loader_begin_patch();
void loader_end_patch(bool executable_restored);
// Fixed-size process diagnostics. A caller must read only after target dlopen
// returns; this is not a concurrent telemetry API and performs no loader calls.
struct LoaderReport {
    enum Outcome { UNSEEN, BYPASS, ALREADY_LOADED, BOOTSTRAP_CALLED,
                   EAGER_FAILED_FALLBACK_OK, EAGER_FAILED_FALLBACK_FAILED,
                   LOAD_FAILED, EXPOSED } outcome;
    char eager_error[256];
    int diagnostic_bytes, diagnostic_errno;
};
const LoaderReport& loader_report();
} }
#endif
