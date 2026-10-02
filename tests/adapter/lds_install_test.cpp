#include "adapter/lds_install.h"
#include "adapter/bus_hooks.h"
#include "runtime/sha256.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

namespace A=mx5::adapter;

// Authored harness: OEM files are supplied privately through the environment.
// Each invocation is a fresh process because installer bindings are immutable.
// Loading the actual service is not a normal SM boot or a ServiceInit test.
static void require(bool ok,const char* step) {
    if(ok)return;
    std::fprintf(stderr,"LDS_INSTALL_FAIL %s\n",step);
    std::fflush(0);std::exit(1);
}

#if defined(__arm__) && !defined(__aarch64__)
struct Module {
    const char* relative;
    const char* anchor;
    uintptr_t offset;
    const char* hash;
};
static const Module modules[]={
    {"jci/lds/svcjcilds.so","GetServiceInterfaces",0x5f6c,
     "0009af4c01d7628a0be491212ca6d73cfd3210b564b97446111435e3bac6a7ea"},
    {"jci/lib/libjcilds-dbus.so","LDS_DBUS_GetCurrentPosition",0x5424,
     "86fa094608e0c342ea759e522611d4c2d069006e2bb8909767f8835c164f18ce"},
    {"jci/lib/libjcilds-driver.so","LDS_DRIVER_RegisterLocationCallback",0x5c24,
     "b0a6668b8d0ad314c33a959227e816d33946a1e19ea162733b151441bdc74a93"},
    {"jci/lib/libjcidbus.so","JCIDBUS_obj_path_msg_function",0x20484,
     "b44b2f462c09376747a380ee3010501e952f557759898fad01a0fdcd573d375f"},
    {"jci/lib/libjcicommon.so","MEM_Copy",0xce20,
     "637cc53cd0621ec7fed24be2519b3e208191dd22954d2442d70e854413874b0d"},
    {"usr/lib/libdbus-1.so.3","dbus_connection_send",0xb82c,
     "07b06516d6ba93bbfa9db1e817278c7c96bd86fdd0f9dc48c917b1c100a8fe6b"},
    {"lib/libpthread.so.0","pthread_mutex_lock",0x83b8,
     "fc4b5aba8cdfe17322543ea639cc4a1dee82d99543e9c03b33f662f6b3b723f1"},
    {"jci/lib/libjcilds-nmea.so","LDS_NMEA_ParseSentence",0x2244,
     "4a882a4938c82891fb9fcf35e597e35cf5ae35d24690181d99b9789b7cb31059"},
    {"lib/libc.so.6","select",0xb9f90,
     "398c50696f97d95d98917bb5be0f9ffcd4d77f2bad78a8f68933e025438cae41"}
};
enum {MODULE_COUNT=sizeof modules/sizeof modules[0]};
struct Slot {unsigned module;uintptr_t offset;};
static const Slot slots[]={
    {0,0x1459c},{0,0x145b4},{0,0x145d8},{0,0x1460c},{0,0x1462c},{0,0x14634},
    {1,0x16358},{1,0x16364},{1,0x16388},{1,0x163e8},{1,0x163f8},{1,0x16418},
    {1,0x16498},{1,0x164b8},{3,0x346d0},{3,0x34240},{3,0x34274},{3,0x34304},
    {3,0x343d4},{3,0x34400},{3,0x34474},{3,0x34560},{3,0x345ec},{3,0x34604},
    {3,0x34654},{5,0x34600},{5,0x3454c},{0,0x146fc},{2,0x152bc},{2,0x15224}
};
enum {SLOT_COUNT=sizeof slots/sizeof slots[0]};
static unsigned begins,ends,hashes,emits;
static bool lease=true,restored;
static int rejected_module=-1;
static unsigned hash_seen,hash_rejected;
static uintptr_t* begin_change;
static uintptr_t begin_value;
static unsigned protection_calls,protection_failure;
static bool observe_protection;
#ifdef MX5DR_LDS_WRAP_MPROTECT
extern "C" int __real_mprotect(void*,size_t,int);
extern "C" int __wrap_mprotect(void* address,size_t length,int flags) {
    if(observe_protection && ++protection_calls==protection_failure) {
        errno=EACCES;return -1;
    }
    return __real_mprotect(address,length,flags);
}
#endif
static bool begin_patch() {
    ++begins;if(begin_change)*begin_change=begin_value;return lease;
}
static void end_patch(bool value) {++ends;restored=value;}
static bool verify_hash(const char* path,const char* expected) {
    ++hashes;
    for(unsigned i=0;i<MODULE_COUNT;++i) {
        const char* suffix=modules[i].relative;
        const size_t pn=path?std::strlen(path):0,sn=std::strlen(suffix);
        if(pn>=sn && !std::strcmp(path+pn-sn,suffix)) {
            require(expected && !std::strcmp(expected,modules[i].hash),"installer pins exact file hash");
            hash_seen|=1u<<i;
            if(int(i)==rejected_module) {++hash_rejected;return false;}
            return mx5_verify_file_sha256(path,expected);
        }
    }
    require(false,"only verified LDS dependency owner hashes");return false;
}
static uint64_t clock_observed(void*) {return 123456;}
static void emitted(const mx5::runtime::lds_sideband::Record&,void*) {++emits;}
static void published(const A::LdsLockedSend&,void*) {++emits;}
static void invalidated(A::LdsLockedLoss,void*) {++emits;}

struct Fixture {
    void* service;
    uintptr_t bases[MODULE_COUNT],before[SLOT_COUNT];
    A::LdsDescriptor descriptor;
    struct Code {const void* address;size_t size;char hash[65];};
    Code code[MODULE_COUNT*2];unsigned code_count;
    Fixture(int flags):service(0),bases(),before(),descriptor(),code(),code_count(0) {
        const char* root=std::getenv("MX5DR_LDS_STOCK");
        require(root && root[0]=='/',"absolute MX5DR_LDS_STOCK required");
        for(unsigned i=0;i<MODULE_COUNT;++i) {
            char path[1024];
            const int n=snprintf(path,sizeof path,"%s/%s",root,modules[i].relative);
            require(n>0 && size_t(n)<sizeof path,"stock path bounds");
            require(mx5_verify_file_sha256(path,modules[i].hash),"stock identity");
            void* handle=dlopen(path,i?RTLD_NOW|RTLD_NOLOAD:flags);
            if(!handle)std::fprintf(stderr,"dlopen: %s\n",dlerror());
            require(handle!=0,"original service/dependency cold load");
            if(!i)service=handle;
            void* anchor=dlsym(handle,modules[i].anchor);Dl_info info;
            std::memset(&info,0,sizeof info);
            require(anchor && dladdr(anchor,&info) && info.dli_fbase,"module anchor");
            bases[i]=reinterpret_cast<uintptr_t>(info.dli_fbase);
            require(reinterpret_cast<uintptr_t>(anchor)-bases[i]==modules[i].offset,
                    "module anchor offset");
            // Dependency probes must not themselves retain the modules that
            // the installer is responsible for keeping after caller dlclose.
            if(i)require(!dlclose(handle),"release fixture dependency reference");
        }
        for(unsigned i=0;i<SLOT_COUNT;++i)before[i]=*address(i);
        std::memcpy(&descriptor,reinterpret_cast<void*>(bases[1]+0x16538),sizeof descriptor);
        require(!std::strcmp(descriptor.name,"GetPosition") &&
                reinterpret_cast<uintptr_t>(descriptor.service)==bases[1]+0x478c &&
                reinterpret_cast<uintptr_t>(descriptor.generic)==bases[1]+0x735c &&
                !descriptor.reserved,"original descriptor");
        dl_iterate_phdr(snapshot_code,this);
        require(code_count>=MODULE_COUNT,"all original executable segments captured");
    }
    static int snapshot_code(dl_phdr_info* info,size_t,void* user) {
        Fixture& f=*static_cast<Fixture*>(user);bool owner=false;
        for(unsigned i=0;i<MODULE_COUNT;++i)owner|=uintptr_t(info->dlpi_addr)==f.bases[i];
        if(!owner)return 0;
        for(unsigned i=0;i<info->dlpi_phnum;++i) {
            const ElfW(Phdr)& p=info->dlpi_phdr[i];
            if(p.p_type!=PT_LOAD || !(p.p_flags&PF_X))continue;
            require((p.p_flags&(PF_R|PF_W))==PF_R,"original code is read/execute only");
            require(f.code_count<sizeof f.code/sizeof f.code[0],"bounded code regions");
            Code& c=f.code[f.code_count++];
            c.address=reinterpret_cast<void*>(uintptr_t(info->dlpi_addr)+p.p_vaddr);
            c.size=p.p_memsz;mx5_sha256_bytes(c.address,c.size,c.hash);
        }
        return 0;
    }
    uintptr_t* address(unsigned i) const {
        return reinterpret_cast<uintptr_t*>(bases[slots[i].module]+slots[i].offset);
    }
    A::LdsInstallOptions options() const {
        const A::LdsInstallOptions value={service,verify_hash,true,begin_patch,end_patch,
                                         clock_observed,emitted,0,0,0};
        return value;
    }
    void descriptor_unchanged() const {
        require(!std::memcmp(&descriptor,reinterpret_cast<void*>(bases[1]+0x16538),
                             sizeof descriptor),"descriptor is unchanged");
    }
    void code_unchanged() const {
        for(unsigned i=0;i<code_count;++i) {
            char now[65];mx5_sha256_bytes(code[i].address,code[i].size,now);
            require(!std::strcmp(now,code[i].hash),"no original executable bytes changed");
        }
    }
    void refresh_code_hashes() {
        for(unsigned i=0;i<code_count;++i)
            mx5_sha256_bytes(code[i].address,code[i].size,code[i].hash);
    }
    void no_publication() const {
        for(unsigned i=0;i<SLOT_COUNT;++i)
            require(*address(i)==before[i],"rejection publishes no data slots");
        descriptor_unchanged();code_unchanged();
        require(!emits,"rejection emits no observations");
    }
    void release_caller() {
        require(service && !dlclose(service),"caller releases service handle");service=0;
        for(unsigned i=0;i<MODULE_COUNT;++i) {
            Dl_info info;std::memset(&info,0,sizeof info);
            require(dladdr(reinterpret_cast<void*>(bases[i]+modules[i].offset),&info) &&
                    reinterpret_cast<uintptr_t>(info.dli_fbase)==bases[i],
                    "installer retains original dependency after caller dlclose");
        }
    }
    void released_without_retention() {
        require(service && !dlclose(service),"rejected caller releases original service");service=0;
        const char* root=std::getenv("MX5DR_LDS_STOCK");char path[1024];
        const int n=snprintf(path,sizeof path,"%s/%s",root,modules[0].relative);
        require(n>0 && size_t(n)<sizeof path,"unload path bounds");
        void* retained=dlopen(path,RTLD_LAZY|RTLD_NOLOAD);
        if(retained)dlclose(retained);
        require(!retained,"preparation not reached leaves no installer module reference");
    }
};

static void write_code_word(uintptr_t* address,uintptr_t value) {
    const long page=sysconf(_SC_PAGESIZE);require(page>0,"code fault page size");
    const uintptr_t start=uintptr_t(address)&~(uintptr_t(page)-1);
    // Only this process's private mapping changes; no file is opened for write.
    // The pthread case uses MOVS instead of MOV. Original ANDS overwrites those
    // flags before any conditional instruction, preserving loader/libc locks.
    require(!mprotect(reinterpret_cast<void*>(start),size_t(page),PROT_READ|PROT_WRITE),
            "author test-only code corruption");
    *address=value;
    require(!mprotect(reinterpret_cast<void*>(start),size_t(page),PROT_READ|PROT_EXEC),
            "restore test code page execution permission");
}

static void occupy_bus_preparation(const Fixture& f) {
    A::BusBindings b=A::BusBindings();
    b.create=reinterpret_cast<A::BusCreate>(f.before[13]);
    b.connect=reinterpret_cast<A::BusConnect>(f.before[9]);
    b.disconnect=reinterpret_cast<A::BusEnd>(f.before[12]);
    b.free=reinterpret_cast<A::BusEnd>(f.before[6]);
    b.signal=reinterpret_cast<A::BusSignal>(f.before[15]);
    b.is_signal=reinterpret_cast<A::BusIsSignal>(f.bases[5]+0x1459c);
    b.endpoint.registration=reinterpret_cast<decltype(b.endpoint.registration)>(f.bases[5]+0x89ac);
    b.endpoint.get_server_id=reinterpret_cast<decltype(b.endpoint.get_server_id)>(f.bases[5]+0xa448);
    b.endpoint.get_unique_name=reinterpret_cast<decltype(b.endpoint.get_unique_name)>(f.bases[5]+0x90a8);
    b.endpoint.free_guid=reinterpret_cast<decltype(b.endpoint.free_guid)>(f.bases[5]+0x2021c);
    b.endpoint.register_caller=f.bases[3]+0x6b58;
    require(A::prepare_bus_hooks(b,0),"existing immutable bus bindings prepared with original targets");
}

static uintptr_t replacement(unsigned i) {
    const uintptr_t values[]={
        uintptr_t(mx5_lds_update),uintptr_t(mx5_lds_driver_open),uintptr_t(mx5_lds_clear),
        uintptr_t(mx5_lds_register),uintptr_t(mx5_lds_read),uintptr_t(mx5_lds_initialize),
        uintptr_t(mx5_bus_free),uintptr_t(mx5_lds_mem_copy),uintptr_t(mx5_lds_mutex_unlock),
        uintptr_t(mx5_bus_connect),uintptr_t(mx5_lds_mutex_lock),uintptr_t(mx5_lds_set_callback),
        uintptr_t(mx5_bus_disconnect),uintptr_t(mx5_bus_create),uintptr_t(mx5_lds_path),
        uintptr_t(mx5_bus_signal),uintptr_t(mx5_bus_free),uintptr_t(mx5_lds_send),
        uintptr_t(mx5_lds_reply_message),uintptr_t(mx5_bus_connect),uintptr_t(mx5_lds_reply_create),
        uintptr_t(mx5_bus_disconnect),uintptr_t(mx5_bus_create),uintptr_t(mx5_bus_register),
        uintptr_t(mx5_lds_method_build),uintptr_t(mx5_lds_message_lock),uintptr_t(mx5_lds_native_mutex_lock),
        uintptr_t(mx5_lds_driver_close),uintptr_t(mx5_lds_parse_sentence),uintptr_t(mx5_lds_select)
    };
    static_assert(sizeof values/sizeof values[0]==SLOT_COUNT,"all replacement contracts");
    require(i<SLOT_COUNT,"replacement bounds");return values[i];
}
static unsigned callback_count;
static void authored_callback(void* value) {
    require(value==&callback_count,"original registration preserves closure");++callback_count;
}
static void safe_original_forwarding(const Fixture& f) {
    // This is a hardware-free API fixture, not ServiceInit or DriverOpen.
    typedef void (*Lifecycle)();
    typedef int32_t (*Read)(void*);
    typedef int32_t (*Update)(const void*);
    typedef int32_t (*Registration)(uint32_t,A::LdsCallback);
    typedef void* (*Copy)(void*,const void*,unsigned);
    struct Cache {
        int32_t mode,padding;uint64_t utc;
        float latitude,longitude;int32_t altitude;
        float heading,velocity,horizontal,vertical;uint32_t tail;
    };
    static_assert(sizeof(Cache)==48,"verified cache copy width");
    reinterpret_cast<Lifecycle>(*f.address(5))();
    Cache read=Cache();
    require(reinterpret_cast<Read>(*f.address(4))(&read)==100,"installed read calls original");
    require(!std::memcmp(&read,reinterpret_cast<void*>(f.bases[1]+0x16860),sizeof read),
            "read copies actual original initialized cache");
    const Cache authored={1,0,123456789,35.25f,139.5f,27,45.0f,12.5f,1.5f,2.25f,0};
    require(reinterpret_cast<Update>(*f.address(0))(&authored)==100,"installed update calls original");
    require(reinterpret_cast<Read>(*f.address(4))(&read)==100 &&
            !std::memcmp(&read,&authored,sizeof read),"original cache stores authored value exactly");
    Cache direct=Cache();
    require(reinterpret_cast<Read>(f.before[4])(&direct)==100 &&
            !std::memcmp(&direct,&read,sizeof read),"saved original remains callable");
    errno=E2BIG;const int32_t expected=reinterpret_cast<Read>(f.before[4])(0);
    const int expected_errno=errno;
    errno=E2BIG;const int32_t observed=reinterpret_cast<Read>(*f.address(4))(0);
    require(observed==expected && errno==expected_errno,"null read result and original errno forwarded");
    char from[7]="abc123",to[7]={0};
    require(reinterpret_cast<Copy>(*f.address(7))(to,from,sizeof from)==to &&
            !std::memcmp(from,to,sizeof from),"unrelated original MEM_Copy stays callable");
    pthread_mutex_t mutex;require(!pthread_mutex_init(&mutex,0),"authored mutex initialized");
    typedef void (*Lock)(void*,const char*,unsigned);
    typedef void (*Unlock)(void*);
    reinterpret_cast<Lock>(*f.address(10))(&mutex,"authored installer fixture",1);
    require(pthread_mutex_trylock(&mutex)==EBUSY,"retained original mutex provider locked");
    reinterpret_cast<Unlock>(*f.address(8))(&mutex);
    require(!pthread_mutex_trylock(&mutex),"retained original mutex provider unlocked");
    require(!pthread_mutex_unlock(&mutex) && !pthread_mutex_destroy(&mutex),"authored mutex released");
    require(!pthread_mutex_init(&mutex,0),"authored native lock mutex initialized");
    typedef int32_t (*NativeLock)(void*);
    errno=E2BIG;const int32_t native_expected=reinterpret_cast<NativeLock>(f.before[26])(&mutex);
    const int native_errno=errno;
    require(native_expected==0 && !pthread_mutex_unlock(&mutex),"original native lock callable");
    errno=E2BIG;require(reinterpret_cast<NativeLock>(*f.address(26))(&mutex)==native_expected &&
        errno==native_errno,"installed native lock forwards return and errno without a send frame");
    require(pthread_mutex_trylock(&mutex)==EBUSY && !pthread_mutex_unlock(&mutex) &&
        !pthread_mutex_destroy(&mutex),"actual original native mutex acquired exactly once");
    Dl_info info;require(dladdr(reinterpret_cast<void*>(f.bases[5]+0xb82c),&info),"raw module path");
    void* raw=dlopen(info.dli_fname,RTLD_LAZY|RTLD_NOLOAD);require(raw!=0,"retained raw dependency");
    typedef void* (*NewMessage)(int32_t);
    typedef void (*MessageOp)(void*);
    typedef int32_t (*SetSerial)(void*,uint32_t);
    NewMessage create=reinterpret_cast<NewMessage>(dlsym(raw,"dbus_message_new"));
    MessageOp unref=reinterpret_cast<MessageOp>(dlsym(raw,"dbus_message_unref"));
    SetSerial set_serial=reinterpret_cast<SetSerial>(dlsym(raw,"dbus_message_set_reply_serial"));
    require(create && unref && set_serial,"original message API");
    void* message=create(2);require(message && set_serial(message,1),"authored original reply allocated");
    errno=E2BIG;reinterpret_cast<MessageOp>(f.before[25])(message);const int message_errno=errno;
    errno=E2BIG;reinterpret_cast<MessageOp>(*f.address(25))(message);
    require(errno==message_errno,"message lock forwards errno outside a send frame");
    unref(message);dlclose(raw);
    require(reinterpret_cast<Registration>(*f.address(3))(1,authored_callback)==100,
            "unrelated registration forwards into original table");
    A::LdsCallback registered=*reinterpret_cast<A::LdsCallback*>(f.bases[2]+0x158f8+4);
    require(registered==authored_callback,"unrelated callback is unchanged");
    registered(&callback_count);require(callback_count==1,"original registered callback callable");
    reinterpret_cast<Lifecycle>(*f.address(2))();
    require(!emits,"API calls invent no D-Bus request/snapshot observation");
    f.descriptor_unchanged();f.code_unchanged();
}

static unsigned case_index(const char* value,const char* prefix,unsigned count) {
    const size_t n=std::strlen(prefix);require(!std::strncmp(value,prefix,n),"case prefix");
    char* end=0;const unsigned long parsed=std::strtoul(value+n,&end,10);
    require(value[n] && end && !*end && parsed<count,"case index bounds");return unsigned(parsed);
}

static void original_registration_route(Fixture& f,unsigned id) {
    // These ten ID/target pairs are decoded from the original
    // lds_startOperation arguments and its R_ARM_RELATIVE literals. The
    // enclosing hardware-dependent operation is deliberately not invoked.
    const uintptr_t offsets[]={0x41d4,0x41d4,0x400c,0x400c,0x3e50,
                               0x400c,0x41d4,0x41d4,0x3e50,0x400c};
    const uintptr_t literals[]={0x6a6c,0x6a6c,0x6ab0,0x6ab0,0x6acc,
                               0x6ab0,0x6a6c,0x6a6c,0x6acc,0x6ab0};
    require(id<10,"original registered route ID");
    const A::LdsCallback original=reinterpret_cast<A::LdsCallback>(f.bases[0]+offsets[id]);
    require(*reinterpret_cast<uintptr_t*>(f.bases[0]+literals[id])==uintptr_t(original),
            "actual original registration literal matches callback route");
    typedef void (*Lifecycle)();
    reinterpret_cast<Lifecycle>(*f.address(5))();
    typedef int32_t (*Registration)(uint32_t,A::LdsCallback);
    Registration registration=reinterpret_cast<Registration>(*f.address(3));
    require(registration(id,original)==100,"original route registers successfully");
    const A::LdsCallback retained=*reinterpret_cast<A::LdsCallback*>(f.bases[2]+0x158f8+4*id);
    require(retained && retained!=original,"every original route installs its ID-specific wrapper");
    require(registration(id,authored_callback)==104,"original duplicate registration refused");
    require(*reinterpret_cast<A::LdsCallback*>(f.bases[2]+0x158f8+4*id)==retained,
            "duplicate never retargets a retained original wrapper");
    // Each exact original handler has a null-input return path. Forward that
    // path and compare errno without fabricating parser or sensor input.
    errno=E2BIG;original(0);const int expected_errno=errno;
    errno=E2BIG;retained(0);
    require(errno==expected_errno && !callback_count,"retained wrapper calls immutable original target");
    reinterpret_cast<Lifecycle>(*f.address(2))();
    require(!emits,"registration-only fixture invents no response lineage");
    f.descriptor_unchanged();f.code_unchanged();
}

static void run_case(const char* scenario) {
    const bool global=std::strstr(scenario,"global")!=0;
    Fixture f(RTLD_NOW|(global?RTLD_GLOBAL:RTLD_LOCAL));
    A::LdsInstallOptions options=f.options();
    A::InstallResult expected=A::INSTALL_OK;
    bool denied=false,lease_reached=false,preoccupied=false;
    uintptr_t* corrupt=0;uintptr_t original_word=0;
    if(!std::strcmp(scenario,"unload-baseline")) {
        f.released_without_retention();return;
    }
    if(!std::strncmp(scenario,"invalid-",8)) {
        switch(case_index(scenario,"invalid-",9)) {
        case 0:options.service_handle=0;break;
        case 1:options.verify_file_hash=0;break;
        case 2:options.verified_cold_start=false;break;
        case 3:options.begin_patch=0;break;
        case 4:options.end_patch=0;break;
        case 5:options.clock=0;break;
        case 6:options.emit=0;break;
        case 7:options.publish_locked=published;break;
        case 8:options.invalidate_locked=invalidated;break;
        }
        expected=A::INVALID_INSTALL_ARGUMENT;
    } else if(!std::strncmp(scenario,"hash-",5)) {
        rejected_module=int(case_index(scenario,"hash-",MODULE_COUNT));
        expected=A::FILE_IDENTITY_MISMATCH;
    } else if(!std::strncmp(scenario,"chain-",6)) {
        const unsigned i=case_index(scenario,"chain-",SLOT_COUNT);
        // Perturb only our own loaded copy. No OEM file is modified. The
        // target stays inside its real function but is no longer its entry.
        *f.address(i)=f.before[i]+4;f.before[i]=*f.address(i);
        expected=A::NEXT_CHAIN_MISMATCH;
    } else if(!std::strcmp(scenario,"denied-lease")) {
        lease=false;denied=true;expected=A::COLD_START_LOST;
    } else if(!std::strncmp(scenario,"descriptor-",11)) {
        const unsigned i=case_index(scenario,"descriptor-",4);
        uintptr_t* words=reinterpret_cast<uintptr_t*>(f.bases[1]+0x16538);
        words[i]+=4;
        std::memcpy(&f.descriptor,words,sizeof f.descriptor);
        expected=A::ORIGINAL_BYTES_MISMATCH;
    } else if(!std::strncmp(scenario,"cache-literal-",14)) {
        const uintptr_t literals[]={0x43e0,0x48a4,0x54a4,0x57d4};
        corrupt=reinterpret_cast<uintptr_t*>(f.bases[1]+literals[case_index(scenario,"cache-literal-",4)]);
        original_word=*corrupt;write_code_word(corrupt,original_word+4);
        f.refresh_code_hashes();expected=A::NEXT_CHAIN_MISMATCH;
    } else if(!std::strcmp(scenario,"generation-pointer")) {
        corrupt=reinterpret_cast<uintptr_t*>(f.bases[5]+0x3470c);
        original_word=*corrupt;*corrupt=original_word+4;
        expected=A::NEXT_CHAIN_MISMATCH;
    } else if(!std::strcmp(scenario,"function-prefix") || !std::strcmp(scenario,"caller-word") ||
              !std::strcmp(scenario,"register-word") || !std::strcmp(scenario,"pthread-prefix") ||
              !std::strcmp(scenario,"message-prefix") || !std::strcmp(scenario,"native-caller") ||
              !std::strcmp(scenario,"message-caller") || !std::strcmp(scenario,"native-wrapper")) {
        const bool prefix=!std::strcmp(scenario,"function-prefix");
        const bool registration=!std::strcmp(scenario,"register-word");
        corrupt=reinterpret_cast<uintptr_t*>(registration?f.bases[3]+0x6b44:
                    prefix?f.bases[0]+0x5f6c:f.bases[1]+0x5440);
        if(!std::strcmp(scenario,"pthread-prefix"))corrupt=reinterpret_cast<uintptr_t*>(f.bases[6]+0x83bc);
        if(!std::strcmp(scenario,"message-prefix"))corrupt=reinterpret_cast<uintptr_t*>(f.bases[5]+0x13268);
        if(!std::strcmp(scenario,"native-caller"))corrupt=reinterpret_cast<uintptr_t*>(f.bases[5]+0xb840);
        if(!std::strcmp(scenario,"message-caller"))corrupt=reinterpret_cast<uintptr_t*>(f.bases[5]+0xb704);
        if(!std::strcmp(scenario,"native-wrapper"))corrupt=reinterpret_cast<uintptr_t*>(f.bases[5]+0x25388);
        original_word=*corrupt;
        write_code_word(corrupt,original_word^(!std::strcmp(scenario,"pthread-prefix")?0x00100000u:1u));
        f.refresh_code_hashes();expected=A::ORIGINAL_BYTES_MISMATCH;
    } else if(!std::strncmp(scenario,"input-",6)) {
        if(!std::strcmp(scenario,"input-close-prefix"))corrupt=reinterpret_cast<uintptr_t*>(f.bases[2]+0x5808);
        if(!std::strcmp(scenario,"input-parser-prefix"))corrupt=reinterpret_cast<uintptr_t*>(f.bases[7]+0x2244);
        if(!std::strcmp(scenario,"input-select-prefix"))corrupt=reinterpret_cast<uintptr_t*>(f.bases[8]+0xb9f90);
        if(!std::strcmp(scenario,"input-parser-call"))corrupt=reinterpret_cast<uintptr_t*>(f.bases[2]+0x3fd8);
        if(!std::strcmp(scenario,"input-select-call"))corrupt=reinterpret_cast<uintptr_t*>(f.bases[2]+0x3df4);
        if(!std::strcmp(scenario,"input-dispatch-call"))corrupt=reinterpret_cast<uintptr_t*>(f.bases[2]+0x44fc);
        require(corrupt!=0,"known parser ownership instruction");
        original_word=*corrupt;write_code_word(corrupt,original_word^1u);
        f.refresh_code_hashes();expected=A::ORIGINAL_BYTES_MISMATCH;
    } else if(!std::strncmp(scenario,"protect-",8)) {
#ifndef MX5DR_LDS_WRAP_MPROTECT
        require(false,"protect cases require -DMX5DR_LDS_WRAP_MPROTECT and --wrap=mprotect");
#endif
        protection_failure=case_index(scenario,"protect-",5)+1;
        lease_reached=true;expected=A::MEMORY_PROTECTION_FAILED;
    } else if(!std::strcmp(scenario,"lease-chain-change")) {
        begin_change=f.address(17);begin_value=f.before[17]+4;
        lease_reached=true;expected=A::NEXT_CHAIN_MISMATCH;
    } else if(!std::strcmp(scenario,"preoccupied-bus")) {
        occupy_bus_preparation(f);preoccupied=true;lease_reached=true;
        expected=A::CONFIGURATION_FAILED;
    } else {
        require(!std::strncmp(scenario,"registration-",13) ||
                !std::strcmp(scenario,"positive-local") || !std::strcmp(scenario,"positive-global") ||
                !std::strcmp(scenario,"retain-local") || !std::strcmp(scenario,"retain-global") ||
                !std::strcmp(scenario,"unthreaded-publisher") || !std::strcmp(scenario,"repeat"),"known case");
    }
    if(!std::strcmp(scenario,"unthreaded-publisher")) {
        require(*reinterpret_cast<uint32_t*>(f.bases[5]+0x34860)==0,"original threading is not initialized");
        options.publish_locked=published;options.invalidate_locked=invalidated;
    }
    observe_protection=true;
    errno=E2BIG;
    const A::InstallResult result=A::install_lds_v74(options);
    require(errno==E2BIG,"installer preserves entry errno");
    observe_protection=false;
    std::printf("LDS_INSTALL_RESULT %d EXPECTED %d\n",int(result),int(expected));
    require(result==expected,expected==A::INSTALL_OK?"cold exact original install returns INSTALL_OK":
            "invalid cold installation returns exact failure");
    if(expected!=A::INSTALL_OK) {
        require(begins==unsigned(denied||lease_reached) && ends==unsigned(lease_reached) &&
                (!lease_reached || restored),"prepublication rejection honors lease and end(true)");
        if(rejected_module>=0)require(hash_rejected==1,"rejected owner was actually verified");
        if(begin_change)f.before[17]=begin_value;
        f.no_publication();
        if(corrupt) {
            if(!std::strcmp(scenario,"generation-pointer"))*corrupt=original_word;
            else write_code_word(corrupt,original_word);
        }
        if(protection_failure)require(protection_calls==protection_failure,"injected mprotect failure reached");
        if(preoccupied) {
            f.release_caller();safe_original_forwarding(f);
        } else if(lease_reached || denied) {
            safe_original_forwarding(f);
            f.released_without_retention();
        }
        return;
    }
    require(begins==1 && ends==1 && restored,"one successful cold lease");
    require(hashes>=MODULE_COUNT && hash_seen==((1u<<MODULE_COUNT)-1),"all original owners verified");
    for(unsigned i=0;i<SLOT_COUNT;++i)
        require(*f.address(i)==replacement(i) && *f.address(i)!=f.before[i],"all 30 exact data slots published");
    f.descriptor_unchanged();f.code_unchanged();
    if(!std::strncmp(scenario,"registration-",13)) {
        original_registration_route(f,case_index(scenario,"registration-",10));return;
    }
    if(!std::strcmp(scenario,"repeat")) {
        require(A::install_lds_v74(options)==A::ALREADY_INSTALLED,"repeat installation is refused");
        require(begins==1 && ends==1,"repeat acquires no new lease");
        for(unsigned i=0;i<SLOT_COUNT;++i)require(*f.address(i)==replacement(i),"repeat leaves all slots unchanged");
    }
    if(!std::strncmp(scenario,"retain-",7))f.release_caller();
    safe_original_forwarding(f);
}
#endif

int main(int argc,char** argv) {
    alarm(30);
#if defined(__arm__) && !defined(__aarch64__)
    require(argc==2,"one case per process");
    run_case(argv[1]);
#else
    (void)argc;(void)argv;
    const A::LdsInstallOptions options=A::LdsInstallOptions();
    require(A::install_lds_v74(options)==A::UNSUPPORTED_ARCH,"unsupported host");
#endif
    std::puts("PASS LDS cold installer case; ServiceInit not executed");
}
