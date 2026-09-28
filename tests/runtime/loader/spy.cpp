#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#ifndef MX5_LOADER_TARGET
#error Target fixture required
#endif
static unsigned count, early_waiting, early_release;
static int seen[128];
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
typedef void* (*Open)(const char*, int);
static Open next() { return reinterpret_cast<Open>(dlsym(RTLD_NEXT, "dlopen")); }
extern "C" void* spy_direct_load(const char* p, int f) { return next()(p,f); }
extern "C" unsigned spy_early_waiting() { return __sync_fetch_and_add(&early_waiting,0); }
extern "C" void spy_release_early() { __sync_lock_test_and_set(&early_release,1); }
extern "C" unsigned spy_count() { return count; }
extern "C" int spy_flag(unsigned n) { return seen[n]; }
extern "C" void* dlopen(const char* p,int f) {
    if (p && !strcmp(p,MX5_LOADER_TARGET)) {
        pthread_mutex_lock(&mu);
        unsigned index=count;
        if (count < 128) seen[count] = f;
        ++count; pthread_mutex_unlock(&mu);
        if (index==0 && (f & RTLD_NOLOAD) && getenv("TEST_EARLY_NOLOAD")) {
            __sync_lock_test_and_set(&early_waiting,1);
            while(!__sync_fetch_and_add(&early_release,0)) usleep(1000);
        }
    }
    void* h=next()(p,f);
    // Verify errno belongs to the next chain, not preflight/bootstrap/cleanup.
    errno = h ? EBUSY : EACCES;
    return h;
}
