#include "runtime/lds_request_source.h"
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>

namespace R=mx5::runtime;
namespace A=mx5::adapter;
namespace L=R::lds_sideband;
namespace Q=R::request_trace;
typedef R::LdsRequestSource Source;
static unsigned checks;
#define CHECK(x) do { ++checks; if(!(x)) { std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);std::fflush(stderr);assert(x); } } while(0)
static Q::Text text(const char* p) { Q::Text t;L::copy_text(&t,p);return t; }
static A::Observation position(uint64_t id=1,uint64_t when=100) {
    A::Observation o=A::Observation();o.kind=A::Observation::POSITION;
    o.call_sequence=uint32_t(id);o.prediction_generation=5;o.mono_ns=when+2;
    o.position.mode=o.original_mode=1;o.position.utc_seconds=1700000000;
    o.position.latitude_deg=35;o.position.longitude_deg=129;o.position.altitude_m=-12;
    o.position.heading_deg=280;o.position.velocity_kmh=30;o.position.horizontal=1;o.position.vertical=1.5;
    o.request_result=Q::OK;Q::Trace& t=o.request_trace;
    t.request=Q::Token{id,7};t.worker=Q::Token{id+100,7};t.issue.observed_ns=when;
    t.issue.endpoint.server_guid=text("transport-guid");t.issue.endpoint.unique_name=text(":1.20");
    t.issue.wire.known=t.issue.wire.endpoint_matched=true;t.issue.wire.serial=uint32_t(id+10);
    t.reply.wire.known=true;t.reply.wire.type=2;t.reply.wire.serial=uint32_t(id+20);
    t.reply.wire.reply_serial=t.issue.wire.serial;t.reply.wire.sender=text(":1.30");
    return o;
}
static L::Record record(const A::Observation& o,uint64_t sequence=1) {
    L::Record r=L::Record();r.source_instance=77;r.sequence=sequence;r.observed_ns=o.mono_ns+10;
    r.flags=111;r.send_result=1;r.reply_type=2;r.position=o.position;
    r.wire.server_guid=o.request_trace.issue.endpoint.server_guid;
    r.wire.client_unique=r.wire.destination=o.request_trace.issue.endpoint.unique_name;
    r.wire.server_unique=o.request_trace.reply.wire.sender;
    r.wire.request_serial=r.wire.reply_serial=o.request_trace.issue.wire.serial;
    r.wire.response_serial=o.request_trace.reply.wire.serial;
    r.field_lineage.lifetime=2;r.field_lineage.write_sequence=3;
    for(unsigned i=0;i<9;++i) { r.field_lineage.fields[i].write_sequence=i%3+1;r.field_lineage.fields[i].observed_ns=50+i; }
    return r;
}
static L::Diagnostic diagnostic(uint64_t when=120) {
    L::Diagnostic d=L::Diagnostic();d.fault=L::RECEIVE_OK;d.sender_pid=42;d.received_ns=when;return d;
}
static void owned_match() {
    for(unsigned reverse=0;reverse<2;++reverse) {
        Source source;A::Observation o=position();L::Record r=record(o);L::Diagnostic d=diagnostic();
        const A::Observation identity=o;source.advance(1000); // Startup baseline may be later than queued data.
        if(reverse) { source.sideband(r,d,1000);source.position(o,1001); }
        else { source.position(o,1000);source.sideband(r,d,1001); }
        Source::JoinedReply joined;
        CHECK(source.lookup(identity,&joined)==Source::MATCHED);
        CHECK(source.current(joined));CHECK(joined.revision!=0);
        o.position.latitude_deg=9;r.position.latitude_deg=8;r.field_lineage.fields[0].write_sequence=99;
        d.sender_pid=99;
        CHECK(joined.observation.position.latitude_deg==35&&joined.record.position.latitude_deg==35);
        CHECK(joined.record.field_lineage.fields[0].write_sequence==1&&joined.diagnostic.sender_pid==42);
        CHECK(!joined.observation.provenance.exact_request&&!joined.observation.provenance.verified_lds);
        CHECK(!joined.observation.provenance.legacy_receiver&&joined.observation.provenance.source_epoch==0);
        CHECK(joined.observation.request_trace.issue.session_context.result==R::session_trace::UNOBSERVED);
        CHECK(joined.record.observed_ns>joined.observation.mono_ns);
    }
}
static Source::JoinedReply join(Source& s,const A::Observation& o,uint64_t now=1000) {
    s.position(o,now);s.sideband(record(o,o.request_trace.request.id),diagnostic(now),now);
    Source::JoinedReply out;CHECK(s.lookup(o,&out)==Source::MATCHED);CHECK(s.current(out));return out;
}
static void clean_output(Source& s,const A::Observation& o,Source::Result wanted) {
    Source::JoinedReply out;out.revision=99;out.observation.call_sequence=77;
    out.record.sequence=88;out.diagnostic.sender_pid=99;
    CHECK(s.lookup(o,&out)==wanted);CHECK(out.revision==0&&out.observation.call_sequence==0);
    CHECK(out.record.sequence==0&&out.diagnostic.sender_pid==0);CHECK(!s.current(out));
}
static void failed_adapter_context() {
    Source source;A::Observation first=position();
    const Source::JoinedReply prior=join(source,first);
    A::Observation lost=position(2,1100);lost.reason=A::CONTEXT_UNAVAILABLE;
    source.position(lost,2000);
    source.sideband(record(lost,2),diagnostic(2001),2001);
    clean_output(source,lost,Source::UNAVAILABLE);
    CHECK(!source.current(prior));
    CHECK(source.status().rejected>=1&&source.status().matches==1);
    join(source,position(3,3000),4000);
}
static void reversal_and_identity() {
    Source s;A::Observation a=position(1),b=position(2);
    L::Record ra=record(a,2),rb=record(b,1);
    ra.field_lineage.write_sequence=3;rb.field_lineage.write_sequence=99;
    s.position(a,1000);s.position(b,1001);s.sideband(rb,diagnostic(),1002);s.sideband(ra,diagnostic(),1003);
    Source::JoinedReply ja,jb;CHECK(s.lookup(a,&ja)==Source::MATCHED);CHECK(s.lookup(b,&jb)==Source::MATCHED);
    CHECK(s.current(ja)&&s.current(jb));CHECK(ja.record.sequence>jb.record.sequence);
    CHECK(ja.record.field_lineage.write_sequence<jb.record.field_lineage.write_sequence);
    A::Observation other=a;
    ++other.request_trace.request.id;clean_output(s,other,Source::NOT_FOUND);other=a;
    ++other.request_trace.request.epoch;clean_output(s,other,Source::UNAVAILABLE);other=a;
    ++other.request_trace.worker.id;clean_output(s,other,Source::NOT_FOUND);other=a;
    ++other.request_trace.worker.epoch;clean_output(s,other,Source::UNAVAILABLE);other=a;
    ++other.call_sequence;clean_output(s,other,Source::NOT_FOUND);other=a;
    ++other.prediction_generation;clean_output(s,other,Source::NOT_FOUND);
    Source unrelated;join(unrelated,a);CHECK(!unrelated.current(ja));
}
static void late_conflicts() {
    Source s;A::Observation o=position();const Source::JoinedReply first=join(s,o);
    L::Record r=record(o);s.sideband(r,diagnostic(2000),2000); // Exact replay doesn't replace receipt or revision.
    Source::JoinedReply same;CHECK(s.lookup(o,&same)==Source::MATCHED);
    CHECK(same.revision==first.revision&&same.diagnostic.received_ns==1000);CHECK(s.current(first));
    ++r.field_lineage.fields[0].observed_ns;s.sideband(r,diagnostic(),2001);
    clean_output(s,o,Source::CONFLICT);CHECK(!s.current(first));
    s.sideband(record(o),diagnostic(),2002);clean_output(s,o,Source::CONFLICT);

    Source second;const Source::JoinedReply old=join(second,o);
    A::Observation b=position(2);L::Record stolen=record(b,1);
    second.sideband(stolen,diagnostic(),1001);second.position(b,1002);
    clean_output(second,o,Source::CONFLICT);clean_output(second,b,Source::CONFLICT);CHECK(!second.current(old));

    Source duplicate;const Source::JoinedReply prior=join(duplicate,o);
    A::Observation copy=o;copy.request_trace.request.id=91;copy.request_trace.worker.id=92;
    duplicate.position(copy,1001);clean_output(duplicate,o,Source::CONFLICT);
    clean_output(duplicate,copy,Source::CONFLICT);CHECK(!duplicate.current(prior));

    Source sender;const Source::JoinedReply before=join(sender,o);
    L::Diagnostic different=diagnostic();different.sender_pid=43;
    sender.sideband(record(o),different,1001);clean_output(sender,o,Source::CONFLICT);CHECK(!sender.current(before));
}
static void identity_aliases() {
    // A second identity on one wire key must not disappear from conflict history.
    Source positions;A::Observation a=position();const Source::JoinedReply old=join(positions,a);
    A::Observation b=a;b.call_sequence=2;b.request_trace.request.id=2;b.request_trace.worker.id=102;
    positions.position(b,1001);clean_output(positions,b,Source::CONFLICT);
    A::Observation fork=position(2);positions.position(fork,1002);
    positions.sideband(record(fork,2),diagnostic(),1003);
    clean_output(positions,fork,Source::CONFLICT);CHECK(!positions.current(old));
    CHECK(positions.status().matches==1);

    // An existing identity may also collide with a second key already waiting.
    Source pending;join(pending,a);A::Observation second=position(2);
    pending.sideband(record(second,2),diagnostic(),1001);
    second.call_sequence=a.call_sequence;second.request_trace.request=a.request_trace.request;
    second.request_trace.worker=a.request_trace.worker;pending.position(second,1002);
    clean_output(pending,second,Source::CONFLICT);CHECK(pending.status().matches==1);

    Source records;join(records,a);L::Record duplicate=record(a,2);
    records.sideband(duplicate,diagnostic(),1001);clean_output(records,a,Source::CONFLICT);
    records.position(fork,1002);records.sideband(record(fork,2),diagnostic(),1003);
    clean_output(records,fork,Source::CONFLICT);CHECK(records.status().matches==1);
}
static void causal_issue() {
    Source s;A::Observation o=position(1,300);L::Record old=record(o);
    old.observed_ns=299; // Same local observation clock, before this request was issued.
    s.position(o,1000);s.sideband(old,diagnostic(),1001);clean_output(s,o,Source::CONFLICT);
    CHECK(s.status().matches==0);
    Source equal;old.observed_ns=300;equal.position(o,1000);equal.sideband(old,diagnostic(),1001);
    Source::JoinedReply out;CHECK(equal.lookup(o,&out)==Source::MATCHED);
    CHECK(!out.observation.provenance.verified_lds);
}
static void payloads() {
    for(unsigned field=0;field<9;++field) {
        Source s;A::Observation o=position();L::Record r=record(o);
        switch(field) {
        case 0:++r.position.mode;break;case 1:++r.position.utc_seconds;break;
        case 2:++r.position.latitude_deg;break;case 3:++r.position.longitude_deg;break;
        case 4:++r.position.altitude_m;break;case 5:++r.position.heading_deg;break;
        case 6:++r.position.velocity_kmh;break;case 7:++r.position.horizontal;break;
        case 8:++r.position.vertical;break;
        }
        s.sideband(r,diagnostic(),1000);s.position(o,1001);clean_output(s,o,Source::PAYLOAD_MISMATCH);
    }
    Source s;A::Observation o=position();o.position.horizontal=std::numeric_limits<double>::quiet_NaN();
    o.position.vertical=-0.0;const Source::JoinedReply out=join(s,o);
    CHECK(out.record.position.horizontal!=out.record.position.horizontal);
    Source zero;L::Record r=record(o);r.position.vertical=0.0;
    zero.position(o,1000);zero.sideband(r,diagnostic(),1001);clean_output(zero,o,Source::PAYLOAD_MISMATCH);
}
static void unavailable() {
    for(unsigned reason=0;reason<16;++reason) {
        Source s;A::Observation o=position();L::Record r=record(o);
        switch(reason) {
        case 0:o.request_result=Q::NOT_FOUND;break;
        case 1:o.request_trace.issue.endpoint.server_guid.known=false;break;
        case 2:o.request_trace.issue.endpoint.unique_name.complete=false;break;
        case 3:o.request_trace.reply.wire.sender=text("");break;
        case 4:o.request_trace.issue.wire.endpoint_matched=false;break;
        case 5:o.request_trace.issue.wire.known=false;break;
        case 6:o.request_trace.reply.wire.known=false;break;
        case 7:o.request_trace.reply.wire.type=3;break;
        case 8:o.request_trace.issue.wire.serial=0;break;
        case 9:r.flags&=~L::SNAPSHOT_KNOWN;break;
        case 10:r.send_result=0;break;
        case 11:r.wire.server_unique.complete=false;break;
        case 12:r.reply_type=3;break;
        case 13:r.field_lineage.fields[0].write_sequence=4;break;
        case 14:o.request_trace.worker.id=0;break;
        case 15:++o.request_trace.worker.epoch;break;
        }
        s.position(o,1000);s.sideband(r,diagnostic(),1001);
        // An incomplete peer name cannot identify this request's counterpart.
        clean_output(s,o,reason==11?Source::WAITING:Source::UNAVAILABLE);
    }
    for(unsigned which=0;which<4;++which) {
        Source s;A::Observation o=position();L::Record r=record(o);
        if(which==0)o.request_trace.issue.wire.conflict=true;
        if(which==1) { ++r.wire.reply_serial;++o.request_trace.reply.wire.reply_serial; }
        if(which==2)r.wire.destination=text(":1.99");
        if(which==3)r.flags|=L::CHAIN_CONFLICT;
        s.position(o,1000);s.sideband(r,diagnostic(),1001);clean_output(s,o,Source::CONFLICT);
    }
    Source s;A::Observation o=position();s.position(o,1000);clean_output(s,o,Source::WAITING);
    L::Diagnostic bad=diagnostic();bad.fault=L::WRONG_CREDENTIALS;
    s.sideband(record(o),bad,1001);clean_output(s,o,Source::WAITING);
    s.sideband(record(o),diagnostic(),1002);Source::JoinedReply out;CHECK(s.lookup(o,&out)==Source::MATCHED);
    CHECK(s.lookup(o,0)==Source::MATCHED);
}
static void loss_and_flags() {
    for(unsigned variant=0;variant<3;++variant) {
        Source s;A::Observation o=position();L::Record r=record(o);
        r.path_result=variant==0?0:(variant==1?100:-123);
        r.dropped_before=UINT64_MAX;r.flags|=L::LOSS_COUNTER_SATURATED;
        r.field_lineage=L::Lineage(); // Initial cache can have no observed field origin.
        s.position(o,1000);s.sideband(r,diagnostic(),1001);Source::JoinedReply out;
        CHECK(s.lookup(o,&out)==Source::MATCHED);CHECK(out.record.path_result==r.path_result);
        CHECK(out.record.dropped_before==UINT64_MAX&&out.record.field_lineage.write_sequence==0);
        CHECK(!out.observation.provenance.verified_lds);
    }
}
static void retirement() {
    Source s;A::Observation o=position();const Source::JoinedReply old=join(s,o);
    s.advance(1000+Source::RETENTION_NS-1);CHECK(s.current(old));
    s.sideband(record(o),diagnostic(),1000+Source::RETENTION_NS-1);
    Source::JoinedReply queried;CHECK(s.lookup(o,&queried)==Source::MATCHED);
    s.advance(1000+Source::RETENTION_NS);CHECK(!s.current(old));
    CHECK(s.status().entries==0&&s.status().retirements==1);
    s.position(o,1001+Source::RETENTION_NS);s.sideband(record(o),diagnostic(),1002+Source::RETENTION_NS);
    clean_output(s,o,Source::UNAVAILABLE);
    A::Observation later=position(2,2000+Source::RETENTION_NS);const Source::JoinedReply good=join(s,later,3000+Source::RETENTION_NS);
    CHECK(s.current(good));s.reset(4000+Source::RETENTION_NS);CHECK(!s.current(good));
    s.position(later,4001+Source::RETENTION_NS);s.sideband(record(later,2),diagnostic(),4002+Source::RETENTION_NS);
    clean_output(s,later,Source::UNAVAILABLE);
    join(s,position(3,5000+Source::RETENTION_NS),6000+Source::RETENTION_NS);
}
static void capacity() {
    Source s;A::Observation o=position();const Source::JoinedReply old=join(s,o);
    L::Record conflicting=record(o);++conflicting.field_lineage.fields[0].observed_ns;
    s.sideband(conflicting,diagnostic(),1001);clean_output(s,o,Source::CONFLICT);
    for(unsigned i=2;i<=Source::CAPACITY;++i)s.sideband(record(position(i),i),diagnostic(),1001+i);
    CHECK(s.status().entries==Source::CAPACITY);
    s.position(position(Source::CAPACITY+1),2000);CHECK(s.status().entries==0);
    CHECK(s.status().retirements==1&&!s.current(old));
    s.position(o,2001);s.sideband(record(o),diagnostic(),2002);clean_output(s,o,Source::UNAVAILABLE);
    join(s,position(100,3000),4000); // Capacity is a window boundary, not permanent disabling.
    CHECK(!s.status().exhausted);

    Source side_only;L::Record a=record(o);side_only.sideband(a,diagnostic(),1000);
    ++a.field_lineage.fields[0].observed_ns;side_only.sideband(a,diagnostic(),1001);
    for(unsigned i=2;i<=Source::CAPACITY;++i)side_only.sideband(record(position(i),i),diagnostic(),1001+i);
    side_only.sideband(record(position(99),99),diagnostic(),2000);
    side_only.position(o,2001);side_only.sideband(record(o),diagnostic(),2002);
    clean_output(side_only,o,Source::UNAVAILABLE);
    join(side_only,position(100,3000),4000);
}
static void clocks() {
    Source s;A::Observation o=position();s.position(o);clean_output(s,o,Source::UNAVAILABLE);
    s.advance(1000);const Source::JoinedReply old=join(s,o);
    s.advance(900);CHECK(!s.current(old)); // Clock regression retires, never moves the floor backwards.
    CHECK(s.status().floor_ns==1000);s.reset();CHECK(s.status().floor_ns==1000);
    A::Observation new_callback=o;new_callback.mono_ns=1500;
    s.position(new_callback,2000);s.sideband(record(new_callback),diagnostic(),2001);
    clean_output(s,new_callback,Source::UNAVAILABLE); // New callback cannot renew an old issue.
    A::Observation future=position(2,UINT64_MAX-20);s.position(future,2002);
    s.sideband(record(future,2),diagnostic(),2003);CHECK(s.status().floor_ns==1000);
    join(s,position(3,3000),4000);
}
namespace mx5 { namespace runtime {
struct LdsRequestSourceTestAccess {
    static void near_exhaustion(LdsRequestSource& s) { s.status_.revision=UINT64_MAX-1; }
};
} }
static void exhaustion_errno() {
    Source s;A::Observation o=position();errno=E2BIG;Source::JoinedReply old=join(s,o);
    CHECK(errno==E2BIG);R::LdsRequestSourceTestAccess::near_exhaustion(s);
    L::Record changed=record(o);++changed.field_lineage.fields[0].observed_ns;s.sideband(changed,diagnostic(),1001);
    s.position(position(2),1002);CHECK(s.status().exhausted);CHECK(!s.current(old));
    s.reset(2000); // Exhausted must never re-issue a wrapping revision.
    s.position(position(3,3000),4000);s.sideband(record(position(3,3000),3),diagnostic(),4001);
    clean_output(s,position(3,3000),Source::UNAVAILABLE);CHECK(errno==E2BIG);
}
int main(int argc,char** argv) {
    const char* wanted=argc>1?argv[1]:"all";
    struct Test { const char* name;void(*run)(); };
    const Test cases[]={
        {"owned",owned_match},{"adapter_loss",failed_adapter_context},
        {"reverse",reversal_and_identity},{"conflicts",late_conflicts},
        {"aliases",identity_aliases},{"causal",causal_issue},
        {"payloads",payloads},{"unavailable",unavailable},{"loss",loss_and_flags},
        {"retention",retirement},{"capacity",capacity},{"clocks",clocks},{"exhaustion",exhaustion_errno}
    };
    unsigned ran=0;
    for(unsigned i=0;i<sizeof cases/sizeof cases[0];++i)
        if(!strcmp(wanted,"all")||!strcmp(wanted,cases[i].name)) { cases[i].run();++ran; }
    CHECK(ran!=0);std::printf("LDS request source %s: %u cases, %u checks PASS\n",wanted,ran,checks);
}
