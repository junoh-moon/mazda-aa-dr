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
typedef uint64_t (*ObservationClock)(void*);

class ReplyScope;
class WorkerScope;

// Glue for verified live call boundaries. It does not install hooks, replace
// callbacks/userdata, call the original operation, or establish qualification.
// Production wrappers/installation live in adapter/request_hooks and v74_install.
// TODO: verified bus/session/receiver lifetimes and ASSIST qualification.
// The caller must forward each original call exactly once even on failure.
// As with Ledger, initialize before producers and outlive every live scope.
class Observer {
public:
    Observer(const ReplyApi&, ObservationClock = 0, void* clock_user = 0);
    bool valid() const;
    Result request_begin(void* method, Token*);
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
    ObservationClock clock_;
    void* clock_user_;
    Ledger ledger_;
    uint64_t now() const;
    Reply read_reply(void* method) const;
};

// Stack-only scopes surround the entire original synchronous notify/doWork
// call. They mask outer scopes even when a nested observation is missing or
// belongs to another Observer. Neither scope owns any OEM object or string.
// Destruction must be LIFO on the creating thread; never heap-queue a scope.
class ReplyScope {
public:
    ReplyScope(Observer&, void* method);
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
