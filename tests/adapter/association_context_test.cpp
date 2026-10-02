// The real adapter owns the same already-captured request/association throughout
// POSITION, provenance and SEND. Inputs here are authored observation identities.
#include "adapter/adapter.h"
#include "runtime/lds_association_protocol.h"
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>

#ifdef NDEBUG
#error Association context regressions require assertions
#endif
#ifdef MX5_ASSOCIATION_CONTEXT_DSO_TEST
#include "association_context_dso_access.h"
#endif
namespace A=mx5::adapter;
namespace L=mx5::runtime::lds_association;
namespace R=mx5::runtime::request_trace;
namespace S=mx5::sensors::lds_lineage;
namespace C=mx5::sensors::nmea_course_token;
static_assert(S::FIELD_COUNT==9,"Owned comparison covers every field origin");
namespace {
const char* const scenarios[]={
    "captured","nested","mutate_after","nested_missing","wrong_call","wrong_generation",
    "wrong_request","wrong_worker","wrong_stage","unavailable","missing","malformed",
    "request_failed","reader_conflict","reader_mismatch","frame_reuse","provenance_failed",
    "presence_empty","presence_present","legacy_layout","invalid_presence","presence_without_origin",
    "status_empty","status_a","status_v","status_other","invalid_status","status_without_origin","legacy_layout_v2"};
const char* scenario;
unsigned reads[2],lookups[2],checks[2],sends,position_count[2],send_count[2];
bool seen[2];
uint32_t generations[2];
unsigned char raw[2][72];
uint8_t payload[48];
A::Observation positions[2],sent[2];
A::VehicleData* borrowed;
bool is(const char* s) { return !std::strcmp(scenario,s); }
bool known() {
    for(unsigned i=0;i<sizeof scenarios/sizeof scenarios[0];++i)if(is(scenarios[i]))return true;
    return false;
}
unsigned frames() { return is("nested")||is("nested_missing")||is("frame_reuse")?2U:1U; }
unsigned index(uint32_t call) { assert(call==1||call==2);return call-1; }
void capture(unsigned n,uint32_t generation) {
    if(seen[n])assert(generations[n]==generation);
    else {seen[n]=true;generations[n]=generation;}
}
uint64_t clock_fn(void*) { errno=EAGAIN;return 1000000000; }
R::Result request(const void* p,R::Trace* out,void*) {
    const unsigned n=p==raw[1]?1:0;++reads[n];
    *out=R::Trace();errno=E2BIG;
    if(is("request_failed"))return R::NOT_FOUND;
    out->request=R::Token{10+n,1};out->worker=R::Token{20+n,1};
    return R::OK;
}
L::Owned identity(unsigned n,uint32_t generation) {
    L::Owned out=L::Owned();out.result=L::MATCHED_LOCKED_FOR_SEND;out.stage=L::LOCKED_FOR_SEND;
    out.call_sequence=n+1;out.prediction_generation=generation;
    out.view_revision=3;out.layout_version=L::protocol::VERSION;out.source_instance=41;out.record_sequence=42;
    out.locked_observed_ns=99;out.map_loss_epoch=1;
    out.request_id=10+n;out.request_epoch=1;out.worker_id=20+n;out.worker_epoch=1;
    out.cache_lifetime=51;out.write_sequence=52;
    for(unsigned i=0;i<S::FIELD_COUNT;++i) {
        out.fields[i].write_sequence=52-i;out.fields[i].observed_ns=98-i;
    }
    out.heading_presence=is("presence_empty")?C::EMPTY:is("presence_present")?C::PRESENT:C::UNKNOWN;
    out.heading_rmc_status=is("status_empty")?C::RMC_EMPTY:is("status_a")?C::RMC_A:
        is("status_v")?C::RMC_V:is("status_other")?C::RMC_OTHER:C::RMC_UNKNOWN;
    return out;
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
    assert(a.heading_rmc_status==b.heading_rmc_status);
}
bool expected_match(unsigned n) {
    return is("captured")||is("nested")||is("mutate_after")||is("provenance_failed")||
        is("presence_empty")||is("presence_present")||is("status_empty")||is("status_a")||
        is("status_v")||is("status_other")||
        ((is("nested_missing")||is("frame_reuse"))&&n==0);
}
// A rejected association keeps only the conflict/mismatch classification.
L::Owned expected(unsigned n) {
    if(expected_match(n)) {assert(seen[n]);return identity(n,generations[n]);}
    L::Owned out=L::Owned();
    if(is("wrong_call")||is("wrong_generation")||is("wrong_request")||is("wrong_worker")||
       is("wrong_stage")||is("reader_conflict")||is("legacy_layout")||
       is("invalid_presence")||is("presence_without_origin")||is("invalid_status")||
       is("status_without_origin")||is("legacy_layout_v2"))out.result=L::CONFLICT;
    else if(is("reader_mismatch"))out.result=L::PAYLOAD_MISMATCH;
    return out;
}
void check_request(const A::PositionContext& c,unsigned n) {
    if(is("request_failed")) {
        assert(c.request_result==R::NOT_FOUND);
        assert(!c.request_trace.request.id&&!c.request_trace.worker.id);
    } else {
        assert(c.request_result==R::OK);
        assert(c.request_trace.request.id==10+n&&c.request_trace.worker.id==20+n);
    }
}
bool association(const A::PositionContext& c,L::Owned* out,void*) {
    const unsigned n=index(c.call_sequence);++lookups[n];
    assert(c.lds_association==0&&reads[n]==1);
    check_request(c,n);
    assert(c.prediction_generation==A::generation());
    capture(n,c.prediction_generation);
    *out=identity(n,c.prediction_generation);errno=ENOTTY;
    if(is("wrong_call"))++out->call_sequence;
    if(is("wrong_generation"))++out->prediction_generation;
    if(is("wrong_request"))++out->request_id;
    if(is("wrong_worker"))++out->worker_epoch;
    if(is("wrong_stage"))out->stage=L::NO_STAGE;
    if(is("legacy_layout"))out->layout_version=1; // Deliberately old, not the current protocol constant.
    if(is("legacy_layout_v2"))out->layout_version=2;
    if(is("invalid_status"))out->heading_rmc_status=static_cast<C::RmcStatus>(5);
    if(is("status_without_origin")) {
        out->heading_rmc_status=C::RMC_A;out->fields[S::HEADING]=S::FieldOrigin();
    }
    if(is("invalid_presence"))out->heading_presence=static_cast<C::Presence>(3);
    if(is("presence_without_origin")) {
        out->heading_presence=C::PRESENT;out->fields[S::HEADING]=S::FieldOrigin();
    }
    if(is("reader_conflict")) {out->result=L::CONFLICT;return false;}
    if(is("reader_mismatch")) {out->result=L::PAYLOAD_MISMATCH;return false;}
    // A failed request may still reach the reader; it must never become MATCHED.
    if(is("request_failed")||is("unavailable")||
       ((is("nested_missing")||is("frame_reuse"))&&n==1))return false;
    return true;
}
int32_t next(void* session,A::VehicleData* data) {
    assert(session==&sends&&data==borrowed&&data->payload==payload&&errno==EDOM);
    assert(data->type==1&&data->length==48);
    for(unsigned i=0;i<48;++i)assert(payload[i]==uint8_t(i+1));
    ++sends;errno=ERANGE;return -319;
}
void send() {
    A::VehicleData data={1,payload,48};borrowed=&data;const unsigned before=sends;
    errno=EDOM;assert(A::send_vehicle_data(&sends,&data)==-319&&errno==ERANGE&&sends==before+1);
}
bool provenance(void*,const A::PositionContext& c,A::Provenance* out,void*) {
    const unsigned n=index(c.call_sequence);++checks[n];
    assert(c.lds_association&&reads[n]==1);
    if(is("missing"))assert(!lookups[n]);
    else if(is("request_failed"))assert(lookups[n]<=1);
    else assert(lookups[n]==1);
    check_request(c,n);
    capture(n,c.prediction_generation);
    same_owned(*c.lds_association,expected(n));
    const L::Owned before=*c.lds_association;
    if((is("nested")||is("nested_missing"))&&n==0) {
        A::position_enter(0,raw[1]);send();A::position_leave();
        same_owned(before,*c.lds_association);
        assert(c.request_trace.request.id==10);
    }
    // Association is deliberately not physical provenance.
    *out=A::Provenance();errno=EBUSY;
    if(is("provenance_failed")) {
        out->source_epoch=7;out->session_epoch=8;
        out->exact_request=out->verified_lds=out->legacy_receiver=true;
    }
    return false;
}
void sink(const A::Observation* o,void*) {
    assert(o->kind==A::Observation::POSITION||o->kind==A::Observation::SEND);
    const unsigned n=index(o->call_sequence);assert(n<frames());
    if(seen[n])assert(o->prediction_generation==generations[n]);
    same_owned(o->lds_association,expected(n));
    const A::Provenance& p=o->provenance;
    assert(!p.source_epoch&&!p.session_epoch);
    assert(!p.exact_request&&!p.verified_lds&&!p.legacy_receiver);
    if(o->kind==A::Observation::POSITION) {assert(++position_count[n]==1);positions[n]=*o;}
    else {assert(++send_count[n]==1);sent[n]=*o;}
    errno=EIO;
}
}
int main(int argc,char** argv) {
#ifdef MX5_ASSOCIATION_CONTEXT_DSO_TEST
    initialize_association_context_dso();
#endif
    assert(argc==2);scenario=argv[1];assert(known());
    for(unsigned i=0;i<48;++i)payload[i]=uint8_t(i+1);
    A::Options options=A::Options();options.sink=sink;options.clock=clock_fn;
    options.request_reader=request;options.provenance=provenance;
    options.association_reader=is("missing")?0:association;
    assert(A::configure(next,options));
    const bool malformed=is("malformed");
    errno=EDOM;A::position_enter(0,malformed?0:raw[0]);assert(errno==EDOM);
    if(malformed)assert(!lookups[0]&&!checks[0]);
    else assert(checks[0]==1);
    if(is("mutate_after"))A::invalidate();
    send();A::position_leave();
    if(is("frame_reuse")) {
        assert(!reads[1]&&!lookups[1]&&!checks[1]);
        errno=EDOM;A::position_enter(0,raw[1]);assert(errno==EDOM);
        assert(lookups[1]==1&&checks[1]==1);
        send();A::position_leave();
    }
    const unsigned count=frames();
    assert(sends==count);
    for(unsigned f=0;f<2;++f) {
        const unsigned active=f<count?1U:0U;
        assert(reads[f]==active&&position_count[f]==active&&send_count[f]==active);
        assert(checks[f]==(malformed?0U:active));
        if(!active) {assert(!lookups[f]);continue;}
        if(malformed||is("missing"))assert(!lookups[f]);
        else if(is("request_failed"))assert(lookups[f]<=1);
        else assert(lookups[f]==1);
        assert(positions[f].kind==A::Observation::POSITION&&positions[f].call_sequence==f+1);
        assert(sent[f].kind==A::Observation::SEND&&sent[f].call_sequence==f+1);
        assert(positions[f].prediction_generation==sent[f].prediction_generation);
        const L::Owned want=expected(f);
        same_owned(positions[f].lds_association,want);
        same_owned(sent[f].lds_association,want);
        same_owned(positions[f].lds_association,sent[f].lds_association);
    }
    std::printf("PASS association context %s: owned callback and original send\n",scenario);
}
