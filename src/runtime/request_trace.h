#ifndef MX5_RUNTIME_REQUEST_TRACE_H
#define MX5_RUNTIME_REQUEST_TRACE_H

#include <atomic>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include "session_trace.h"
#include "bus_trace.h"

namespace mx5 { namespace runtime { namespace request_trace {

// This Ledger is independent of OEM hooks. adapter/request_hooks connects the
// verified request/notify/post/doWork/destruction boundaries in the pinned BLM.
//
// Observational association only: OK never means qualified Provenance, a
// verified provider/receiver, fresh measurement time, or permission to ASSIST.
// The caller must forward the original operation exactly once regardless of
// every result below. No callback, userdata, OEM storage or reference count is
// owned or modified here. Addresses are opaque comparison keys, never read.
//
// Initialize before producers; destroy only after they have stopped. Each API
// attempts each required table mutex at most once, performs fixed-capacity work,
// preserves errno and
// neither allocates, takes a blocking lock, sleeps, calls OEM code nor performs
// I/O. worker_post visits the two tables sequentially, never holding both
// mutexes. position_take/status use bounded atomic reads without either mutex.
// Lock-free atomic primitives are NOT a wait-free or wall-time guarantee.
// Pointer APIs must
// run inline in the verified object's live call/destruction boundary, not in
// a later address-only notification. A generation cannot prove that boundary.
// Tokens and WorkerContext belong exclusively to the Ledger instance that
// produced them, during that instance's lifetime. The intended runtime has one
// Ledger; IDs are not globally unique across instances or process restarts.

enum Result {
    OK = 0, NOT_FOUND, NOT_READY, STALE, BUSY, FULL, CONFLICT,
    WRONG_POSITION, USED, BAD_INPUT, EXHAUSTED
};
// Non-OK clears any provided output value. Results describe observation, not
// OEM success: request_end/worker_destroy can return STALE after reclaiming
// their slot if another event was lost before unlock. Do not retry the OEM
// operation, or send a later address-only cleanup, based on that result.

enum LossReason {
    LOSS_CONTENTION = 1, LOSS_CAPACITY = 2, LOSS_COLLISION = 4,
    LOSS_ID_EXHAUSTED = 8, LOSS_EPOCH_EXHAUSTED = 16
};

struct Token {
    uint64_t id, epoch; // Process-local observation IDs; zero is unknown.
};

enum IssueKnown {
    ISSUE_BUS_LIFETIME = 1, ISSUE_SESSION_LIFETIME = 2,
    ISSUE_SESSION_STATE = 4
};
struct Text {
    enum { CAPACITY = 64 };
    char bytes[CAPACITY];
    bool known, complete;
};
// Copies a borrowed NUL-terminated field while it is alive. NULL is unknown;
// a long field keeps a diagnostic prefix with complete=false. A prefix must
// not be used as sender/error/route identity. No borrowed string is retained.
Text copy_text(const char* text);

struct Route {
    Text destination, path, interface_name, member;
};
// Owned registration-time endpoint, scoped to Issue.connection. Neither the
// server GUID nor this client's unique name establishes provider qualification.
// The server GUID is transport-specific, not the bus's GetId result.
// Incomplete prefixes are diagnostic text, never an exact join key.
struct Endpoint { Text server_guid, unique_name; };
// Supplemental observations from the exact raw transport calls. Neither
// identity nor successful submission establishes provider/sensor qualification.
struct WireIssue {
    uint64_t observed_ns; // Receipt/inspection clock only; zero is unknown.
    uint32_t serial;      // Assigned request serial, scoped to its connection.
    // A known request serial must be nonzero. Unknown clears serial/time but
    // retains an observed conflict; an absent clock alone does not erase it.
    bool known, conflict;
    // Exact actual-send raw connection and issue object/lifetime were observed
    // together. Unavailable inspection is false, not a conflict. An exact key
    // also needs both nonempty, known, complete endpoint fields and no conflict.
    bool endpoint_matched;
};
struct WireReply {
    uint64_t observed_ns; // Header inspection time, never measurement time.
    uint32_t serial, reply_serial;
    int32_t type;        // Raw DBus type, distinct from the JCIDBUS type below.
    // known means the raw message header was copied, not that a remote packet
    // arrived. A local NoReply error may have serial=0 and no sender while its
    // type, reply_serial and error name remain observable. Unknown clears the
    // whole raw record, including its owned text.
    bool known;
    Text sender, error_name;
};
struct Issue {
    // bus_lifetime records the process-local successful connect epoch below.
    // Qualified request/session ownership is still unknown, not inferred from
    // a connection object, a service name or the ambient AA session.
    uint64_t observed_ns;
    uint64_t bus_lifetime, session_lifetime, session_event;
    int32_t session_state;
    unsigned known;
    // Unique live observed AA context at issue, not request ownership. Reserved
    // session_lifetime/state above stay unknown until that binding is proved.
    session_trace::Snapshot session_context;
    bus_trace::Snapshot connection;
    Endpoint endpoint;
    // Original method's routing fields, copied BEFORE async submission.
    // A well-known destination is not the provider identity or bus lifetime.
    Route route;
    WireIssue wire;
};

struct Reply {
    bus_trace::Snapshot connection;
    uint64_t observed_ns;
    int32_t type;
    uint32_t wire_serial;
    bool type_known, wire_serial_known;
    Text sender, error_name;
    WireReply wire; // Separate from the original public-getter fields above.
};

struct Trace {
    Token request, worker;
    Issue issue; // Copied at issue, never replaced by callback-time globals.
    Reply reply; // Actual callback metadata, including an observed error.
};

// The doWork hook owns this on its synchronous stack/TLS frame. It is
// deliberately not copyable or shareable between threads. Only Trace is an
// owned, pointer-free record that may be queued after the OEM call returns.
class WorkerContext {
public:
    WorkerContext();
private:
    friend class Ledger;
    WorkerContext(const WorkerContext&) = delete;
    WorkerContext& operator=(const WorkerContext&) = delete;
    Trace trace_;
    uintptr_t position_;
    bool active_;
};

struct Status {
    // Includes an already-effective pending invalidation even before the next
    // request writer commits it. No token in that next epoch exists yet.
    uint64_t loss_epoch;
    unsigned requests, workers, loss_reasons;
    bool exhausted;
};

class Ledger {
public:
    // Initial observational budget, NOT a proved OEM concurrency limit.
    enum { REQUEST_CAPACITY = 64, WORKER_CAPACITY = 64 };
    Ledger();
    ~Ledger();

    // Register the real method before calling the original async submit:
    // a reply may arrive before submit returns. No numeric position is a key.
    Result request_begin(const void* method, const Issue&, Token* out);
    // Supplement an in-flight request before reply_enter freezes the trace.
    // Equal repeated serials preserve the first observation; differing serials
    // mark a sticky wire conflict. Metadata contention never invalidates the
    // request/worker lifetime epoch. A failed submit does not end the request.
    Result wire_issue(Token request, const WireIssue&);
    Result reply_enter(const void* method, const Reply&, Token* out);

    // Only an actually observed method end/destruction permits reclamation.
    // A reply, elapsed time, disconnect intention or assumed cancellation does
    // not release a pending slot. The wrapper observes actual method free,
    // including original pending cleanup without a notify callback.
    Result request_end(const void* method);

    // Called before original PostWorker, while a matching reply scope is live.
    // It copies the reply record, so later method destruction cannot erase the
    // queued worker's metadata. expected_position comes from verified OEM ABI,
    // not pointer arithmetic guessed by this component.
    Result worker_post(const void* worker, const void* expected_position,
                       Token request, Token* out);
    // Actual doWork consumes the queued binding once and creates a live scope.
    // Destruction without doWork is the other worker-slot reclamation path.
    Result worker_enter(const void* worker, WorkerContext* out);
    Result worker_destroy(const void* worker);
    // With an active scope and non-null out, the attempt consumes the scope
    // even on BUSY, STALE or WRONG_POSITION. A null scope/out is BAD_INPUT and
    // does not consume an existing scope. Never retry a consumed scope.
    Result position_take(WorkerContext*, const void* raw_position, Trace* out);
    void worker_leave(WorkerContext*);

    // Worker-side diagnostics never acquire a table mutex or invalidate an
    // event. Counts/reasons come from one atomic word; epoch/pending are checked
    // twice. A changing snapshot is unavailable, not retried in this call.
    // BUSY/STALE return a zero Status, not a cached or partially current report.
    Result status(Status* out);

private:
    Ledger(const Ledger&) = delete;
    Ledger& operator=(const Ledger&) = delete;
    struct RequestSlot {
        uintptr_t method;
        Token token;
        Issue issue;
        Reply reply;
        bool replied;
    };
    struct WorkerSlot {
        uintptr_t worker, position;
        Trace trace;
    };
    pthread_mutex_t mutex_, worker_mutex_;
    // Request writers alone allocate IDs and advance epoch_. A pending loss
    // already invalidates old tokens. Publish the new epoch BEFORE clearing
    // pending: no new-epoch token can be issued during that handoff.
    std::atomic<unsigned> pending_loss_;
    std::atomic<uint64_t> published_epoch_;
    // Atomic counts share one word, so status cannot combine counts from
    // different instants. Other bits publish reasons/exhaustion/update state.
    enum {
        COUNT_MASK = 127, WORKER_SHIFT = 7, REASON_SHIFT = 14,
        REASON_MASK = 31, EXHAUSTED_BIT = 1 << 19, UPDATING_BIT = 1 << 20
    };
    static_assert(unsigned(REQUEST_CAPACITY) <= unsigned(COUNT_MASK) &&
                  unsigned(WORKER_CAPACITY) <= unsigned(COUNT_MASK),
                  "Request trace counts must fit their atomic snapshot fields");
    static_assert((LOSS_CONTENTION | LOSS_CAPACITY | LOSS_COLLISION |
                   LOSS_ID_EXHAUSTED | LOSS_EPOCH_EXHAUSTED) <= REASON_MASK,
                  "Request trace loss reasons must fit their snapshot field");
    std::atomic<unsigned> published_state_;
    uint64_t epoch_, next_id_;
    unsigned loss_reasons_;
    bool request_initialized_, worker_initialized_, initialized_, exhausted_;
    RequestSlot requests_[REQUEST_CAPACITY];
    WorkerSlot workers_[WORKER_CAPACITY];

    Result enter(bool cleanup = false);
    Result leave(Result);
    Result enter_worker();
    Result leave_worker(Result, uint64_t expected_epoch = 0);
    Result check_epoch(uint64_t) const;
    void lose(unsigned reason, bool exhaust = false, bool consume_pending = false);
    uint64_t allocate_id();
    RequestSlot* request_at(uintptr_t method);
    RequestSlot* request_for(Token);
    WorkerSlot* worker_at(uintptr_t worker);

    // Fault injection only: no test-control API is shipped. The tests force
    // contention and counter exhaustion without billions of fake events.
    friend struct RequestTraceTestAccess;
};

const char* result_name(Result);

} } }
#endif
