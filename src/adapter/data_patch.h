#ifndef MX5_ADAPTER_DATA_PATCH_H
#define MX5_ADAPTER_DATA_PATCH_H

#include "adapter.h"
#include <sys/mman.h>

namespace mx5 { namespace adapter { namespace data_patch {

// Data-only publication under the cold LDS loader lease. Before entry the
// installer must verify every owner, live next target and non-RELRO RW segment.
// No executable page, trampoline or owned original target is changed or freed.
struct Slot { uintptr_t address,expected,replacement; };
struct Plan { enum { SLOT_CAPACITY=32 }; Slot slots[SLOT_CAPACITY];unsigned slot_count; };
struct Ops {
    void* user;
    int (*protect)(void*,size_t,int,void*);
    // Initialize immutable forwarding targets; no loader or original calls.
    // These targets outlive any partial publication and subsequent rollback.
    bool (*prepare)(void*);
    bool (*exchange)(uintptr_t*,uintptr_t,uintptr_t,void*);
};
struct Result {
    InstallResult status;
    unsigned published,restored,unrestored;
    bool prepared;
    Result():status(INVALID_INSTALL_ARGUMENT),published(0),restored(0),
        unrestored(0),prepared(false) {}
};
inline bool native_exchange(uintptr_t* address,uintptr_t expected,
                            uintptr_t replacement,void*) {
    return __atomic_compare_exchange_n(address,&expected,replacement,false,
                                       __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
}
inline bool matches(const Plan& plan) {
    for(unsigned i=0;i<plan.slot_count;++i)
        if(__atomic_load_n(reinterpret_cast<uintptr_t*>(plan.slots[i].address),
                           __ATOMIC_ACQUIRE)!=plan.slots[i].expected)return false;
    return true;
}
inline Result apply(const Plan& plan,size_t page_size,const Ops& ops) {
    Result result;
    if(!plan.slot_count || plan.slot_count>Plan::SLOT_CAPACITY || page_size<64 ||
       (page_size&(page_size-1)) || !ops.protect || !ops.prepare || !ops.exchange)
        return result;
    uintptr_t pages[Plan::SLOT_CAPACITY];unsigned page_count=0;
    for(unsigned i=0;i<plan.slot_count;++i) {
        const Slot& slot=plan.slots[i];
        if(!slot.address || (slot.address&(sizeof(uintptr_t)-1)) ||
           !slot.expected || !slot.replacement || slot.expected==slot.replacement)
            return result;
        for(unsigned j=0;j<i;++j)if(plan.slots[j].address==slot.address)return result;
        uintptr_t page=slot.address&~(uintptr_t(page_size)-1);bool seen=false;
        for(unsigned j=0;j<page_count;++j)if(pages[j]==page)seen=true;
        if(!seen)pages[page_count++]=page;
    }
    if(!matches(plan)) { result.status=NEXT_CHAIN_MISMATCH;return result; }
    for(unsigned i=0;i<page_count;++i) {
        // The caller established RW/non-executable ownership. Reassert only
        // this permission before publication; there is no RX restoration.
        if(ops.protect(reinterpret_cast<void*>(pages[i]),page_size,
                       PROT_READ|PROT_WRITE,ops.user)) {
            result.status=MEMORY_PROTECTION_FAILED;return result;
        }
    }
    if(!matches(plan)) { result.status=NEXT_CHAIN_MISMATCH;return result; }
    if(!ops.prepare(ops.user)) { result.status=CONFIGURATION_FAILED;return result; }
    result.prepared=true;
    if(!matches(plan)) { result.status=NEXT_CHAIN_MISMATCH;return result; }
    for(unsigned i=0;i<plan.slot_count;++i) {
        const Slot& slot=plan.slots[i];
        if(ops.exchange(reinterpret_cast<uintptr_t*>(slot.address),slot.expected,
                        slot.replacement,ops.user)) { ++result.published;continue; }
        // Restore only pointers still owned by this installation. An external
        // edit must never be overwritten. Observers remain inactive on failure;
        // any already fetched wrappers keep immutable forwarding targets.
        for(unsigned j=result.published;j>0;--j) {
            const Slot& prior=plan.slots[j-1];
            if(ops.exchange(reinterpret_cast<uintptr_t*>(prior.address),prior.replacement,
                            prior.expected,ops.user))++result.restored;
            else ++result.unrestored;
        }
        result.status=NEXT_CHAIN_MISMATCH;return result;
    }
    result.status=INSTALL_OK;
    // Only the installer may activate observation after this successful return.
    return result;
}
} } }
#endif
