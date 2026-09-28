#include "runtime/loader.h"
#include "runtime/config.h"
#include <cstdlib>
#include <dlfcn.h>
#include <errno.h>
#include <unistd.h>
static unsigned bootstraps, policies, preparing, prepare_release, patch_active, patch_finished;
namespace mx5 { namespace runtime {
bool loader_enabled() {
    __sync_fetch_and_add(&policies, 1);
    // A short delay lets the concurrent test put several callers in flight.
    if (getenv("TEST_SLOW_POLICY")) usleep(50000);
    Config config;
    return startup_enabled(getenv("TEST_CONFIG"), getenv("TEST_DISABLE"), &config);
}
void loader_bootstrap(void*) {
    if (getenv("TEST_PREPARE_RACE")) {
        __sync_lock_test_and_set(&preparing, 1);
        while (!__sync_fetch_and_add(&prepare_release, 0)) usleep(1000);
    }
    if (!loader_begin_patch()) return;
    if (getenv("TEST_PATCH_RACE")) {
        __sync_lock_test_and_set(&patch_active, 1);
        usleep(100000); // Synthetic patch duration; no dynamic-loader calls.
    }
    __sync_fetch_and_add(&bootstraps, 1);
    __sync_lock_test_and_set(&patch_finished, 1);
    loader_end_patch(true);
    // Deliberate internal failure must not replace a successful load's state.
    dlsym(RTLD_DEFAULT, "mx5_internal_missing_symbol"); errno = EDOM;
}
} }
extern "C" unsigned test_bootstraps() { return bootstraps; }
extern "C" unsigned test_policies() { return __sync_fetch_and_add(&policies, 0); }
extern "C" int test_outcome() { return int(mx5::runtime::loader_report().outcome); }
extern "C" const char* test_eager_error() { return mx5::runtime::loader_report().eager_error; }

extern "C" unsigned test_preparing() { return __sync_fetch_and_add(&preparing, 0); }
extern "C" void test_release_prepare() { __sync_lock_test_and_set(&prepare_release, 1); }
extern "C" unsigned test_patch_active() { return __sync_fetch_and_add(&patch_active, 0); }
extern "C" unsigned test_patch_finished() { return __sync_fetch_and_add(&patch_finished, 0); }

extern "C" int test_diagnostic_errno() { return mx5::runtime::loader_report().diagnostic_errno; }
