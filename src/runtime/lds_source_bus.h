#ifndef MX5_RUNTIME_LDS_SOURCE_BUS_H
#define MX5_RUNTIME_LDS_SOURCE_BUS_H

#include "bus_trace.h"

namespace mx5 { namespace runtime {

// Worker-owned negative fence for historical exact request association only.
// Before a source has been observed, create/connect and the first POSITION
// source marker can change revision without ending that request's lifetime.
// This policy does not prove hidden lifecycles before its first observation,
// current global identity, physical freshness or qualified ASSIST provenance.
class LdsSourceBus {
public:
    LdsSourceBus():current_(),baseline_(false) {}
    // True means the caller must retire the resolver at its actual inspection
    // clock. No constructor/update time is invented here. Once a stable source
    // or negative observation is seen, every changed boundary remains a fence.
    bool update(const bus_trace::Boundary& next) {
        if(!baseline_) {
            if(next.connection.result==bus_trace::UNOBSERVED ||
               next.connection.result==bus_trace::TRANSITION)return false;
            current_=next;baseline_=true;
            return !(next.connection.result==bus_trace::CONNECTED &&
                     next.revision && next.connection.object && next.connection.lifetime);
        }
        if(same(current_,next))return false;
        current_=next;return true;
    }
private:
    bus_trace::Boundary current_;
    bool baseline_;
    static bool same(const bus_trace::Boundary& a,const bus_trace::Boundary& b) {
        return a.revision==b.revision && a.connection.result==b.connection.result &&
            a.connection.object==b.connection.object && a.connection.lifetime==b.connection.lifetime;
    }
};

} }
#endif
