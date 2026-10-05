#include "runtime/model_session.h"
#include "runtime/model_bus.h"
#include <cassert>
#include <cstdio>
#include <cstring>
namespace R=mx5::runtime;
namespace S=R::session_trace;
namespace B=R::bus_trace;
static mx5::adapter::Observation position(S::Snapshot session) {
    mx5::adapter::Observation o=mx5::adapter::Observation();
    o.kind=mx5::adapter::Observation::POSITION;o.mono_ns=120;
    o.request_trace.request.id=1;o.request_trace.request.epoch=3;
    o.request_trace.worker.id=2;o.request_trace.worker.epoch=3;
    o.request_trace.issue.observed_ns=100;o.request_trace.reply.observed_ns=110;
    o.request_trace.issue.session_context=session;return o;
}
static void bus_gate() {
    R::ModelBus gate;const B::Boundary b={{B::CONNECTED,2,7},20};
    assert(!gate.available() && gate.update(b,90)==R::ModelBus::INITIAL && gate.available());
    mx5::adapter::Observation o=mx5::adapter::Observation();
    o.request_trace.issue.connection=o.request_trace.reply.connection=b.connection;
    o.request_trace.issue.known=R::request_trace::ISSUE_BUS_LIFETIME;
    o.request_trace.issue.bus_lifetime=7;o.request_trace.issue.observed_ns=100;
    for(unsigned mode=0;mode<4;++mode) {
        o.position.mode=mode;assert(!gate.reject(o));
        assert(gate.update(b,200)==R::ModelBus::SAME && gate.epoch()==1 && gate.since_ns()==90);
    }
    for(unsigned invalid=0;invalid<7;++invalid) {
        mx5::adapter::Observation bad=o;
        if(invalid==0)bad.request_result=R::request_trace::BUSY;
        if(invalid==1)bad.request_trace.issue.known=0;
        if(invalid==2)bad.request_trace.issue.bus_lifetime=8;
        if(invalid==3)bad.request_trace.issue.connection.object=3;
        if(invalid==4)bad.request_trace.reply.connection.lifetime=8;
        if(invalid==5)bad.request_trace.reply.connection.result=B::DISCONNECTED;
        if(invalid==6)bad.request_trace.issue.observed_ns=89;
        assert(!strcmp(gate.reject(bad),invalid<3?"request_bus_unobserved":
                       invalid<6?"bus_changed_since_issue":"request_before_bus_boundary"));
    }
    B::Boundary changed=b;++changed.revision;
    assert(gate.update(changed,150)==R::ModelBus::CHANGED && gate.epoch()==2);
    assert(!strcmp(gate.reject(o),"request_before_bus_boundary"));
    o.request_trace.issue.observed_ns=150;assert(!gate.reject(o));
    const B::Result absent[]={B::UNOBSERVED,B::NONE,B::AMBIGUOUS,B::TRANSITION,B::FAULT};
    for(unsigned i=0;i<sizeof absent/sizeof absent[0];++i) {
        B::Boundary lost=B::Boundary();lost.connection.result=absent[i];
        assert(gate.update(lost,200+i)==R::ModelBus::CHANGED && !gate.available());
        assert(!strcmp(gate.reject(o),"bus_unavailable"));
    }
    puts("MODEL bus fence: issue/reply lifetime, boundary time, ambiguity, faults and GPS/GAP independence passed");
}
int main() {
    bus_gate();
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
    {
        // BETA declined fence (design decision 6): only the exact never-prepared
        // UNOBSERVED snapshot is admitted, and only after an explicit opt-in.
        R::ModelSession declined;declined.accept_declined(true);
        const S::Snapshot unobserved=S::Snapshot();
        assert(!declined.available());
        assert(declined.update(unobserved,700)==R::ModelSession::INITIAL && declined.available());
        assert(!declined.reject(position(unobserved)));
        assert(!strcmp(declined.reject(position(s)),"session_changed_since_issue"));
        for(unsigned i=0;i<4;++i) {
            S::Snapshot absent=S::Snapshot();absent.result=unavailable[i];
            assert(declined.update(absent,710+i)==R::ModelSession::CHANGED && !declined.available());
            assert(!strcmp(declined.reject(position(unobserved)),"session_unavailable"));
        }
        S::Snapshot odd=S::Snapshot();odd.revision=3; // UNOBSERVED never carries a revision
        declined.update(odd,720);assert(!declined.available());
        R::ModelSession strict; // without the opt-in nothing changes
        assert(strict.update(unobserved,730)==R::ModelSession::INITIAL && !strict.available());
    }
    puts("MODEL session fence: observed revisions, unavailable states, request identity/time, GPS/GAP independence, BETA declined opt-in passed");
}
