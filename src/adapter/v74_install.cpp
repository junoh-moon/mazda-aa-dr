#include "adapter.h"
#include "cold_patch.h"
#include "request_hooks.h"
#include "session_hooks.h"
#include "bus_hooks.h"
#include "install_policy.h"
#include <cstring>

#if defined(__arm__) && !defined(__ARM_PCS_VFP) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#include <cstdio>
#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <sys/mman.h>
#include <unistd.h>

extern "C" void mx5_position_veneer();
extern "C" {
__attribute__((visibility("hidden"))) void* mx5_position_trampoline = 0;
}

namespace {
namespace A=mx5::adapter;
namespace C=A::cold_patch;
void copy_safe(char* out,size_t capacity,const char* in) {
    size_t n=0;
    for(;in&&in[n]&&n+1<capacity;++n) {
        const char c=in[n];
        out[n]=((c>='0'&&c<='9')||(c>='A'&&c<='Z')||(c>='a'&&c<='z')||c=='.'||c=='_'||c=='/'||c=='-'||c=='+')?c:'_';
    }
    out[n]=0;
}
// Diagnostic only; the caller still returns NEXT_CHAIN_MISMATCH. Remembers the first
// failing slot comparison and the module that owns the value found there.
A::InstallResult mismatch(const A::InstallOptions& in,unsigned stage,uintptr_t address,uintptr_t expected) {
    A::InstallReport* r=in.report;
    if(r&&!r->declined_stage) {
        const uintptr_t observed=*reinterpret_cast<uintptr_t*>(address);
        r->declined_stage=stage;
        r->slot_offset=address-in.blm_load_bias;
        r->slot_expected_offset=expected-in.interface_load_bias;
        Dl_info info;
        if(dladdr(reinterpret_cast<void*>(observed),&info)) {
            copy_safe(r->owner,sizeof r->owner,info.dli_fname);
            copy_safe(r->symbol,sizeof r->symbol,info.dli_sname);
            r->observed_offset=observed-reinterpret_cast<uintptr_t>(info.dli_fbase);
        }
    }
    return A::NEXT_CHAIN_MISMATCH;
}
const uintptr_t kRequest = 0xc7460, kSendSlot = 0xf88bc, kSendExport = 0x1a538;
const char* const kBlmHash = "10e7235bfce075b44c1a8ffc99bbc9b63af85d9262df868ca1874ec36d8d3b71";
const char* const kInterfaceHash = "e9eb5e0d42719c98efc5ef86a270b4b3c86467aced55d4c8158bd99e06bbd436";
const uint8_t kPrologue[16] = {
    0x10,0x48,0x2d,0xe9, 0x08,0xb0,0x8d,0xe2,
    0x1c,0xd0,0x4d,0xe2, 0x00,0x44,0x9f,0xe5
};
const uint8_t kExportPrologue[16] = {
    0x70,0x40,0x2d,0xe9, 0xac,0x42,0x9f,0xe5,
    0xac,0xc2,0x9f,0xe5, 0x04,0x40,0x8f,0xe0
};
bool installed = false;
bool target_elf(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    Elf32_Ehdr h;
    const bool read = std::fread(&h, 1, sizeof h, f) == sizeof h;
    std::fclose(f);
    return read && !std::memcmp(h.e_ident, ELFMAG, SELFMAG) &&
        h.e_ident[EI_CLASS] == ELFCLASS32 && h.e_ident[EI_DATA] == ELFDATA2LSB &&
        h.e_machine == EM_ARM && h.e_type == ET_DYN && h.e_flags == 0x05000002;
}
struct FindSegment {
    uintptr_t base, address;
    size_t size;
    int permissions;
    bool found;
};
int find_segment(dl_phdr_info* info, size_t, void* arg) {
    FindSegment& f = *static_cast<FindSegment*>(arg);
    if (uintptr_t(info->dlpi_addr) != f.base) return 0;
    for (unsigned i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr)& p = info->dlpi_phdr[i];
        const uintptr_t start = f.base + p.p_vaddr, end = start + p.p_memsz;
        if (p.p_type != PT_LOAD || f.address < start || f.address > end ||
            f.size > end - f.address) continue;
        f.permissions = ((p.p_flags & PF_R) ? PROT_READ : 0) |
                        ((p.p_flags & PF_W) ? PROT_WRITE : 0) |
                        ((p.p_flags & PF_X) ? PROT_EXEC : 0);
        f.found = true; return 1;
    }
    return 0;
}
bool segment(uintptr_t base, uintptr_t address, size_t size, int expected) {
    FindSegment f = {base,address,size,0,false};
    dl_iterate_phdr(find_segment, &f);
    return f.found && f.permissions == expected;
}
bool matches_module(uintptr_t address, uintptr_t bias, const char* path) {
    Dl_info info = Dl_info();
    return dladdr(reinterpret_cast<void*>(address), &info) &&
        reinterpret_cast<uintptr_t>(info.dli_fbase) == bias && info.dli_fname &&
        std::strcmp(info.dli_fname, path) == 0;
}
const uint8_t kPost[16]={0x10,0x48,0x2d,0xe9,0x08,0xb0,0x8d,0xe2,0x2c,0xd0,0x4d,0xe2,0x30,0x00,0x0b,0xe5};
const uint8_t kWork[16]={0x00,0x48,0x2d,0xe9,0x04,0xb0,0x8d,0xe2,0x08,0xd0,0x4d,0xe2,0x08,0x00,0x0b,0xe5};
const uint8_t kNotify[16]={0xf0,0x41,0x2d,0xe9,0x00,0x30,0xa0,0xe3,0x88,0xd0,0x4d,0xe2,0x00,0x40,0xa0,0xe3};
struct ApiEntry { const char* name; uintptr_t offset; uint32_t words[4]; };
const ApiEntry kBusApi[]={
    {"JCIDBUS_method_get_reply",0x19778,{0xe52db004,0xe28db000,0xe24dd00c,0xe50b0008}},
    {"JCIDBUS_reply_get_type",0x10b20,{0xe52db004,0xe28db000,0xe24dd014,0xe50b0010}},
    {"JCIDBUS_reply_get_sender",0x10c10,{0xe52db004,0xe28db000,0xe24dd00c,0xe50b0008}},
    {"JCIDBUS_reply_get_error",0x10528,{0xe52db004,0xe28db000,0xe24dd00c,0xe50b0008}},
    {"JCIDBUS_reply_get_msg_serial",0x1041c,{0xe92d4800,0xe28db004,0xe24dd010,0xe50b0010}},
    {"JCIDBUS_method_get_destination",0x19ed4,{0xe52db004,0xe28db000,0xe24dd00c,0xe50b0008}},
    {"JCIDBUS_method_get_path",0x19e98,{0xe52db004,0xe28db000,0xe24dd00c,0xe50b0008}},
    {"JCIDBUS_method_get_interface",0x19e20,{0xe52db004,0xe28db000,0xe24dd00c,0xe50b0008}},
    {"JCIDBUS_method_get_name",0x19e5c,{0xe52db004,0xe28db000,0xe24dd00c,0xe50b0008}},
    {"JCIDBUS_method_send_async_with_notify",0x1a09c,{0xe92d4800,0xe28db004,0xe24dd038,0xe50b0020}},
    {"JCIDBUS_method_free",0x19494,{0xe92d4800,0xe28db004,0xe24dd010,0xe50b0010}},
    {"JCIDBUS_free_method_only",0x195e8,{0xe92d4800,0xe28db004,0xe24dd010,0xe50b0010}},
    {"JCIDBUS_conn_create",0xb8c4,{0xe92d4800,0xe28db004,0xe24dd018,0xe50b0018}},
    {"JCIDBUS_conn_connect",0xb360,{0xe92d4800,0xe28db004,0xe24dd028,0xe50b0010}},
    {"JCIDBUS_conn_disconnect",0xac54,{0xe92d4800,0xe28db004,0xe24dd040,0xe50b0030}},
    {"JCIDBUS_conn_free",0xb930,{0xe92d4800,0xe28db004,0xe24dd020,0xe50b0010}},
    {"JCIDBUS_signal_handler",0x2098c,{0xe92d4800,0xe28db004,0xe24dd038,0xe50b0028}},
    {"create_method_msg",0x19ff0,{0xe92d4800,0xe28db004,0xe24dd010,0xe50b0010}},
    {"JCIDBUS_pending_msg_handler",0xeebc,{0xe92d4800,0xe28db004,0xe24dd048,0xe50b0038}}
};
struct RawApiEntry { const char* name; uintptr_t offset; size_t size; uint32_t words[4]; };
const RawApiEntry kRawApi[]={
    {"dbus_connection_send_with_reply",0xb8bc,16,{0xe92d45f8,0xe2526000,0xe1a07003,0x13a03000}},
    {"dbus_pending_call_steal_reply",0x166d8,16,{0xe92d4038,0xe1a04000,0xe5900010,0xebffcd39}},
    {"dbus_message_get_serial",0x13298,8,{0xe2800004,0xeaffede7,0,0}},
    {"dbus_message_get_reply_serial",0x132c8,16,{0xe52de004,0xe3a01005,0xe24dd00c,0xe3a02075}},
    {"dbus_message_get_type",0x1353c,8,{0xe2800004,0xeaffed31,0,0}},
    {"dbus_message_get_sender",0x14344,16,{0xe52de004,0xe3a02000,0xe24dd00c,0xe3a01007}},
    {"dbus_message_get_error_name",0x142c4,16,{0xe52de004,0xe3a02000,0xe24dd00c,0xe3a01004}},
    {"dbus_bus_register",0x89ac,16,{0xe92d47f0,0xe59f4178,0xe59f3178,0xe08f4004}},
    {"dbus_connection_get_server_id",0xa448,16,{0xe92d4038,0xe1a04000,0xe5900004,0xeb004115}},
    {"dbus_bus_get_unique_name",0x90a8,16,{0xe92d4038,0xe59f3038,0xe08f3003,0xe59f2034}},
    {"dbus_free",0x2021c,12,{0xe3500000,0x012fff1e,0xeaff93a6,0}}
};
A::InstallResult request_plan(const A::InstallOptions& in,C::Plan& plan,A::RequestBindings& bindings,A::BusBindings& connection) {
    if(!in.blm_handle)return A::INVALID_INSTALL_ARGUMENT;
    void* bus_entry=dlsym(in.blm_handle,kBusApi[0].name);
    void* data_entry=dlsym(in.blm_handle,"LDS_DATA_GetPosition");
    Dl_info bus=Dl_info(),data=Dl_info();
    if(!bus_entry || !data_entry || !dladdr(bus_entry,&bus) || !dladdr(data_entry,&data) ||
       !bus.dli_fbase || !data.dli_fbase || !bus.dli_fname || !data.dli_fname)return A::MODULE_MISMATCH;
    const uintptr_t bb=reinterpret_cast<uintptr_t>(bus.dli_fbase),db=reinterpret_cast<uintptr_t>(data.dli_fbase);
    if(!target_elf(bus.dli_fname) || !target_elf(data.dli_fname) ||
       !in.verify_file_hash(bus.dli_fname,"b44b2f462c09376747a380ee3010501e952f557759898fad01a0fdcd573d375f") ||
       !in.verify_file_hash(data.dli_fname,"bd4039da18c4039ba8d901ae49e917c9e7d22357d42ea930b43253eb55b8fbea"))
        return A::FILE_IDENTITY_MISMATCH;
    for(unsigned i=0;i<sizeof kBusApi/sizeof kBusApi[0];++i) {
        const ApiEntry& e=kBusApi[i];const uintptr_t address=bb+e.offset;
        if(dlsym(in.blm_handle,e.name)!=reinterpret_cast<void*>(address) ||
           !matches_module(address,bb,bus.dli_fname) || !segment(bb,address,16,PROT_READ|PROT_EXEC))
            return A::MODULE_MISMATCH;
        if(std::memcmp(reinterpret_cast<void*>(address),e.words,16))return A::ORIGINAL_BYTES_MISMATCH;
    }
    void* predicate=dlsym(in.blm_handle,"dbus_message_is_signal");Dl_info dbus=Dl_info();
    if(!predicate || !dladdr(predicate,&dbus) || !dbus.dli_fbase || !dbus.dli_fname)
        return A::MODULE_MISMATCH;
    const uintptr_t lb=reinterpret_cast<uintptr_t>(dbus.dli_fbase),pa=lb+0x1459c;
    if(!target_elf(dbus.dli_fname) ||
       !in.verify_file_hash(dbus.dli_fname,"07b06516d6ba93bbfa9db1e817278c7c96bd86fdd0f9dc48c917b1c100a8fe6b"))
        return A::FILE_IDENTITY_MISMATCH;
    if(predicate!=reinterpret_cast<void*>(pa) || !matches_module(pa,lb,dbus.dli_fname) ||
       !segment(lb,pa,16,PROT_READ|PROT_EXEC))return A::MODULE_MISMATCH;
    const uint32_t predicate_bytes[]={0xe1a0c001,0xe1a03002,0xe3a01004,0xe1a0200c};
    if(std::memcmp(predicate,predicate_bytes,16))return A::ORIGINAL_BYTES_MISMATCH;
    for(unsigned i=0;i<sizeof kRawApi/sizeof kRawApi[0];++i) {
        const RawApiEntry& e=kRawApi[i];const uintptr_t address=lb+e.offset;
        if(dlsym(in.blm_handle,e.name)!=reinterpret_cast<void*>(address) ||
           !matches_module(address,lb,dbus.dli_fname) ||
           !segment(lb,address,e.size,PROT_READ|PROT_EXEC))return A::MODULE_MISMATCH;
        if(std::memcmp(reinterpret_cast<void*>(address),e.words,e.size))return A::ORIGINAL_BYTES_MISMATCH;
    }
    // This exact call owns conn+0x298 and reads its live raw pointer at +0x268.
    // It is not safe to inspect that field later after outer connect returns.
    const uint32_t register_call[]={0xe5932268,0xe24b3024,0xe1a00002,0xe1a01003,0xebfffdb3};
    if(!matches_module(bb+0x6b44,bb,bus.dli_fname) ||
       !segment(bb,bb+0x6b44,sizeof register_call,PROT_READ|PROT_EXEC))return A::MODULE_MISMATCH;
    if(std::memcmp(reinterpret_cast<void*>(bb+0x6b44),register_call,sizeof register_call))
        return A::ORIGINAL_BYTES_MISMATCH;
    if(data_entry!=reinterpret_cast<void*>(db+0x27e0) ||
       !segment(db,db+0x2228,16,PROT_READ|PROT_EXEC) ||
       std::memcmp(reinterpret_cast<void*>(db+0x2228),kNotify,16))return A::ORIGINAL_BYTES_MISMATCH;
    const uintptr_t blm=in.blm_load_bias;
    const uintptr_t offsets[]={0x6f4b0,0xc9958,0xca308};
    const uint8_t* bytes[]={kPost,kWork,kWork};
    const uintptr_t wrappers[]={reinterpret_cast<uintptr_t>(&mx5_request_post_veneer),reinterpret_cast<uintptr_t>(&mx5_request_work_veneer),reinterpret_cast<uintptr_t>(&mx5_request_destroy_veneer)};
    for(unsigned i=0;i<3;++i) {
        const uintptr_t address=blm+offsets[i];
        // These BLM functions are local ELF symbols, not dlsym exports. Their
        // addresses/ABI are pinned by the whole-file hash and live byte checks.
        if(!matches_module(address,blm,in.blm_path) ||
           !segment(blm,address,16,PROT_READ|PROT_EXEC))return A::MODULE_MISMATCH;
        if(std::memcmp(reinterpret_cast<void*>(address),bytes[i],16))return A::ORIGINAL_BYTES_MISMATCH;
        const C::Entry entry={address,bytes[i],wrappers[i]};plan.entries[plan.entry_count++]=entry;
    }
    if(!matches_module(blm+0xf7138,blm,in.blm_path) ||
       !segment(blm,blm+0xf7138,36,PROT_READ|PROT_WRITE))return A::MODULE_MISMATCH;
    // Cleanup is reachable before submit; no callback is replaced until all
    // code patches are executable and every original target is initialized.
    const C::Slot send=plan.slots[0];
    const C::Slot slots[]={
        {bb+0x34364,bb+0x19494,reinterpret_cast<uintptr_t>(&mx5_request_free)},
        {bb+0x34350,bb+0x195e8,reinterpret_cast<uintptr_t>(&mx5_request_free_only)},
        {db+0xc170,bb+0x19494,reinterpret_cast<uintptr_t>(&mx5_request_free)},
        send,
        {db+0xc188,bb+0x1a09c,reinterpret_cast<uintptr_t>(&mx5_request_submit)}
    };
    for(unsigned i=0;i<5;++i) {
        const C::Slot& s=slots[i];const uintptr_t base=i<2?bb:(i==3?blm:db);
        if(!segment(base,s.address,4,PROT_READ|PROT_WRITE))return A::MODULE_MISMATCH;
        if(*reinterpret_cast<uintptr_t*>(s.address)!=s.expected)return mismatch(in,2,s.address,s.expected);
        plan.slots[i]=s;
    }
    plan.slot_count=5;
    // Install bus cleanup before connect/create, then request submit. The
    // separately prepared session slots are appended by session_plan below.
    // Only GOT data is changed: libjcidbus may already have running callers.
    const C::Slot submit=plan.slots[--plan.slot_count];
    const C::Slot signal={bb+0x34240,bb+0x2098c,reinterpret_cast<uintptr_t>(&mx5_bus_signal)};
    if(!segment(bb,signal.address,4,PROT_READ|PROT_WRITE))return A::MODULE_MISMATCH;
    if(*reinterpret_cast<uintptr_t*>(signal.address)!=signal.expected)return mismatch(in,2,signal.address,signal.expected);
    plan.slots[plan.slot_count++]=signal;
    // Registration support must be reachable before either connect owner and
    // submit. It participates in the same preflight/prepare/publication plan.
    const C::Slot registration={bb+0x34604,lb+0x89ac,reinterpret_cast<uintptr_t>(&mx5_bus_register)};
    if(!segment(bb,registration.address,4,PROT_READ|PROT_WRITE))return A::MODULE_MISMATCH;
    if(*reinterpret_cast<uintptr_t*>(registration.address)!=registration.expected)return mismatch(in,2,registration.address,registration.expected);
    plan.slots[plan.slot_count++]=registration;
    const uintptr_t bus_slots[]={0x34560,0x34274,0x34400,0x345ec};
    const uintptr_t blm_slots[]={0xf7710,0xf7ce8,0xf8b84,0xf81a0};
    const uintptr_t targets[]={0xac54,0xb930,0xb360,0xb8c4};
    const uintptr_t replacements[]={reinterpret_cast<uintptr_t>(&mx5_bus_disconnect),
        reinterpret_cast<uintptr_t>(&mx5_bus_free),reinterpret_cast<uintptr_t>(&mx5_bus_connect),
        reinterpret_cast<uintptr_t>(&mx5_bus_create)};
    for(unsigned i=0;i<4;++i)for(unsigned owner=0;owner<2;++owner) {
        const uintptr_t base=owner?blm:bb;
        const C::Slot s={base+(owner?blm_slots[i]:bus_slots[i]),bb+targets[i],replacements[i]};
        if(!segment(base,s.address,4,PROT_READ|PROT_WRITE))return A::MODULE_MISMATCH;
        if(*reinterpret_cast<uintptr_t*>(s.address)!=s.expected)return mismatch(in,2,s.address,s.expected);
        plan.slots[plan.slot_count++]=s;
    }
    // Observe raw headers using data pointers only. JCIDBUS's notify callback
    // pointer is a GLOB_DAT relocation, not a patch to running handler code.
    // All support is reachable before the LDS submit wrapper is published.
    const C::Slot wire_slots[]={
        {bb+0x3449c,lb+0x166d8,reinterpret_cast<uintptr_t>(&mx5_request_steal)},
        {bb+0x346ec,bb+0xeebc,reinterpret_cast<uintptr_t>(&mx5_request_pending)},
        {bb+0x344dc,lb+0xb8bc,reinterpret_cast<uintptr_t>(&mx5_request_wire_send)},
        {bb+0x34230,bb+0x19ff0,reinterpret_cast<uintptr_t>(&mx5_request_message)}
    };
    for(unsigned i=0;i<4;++i) {
        const C::Slot& s=wire_slots[i];
        if(!segment(bb,s.address,4,PROT_READ|PROT_WRITE))return A::MODULE_MISMATCH;
        if(*reinterpret_cast<uintptr_t*>(s.address)!=s.expected)return mismatch(in,2,s.address,s.expected);
        plan.slots[plan.slot_count++]=s;
    }
    plan.slots[plan.slot_count++]=submit;
    connection.create=reinterpret_cast<A::BusCreate>(bb+0xb8c4);
    connection.connect=reinterpret_cast<A::BusConnect>(bb+0xb360);
    connection.disconnect=reinterpret_cast<A::BusEnd>(bb+0xac54);
    connection.free=reinterpret_cast<A::BusEnd>(bb+0xb930);
    connection.signal=reinterpret_cast<A::BusSignal>(bb+0x2098c);
    connection.is_signal=reinterpret_cast<A::BusIsSignal>(predicate);
    connection.endpoint.registration=reinterpret_cast<decltype(connection.endpoint.registration)>(lb+0x89ac);
    connection.endpoint.get_server_id=reinterpret_cast<decltype(connection.endpoint.get_server_id)>(lb+0xa448);
    connection.endpoint.get_unique_name=reinterpret_cast<decltype(connection.endpoint.get_unique_name)>(lb+0x90a8);
    connection.endpoint.free_guid=reinterpret_cast<decltype(connection.endpoint.free_guid)>(lb+0x2021c);
    connection.endpoint.register_caller=bb+0x6b58;
    bindings.reply.get_reply=reinterpret_cast<decltype(bindings.reply.get_reply)>(bb+0x19778);
    bindings.reply.get_type=reinterpret_cast<decltype(bindings.reply.get_type)>(bb+0x10b20);
    bindings.reply.get_sender=reinterpret_cast<decltype(bindings.reply.get_sender)>(bb+0x10c10);
    bindings.reply.get_error=reinterpret_cast<decltype(bindings.reply.get_error)>(bb+0x10528);
    bindings.reply.get_reply_serial=reinterpret_cast<decltype(bindings.reply.get_reply_serial)>(bb+0x1041c);
    bindings.method.get_destination=reinterpret_cast<decltype(bindings.method.get_destination)>(bb+0x19ed4);
    bindings.method.get_path=reinterpret_cast<decltype(bindings.method.get_path)>(bb+0x19e98);
    bindings.method.get_interface=reinterpret_cast<decltype(bindings.method.get_interface)>(bb+0x19e20);
    bindings.method.get_name=reinterpret_cast<decltype(bindings.method.get_name)>(bb+0x19e5c);
    bindings.submit=reinterpret_cast<A::RequestSubmit>(bb+0x1a09c);
    bindings.notify=reinterpret_cast<A::RequestNotify>(db+0x2228);
    bindings.free_method=reinterpret_cast<A::RequestFree>(bb+0x19494);
    bindings.free_method_only=reinterpret_cast<A::RequestFree>(bb+0x195e8);
    bindings.position_vptr=blm+0xf7140;
    bindings.wire.build=reinterpret_cast<decltype(bindings.wire.build)>(bb+0x19ff0);
    bindings.wire.pending=reinterpret_cast<decltype(bindings.wire.pending)>(bb+0xeebc);
    bindings.wire.send=reinterpret_cast<decltype(bindings.wire.send)>(lb+0xb8bc);
    bindings.wire.steal=reinterpret_cast<decltype(bindings.wire.steal)>(lb+0x166d8);
    bindings.wire.serial=reinterpret_cast<decltype(bindings.wire.serial)>(lb+0x13298);
    bindings.wire.reply_serial=reinterpret_cast<decltype(bindings.wire.reply_serial)>(lb+0x132c8);
    bindings.wire.type=reinterpret_cast<decltype(bindings.wire.type)>(lb+0x1353c);
    bindings.wire.sender=reinterpret_cast<decltype(bindings.wire.sender)>(lb+0x14344);
    bindings.wire.error=reinterpret_cast<decltype(bindings.wire.error)>(lb+0x142c4);
    return A::INSTALL_OK;
}
A::InstallResult session_plan(const A::InstallOptions& in,C::Plan& plan,A::SessionBindings& bindings) {
    const uintptr_t blm=in.blm_load_bias,api=in.interface_load_bias;
    const ApiEntry entries[]={
        {"aap_create_session",0x1740c,{0xe92d4ff0,0xe59f4aa0,0xe59fcaa0,0xe08f4004}},
        {"aap_destroy_session",0x16e00,{0xe92d40f0,0xe59f452c,0xe59fc52c,0xe08f4004}}
    };
    for(unsigned i=0;i<2;++i) {
        const ApiEntry& e=entries[i];const uintptr_t address=api+e.offset;
        if(dlsym(in.blm_handle,e.name)!=reinterpret_cast<void*>(address) ||
           !matches_module(address,api,in.interface_path) ||
           !segment(api,address,16,PROT_READ|PROT_EXEC))return A::MODULE_MISMATCH;
        if(std::memcmp(reinterpret_cast<void*>(address),e.words,16))return A::ORIGINAL_BYTES_MISMATCH;
    }
    const uint32_t status_bytes[]={0xe92d4810,0xe28db008,0xe24dd034,0xe59f4278};
    if(!matches_module(blm+0x8b128,blm,in.blm_path) ||
       !segment(blm,blm+0x8b128,16,PROT_READ|PROT_EXEC))return A::MODULE_MISMATCH;
    if(std::memcmp(reinterpret_cast<void*>(blm+0x8b128),status_bytes,16))return A::ORIGINAL_BYTES_MISMATCH;
    const C::Slot slots[]={
        {blm+0xf7f6c,api+0x16e00,reinterpret_cast<uintptr_t>(&mx5_session_destroy)},
        {blm+0xf8988,api+0x1740c,reinterpret_cast<uintptr_t>(&mx5_session_create)}
    };
    for(unsigned i=0;i<2;++i) {
        const C::Slot& s=slots[i];
        if(!segment(blm,s.address,4,PROT_READ|PROT_WRITE))return A::MODULE_MISMATCH;
        if(*reinterpret_cast<uintptr_t*>(s.address)!=s.expected)return mismatch(in,3,s.address,s.expected);
        plan.slots[plan.slot_count++]=s; // Destruction is reachable before creation.
    }
    bindings.create=reinterpret_cast<A::SessionCreate>(api+0x1740c);
    bindings.destroy=reinterpret_cast<A::SessionDestroy>(api+0x16e00);
    bindings.status=reinterpret_cast<A::SessionStatus>(blm+0x8b128);
    return A::INSTALL_OK;
}
struct Setup { const A::InstallOptions* options; uintptr_t send; A::RequestBindings bindings; A::SessionBindings session; A::BusBindings bus; bool sessions_enabled; };
int protect(void* p,size_t n,int flags,void*) { return mprotect(p,n,flags); }
void* allocate(size_t n,void*) {
    void* p=mmap(0,n,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    return p==MAP_FAILED?0:p;
}
void release(void* p,size_t n,void*) { munmap(p,n); }
void flush(uintptr_t p,size_t n,void*) {
    __builtin___clear_cache(reinterpret_cast<char*>(p),reinterpret_cast<char*>(p+n));
}
bool prepare(void* tramp,void* user) {
    Setup& setup=*static_cast<Setup*>(user);const A::InstallOptions& in=*setup.options;
    if(!A::configure(reinterpret_cast<A::SendFunction>(setup.send),in.runtime))return false;
    if(in.observe_requests) {
        setup.bindings.post_trampoline=static_cast<char*>(tramp)+16;
        setup.bindings.work_trampoline=static_cast<char*>(tramp)+32;
        setup.bindings.destroy_trampoline=static_cast<char*>(tramp)+48;
        if(!A::prepare_request_hooks(setup.bindings,in.runtime.clock,in.runtime.user))return false;
        if(setup.sessions_enabled&&!A::prepare_session_hooks(setup.session))return false;
        if(!A::prepare_bus_hooks(setup.bus))return false;
    }
    mx5_position_trampoline=tramp;
    return true;
}
}
#endif

namespace mx5 { namespace adapter {
InstallResult install_v74(const InstallOptions& in) {
#if defined(__arm__) && !defined(__ARM_PCS_VFP) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    if (installed) return ALREADY_INSTALLED;
    if (!in.verified_cold_start || !in.verify_file_hash || !in.blm_path ||
        !in.interface_path || !in.blm_load_bias || !in.interface_load_bias ||
        ((in.begin_patch == 0) != (in.end_patch == 0)))
        return INVALID_INSTALL_ARGUMENT;
    if (!target_elf(in.blm_path) || !target_elf(in.interface_path) ||
        !in.verify_file_hash(in.blm_path, kBlmHash) ||
        !in.verify_file_hash(in.interface_path, kInterfaceHash))
        return FILE_IDENTITY_MISMATCH;
    const uintptr_t entry = in.blm_load_bias + kRequest;
    const uintptr_t slot_address = in.blm_load_bias + kSendSlot;
    const uintptr_t expected_next = in.interface_load_bias + kSendExport;
    if (!matches_module(entry, in.blm_load_bias, in.blm_path) ||
        !matches_module(expected_next, in.interface_load_bias, in.interface_path) ||
        !segment(in.blm_load_bias, entry, sizeof kPrologue, PROT_READ|PROT_EXEC) ||
        !segment(in.blm_load_bias, slot_address, 4, PROT_READ|PROT_WRITE) ||
        !segment(in.interface_load_bias, expected_next, 16, PROT_READ|PROT_EXEC))
        return MODULE_MISMATCH;
    if (std::memcmp(reinterpret_cast<void*>(entry), kPrologue, sizeof kPrologue) ||
        std::memcmp(reinterpret_cast<void*>(expected_next), kExportPrologue, sizeof kExportPrologue))
        return ORIGINAL_BYTES_MISMATCH;
    uintptr_t* slot = reinterpret_cast<uintptr_t*>(slot_address);
    // Binding must already be eager (RTLD_NOW or process LD_BIND_NOW=1).
    // A lazy PLT resolver or another shim is intentionally not skipped.
    if (*slot != expected_next) return mismatch(in,1,slot_address,expected_next);
    C::Plan plan=C::Plan();
    const C::Entry position={entry,kPrologue,reinterpret_cast<uintptr_t>(&mx5_position_veneer)};
    const C::Slot send={slot_address,expected_next,reinterpret_cast<uintptr_t>(&mx5_send_vehicle_data)};
    plan.entries[0]=position;plan.entry_count=1;plan.slots[0]=send;plan.slot_count=1;
    Setup setup={&in,expected_next,A::RequestBindings(),A::SessionBindings(),A::BusBindings(),true};
    if(in.observe_requests) {
        if(in.runtime.request_reader!=A::read_request_trace ||
           in.runtime.session_reader!=A::read_send_session)return INVALID_INSTALL_ARGUMENT;
        const InstallResult check=request_plan(in,plan,setup.bindings,setup.bus);
        if(check!=INSTALL_OK)return check;
        const unsigned slots_before=plan.slot_count;
        const InstallResult sessions=session_plan(in,plan,setup.session);
        if(sessions==NEXT_CHAIN_MISMATCH && in.report && in.report->declined_stage==3 &&
           A::known_session_shim(in.report->owner)) {
            // The user's oem-aa-mod patch interposes aap_create/destroy_session. Observing sessions
            // would call the original entry points directly and bypass that shim, so skip only the
            // session observation; the position/send hook below does not involve those slots.
            plan.slot_count=slots_before;
            setup.session=A::SessionBindings();
            setup.sessions_enabled=false;
            in.report->sessions_declined=true;
        } else if(sessions!=INSTALL_OK)return sessions;
    }
    const long page_size=sysconf(_SC_PAGESIZE);
    if(page_size<=0 || (page_size&(page_size-1)))return MEMORY_PROTECTION_FAILED;
    if (in.begin_patch && !in.begin_patch()) return COLD_START_LOST;
    struct PatchLease {
        void (*release)(bool);
        bool safe;
        ~PatchLease() { if (release) release(safe); }
    } lease = {in.end_patch, true};
    // All loader/hash/file queries finished before the single cold lease.
    const C::Ops ops={&setup,protect,allocate,release,flush,prepare};
    const InstallResult result=C::apply(plan,size_t(page_size),ops);
    lease.safe=result!=RESTORE_FAILED_FATAL;
    if(result!=INSTALL_OK)return result;
    installed = true;
    return INSTALL_OK;
#else
    (void)in;
    return UNSUPPORTED_ARCH;
#endif
}
const char* install_result_name(InstallResult r) {
    switch (r) {
    case INSTALL_OK: return "ok";
    case ALREADY_INSTALLED: return "already_installed";
    case UNSUPPORTED_ARCH: return "unsupported_architecture";
    case INVALID_INSTALL_ARGUMENT: return "invalid_install_argument_or_not_cold";
    case FILE_IDENTITY_MISMATCH: return "file_identity_mismatch";
    case MODULE_MISMATCH: return "module_mapping_mismatch";
    case ORIGINAL_BYTES_MISMATCH: return "original_bytes_mismatch";
    case NEXT_CHAIN_MISMATCH: return "next_chain_mismatch_or_lazy_binding";
    case MEMORY_PROTECTION_FAILED: return "memory_protection_failed";
    case TRAMPOLINE_ALLOCATION_FAILED: return "trampoline_allocation_failed";
    case CONFIGURATION_FAILED: return "configuration_failed";
    case COLD_START_LOST: return "cold_start_lost_to_concurrent_load";
    case RESTORE_FAILED_FATAL: return "fatal_cannot_restore_oem_execute_permission";
    }
    return "unknown_install_result";
}
} }
