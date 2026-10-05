#ifndef MX5_RUNTIME_MOTION_GAP_H
#define MX5_RUNTIME_MOTION_GAP_H
// Reverse latch stickiness across short input gaps (coordinator decision
// "E", 2026-10-05). An input rejection bumps the MODEL source epoch and resets
// the estimators. If the rejection is only a short gap of the same producer
// (stale delivery or a forward sequence discontinuity, at most KEEP_NS of data
// and KEEP_EVENTS missing events, and the rejected event is not a REVERSE
// message), the MODEL reverse latch is kept: a missed reverse change in such a
// gap is unlikely, a wrongly latched forward during a reversal is bounded by
// the low reversing speed, and a wrongly latched reverse is caught by the BETA
// withdrawal (latched reverse + wheels > 15 km/h for > 2 s). Larger gaps,
// replays/rewinds, source changes and every other rejection clear it.
// Worker-thread state only (runtime worker and the replay harness).
#include "navigation/channel.h"
#include <stdint.h>

namespace mx5 { namespace runtime {

struct MotionGapTracker {
    static const uint64_t KEEP_NS=2000000000ULL;
    static const uint64_t KEEP_EVENTS=16;
    enum Close { NO_GAP=0, KEPT, TOO_LARGE };
    bool have_last, open;
    uint64_t epoch, last_seq, last_ns, start_ns;
    navigation::ReceiveFault reason;
    MotionGapTracker():have_last(false),open(false),epoch(0),last_seq(0),last_ns(0),start_ns(0),
                       reason(navigation::RECEIVE_OK) {}
    // A rejected event: true when the reset it causes may keep the latch.
    // missing counts the events lost since the last accepted one (inclusive
    // of this one); span is the producer receipt time covered by the gap.
    bool reject(bool decoded,navigation::ReceiveFault why,const navigation::RawEvent& e,
                uint64_t* missing,uint64_t* span) {
        *missing=0;*span=0;
        if(!decoded || (why!=navigation::RECEIVE_STALE && why!=navigation::RECEIVE_SEQUENCE) ||
           !have_last || e.epoch!=epoch || e.receive_seq<=last_seq || e.kind==navigation::REVERSE) {
            open=false;have_last=false;   // the next accepted event starts afresh
            return false;
        }
        if(!open) { open=true;start_ns=last_ns; }
        reason=why;
        *missing=e.receive_seq-last_seq;
        *span=e.received_ns>start_ns?e.received_ns-start_ns:0;
        return *missing<=KEEP_EVENTS && *span<=KEEP_NS;
    }
    // An accepted event closes an open gap; TOO_LARGE means the gap grew
    // beyond the keep limit after all and the latch must be cleared now.
    Close accept(const navigation::RawEvent& e,uint64_t* missing,uint64_t* span) {
        Close result=NO_GAP;*missing=0;*span=0;
        if(open) {
            *missing=e.receive_seq>last_seq?e.receive_seq-last_seq-1:UINT64_MAX;
            *span=e.received_ns>start_ns?e.received_ns-start_ns:0;
            result=e.epoch!=epoch || *missing>KEEP_EVENTS || *span>KEEP_NS?TOO_LARGE:KEPT;
        }
        open=false;have_last=true;epoch=e.epoch;last_seq=e.receive_seq;last_ns=e.received_ns;
        return result;
    }
};

} }
#endif
