#include "lds_request_source.h"
#include <cstring>

namespace mx5 { namespace runtime {
namespace {
namespace A=adapter;
namespace L=lds_sideband;
namespace Q=request_trace;
typedef LdsRequestSource Source;
struct Key {
    Q::Text guid,client,server;
    uint32_t request,response,reply;
};
void count(uint64_t& value) { if(value!=UINT64_MAX)++value; }
bool complete(const Q::Text& t) {
    return t.known&&t.complete&&t.bytes[0]&&strnlen(t.bytes,sizeof t.bytes)<sizeof t.bytes;
}
bool equal(const Q::Text& a,const Q::Text& b) {
    return complete(a)&&complete(b)&&!strncmp(a.bytes,b.bytes,sizeof a.bytes);
}
bool same(const Key& a,const Key& b) {
    return a.request==b.request&&a.response==b.response&&a.reply==b.reply&&
        equal(a.guid,b.guid)&&equal(a.client,b.client)&&equal(a.server,b.server);
}
bool key(const A::Observation& o,Key* out) {
    const Q::Trace& t=o.request_trace;
    out->guid=t.issue.endpoint.server_guid;out->client=t.issue.endpoint.unique_name;
    out->server=t.reply.wire.sender;out->request=t.issue.wire.serial;
    out->response=t.reply.wire.serial;out->reply=t.reply.wire.reply_serial;
    return complete(out->guid)&&complete(out->client)&&complete(out->server)&&
        out->request&&out->response&&out->reply;
}
bool key(const L::Record& r,Key* out) {
    out->guid=r.wire.server_guid;out->client=r.wire.client_unique;out->server=r.wire.server_unique;
    out->request=r.wire.request_serial;out->response=r.wire.response_serial;out->reply=r.wire.reply_serial;
    return complete(out->guid)&&complete(out->client)&&complete(out->server)&&
        out->request&&out->response&&out->reply;
}
bool identity(const A::Observation& a,const A::Observation& b) {
    return a.call_sequence==b.call_sequence&&a.prediction_generation==b.prediction_generation&&
        a.request_trace.request.id==b.request_trace.request.id&&
        a.request_trace.request.epoch==b.request_trace.request.epoch&&
        a.request_trace.worker.id==b.request_trace.worker.id&&
        a.request_trace.worker.epoch==b.request_trace.worker.epoch;
}
Source::Result position_state(const A::Observation& o) {
    const Q::Trace& t=o.request_trace;Key k;
    if(o.request_result!=Q::OK||!t.request.id||!t.worker.id||!t.request.epoch||
       t.request.epoch!=t.worker.epoch)return Source::UNAVAILABLE;
    if(t.issue.wire.conflict||(t.issue.wire.known&&t.reply.wire.known&&
       t.issue.wire.serial&&t.reply.wire.reply_serial&&t.issue.wire.serial!=t.reply.wire.reply_serial))
        return Source::CONFLICT;
    if(!key(o,&k)||!t.issue.wire.known||!t.issue.wire.endpoint_matched||
       !t.reply.wire.known||t.reply.wire.type!=2)return Source::UNAVAILABLE;
    return Source::WAITING;
}
Source::Result record_state(const L::Record& r) {
    Key k;
    if((r.flags&L::CHAIN_CONFLICT)||(r.wire.request_serial&&r.wire.reply_serial&&
       r.wire.request_serial!=r.wire.reply_serial)||
       (complete(r.wire.destination)&&complete(r.wire.client_unique)&&
        !equal(r.wire.destination,r.wire.client_unique)))return Source::CONFLICT;
    const unsigned required=L::SNAPSHOT_KNOWN|L::REQUEST_KNOWN|L::REPLY_KNOWN|
        L::RAW_SEND_SUCCEEDED|L::PATH_RETURNED|L::RAW_SEND_CALLED;
    if(!key(r,&k)||!equal(r.wire.destination,r.wire.client_unique)||
       (r.flags&required)!=required||!r.send_result||r.reply_type!=2)return Source::UNAVAILABLE;
    // These are cache-assignment observations, not a measurement clock/epoch.
    const L::Lineage& lineage=r.field_lineage;
    if(!lineage.lifetime&&lineage.write_sequence)return Source::UNAVAILABLE;
    for(unsigned i=0;i<sensors::lds_lineage::FIELD_COUNT;++i)
        if(lineage.fields[i].write_sequence>lineage.write_sequence||
           (!lineage.fields[i].write_sequence&&lineage.fields[i].observed_ns))return Source::UNAVAILABLE;
    return Source::WAITING;
}
bool same_double(double a,double b) {
    uint64_t x,y;memcpy(&x,&a,sizeof x);memcpy(&y,&b,sizeof y);return x==y;
}
bool payload(const A::PositionInput& a,const A::PositionInput& b) {
    // No native-struct padding comparison, tolerance or numeric write inference.
    return a.mode==b.mode&&a.utc_seconds==b.utc_seconds&&a.altitude_m==b.altitude_m&&
        same_double(a.latitude_deg,b.latitude_deg)&&same_double(a.longitude_deg,b.longitude_deg)&&
        same_double(a.heading_deg,b.heading_deg)&&same_double(a.velocity_kmh,b.velocity_kmh)&&
        same_double(a.horizontal,b.horizontal)&&same_double(a.vertical,b.vertical);
}
bool same_record(const L::Record& a,const L::Record& b) {
    unsigned char x[L::RECORD_SIZE],y[L::RECORD_SIZE];
    return L::encode(a,x)&&L::encode(b,y)&&!memcmp(x,y,sizeof x);
}
bool record_identity(const L::Record& a,const L::Diagnostic& da,const L::Record& b,const L::Diagnostic& db) {
    return da.sender_pid==db.sender_pid&&da.sender_uid==db.sender_uid&&
        a.source_instance==b.source_instance&&a.sequence==b.sequence;
}
bool position_clock(const A::Observation& o,uint64_t now,uint64_t floor) {
    // Only immutable original issue time can cross a retirement boundary.
    return now&&o.mono_ns&&o.mono_ns<=now&&o.request_trace.issue.observed_ns&&
        o.request_trace.issue.observed_ns<=now&&o.request_trace.issue.observed_ns>floor&&o.mono_ns>floor;
}
bool record_clock(const L::Record& r,uint64_t now,uint64_t floor) {
    return now&&r.observed_ns&&r.observed_ns<=now&&r.observed_ns>floor;
}
}

LdsRequestSource::LdsRequestSource():entries_(),status_(),now_(0) {}
uint64_t LdsRequestSource::revision() {
    if(status_.revision==UINT64_MAX) {
        status_.exhausted=true;
        for(unsigned i=0;i<CAPACITY;++i)entries_[i].used=false;
        status_.entries=0;return 0;
    }
    return ++status_.revision;
}
void LdsRequestSource::retire(uint64_t now) {
    if(now>now_)now_=now;
    if(now_>status_.floor_ns)status_.floor_ns=now_;
    if(status_.entries)count(status_.retirements);
    for(unsigned i=0;i<CAPACITY;++i)entries_[i].used=false;
    status_.entries=0;
    if(!status_.exhausted)revision();
}
void LdsRequestSource::advance(uint64_t now) {
    if(!now)return;
    if(now_&&now<now_) { retire(now_);return; }
    now_=now;
    for(unsigned i=0;i<CAPACITY;++i)
        if(entries_[i].used&&now_-entries_[i].first_ns>=RETENTION_NS) { retire(now_);return; }
}
void LdsRequestSource::reset(uint64_t now) { retire(now); }
LdsRequestSource::Entry* LdsRequestSource::allocate() {
    if(status_.exhausted)return 0;
    for(unsigned i=0;i<CAPACITY;++i)if(!entries_[i].used) {
        const uint64_t rev=revision();if(!rev)return 0;
        Entry& e=entries_[i];e=Entry();e.used=true;e.first_ns=now_;e.revision=rev;e.state=WAITING;
        ++status_.entries;return &e;
    }
    // Forgetting just one contradiction would permit its replay to look unique.
    // End this whole evidence window and resume only post-boundary requests.
    retire(now_);return 0;
}
void LdsRequestSource::conflict(Entry& e) {
    if(e.state==CONFLICT)return;
    const uint64_t rev=revision();if(!rev)return;
    e.revision=rev;e.state=CONFLICT;count(status_.conflicts);
}
void LdsRequestSource::refresh(Entry& e) {
    if(e.state==CONFLICT||e.state==PAYLOAD_MISMATCH)return;
    const Result p=e.has_position?position_state(e.observation):WAITING;
    const Result r=e.has_record?record_state(e.record):WAITING;
    if(p==CONFLICT||r==CONFLICT) { conflict(e);return; }
    if(p==UNAVAILABLE||r==UNAVAILABLE) { e.state=UNAVAILABLE;return; }
    if(!e.has_position||!e.has_record) { e.state=WAITING;return; }
    Key a,b;
    if(!key(e.observation,&a)||!key(e.record,&b)||!same(a,b)) { conflict(e);return; }
    // The observed reply Path cannot complete before this same-machine request
    // was issued. This rejects old-key replay, not old physical measurements.
    if(e.record.observed_ns<e.observation.request_trace.issue.observed_ns) { conflict(e);return; }
    if(!payload(e.observation.position,e.record.position)) { e.state=PAYLOAD_MISMATCH;return; }
    if(e.state!=MATCHED)count(status_.matches);
    e.state=MATCHED;
}
void LdsRequestSource::position(const adapter::Observation& o,uint64_t now) {
    if(now)advance(now);
    count(status_.positions);
    if(status_.exhausted||o.kind!=adapter::Observation::POSITION||
       !position_clock(o,now_,status_.floor_ns)) { count(status_.rejected);return; }
    Key incoming;const bool keyed=key(o,&incoming);
    Entry* by_identity=0;Entry* by_key=0;bool collision=false;
    for(unsigned i=0;i<CAPACITY;++i) {
        Entry& e=entries_[i];if(!e.used)continue;
        Key existing;
        if(e.has_position&&identity(e.observation,o)) {
            conflict(e); // position_take is one-shot; a repeated callback is ambiguity.
            by_identity=&e;collision=true;
        }
        if(keyed&&((e.has_position&&key(e.observation,&existing)&&same(incoming,existing))||
                  (e.has_record&&key(e.record,&existing)&&same(incoming,existing)))) {
            if(e.has_position) { conflict(e);collision=true; }
            by_key=&e;
        }
    }
    if(status_.exhausted)return;
    Entry* target=by_key?by_key:by_identity;
    if(target&&target->has_position) {
        Key prior;
        if(identity(target->observation,o)&&
           (!keyed||!key(target->observation,&prior)||same(incoming,prior)))return;
        // Retain this additional conflicting identity/key, so changing its key
        // later cannot make the already observed ambiguity disappear.
        target=0;
    }
    if(!target)target=allocate();
    if(!target) { count(status_.rejected);return; }
    const uint64_t rev=revision();if(!rev)return;
    target->observation=o;target->has_position=true;target->revision=rev;
    if(collision)conflict(*target);
    refresh(*target);
}
void LdsRequestSource::sideband(const L::Record& r,const L::Diagnostic& d,uint64_t now) {
    if(now)advance(now);
    count(status_.records);
    unsigned char encoded[L::RECORD_SIZE];
    if(status_.exhausted||d.fault!=L::RECEIVE_OK||d.sender_pid<=0||!L::encode(r,encoded)||
       !record_clock(r,now_,status_.floor_ns)) { count(status_.rejected);return; }
    Key incoming;const bool keyed=key(r,&incoming);
    Entry* target=0;bool collision=false;
    for(unsigned i=0;i<CAPACITY;++i) {
        Entry& e=entries_[i];if(!e.used)continue;
        if(e.has_record&&record_identity(r,d,e.record,e.diagnostic)) {
            if(same_record(r,e.record))return; // Receipt time cannot renew the first entry.
            collision=true;conflict(e);
        }
        Key existing;
        if(keyed&&((e.has_position&&key(e.observation,&existing)&&same(incoming,existing))||
                   (e.has_record&&key(e.record,&existing)&&same(incoming,existing))))target=&e;
    }
    if(status_.exhausted)return;
    if(target&&target->has_record) {
        conflict(*target);
        if(record_identity(r,d,target->record,target->diagnostic))return;
        // Preserve a second sender identity that collided on this wire key.
        target=0;collision=true;
    }
    if(!target)target=allocate();
    if(!target) { count(status_.rejected);return; }
    const uint64_t rev=revision();if(!rev)return;
    target->record=r;target->diagnostic=d;target->has_record=true;target->revision=rev;
    if(collision)conflict(*target);
    refresh(*target);
}
LdsRequestSource::Result LdsRequestSource::lookup(const adapter::Observation& o,JoinedReply* out) const {
    if(out)*out=JoinedReply();
    if(status_.exhausted||o.kind!=adapter::Observation::POSITION||
       !position_clock(o,now_,status_.floor_ns))return UNAVAILABLE;
    for(unsigned i=0;i<CAPACITY;++i) {
        const Entry& e=entries_[i];if(!e.used||!e.has_position||!identity(e.observation,o))continue;
        if(e.state==MATCHED&&out) {
            out->owner_=this;out->revision=e.revision;out->observation=e.observation;
            out->record=e.record;out->diagnostic=e.diagnostic;
        }
        return e.state;
    }
    // A second callback with different owned tokens poisons the shared key but
    // is never returned as if it were the first callback's immutable payload.
    Key incoming;if(key(o,&incoming))for(unsigned i=0;i<CAPACITY;++i) {
        const Entry& e=entries_[i];Key existing;
        if(e.used&&e.state==CONFLICT&&
           ((e.has_position&&key(e.observation,&existing)&&same(incoming,existing))||
            (e.has_record&&key(e.record,&existing)&&same(incoming,existing))))return CONFLICT;
    }
    return position_state(o)==UNAVAILABLE?UNAVAILABLE:NOT_FOUND;
}
bool LdsRequestSource::current(const JoinedReply& joined) const {
    if(joined.owner_!=this||!joined.revision||status_.exhausted)return false;
    for(unsigned i=0;i<CAPACITY;++i) {
        const Entry& e=entries_[i];
        if(e.used&&e.state==MATCHED&&e.revision==joined.revision&&identity(e.observation,joined.observation))return true;
    }
    return false;
}

} }
