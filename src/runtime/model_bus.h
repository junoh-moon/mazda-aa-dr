#ifndef MX5_RUNTIME_MODEL_BUS_H
#define MX5_RUNTIME_MODEL_BUS_H
#include "adapter/adapter.h"
namespace mx5 { namespace runtime {
// Worker-owned MODEL fence. An observed LDS connection is not qualified
// provider identity, sensor measurement time, or proof of phone acceptance.
class ModelBus {
public:
    enum Update { INITIAL, SAME, CHANGED };
    ModelBus():current_(),epoch_(0),since_ns_(0),exhausted_(false) {}
    Update update(const bus_trace::Boundary& b,uint64_t now) {
        if(epoch_ && current_.revision==b.revision && same(current_.connection,b.connection))return SAME;
        const Update result=epoch_?CHANGED:INITIAL;
        if(epoch_==UINT64_MAX)exhausted_=true;else ++epoch_;
        current_=b;since_ns_=now;return result;
    }
    bool available() const {
        const bus_trace::Snapshot& c=current_.connection;
        return !exhausted_ && epoch_ && current_.revision && c.result==bus_trace::CONNECTED &&
               c.object && c.lifetime;
    }
    const char* reject(const adapter::Observation& o) const {
        if(!available())return "bus_unavailable";
        const request_trace::Trace& t=o.request_trace;
        if(o.request_result!=request_trace::OK || !(t.issue.known&request_trace::ISSUE_BUS_LIFETIME) ||
           !t.issue.bus_lifetime || t.issue.bus_lifetime!=t.issue.connection.lifetime)
            return "request_bus_unobserved";
        if(!same(t.issue.connection,current_.connection) || !same(t.reply.connection,current_.connection))
            return "bus_changed_since_issue";
        if(t.issue.observed_ns<since_ns_)return "request_before_bus_boundary";
        return 0;
    }
    uint64_t epoch() const { return epoch_; }
    uint64_t since_ns() const { return since_ns_; }
    bool exhausted() const { return exhausted_; }
    const bus_trace::Boundary& current() const { return current_; }
private:
    bus_trace::Boundary current_;
    uint64_t epoch_,since_ns_;
    bool exhausted_;
    static bool same(const bus_trace::Snapshot& a,const bus_trace::Snapshot& b) {
        return a.result==b.result && a.object==b.object && a.lifetime==b.lifetime;
    }
};
} }
#endif
