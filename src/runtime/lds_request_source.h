#ifndef MX5_RUNTIME_LDS_REQUEST_SOURCE_H
#define MX5_RUNTIME_LDS_REQUEST_SOURCE_H

#include "lds_sideband.h"

namespace mx5 { namespace runtime {

// Single worker owner: bounded, allocation-free and without locks/OEM calls.
// This is revocable observational association, NEVER qualified ASSIST input.
// In particular the original callback Provenance and all clocks stay unchanged.
class LdsRequestSource {
public:
    enum { CAPACITY=64 };
    static const uint64_t RETENTION_NS=5000000000ULL;
    enum Result { NOT_FOUND, WAITING, MATCHED, UNAVAILABLE, CONFLICT, PAYLOAD_MISMATCH };
    struct JoinedReply {
        uint64_t revision;
        adapter::Observation observation;
        lds_sideband::Record record;
        lds_sideband::Diagnostic diagnostic;
    private:
        friend class LdsRequestSource;
        const LdsRequestSource* owner_;
    public:
        JoinedReply():revision(0),observation(),record(),diagnostic(),owner_(0) {}
    };
    struct Status {
        uint64_t positions,records,matches,conflicts,retirements,rejected;
        uint64_t revision,floor_ns;
        unsigned entries;
        bool exhausted;
    };
    LdsRequestSource();
    // now is the worker's monotonic inspection clock, never measurement time.
    // Zero uses the last advance() clock; input clocks never advance it.
    // A first advance establishes a baseline without rejecting queued input.
    void position(const adapter::Observation&,uint64_t now=0);
    void sideband(const lds_sideband::Record&,const lds_sideband::Diagnostic&,uint64_t now=0);
    void advance(uint64_t now);
    // Clears current views and retains the negative retirement floor. It does
    // not permit old queued input to re-enter as a new unique request.
    void reset(uint64_t now=0);
    // Uses both request/worker tokens (including epochs), call and generation.
    // Unknown ambient AA session is not a barrier to exact wire association.
    // Non-MATCHED clears out. Copying the value does not grant a final lease:
    // check current() on this SAME live source before each subsequent use.
    // current checks owner/revision/identity; it is not authentication of a
    // caller-edited copy. Results are valid only within this object's lifetime.
    // It reflects the last worker advance/feed/reset; it does not read current
    // global lifecycle or wall time. The owner must synchronize those first.
    Result lookup(const adapter::Observation& identity,JoinedReply* out) const;
    bool current(const JoinedReply&) const;
    const Status& status() const { return status_; }
private:
    friend struct LdsRequestSourceTestAccess;
    struct Entry {
        bool used,has_position,has_record;
        Result state;
        uint64_t first_ns,revision;
        adapter::Observation observation;
        lds_sideband::Record record;
        lds_sideband::Diagnostic diagnostic;
    };
    Entry entries_[CAPACITY];
    Status status_;
    uint64_t now_;
    void retire(uint64_t now);
    uint64_t revision();
    Entry* allocate();
    void refresh(Entry&);
    void conflict(Entry&);
    LdsRequestSource(const LdsRequestSource&)=delete;
    LdsRequestSource& operator=(const LdsRequestSource&)=delete;
};

} }
#endif
