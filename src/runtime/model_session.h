#ifndef MX5_RUNTIME_MODEL_SESSION_H
#define MX5_RUNTIME_MODEL_SESSION_H
#include "adapter/adapter.h"

namespace mx5 { namespace runtime {
// Worker-owned negative fence, never qualified provenance or phone acceptance.
// A revision also detects a completed transition between worker wakeups that
// leaves the surviving lifetime/status unchanged (e.g. a failed second create).
class ModelSession {
public:
    enum Update { INITIAL, SAME, CHANGED };
    ModelSession():current_(),seen_(false),since_ns_(0),accept_declined_(false) {}
    // BETA only (validation/ASSIST_BETA_DESIGN_2026-10-05.md decision 6): the
    // installer KNOWINGLY skipped session observation because a third-party
    // shim owns the session slots (InstallReport.sessions_declined). Then the
    // constant never-prepared UNOBSERVED snapshot is the fence and the BETA
    // runtime counts send-time session_storage changes instead. Any other
    // non-OBSERVED result (transition, ambiguity, fault, no live session)
    // stays unavailable. Set once before the worker's first update.
    void accept_declined(bool value) { accept_declined_=value; }
    Update update(const session_trace::Snapshot& s,uint64_t now) {
        if(seen_ && same(current_,s))return SAME;
        const Update result=seen_?CHANGED:INITIAL;
        current_=s;seen_=true;since_ns_=now;return result;
    }
    bool available() const {
        if(!seen_)return false;
        if(current_.result==session_trace::OBSERVED && current_.lifetime && current_.revision)
            return true;
        return accept_declined_ && declined(current_);
    }
    const char* reject(const adapter::Observation& o) const {
        if(!available())return "session_unavailable";
        const request_trace::Trace& t=o.request_trace;
        if(o.request_result!=request_trace::OK || !t.request.id || !t.worker.id ||
           !t.request.epoch || t.request.epoch!=t.worker.epoch)return "request_unobserved";
        if(!same(t.issue.session_context,current_))return "session_changed_since_issue";
        if(!t.issue.observed_ns || !t.reply.observed_ns ||
           t.issue.observed_ns>t.reply.observed_ns || t.reply.observed_ns>o.mono_ns)
            return "request_time_order";
        return 0;
    }
    uint64_t since_ns() const { return since_ns_; }
    const session_trace::Snapshot& current() const { return current_; }
private:
    session_trace::Snapshot current_;
    bool seen_;
    uint64_t since_ns_;
    bool accept_declined_;
    static bool declined(const session_trace::Snapshot& s) {
        return s.result==session_trace::UNOBSERVED && !s.lifetime && !s.event &&
            !s.state_known && !s.revision;
    }
    static bool same(const session_trace::Snapshot& a,const session_trace::Snapshot& b) {
        return a.result==b.result && a.revision==b.revision && a.lifetime==b.lifetime &&
            a.event==b.event && a.state_known==b.state_known && (!a.state_known || a.state==b.state);
    }
};
} }
#endif
