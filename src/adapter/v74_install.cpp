#include "adapter.h"
#include "cold_patch.h"
#include "request_hooks.h"
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
    {"JCIDBUS_method_send_async_with_notify",0x1a09c,{0xe92d4800,0xe28db004,0xe24dd038,0xe50b0020}},
    {"JCIDBUS_method_free",0x19494,{0xe92d4800,0xe28db004,0xe24dd010,0xe50b0010}},
    {"JCIDBUS_free_method_only",0x195e8,{0xe92d4800,0xe28db004,0xe24dd010,0xe50b0010}}
};
A::InstallResult request_plan(const A::InstallOptions& in,C::Plan& plan,A::RequestBindings& bindings) {
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
        if(*reinterpret_cast<uintptr_t*>(s.address)!=s.expected)return A::NEXT_CHAIN_MISMATCH;
        plan.slots[i]=s;
    }
    plan.slot_count=5;
    bindings.reply.get_reply=reinterpret_cast<decltype(bindings.reply.get_reply)>(bb+0x19778);
    bindings.reply.get_type=reinterpret_cast<decltype(bindings.reply.get_type)>(bb+0x10b20);
    bindings.reply.get_sender=reinterpret_cast<decltype(bindings.reply.get_sender)>(bb+0x10c10);
    bindings.reply.get_error=reinterpret_cast<decltype(bindings.reply.get_error)>(bb+0x10528);
    bindings.reply.get_reply_serial=reinterpret_cast<decltype(bindings.reply.get_reply_serial)>(bb+0x1041c);
    bindings.submit=reinterpret_cast<A::RequestSubmit>(bb+0x1a09c);
    bindings.notify=reinterpret_cast<A::RequestNotify>(db+0x2228);
    bindings.free_method=reinterpret_cast<A::RequestFree>(bb+0x19494);
    bindings.free_method_only=reinterpret_cast<A::RequestFree>(bb+0x195e8);
    bindings.position_vptr=blm+0xf7140;
    return A::INSTALL_OK;
}
struct Setup { const A::InstallOptions* options; uintptr_t send; A::RequestBindings bindings; };
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
    if (*slot != expected_next) return NEXT_CHAIN_MISMATCH;
    C::Plan plan=C::Plan();
    const C::Entry position={entry,kPrologue,reinterpret_cast<uintptr_t>(&mx5_position_veneer)};
    const C::Slot send={slot_address,expected_next,reinterpret_cast<uintptr_t>(&mx5_send_vehicle_data)};
    plan.entries[0]=position;plan.entry_count=1;plan.slots[0]=send;plan.slot_count=1;
    Setup setup={&in,expected_next,A::RequestBindings()};
    if(in.observe_requests) {
        if(in.runtime.request_reader!=A::read_request_trace)return INVALID_INSTALL_ARGUMENT;
        const InstallResult check=request_plan(in,plan,setup.bindings);
        if(check!=INSTALL_OK)return check;
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
