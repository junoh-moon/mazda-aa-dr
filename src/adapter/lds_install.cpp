#include "lds_install.h"
#include "data_patch.h"
#include "bus_hooks.h"
#include <errno.h>

#if defined(__arm__) && !defined(__ARM_PCS_VFP) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {
namespace A=mx5::adapter;
namespace D=A::data_patch;
enum ModuleId { SERVICE,LDS,DRIVER,BUS,RAW,COMMON,MODULE_COUNT };
struct ModuleSpec { const char* anchor;const char* hash; };
// Exact NA 74.00.324A. Use the live service dependency scope.
const ModuleSpec modules[]={
    {"GetServiceInterfaces","0009af4c01d7628a0be491212ca6d73cfd3210b564b97446111435e3bac6a7ea"},
    {"LDS_DBUS_GetCurrentPosition","86fa094608e0c342ea759e522611d4c2d069006e2bb8909767f8835c164f18ce"},
    {"LDS_DRIVER_RegisterLocationCallback","b0a6668b8d0ad314c33a959227e816d33946a1e19ea162733b151441bdc74a93"},
    {"JCIDBUS_conn_create","b44b2f462c09376747a380ee3010501e952f557759898fad01a0fdcd573d375f"},
    {"dbus_message_get_type","07b06516d6ba93bbfa9db1e817278c7c96bd86fdd0f9dc48c917b1c100a8fe6b"},
    {"MEM_Copy","637cc53cd0621ec7fed24be2519b3e208191dd22954d2442d70e854413874b0d"},
};
struct FunctionSpec {
    ModuleId owner;const char* name;uintptr_t offset;uint32_t prefix[2];
};
// Short entry signatures only; no OEM implementation is shipped.
const FunctionSpec functions[]={
    {SERVICE,"GetServiceInterfaces",0x5f6c,{0xe59f0000,0xe12fff1e}},
    {LDS,"LDS_DBUS_UpdateCurrentPosition",0x574c,{0xe92d4010,0xe2504000}},
    {DRIVER,"LDS_DRIVER_Open",0x4e6c,{0xe92d41f0,0xe24dd018}},
    {LDS,"LDS_DBUS_Position_Data_Interface_Clear",0x4460,{0xe52de004,0xe59f00bc}},
    {DRIVER,"LDS_DRIVER_RegisterLocationCallback",0x5c24,{0xe52de004,0xe2512000}},
    {LDS,"LDS_DBUS_GetCurrentPosition",0x5424,{0xe92d4010,0xe2504000}},
    {LDS,"LDS_DBUS_Position_Data_Interface_Initialize",0x413c,{0xe92d45f0,0xe59f0280}},
    {BUS,"JCIDBUS_conn_free",0xb930,{0xe92d4800,0xe28db004}},
    {COMMON,"MEM_Copy",0xce20,{0xe92d4010,0xe24dd010}},
    {SERVICE,"LDS_MutexUnlock",0x7b48,{0xe52de004,0xe3500000}},
    {BUS,"JCIDBUS_conn_connect",0xb360,{0xe92d4800,0xe28db004}},
    {SERVICE,"LDS_MutexLock_proc",0x7aa0,{0xe92d4030,0xe3500000}},
    {BUS,"JCIDBUS_obj_method_set_callback",0x1fe3c,{0xe52db004,0xe28db000}},
    {BUS,"JCIDBUS_conn_disconnect",0xac54,{0xe92d4800,0xe28db004}},
    {BUS,"JCIDBUS_conn_create",0xb8c4,{0xe92d4800,0xe28db004}},
    {BUS,"JCIDBUS_obj_path_msg_function",0x20484,{0xe92d4800,0xe28db004}},
    {BUS,"JCIDBUS_signal_handler",0x2098c,{0xe92d4800,0xe28db004}},
    {RAW,"dbus_connection_send",0xb82c,{0xe92d41f0,0xe1a04000}},
    {BUS,"JCIDBUS_reply_create_msg",0xff34,{0xe92d4800,0xe28db004}},
    {BUS,"JCIDBUS_reply_create",0x10380,{0xe92d4810,0xe28db008}},
    {RAW,"dbus_bus_register",0x89ac,{0xe92d47f0,0xe59f4178}},
    {BUS,"JCIDBUS_method_build",0x19ac8,{0xe92d4870,0xe28db010}},
    {RAW,"dbus_message_get_type",0x1353c,{0xe2800004,0xeaffed31}},
    {RAW,"dbus_message_get_serial",0x13298,{0xe2800004,0xeaffede7}},
    {RAW,"dbus_message_get_reply_serial",0x132c8,{0xe52de004,0xe3a01005}},
    {RAW,"dbus_message_get_sender",0x14344,{0xe52de004,0xe3a02000}},
    {RAW,"dbus_message_get_destination",0x14304,{0xe52de004,0xe3a02000}},
    {RAW,"dbus_message_get_path",0x14078,{0xe52de004,0xe3a02000}},
    {RAW,"dbus_message_get_interface",0x14144,{0xe52de004,0xe3a02000}},
    {RAW,"dbus_message_get_member",0x141c8,{0xe52de004,0xe3a02000}},
    {RAW,"dbus_message_is_signal",0x1459c,{0xe1a0c001,0xe1a03002}},
    {RAW,"dbus_connection_get_server_id",0xa448,{0xe92d4038,0xe1a04000}},
    {RAW,"dbus_bus_get_unique_name",0x90a8,{0xe92d4038,0xe59f3038}},
    {RAW,"dbus_free",0x2021c,{0xe3500000,0x012fff1e}},
    {LDS,"LDS_DATA_GetPosition_svc",0x478c,{0xe92d4ff0,0xe3520000}},
    {LDS,0,0x735c,{0xe92d41f0,0xe3a0c000}},
    {SERVICE,0,0x41d4,{0xe92d40f0,0xe2504000}},
    {SERVICE,0,0x400c,{0xe92d4070,0xe2504000}},
    {SERVICE,0,0x3e50,{0xe92d4030,0xe2504000}},
};
struct Module { uintptr_t base;const char* path; };
struct Range {
    uintptr_t base,address;size_t size,page;unsigned flags;
    bool found,relro;
};
int find_range(dl_phdr_info* info,size_t,void* user) {
    Range& r=*static_cast<Range*>(user);
    if(uintptr_t(info->dlpi_addr)!=r.base)return 0;
    for(unsigned i=0;i<info->dlpi_phnum;++i) {
        const ElfW(Phdr)& p=info->dlpi_phdr[i];
        const uintptr_t start=r.base+p.p_vaddr,end=start+p.p_memsz;
        if(end<start)continue;
        if(p.p_type==PT_GNU_RELRO && (r.flags&PF_W)) {
            const uintptr_t first=start&~(uintptr_t(r.page)-1);
            const uintptr_t last=(end+r.page-1)&~(uintptr_t(r.page)-1);
            if(r.address<last && first<r.address+r.size)r.relro=true;
        }
        if(p.p_type==PT_LOAD && p.p_flags==r.flags &&
           r.address>=start && r.address<=end && r.size<=end-r.address)
            r.found=true;
    }
    return 1;
}
bool range(const Module& m,uintptr_t address,size_t size,unsigned flags,size_t page) {
    if(address<m.base || address+size<address)return false;
    Range r={m.base,address,size,page,flags,false,false};
    dl_iterate_phdr(find_range,&r);
    return r.found && !r.relro;
}
bool owner(const Module& m,uintptr_t address) {
    Dl_info info=Dl_info();
    return dladdr(reinterpret_cast<void*>(address),&info) && info.dli_fname &&
        uintptr_t(info.dli_fbase)==m.base && !std::strcmp(info.dli_fname,m.path);
}
bool target_elf(const char* path) {
    FILE* file=std::fopen(path,"rb");if(!file)return false;
    Elf32_Ehdr header;
    const bool read=std::fread(&header,1,sizeof header,file)==sizeof header;
    std::fclose(file);
    return read && !std::memcmp(header.e_ident,ELFMAG,SELFMAG) &&
        header.e_ident[EI_CLASS]==ELFCLASS32 && header.e_ident[EI_DATA]==ELFDATA2LSB &&
        header.e_machine==EM_ARM && header.e_type==ET_DYN && header.e_flags==0x05000002;
}
A::InstallResult check_modules(const A::LdsInstallOptions& in,Module* out,size_t page) {
    for(unsigned i=0;i<MODULE_COUNT;++i) {
        void* entry=dlsym(in.service_handle,modules[i].anchor);
        Dl_info info=Dl_info();
        if(!entry || !dladdr(entry,&info) || !info.dli_fbase || !info.dli_fname)
            return A::MODULE_MISMATCH;
        out[i].base=uintptr_t(info.dli_fbase);out[i].path=info.dli_fname;
        if(!target_elf(info.dli_fname) || !in.verify_file_hash(info.dli_fname,modules[i].hash))
            return A::FILE_IDENTITY_MISMATCH;
    }
    for(unsigned i=0;i<sizeof functions/sizeof functions[0];++i) {
        const FunctionSpec& f=functions[i];const Module& m=out[f.owner];
        const uintptr_t address=m.base+f.offset;
        if(!owner(m,address) || !range(m,address,sizeof f.prefix,PF_R|PF_X,page) ||
           (f.name && dlsym(in.service_handle,f.name)!=reinterpret_cast<void*>(address)))
            return A::MODULE_MISMATCH;
        if(std::memcmp(reinterpret_cast<void*>(address),f.prefix,sizeof f.prefix))
            return A::ORIGINAL_BYTES_MISMATCH;
    }
    return A::INSTALL_OK;
}
A::InstallResult check_context(const Module* m,size_t page) {
    const Module& l=m[LDS];
    if(!range(l,l.base+0x16860,48,PF_R|PF_W,page) ||
       !range(l,l.base+0x16680,24,PF_R|PF_W,page) ||
       !range(l,l.base+0x16538,sizeof(A::LdsDescriptor),PF_R|PF_W,page))
        return A::MODULE_MISMATCH;
    const A::LdsDescriptor& d=*reinterpret_cast<const A::LdsDescriptor*>(l.base+0x16538);
    if(!range(l,uintptr_t(d.name),12,PF_R|PF_X,page) ||
       std::memcmp(d.name,"GetPosition",12) ||
       uintptr_t(d.service)!=l.base+0x478c || uintptr_t(d.generic)!=l.base+0x735c ||
       d.reserved)return A::ORIGINAL_BYTES_MISMATCH;
    const uintptr_t cache_literals[]={0x43e0,0x48a4,0x54a4,0x57d4};
    for(unsigned i=0;i<sizeof cache_literals/sizeof cache_literals[0];++i) {
        const uintptr_t address=l.base+cache_literals[i];
        if(!range(l,address,4,PF_R|PF_X,page))return A::MODULE_MISMATCH;
        if(*reinterpret_cast<const uintptr_t*>(address)!=l.base+0x16860)
            return A::NEXT_CHAIN_MISMATCH;
    }
    struct Call {ModuleId owner;uintptr_t address;uint32_t word;};
    const Call calls[]={
        {LDS,0x5440,0xebfff878},{LDS,0x5450,0xebfff805},{LDS,0x5458,0xebfff81e},
        {LDS,0x5768,0xebfff7ae},{LDS,0x5778,0xebfff73b},{LDS,0x5780,0xebfff754},
        {LDS,0x47fc,0xebfffb89},{LDS,0x4894,0xebfffb0f},
        {SERVICE,0x41e8,0xebfffcf1},{SERVICE,0x4380,0xebfffc1f},
        {SERVICE,0x4020,0xebfffd63},{SERVICE,0x40fc,0xebfffcc0},
        {SERVICE,0x3e64,0xebfffdd2},{SERVICE,0x3f74,0xebfffd22}
    };
    for(unsigned i=0;i<sizeof calls/sizeof calls[0];++i) {
        const Module& mod=m[calls[i].owner];const uintptr_t address=mod.base+calls[i].address;
        if(!range(mod,address,4,PF_R|PF_X,page))return A::MODULE_MISMATCH;
        if(*reinterpret_cast<const uint32_t*>(address)!=calls[i].word)
            return A::ORIGINAL_BYTES_MISMATCH;
    }
    // Register captures the raw pointer while the original conn owns it.
    const uint32_t register_call[]={0xe5932268,0xe24b3024,0xe1a00002,0xe1a01003,0xebfffdb3};
    const uintptr_t call=m[BUS].base+0x6b44;
    if(!range(m[BUS],call,sizeof register_call,PF_R|PF_X,page))return A::MODULE_MISMATCH;
    if(std::memcmp(reinterpret_cast<void*>(call),register_call,sizeof register_call))
        return A::ORIGINAL_BYTES_MISMATCH;
    return A::INSTALL_OK;
}
A::InstallResult make_plan(const Module* m,size_t page,D::Plan& plan) {
    struct Slot {ModuleId owner;uintptr_t offset;ModuleId next;uintptr_t entry,replacement;};
    // Connection/reply support precedes LDS service entry publication. Every
    // wrapper forwards through immutable bindings even before activation.
    const Slot slots[]={
        {BUS,0x346d0,BUS,0x20484,reinterpret_cast<uintptr_t>(&mx5_lds_path)},
        {BUS,0x34240,BUS,0x2098c,reinterpret_cast<uintptr_t>(&mx5_bus_signal)},
        {BUS,0x34274,BUS,0xb930,reinterpret_cast<uintptr_t>(&mx5_bus_free)},
        {BUS,0x34304,RAW,0xb82c,reinterpret_cast<uintptr_t>(&mx5_lds_send)},
        {BUS,0x343d4,BUS,0xff34,reinterpret_cast<uintptr_t>(&mx5_lds_reply_message)},
        {BUS,0x34400,BUS,0xb360,reinterpret_cast<uintptr_t>(&mx5_bus_connect)},
        {BUS,0x34474,BUS,0x10380,reinterpret_cast<uintptr_t>(&mx5_lds_reply_create)},
        {BUS,0x34560,BUS,0xac54,reinterpret_cast<uintptr_t>(&mx5_bus_disconnect)},
        {BUS,0x345ec,BUS,0xb8c4,reinterpret_cast<uintptr_t>(&mx5_bus_create)},
        {BUS,0x34604,RAW,0x89ac,reinterpret_cast<uintptr_t>(&mx5_bus_register)},
        {BUS,0x34654,BUS,0x19ac8,reinterpret_cast<uintptr_t>(&mx5_lds_method_build)},
        {LDS,0x16358,BUS,0xb930,reinterpret_cast<uintptr_t>(&mx5_bus_free)},
        {LDS,0x16364,COMMON,0xce20,reinterpret_cast<uintptr_t>(&mx5_lds_mem_copy)},
        {LDS,0x16388,SERVICE,0x7b48,reinterpret_cast<uintptr_t>(&mx5_lds_mutex_unlock)},
        {LDS,0x163e8,BUS,0xb360,reinterpret_cast<uintptr_t>(&mx5_bus_connect)},
        {LDS,0x163f8,SERVICE,0x7aa0,reinterpret_cast<uintptr_t>(&mx5_lds_mutex_lock)},
        {LDS,0x16418,BUS,0x1fe3c,reinterpret_cast<uintptr_t>(&mx5_lds_set_callback)},
        {LDS,0x16498,BUS,0xac54,reinterpret_cast<uintptr_t>(&mx5_bus_disconnect)},
        {LDS,0x164b8,BUS,0xb8c4,reinterpret_cast<uintptr_t>(&mx5_bus_create)},
        {SERVICE,0x1459c,LDS,0x574c,reinterpret_cast<uintptr_t>(&mx5_lds_update)},
        {SERVICE,0x145b4,DRIVER,0x4e6c,reinterpret_cast<uintptr_t>(&mx5_lds_driver_open)},
        {SERVICE,0x145d8,LDS,0x4460,reinterpret_cast<uintptr_t>(&mx5_lds_clear)},
        {SERVICE,0x1460c,DRIVER,0x5c24,reinterpret_cast<uintptr_t>(&mx5_lds_register)},
        {SERVICE,0x1462c,LDS,0x5424,reinterpret_cast<uintptr_t>(&mx5_lds_read)},
        {SERVICE,0x14634,LDS,0x413c,reinterpret_cast<uintptr_t>(&mx5_lds_initialize)},
    };
    static_assert(sizeof slots/sizeof slots[0]<=D::Plan::SLOT_CAPACITY,"slot capacity");
    for(unsigned i=0;i<sizeof slots/sizeof slots[0];++i) {
        const Slot& s=slots[i];
        const D::Slot slot={m[s.owner].base+s.offset,m[s.next].base+s.entry,s.replacement};
        if(!range(m[s.owner],slot.address,sizeof(uintptr_t),PF_R|PF_W,page))
            return A::MODULE_MISMATCH;
        if(__atomic_load_n(reinterpret_cast<uintptr_t*>(slot.address),__ATOMIC_ACQUIRE)!=slot.expected)
            return A::NEXT_CHAIN_MISMATCH;
        plan.slots[plan.slot_count++]=slot;
    }
    return A::INSTALL_OK;
}
struct Retained {
    void* handle;bool keep;
    Retained():handle(0),keep(false) {}
    ~Retained() { if(handle && !keep)dlclose(handle); }
};
struct Setup { A::LdsBindings lds;A::BusBindings bus;Retained* retained; };
template<class F> F function(const Module* m,ModuleId id,uintptr_t offset) {
    return reinterpret_cast<F>(m[id].base+offset);
}
void bindings(const A::LdsInstallOptions& in,const Module* m,Setup& s) {
    A::LdsBindings& b=s.lds;
#define LDS_FUNCTION(field,id,offset) b.field=function<decltype(b.field)>(m,id,offset)
    LDS_FUNCTION(initialize,LDS,0x413c);LDS_FUNCTION(clear,LDS,0x4460);
    LDS_FUNCTION(driver_open,DRIVER,0x4e6c);LDS_FUNCTION(registration,DRIVER,0x5c24);
    LDS_FUNCTION(read,LDS,0x5424);LDS_FUNCTION(update,LDS,0x574c);
    LDS_FUNCTION(lock,SERVICE,0x7aa0);LDS_FUNCTION(unlock,SERVICE,0x7b48);
    LDS_FUNCTION(copy,COMMON,0xce20);LDS_FUNCTION(set_callback,BUS,0x1fe3c);
    LDS_FUNCTION(generic,LDS,0x735c);LDS_FUNCTION(service,LDS,0x478c);
    LDS_FUNCTION(path,BUS,0x20484);LDS_FUNCTION(method_build,BUS,0x19ac8);
    LDS_FUNCTION(reply_create,BUS,0x10380);LDS_FUNCTION(reply_message,BUS,0xff34);
    LDS_FUNCTION(send,RAW,0xb82c);
    LDS_FUNCTION(raw.type,RAW,0x1353c);LDS_FUNCTION(raw.serial,RAW,0x13298);
    LDS_FUNCTION(raw.reply_serial,RAW,0x132c8);LDS_FUNCTION(raw.sender,RAW,0x14344);
    LDS_FUNCTION(raw.destination,RAW,0x14304);LDS_FUNCTION(raw.path,RAW,0x14078);
    LDS_FUNCTION(raw.interface_name,RAW,0x14144);LDS_FUNCTION(raw.member,RAW,0x141c8);
#undef LDS_FUNCTION
    const uintptr_t l=m[LDS].base,svc=m[SERVICE].base;
    b.descriptor=reinterpret_cast<const A::LdsDescriptor*>(l+0x16538);
    b.current_cache=reinterpret_cast<void*>(l+0x16860);
    b.current_mutex=reinterpret_cast<void*>(l+0x16680);
    b.routes[0]=A::LdsRoute{reinterpret_cast<A::LdsCallback>(svc+0x41d4),0x06f,svc+0x41ec,svc+0x4384};
    b.routes[2]=A::LdsRoute{reinterpret_cast<A::LdsCallback>(svc+0x400c),0x181,svc+0x4024,svc+0x4100};
    b.routes[4]=A::LdsRoute{reinterpret_cast<A::LdsCallback>(svc+0x3e50),0x01d,svc+0x3e68,svc+0x3f78};
    // lds_startOperation registers the same three handlers under ten IDs.
    // Each ID still gets its own immutable forwarding wrapper in lds_hooks.
    b.routes[1]=b.routes[0];b.routes[6]=b.routes[0];b.routes[7]=b.routes[0];
    b.routes[3]=b.routes[2];b.routes[5]=b.routes[2];b.routes[9]=b.routes[2];
    b.routes[8]=b.routes[4];
    b.sites=A::LdsCacheSites{l+0x5444,l+0x5454,l+0x545c,l+0x576c,l+0x577c,l+0x5784,l+0x4800,l+0x4898};
    b.clock=in.clock;b.emit=in.emit;b.user=in.user;
    A::BusBindings& bus=s.bus;
#define BUS_FUNCTION(field,id,offset) bus.field=function<decltype(bus.field)>(m,id,offset)
    BUS_FUNCTION(create,BUS,0xb8c4);BUS_FUNCTION(connect,BUS,0xb360);
    BUS_FUNCTION(disconnect,BUS,0xac54);BUS_FUNCTION(free,BUS,0xb930);
    BUS_FUNCTION(signal,BUS,0x2098c);BUS_FUNCTION(is_signal,RAW,0x1459c);
    BUS_FUNCTION(endpoint.registration,RAW,0x89ac);
    BUS_FUNCTION(endpoint.get_server_id,RAW,0xa448);
    BUS_FUNCTION(endpoint.get_unique_name,RAW,0x90a8);BUS_FUNCTION(endpoint.free_guid,RAW,0x2021c);
#undef BUS_FUNCTION
    bus.endpoint.register_caller=m[BUS].base+0x6b58;
}
int protect(void* address,size_t size,int flags,void*) {
    return mprotect(address,size,flags);
}
bool prepare(void* user) {
    Setup& s=*static_cast<Setup*>(user);
    // Even a partly prepared binding object must keep all its original targets.
    s.retained->keep=true;
    // This DSO runs only inside LDS. No AA prediction state exists here to
    // revoke; bus_hooks retains its own connection/version invalidation.
    return A::prepare_bus_hooks(s.bus,0) && A::prepare_lds_hooks(s.lds);
}
bool installed;
}
#endif

namespace mx5 { namespace adapter {
InstallResult install_lds_v74(const LdsInstallOptions& in) {
    struct Errno {int value;~Errno(){errno=value;}} saved={errno};
#if defined(__arm__) && !defined(__ARM_PCS_VFP) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    if(installed)return ALREADY_INSTALLED;
    if(!in.service_handle || !in.verify_file_hash || !in.verified_cold_start ||
       !in.begin_patch || !in.end_patch || !in.clock || !in.emit)
        return INVALID_INSTALL_ARGUMENT;
    const long page=sysconf(_SC_PAGESIZE);
    if(page<=0 || (page&(page-1)))return MEMORY_PROTECTION_FAILED;
    Module m[MODULE_COUNT]={};
    InstallResult result=check_modules(in,m,size_t(page));
    if(result!=INSTALL_OK)return result;
    result=check_context(m,size_t(page));if(result!=INSTALL_OK)return result;
    data_patch::Plan plan=data_patch::Plan();
    result=make_plan(m,size_t(page),plan);if(result!=INSTALL_OK)return result;
    Retained retained;
    // Bypass this DSO's own interposer: a nested target NOLOAD through it would
    // cancel its cold lease. RTLD_NEXT still preserves later shims.
    typedef void* (*Dlopen)(const char*,int);
    Dlopen next=reinterpret_cast<Dlopen>(dlsym(RTLD_NEXT,"dlopen"));
    if(!next)return MODULE_MISMATCH;
    retained.handle=next(m[SERVICE].path,RTLD_LAZY|RTLD_NOLOAD|RTLD_LOCAL);
    if(!retained.handle ||
       dlsym(retained.handle,"GetServiceInterfaces")!=reinterpret_cast<void*>(m[SERVICE].base+0x5f6c))
        return MODULE_MISMATCH;
    Setup setup={LdsBindings(),BusBindings(),&retained};
    bindings(in,m,setup);
    // No loader, file, initialization, hardware or user observer calls in lease.
    if(!in.begin_patch())return COLD_START_LOST;
    struct Lease {void (*end)(bool);~Lease(){end(true);}} lease={in.end_patch};
    const data_patch::Ops ops={&setup,protect,prepare,data_patch::native_exchange};
    const data_patch::Result applied=data_patch::apply(plan,size_t(page),ops);
    if(applied.status!=INSTALL_OK)return applied.status;
    if(!activate_lds_hooks())return CONFIGURATION_FAILED;
    installed=true;
    return INSTALL_OK;
#else
    (void)in;return UNSUPPORTED_ARCH;
#endif
}
} }
