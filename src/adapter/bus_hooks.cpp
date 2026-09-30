#include "bus_hooks.h"
#include "adapter.h"
#include <atomic>
#include <errno.h>

#if defined(__arm__) && !defined(__EXCEPTIONS)
#error "Bus wrappers require exception cleanup support"
#endif
namespace {
namespace A=mx5::adapter;
namespace B=mx5::runtime::bus_trace;
static_assert(ATOMIC_INT_LOCK_FREE==2 && ATOMIC_LLONG_LOCK_FREE==2 && ATOMIC_POINTER_LOCK_FREE==2,
              "Bus observation requires lock-free atomics");
enum Phase { UNUSED, CREATING, IDLE, CONNECTING, ACTIVE, ENDED };
struct Context {
    std::atomic<unsigned> phase;
    std::atomic<uintptr_t> address;
    std::atomic<uint64_t> lifetime;
    std::atomic<uint64_t> source_lifetime;
    A::BusClosed next;
    void* user;
    constexpr Context():phase(UNUSED),address(0),lifetime(0),source_lifetime(0),next(0),user(0) {}
};
// Keep the containing object constant-initialized, including on GCC 4.9.
struct Pool {
    Context entries[A::BUS_CONTEXT_CAPACITY];
    constexpr Pool():entries() {}
    Context& operator[](unsigned i) { return entries[i]; }
};
Pool contexts;
std::atomic<unsigned> prepared(0),used(0),faults(0),mutations(0),lifecycle(0);
std::atomic<uint64_t> version(0),next_lifetime(0);
A::BusBindings original=A::BusBindings();
bool attempted;
__thread const void* freeing __attribute__((tls_model("initial-exec")));
struct PreserveErrno {
    const int value;
    PreserveErrno():value(errno) {}
    ~PreserveErrno() { errno=value; }
};
void ready() { if(!prepared.load(std::memory_order_acquire))__builtin_trap(); }
void fault(unsigned why) { faults.fetch_or(why); }
struct Mutation {
    bool complete;
    const bool life;
    explicit Mutation(bool is_lifecycle=true):complete(false),life(is_lifecycle) {
        const PreserveErrno saved;
        mutations.fetch_add(1);
        if(life && lifecycle.fetch_add(1))fault(A::BUS_CONTENTION);
        A::invalidate();
    }
    ~Mutation() {
        const PreserveErrno saved;
        if(!complete)fault(A::BUS_UNWIND);
        A::invalidate();
        if(version.fetch_add(1)==UINT64_MAX)fault(A::BUS_EXHAUSTED);
        if(life)lifecycle.fetch_sub(1);
        mutations.fetch_sub(1);
    }
};
struct FreeScope {
    const void* previous;
    explicit FreeScope(const void* p):previous(freeing) { freeing=p; }
    ~FreeScope() { freeing=previous; }
};
Context* lookup(const void* p) {
    if(!p)return 0;
    Context* found=0;
    for(unsigned i=0,n=used.load();i<n;++i) {
        Context& c=contexts[i];const unsigned phase=c.phase.load(std::memory_order_acquire);
        if(phase==UNUSED || phase==ENDED || c.address.load()!=reinterpret_cast<uintptr_t>(p))continue;
        if(found) { fault(A::BUS_COLLISION);return 0; }
        found=&c;
    }
    return found;
}
template<unsigned i> int32_t closed(void* connection,void* user) {
    ready();Mutation mutation(false);Context& c=contexts[i];
    {
        const PreserveErrno saved;
        unsigned phase=c.phase.load(std::memory_order_acquire);
        if(phase==UNUSED)__builtin_trap();
        const uintptr_t address=c.address.load();
        if(user!=c.user || (address && address!=reinterpret_cast<uintptr_t>(connection)))fault(A::BUS_CALLBACK);
        // Do not resurrect a freed object or close its address's new owner.
        // A close during connect prevents that call from publishing ACTIVE.
        if(phase!=ENDED && !c.phase.compare_exchange_strong(phase,IDLE,
                std::memory_order_seq_cst,std::memory_order_seq_cst))fault(A::BUS_CONTENTION);
    }
    const int32_t result=c.next(connection,user);
    mutation.complete=true;return result;
}
template<unsigned... I> struct Indices {};
template<unsigned N,unsigned... I> struct MakeIndices:MakeIndices<N-1,N-1,I...> {};
template<unsigned... I> struct MakeIndices<0,I...> { typedef Indices<I...> Type; };
template<unsigned... I> A::BusClosed callback(unsigned i,Indices<I...>) {
    static const A::BusClosed table[]={&closed<I>...};return table[i];
}
Context* reserve(A::BusClosed next,void* user,A::BusClosed* wrapper) {
    if(!next)return 0; // NULL must remain NULL: the original rejects it.
    unsigned i=used.load();
    if(i>=A::BUS_CONTEXT_CAPACITY) { fault(A::BUS_CAPACITY);return 0; }
    if(!used.compare_exchange_strong(i,i+1,std::memory_order_seq_cst,std::memory_order_seq_cst)) {
        fault(A::BUS_CONTENTION);return 0;
    }
    Context& c=contexts[i];c.next=next;c.user=user;
    c.phase.store(CREATING,std::memory_order_release);
    *wrapper=callback(i,MakeIndices<A::BUS_CONTEXT_CAPACITY>::Type());return &c;
}
B::Snapshot unavailable(B::Result result) { B::Snapshot out=B::Snapshot();out.result=result;return out; }
B::Boundary boundary_unavailable(B::Result result) {
    B::Boundary out=B::Boundary();out.connection=unavailable(result);return out;
}
}
namespace mx5 { namespace adapter {
bool prepare_bus_hooks(const BusBindings& b) {
    const PreserveErrno saved;
    if(attempted || !b.create || !b.connect || !b.disconnect || !b.free || !b.signal || !b.is_signal)return false;
    attempted=true;original=b;prepared.store(1,std::memory_order_release);return true;
}
BusHealth bus_hook_health() {
    const PreserveErrno saved;
    const BusHealth out={prepared.load(std::memory_order_acquire)!=0,used.load(),faults.load()};return out;
}
B::Snapshot read_bus_connection(const void* connection) {
    const PreserveErrno saved;
    if(!prepared.load(std::memory_order_acquire))return unavailable(B::UNOBSERVED);
    if(faults.load())return unavailable(B::FAULT);
    const uint64_t before=version.load();
    if(mutations.load())return unavailable(B::TRANSITION);
    B::Snapshot out=unavailable(B::UNOBSERVED);
    if(Context* c=lookup(connection)) {
        const unsigned phase=c->phase.load();
        out.result=phase==ACTIVE?B::CONNECTED:B::DISCONNECTED;
        out.object=static_cast<uint32_t>(c-&contexts[0])+1;
        if(phase==ACTIVE)out.lifetime=c->lifetime.load();
    }
    if(mutations.load() || version.load()!=before)return unavailable(B::TRANSITION);
    if(faults.load())return unavailable(B::FAULT);
    return out;
}
void observe_position_bus(const void* connection) {
    const PreserveErrno saved;
    const B::Snapshot s=read_bus_connection(connection);
    if(s.result!=B::CONNECTED || !s.object || !s.lifetime)return;
    Context& c=contexts[s.object-1];
    if(c.source_lifetime.load()==s.lifetime)return;
    Mutation mutation(false);
    if(c.phase.load()==ACTIVE && c.lifetime.load()==s.lifetime &&
       c.address.load()==reinterpret_cast<uintptr_t>(connection))c.source_lifetime.store(s.lifetime);
    mutation.complete=true;
}
B::Boundary read_position_bus() {
    const PreserveErrno saved;
    if(!prepared.load(std::memory_order_acquire))return boundary_unavailable(B::UNOBSERVED);
    if(faults.load())return boundary_unavailable(B::FAULT);
    const uint64_t before=version.load();
    if(mutations.load())return boundary_unavailable(B::TRANSITION);
    B::Boundary out=boundary_unavailable(B::UNOBSERVED);
    bool seen=false;unsigned active=0;
    for(unsigned i=0,n=used.load();i<n;++i) {
        Context& c=contexts[i];const uint64_t source=c.source_lifetime.load();
        if(!source)continue;
        seen=true;
        if(c.phase.load()==ACTIVE && c.lifetime.load()==source) {
            ++active;out.connection.result=B::CONNECTED;
            out.connection.object=i+1;out.connection.lifetime=source;
        }
    }
    if(active>1)out=boundary_unavailable(B::AMBIGUOUS);
    else if(!active && seen)out=boundary_unavailable(B::NONE);
    if(mutations.load() || version.load()!=before)return boundary_unavailable(B::TRANSITION);
    if(faults.load())return boundary_unavailable(B::FAULT);
    out.revision=before;return out;
}
} }
extern "C" void* mx5_bus_create(A::BusClosed next,void* user) {
    ready();Mutation mutation;
    A::BusClosed wrapper=next;Context* c;
    { const PreserveErrno saved;c=reserve(next,user,&wrapper); }
    void* result=original.create(wrapper,user);
    {
        const PreserveErrno saved;
        if(c) {
            if(!result)c->phase.store(ENDED);
            else {
                if(lookup(result))fault(A::BUS_COLLISION);
                c->address.store(reinterpret_cast<uintptr_t>(result));
                unsigned phase=CREATING;
                c->phase.compare_exchange_strong(phase,IDLE,std::memory_order_seq_cst,std::memory_order_seq_cst);
            }
        }
        mutation.complete=true;
    }
    return result;
}
extern "C" int32_t mx5_bus_connect(void* connection,const char* name,int32_t type,uintptr_t callback_word) {
    ready();Mutation mutation;Context* c;uint64_t id=0;
    {
        const PreserveErrno saved;c=lookup(connection);
        if(c) {
            c->phase.store(CONNECTING);
            const uint64_t before=next_lifetime.fetch_add(1);
            if(before==UINT64_MAX)fault(A::BUS_EXHAUSTED);else id=before+1;
            c->lifetime.store(id);
        }
    }
    const int32_t result=original.connect(connection,name,type,callback_word);
    {
        const PreserveErrno saved;
        if(c) {
            unsigned phase=CONNECTING;
            c->phase.compare_exchange_strong(phase,result && id?ACTIVE:IDLE,
                    std::memory_order_seq_cst,std::memory_order_seq_cst);
        }
        mutation.complete=true;
    }
    return result;
}
extern "C" void mx5_bus_disconnect(void* connection) {
    ready();
    // Original conn_free calls conn_disconnect internally after retirement.
    Mutation mutation(!connection || freeing!=connection);
    { const PreserveErrno saved;if(Context* c=lookup(connection))c->phase.store(IDLE); }
    original.disconnect(connection);mutation.complete=true;
}
extern "C" void mx5_bus_free(void* connection) {
    ready();Mutation mutation;FreeScope scope(connection);
    { const PreserveErrno saved;if(Context* c=lookup(connection))c->phase.store(ENDED); }
    original.free(connection);mutation.complete=true;
}
extern "C" int32_t mx5_bus_signal(void* connection,void* message) {
    ready();bool disconnected;
    {
        const PreserveErrno saved;
        disconnected=message && original.is_signal(message,"org.freedesktop.DBus.Local","Disconnected");
    }
    if(!disconnected)return original.signal(connection,message);
    // The stock signal handler consumes every signal before its later core
    // filter can deliver the close callback. Observe the actual message here;
    // never call that callback ourselves or change the handler's return value.
    Mutation mutation(false);
    { const PreserveErrno saved;if(Context* c=lookup(connection))c->phase.store(IDLE); }
    const int32_t result=original.signal(connection,message);
    mutation.complete=true;return result;
}
