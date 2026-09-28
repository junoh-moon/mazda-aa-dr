#include "adapter.h"
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
}
#endif

namespace mx5 { namespace adapter {
InstallResult install_v74(const InstallOptions& in) {
#if defined(__arm__) && !defined(__ARM_PCS_VFP) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    if (installed) return ALREADY_INSTALLED;
    if (!in.verified_cold_start || !in.verify_file_hash || !in.blm_path ||
        !in.interface_path || !in.blm_load_bias || !in.interface_load_bias)
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
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0 || (page_size & (page_size - 1))) return MEMORY_PROTECTION_FAILED;
    const uintptr_t got_page = slot_address & ~(uintptr_t(page_size) - 1);
    // Exact stock ELF has no GNU_RELRO. Establish writable permissions rather
    // than relying solely on the PT_LOAD flags of an already mapped object.
    if (mprotect(reinterpret_cast<void*>(got_page), size_t(page_size), PROT_READ|PROT_WRITE))
        return MEMORY_PROTECTION_FAILED;
    void* trampoline = mmap(0, size_t(page_size), PROT_READ|PROT_WRITE,
                            MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (trampoline == MAP_FAILED) return TRAMPOLINE_ALLOCATION_FAILED;
    uint32_t* code = static_cast<uint32_t*>(trampoline);
    std::memcpy(code, kPrologue, 8); // push/add; neither is PC-relative.
    code[2] = 0xe51ff004; code[3] = uint32_t(entry + 8);
    __builtin___clear_cache(static_cast<char*>(trampoline), static_cast<char*>(trampoline) + 16);
    if (mprotect(trampoline, size_t(page_size), PROT_READ|PROT_EXEC)) {
        munmap(trampoline, size_t(page_size)); return MEMORY_PROTECTION_FAILED;
    }
    const uintptr_t page = entry & ~(uintptr_t(page_size) - 1);
    // No thread may execute target code during this temporary RW/NX window.
    if (mprotect(reinterpret_cast<void*>(page), size_t(page_size), PROT_READ|PROT_WRITE)) {
        munmap(trampoline, size_t(page_size)); return MEMORY_PROTECTION_FAILED;
    }
    // Recheck after memory preparation and before any mutation.
    if (*slot != expected_next ||
        std::memcmp(reinterpret_cast<void*>(entry), kPrologue, sizeof kPrologue)) {
        const bool restored = !mprotect(reinterpret_cast<void*>(page), size_t(page_size), PROT_READ|PROT_EXEC);
        munmap(trampoline, size_t(page_size));
        return restored ? ORIGINAL_BYTES_MISMATCH : RESTORE_FAILED_FATAL;
    }
    if (!configure(reinterpret_cast<SendFunction>(expected_next), in.runtime)) {
        const bool restored = !mprotect(reinterpret_cast<void*>(page), size_t(page_size), PROT_READ|PROT_EXEC);
        munmap(trampoline, size_t(page_size));
        return restored ? CONFIGURATION_FAILED : RESTORE_FAILED_FATAL;
    }
    mx5_position_trampoline = trampoline;
    uint32_t patch[2] = {0xe51ff004, uint32_t(reinterpret_cast<uintptr_t>(&mx5_position_veneer))};
    std::memcpy(reinterpret_cast<void*>(entry), patch, 8);
    __builtin___clear_cache(reinterpret_cast<char*>(entry), reinterpret_cast<char*>(entry + 8));
    if (mprotect(reinterpret_cast<void*>(page), size_t(page_size), PROT_READ|PROT_EXEC)) {
        std::memcpy(reinterpret_cast<void*>(entry), kPrologue, 8);
        __builtin___clear_cache(reinterpret_cast<char*>(entry), reinterpret_cast<char*>(entry + 8));
        // Restoration of RX is mandatory. Returning into an NX OEM page is not
        // a usable fallback: bootstrap must stop this service on this failure.
        const bool restored = !mprotect(reinterpret_cast<void*>(page), size_t(page_size), PROT_READ|PROT_EXEC);
        mx5_position_trampoline = 0;
        munmap(trampoline, size_t(page_size));
        return restored ? MEMORY_PROTECTION_FAILED : RESTORE_FAILED_FATAL;
    }
    // The verified stock GOT segment is writable; no changes to touch symbols.
    *slot = reinterpret_cast<uintptr_t>(&mx5_send_vehicle_data);
    __sync_synchronize();
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
    case RESTORE_FAILED_FATAL: return "fatal_cannot_restore_oem_execute_permission";
    }
    return "unknown_install_result";
}
} }
