#include "runtime/model_session.h"
#include <cassert>
#include <cstdio>
#include <cstring>
namespace R=mx5::runtime;
namespace S=R::session_trace;
static mx5::adapter::Observation position(S::Snapshot session) {
    mx5::adapter::Observation o=mx5::adapter::Observation();
    o.kind=mx5::adapter::Observation::POSITION;o.mono_ns=120;
    o.request_trace.request.id=1;o.request_trace.request.epoch=3;
    o.request_trace.worker.id=2;o.request_trace.worker.epoch=3;
    o.request_trace.issue.observed_ns=100;o.request_trace.reply.observed_ns=110;
    o.request_trace.issue.session_context=session;return o;
}
int main() {
    R::ModelSession gate;
    const S::Snapshot s={S::OBSERVED,1,0,0,false,1};
    assert(!gate.available());assert(gate.update(s,90)==R::ModelSession::INITIAL);
    assert(gate.available() && gate.since_ns()==90);
    for(unsigned mode=0;mode<4;++mode) {
        mx5::adapter::Observation o=position(s);o.position.mode=mode;o.prediction_generation=mode+5;
        assert(!gate.reject(o)); // Ordinary GPS/native/GAP control is not an AA lifetime.
        assert(gate.update(s,150)==R::ModelSession::SAME && gate.since_ns()==90);
        assert(!o.request_trace.issue.known); // No qualification was inferred.
    }
    for(unsigned invalid=0;invalid<8;++invalid) {
        mx5::adapter::Observation o=position(s);
        if(invalid==0)o.request_result=R::request_trace::BUSY;
        if(invalid==1)o.request_trace.request.id=0;
        if(invalid==2)o.request_trace.worker.id=0;
        if(invalid==3)o.request_trace.worker.epoch=4;
        if(invalid==4)o.request_trace.issue.observed_ns=0;
        if(invalid==5)o.request_trace.reply.observed_ns=0;
        if(invalid==6)o.request_trace.issue.observed_ns=111;
        if(invalid==7)o.request_trace.reply.observed_ns=121;
        assert(!strcmp(gate.reject(o),invalid<4?"request_unobserved":"request_time_order"));
    }
    S::Snapshot changed=s;++changed.revision;
    assert(gate.update(changed,200)==R::ModelSession::CHANGED);
    assert(!strcmp(gate.reject(position(s)),"session_changed_since_issue"));
    assert(!gate.reject(position(changed)) && gate.since_ns()==200);
    const S::Result unavailable[]={S::NONE,S::TRANSITION,S::AMBIGUOUS,S::FAULT,S::UNOBSERVED};
    for(unsigned i=0;i<sizeof unavailable/sizeof unavailable[0];++i) {
        S::Snapshot absent=S::Snapshot();absent.result=unavailable[i];
        assert(gate.update(absent,300+i)==R::ModelSession::CHANGED && !gate.available());
        assert(gate.update(absent,400+i)==R::ModelSession::SAME);
        assert(!strcmp(gate.reject(position(changed)),"session_unavailable"));
    }
    assert(gate.update(changed,500)==R::ModelSession::CHANGED && gate.available());
    assert(!gate.reject(position(changed)));
    changed.revision=0;gate.update(changed,600);assert(!gate.available());
    puts("MODEL session fence: observed revisions, unavailable states, request identity/time, GPS/GAP independence passed");
}
