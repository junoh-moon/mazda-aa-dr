#include "session_hooks.h"
#include "adapter.h"
#include <atomic>
#include <errno.h>
#include <string.h>

#if defined(__arm__) && !defined(__EXCEPTIONS)
#error "Session wrappers require exception cleanup support"
#endif

namespace {
namespace A=mx5::adapter;
namespace S=mx5::runtime::session_trace;
static_assert(ATOMIC_INT_LOCK_FREE==2 && ATOMIC_LLONG_LOCK_FREE==2 && ATOMIC_LONG_LOCK_FREE==2,
              "Session observation must not introduce atomic library locks");
#if defined(__arm__)
static_assert(sizeof(A::SessionCallbacks)==76,"Exact NA 74.00.324A callback table");
#endif
enum Phase { UNUSED, CREATING, LIVE, CLOSING, ENDED };
struct Context {
    std::atomic<unsigned> phase;
    std::atomic<uint64_t> status; // One coherent (event, raw state) pair.
    // Immutable before CREATING publication, never recycled or freed.
    uintptr_t storage;
    void* user;
    A::SessionStatus next;
    A::SessionCallbacks callbacks;
    // Interposed dlopen can reach us before this DSO's dynamic constructors.
    constexpr Context():phase(UNUSED),status(0),storage(0),user(0),next(0),callbacks() {}
};
struct ContextPool {
    Context entries[A::SESSION_CONTEXT_CAPACITY];
    // GCC 4.9 still emits dynamic constructors for a standalone Context array.
    // Constant-initialize the containing object, including all array elements.
    constexpr ContextPool():entries() {}
    Context& operator[](unsigned i) { return entries[i]; }
};
ContextPool contexts;
std::atomic<unsigned> prepared(0), used(0), faults(0), mutations(0), lifecycles(0);
std::atomic<uint64_t> version(0);
A::SessionBindings original=A::SessionBindings();
bool attempted;
struct PreserveErrno {
    const int value;
    PreserveErrno():value(errno) {}
    ~PreserveErrno() { errno=value; }
};
void ready() { if(!prepared.load(std::memory_order_acquire))__builtin_trap(); }
void fault(unsigned reason) { faults.fetch_or(reason,std::memory_order_seq_cst); }
struct Mutation {
    const bool lifecycle;
    bool complete;
    explicit Mutation(bool changes_lifetime):lifecycle(changes_lifetime),complete(false) {
        const PreserveErrno saved;
        mutations.fetch_add(1);
        // Even APIs that serialize handle writes internally may overlap at
        // this boundary. Return order cannot establish storage ownership.
        // A status callback inside create/destroy is normal: it participates
        // in revision/revocation, but is not a second storage lifecycle call.
        if(lifecycle && lifecycles.fetch_add(1))fault(A::SESSION_CONTENTION);
        A::invalidate(); // Before the original lifecycle call can change state.
    }
    ~Mutation() {
        const PreserveErrno saved;
        if(!complete)fault(A::SESSION_UNWIND);
        // Reject candidates published during the call, including unwind.
        A::invalidate();
        if(version.fetch_add(1)==UINT64_MAX)fault(A::SESSION_REVISION_EXHAUSTED);
        if(lifecycle)lifecycles.fetch_sub(1);
        mutations.fetch_sub(1);
    }
};
template<unsigned index> void status(void* user,void* full_info) {
    ready();Mutation mutation(false);
    Context& c=contexts[index];
    {
        const PreserveErrno saved;
        // Acquire publishes the immutable original callback and user even for
        // an early callback on a pre-existing OEM thread. Phase is never UNUSED
        // after publication, including after destruction or failed creation.
        if(c.phase.load(std::memory_order_acquire)==UNUSED)__builtin_trap();
        if(!full_info || user!=c.user)fault(A::SESSION_CALLBACK);
        else {
            uint32_t state;memcpy(&state,full_info,4);
            uint64_t before=c.status.load();
            const uint32_t event=uint32_t(before>>32);
            if(event==UINT32_MAX)fault(A::SESSION_EVENT_EXHAUSTED);
            // Explicit success/failure orders avoid GCC 4.9's externally
            // visible std::__cmpexch_failure_order inline helper at -Os.
            else if(!c.status.compare_exchange_strong(before,(uint64_t(event+1)<<32)|state,
                        std::memory_order_seq_cst,std::memory_order_seq_cst))
                fault(A::SESSION_CONTENTION); // Do not reorder concurrent callbacks.
        }
    }
    c.next(user,full_info); // Full original pointer/user, exactly once, also late.
    mutation.complete=true;
}
template<unsigned... I> struct Indices {};
template<unsigned N,unsigned... I> struct MakeIndices:MakeIndices<N-1,N-1,I...> {};
template<unsigned... I> struct MakeIndices<0,I...> { typedef Indices<I...> Type; };
template<unsigned... I> A::SessionStatus wrapper(unsigned index,Indices<I...>) {
    static const A::SessionStatus table[]={&status<I>...};return table[index];
}
Context* reserve(const A::SessionCallbacks* cb,void* user,void** storage) {
    if(!cb || !storage || cb->entry[1]!=reinterpret_cast<uintptr_t>(original.status)) {
        fault(A::SESSION_UNTRACKED);return 0;
    }
    unsigned index=used.load();
    if(index>=A::SESSION_CONTEXT_CAPACITY) { fault(A::SESSION_CAPACITY);return 0; }
    if(!used.compare_exchange_strong(index,index+1,std::memory_order_seq_cst,std::memory_order_seq_cst)) {
        fault(A::SESSION_CONTENTION);return 0;
    }
    Context& c=contexts[index];
    c.storage=reinterpret_cast<uintptr_t>(storage);c.user=user;c.next=original.status;
    c.callbacks=*cb;
    c.callbacks.entry[1]=reinterpret_cast<uintptr_t>(wrapper(index,MakeIndices<A::SESSION_CONTEXT_CAPACITY>::Type()));
    c.phase.store(CREATING,std::memory_order_release);
    return &c;
}
S::Snapshot unavailable(S::Result result) {
    S::Snapshot out=S::Snapshot();out.result=result;return out;
}
S::Snapshot snapshot(const void* storage,bool ambient) {
    const PreserveErrno saved;
    if(!prepared.load(std::memory_order_acquire))return unavailable(S::UNOBSERVED);
    if(faults.load())return unavailable(S::FAULT);
    const uint64_t before=version.load();
    if(mutations.load())return unavailable(S::TRANSITION);
    S::Snapshot out=unavailable(S::NONE);
    const unsigned count=used.load();
    for(unsigned i=0;i<count;++i) {
        const Context& c=contexts[i];
        if(c.phase.load(std::memory_order_acquire)!=LIVE ||
           (!ambient && (!storage || c.storage!=reinterpret_cast<uintptr_t>(storage))))continue;
        if(out.result==S::OBSERVED) { out=unavailable(S::AMBIGUOUS);break; }
        const uint64_t pair=c.status.load();
        out.result=S::OBSERVED;out.lifetime=i+1;out.event=uint32_t(pair>>32);
        const uint32_t bits=uint32_t(pair);memcpy(&out.state,&bits,4);
        out.state_known=out.event!=0;
    }
    // All lifecycle writers increment version before dropping their in-flight
    // count. Readers never take a mutex or cause event loss. The atomics here
    // are seq_cst so a completed overlapping mutation cannot evade both checks.
    if(mutations.load() || version.load()!=before)return unavailable(S::TRANSITION);
    if(faults.load())return unavailable(S::FAULT);
    out.revision=before;
    return out;
}
}

namespace mx5 { namespace adapter {
bool prepare_session_hooks(const SessionBindings& b) {
    const PreserveErrno saved;
    if(attempted || !b.create || !b.destroy || !b.status)return false;
    attempted=true;original=b;prepared.store(1,std::memory_order_release);return true;
}
SessionHealth session_hook_health() {
    const PreserveErrno saved;
    const SessionHealth out={prepared.load(std::memory_order_acquire)!=0,used.load(),faults.load()};
    return out;
}
S::Snapshot read_issue_session() { return snapshot(0,true); }
void read_send_session(const void* storage,S::Snapshot* out,void*) {
    if(out)*out=snapshot(storage,false);
}
} }

extern "C" int32_t mx5_session_create(const char* xml,void* user,const A::SessionCallbacks* cb,void** storage) {
    ready();Mutation mutation(true);
    Context* c;
    { const PreserveErrno saved;c=reserve(cb,user,storage); }
    const int32_t result=original.create(xml,user,c?&c->callbacks:cb,storage);
    {
        const PreserveErrno saved;
        if(c) {
            unsigned phase=CREATING;
            // A concurrent/reentrant destroy may already have ended this
            // creation. A successful return must never resurrect it.
            // Observe the API result only. Reading *storage here would race
            // a destroy protected by the OEM's own, inaccessible mutex.
            c->phase.compare_exchange_strong(phase,!result?LIVE:ENDED,
                                            std::memory_order_seq_cst,std::memory_order_seq_cst);
        }
        mutation.complete=true;
    }
    return result;
}
extern "C" int32_t mx5_session_destroy(void** storage) {
    ready();Mutation mutation(true);
    uint64_t closing=0;
    {
        const PreserveErrno saved;
        const unsigned count=used.load();
        for(unsigned i=0;i<count;++i) {
            Context& c=contexts[i];unsigned phase=c.phase.load(std::memory_order_acquire);
            if((phase!=CREATING && phase!=LIVE) || c.storage!=reinterpret_cast<uintptr_t>(storage))continue;
            if(c.phase.compare_exchange_strong(phase,CLOSING,std::memory_order_seq_cst,std::memory_order_seq_cst))
                closing|=uint64_t(1)<<i;
            else fault(A::SESSION_CONTENTION);
        }
    }
    const int32_t result=original.destroy(storage);
    {
        const PreserveErrno saved;
        // Even failed/partial destruction revokes our claim of a live session.
        // No assumption about the OEM handle's eventual cleanup is made.
        for(unsigned i=0;i<A::SESSION_CONTEXT_CAPACITY;++i)
            if(closing&(uint64_t(1)<<i))contexts[i].phase.store(ENDED);
        mutation.complete=true;
    }
    return result;
}
