// Compile only this authored runtime TU with GCC out-of-line atomics. The
// scheduler and static libatomic live in the test executable, never the DSO.
#include "../../src/runtime/runtime.cpp"
#include <assert.h>
#include <new>
namespace mx5 { namespace runtime {
struct JournalQueueTestAccess {
    template<class T,unsigned N> static const volatile void* address(JournalQueue<T,N>& q,unsigned which) {
        if(which==0)return &q.state_;
        if(which==1)return &q.ticket_;
        if(which==2)return &q.lost_;
        return &q.slots_[0].ready;
    }
};
} }
static int32_t unused_send(void*,A::VehicleData*) { return 0; }
// Test-only early constructor, ahead of default C++ dynamic initialization.
// A non-constant global queue constructor would erase this queued event.
static void __attribute__((constructor(101))) early_observation() {
    A::Observation o=A::Observation();o.kind=A::Observation::POSITION;
    o.call_sequence=77;o.position.utc_seconds=77;o.request_trace.issue.observed_ns=77;
    o.original[47]=77;sink(&o,0);
}
extern "C" {
void jq_reset() {
    queue.~ObservationQueue();new(&queue) ObservationQueue;
    audit_fault=0;
    static bool configured=false;
    if(!configured) {
        A::Options o=A::Options();assert(A::configure(unused_send,o));configured=true;
    }
    assert(A::set_mode(A::SCRUB_STALE));
}
const volatile void* jq_address(unsigned which) {
    return mx5::runtime::JournalQueueTestAccess::address(queue,which);
}
void jq_push(unsigned sequence) {
    A::Observation o=A::Observation();o.kind=A::Observation::POSITION;
    o.call_sequence=sequence;o.position.utc_seconds=sequence;
    o.request_trace.issue.observed_ns=sequence;o.original[47]=uint8_t(sequence);
    sink(&o,0);
}
bool jq_pop(unsigned* sequence) {
    A::Observation o;
    if(!pop(&o))return false;
    assert(o.position.utc_seconds==o.call_sequence&&
           o.request_trace.issue.observed_ns==o.call_sequence&&o.original[47]==uint8_t(o.call_sequence));
    *sequence=o.call_sequence;return true;
}
void jq_close() { freeze_capture(); }
bool jq_drained() { return queue.drained(); }
bool jq_lost() { return queue.lost(); }
unsigned jq_audit() { return __sync_fetch_and_add(&audit_fault,0); }
uint64_t jq_dropped() { return queue.dropped(); }
bool jq_finish(const char* root,bool tail) {
    Journal j(root);
    if(tail && !drain_capture_tail(j))return false;
    return finish_capture(j,"12345678-1234-1234-1234-123456789abc",1,clock_ns(0));
}
}
