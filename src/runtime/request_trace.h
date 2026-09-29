#ifndef MX5_RUNTIME_REQUEST_TRACE_H
#define MX5_RUNTIME_REQUEST_TRACE_H

#include <atomic>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

namespace mx5 { namespace runtime { namespace request_trace {

// TODO: connect only after the exact OEM request, callback, post, doWork and
// destruction ABIs are verified. This component currently has NO OEM hooks.
//
// Observational association only: OK never means qualified Provenance, a
// verified provider/receiver, fresh measurement time, or permission to ASSIST.
// The caller must forward the original operation exactly once regardless of
// every result below. No callback, userdata, OEM storage or reference count is
// owned or modified here. Addresses are opaque comparison keys, never read.
//
// Initialize before producers; destroy only after they have stopped. Each API
// attempts the mutex at most once, performs fixed-capacity work, preserves errno and
// neither allocates, takes a blocking lock, sleeps, calls OEM code nor performs
// I/O. Lock-free atomic primitives are NOT a wait-free or wall-time guarantee.
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
struct Issue {
    // Optional facts supplied by a future hook. Leave known clear until the
    // corresponding original lifetime/status boundary has actually been seen.
    uint64_t observed_ns;
    uint64_t bus_lifetime, session_lifetime, session_event;
    int32_t session_state;
    unsigned known;
};

struct Text {
    enum { CAPACITY = 64 };
    char bytes[CAPACITY];
    bool known, complete;
};
// Copies a borrowed NUL-terminated field while it is alive. NULL is unknown;
// a long field keeps a diagnostic prefix with complete=false. A prefix must
// not be used as sender/error identity. No borrowed string is retained.
Text copy_text(const char* text);

struct Reply {
    uint64_t observed_ns;
    int32_t type;
    uint32_t wire_serial;
    bool type_known, wire_serial_known;
    Text sender, error_name;
};

struct Trace {
    Token request, worker;
    Issue issue; // Copied at issue, never replaced by callback-time globals.
    Reply reply; // Actual callback metadata, including an observed error.
};

// The future doWork hook owns this on its synchronous stack/TLS frame. It is
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
    Result reply_enter(const void* method, const Reply&, Token* out);

    // Only an actually observed method end/destruction permits reclamation.
    // A reply, elapsed time, disconnect intention or assumed cancellation does
    // not release a pending slot. Cancel ABI integration remains unimplemented.
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

    // Worker-side diagnostics. Failure to read status does not lose a lifetime
    // event; unlike the event APIs, a busy status read does not invalidate.
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
    pthread_mutex_t mutex_;
    // Only this lock-free 32-bit value is touched without mutex_. A missed
    // event makes all old tokens unusable at the next API entry. The holder
    // also checks it before returning OK. Epoch and ID arithmetic stays under
    // the mutex; no 64-bit atomic or source-level CAS retry loop is required.
    std::atomic<unsigned> pending_loss_;
    uint64_t epoch_, next_id_;
    unsigned loss_reasons_;
    bool initialized_, exhausted_;
    RequestSlot requests_[REQUEST_CAPACITY];
    WorkerSlot workers_[WORKER_CAPACITY];

    Result enter(bool cleanup = false, bool mark_busy_loss = true);
    Result leave(Result);
    void lose(unsigned reason);
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
