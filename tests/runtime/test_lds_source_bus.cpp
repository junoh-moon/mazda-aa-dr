#ifdef MX5_LDS_SOURCE_BUS_LEGACY
#include "runtime/model_bus.h"
#else
#include "runtime/lds_source_bus.h"
#endif
#include "runtime/lds_request_source.h"
#include <cassert>
#include <cstdio>
#include <cstring>

namespace R=mx5::runtime;
namespace B=R::bus_trace;
namespace Q=R::request_trace;
namespace L=R::lds_sideband;
namespace A=mx5::adapter;
#ifdef MX5_LDS_SOURCE_BUS_LEGACY
typedef R::ModelBus Policy;
#else
typedef R::LdsSourceBus Policy;
#endif
static unsigned checks;
#define CHECK(x) do { ++checks; const bool passed=!!(x);if(!passed) { \
    std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); \
    std::fflush(stderr);assert(passed); } } while(0)

// The RED build uses the existing worker's ModelBus::CHANGED decision. The
// new policy has no clock: only the worker decides when to reset the source.
static bool update(Policy& p,const B::Boundary& b,uint64_t now) {
#ifdef MX5_LDS_SOURCE_BUS_LEGACY
    return p.update(b,now)==R::ModelBus::CHANGED;
#else
    (void)now;return p.update(b);
#endif
}
static B::Boundary boundary(B::Result result,uint64_t revision=0,
                            uint32_t object=0,uint64_t lifetime=0) {
    const B::Boundary b={{result,object,lifetime},revision};return b;
}
static B::Boundary connected(uint64_t revision=10,uint32_t object=1,uint64_t life=1) {
    return boundary(B::CONNECTED,revision,object,life);
}
static void initial_discovery() {
    Policy p;
    CHECK(!update(p,boundary(B::UNOBSERVED),100));
    // create/connect raises global revision before a POSITION marks its bus.
    CHECK(!update(p,boundary(B::UNOBSERVED,2),110));
    CHECK(!update(p,boundary(B::UNOBSERVED,2),120));
    CHECK(!update(p,boundary(B::TRANSITION),130));
    CHECK(!update(p,boundary(B::UNOBSERVED,4),140));
    CHECK(!update(p,boundary(B::TRANSITION),150));
    // The first source marker also raises revision, without ending a lifetime.
    CHECK(!update(p,connected(),160));
    CHECK(!update(p,connected(),170));
    CHECK(update(p,connected(11),180));
    CHECK(!update(p,connected(11),190));

    Policy during_connect;
    CHECK(!update(during_connect,boundary(B::TRANSITION),100));
    CHECK(!update(during_connect,boundary(B::TRANSITION),110));
    CHECK(!update(during_connect,boundary(B::UNOBSERVED,8),120));
    CHECK(!update(during_connect,connected(),130));
}
static void known_at_start() {
    Policy p;
    CHECK(!update(p,connected(),100));
    CHECK(!update(p,connected(),200));
    CHECK(update(p,connected(11),300));
    CHECK(!update(p,connected(11),400));
}
static void negative_at_start() {
    const B::Result negative[]={B::NONE,B::AMBIGUOUS,B::FAULT,B::DISCONNECTED};
    for(unsigned i=0;i<sizeof negative/sizeof negative[0];++i) {
        Policy p;const B::Boundary lost=boundary(negative[i],20);
        CHECK(update(p,lost,100));
        CHECK(!update(p,lost,110));
        // A later unknown snapshot must not erase an already observed loss.
        CHECK(update(p,boundary(B::UNOBSERVED),120));
        CHECK(!update(p,boundary(B::UNOBSERVED),130));
        CHECK(update(p,boundary(B::TRANSITION),140));
        CHECK(update(p,connected(21),150));
        CHECK(!update(p,connected(21),160));
    }
}
static void invalid_connected_at_start() {
    const B::Boundary bad[]={connected(0),connected(10,0),connected(10,1,0),
                            boundary(static_cast<B::Result>(99),10)};
    for(unsigned i=0;i<sizeof bad/sizeof bad[0];++i) {
        Policy p;
        CHECK(update(p,bad[i],100));
        CHECK(!update(p,bad[i],110));
        CHECK(update(p,connected(11),120));
        CHECK(!update(p,connected(11),130));
    }
}
static void changes_after_baseline() {
    const B::Boundary changed[]={connected(11),connected(10,2),connected(10,1,2),
        boundary(B::UNOBSERVED),boundary(B::TRANSITION),boundary(B::NONE,11),
        boundary(B::AMBIGUOUS,11),boundary(B::FAULT),boundary(B::DISCONNECTED,11),
        connected(0),connected(10,0),connected(10,1,0)};
    for(unsigned i=0;i<sizeof changed/sizeof changed[0];++i) {
        Policy p;CHECK(!update(p,connected(),100));
        CHECK(update(p,changed[i],200));
        CHECK(!update(p,changed[i],300));
        CHECK(update(p,connected(12,1,2),400));
        CHECK(!update(p,connected(12,1,2),500));
    }
    Policy reconnect;CHECK(!update(reconnect,connected(),100));
    CHECK(update(reconnect,boundary(B::NONE,11),200));
    CHECK(update(reconnect,boundary(B::UNOBSERVED,12),300));
    CHECK(update(reconnect,boundary(B::TRANSITION),400));
    CHECK(update(reconnect,connected(13,1,2),500));
    CHECK(!update(reconnect,connected(13,1,2),600));
}
static Q::Text text(const char* value) { Q::Text out;L::copy_text(&out,value);return out; }
static A::Observation position(uint64_t id,uint64_t when) {
    A::Observation o=A::Observation();o.kind=A::Observation::POSITION;
    o.call_sequence=uint32_t(id);o.prediction_generation=3;o.mono_ns=when+2;
    o.position.mode=o.original_mode=1;o.position.latitude_deg=35;o.position.longitude_deg=129;
    Q::Trace& t=o.request_trace;o.request_result=Q::OK;
    t.request=Q::Token{id,1};t.worker=Q::Token{id+100,1};t.issue.observed_ns=when;
    t.issue.endpoint.server_guid=text("authored-guid");t.issue.endpoint.unique_name=text(":1.2");
    t.issue.wire.known=t.issue.wire.endpoint_matched=true;t.issue.wire.serial=uint32_t(id+10);
    t.reply.wire.known=true;t.reply.wire.type=2;t.reply.wire.serial=uint32_t(id+20);
    t.reply.wire.reply_serial=t.issue.wire.serial;t.reply.wire.sender=text(":1.1");return o;
}
static L::Record record(const A::Observation& o) {
    L::Record r=L::Record();r.source_instance=1;r.sequence=o.request_trace.request.id;
    r.observed_ns=o.mono_ns+1;r.flags=111;r.send_result=1;r.reply_type=2;r.position=o.position;
    r.wire.server_guid=o.request_trace.issue.endpoint.server_guid;
    r.wire.client_unique=r.wire.destination=o.request_trace.issue.endpoint.unique_name;
    r.wire.server_unique=o.request_trace.reply.wire.sender;
    r.wire.request_serial=r.wire.reply_serial=o.request_trace.issue.wire.serial;
    r.wire.response_serial=o.request_trace.reply.wire.serial;
    return r;
}
static void feed(R::LdsRequestSource& source,const A::Observation& o,bool reverse,uint64_t now) {
    L::Diagnostic d=L::Diagnostic();d.fault=L::RECEIVE_OK;d.sender_pid=42;d.received_ns=now;
    if(reverse) { source.sideband(record(o),d,now);source.position(o,now); }
    else { source.position(o,now);source.sideband(record(o),d,now); }
}
static void apply(R::LdsRequestSource& source,Policy& policy,const B::Boundary& b,uint64_t now) {
    source.advance(now);if(update(policy,b,now))source.reset(now);
}
static void queued_pair_and_retirement() {
    for(unsigned reverse=0;reverse<2;++reverse) {
        Policy p;R::LdsRequestSource source;
        apply(source,p,boundary(B::UNOBSERVED),100);
        apply(source,p,boundary(B::UNOBSERVED,2),200);
        apply(source,p,boundary(B::TRANSITION),250);
        const A::Observation old=position(1,300);
        apply(source,p,connected(),400); // Original request completed before this worker inspection.
        feed(source,old,reverse,400);
        R::LdsRequestSource::JoinedReply joined;
        CHECK(source.lookup(old,&joined)==R::LdsRequestSource::MATCHED);
        CHECK(source.current(joined));CHECK(source.status().floor_ns==0);
        CHECK(!joined.observation.provenance.exact_request&&!joined.observation.provenance.verified_lds);
        apply(source,p,boundary(B::NONE,11),500);
        CHECK(!source.current(joined));CHECK(source.status().floor_ns==500);
        apply(source,p,boundary(B::TRANSITION),550);
        apply(source,p,connected(12,1,2),600);
        feed(source,old,reverse,610);
        CHECK(source.lookup(old,&joined)==R::LdsRequestSource::UNAVAILABLE);
        CHECK(source.status().matches==1&&source.status().rejected==2);
        const A::Observation fresh=position(2,650);feed(source,fresh,reverse,700);
        CHECK(source.lookup(fresh,&joined)==R::LdsRequestSource::MATCHED);
        CHECK(source.current(joined));CHECK(source.status().matches==2);
    }
}
struct Case { const char* name;void (*run)(); };
int main(int argc,char** argv) {
    const Case cases[]={{"initial_discovery",initial_discovery},{"known_at_start",known_at_start},
        {"negative_at_start",negative_at_start},{"invalid_connected_at_start",invalid_connected_at_start},
        {"changes_after_baseline",changes_after_baseline},{"queued_pair_and_retirement",queued_pair_and_retirement}};
    unsigned ran=0;
    for(unsigned i=0;i<sizeof cases/sizeof cases[0];++i) {
        if(argc>1&&std::strcmp(argv[1],cases[i].name))continue;
        cases[i].run();++ran;
    }
    CHECK(ran!=0);std::printf("LDS source bus: %u cases, %u checks passed\n",ran,checks);return 0;
}
