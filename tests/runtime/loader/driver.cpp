#include "runtime/loader.h"
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <dlfcn.h>
#include <pthread.h>
#ifndef MX5_LOADER_TARGET
#error Target fixture required
#endif
using mx5::runtime::LoaderReport;
template<class T> static T symbol(const char* name) {
    T p=reinterpret_cast<T>(dlsym(RTLD_DEFAULT,name)); assert(p); return p;
}
static void* threaded(void*) {
    errno=17; void* h=dlopen(MX5_LOADER_TARGET,RTLD_LAZY|RTLD_LOCAL);
    assert(h && errno==EBUSY); assert(!dlerror()); return h;
}
static void* noload_thread(void*) {
    errno=17; void* h=dlopen(MX5_LOADER_TARGET,RTLD_LAZY|RTLD_NOLOAD);
    assert(h && errno==EBUSY); assert(!dlerror()); return h;
}
int main(int argc,char** argv) {
    assert(argc==2); const char* name=argv[1];
    const bool closed_log = strstr(name,"log_closed") != 0;
    if (closed_log) { close(2); name = !strncmp(name,"lazy",4) ? "lazy" : "bad"; }
    unsigned(*boots)()=symbol<unsigned(*)()>("test_bootstraps");
    unsigned(*policies)()=symbol<unsigned(*)()>("test_policies");
    unsigned(*count)()=symbol<unsigned(*)()>("spy_count");
    int(*flag)(unsigned)=symbol<int(*)(unsigned)>("spy_flag");
    int(*outcome)()=symbol<int(*)()>("test_outcome");
    const char*(*eager_error)()=symbol<const char*(*)()>("test_eager_error");
    int(*diagnostic_errno)()=symbol<int(*)()>("test_diagnostic_errno");
    int flags=RTLD_LAZY|RTLD_LOCAL;
    void* prior=0;
    if (!strcmp(name,"existing")) {
        void*(*direct)(const char*,int)=symbol<void*(*)(const char*,int)>("spy_direct_load");
        prior=direct(MX5_LOADER_TARGET,flags); assert(prior);
    }
    if (!strcmp(name,"early_noload")) {
        unsigned(*waiting)()=symbol<unsigned(*)()>("spy_early_waiting");
        void(*resume)()=symbol<void(*)()>("spy_release_early");
        unsigned(*preparing)()=symbol<unsigned(*)()>("test_preparing");
        void(*release)()=symbol<void(*)()>("test_release_prepare");
        pthread_t reader,owner;
        assert(!pthread_create(&reader,0,noload_thread,0));
        while(!waiting()) usleep(1000);
        assert(!pthread_create(&owner,0,threaded,0));
        while(!preparing()) usleep(1000);
        resume();
        void *read_handle,*owner_handle;
        assert(!pthread_join(reader,&read_handle));
        release(); assert(!pthread_join(owner,&owner_handle));
        assert(boots()==0 && policies()==1 && outcome()==LoaderReport::EXPOSED);
        dlclose(read_handle); dlclose(owner_handle);
    } else if (!strcmp(name,"prepare_race") || !strcmp(name,"patch_race") ||
        !strcmp(name,"prepare_noload") || !strcmp(name,"patch_noload")) {
        const bool preparing = !strncmp(name,"prepare_",8);
        const bool noload = strstr(name,"noload") != 0;
        unsigned(*ready)()=symbol<unsigned(*)()>(preparing ? "test_preparing" : "test_patch_active");
        void(*release)()=symbol<void(*)()>("test_release_prepare");
        unsigned(*finished)()=symbol<unsigned(*)()>("test_patch_finished");
        pthread_t owner; assert(!pthread_create(&owner,0,threaded,0));
        while(!ready()) usleep(1000);
        void* second=dlopen(MX5_LOADER_TARGET,flags | (noload ? RTLD_NOLOAD : 0)); assert(second);
        if (preparing) release();
        else assert(finished()); // The second caller cannot escape during patch.
        void* handle; assert(!pthread_join(owner,&handle));
        assert(policies()==1 && boots()==unsigned(!preparing));
        if(preparing) assert(outcome()==LoaderReport::EXPOSED);
        dlclose(handle); dlclose(second);
    } else if (!strcmp(name,"cross_constructor")) {
        pthread_t owner; assert(!pthread_create(&owner,0,threaded,0));
        while(!policies()) usleep(1000);
        void* foreign=dlopen(getenv("TEST_CROSS"),RTLD_NOW); assert(foreign);
        void* handle; assert(!pthread_join(owner,&handle));
        assert(boots()==0 && policies()==1);
        dlclose(handle); dlclose(foreign);
    } else if (!strcmp(name,"concurrent")) {
        pthread_t threads[8]; void* handles[8];
        for(unsigned i=0;i<8;++i) assert(!pthread_create(&threads[i],0,threaded,0));
        for(unsigned i=0;i<8;++i) assert(!pthread_join(threads[i],&handles[i]));
        assert(boots()==0 && policies()==1 && count()==9);
        for(unsigned i=0;i<8;++i) dlclose(handles[i]);
    } else {
        const bool off=!strcmp(name,"off") || !strcmp(name,"invalid") || !strcmp(name,"disabled") || !strcmp(name,"missing_config") || !strcmp(name,"marker_error") || !strcmp(name,"marker_symlink");
        if (!strcmp(name,"noload")) flags|=RTLD_NOLOAD;
        if (!strcmp(name,"invalid_flags")) flags=RTLD_LOCAL;
        if (!strcmp(name,"caller_now")) flags=RTLD_NOW|RTLD_LOCAL;
        if (!strcmp(name,"off_global")) flags|=RTLD_GLOBAL;
        dlerror(); errno=17;
        void* h=dlopen(MX5_LOADER_TARGET,flags);
        const int result_errno=errno;
        const char* error=dlerror();
        const bool fail=!strcmp(name,"bad") || !strcmp(name,"noload") || !strcmp(name,"invalid_flags") || !strcmp(name,"caller_now");
        if(fail) { assert(!h && result_errno==EACCES); if(strcmp(name,"noload")) assert(error && *error); }
        else { assert(h && result_errno==EBUSY && !error); }
        if(off || !strcmp(name,"off_global")) {
            assert(count()==1 && flag(0)==flags && policies()==1 && boots()==0);
            assert(outcome()==LoaderReport::BYPASS);
        } else if(!strcmp(name,"noload") || !strcmp(name,"invalid_flags")) {
            assert(count()==1 && flag(0)==flags && policies()==0 && boots()==0);
        } else if(!strcmp(name,"lazy") || !strcmp(name,"bad")) {
            assert(count()==3 && flag(0)==(RTLD_LAZY|RTLD_NOLOAD));
            assert(flag(1)==RTLD_NOW && flag(2)==flags && boots()==0);
            assert(strstr(eager_error(),"mx5_fixture_missing"));
            assert(diagnostic_errno() == (closed_log ? EBADF : 0));
            assert(outcome()==(!strcmp(name,"lazy") ? LoaderReport::EAGER_FAILED_FALLBACK_OK : LoaderReport::EAGER_FAILED_FALLBACK_FAILED));
        } else if(!strcmp(name,"caller_now")) {
            assert(count()==2 && flag(1)==flags && boots()==0);
            assert(outcome()==LoaderReport::LOAD_FAILED);
        } else if(!strcmp(name,"existing")) {
            assert(count()==2 && flag(1)==flags && boots()==0);
            assert(outcome()==LoaderReport::ALREADY_LOADED);
        } else if(!strcmp(name,"reentrant")) {
            assert(count()==3 && boots()==0 && outcome()==LoaderReport::EXPOSED);
        } else { assert(count()==2 && boots()==1 && outcome()==LoaderReport::BOOTSTRAP_CALLED); }
        if(strcmp(name,"noload") && strcmp(name,"invalid_flags")) {
            unsigned before=count();
            void* second=dlopen(MX5_LOADER_TARGET,flags);
            assert(count()==before+1 && flag(before)==flags && policies()==1);
            if(second) dlclose(second);
        }
        if(h)dlclose(h);
    }
    if(prior)dlclose(prior);
    void* self=dlopen(0,RTLD_NOW); assert(self); dlclose(self);
    assert(!dlopen("/nonexistent-mx5-unrelated.so",RTLD_NOW));
    assert(dlerror());
    std::printf("PASS loader %s\n",name);
}
