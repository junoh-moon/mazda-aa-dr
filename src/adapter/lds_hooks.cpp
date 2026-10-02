#include "lds_hooks.h"
#include "bus_hooks.h"
#include <atomic>
#include <errno.h>
#include <new>
#include <string.h>

namespace {
namespace A=mx5::adapter;
namespace S=mx5::runtime::lds_sideband;
namespace L=mx5::sensors::lds_lineage;
namespace R=mx5::runtime::request_trace;
namespace B=mx5::runtime::bus_trace;
namespace T=mx5::sensors::nmea_course_token;
A::LdsBindings original;
std::atomic<unsigned> preparation(0); // 0 empty, 1 copying, 2 prepared, 3 active.
alignas(L::Ledger) unsigned char ledger_storage[sizeof(L::Ledger)];
L::Ledger* ledger;

struct Errno {
    const int saved;
    Errno():saved(errno) {}
    ~Errno() { errno=saved; }
};
void ready() {
    if(preparation.load(std::memory_order_acquire)<2)__builtin_trap();
}
bool active() { return preparation.load(std::memory_order_acquire)==3; }

// Low two bits: 1 initializing, 2 observed normal initialization, 3 unknown.
// These are observation lifetimes, not producer/session measurement epochs.
// UINT_MAX is permanent exhaustion. No ledger operation occurs outside the
// already-held original cache mutex, including applying this deferred reset.
std::atomic<unsigned> lifecycle(0),cache_users(0);
unsigned applied_lifetime;
// Observation boundary only, with the same busy/known/unknown low-bit states.
// No thread writes another thread's pending parse payload.
std::atomic<uint32_t> input_lifetime(2),input_users(0);
struct InputBoundary {
    uint32_t token;
    InputBoundary():token(0) {
        const uint32_t users=input_users.fetch_add(1,std::memory_order_acq_rel);
        if(users==UINT32_MAX) { input_lifetime.store(UINT32_MAX,std::memory_order_release);return; }
        uint32_t before=input_lifetime.load(std::memory_order_acquire);
        if(before>=UINT32_MAX-3) { input_lifetime.store(UINT32_MAX,std::memory_order_release);return; }
        const uint32_t next=((before>>2)+1)*4+1;
        if(!input_lifetime.compare_exchange_strong(before,next,std::memory_order_acq_rel,std::memory_order_acquire)) {
            input_lifetime.fetch_or(3,std::memory_order_acq_rel);return;
        }
        token=next;
        if(users || (before&3)==1)input_lifetime.fetch_or(3,std::memory_order_acq_rel);
    }
    void completed() {
        if(!token)return;
        if(input_users.load(std::memory_order_acquire)!=1) {
            input_lifetime.fetch_or(3,std::memory_order_acq_rel);return;
        }
        uint32_t expected=token;
        input_lifetime.compare_exchange_strong(expected,token+1,std::memory_order_acq_rel,std::memory_order_acquire);
        token=0;
    }
    ~InputBoundary() {
        const Errno saved;
        if(token) {
            uint32_t expected=token;
            input_lifetime.compare_exchange_strong(expected,token+2,std::memory_order_acq_rel,std::memory_order_acquire);
        }
        input_users.fetch_sub(1,std::memory_order_acq_rel);
    }
};
void unknown_lifecycle() { lifecycle.fetch_or(3,std::memory_order_acq_rel); }
struct Lifecycle {
    unsigned token;
    explicit Lifecycle(bool initializing):token(0) {
        unsigned before=lifecycle.load(std::memory_order_acquire);
        if(before>=UINT32_MAX-3) { lifecycle.store(UINT32_MAX,std::memory_order_release);return; }
        const unsigned next=((before>>2)+1)*4+(initializing?1:3);
        if(!lifecycle.compare_exchange_strong(before,next,std::memory_order_acq_rel)) {
            unknown_lifecycle();return;
        }
        token=next;
        if(cache_users.load(std::memory_order_acquire) || (before&3)==1)unknown_lifecycle();
    }
    void completed() {
        if(!token || (token&3)!=1)return;
        if(cache_users.load(std::memory_order_acquire)) { unknown_lifecycle();return; }
        unsigned expected=token;
        lifecycle.compare_exchange_strong(expected,token+1,std::memory_order_acq_rel);
        token=0;
    }
    ~Lifecycle() {
        const Errno saved;
        if(token && (token&3)==1) {
            unsigned expected=token;
            lifecycle.compare_exchange_strong(expected,token+2,std::memory_order_acq_rel);
        }
    }
};
bool cache_ready() {
    const unsigned state=lifecycle.load(std::memory_order_acquire);
    if((state&3)!=2)return false;
    const unsigned epoch=state>>2;
    if(epoch!=applied_lifetime) {
        if(!ledger->begin_lifetime(epoch))return false;
        applied_lifetime=epoch;
    }
    return true;
}
L::Snapshot cache_snapshot() { return cache_ready()?ledger->snapshot():L::Snapshot(); }
uint64_t now() { return original.clock?original.clock(original.user):0; }

struct CallbackFrame;
struct Operation;
struct PathFrame;
struct GenericFrame;
struct SendFrame;
__thread CallbackFrame* callback_frame __attribute__((tls_model("initial-exec")));
__thread Operation* operation __attribute__((tls_model("initial-exec")));
__thread PathFrame* path_frame __attribute__((tls_model("initial-exec")));
__thread GenericFrame* generic_frame __attribute__((tls_model("initial-exec")));
__thread SendFrame* send_frame __attribute__((tls_model("initial-exec")));
struct Hold { bool held; Operation* owner; };
__thread Hold hold __attribute__((tls_model("initial-exec")));
enum { INPUT_RUNNING=1,INPUT_READY=2,INPUT_POISONED=4,INPUT_EXHAUSTED=8 };
struct PendingInput {
    void* workspace;
    uint32_t lifetime,sequence,route;
    T::Presence presence;
    T::RmcStatus status;
    uint32_t state;
};
static_assert(sizeof(PendingInput)<=32,"Only small parser ownership state may live in TLS");
__thread PendingInput pending_input __attribute__((tls_model("initial-exec")));

void poison_input() {
    pending_input.workspace=0;
    pending_input.presence=T::UNKNOWN;
    pending_input.status=T::RMC_UNKNOWN;
    pending_input.state=(pending_input.state&INPUT_EXHAUSTED)|INPUT_POISONED;
}
struct ParseInput {
    uint32_t sequence,lifetime;
    bool returned;
    ParseInput():sequence(0),lifetime(input_lifetime.load(std::memory_order_acquire)),returned(false) {
        const bool uncertain=pending_input.state || callback_frame;
        if(pending_input.sequence==UINT32_MAX)pending_input.state|=INPUT_EXHAUSTED;
        else sequence=++pending_input.sequence;
        const uint32_t exhausted=pending_input.state&INPUT_EXHAUSTED;
        pending_input.workspace=0;pending_input.presence=T::UNKNOWN;pending_input.status=T::RMC_UNKNOWN;
        pending_input.lifetime=lifetime;
        pending_input.state=INPUT_RUNNING|exhausted|(uncertain?INPUT_POISONED:0);
    }
    void completed(bool verified,void* workspace,uint32_t route,T::Tokens tokens) {
        returned=true;
        if(!sequence || pending_input.sequence!=sequence || pending_input.state!=INPUT_RUNNING) {
            poison_input();return;
        }
        pending_input.state=0;
        // The exact reader dispatches only original IDs 0..8. Only RMC's
        // two IDs carry these lexical tokens; neither PRESENT nor A is validity.
        if(!verified || !workspace || route>8 || !original.routes[route].callback ||
           (lifetime&3)!=2 || input_lifetime.load(std::memory_order_acquire)!=lifetime)return;
        pending_input.workspace=workspace;pending_input.route=route;
        pending_input.presence=route==0 || route==6?tokens.course:T::UNKNOWN;
        pending_input.status=route==0 || route==6?tokens.status:T::RMC_UNKNOWN;
        pending_input.state=INPUT_READY;
    }
    ~ParseInput() { const Errno saved;if(!returned)poison_input(); }
};

struct CallbackFrame {
    CallbackFrame* previous;
    const A::LdsRoute& route;
    L::Snapshot read;
    void* destination;
    bool have_read;
    T::Presence heading_presence;
    T::RmcStatus heading_rmc_status;
    uint32_t input_sequence,input_epoch;
    CallbackFrame(const A::LdsRoute& r,unsigned id,void* workspace,uintptr_t caller):previous(callback_frame),route(r),read(),destination(0),have_read(false),heading_presence(T::UNKNOWN),heading_rmc_status(T::RMC_UNKNOWN),input_sequence(0),input_epoch(0) {
        if(!previous && pending_input.state==INPUT_READY &&
           caller==original.input.dispatch_return && workspace==pending_input.workspace &&
           id==pending_input.route && input_lifetime.load(std::memory_order_acquire)==pending_input.lifetime) {
            heading_presence=pending_input.presence;heading_rmc_status=pending_input.status;
            input_sequence=pending_input.sequence;
            input_epoch=pending_input.lifetime;pending_input.state=0;
            pending_input.workspace=0;pending_input.presence=T::UNKNOWN;pending_input.status=T::RMC_UNKNOWN;
        } else if(previous || pending_input.state)poison_input();
        callback_frame=this;
    }
    T::Tokens take_input() {
        T::Tokens result={T::UNKNOWN,T::RMC_UNKNOWN};
        if(callback_frame==this && input_sequence && pending_input.sequence==input_sequence &&
           pending_input.state==0 && input_lifetime.load(std::memory_order_acquire)==input_epoch)
            result=T::Tokens{heading_presence,heading_rmc_status};
        // Both values are owned by the same invocation and burned together.
        input_sequence=0;heading_presence=T::UNKNOWN;heading_rmc_status=T::RMC_UNKNOWN;
        return result;
    }
    ~CallbackFrame() { const Errno saved;callback_frame=previous; }
};
enum Kind { READ,WRITE,SERVICE };
struct Operation {
    Operation* previous;
    Kind kind;
    const void* buffer;
    bool verified,copied,unlocked;
    L::Snapshot read;
    uint32_t mask;
    CallbackFrame* input_callback;
    PathFrame* response;
    int32_t *mode,*altitude;
    uint64_t* utc;
    double *latitude,*longitude,*heading,*velocity,*horizontal,*vertical;
    Operation(Kind k,const void* b):previous(operation),kind(k),buffer(b),verified(false),copied(false),unlocked(false),read(),mask(0),input_callback(0),response(0),mode(0),altitude(0),utc(0),latitude(0),longitude(0),heading(0),velocity(0),horizontal(0),vertical(0) {
        operation=this;
        if(cache_users.fetch_add(1,std::memory_order_acq_rel)==UINT32_MAX)
            lifecycle.store(UINT32_MAX,std::memory_order_release);
        if((lifecycle.load(std::memory_order_acquire)&3)==1)unknown_lifecycle();
    }
    ~Operation() {
        const Errno saved;
        if(hold.owner==this)hold.owner=0;
        operation=previous;cache_users.fetch_sub(1,std::memory_order_acq_rel);
    }
};
struct PathFrame {
    PathFrame* previous;
    GenericFrame* outer_generic;
    void *raw_connection,*request,*connection,*method,*reply,*message;
    unsigned builds,replies,messages,sends;
    bool eligible,published,invalidated,returned;
    B::Snapshot bus;
    R::Endpoint endpoint;
    A::EndpointMatch pre_match;
    S::Record record;
    PathFrame(void* raw,void* req,void* c):previous(path_frame),outer_generic(generic_frame),raw_connection(raw),request(req),connection(c),method(0),reply(0),message(0),builds(0),replies(0),messages(0),sends(0),eligible(false),published(false),invalidated(false),returned(false),bus(),endpoint(),pre_match(A::ENDPOINT_UNAVAILABLE),record() {
        path_frame=this;generic_frame=0;
        if(!req)return;
        const char* p=original.raw.path(req);
        const char* i=original.raw.interface_name(req);
        const char* m=original.raw.member(req);
        eligible=original.raw.type(req)==1 && p && i && m &&
            !strcmp(p,"/com/jci/lds/data") && !strcmp(i,"com.jci.lds.data") && !strcmp(m,"GetPosition");
        if(!eligible)return;
        record.flags|=S::REQUEST_KNOWN;
        record.wire.request_serial=original.raw.serial(req);
        S::copy_text(&record.wire.client_unique,original.raw.sender(req));
        uintptr_t key=0;
        bus=A::read_bus_endpoint(c,&endpoint,&key);
        pre_match=A::bus_endpoint_matches(c,bus,uintptr_t(raw));
        if(pre_match==A::ENDPOINT_MISMATCH)record.flags|=S::CHAIN_CONFLICT;
    }
    void invalidate(A::LdsLockedLoss loss) {
        if(published && !invalidated) { invalidated=true;original.invalidate_locked(loss,original.user); }
    }
    void conflict() { record.flags|=S::CHAIN_CONFLICT;invalidate(A::LOCKED_CHAIN_CONFLICT); }
    ~PathFrame() {
        const Errno saved;
        if(!returned)invalidate(A::LOCKED_CHAIN_CONFLICT);
        path_frame=previous;generic_frame=outer_generic;
    }
};
bool text_complete(const R::Text& text) {
    return text.known && text.complete && text.bytes[0] && memchr(text.bytes,0,sizeof text.bytes);
}
void* connection_mutex(void* connection) {
    void* value=0;
    if(connection)memcpy(&value,static_cast<const char*>(connection)+original.send_sites.connection_mutex_offset,sizeof value);
    return value;
}
struct SendFrame {
    SendFrame* previous;
    PathFrame* path;
    void *connection,*message,*mutex;
    uint32_t generation;
    unsigned native_calls,message_calls,ordinal;
    int32_t native_result;
    bool eligible,returned;
    SendFrame(void* c,void* m):previous(send_frame),path(path_frame),connection(c),message(m),mutex(0),
        generation(0),native_calls(0),message_calls(0),ordinal(0),native_result(-1),eligible(false),returned(false) {
        // Even an unrelated nested send masks the outer frame.
        send_frame=this;
        if(!active() || !path || !path->eligible)return;
        ordinal=++path->sends;
        if(ordinal!=1 || c!=path->raw_connection || !m || m!=path->message)path->conflict();
        if(!original.publish_locked || (path->record.flags&S::CHAIN_CONFLICT) ||
           !(path->record.flags&S::SNAPSHOT_KNOWN) || path->builds!=1 || path->replies!=1 ||
           path->messages!=1 || path->sends!=1 || path->pre_match!=A::ENDPOINT_MATCH ||
           !path->record.wire.request_serial || !text_complete(path->record.wire.client_unique) ||
           !text_complete(path->endpoint.server_guid) || !text_complete(path->endpoint.unique_name))return;
        generation=__atomic_load_n(original.send_sites.current_generation,__ATOMIC_ACQUIRE);
        const uint32_t initialized=__atomic_load_n(original.send_sites.initialized_generation,__ATOMIC_ACQUIRE);
        mutex=connection_mutex(c);
        eligible=generation && generation==initialized && mutex &&
            uintptr_t(mutex)!=original.send_sites.uninitialized_mutex;
    }
    bool same() const { return eligible && send_frame==this && path_frame==path; }
    bool unchanged() const {
        return same() && connection_mutex(connection)==mutex &&
            __atomic_load_n(original.send_sites.current_generation,__ATOMIC_ACQUIRE)==generation &&
            __atomic_load_n(original.send_sites.initialized_generation,__ATOMIC_ACQUIRE)==generation;
    }
    ~SendFrame() {
        const Errno saved;
        if(!returned && path)path->invalidate(A::LOCKED_SEND_FAILED);
        send_frame=previous;
    }
};
struct GenericFrame {
    GenericFrame* previous;
    PathFrame* response;
    explicit GenericFrame(PathFrame* p):previous(generic_frame),response(p) { generic_frame=this; }
    ~GenericFrame() { const Errno saved;generic_frame=previous; }
};

template<unsigned Id> void callback(void* value) {
    const uintptr_t caller=uintptr_t(__builtin_return_address(0));
    ready();
    CallbackFrame scope(original.routes[Id],Id,value,caller);
    original.routes[Id].callback(value);
}
const A::LdsCallback callback_wrappers[12]={callback<0>,callback<1>,callback<2>,callback<3>,callback<4>,callback<5>,callback<6>,callback<7>,callback<8>,callback<9>,callback<10>,callback<11>};

void service(void* c,int32_t* mode,uint64_t* utc,double* lat,double* lon,int32_t* alt,
             double* heading,double* velocity,double* horizontal,double* vertical) {
    Operation scope(SERVICE,0);
    scope.mode=mode;scope.utc=utc;scope.latitude=lat;scope.longitude=lon;scope.altitude=alt;
    scope.heading=heading;scope.velocity=velocity;scope.horizontal=horizontal;scope.vertical=vertical;
    if(generic_frame)scope.response=generic_frame->response;
    original.service(c,mode,utc,lat,lon,alt,heading,velocity,horizontal,vertical);
}
int32_t generic(void* c,void* m,void* r,void* d) {
    ready();
    PathFrame* p=path_frame;
    const bool matched=active() && d==original.descriptor && p && p->eligible &&
        p->connection==c && m && p->method==m && r && p->reply==r;
    GenericFrame scope(matched?p:0);
    if(d!=original.descriptor)return original.generic(c,m,r,d);
    A::LdsDescriptor copy=*original.descriptor;
    copy.service=service;
    return original.generic(c,m,r,&copy);
}
void snapshot(Operation& op) {
    PathFrame* p=op.response;
    if(!p || p!=path_frame || !op.mode || !op.utc || !op.latitude || !op.longitude ||
       !op.altitude || !op.heading || !op.velocity || !op.horizontal || !op.vertical)return;
    if(p->record.flags&S::SNAPSHOT_KNOWN) { p->conflict();return; }
    A::PositionInput& value=p->record.position;
    value.mode=*op.mode;value.utc_seconds=*op.utc;value.latitude_deg=*op.latitude;
    value.longitude_deg=*op.longitude;value.altitude_m=*op.altitude;
    value.heading_deg=*op.heading;value.velocity_kmh=*op.velocity;
    value.horizontal=*op.horizontal;value.vertical=*op.vertical;
    const L::Snapshot lineage=cache_snapshot();
    p->record.field_lineage.lifetime=lineage.lifetime;
    p->record.field_lineage.write_sequence=lineage.write_sequence;
    p->record.field_lineage.heading_presence=lineage.heading_presence;
    p->record.field_lineage.heading_rmc_status=lineage.heading_rmc_status;
    for(unsigned i=0;i<L::FIELD_COUNT;++i)p->record.field_lineage.fields[i]=lineage.fields[i];
    p->record.flags|=S::SNAPSHOT_KNOWN;
}
bool complete(const A::LdsBindings& b) {
    // Data slots are patched even when the optional publisher is unavailable.
    // Immutable original targets must therefore always be forwardable.
    if(!b.message_lock || !b.native_mutex_lock)return false;
    if(!b.input.parse_sentence || !b.input.select || !b.input.driver_close ||
       !b.input.parse_return || !b.input.select_return || !b.input.dispatch_return)return false;
    if(bool(b.publish_locked)!=bool(b.invalidate_locked))return false;
    if(b.publish_locked && (!b.send_sites.message_lock_return || !b.send_sites.native_lock_return ||
       !b.send_sites.current_generation || !b.send_sites.initialized_generation))return false;
    if(!b.initialize || !b.clear || !b.driver_open || !b.registration || !b.read || !b.update ||
       !b.lock || !b.unlock || !b.copy || !b.set_callback || !b.generic || !b.service || !b.path ||
       !b.method_build || !b.reply_create || !b.reply_message || !b.send || !b.descriptor ||
       !b.current_cache || !b.current_mutex || !b.emit || !b.raw.type || !b.raw.serial ||
       !b.raw.reply_serial || !b.raw.sender || !b.raw.destination || !b.raw.path ||
       !b.raw.interface_name || !b.raw.member)return false;
    if(!b.sites.read_lock || !b.sites.read_copy || !b.sites.read_unlock || !b.sites.update_lock ||
       !b.sites.update_copy || !b.sites.update_unlock || !b.sites.service_lock || !b.sites.service_unlock)return false;
    for(unsigned i=0;i<12;++i) {
        const A::LdsRoute& r=b.routes[i];
        if(r.callback && (!r.read_caller || !r.update_caller || (r.assigned_mask&~L::ALL_FIELDS)))return false;
        if(!r.callback && (r.read_caller || r.update_caller || r.assigned_mask))return false;
    }
    return b.descriptor->service==b.service && b.descriptor->generic==b.generic;
}
}

namespace mx5 { namespace adapter {
bool prepare_lds_hooks(const LdsBindings& b) {
    const Errno saved;
    if(!complete(b))return false;
    unsigned empty=0;
    if(!preparation.compare_exchange_strong(empty,1,std::memory_order_acq_rel))return false;
    original=b;ledger=new(ledger_storage)L::Ledger;
    preparation.store(2,std::memory_order_release);return true;
}
bool activate_lds_hooks() {
    const Errno saved;
    unsigned expected=2;
    return preparation.compare_exchange_strong(expected,3,std::memory_order_acq_rel);
}
} }

extern "C" void mx5_lds_initialize() {
    ready();if(!active()) { original.initialize();return; }
    InputBoundary input;Lifecycle scope(true);original.initialize();
    const Errno saved;scope.completed();input.completed();
}
extern "C" void mx5_lds_clear() {
    ready();if(!active()) { original.clear();return; }
    InputBoundary input;Lifecycle scope(false);original.clear();
    const Errno saved;input.completed();
}
extern "C" int32_t mx5_lds_driver_open(uintptr_t word) {
    ready();if(!active())return original.driver_open(word);
    InputBoundary input;const int32_t result=original.driver_open(word);
    const Errno saved;input.completed();return result;
}
extern "C" void mx5_lds_driver_close() {
    ready();if(!active()) { original.input.driver_close();return; }
    InputBoundary input;original.input.driver_close();
    const Errno saved;input.completed();
}
extern "C" uint32_t mx5_lds_parse_sentence(char* input,void* workspace,uint32_t word) {
    const uintptr_t caller=uintptr_t(__builtin_return_address(0));
    ready();if(!active())return original.input.parse_sentence(input,workspace,word);
    const int entry_errno=errno;
    ParseInput scope;
    const bool verified=caller==original.input.parse_return;
    T::Tokens tokens={T::UNKNOWN,T::RMC_UNKNOWN};
    // The exact original reader calls with an assembled NUL-terminated string.
    // Its third word is preserved, not promoted into a readable-buffer length.
    if(verified && input && workspace) {
        size_t size=0;
        while(size<T::SCAN_LIMIT && input[size])++size;
        if(size<T::SCAN_LIMIT)tokens=T::classify_rmc(input,size+1);
    }
    errno=entry_errno;
    const uint32_t result=original.input.parse_sentence(input,workspace,word);
    const Errno saved;scope.completed(verified,workspace,result,tokens);return result;
}
extern "C" int mx5_lds_select(int n,fd_set* r,fd_set* w,fd_set* e,timeval* timeout) {
    const uintptr_t caller=uintptr_t(__builtin_return_address(0));
    ready();
    if(active() && caller==original.input.select_return) {
        const Errno saved;
        if(callback_frame || (pending_input.state&INPUT_RUNNING))poison_input();
        else {
            pending_input.workspace=0;pending_input.presence=T::UNKNOWN;pending_input.status=T::RMC_UNKNOWN;
            pending_input.state&=INPUT_EXHAUSTED;
        }
    }
    return original.input.select(n,r,w,e,timeout);
}
extern "C" int32_t mx5_lds_register(uint32_t id,A::LdsCallback fn) {
    ready();
    if(active() && id<12 && fn && fn==original.routes[id].callback)fn=callback_wrappers[id];
    return original.registration(id,fn);
}
extern "C" int32_t mx5_lds_read(void* buffer) {
    const uintptr_t caller=uintptr_t(__builtin_return_address(0));
    ready();if(!active())return original.read(buffer);
    Operation scope(READ,buffer);
    CallbackFrame* const callback=callback_frame;
    scope.verified=callback && caller==callback->route.read_caller;
    if(callback)callback->have_read=false;
    const int32_t result=original.read(buffer);
    const Errno saved;
    if(scope.verified && scope.copied && scope.unlocked && result==100) {
        callback->read=scope.read;callback->destination=buffer;callback->have_read=true;
    }
    return result;
}
extern "C" int32_t mx5_lds_update(const void* buffer) {
    const uintptr_t caller=uintptr_t(__builtin_return_address(0));
    ready();if(!active())return original.update(buffer);
    Operation scope(WRITE,buffer);
    CallbackFrame* const callback=callback_frame;
    scope.verified=callback && callback->have_read && callback->destination==buffer &&
        caller==callback->route.update_caller;
    if(scope.verified) { scope.read=callback->read;scope.mask=callback->route.assigned_mask;scope.input_callback=callback; }
    // One actual read may authorize only its first verified update. It is not
    // ambient permission for a later call that happens to reuse stack storage.
    if(callback)callback->have_read=false;
    return original.update(buffer);
}
extern "C" void mx5_lds_mutex_lock(void* mutex,const char* file,unsigned line) {
    const uintptr_t caller=uintptr_t(__builtin_return_address(0));
    ready();original.lock(mutex,file,line);
    const Errno saved;
    if(!active() || mutex!=original.current_mutex)return;
    hold.held=true;hold.owner=0;
    if(!operation)return;
    const uintptr_t expected=operation->kind==READ?original.sites.read_lock:
        operation->kind==WRITE?original.sites.update_lock:original.sites.service_lock;
    if(caller==expected)hold.owner=operation;
}
extern "C" void mx5_lds_mutex_unlock(void* mutex) {
    const uintptr_t caller=uintptr_t(__builtin_return_address(0));
    ready();
    {
        const Errno saved;
        if(active() && mutex==original.current_mutex && hold.held) {
            if(hold.owner && hold.owner==operation) {
                const uintptr_t expected=operation->kind==READ?original.sites.read_unlock:
                    operation->kind==WRITE?original.sites.update_unlock:original.sites.service_unlock;
                if(caller==expected) {
                    if(operation->kind==SERVICE)snapshot(*operation);
                    operation->unlocked=true;
                }
            }
            hold.held=false;hold.owner=0;
        }
    }
    original.unlock(mutex);
}
extern "C" void* mx5_lds_mem_copy(void* dst,const void* src,unsigned bytes) {
    const uintptr_t caller=uintptr_t(__builtin_return_address(0));
    ready();void* result=original.copy(dst,src,bytes);
    const Errno saved;
    if(!active())return result;
    if(dst==original.current_cache && bytes) {
        // A claim belongs to its first actual cache write. A second write,
        // even after another verified read in the same callback, cannot reuse
        // that input. Unverified writes burn the claim without promoting it.
        const T::Tokens claimed=callback_frame?callback_frame->take_input():T::Tokens{T::UNKNOWN,T::RMC_UNKNOWN};
        if(!hold.held) { unknown_lifecycle();return result; }
        if(cache_ready()) {
            const bool matched=operation && hold.owner==operation && operation->kind==WRITE &&
                caller==original.sites.update_copy && src==operation->buffer && bytes==48 && operation->verified;
            const T::Tokens tokens=matched && operation->input_callback==callback_frame?claimed:T::Tokens{T::UNKNOWN,T::RMC_UNKNOWN};
            ledger->commit(matched?&operation->read:0,matched?operation->mask:0,now(),tokens.course,tokens.status);
        }
    } else if(hold.held && operation && hold.owner==operation && operation->kind==READ &&
              caller==original.sites.read_copy && src==original.current_cache &&
              dst==operation->buffer && bytes==48) {
        if(operation->copied)operation->verified=false;
        operation->read=cache_snapshot();operation->copied=true;
    }
    return result;
}
extern "C" int32_t mx5_lds_set_callback(void* object,A::LdsGeneric fn,void* data) {
    ready();
    if(active() && fn==original.generic && data==original.descriptor)fn=generic;
    return original.set_callback(object,fn,data);
}
extern "C" int32_t mx5_lds_path(void* raw,void* message,void* connection) {
    ready();if(!active())return original.path(raw,message,connection);
    const int entry_errno=errno;
    PathFrame scope(raw,message,connection);errno=entry_errno;
    const int32_t result=original.path(raw,message,connection);
    const Errno saved;
    scope.returned=true;
    if(result)scope.invalidate(A::LOCKED_CHAIN_CONFLICT);
    if(scope.eligible && !hold.held) {
        scope.record.flags|=S::PATH_RETURNED;scope.record.path_result=result;
        if(scope.pre_match==A::ENDPOINT_MATCH &&
           A::bus_endpoint_matches(connection,scope.bus,uintptr_t(raw))==A::ENDPOINT_MATCH) {
            scope.record.wire.server_guid=scope.endpoint.server_guid;
            scope.record.wire.server_unique=scope.endpoint.unique_name;
        } else scope.invalidate(A::LOCKED_CHAIN_CONFLICT);
        scope.record.observed_ns=now();original.emit(scope.record,original.user);
    }
    return result;
}
extern "C" void* mx5_lds_method_build(void* request) {
    ready();void* result=original.method_build(request);
    const Errno saved;
    PathFrame* p=path_frame;
    if(p && p->eligible) {
        if(++p->builds!=1 || request!=p->request)p->conflict();
        else p->method=result;
    }
    return result;
}
extern "C" void* mx5_lds_reply_create(void* method,void* request) {
    ready();void* result=original.reply_create(method,request);
    const Errno saved;
    PathFrame* p=path_frame;
    if(p && p->eligible) {
        if(++p->replies!=1 || !method || method!=p->method || request!=p->request)p->conflict();
        else p->reply=result;
    }
    return result;
}
extern "C" void* mx5_lds_reply_message(void* reply,void* request) {
    ready();void* result=original.reply_message(reply,request);
    const Errno saved;
    PathFrame* p=path_frame;
    if(p && p->eligible) {
        if(++p->messages!=1 || !reply || reply!=p->reply || request!=p->request)p->conflict();
        else p->message=result;
    }
    return result;
}
extern "C" int32_t mx5_lds_send(void* connection,void* message,uint32_t* serial) {
    ready();const int entry_errno=errno;
    SendFrame scope(connection,message);errno=entry_errno;
    const int32_t result=original.send(connection,message,serial);
    const Errno saved;
    scope.returned=true;
    PathFrame* p=scope.path;
    if(p && p->eligible) {
        if(!result)p->invalidate(A::LOCKED_SEND_FAILED);
        else if(p->published && !scope.unchanged())p->invalidate(A::LOCKED_CHAIN_CONFLICT);
        // A nested extra send invalidates the association but must not erase
        // the first call's original raw send result from the after-Path record.
        if(scope.ordinal!=1)return result;
        p->record.flags|=S::RAW_SEND_CALLED;
        p->record.send_result=result;
        if(result)p->record.flags|=S::RAW_SEND_SUCCEEDED;
        if(!message)return result;
        p->record.reply_type=original.raw.type(message);
        p->record.wire.response_serial=original.raw.serial(message);
        p->record.wire.reply_serial=original.raw.reply_serial(message);
        S::copy_text(&p->record.wire.destination,original.raw.destination(message));
        p->record.flags|=S::REPLY_KNOWN;
    }
    return result;
}
extern "C" void mx5_lds_message_lock(void* message) {
    ready();
    const uintptr_t caller=uintptr_t(__builtin_extract_return_addr(__builtin_return_address(0)));
    SendFrame* frame=send_frame;
    original.message_lock(message);
    const Errno saved;
    if(!frame || !frame->same() || message!=frame->message ||
       caller!=original.send_sites.message_lock_return)return;
    PathFrame& p=*frame->path;
    if(++frame->message_calls!=1) { p.conflict();return; }
    if(!frame->unchanged() || frame->native_calls!=1 || frame->native_result ||
       (p.record.flags&S::CHAIN_CONFLICT) || p.sends!=1)return;
    const A::EndpointMatch endpoint=A::bus_endpoint_matches(p.connection,p.bus,uintptr_t(frame->connection));
    if(endpoint!=A::ENDPOINT_MATCH) { if(endpoint==A::ENDPOINT_MISMATCH)p.conflict();return; }
    A::LdsLockedSend value=A::LdsLockedSend();
    value.stage=mx5::runtime::lds_association::LOCKED_FOR_SEND;
    value.reply_type=original.raw.type(message);
    value.wire=p.record.wire;
    value.wire.server_guid=p.endpoint.server_guid;
    value.wire.server_unique=p.endpoint.unique_name;
    value.wire.response_serial=original.raw.serial(message);
    value.wire.reply_serial=original.raw.reply_serial(message);
    S::copy_text(&value.wire.destination,original.raw.destination(message));
    if(value.reply_type!=2 || !value.wire.response_serial ||
       value.wire.reply_serial!=value.wire.request_serial || !text_complete(value.wire.destination) ||
       strcmp(value.wire.destination.bytes,value.wire.client_unique.bytes)) { p.conflict();return; }
    value.observed_ns=now();value.field_lineage=p.record.field_lineage;value.position=p.record.position;
    p.published=true;original.publish_locked(value,original.user);
}
extern "C" int32_t mx5_lds_native_mutex_lock(void* mutex) {
    ready();
    const uintptr_t caller=uintptr_t(__builtin_extract_return_addr(__builtin_return_address(0)));
    SendFrame* frame=send_frame;
    const bool match=frame && frame->same() && frame->mutex==mutex &&
        caller==original.send_sites.native_lock_return;
    const int32_t result=original.native_mutex_lock(mutex);
    const Errno saved;
    if(match && frame->same()) {
        ++frame->native_calls;frame->native_result=result;
        if(frame->native_calls!=1)frame->path->conflict();
    }
    return result;
}
