#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <limits.h>
#include <pthread.h>

static void* load_thread(void*) {
    for (unsigned i=0;i<100;++i) {
        void* h=dlopen("libm.so.6",RTLD_NOW|RTLD_LOCAL);assert(h);
        typedef double(*Cosine)(double);
        Cosine cosine=reinterpret_cast<Cosine>(dlsym(h,"cos"));
        assert(cosine && cosine(0.0)==1.0);assert(!dlclose(h));
    }
    return 0;
}
int main(int argc, char** argv) {
    // The dynamic loader may warn and continue when LD_PRELOAD cannot load.
    // Prove the production interposer owns dlopen before exercising it.
    // RTLD_DEFAULT can return this non-PIE executable's canonical PLT entry;
    // RTLD_NEXT starts at its first dependency, including the preload.
    Dl_info info;
    char expected[PATH_MAX], actual[PATH_MAX];
    if(argc!=2 || !realpath(argv[1],expected) ||
       !dladdr(dlsym(RTLD_NEXT,"dlopen"),&info) ||
       !info.dli_fname || !realpath(info.dli_fname,actual) ||
       std::strcmp(expected,actual)!=0) {
        std::fputs("Production preload did not supply dlopen; smoke test failed\n",stderr);
        return 1;
    }
    pthread_t threads[4];
    for(unsigned i=0;i<4;++i)assert(!pthread_create(&threads[i],0,load_thread,0));
    for(unsigned i=0;i<4;++i)assert(!pthread_join(threads[i],0));
    void* self=dlopen(0,RTLD_NOW);assert(self);assert(!dlclose(self));
    assert(!dlopen("/nonexistent-mx5dr-fixture.so",RTLD_NOW|RTLD_NOLOAD));
    puts("Preload loader smoke: 400 concurrent non-target loads, null path and NOLOAD passed; no OEM module executed");
}
