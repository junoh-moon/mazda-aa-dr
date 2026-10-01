// Authored requests and qualification only. Exercise the real one-shot Ledger
// and adapter; no physical provider, sensor, OEM runtime or phone is qualified.
#include "adapter/adapter.h"
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>

#ifdef NDEBUG
#error Provenance context regressions require assertions
#endif
namespace A=mx5::adapter;
namespace R=mx5::runtime::request_trace;
#ifdef MX5_PROVENANCE_CONTEXT_DSO_TEST
#include "provenance_context_dso_access.h"
#endif
namespace {
struct Slot {
    unsigned char raw[72];
    int method,worker_object;
    R::WorkerContext scope;
    R::Token request,worker;
    R::Issue issue;
    R::Reply reply;
    R::Result expected_result;
    unsigned expected_sequence,reads,checks;
    uint32_t captured_generation;
    bool qualify;
    Slot():raw(),method(0),worker_object(0),scope(),request(),worker(),issue(),reply(),
        expected_result(R::OK),expected_sequence(0),reads(0),checks(0),
        captured_generation(0),qualify(true) {}
};
R::Ledger ledger;
Slot slots[2];
const char* scenario;
unsigned sends,positions;
A::Observation last_position,last_send;
uint8_t payload[48];
A::VehicleData* borrowed;
bool expect_original;
const uint64_t NOW=1000000000ULL;

bool is(const char* value) { return !std::strcmp(scenario,value); }
void put32(unsigned char* p,uint32_t value) {
    for(unsigned i=0;i<4;++i)p[i]=static_cast<unsigned char>(value>>(8*i));
}
void put64(unsigned char* p,uint64_t value) {
    for(unsigned i=0;i<8;++i)p[i]=static_cast<unsigned char>(value>>(8*i));
}
void put_double(unsigned char* p,double value) {
    uint64_t bits;std::memcpy(&bits,&value,sizeof bits);put64(p,bits);
}
void prepare(Slot& slot,unsigned index) {
    // Identical position numbers deliberately cannot identify the request.
    put64(slot.raw+8,1234567890ULL);
    put_double(slot.raw+16,35.5);put_double(slot.raw+24,135.5);
    put32(slot.raw+32,12);put_double(slot.raw+40,20);
    put_double(slot.raw+48,18);put_double(slot.raw+56,1.0);
    put_double(slot.raw+64,1.5);
    slot.expected_sequence=index+1;
    slot.issue.observed_ns=100+index;
    slot.issue.bus_lifetime=10+index;
    slot.issue.known=R::ISSUE_BUS_LIFETIME;
    slot.issue.connection=mx5::runtime::bus_trace::Snapshot{
        mx5::runtime::bus_trace::CONNECTED,index+1,10+index};
    slot.issue.endpoint.server_guid=R::copy_text(index?"bus-b":"bus-a");
    slot.issue.endpoint.unique_name=R::copy_text(index?":1.21":":1.20");
    slot.issue.route.destination=R::copy_text("com.jci.lds");
    slot.issue.route.path=R::copy_text("/com/jci/lds/data");
    slot.issue.route.interface_name=R::copy_text("com.jci.lds.data");
    slot.issue.route.member=R::copy_text("GetPosition");
    slot.issue.wire.known=true;slot.issue.wire.serial=40+index;
    slot.reply.observed_ns=200+index;
    slot.reply.type=2;slot.reply.type_known=true;
    slot.reply.sender=R::copy_text(index?":1.31":":1.30");
    slot.reply.wire.known=true;slot.reply.wire.serial=50+index;
    slot.reply.wire.reply_serial=40+index;slot.reply.wire.type=2;
    slot.reply.wire.sender=slot.reply.sender;
    assert(ledger.request_begin(&slot.method,slot.issue,&slot.request)==R::OK);
    R::Token delivered;
    assert(ledger.reply_enter(&slot.method,slot.reply,&delivered)==R::OK);
    assert(ledger.worker_post(&slot.worker_object,slot.raw,delivered,&slot.worker)==R::OK);
    assert(ledger.worker_enter(&slot.worker_object,&slot.scope)==R::OK);
    assert(ledger.request_end(&slot.method)==R::OK);
}
void empty(const R::Trace& trace) {
    assert(!trace.request.id&&!trace.request.epoch&&!trace.worker.id&&!trace.worker.epoch);
    assert(!trace.issue.observed_ns&&!trace.issue.known&&!trace.issue.bus_lifetime);
    assert(!trace.issue.endpoint.server_guid.known&&!trace.issue.endpoint.unique_name.known);
    assert(!trace.issue.route.destination.known&&!trace.issue.route.member.known);
    assert(!trace.issue.wire.known&&!trace.issue.wire.serial);
    assert(!trace.reply.type_known&&!trace.reply.observed_ns&&!trace.reply.sender.known);
    assert(!trace.reply.wire.known&&!trace.reply.wire.serial&&!trace.reply.wire.sender.known);
}
void same_trace(const Slot& slot,const R::Trace& trace) {
    assert(trace.request.id==slot.request.id&&trace.request.epoch==slot.request.epoch);
    assert(trace.worker.id==slot.worker.id&&trace.worker.epoch==slot.worker.epoch);
    assert(trace.issue.observed_ns==slot.issue.observed_ns);
    assert(trace.issue.bus_lifetime==slot.issue.bus_lifetime&&trace.issue.known==slot.issue.known);
    assert(!std::strcmp(trace.issue.endpoint.server_guid.bytes,slot.issue.endpoint.server_guid.bytes));
    assert(!std::strcmp(trace.issue.endpoint.unique_name.bytes,slot.issue.endpoint.unique_name.bytes));
    assert(!std::strcmp(trace.issue.route.member.bytes,"GetPosition"));
    assert(trace.issue.wire.serial==slot.issue.wire.serial);
    assert(trace.reply.observed_ns==slot.reply.observed_ns&&trace.reply.type==2);
    assert(!std::strcmp(trace.reply.sender.bytes,slot.reply.sender.bytes));
    assert(trace.reply.wire.serial==slot.reply.wire.serial);
    assert(trace.reply.wire.reply_serial==slot.issue.wire.serial);
}
R::Result read_request(const void* input,R::Trace* out,void*) {
    Slot& slot=input==slots[1].raw?slots[1]:slots[0];
    ++slot.reads;
    const R::Result actual=ledger.position_take(&slot.scope,input,out);
    if(is("failure")) {
        // A misbehaving reader leaves real nonzero metadata on failure. The
        // adapter must pass the failure and a cleared trace to its consumer.
        assert(actual==R::OK);
        errno=EBUSY;return R::STALE;
    }
    errno=EBUSY;return actual;
}
uint64_t clock_fn(void*) { errno=EAGAIN;return NOW; }
A::DrSnapshot snapshot() {
    A::DrSnapshot value=A::DrSnapshot();
    value.source_epoch=11;value.session_epoch=12;
    value.prediction_generation=A::generation();
    value.frontier_mono_ns=NOW-1000;value.valid_until_mono_ns=NOW+1000000;
    value.derived_utc_ns=1234567890000000000ULL;
    value.latitude_deg=35.6;value.longitude_deg=135.6;
    value.speed_mps=5;value.travel_bearing_deg=20;
    value.ready=value.profile_verified=value.input_quality_verified=value.limits_ok=true;
    return value;
}
int32_t next_send(void* session,A::VehicleData* data) {
    assert(session==&sends&&errno==EDOM);
    assert((data==borrowed)==expect_original);
    if(expect_original)assert(data->payload==payload);
    else assert(data->payload!=payload);
    ++sends;errno=ERANGE;return -707;
}
void sink(const A::Observation* event,void*) {
    if(event->kind==A::Observation::POSITION) {
        ++positions;last_position=*event;
        // Even a fresh candidate cannot make an older callback generation
        // valid again. Qualification in this fixture is explicitly authored.
        assert(A::publish_snapshot(snapshot()));
    } else last_send=*event;
    errno=E2BIG;
}
void send(bool original,A::Reason reason) {
    A::VehicleData data={1,payload,48};borrowed=&data;expect_original=original;
    const unsigned before=sends;errno=EDOM;
    assert(A::send_vehicle_data(&sends,&data)==-707&&errno==ERANGE);
    assert(sends==before+1&&last_send.reason==reason);
    assert(last_send.choice==(original?A::ORIGINAL:A::DR_REPLACEMENT));
    for(unsigned i=0;i<48;++i)assert(payload[i]==i+1);
}
bool inspect(void* manager,const A::PositionInput& position,R::Result result,
             const R::Trace& trace,uint32_t call,uint32_t generation,A::Provenance* out) {
    Slot& slot=*static_cast<Slot*>(manager);
    ++slot.checks;
    assert(call==slot.expected_sequence&&generation!=0);
    assert(result==slot.expected_result);
    assert(position.mode==0&&position.utc_seconds==1234567890ULL);
    assert(position.latitude_deg==35.5&&position.longitude_deg==135.5&&position.altitude_m==12);
    assert(position.heading_deg==20&&position.velocity_kmh==18);
    assert(position.horizontal==1&&position.vertical==1.5);
    if(result==R::OK)same_trace(slot,trace);else empty(trace);
    assert(slot.reads==(is("missing")?0U:1U));
    slot.captured_generation=generation;
    if(is("nested") && &slot==&slots[0]) {
        errno=EDOM;A::position_enter(&slots[1],slots[1].raw);assert(errno==EDOM);
        send(true,A::NESTED_CALL);A::position_leave();
        // Inner reads, callbacks and SEND must not overwrite this borrowed view.
        same_trace(slot,trace);
        assert(position.latitude_deg==35.5&&call==1&&generation==slot.captured_generation);
    }
    if(is("invalidate")) {
        assert(generation==A::generation());A::invalidate();
        assert(generation<A::generation());
    }
    out->source_epoch=11;out->session_epoch=12;
    out->exact_request=out->verified_lds=out->legacy_receiver=true;
    errno=ENOTTY;
    return slot.qualify;
}

// Keep this regression executable against the pre-context API: that callback
// receives the numbers only, so the same identity assertion above fails. This
// overload is never selected by the new product callback type.
bool __attribute__((unused)) provenance(void* manager,const A::PositionInput* p,
                                       A::Provenance* out,void*) {
    return inspect(manager,*p,R::NOT_FOUND,R::Trace(),0,0,out);
}
template<class Context>
bool provenance(void* manager,const Context& input,A::Provenance* out,void*) {
    return inspect(manager,input.position,input.request_result,input.request_trace,
                   input.call_sequence,input.prediction_generation,out);
}
}

int main(int argc,char** argv) {
    assert(argc==2);scenario=argv[1];
#ifdef MX5_PROVENANCE_CONTEXT_DSO_TEST
    initialize_provenance_context_test_dso();
#endif
    assert(is("captured")||is("nested")||is("failure")||is("missing")||
           is("invalidate")||is("unqualified")||is("malformed"));
    prepare(slots[0],0);prepare(slots[1],1);
    for(unsigned i=0;i<48;++i)payload[i]=static_cast<uint8_t>(i+1);
    if(is("failure"))slots[0].expected_result=R::STALE;
    if(is("missing"))slots[0].expected_result=R::NOT_FOUND;
    if(is("failure")||is("missing")||is("unqualified"))slots[0].qualify=false;
    A::Options options=A::Options();
    options.clock=clock_fn;options.sink=sink;options.provenance=provenance;
    options.request_reader=is("missing")?0:read_request;
    options.allow_assist=true;options.max_snapshot_age_ns=150000000;
    assert(A::configure(next_send,options)&&A::set_mode(A::ASSIST));
    errno=EDOM;A::position_enter(&slots[0],is("malformed")?0:slots[0].raw);
    assert(errno==EDOM);
    if(is("malformed")) {
        assert(slots[0].checks==0&&slots[0].reads==1);
        assert(last_position.original_mode==-1&&!last_position.provenance.exact_request);
        // A null input is forwarded under the existing adapter failure policy.
        send(true,A::NO_CONTEXT);
    } else {
        assert(slots[0].checks==1);
        assert(last_position.call_sequence==1);
        assert(last_position.prediction_generation==slots[0].captured_generation);
        if(slots[0].expected_result==R::OK)same_trace(slots[0],last_position.request_trace);
        else empty(last_position.request_trace);
        if(is("invalidate"))send(true,A::EPOCH_MISMATCH);
        else if(!slots[0].qualify) {
            assert(!last_position.provenance.exact_request);
            send(true,A::BAD_PROVENANCE);
        } else send(false,A::PASS);
        assert(last_send.call_sequence==1&&last_send.prediction_generation==slots[0].captured_generation);
        if(slots[0].expected_result==R::OK)same_trace(slots[0],last_send.request_trace);
        else empty(last_send.request_trace);
        if(!is("missing")) {
            R::Trace duplicate;
            assert(ledger.position_take(&slots[0].scope,slots[0].raw,&duplicate)==R::USED);
            empty(duplicate);
        }
    }
    A::position_leave();
    assert(positions==(is("nested")?2U:1U));
    if(is("nested"))assert(slots[1].checks==1&&slots[1].reads==1);
    for(unsigned i=0;i<2;++i)ledger.worker_leave(&slots[i].scope);
    std::printf("PASS provenance context %s: verified\n",scenario);
}
