// Compile the real authored Ledger with only its unlock calls redirected to a
// test scheduler. No test-control API, OEM code or scheduler is shipped.
#include "runtime/request_trace.h"
#include <assert.h>
#include <errno.h>
#include <limits>
#include <sched.h>
#include <stdio.h>
#include <unistd.h>
static int scheduled_unlock(pthread_mutex_t*);
#define pthread_mutex_unlock scheduled_unlock
#include "../../src/runtime/request_trace.cpp"
#undef pthread_mutex_unlock

#ifdef NDEBUG
#error Request handoff regression checks require assertions
#endif

namespace R = mx5::runtime::request_trace;
namespace mx5 { namespace runtime { namespace request_trace {
struct RequestTraceTestAccess {
    static pthread_mutex_t* request(Ledger& l) { return &l.mutex_; }
    static pthread_mutex_t* worker(Ledger& l) { return &l.worker_mutex_; }
    static void transition(Ledger& l, uint64_t epoch, unsigned pending, bool updating) {
        // Atomic state injection for reader boundary tests, not an execution
        // claim about the ordering of the real lose() writer.
        l.epoch_ = epoch; l.published_epoch_.store(epoch);
        l.pending_loss_.store(pending);
        if (updating) l.published_state_.fetch_or(Ledger::UPDATING_BIT);
        else l.published_state_.fetch_and(~unsigned(Ledger::UPDATING_BIT));
    }
};
} } }

static thread_local bool scheduled_thread;
static std::atomic<pthread_mutex_t*> pause_mutex(0);
static std::atomic<unsigned> pause_phase(0), paused(0), resume(0);
static void pause_at(unsigned phase, pthread_mutex_t* mutex) {
    if (!scheduled_thread || pause_mutex.load() != mutex || pause_phase.load() != phase) return;
    paused.store(1);
    while (!resume.load()) sched_yield();
}
static int scheduled_unlock(pthread_mutex_t* mutex) {
    pause_at(1, mutex);
    const int result = pthread_mutex_unlock(mutex);
    pause_at(2, mutex);
    return result;
}
struct Job {
    R::Ledger* ledger;
    int* method;
    int* worker;
    int* position;
    R::Token request, posted;
    R::Trace trace;
    unsigned action;
    R::Result result;
};
static void* execute(void* input) {
    Job& j = *static_cast<Job*>(input);
    scheduled_thread = true;
    errno = EDOM;
    if (j.action == 0) j.result = j.ledger->request_end(j.method);
    else if (j.action == 1) {
        R::WorkerContext scope;
        j.result = j.ledger->worker_enter(j.worker, &scope);
        if (j.result == R::OK) j.result = j.ledger->position_take(&scope, j.position, &j.trace);
        j.ledger->worker_leave(&scope);
    } else j.result = j.ledger->worker_post(j.worker, j.position, j.request, &j.posted);
    assert(errno == EDOM);
    return 0;
}
static pthread_t start(Job& job, pthread_mutex_t* mutex, unsigned phase) {
    pause_mutex.store(mutex); pause_phase.store(phase); paused.store(0); resume.store(0);
    pthread_t thread;
    assert(!pthread_create(&thread, 0, execute, &job));
    while (!paused.load()) sched_yield();
    return thread;
}
static void finish(pthread_t thread) {
    resume.store(1); assert(!pthread_join(thread, 0)); pause_phase.store(0);
}
static R::Token ready(R::Ledger& ledger, int* method) {
    R::Token token;
    R::Issue issue = R::Issue(); issue.observed_ns = 1234567;
    R::Reply reply = R::Reply(); reply.sender = R::copy_text(":1.55");
    assert(ledger.request_begin(method, issue, &token) == R::OK);
    assert(ledger.reply_enter(method, reply, &token) == R::OK);
    return token;
}
static R::Status status(R::Ledger& ledger) {
    R::Status s;
    errno = EDOM;
    assert(ledger.status(&s) == R::OK && errno == EDOM);
    return s;
}
static void check_trace(const R::Trace& t, R::Token request) {
    assert(t.request.id == request.id && t.request.epoch == request.epoch);
    assert(t.issue.observed_ns == 1234567 && !strcmp(t.reply.sender.bytes, ":1.55"));
}
static void independent_tables() {
    for (unsigned action = 0; action != 2; ++action) {
        R::Ledger ledger; int method = 0, worker = 0, position = 0;
        Job job = Job(); job.ledger = &ledger; job.method = &method;
        job.worker = &worker; job.position = &position; job.action = action;
        job.request = ready(ledger, &method);
        assert(ledger.worker_post(&worker, &position, job.request, &job.posted) == R::OK);
        pthread_mutex_t* held = action ? R::RequestTraceTestAccess::worker(ledger)
                                      : R::RequestTraceTestAccess::request(ledger);
        pthread_t thread = start(job, held, 1); // Paused with the real table lock held.
        const R::Status during = status(ledger);
        assert(during.requests == action && during.workers == !action);
        assert(during.loss_epoch == 1 && !during.loss_reasons);
        if (action) assert(ledger.request_end(&method) == R::OK);
        else {
            R::WorkerContext scope; R::Trace trace;
            assert(ledger.worker_enter(&worker, &scope) == R::OK);
            assert(ledger.position_take(&scope, &position, &trace) == R::OK);
            check_trace(trace, job.request);
        }
        finish(thread);
        assert(job.result == R::OK);
        if (action) check_trace(job.trace, job.request);
        const R::Status done = status(ledger);
        assert(!done.requests && !done.workers && !done.loss_reasons && done.loss_epoch == 1);
    }
    puts("PASS paused real request/worker critical sections cannot obstruct the other table or status");
}
static void post_gap(bool pending_only) {
    R::Ledger ledger; int method = 0, worker = 0, position = 0, unknown = 0;
    Job job = Job(); job.ledger = &ledger; job.method = &method;
    job.worker = &worker; job.position = &position; job.action = 2;
    job.request = ready(ledger, &method);
    const pthread_t thread = start(job, R::RequestTraceTestAccess::request(ledger), 2);
    // The actual worker_post has copied metadata and released request ownership.
    if (pending_only) {
        pthread_mutex_t* lock = R::RequestTraceTestAccess::worker(ledger);
        assert(!pthread_mutex_lock(lock));
        assert(ledger.worker_destroy(&unknown) == R::BUSY);
        assert(!pthread_mutex_unlock(lock));
    } else {
        R::Token rejected;
        assert(ledger.request_begin(&method, R::Issue(), &rejected) == R::CONFLICT);
        assert(!rejected.id);
    }
    const R::Status before = status(ledger);
    assert(before.loss_epoch == 2 && before.loss_reasons && !before.workers);
    finish(thread);
    assert(job.result == R::STALE && !job.posted.id && !job.posted.epoch);
    assert(!status(ledger).workers);
    assert(ledger.request_end(&method) == R::OK);
    assert(status(ledger).loss_epoch == before.loss_epoch);
    const R::Token fresh = ready(ledger, &method);
    R::Token posted;
    assert(ledger.worker_post(&worker, &position, fresh, &posted) == R::OK);
    R::WorkerContext scope; R::Trace trace;
    assert(ledger.worker_enter(&worker, &scope) == R::OK);
    assert(ledger.position_take(&scope, &position, &trace) == R::OK);
    check_trace(trace, fresh);
    assert(ledger.request_end(&method) == R::OK);
    puts(pending_only ? "PASS pending loss during real two-stage worker post rejects the copy, then recovers"
                      : "PASS committed loss during real two-stage worker post rejects the copy, then recovers");
}
static void publication_readers() {
    R::Ledger ledger; int method = 0, worker = 0, position = 0;
    const R::Token request = ready(ledger, &method); R::Token work;
    assert(ledger.worker_post(&worker, &position, request, &work) == R::OK);
    R::WorkerContext scope; assert(ledger.worker_enter(&worker, &scope) == R::OK);
    R::RequestTraceTestAccess::transition(ledger, 2, R::LOSS_CONTENTION, true);
    R::Status unavailable = {99, 1, 1, 1, true};
    assert(ledger.status(&unavailable) == R::BUSY);
    assert(!unavailable.loss_epoch && !unavailable.requests && !unavailable.workers &&
           !unavailable.loss_reasons && !unavailable.exhausted);
    R::Trace trace;
    assert(ledger.position_take(&scope, &position, &trace) == R::STALE);
    assert(!trace.request.id && !trace.worker.id);
    // A new loss AFTER the previous publication/clear is a distinct pending
    // invalidation. Status is already meaningful without taking either lock.
    R::RequestTraceTestAccess::transition(ledger, 2, R::LOSS_CONTENTION, false);
    assert(status(ledger).loss_epoch == 3);
    assert(ledger.request_end(&method) == R::OK);
    assert(status(ledger).loss_epoch == 3 && status(ledger).loss_reasons == R::LOSS_CONTENTION);
    const R::Token fresh = ready(ledger, &method);
    assert(fresh.epoch == 3);
    assert(ledger.request_end(&method) == R::OK);

    R::Ledger terminal;
    const uint64_t maximum = std::numeric_limits<uint64_t>::max();
    R::RequestTraceTestAccess::transition(terminal, maximum, 0, false);
    const R::Token last = ready(terminal, &method);
    assert(terminal.worker_post(&worker, &position, last, &work) == R::OK);
    assert(terminal.worker_enter(&worker, &scope) == R::OK);
    R::RequestTraceTestAccess::transition(terminal, maximum, R::LOSS_CONTENTION, false);
    const R::Status pending = status(terminal);
    assert(pending.loss_epoch == maximum && pending.exhausted &&
           pending.loss_reasons == (R::LOSS_CONTENTION | R::LOSS_EPOCH_EXHAUSTED));
    assert(terminal.position_take(&scope, &position, &trace) == R::STALE);
    assert(!trace.request.id && !trace.worker.id);
    assert(terminal.request_end(&method) == R::OK);
    const R::Status committed = status(terminal);
    assert(committed.loss_epoch == pending.loss_epoch && committed.exhausted &&
           committed.loss_reasons == pending.loss_reasons && !committed.requests);
    assert(terminal.request_begin(&method, R::Issue(), &work) == R::EXHAUSTED && !work.id);
    puts("PASS atomic publication reader boundaries, pending epoch stability and terminal cleanup");
}
static void owned_scope_without_locks() {
    R::Ledger ledger; int method = 0, worker = 0, position = 0;
    const R::Token request = ready(ledger, &method); R::Token posted;
    assert(ledger.worker_post(&worker, &position, request, &posted) == R::OK);
    R::WorkerContext scope; R::Trace trace;
    assert(ledger.worker_enter(&worker, &scope) == R::OK);
    pthread_mutex_t* a = R::RequestTraceTestAccess::request(ledger);
    pthread_mutex_t* b = R::RequestTraceTestAccess::worker(ledger);
    assert(!pthread_mutex_lock(a) && !pthread_mutex_lock(b));
    errno = EDOM;
    assert(ledger.position_take(&scope, &position, &trace) == R::OK && errno == EDOM);
    check_trace(trace, request);
    const R::Status s = status(ledger);
    assert(s.loss_epoch == 1 && !s.loss_reasons && s.requests == 1 && !s.workers);
    assert(!pthread_mutex_unlock(b) && !pthread_mutex_unlock(a));
    assert(ledger.position_take(&scope, &position, &trace) == R::USED && !trace.request.id);
    assert(ledger.request_end(&method) == R::OK);
    puts("PASS owned position scope and status need neither table mutex");
}
static void post_copy_survives_reuse() {
    R::Ledger ledger; int method = 0, worker = 0, position = 0;
    Job job = Job(); job.ledger = &ledger; job.method = &method;
    job.worker = &worker; job.position = &position; job.action = 2;
    job.request = ready(ledger, &method);
    const pthread_t thread = start(job, R::RequestTraceTestAccess::request(ledger), 2);
    assert(ledger.request_end(&method) == R::OK);
    R::Issue fresh_issue = R::Issue(); fresh_issue.observed_ns = 9876543;
    R::Reply fresh_reply = R::Reply(); fresh_reply.sender = R::copy_text(":1.99");
    R::Token fresh;
    assert(ledger.request_begin(&method, fresh_issue, &fresh) == R::OK);
    assert(ledger.reply_enter(&method, fresh_reply, &fresh) == R::OK);
    assert(fresh.id != job.request.id && fresh.epoch == job.request.epoch);
    finish(thread);
    assert(job.result == R::OK && job.posted.id);
    R::WorkerContext scope; R::Trace trace;
    assert(ledger.worker_enter(&worker, &scope) == R::OK);
    assert(ledger.position_take(&scope, &position, &trace) == R::OK);
    check_trace(trace, job.request); // The replacement request cannot rewrite the queued copy.
    const R::Status s = status(ledger);
    assert(s.requests == 1 && !s.workers && !s.loss_reasons && s.loss_epoch == 1);
    assert(ledger.request_end(&method) == R::OK);
    puts("PASS actual two-stage post retains its owned reply across method end and address reuse");
}
int main() {
    alarm(30); // A blocking-lock mutant must fail instead of hanging the suite.
    independent_tables(); post_gap(false); post_gap(true); publication_readers();
    owned_scope_without_locks();
    post_copy_survives_reuse();
    alarm(0);
    return 0;
}
