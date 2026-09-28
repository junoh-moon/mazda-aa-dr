#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "loader.h"
#include <dlfcn.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#ifndef MX5_LOADER_TARGET
#define MX5_LOADER_TARGET "/jci/aapa/blmjciaapa.so"
#endif
namespace {
typedef void* (*Dlopen)(const char*, int);
Dlopen next_dlopen = 0;
enum Phase { FRESH, CANDIDATE, EXPOSED, PATCHING, FINISHED };
unsigned phase = FRESH;
__thread bool inside = false;
mx5::runtime::LoaderReport report = mx5::runtime::LoaderReport();
// Unlike a mutex held over dlopen, this cannot invert the dynamic loader lock.
// Another caller either cancels cold eligibility before the patch or waits only
// for the bounded patch span, whose owner performs no dynamic-loader calls.
void expose_or_wait() {
    for (;;) {
        unsigned p = __atomic_load_n(&phase, __ATOMIC_ACQUIRE);
        if (p == CANDIDATE) {
            unsigned expected = CANDIDATE;
            if (!__atomic_compare_exchange_n(&phase, &expected, EXPOSED, false,
                                             __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) continue;
        } else if (p == PATCHING) {
            const timespec pause = {0, 1000000};
            nanosleep(&pause, 0); continue;
        }
        return;
    }
}
void copy_error() {
    const char* error = dlerror();
    if (!error) error = "dlopen returned NULL without diagnostic";
    size_t n = 0;
    while (n + 1 < sizeof report.eager_error && error[n]) ++n;
    memcpy(report.eager_error, error, n); report.eager_error[n] = 0;
}
void fallback_diagnostic(bool loaded) {
    // One bounded-size, best-effort service stderr record. No dl* calls, heap
    // allocation, retries or disk-file creation. This does not claim durable
    // logging or a wall-time bound if the service's stderr sink blocks.
    char line[384];
    const char* prefix = loaded ? "mx5dr loader: eager_failed fallback=success error=" :
                                 "mx5dr loader: eager_failed fallback=failed error=";
    size_t n = strlen(prefix); memcpy(line, prefix, n);
    for (size_t i = 0; report.eager_error[i] && n + 1 < sizeof line; ++i) {
        unsigned char c = static_cast<unsigned char>(report.eager_error[i]);
        line[n++] = (c >= 32 && c < 127) ? char(c) : '?';
    }
    line[n++] = '\n';
    const ssize_t result = write(STDERR_FILENO, line, n);
    report.diagnostic_bytes = int(result);
    report.diagnostic_errno = result < 0 ? errno : 0;
}
}
namespace mx5 { namespace runtime {
const LoaderReport& loader_report() { return report; }
bool loader_begin_patch() {
    unsigned expected = CANDIDATE;
    return __atomic_compare_exchange_n(&phase, &expected, PATCHING, false,
                                      __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}
void loader_end_patch(bool executable_restored) {
    // A fatal RX restoration failure must exit the service, not release callers
    // into non-executable OEM memory. The bootstrap handles that fatal result.
    if (executable_restored) __atomic_store_n(&phase, FINISHED, __ATOMIC_RELEASE);
}
} }
extern "C" __attribute__((visibility("default")))
void* dlopen(const char* path, int flags) {
    const int incoming_errno = errno;
    Dlopen real = __atomic_load_n(&next_dlopen, __ATOMIC_ACQUIRE);
    if (!real) {
        real = reinterpret_cast<Dlopen>(dlsym(RTLD_NEXT, "dlopen"));
        if (real) __atomic_store_n(&next_dlopen, real, __ATOMIC_RELEASE);
    }
    if (!real) { errno = ENOSYS; return 0; }
    const bool target = path && !strcmp(path, MX5_LOADER_TARGET);
    if (inside || !target || (flags & RTLD_NOLOAD) ||
        !(flags & (RTLD_LAZY | RTLD_NOW))) {
        if (target) expose_or_wait();
        errno = incoming_errno;
        void* handle = real(path, flags);
        const int result_errno = errno;
        // A NOLOAD caller can pass the FRESH precheck, then overlap the first
        // ordinary load. Check again before a successful handle escapes.
        if (target && handle) expose_or_wait();
        errno = result_errno;
        return handle;
    }
    unsigned expected = FRESH;
    if (!__atomic_compare_exchange_n(&phase, &expected, CANDIDATE, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        expose_or_wait();
        errno = incoming_errno;
        return real(path, flags);
    }
    inside = true;
    // Exactly one startup decision, including OFF and failed attempts. A later
    // load/configuration edit cannot turn a previously exposed module into cold.
    using mx5::runtime::LoaderReport;
    report.outcome = LoaderReport::BYPASS;
    if (!mx5::runtime::loader_enabled()) {
        __atomic_store_n(&phase, FINISHED, __ATOMIC_RELEASE); inside = false;
        errno = incoming_errno;
        return real(path, flags);
    }
    // LAZY presence check only; never request NOW before proving a cold load.
    void* existing = real(path, RTLD_LAZY | RTLD_NOLOAD);
    if (existing) {
        dlclose(existing); // Complete internal operations before the OEM call.
        dlerror();
        report.outcome = LoaderReport::ALREADY_LOADED;
        __atomic_store_n(&phase, FINISHED, __ATOMIC_RELEASE); inside = false;
        errno = incoming_errno;
        return real(path, flags);
    }
    dlerror(); // The internal absence probe must not leak an error.
    const int eager_flags = (flags & ~RTLD_LAZY) | RTLD_NOW;
    errno = incoming_errno;
    void* handle = real(path, eager_flags);
    int result_errno = errno;
    if (!handle && eager_flags != flags) {
        copy_error(); // Preserve the first cause before the caller's retry.
        errno = incoming_errno;
        handle = real(path, flags);
        result_errno = errno;
        report.outcome = handle ? LoaderReport::EAGER_FAILED_FALLBACK_OK :
                                 LoaderReport::EAGER_FAILED_FALLBACK_FAILED;
        fallback_diagnostic(handle != 0);
        // Never bootstrap a fallback. Do not call dlerror/dlsym after a failed
        // retry: its real loader diagnostic belongs to the OEM caller.
    } else if (!handle) {
        report.outcome = LoaderReport::LOAD_FAILED;
    } else if (__atomic_load_n(&phase, __ATOMIC_ACQUIRE) == EXPOSED) {
        report.outcome = LoaderReport::EXPOSED;
    } else {
        report.outcome = LoaderReport::BOOTSTRAP_CALLED;
        mx5::runtime::loader_bootstrap(handle);
        if (__atomic_load_n(&phase, __ATOMIC_ACQUIRE) == EXPOSED)
            report.outcome = LoaderReport::EXPOSED;
        dlerror(); // Bootstrap symbol lookups are internal, load succeeded.
    }
    __atomic_store_n(&phase, FINISHED, __ATOMIC_RELEASE);
    inside = false;
    errno = result_errno;
    return handle;
}
