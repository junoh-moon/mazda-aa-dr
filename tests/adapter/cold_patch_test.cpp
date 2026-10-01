#include "adapter/cold_patch.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
namespace A=mx5::adapter;
namespace C=A::cold_patch;
struct Fixture {
    enum { PAGE=4096, RX=PROT_READ|PROT_EXEC, RW=PROT_READ|PROT_WRITE };
    unsigned calls,fail1,fail2,prepared,released,changed_slot_index;
    bool allocation_fail,prepare_fail,changed_code,changed_slot;
    unsigned char *code,*got,*tramp;
    int permissions[5];
    C::Plan plan;
    static const uint8_t bytes[16];
    Fixture():calls(0),fail1(0),fail2(0),prepared(0),released(0),changed_slot_index(0),
        allocation_fail(false),prepare_fail(false),changed_code(false),changed_slot(false),plan() {
        void* p;assert(!posix_memalign(&p,PAGE,4*PAGE));code=static_cast<unsigned char*>(p);
        assert(!posix_memalign(&p,PAGE,PAGE));got=static_cast<unsigned char*>(p);
        assert(!posix_memalign(&p,PAGE,PAGE));tramp=static_cast<unsigned char*>(p);
        memset(code,0,4*PAGE);memset(got,0,PAGE);memset(tramp,0,PAGE);
        for(unsigned i=0;i<4;++i) {
            permissions[i]=RX;memcpy(code+i*PAGE+32,bytes,16);
            C::Entry e={reinterpret_cast<uintptr_t>(code+i*PAGE+32),bytes,0x1000+16*i};
            plan.entries[plan.entry_count++]=e;
        }
        permissions[4]=RW;
        for(unsigned i=0;i<C::Plan::SLOT_CAPACITY;++i) {
            uintptr_t* slot=reinterpret_cast<uintptr_t*>(got)+i;*slot=0x2000+16*i;
            C::Slot s={reinterpret_cast<uintptr_t>(slot),*slot,0x3000+16*i};
            plan.slots[plan.slot_count++]=s;
        }
    }
    ~Fixture() { free(code);free(got);free(tramp); }
    static int protect(void* p,size_t n,int flags,void* u) {
        Fixture& f=*static_cast<Fixture*>(u);assert(n==PAGE);++f.calls;
        if(f.calls==f.fail1 || f.calls==f.fail2)return -1;
        if(p==f.tramp)f.permissions[4]=flags;
        else if(p==f.got)assert(flags==RW);
        else {
            const ptrdiff_t offset=static_cast<unsigned char*>(p)-f.code;
            assert(offset>=0 && offset<4*PAGE && offset%PAGE==0);
            f.permissions[offset/PAGE]=flags;
            if(offset==3*PAGE && flags==RW && !f.prepared) {
                if(f.changed_code)f.code[32]^=1;
                if(f.changed_slot)reinterpret_cast<uintptr_t*>(f.got)[f.changed_slot_index]=0xbad0;
            }
        }
        return 0;
    }
    static void* allocate(size_t n,void* u) {
        Fixture& f=*static_cast<Fixture*>(u);assert(n==PAGE);
        return f.allocation_fail?0:f.tramp;
    }
    static void release(void* p,size_t n,void* u) {
        Fixture& f=*static_cast<Fixture*>(u);assert(p==f.tramp && n==PAGE);++f.released;
    }
    static void flush(uintptr_t address,size_t n,void* u) {
        Fixture& f=*static_cast<Fixture*>(u);
        if(address==reinterpret_cast<uintptr_t>(f.tramp))assert(n==64 && f.permissions[4]==RW);
        else {
            const uintptr_t offset=address-reinterpret_cast<uintptr_t>(f.code);
            assert(offset<4*PAGE && n==8 && f.permissions[offset/PAGE]==RW);
        }
    }
    static bool prepare(void* p,void* u) {
        Fixture& f=*static_cast<Fixture*>(u);assert(p==f.tramp && f.permissions[4]==RX);
        ++f.prepared;
        for(unsigned i=0;i<f.plan.entry_count;++i) {
            assert(f.permissions[(f.plan.entries[i].address-reinterpret_cast<uintptr_t>(f.code))/PAGE]==RW);
            uint32_t* t=reinterpret_cast<uint32_t*>(f.tramp+16*i);
            assert(!memcmp(t,bytes,8) && t[2]==0xe51ff004 && t[3]==uint32_t(f.plan.entries[i].address+8));
        }
        for(unsigned i=0;i<f.plan.slot_count;++i)assert(*reinterpret_cast<uintptr_t*>(f.plan.slots[i].address)==f.plan.slots[i].expected);
        return !f.prepare_fail;
    }
    A::InstallResult run() {
        const C::Ops ops={this,protect,allocate,release,flush,prepare};
        return C::apply(plan,PAGE,ops);
    }
    void restored(bool success=false) {
        for(unsigned i=0;i<4;++i) {
            assert(permissions[i]==RX);
        }
        for(unsigned i=0;i<plan.entry_count;++i) {
            const unsigned char* address=reinterpret_cast<const unsigned char*>(plan.entries[i].address);
            if(!success)assert(!memcmp(address,bytes,16));
            else {
                uint32_t words[2];memcpy(words,address,8);
                assert(words[0]==0xe51ff004 && words[1]==plan.entries[i].replacement);
                assert(!memcmp(address+8,bytes+8,8));
            }
        }
        for(unsigned i=0;i<plan.slot_count;++i)assert(*reinterpret_cast<uintptr_t*>(plan.slots[i].address)==
            (success?plan.slots[i].replacement:plan.slots[i].expected));
    }
};
const uint8_t Fixture::bytes[16]={0x10,0x48,0x2d,0xe9,8,0xb0,0x8d,0xe2,1,2,3,4,5,6,7,8};
int main() {
    // AA now needs registration support before its existing connect/submit
    // slots become visible; all 21 data slots share one transaction.
    assert(C::Plan::SLOT_CAPACITY>=21);
    assert(sizeof(C::Plan().slots)/sizeof(C::Slot)>=21);
    unsigned calls=0;
    { Fixture f;assert(f.run()==A::INSTALL_OK);f.restored(true);assert(f.prepared==1 && !f.released);calls=f.calls; }
    assert(calls==C::Plan::SLOT_CAPACITY+9);
    for(unsigned i=1;i<=calls;++i) {
        Fixture f;f.fail1=i;assert(f.run()==A::MEMORY_PROTECTION_FAILED);f.restored();
        if(f.prepared)assert(!f.released);
    }
    { Fixture f;f.allocation_fail=true;assert(f.run()==A::TRAMPOLINE_ALLOCATION_FAILED);f.restored(); }
    { Fixture f;f.prepare_fail=true;assert(f.run()==A::CONFIGURATION_FAILED);f.restored();assert(f.released==1); }
    { Fixture f;f.changed_code=true;assert(f.run()==A::ORIGINAL_BYTES_MISMATCH);assert(!f.prepared);f.code[32]^=1;f.restored(); }
    for(unsigned i=0;i<C::Plan::SLOT_CAPACITY;++i) {
        Fixture f;f.changed_slot=true;f.changed_slot_index=i;
        assert(f.run()==A::NEXT_CHAIN_MISMATCH && !f.prepared && f.released==1);
        reinterpret_cast<uintptr_t*>(f.got)[i]=f.plan.slots[i].expected;f.restored();
    }
    for(unsigned second=calls+1;second<=calls+7;second+=3) {
        Fixture f;f.fail1=C::Plan::SLOT_CAPACITY+6;f.fail2=second;assert(f.run()==A::RESTORE_FAILED_FATAL);
        assert(!f.released); // Patched or non-executable OEM page: stop the service.
        for(unsigned i=0;i<f.plan.slot_count;++i)assert(*reinterpret_cast<uintptr_t*>(f.plan.slots[i].address)==f.plan.slots[i].expected);
    }
    // The doWork and destructor may share a page under a different page size.
    { Fixture f;f.plan.entries[3].address=f.plan.entries[2].address+32;
      memcpy(reinterpret_cast<void*>(f.plan.entries[3].address),Fixture::bytes,16);
      assert(f.run()==A::INSTALL_OK);assert(f.calls==C::Plan::SLOT_CAPACITY+7);f.restored(true); }
    printf("PASS cold transaction: %u protection failures, rechecks, preparation/allocation and fatal rollback\n",calls);
}
