#include "bus_hooks.h"
#include "adapter.h"
#include <atomic>
#include <errno.h>
#include <string.h>

#if defined(__arm__) && !defined(__EXCEPTIONS)
#error "Bus wrappers require exception cleanup support"
#endif
namespace {
namespace A=mx5::adapter;
namespace B=mx5::runtime::bus_trace;
namespace R=mx5::runtime::request_trace;
static_assert(ATOMIC_INT_LOCK_FREE==2 && ATOMIC_LLONG_LOCK_FREE==2 && ATOMIC_POINTER_LOCK_FREE==2,
              "Bus observation requires lock-free atomics");
enum Phase { UNUSED, CREATING, IDLE, CONNECTING, ACTIVE, ENDED };
struct AtomicText {
    // atomic<T>'s default constructor does not initialize T in C++11. An
    // explicit constant-initialized element also prevents a later DSO dynamic
    // constructor from erasing connection observations made by early dlopen.
    struct Byte {
        std::atomic<unsigned char> value;
        constexpr Byte():value(0) {}
    };
    Byte bytes[R::Text::CAPACITY];
    std::atomic<unsigned> flags;
    constexpr AtomicText():bytes(),flags(0) {}
    void store(const R::Text& text) {
        for(unsigned i=0;i<R::Text::CAPACITY;++i)bytes[i].value.store(static_cast<unsigned char>(text.bytes[i]));
        flags.store((text.known?1u:0u)|(text.complete?2u:0u));
    }
    R::Text load() const {
        R::Text text=R::Text();
        for(unsigned i=0;i<R::Text::CAPACITY;++i)text.bytes[i]=static_cast<char>(bytes[i].value.load());
        const unsigned f=flags.load();text.known=(f&1)!=0;text.complete=(f&2)!=0;return text;
    }
};
static_assert(ATOMIC_CHAR_LOCK_FREE==2,"Endpoint copies require lock-free byte storage");
struct Context {
    std::atomic<unsigned> phase;
    std::atomic<uintptr_t> address;
    std::atomic<uint64_t> lifetime;
    std::atomic<uint64_t> source_lifetime;
    std::atomic<uint64_t> endpoint_lifetime;
    std::atomic<uintptr_t> raw_key;
    AtomicText server_guid,unique_name;
    A::BusClosed next;
    void* user;
    constexpr Context():phase(UNUSED),address(0),lifetime(0),source_lifetime(0),
        endpoint_lifetime(0),raw_key(0),server_guid(),unique_name(),next(0),user(0) {}
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
A::PredictionInvalidator prediction_invalidator=0;
bool attempted;
__thread const void* freeing __attribute__((tls_model("initial-exec")));
struct PreserveErrno {
    const int value;
    PreserveErrno():value(errno) {}
    ~PreserveErrno() { errno=value; }
};
struct ConnectFrame;
__thread ConnectFrame* connecting __attribute__((tls_model("initial-exec")));
struct ConnectFrame {
    ConnectFrame* previous;
    Context* context;
    void* connection;
    uint64_t lifetime;
    uintptr_t raw;
    unsigned registrations;
    bool in_register,captured;
    R::Endpoint endpoint;
    ConnectFrame(Context* c,void* value,uint64_t id):previous(connecting),context(c),
        connection(value),lifetime(id),raw(0),registrations(0),in_register(false),captured(false),endpoint() {
        connecting=this; // Even unobserved/nested connects mask the outer owner.
    }
    ~ConnectFrame() { const PreserveErrno saved;connecting=previous; }
    bool current() const {
        return context && lifetime && context->phase.load()==CONNECTING &&
            context->lifetime.load()==lifetime && context->address.load()==uintptr_t(connection);
    }
};
struct RegisterScope {
    ConnectFrame* frame;
    bool previous;
    explicit RegisterScope(ConnectFrame* f):frame(f),previous(f && f->in_register) {
        if(frame){frame->in_register=true;if(frame->registrations<2)++frame->registrations;frame->captured=false;}
    }
    ~RegisterScope() { const PreserveErrno saved;if(frame)frame->in_register=previous; }
};
struct GuidOwner {
    char* value;
    explicit GuidOwner(char* p):value(p) {}
    ~GuidOwner() { const PreserveErrno saved;if(value)original.endpoint.free_guid(value); }
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
        if(prediction_invalidator)prediction_invalidator();
    }
    ~Mutation() {
        const PreserveErrno saved;
        if(!complete)fault(A::BUS_UNWIND);
        if(prediction_invalidator)prediction_invalidator();
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
bool prepare_bus_hooks(const BusBindings& b,PredictionInvalidator invalidate_prediction) {
    const PreserveErrno saved;
    if(attempted || !b.create || !b.connect || !b.disconnect || !b.free || !b.signal || !b.is_signal)return false;
    const BusEndpointApi& e=b.endpoint;
    const bool any=e.registration||e.get_server_id||e.get_unique_name||e.free_guid||e.register_caller;
    if(any && (!e.registration||!e.get_server_id||!e.get_unique_name||!e.free_guid||!e.register_caller))return false;
    attempted=true;original=b;prediction_invalidator=invalidate_prediction;
    prepared.store(1,std::memory_order_release);return true;
}
BusHealth bus_hook_health() {
    const PreserveErrno saved;
    const BusHealth out={prepared.load(std::memory_order_acquire)!=0,used.load(),faults.load()};return out;
}
B::Snapshot read_bus_endpoint(const void* connection,R::Endpoint* endpoint,uintptr_t* raw) {
    const PreserveErrno saved;
    if(endpoint)*endpoint=R::Endpoint();
    if(raw)*raw=0;
    if(!prepared.load(std::memory_order_acquire))return unavailable(B::UNOBSERVED);
    if(faults.load())return unavailable(B::FAULT);
    const uint64_t before=version.load();
    if(mutations.load())return unavailable(B::TRANSITION);
    B::Snapshot out=unavailable(B::UNOBSERVED);
    R::Endpoint owned=R::Endpoint();uintptr_t key=0;
    if(Context* c=lookup(connection)) {
        const unsigned phase=c->phase.load();
        out.result=phase==ACTIVE?B::CONNECTED:B::DISCONNECTED;
        out.object=static_cast<uint32_t>(c-&contexts[0])+1;
        if(phase==ACTIVE) {
            out.lifetime=c->lifetime.load();
            if(out.lifetime && c->endpoint_lifetime.load()==out.lifetime) {
                if(endpoint) { owned.server_guid=c->server_guid.load();owned.unique_name=c->unique_name.load(); }
                key=c->raw_key.load();
            }
        }
    }
    if(mutations.load() || version.load()!=before)return unavailable(B::TRANSITION);
    if(faults.load())return unavailable(B::FAULT);
    if(endpoint)*endpoint=owned;
    if(raw)*raw=key;
    return out;
}
B::Snapshot read_bus_connection(const void* connection) { return read_bus_endpoint(connection,0,0); }
EndpointMatch bus_endpoint_matches(const void* connection,const B::Snapshot& issue,uintptr_t raw) {
    if(!raw || issue.result!=B::CONNECTED || !issue.object || !issue.lifetime)return ENDPOINT_UNAVAILABLE;
    uintptr_t current=0;const B::Snapshot now=read_bus_endpoint(connection,0,&current);
    if(now.result!=B::CONNECTED || !current)return ENDPOINT_UNAVAILABLE;
    return now.object==issue.object && now.lifetime==issue.lifetime && current==raw?ENDPOINT_MATCH:ENDPOINT_MISMATCH;
}
void observe_position_bus(const void* connection) {
    const PreserveErrno saved;
    if(!prepared.load(std::memory_order_acquire) || faults.load())return;
    // Pin the never-reused slot. Another source marker is not a lifecycle
    // boundary and must not make this completed submission disappear.
    Context* const c=lookup(connection);
    if(!c || c->phase.load()!=ACTIVE)return;
    const uint64_t lifetime=c->lifetime.load();
    uint64_t source=c->source_lifetime.load();
    if(!lifetime || source>=lifetime)return;
    Mutation mutation(false);
    if(c->phase.load()==ACTIVE && c->lifetime.load()==lifetime &&
       c->address.load()==reinterpret_cast<uintptr_t>(connection)) {
        // A delayed older-lifetime marker cannot overwrite a newer marker.
        // With strong CAS each failure raises source; this is lock-free,
        // not a constant-time or wait-free bound.
        while(source<lifetime && !c->source_lifetime.compare_exchange_strong(
                source,lifetime,std::memory_order_seq_cst,std::memory_order_seq_cst)) {}
    }
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
extern "C" int32_t mx5_bus_register(void* raw,void* error) {
    ready();ConnectFrame* const frame=connecting;RegisterScope scope(frame);
    // The exact original call owns JCIDBUS's mutex and its live raw pointer.
    // Do not inspect +0x268 after outer connect: disconnect may leave it stale.
    const uintptr_t caller=reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    bool matched=false;
    {
        const PreserveErrno saved;
        if(frame && !scope.previous && frame->current() && caller==original.endpoint.register_caller) {
            uint32_t actual=0;memcpy(&actual,static_cast<char*>(frame->connection)+0x268,4);
            matched=raw && uintptr_t(actual)==uintptr_t(raw);
        }
    }
    const int32_t result=original.endpoint.registration(raw,error);
    const PreserveErrno saved;
    if(result && matched && frame->registrations==1 && frame->current()) {
        // Setup-only getter allocation/locks; no getter occurs on request/send.
        GuidOwner guid(original.endpoint.get_server_id(raw));
        R::Endpoint owned=R::Endpoint();owned.server_guid=R::copy_text(guid.value);
        owned.unique_name=R::copy_text(original.endpoint.get_unique_name(raw));
        if(frame->registrations==1 && frame->current()) {
            frame->endpoint=owned;frame->raw=uintptr_t(raw);frame->captured=true;
        }
    }
    return result;
}
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
    ConnectFrame frame(c,connection,id);
    const int32_t result=original.connect(connection,name,type,callback_word);
    {
        const PreserveErrno saved;
        if(c && frame.current()) {
            // All shared bytes are atomic: a reconnecting writer racing a
            // reader is not a C++ data race hidden behind version checks.
            c->endpoint_lifetime.store(0);
            if(result && frame.captured && frame.registrations==1) {
                c->server_guid.store(frame.endpoint.server_guid);c->unique_name.store(frame.endpoint.unique_name);
                c->raw_key.store(frame.raw);c->endpoint_lifetime.store(id);
            }
            unsigned phase=CONNECTING;
            c->phase.compare_exchange_strong(phase,result?ACTIVE:IDLE,
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
    ready();bool disconnected;Context* context;
    {
        const PreserveErrno saved;
        // Keep the immutable observation slot across foreign classification.
        // A freed address may have a new owner when that call returns.
        context=message?lookup(connection):0;
        disconnected=message && original.is_signal(message,"org.freedesktop.DBus.Local","Disconnected");
    }
    if(!disconnected)return original.signal(connection,message);
    // The stock signal handler consumes every signal before its later core
    // filter can deliver the close callback. Observe the actual message here;
    // never call that callback ourselves or change the handler's return value.
    Mutation mutation(false);
    {
        const PreserveErrno saved;
        if(context) {
            unsigned phase=context->phase.load(std::memory_order_acquire);
            if(phase!=ENDED && !context->phase.compare_exchange_strong(phase,IDLE,
                    std::memory_order_seq_cst,std::memory_order_seq_cst))fault(A::BUS_CONTENTION);
        }
    }
    const int32_t result=original.signal(connection,message);
    mutation.complete=true;return result;
}
