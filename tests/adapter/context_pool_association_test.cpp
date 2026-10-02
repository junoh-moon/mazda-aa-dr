// Supplement the imported context_pool_test with owned LDS associations and
// fork/early-init lifetimes. All identities are authored observations; no
// ASSIST qualification is set. Exhaustion retains the imported sticky fault.
// Thread synchronization is outside callbacks. No adapter implementation is
// included and no private pool/slot/owner state is inspected.
#include "adapter/adapter.h"
#include "runtime/lds_association_protocol.h"
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef NDEBUG
#error Context pool regressions require assertions
#endif
#ifdef MX5_CONTEXT_POOL_ASSOCIATION_DSO_TEST
#include "context_pool_association_dso_access.h"
#endif
namespace A=mx5::adapter;
namespace R=mx5::runtime::request_trace;
namespace L=mx5::runtime::lds_association;
namespace B=mx5::runtime::bus_trace;
namespace S=mx5::sensors::lds_lineage;
namespace C=mx5::sensors::nmea_course_token;
namespace {
const unsigned CAPACITY=64;
const int32_t ORIGINAL_RESULT=-731;
struct Frame {
    unsigned id,reads,associations,provenances,position_count,send_count,original_count;
    uint8_t raw[72],payload[48];
    R::Trace trace;
    L::Owned owned;
    A::Observation position,sent;
    A::VehicleData* borrowed;
    A::Choice send_choice;
};
static __thread Frame* active;
// POD zero initialization is intentional: the constructor(101) test must not
// itself overwrite captured values in a later fixture global constructor.
static Frame early;

void put32(uint8_t* p,uint32_t value) {
    for(unsigned i=0;i<4;++i)p[i]=uint8_t(value>>(i*8));
}
void put64(uint8_t* p,uint64_t value) {
    for(unsigned i=0;i<8;++i)p[i]=uint8_t(value>>(i*8));
}
void put_double(uint8_t* p,double value) {
    uint64_t bits=0;std::memcpy(&bits,&value,sizeof bits);put64(p,bits);
}
R::Text text(unsigned value) {
    R::Text out=R::Text();out.known=out.complete=true;
    for(unsigned i=0;i<R::Text::CAPACITY-1;++i)out.bytes[i]=char('a'+(value+i)%26);
    return out;
}
void initialize(Frame& f,unsigned id) {
    std::memset(&f,0,sizeof f);f.id=id;f.send_choice=A::ORIGINAL;
    put32(f.raw,0);put64(f.raw+8,id);
    put_double(f.raw+16,10.0+id/100000.0);put_double(f.raw+24,20.0+id/100000.0);
    put32(f.raw+32,id);put_double(f.raw+40,id%3?0.0:30.0);put_double(f.raw+48,40.0);
    put_double(f.raw+56,1.25);put_double(f.raw+64,2.5);
    for(unsigned i=0;i<48;++i)f.payload[i]=uint8_t((i+id)%251+1);
    f.trace.request=R::Token{10000+id,20000+id};
    f.trace.worker=R::Token{30000+id,40000+id};
    f.trace.issue.observed_ns=50000+id;
    f.trace.issue.connection=B::Snapshot{B::CONNECTED,id,60000+id};
    f.trace.issue.endpoint.server_guid=text(id);
    f.trace.issue.endpoint.unique_name=text(id+1);
    f.trace.issue.route.destination=text(id+2);f.trace.issue.route.path=text(id+3);
    f.trace.issue.route.interface_name=text(id+4);f.trace.issue.route.member=text(id+5);
    f.trace.issue.wire=R::WireIssue{70000+id,id,true,false,true};
    f.trace.reply.connection=f.trace.issue.connection;
    f.trace.reply.observed_ns=80000+id;
    f.trace.reply.wire.observed_ns=90000+id;f.trace.reply.wire.serial=1000+id;
    f.trace.reply.wire.reply_serial=id;f.trace.reply.wire.type=2;
    f.trace.reply.wire.known=true;f.trace.reply.wire.sender=text(id+6);
}
void same_text(const R::Text& a,const R::Text& b) {
    assert(a.known==b.known&&a.complete==b.complete);
    assert(!std::memcmp(a.bytes,b.bytes,R::Text::CAPACITY));
}
void same_trace(const R::Trace& a,const R::Trace& b) {
    assert(a.request.id==b.request.id&&a.request.epoch==b.request.epoch);
    assert(a.worker.id==b.worker.id&&a.worker.epoch==b.worker.epoch);
    assert(a.issue.observed_ns==b.issue.observed_ns);
    assert(a.issue.connection.result==b.issue.connection.result);
    assert(a.issue.connection.object==b.issue.connection.object);
    assert(a.issue.connection.lifetime==b.issue.connection.lifetime);
    same_text(a.issue.endpoint.server_guid,b.issue.endpoint.server_guid);
    same_text(a.issue.endpoint.unique_name,b.issue.endpoint.unique_name);
    same_text(a.issue.route.destination,b.issue.route.destination);
    same_text(a.issue.route.path,b.issue.route.path);
    same_text(a.issue.route.interface_name,b.issue.route.interface_name);
    same_text(a.issue.route.member,b.issue.route.member);
    assert(a.issue.wire.observed_ns==b.issue.wire.observed_ns);
    assert(a.issue.wire.serial==b.issue.wire.serial&&a.issue.wire.known==b.issue.wire.known);
    assert(a.issue.wire.conflict==b.issue.wire.conflict);
    assert(a.issue.wire.endpoint_matched==b.issue.wire.endpoint_matched);
    assert(a.reply.observed_ns==b.reply.observed_ns);
    assert(a.reply.wire.observed_ns==b.reply.wire.observed_ns);
    assert(a.reply.wire.serial==b.reply.wire.serial&&a.reply.wire.reply_serial==b.reply.wire.reply_serial);
    assert(a.reply.wire.type==b.reply.wire.type&&a.reply.wire.known==b.reply.wire.known);
    same_text(a.reply.wire.sender,b.reply.wire.sender);
}
void same_owned(const L::Owned& a,const L::Owned& b) {
    assert(a.result==b.result&&a.stage==b.stage);
    assert(a.call_sequence==b.call_sequence&&a.prediction_generation==b.prediction_generation);
    assert(a.view_revision==b.view_revision&&a.layout_version==b.layout_version);
    assert(a.source_instance==b.source_instance&&a.record_sequence==b.record_sequence);
    assert(a.locked_observed_ns==b.locked_observed_ns&&a.map_loss_epoch==b.map_loss_epoch);
    assert(a.request_id==b.request_id&&a.request_epoch==b.request_epoch);
    assert(a.worker_id==b.worker_id&&a.worker_epoch==b.worker_epoch);
    assert(a.cache_lifetime==b.cache_lifetime&&a.write_sequence==b.write_sequence);
    for(unsigned i=0;i<S::FIELD_COUNT;++i) {
        assert(a.fields[i].write_sequence==b.fields[i].write_sequence);
        assert(a.fields[i].observed_ns==b.fields[i].observed_ns);
    }
    assert(a.heading_presence==b.heading_presence);
}
uint64_t clock_fn(void*) {assert(active);errno=EAGAIN;return 1000000+active->id;}
R::Result request(const void* raw,R::Trace* out,void*) {
    assert(active&&raw==active->raw);++active->reads;
    *out=active->trace;errno=E2BIG;return R::OK;
}
bool association(const A::PositionContext& c,L::Owned* out,void*) {
    assert(active&&active->reads==1&&c.lds_association==0);
    ++active->associations;assert(c.position.utc_seconds==active->id);
    assert(c.request_result==R::OK);same_trace(c.request_trace,active->trace);
    L::Owned value=L::Owned();
    value.result=L::MATCHED_LOCKED_FOR_SEND;value.stage=L::LOCKED_FOR_SEND;
    value.call_sequence=c.call_sequence;value.prediction_generation=c.prediction_generation;
    value.view_revision=active->id;value.layout_version=L::protocol::VERSION;
    value.source_instance=100000+active->id;value.record_sequence=200000+active->id;
    value.locked_observed_ns=300000+active->id;value.map_loss_epoch=400000+active->id;
    value.request_id=c.request_trace.request.id;value.request_epoch=c.request_trace.request.epoch;
    value.worker_id=c.request_trace.worker.id;value.worker_epoch=c.request_trace.worker.epoch;
    value.cache_lifetime=500000+active->id;value.write_sequence=600000+active->id;
    for(unsigned i=0;i<S::FIELD_COUNT;++i) {
        value.fields[i].write_sequence=700000+active->id*16+i;
        value.fields[i].observed_ns=800000+active->id*16+i;
    }
    value.heading_presence=static_cast<C::Presence>(active->id%3);
    active->owned=value;*out=value;errno=ENOTTY;return true;
}
bool provenance(void* manager,const A::PositionContext& c,A::Provenance* out,void*) {
    assert(active&&manager==active&&active->reads==1&&active->associations==1);
    ++active->provenances;assert(c.lds_association);
    same_trace(c.request_trace,active->trace);same_owned(*c.lds_association,active->owned);
    *out=A::Provenance();out->source_epoch=active->id+91;out->session_epoch=active->id+92;
    // Epochs are authored preservation sentinels; every physical flag is false.
    errno=EBUSY;return true;
}
void sink(const A::Observation* o,void*) {
    assert(active);
    if(o->kind==A::Observation::POSITION) {++active->position_count;active->position=*o;}
    else {assert(o->kind==A::Observation::SEND);++active->send_count;active->sent=*o;}
    errno=EIO;
}
int32_t next(void* session,A::VehicleData* data) {
    assert(active&&session==active&&errno==EDOM);
    assert(data&&data->type==1&&data->length==48&&data->payload);
    ++active->original_count;
    if(active->send_choice==A::ORIGINAL) {
        assert(data==active->borrowed&&data->payload==active->payload);
        assert(!std::memcmp(data->payload,active->payload,48));
    } else {
        assert(active->send_choice==A::SCRUBBED);
        assert(data!=active->borrowed&&data->payload!=active->payload);
        const uint8_t* bytes=static_cast<const uint8_t*>(data->payload);
        for(unsigned i=0;i<48;++i) {
            const bool cleared=i==32||i==40||(i>=36&&i<40)||i>=44;
            assert(bytes[i]==(cleared?0:active->payload[i]));
        }
    }
    errno=ERANGE;return ORIGINAL_RESULT;
}
void enter(Frame& f) {
    active=&f;errno=EDOM;A::position_enter(&f,f.raw);assert(errno==EDOM);
}
void leave(Frame& f) {
    active=&f;errno=EDOM;A::position_leave();assert(errno==EDOM);active=0;
}
void send(Frame& f,A::Choice choice=A::ORIGINAL) {
    active=&f;f.send_choice=choice;
    A::VehicleData data={1,f.payload,48};f.borrowed=&data;
    const unsigned before=f.original_count;
    errno=EDOM;const int32_t result=A::send_vehicle_data(&f,&data);
    assert(result==ORIGINAL_RESULT&&errno==ERANGE&&f.original_count==before+1);
    assert(data.payload==f.payload&&data.type==1&&data.length==48);
    for(unsigned i=0;i<48;++i)assert(f.payload[i]==uint8_t((i+f.id)%251+1));
    f.borrowed=0;
}
void verify(const Frame& f,bool context,A::Reason reason,A::Choice choice=A::ORIGINAL,
            bool failure_identity=true) {
    assert(f.original_count==1&&f.send_count==1);
    assert(f.sent.result==ORIGINAL_RESULT&&f.sent.has_payload);
    assert(!std::memcmp(f.sent.original,f.payload,48));
    if(f.sent.reason!=reason||f.sent.choice!=choice) {
        std::fprintf(stderr,"context_pool id=%u call=%u request=%llu originals=%u reason=%d expected=%d choice=%d expected_choice=%d\n",
            f.id,f.sent.call_sequence,static_cast<unsigned long long>(f.sent.request_trace.request.id),
            f.original_count,int(f.sent.reason),int(reason),int(f.sent.choice),int(choice));
    }
    assert(f.sent.reason==reason&&f.sent.choice==choice);
    if(!context) {
        if(failure_identity) {
            assert(f.sent.call_sequence&&f.sent.call_sequence==f.position.call_sequence);
            assert(f.sent.prediction_generation==f.position.prediction_generation);
            assert(f.sent.original_mode==f.position.original_mode);
        } else assert(!f.sent.call_sequence&&!f.sent.prediction_generation);
        assert(f.sent.request_result==R::NOT_FOUND&&!f.sent.request_trace.request.id);
        assert(f.sent.lds_association.result==L::UNAVAILABLE);
        assert(!f.sent.provenance.source_epoch&&!f.sent.provenance.session_epoch);
        assert(!std::memcmp(f.sent.outgoing,f.payload,48));
        return;
    }
    assert(f.reads==1&&f.associations==1&&f.provenances==1&&f.position_count==1);
    assert(f.position.position.utc_seconds==f.id&&f.position.position.altitude_m==int32_t(f.id));
    assert(f.position.call_sequence&&f.position.call_sequence==f.sent.call_sequence);
    assert(f.position.prediction_generation==f.sent.prediction_generation);
    assert(f.sent.request_result==R::OK);same_trace(f.position.request_trace,f.trace);
    same_trace(f.sent.request_trace,f.trace);
    same_owned(f.position.lds_association,f.owned);same_owned(f.sent.lds_association,f.owned);
    assert(f.sent.provenance.source_epoch==f.id+91&&f.sent.provenance.session_epoch==f.id+92);
    assert(f.position.provenance.source_epoch==f.sent.provenance.source_epoch);
    assert(f.position.provenance.session_epoch==f.sent.provenance.session_epoch);
    assert(!f.sent.provenance.exact_request&&!f.sent.provenance.verified_lds&&!f.sent.provenance.legacy_receiver);
    if(choice==A::ORIGINAL)assert(!std::memcmp(f.sent.outgoing,f.payload,48));
}
void verify_exhausted_position(const Frame& f) {
    // Capacity loss affects association only. The actual POSITION and its
    // already-available raw request must still reach the ordinary sink.
    assert(f.position_count==1&&f.reads==1);
    assert(f.position.reason==A::CONTEXT_UNAVAILABLE&&f.position.original_mode==0);
    assert(f.position.request_result==R::OK);same_trace(f.position.request_trace,f.trace);
    A::PositionInput decoded=A::PositionInput();assert(A::decode_position(f.raw,&decoded));
    const A::PositionInput& p=f.position.position;
    assert(p.mode==decoded.mode&&p.utc_seconds==decoded.utc_seconds);
    assert(p.latitude_deg==decoded.latitude_deg&&p.longitude_deg==decoded.longitude_deg);
    assert(p.altitude_m==decoded.altitude_m&&p.heading_deg==decoded.heading_deg);
    assert(p.velocity_kmh==decoded.velocity_kmh&&p.horizontal==decoded.horizontal&&p.vertical==decoded.vertical);
    assert(!f.associations&&!f.provenances);
    assert(f.position.lds_association.result==L::UNAVAILABLE);
    assert(!f.position.provenance.source_epoch&&!f.position.provenance.session_epoch);
    assert(!f.position.provenance.exact_request&&!f.position.provenance.verified_lds&&!f.position.provenance.legacy_receiver);
}
__attribute__((constructor(101))) void before_global_constructors() {
#ifdef MX5_CONTEXT_POOL_ASSOCIATION_DSO_TEST
    // The preloaded DSO's constructors already ran. This checks capture before
    // the caller executable's later constructors, not before DSO initialization.
    initialize_context_pool_association_test_dso();
#endif
    initialize(early,1);
    A::Options options=A::Options();options.sink=sink;options.clock=clock_fn;
    options.request_reader=request;options.association_reader=association;options.provenance=provenance;
    assert(!options.allow_assist&&A::configure(next,options));
    enter(early);
}

struct Holder {Frame frame;pthread_t thread;std::atomic<unsigned> release;};
static Holder holders[CAPACITY];
static std::atomic<unsigned> ready(0);
void* hold(void* arg) {
    Holder& h=*static_cast<Holder*>(arg);enter(h.frame);
    ready.fetch_add(1,std::memory_order_release);
    while(!h.release.load(std::memory_order_acquire))usleep(1000);
    send(h.frame);verify(h.frame,true,A::DISABLED);leave(h.frame);return 0;
}
void start_holders(unsigned count) {
    ready.store(0,std::memory_order_relaxed);
    for(unsigned i=0;i<count;++i) {
        initialize(holders[i].frame,100+i);holders[i].release.store(0,std::memory_order_relaxed);
        assert(pthread_create(&holders[i].thread,0,hold,&holders[i])==0);
    }
    while(ready.load(std::memory_order_acquire)!=count)usleep(1000);
}
void release_holder(unsigned i) {
    holders[i].release.store(1,std::memory_order_release);
    assert(pthread_join(holders[i].thread,0)==0);
}
void distinct_holders(unsigned count) {
    for(unsigned i=0;i<count;++i)for(unsigned j=0;j<i;++j)
        assert(holders[i].frame.sent.call_sequence!=holders[j].frame.sent.call_sequence);
}
void round(Frame& f,unsigned id,bool context,A::Reason reason,A::Choice choice=A::ORIGINAL) {
    initialize(f,id);enter(f);send(f,choice);verify(f,context,reason,choice);leave(f);
}
void recovery() {
    // A released slot restores observation; it does not erase the imported
    // observation-fault policy. Failed calculations stay disabled, while OEM
    // forwarding and new request/association capture continue.
    const bool fault=A::faulted();
    assert(A::set_mode(A::SCRUB_STALE));Frame f;
    round(f,999,true,fault?A::DISABLED:A::PASS,fault?A::ORIGINAL:A::SCRUBBED);
    assert(A::faulted()==fault);
}
void capacity(bool reuse,bool raw_first=false) {
    start_holders(CAPACITY);Frame extra;
    initialize(extra,500);enter(extra);
    if(raw_first)verify_exhausted_position(extra);
    send(extra);verify(extra,false,A::CONTEXT_UNAVAILABLE);verify_exhausted_position(extra);leave(extra);
    assert(A::faulted());
    if(reuse) {
        release_holder(0);
        round(extra,501,true,A::DISABLED);
        // A live replacement consumes the single free slot, and its release
        // must not damage any still-owned frame from a different thread.
        initialize(extra,502);enter(extra);
        Frame nested;round(nested,503,false,A::CONTEXT_UNAVAILABLE);
        verify_exhausted_position(nested);
        send(extra);verify(extra,true,A::DISABLED);leave(extra);
    }
    for(unsigned i=reuse?1U:0U;i<CAPACITY;++i)release_holder(i);
    distinct_holders(CAPACITY);recovery();
}
void nested() {
    Frame frame[10];
    for(unsigned round_no=0;round_no<3;++round_no) {
        for(unsigned i=0;i<10;++i) {initialize(frame[i],1000+round_no*10+i);enter(frame[i]);}
        for(unsigned n=10;n>0;--n) {
            Frame& f=frame[n-1];send(f);
            verify(f,n<=8,n>8?A::CONTEXT_UNAVAILABLE:(n==1?A::DISABLED:A::NESTED_CALL),
                   A::ORIGINAL,n<=9);
            if(n>8)verify_exhausted_position(f);
            leave(f);
        }
    }
    recovery();
}
void concurrent() {
    // Fixed full windows prevent scheduling-dependent free-slot availability
    // from becoming an assertion about an unbounded retry policy.
    for(unsigned n=0;n<4;++n) {
        start_holders(CAPACITY);
        for(unsigned i=0;i<CAPACITY;++i)holders[i].release.store(1,std::memory_order_release);
        for(unsigned i=0;i<CAPACITY;++i)assert(pthread_join(holders[i].thread,0)==0);
        distinct_holders(CAPACITY);
    }
    recovery();
}
void await_child(pid_t child) {
    int status=0;assert(waitpid(child,&status,0)==child);
    assert(WIFEXITED(status)&&WEXITSTATUS(status)==0);
}
void child_generation(uint32_t parent_generation) {
    const uint32_t child=A::generation();
    if(!child||child==parent_generation)
        std::fprintf(stderr,"fork generation unchanged: parent=%u child=%u\n",parent_generation,child);
    assert(child&&child!=parent_generation);
    std::printf("fork generation: parent=%u child=%u\n",parent_generation,child);
    std::fflush(stdout);
}
void fork_full() {
    start_holders(CAPACITY);const uint32_t before=A::generation();
    const pid_t child=fork();assert(child>=0);
    if(!child) {
        alarm(10);child_generation(before);Frame f;
        // No manual fork-reset API: the actual libc fork path must reclaim
        // the 64 vanished parent threads before a new callback can observe.
        round(f,700,true,A::DISABLED);_exit(0);
    }
    assert(A::generation()==before);await_child(child);assert(A::generation()==before);
    Frame extra;round(extra,701,false,A::CONTEXT_UNAVAILABLE);
    verify_exhausted_position(extra);
    for(unsigned i=0;i<CAPACITY;++i)release_holder(i);
    distinct_holders(CAPACITY);recovery();
}
void fork_live() {
    // The product claims first-fit: entering the caller before the holders
    // makes a child that wrongly frees every slot overwrite this exact frame.
    Frame caller;initialize(caller,800);enter(caller);start_holders(CAPACITY-1);
    const uint32_t before=A::generation();assert(caller.position.prediction_generation==before);
    const pid_t child=fork();assert(child>=0);
    if(!child) {
        alarm(10);child_generation(before);
        // Reclaim orphan slots without erasing the caller's captured frame.
        Frame inner;round(inner,801,true,A::NESTED_CALL);
        send(caller);
        // Check the owned values before generic reason assertions so an
        // over-reclaim mutation is rejected for the actual aliasing evidence.
        same_trace(caller.sent.request_trace,caller.trace);same_owned(caller.sent.lds_association,caller.owned);
        verify(caller,true,A::DISABLED);leave(caller);
        Frame fresh;round(fresh,802,true,A::DISABLED);_exit(0);
    }
    assert(A::generation()==before);await_child(child);assert(A::generation()==before);
    send(caller);verify(caller,true,A::DISABLED);leave(caller);
    for(unsigned i=0;i<CAPACITY-1;++i)release_holder(i);
    distinct_holders(CAPACITY-1);recovery();
}
void fork_nested() {
    Frame live[3];
    for(unsigned i=0;i<3;++i) {initialize(live[i],900+i);enter(live[i]);}
    start_holders(CAPACITY-3);const uint32_t before=A::generation();
    const pid_t child=fork();assert(child>=0);
    if(!child) {
        alarm(10);child_generation(before);Frame inner;round(inner,903,true,A::NESTED_CALL);
        for(unsigned n=3;n>0;--n) {
            Frame& f=live[n-1];send(f);
            same_trace(f.sent.request_trace,f.trace);same_owned(f.sent.lds_association,f.owned);
            verify(f,true,n==1?A::DISABLED:A::NESTED_CALL);leave(f);
        }
        Frame fresh;round(fresh,904,true,A::DISABLED);_exit(0);
    }
    assert(A::generation()==before);await_child(child);assert(A::generation()==before);
    for(unsigned n=3;n>0;--n) {
        Frame& f=live[n-1];send(f);verify(f,true,n==1?A::DISABLED:A::NESTED_CALL);leave(f);
    }
    for(unsigned i=0;i<CAPACITY-3;++i)release_holder(i);
    distinct_holders(CAPACITY-3);recovery();
}
void fork_unavailable() {
    start_holders(CAPACITY);Frame failed;initialize(failed,910);enter(failed);
    verify_exhausted_position(failed);assert(A::faulted());
    const uint32_t before=A::generation();const pid_t child=fork();assert(child>=0);
    if(!child) {
        alarm(10);child_generation(before);
        Frame inner;round(inner,911,true,A::NESTED_CALL);
        send(failed);verify(failed,false,A::CONTEXT_UNAVAILABLE);leave(failed);
        Frame fresh;round(fresh,912,true,A::DISABLED);_exit(0);
    }
    assert(A::generation()==before);await_child(child);assert(A::generation()==before);
    send(failed);verify(failed,false,A::CONTEXT_UNAVAILABLE);leave(failed);
    for(unsigned i=0;i<CAPACITY;++i)release_holder(i);
    distinct_holders(CAPACITY);recovery();
}
void fork_depth9() {
    Frame live[8];
    for(unsigned i=0;i<8;++i) {initialize(live[i],920+i);enter(live[i]);}
    start_holders(CAPACITY-8);Frame failed;initialize(failed,928);enter(failed);
    verify_exhausted_position(failed);assert(A::faulted());
    const uint32_t before=A::generation();const pid_t child=fork();assert(child>=0);
    if(!child) {
        alarm(10);child_generation(before);
        send(failed);verify(failed,false,A::CONTEXT_UNAVAILABLE);leave(failed);
        for(unsigned n=8;n>0;--n) {
            Frame& f=live[n-1];send(f);
            same_trace(f.sent.request_trace,f.trace);same_owned(f.sent.lds_association,f.owned);
            verify(f,true,n==1?A::DISABLED:A::NESTED_CALL);leave(f);
        }
        Frame fresh;round(fresh,929,true,A::DISABLED);_exit(0);
    }
    assert(A::generation()==before);await_child(child);assert(A::generation()==before);
    send(failed);verify(failed,false,A::CONTEXT_UNAVAILABLE);leave(failed);
    for(unsigned n=8;n>0;--n) {
        Frame& f=live[n-1];send(f);verify(f,true,n==1?A::DISABLED:A::NESTED_CALL);leave(f);
    }
    for(unsigned i=0;i<CAPACITY-8;++i)release_holder(i);
    distinct_holders(CAPACITY-8);recovery();
}
void fork_generation() {
    // Independent of pool pressure: an inherited generation must be revoked in
    // the child even when all slots are free. This grants no physical readiness.
    const uint32_t before=A::generation();const pid_t child=fork();assert(child>=0);
    if(!child) {
        alarm(10);child_generation(before);Frame child_frame;
        round(child_frame,850,true,A::DISABLED);
        assert(child_frame.sent.prediction_generation!=before);_exit(0);
    }
    assert(A::generation()==before);await_child(child);assert(A::generation()==before);
    Frame parent_frame;round(parent_frame,851,true,A::DISABLED);
    assert(parent_frame.sent.prediction_generation==before);recovery();
}
}
int main(int argc,char** argv) {
    alarm(30);assert(argc==2);
    // This frame was entered before normal C++ dynamic initializers. Do not
    // reconfigure or re-enter it: that would hide a late pool-constructor reset.
    send(early);verify(early,true,A::DISABLED);leave(early);
    if(!std::strcmp(argv[1],"early"))recovery();
    else if(!std::strcmp(argv[1],"capacity"))capacity(false);
    else if(!std::strcmp(argv[1],"capacity_raw"))capacity(false,true);
    else if(!std::strcmp(argv[1],"reuse"))capacity(true);
    else if(!std::strcmp(argv[1],"nested"))nested();
    else if(!std::strcmp(argv[1],"concurrent"))concurrent();
    else if(!std::strcmp(argv[1],"fork_full"))fork_full();
    else if(!std::strcmp(argv[1],"fork_live"))fork_live();
    else if(!std::strcmp(argv[1],"fork_nested"))fork_nested();
    else if(!std::strcmp(argv[1],"fork_unavailable"))fork_unavailable();
    else if(!std::strcmp(argv[1],"fork_depth9"))fork_depth9();
    else if(!std::strcmp(argv[1],"fork_generation"))fork_generation();
    else assert(!"unknown context pool scenario");
    alarm(0);std::printf("PASS context pool association %s: original forwarding and owned frame lifetime\n",argv[1]);
}
