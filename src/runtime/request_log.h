#ifndef MX5_RUNTIME_REQUEST_LOG_H
#define MX5_RUNTIME_REQUEST_LOG_H
#include "request_trace.h"
#include <cstdio>
#include <cstring>

namespace mx5 { namespace runtime {
// A complete escaped request plus the owned LDS association and SEND payload
// needs more than the request-only buffer. Formatting stays on the worker.
enum { REQUEST_JSON_CAPACITY = 6144, OBSERVATION_JSON_CAPACITY = 8192 };
namespace request_log_detail {
class Json {
    char* out_;size_t cap_,used_;bool ok_;
public:
    Json(char* out,size_t cap):out_(out),cap_(cap),used_(0),ok_(out && cap) { if(ok_)out[0]=0; }
    void add(const char* s) {
        if(!ok_)return;
        const size_t n=std::strlen(s);
        if(n>=cap_-used_) { ok_=false;out_[0]=0;return; }
        std::memcpy(out_+used_,s,n+1);used_+=n;
    }
    void number(const char* name,uint64_t value,bool known=true) {
        add(",\"");add(name);add("\":");
        char text[32];::snprintf(text,sizeof text,"%llu",(unsigned long long)value);
        add(known?text:"null");
    }
    void signed_number(const char* name,int32_t value,bool known) {
        add(",\"");add(name);add("\":");
        char text[32];::snprintf(text,sizeof text,"%d",value);add(known?text:"null");
    }
    void text(const char* name,const request_trace::Text& t,bool first=false) {
        if(!first)add(",");
        add("\"");add(name);add("\":{\"value\":");
        bool terminated=false;
        if(!t.known)add("null");
        else {
            add("\"");
            for(size_t i=0;i<sizeof t.bytes;++i) {
                const unsigned char c=static_cast<unsigned char>(t.bytes[i]);
                if(!c) { terminated=true;break; }
                char escaped[7];
                // Byte-wise escaping also keeps unexpected non-UTF8 valid JSON.
                if(c<32 || c>=127 || c=='"' || c=='\\')
                    ::snprintf(escaped,sizeof escaped,"\\u%04x",unsigned(c));
                else { escaped[0]=char(c);escaped[1]=0; }
                add(escaped);
            }
            add("\"");
        }
        add(",\"complete\":");add(t.known && t.complete && terminated?"true":"false");add("}");
    }
    bool ok() const { return ok_; }
};
}
inline bool format_session_trace(char* out,size_t cap,const session_trace::Snapshot& s,bool send) {
    const bool observed=s.result==session_trace::OBSERVED;
    request_log_detail::Json j(out,cap);
    j.add("{\"result\":\"");j.add(session_trace::result_name(s.result));
    j.add("\",\"basis\":\"");j.add(send?"send_storage":"unique_live_context");j.add("\"");
    j.number("lifetime",s.lifetime,observed);
    j.number("revision",s.revision,observed || s.result==session_trace::NONE ||
             s.result==session_trace::AMBIGUOUS);
    j.number("event",s.event,observed && s.state_known);
    j.signed_number("state",s.state,observed && s.state_known);j.add("}");
    return j.ok();
}
inline bool format_bus_trace(char* out,size_t cap,const bus_trace::Snapshot& s) {
    request_log_detail::Json j(out,cap);
    j.add("{\"result\":\"");j.add(bus_trace::result_name(s.result));j.add("\"");
    j.number("object",s.object,s.result==bus_trace::CONNECTED || s.result==bus_trace::DISCONNECTED);
    j.number("lifetime",s.lifetime,s.result==bus_trace::CONNECTED);j.add("}");return j.ok();
}
// Only the journal worker formats these copied records. No OEM strings/pointers
// survive into this function. A diagnostic prefix never claims a full identity.
inline bool format_request_trace(char* out,size_t cap,request_trace::Result result,
                                 const request_trace::Trace& input) {
    namespace R=request_trace;
    const R::Trace empty=R::Trace();
    const R::Trace& t=result==R::OK?input:empty;
    request_log_detail::Json j(out,cap);
    j.add("{\"result\":\"");j.add(R::result_name(result));j.add("\",\"association_only\":true");
    j.number("request_id",t.request.id);j.number("request_epoch",t.request.epoch);
    j.number("worker_id",t.worker.id);j.number("worker_epoch",t.worker.epoch);
    j.number("issue_observed_ns",t.issue.observed_ns,t.issue.observed_ns!=0);
    j.number("reply_observed_ns",t.reply.observed_ns,t.reply.observed_ns!=0);
    j.number("bus_lifetime",t.issue.bus_lifetime,t.issue.known&R::ISSUE_BUS_LIFETIME);
    char connection[128];
    if(!format_bus_trace(connection,sizeof connection,t.issue.connection))return false;
    j.add(",\"issue_connection\":");j.add(connection);
    if(!format_bus_trace(connection,sizeof connection,t.reply.connection))return false;
    j.add(",\"reply_connection\":");j.add(connection);
    j.number("session_lifetime",t.issue.session_lifetime,t.issue.known&R::ISSUE_SESSION_LIFETIME);
    j.number("session_event",t.issue.session_event,t.issue.known&R::ISSUE_SESSION_STATE);
    j.signed_number("session_state",t.issue.session_state,t.issue.known&R::ISSUE_SESSION_STATE);
    char session[200];
    if(!format_session_trace(session,sizeof session,t.issue.session_context,false))return false;
    j.add(",\"session_context\":");j.add(session);
    j.add(",\"route\":{");
    j.text("destination",t.issue.route.destination,true);
    j.text("path",t.issue.route.path);j.text("interface",t.issue.route.interface_name);
    j.text("member",t.issue.route.member);j.add("}");
    j.add(",\"endpoint\":{");j.text("server_guid",t.issue.endpoint.server_guid,true);
    j.text("unique_name",t.issue.endpoint.unique_name);j.add("}");
    j.signed_number("reply_type",t.reply.type,t.reply.type_known);
    j.number("wire_serial",t.reply.wire_serial,t.reply.wire_serial_known);
    j.text("sender",t.reply.sender);j.text("error",t.reply.error_name);
    // Raw transport headers are separate from the original public-getter
    // observations above. In particular, a known local error can have serial
    // zero and no sender; neither observation confers request qualification.
    j.add(",\"wire\":{\"issue\":{\"known\":");j.add(t.issue.wire.known?"true":"false");
    j.number("observed_ns",t.issue.wire.observed_ns,t.issue.wire.known && t.issue.wire.observed_ns);
    j.number("serial",t.issue.wire.serial,t.issue.wire.known);
    j.add(",\"conflict\":");j.add(t.issue.wire.conflict?"true":"false");
    j.add(",\"endpoint_matched\":");
    j.add(t.issue.wire.known && !t.issue.wire.conflict && t.issue.wire.endpoint_matched?"true":"false");j.add("}");
    const R::WireReply& raw=t.reply.wire.known?t.reply.wire:empty.reply.wire;
    j.add(",\"reply\":{\"known\":");j.add(raw.known?"true":"false");
    j.number("observed_ns",raw.observed_ns,raw.known && raw.observed_ns);
    j.number("serial",raw.serial,raw.known);j.number("reply_serial",raw.reply_serial,raw.known);
    j.signed_number("type",raw.type,raw.known);
    j.text("sender",raw.sender);j.text("error",raw.error_name);j.add("}}}");
    return j.ok();
}
} }
#endif
