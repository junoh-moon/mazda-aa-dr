#ifndef MX5_RUNTIME_REQUEST_OBSERVER_H
#define MX5_RUNTIME_REQUEST_OBSERVER_H

#include "request_trace.h"

namespace mx5 { namespace runtime { namespace request_trace {

// Exact NA 74.00.324A exported JCIDBUS getter contracts. Opaque method/reply
// objects are passed back to their original APIs, never dereferenced here.
// Resolve and verify the original providers before producers start.
struct ReplyApi {
    void* (*get_reply)(void* method);
    int (*get_type)(void* reply); // API enum: 1 return, 2 error; NOT raw type 3/4.
    const char* (*get_sender)(void* reply);
    const char* (*get_error)(void* reply);
    int (*get_reply_serial)(void* reply, uint32_t* serial); // 0 success.
};
struct MethodApi {
    const char* (*get_destination)(void* method);
    const char* (*get_path)(void* method);
    const char* (*get_interface)(void* method);
    const char* (*get_name)(void* method);
};
typedef uint64_t (*ObservationClock)(void*);

class ReplyScope;
class WorkerScope;

// Glue for verified live call boundaries. It does not install hooks, replace
// callbacks/userdata, call the original operation, or establish qualification.
// Production wrappers/installation live in adapter/request_hooks and v74_install.
// TODO: provider identity, request/session ownership and receiver qualification.
// The caller must forward each original call exactly once even on failure.
// As with Ledger, initialize before producers and outlive every live scope.
class Observer {
public:
    // Standalone observers may omit route getters; production installation
    // requires all four verified functions. Omitted fields stay unknown.
    Observer(const ReplyApi&, ObservationClock = 0, void* clock_user = 0,
             const MethodApi& = MethodApi());
    bool valid() const;
    Result request_begin(void* method, Token*, const session_trace::Snapshot& = session_trace::Snapshot(),
                         const bus_trace::Snapshot& = bus_trace::Snapshot());
    Result wire_issue(Token request, const WireIssue&);
    Result request_end(void* method);
    // Exact worker and position supplied by the verified BLM ABI, before post.
    Result worker_post(void* worker, const void* position, Token*);
    Result worker_destroy(void* worker);
    Result position_take(const void* position, Trace*);
    Result status(Status*);
private:
    friend class ReplyScope;
    friend class WorkerScope;
    Observer(const Observer&) = delete;
    Observer& operator=(const Observer&) = delete;
    ReplyApi api_;
    MethodApi method_api_;
    ObservationClock clock_;
    void* clock_user_;
    Ledger ledger_;
    uint64_t now() const;
    Reply read_reply(void* method) const;
};

// Stack-only scopes surround the entire original synchronous notify/doWork
// call. They mask outer scopes even when a nested observation is missing or
// belongs to another Observer. Neither scope owns any OEM object or string.
// The caller must keep the method alive throughout getter reads/reply_enter.
// The pinned OEM pending handler retains it through notify; its verified reply
// getters only read fields and do not reenter callbacks. A method freed/reused
// inside those reads violates this lifetime boundary and is not address-safe.
// Destruction must be LIFO on the creating thread; never heap-queue a scope.
class ReplyScope {
public:
    ReplyScope(Observer&, void* method, const bus_trace::Snapshot& = bus_trace::Snapshot(),
               const WireReply& = WireReply());
    ~ReplyScope();
    Result result() const { return result_; }
    Token token() const { return token_; }
private:
    friend class Observer;
    ReplyScope(const ReplyScope&) = delete;
    ReplyScope& operator=(const ReplyScope&) = delete;
    Observer* owner_;
    ReplyScope* previous_;
    Token token_;
    Result result_;
};

class WorkerScope {
public:
    WorkerScope(Observer&, void* worker);
    ~WorkerScope();
    Result result() const { return result_; }
private:
    friend class Observer;
    WorkerScope(const WorkerScope&) = delete;
    WorkerScope& operator=(const WorkerScope&) = delete;
    Observer* owner_;
    WorkerScope* previous_;
    WorkerContext context_;
    Result result_;
};

} } }
#endif
