#include "adapter/data_patch.h"
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

namespace D=mx5::adapter::data_patch;
namespace A=mx5::adapter;
typedef int (*Call)(int);
static unsigned calls[2];
static Call originals[2];
static bool prepared;
static int first(int value) { ++calls[0];errno=EDOM;return value+11; }
static int second(int value) { ++calls[1];errno=ERANGE;return value+29; }
static int foreign(int value) { return value-77; }
static int first_hook(int value) { assert(prepared);return originals[0](value); }
static int second_hook(int value) { assert(prepared);return originals[1](value); }

struct Fixture {
    size_t page;
    void* memory;
    uintptr_t *a,*b;
    D::Plan plan;
    D::Ops ops;
    unsigned protects,prepares,exchanges,fail_protect,fail_exchange;
    bool fail_prepare,change_prepare,change_during_protect,foreign_rollback;
    Fixture():page(size_t(sysconf(_SC_PAGESIZE))),memory(0),a(0),b(0),
        plan(),ops(),protects(0),prepares(0),exchanges(0),fail_protect(0),
        fail_exchange(0),fail_prepare(false),change_prepare(false),
        change_during_protect(false),foreign_rollback(false) {
        memory=mmap(0,page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        assert(memory!=MAP_FAILED);
        a=static_cast<uintptr_t*>(memory);
        b=reinterpret_cast<uintptr_t*>(static_cast<char*>(memory)+page);
        *a=reinterpret_cast<uintptr_t>(&first);*b=reinterpret_cast<uintptr_t>(&second);
        plan.slot_count=2;
        plan.slots[0]=D::Slot{reinterpret_cast<uintptr_t>(a),*a,reinterpret_cast<uintptr_t>(&first_hook)};
        plan.slots[1]=D::Slot{reinterpret_cast<uintptr_t>(b),*b,reinterpret_cast<uintptr_t>(&second_hook)};
        ops.user=this;ops.protect=protect;ops.prepare=prepare;ops.exchange=exchange;
        prepared=false;originals[0]=originals[1]=0;calls[0]=calls[1]=0;
    }
    ~Fixture() { assert(!munmap(memory,page*2)); }
    static int protect(void* address,size_t bytes,int mode,void* user) {
        Fixture& f=*static_cast<Fixture*>(user);
        assert(bytes==f.page && mode==(PROT_READ|PROT_WRITE));
        assert(address==f.memory || address==f.b);
        ++f.protects;
        if(f.change_during_protect)*f.b=reinterpret_cast<uintptr_t>(&first);
        if(f.protects==f.fail_protect)return -1;
        return mprotect(address,bytes,mode);
    }
    static bool prepare(void* user) {
        Fixture& f=*static_cast<Fixture*>(user);++f.prepares;
        if(f.fail_prepare)return false;
        originals[0]=&first;originals[1]=&second;prepared=true;
        if(f.change_prepare)*f.b=reinterpret_cast<uintptr_t>(&first);
        return true;
    }
    void exercise_visible_calls() {
        if(*a==plan.slots[0].expected || *a==plan.slots[0].replacement) {
            unsigned before=calls[0];errno=E2BIG;
            assert(reinterpret_cast<Call>(*a)(7)==18 && errno==EDOM && calls[0]==before+1);
        }
        if(*b==plan.slots[1].expected || *b==plan.slots[1].replacement) {
            unsigned before=calls[1];errno=E2BIG;
            assert(reinterpret_cast<Call>(*b)(7)==36 && errno==ERANGE && calls[1]==before+1);
        }
    }
    static bool exchange(uintptr_t* address,uintptr_t expected,uintptr_t replacement,void* user) {
        Fixture& f=*static_cast<Fixture*>(user);++f.exchanges;
        if(f.exchanges==f.fail_exchange) {
            // Another owner changed the failed slot after the final preflight.
            *address=reinterpret_cast<uintptr_t>(&foreign);
            if(f.foreign_rollback)*f.a=reinterpret_cast<uintptr_t>(&foreign);
            return false;
        }
        bool result=D::native_exchange(address,expected,replacement,0);
        f.exercise_visible_calls();
        return result;
    }
    D::Result apply() { return D::apply(plan,page,ops); }
};

static void normal_and_partial() {
    {
        Fixture f;D::Result r=f.apply();
        assert(r.status==A::INSTALL_OK && r.prepared && r.published==2);
        assert(r.restored==0 && r.unrestored==0 && f.prepares==1 && f.protects==2);
        assert(*f.a==f.plan.slots[0].replacement && *f.b==f.plan.slots[1].replacement);
        f.exercise_visible_calls();
    }
    for(unsigned failed=1;failed<=2;++failed) {
        Fixture f;f.fail_exchange=failed;D::Result r=f.apply();
        assert(r.status==A::NEXT_CHAIN_MISMATCH && r.prepared);
        assert(r.published==failed-1 && r.restored==failed-1 && r.unrestored==0);
        assert(prepared && originals[0]==&first && originals[1]==&second);
        if(failed==2)assert(*f.a==f.plan.slots[0].expected);
        // A callable fetched while published remains valid after rollback.
        errno=E2BIG;assert(first_hook(1)==12 && errno==EDOM);
    }
    {
        Fixture f;f.fail_exchange=2;f.foreign_rollback=true;D::Result r=f.apply();
        assert(r.status==A::NEXT_CHAIN_MISMATCH && r.prepared);
        assert(r.published==1 && r.restored==0 && r.unrestored==1);
        assert(*f.a==reinterpret_cast<uintptr_t>(&foreign));
        assert(first_hook(1)==12); // Never discard immutable original targets.
    }
}

static void full_page() {
    Fixture f;f.plan.slot_count=D::Plan::SLOT_CAPACITY;
    for(unsigned i=0;i<f.plan.slot_count;++i) {
        uintptr_t original=reinterpret_cast<uintptr_t>(i%2?&second:&first);
        uintptr_t hook=reinterpret_cast<uintptr_t>(i%2?&second_hook:&first_hook);
        f.a[i]=original;
        f.plan.slots[i]=D::Slot{reinterpret_cast<uintptr_t>(f.a+i),original,hook};
    }
    D::Result r=f.apply();
    assert(r.status==A::INSTALL_OK && r.published==f.plan.slot_count && f.protects==1);
    for(unsigned i=0;i<f.plan.slot_count;++i) {
        errno=E2BIG;int value=reinterpret_cast<Call>(f.a[i])(1);
        assert(value==(i%2?30:12) && errno==(i%2?ERANGE:EDOM));
    }
}

static void prepublication_failures() {
    for(unsigned failed=1;failed<=2;++failed) {
        Fixture f;f.fail_protect=failed;D::Result r=f.apply();
        assert(r.status==A::MEMORY_PROTECTION_FAILED && !r.prepared);
        assert(f.prepares==0 && f.exchanges==0);
    }
    {
        Fixture f;*f.b=reinterpret_cast<uintptr_t>(&first);D::Result r=f.apply();
        assert(r.status==A::NEXT_CHAIN_MISMATCH && !r.prepared && !f.exchanges);
    }
    {
        Fixture f;f.change_during_protect=true;D::Result r=f.apply();
        assert(r.status==A::NEXT_CHAIN_MISMATCH && !r.prepared && !f.exchanges);
    }
    {
        Fixture f;f.fail_prepare=true;D::Result r=f.apply();
        assert(r.status==A::CONFIGURATION_FAILED && !r.prepared && !f.exchanges);
    }
    {
        Fixture f;f.change_prepare=true;D::Result r=f.apply();
        assert(r.status==A::NEXT_CHAIN_MISMATCH && r.prepared && !f.exchanges);
        assert(first_hook(1)==12);
    }
}

static void invalid_arguments() {
    for(unsigned n=0;n<11;++n) {
        Fixture f;size_t page=f.page;
        switch(n) {
        case 0:f.plan.slot_count=0;break;
        case 1:f.plan.slot_count=D::Plan::SLOT_CAPACITY+1;break;
        case 2:f.plan.slots[1].address=f.plan.slots[0].address;break;
        case 3:f.plan.slots[1].address+=1;break;
        case 4:f.plan.slots[1].address=0;break;
        case 5:f.plan.slots[1].expected=0;break;
        case 6:f.plan.slots[1].replacement=f.plan.slots[1].expected;break;
        case 7:page=63;break;
        case 8:f.ops.prepare=0;break;
        case 9:f.ops.protect=0;break;
        case 10:f.ops.exchange=0;break;
        }
        D::Result r=D::apply(f.plan,page,f.ops);
        assert(r.status==A::INVALID_INSTALL_ARGUMENT && !r.prepared && !f.exchanges && !f.protects);
    }
}

int main() {
    normal_and_partial();prepublication_failures();invalid_arguments();full_page();
    std::puts("Data slots: original forwarding during partial publication, rollback ownership and retained targets passed");
}
