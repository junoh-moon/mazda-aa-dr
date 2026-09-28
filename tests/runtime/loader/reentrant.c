#include <dlfcn.h>
#include <assert.h>
#ifndef MX5_LOADER_TARGET
#error Target fixture required
#endif
__attribute__((constructor)) static void reenter(void) {
    void* h=dlopen(MX5_LOADER_TARGET,RTLD_LAZY|RTLD_LOCAL);
    assert(h); dlclose(h);
}
int fixture_answer(void) { return 42; }
