#include <cassert>
#include <cstdio>
#include <dlfcn.h>
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
int main() {
    pthread_t threads[4];
    for(unsigned i=0;i<4;++i)assert(!pthread_create(&threads[i],0,load_thread,0));
    for(unsigned i=0;i<4;++i)assert(!pthread_join(threads[i],0));
    void* self=dlopen(0,RTLD_NOW);assert(self);assert(!dlclose(self));
    assert(!dlopen("/nonexistent-mx5dr-fixture.so",RTLD_NOW|RTLD_NOLOAD));
    puts("Preload loader smoke: 400 concurrent non-target loads, null path and NOLOAD passed; no OEM module executed");
}
