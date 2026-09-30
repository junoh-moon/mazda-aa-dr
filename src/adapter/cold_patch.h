#ifndef MX5_ADAPTER_COLD_PATCH_H
#define MX5_ADAPTER_COLD_PATCH_H

#include "adapter.h"
#include <cstring>
#include <sys/mman.h>

namespace mx5 { namespace adapter { namespace cold_patch {
// Internal transaction, called only while the loader's cold BLM lease is held.
// JCIDBUS may already have callers: change its data slots, never its code pages.
struct Entry { uintptr_t address; const uint8_t* bytes; uintptr_t replacement; };
struct Slot { uintptr_t address, expected, replacement; };
struct Plan {
    Entry entries[4]; unsigned entry_count;
    Slot slots[16]; unsigned slot_count;
};
struct Ops {
    void* user;
    int (*protect)(void*, size_t, int, void*);
    void* (*allocate)(size_t, void*); // NULL on failure; one page, initially RW.
    void (*release)(void*, size_t, void*);
    void (*flush)(uintptr_t, size_t, void*);
    bool (*prepare)(void* trampolines, void*); // No publication or loader calls.
};

inline InstallResult apply(const Plan& plan, size_t page_size, const Ops& ops) {
    if(!plan.entry_count || plan.entry_count>4 || !plan.slot_count || plan.slot_count>16 ||
       page_size<64 || (page_size&(page_size-1)))return INVALID_INSTALL_ARGUMENT;
    uintptr_t pages[4]; unsigned page_count=0, writable=0;
    for(unsigned i=0;i<plan.entry_count;++i) {
        const Entry& e=plan.entries[i];
        if(!e.bytes || !e.address || !e.replacement || (e.address&3) ||
           (e.address&(page_size-1))>page_size-16)return INVALID_INSTALL_ARGUMENT;
        uintptr_t page=e.address&~(uintptr_t(page_size)-1);
        bool seen=false;
        for(unsigned j=0;j<page_count;++j)if(pages[j]==page)seen=true;
        if(!seen)pages[page_count++]=page;
    }
    // Stock GOT pages are RW and contain no executable code / GNU_RELRO.
    // Establish that permission before publication; no later mprotect touches them.
    for(unsigned i=0;i<plan.slot_count;++i) {
        const Slot& s=plan.slots[i];
        if(!s.address || (s.address&(sizeof(uintptr_t)-1)))return INVALID_INSTALL_ARGUMENT;
        const uintptr_t page=s.address&~(uintptr_t(page_size)-1);
        if(ops.protect(reinterpret_cast<void*>(page),page_size,PROT_READ|PROT_WRITE,ops.user))
            return MEMORY_PROTECTION_FAILED;
    }
    void* tramp=ops.allocate(page_size,ops.user);
    if(!tramp)return TRAMPOLINE_ALLOCATION_FAILED;
    for(unsigned i=0;i<plan.entry_count;++i) {
        uint32_t* code=reinterpret_cast<uint32_t*>(static_cast<char*>(tramp)+16*i);
        std::memcpy(code,plan.entries[i].bytes,8); // Verified ARM push/add, no PC operands.
        code[2]=0xe51ff004;code[3]=uint32_t(plan.entries[i].address+8);
    }
    ops.flush(reinterpret_cast<uintptr_t>(tramp),16*plan.entry_count,ops.user);
    if(ops.protect(tramp,page_size,PROT_READ|PROT_EXEC,ops.user)) {
        ops.release(tramp,page_size,ops.user);return MEMORY_PROTECTION_FAILED;
    }
    InstallResult result=MEMORY_PROTECTION_FAILED;
    for(;writable<page_count;++writable)
        if(ops.protect(reinterpret_cast<void*>(pages[writable]),page_size,PROT_READ|PROT_WRITE,ops.user))break;
    bool patched=false, prepared=false;
    if(writable==page_count) {
        result=INSTALL_OK;
        for(unsigned i=0;i<plan.entry_count;++i)
            if(std::memcmp(reinterpret_cast<void*>(plan.entries[i].address),plan.entries[i].bytes,16))
                result=ORIGINAL_BYTES_MISMATCH;
        for(unsigned i=0;i<plan.slot_count;++i)
            if(*reinterpret_cast<volatile uintptr_t*>(plan.slots[i].address)!=plan.slots[i].expected)
                result=NEXT_CHAIN_MISMATCH;
        if(result==INSTALL_OK) {
            prepared=ops.prepare(tramp,ops.user);
            if(!prepared)result=CONFIGURATION_FAILED;
        }
        if(result==INSTALL_OK) {
            for(unsigned i=0;i<plan.entry_count;++i) {
                const Entry& e=plan.entries[i];
                const uint32_t patch[]={0xe51ff004,uint32_t(e.replacement)};
                std::memcpy(reinterpret_cast<void*>(e.address),patch,8);
                ops.flush(e.address,8,ops.user);
            }
            patched=true;
        }
    }
    // Keep track of which pages are still writable, so rollback never writes RX.
    bool rw[4]={false,false,false,false}, safe=true;
    for(unsigned i=0;i<writable;++i)rw[i]=true;
    for(unsigned i=0;i<writable;++i) {
        if(ops.protect(reinterpret_cast<void*>(pages[i]),page_size,PROT_READ|PROT_EXEC,ops.user)) {
            result=MEMORY_PROTECTION_FAILED;
        } else rw[i]=false;
    }
    if(result!=INSTALL_OK) {
        if(patched) {
            for(unsigned i=0;i<page_count;++i) {
                if(!rw[i] && ops.protect(reinterpret_cast<void*>(pages[i]),page_size,PROT_READ|PROT_WRITE,ops.user)) {
                    safe=false;continue;
                }
                rw[i]=true;
                for(unsigned j=0;j<plan.entry_count;++j) {
                    const Entry& e=plan.entries[j];
                    if((e.address&~(uintptr_t(page_size)-1))!=pages[i])continue;
                    std::memcpy(reinterpret_cast<void*>(e.address),e.bytes,8);
                    ops.flush(e.address,8,ops.user);
                }
            }
        }
        for(unsigned i=0;i<writable;++i)
            if(rw[i] && ops.protect(reinterpret_cast<void*>(pages[i]),page_size,PROT_READ|PROT_EXEC,ops.user))safe=false;
        // A prepared binding owns its trampoline until process exit. Never leave
        // dangling targets after a rollback, even though no GOT was published.
        if(!prepared && safe)ops.release(tramp,page_size,ops.user);
        return safe?result:RESTORE_FAILED_FATAL;
    }
    // All fallible operations are complete. Prepare-before-publish and cleanup
    // slots-before-submit order allow existing JCIDBUS threads to call safely.
    __sync_synchronize();
    for(unsigned i=0;i<plan.slot_count;++i) {
        *reinterpret_cast<volatile uintptr_t*>(plan.slots[i].address)=plan.slots[i].replacement;
        // ARM may expose stores to different addresses out of order. In
        // particular, submit must never become visible before cleanup hooks.
        __sync_synchronize();
    }
    return INSTALL_OK;
}
} } }
#endif
