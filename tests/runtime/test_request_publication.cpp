// The real request_trace.cpp is a separate GCC TU with out-of-line atomics.
// Only this test executable links libatomic/wraps the pending exchange. The
// production object keeps its normal inline atomics and has no test hook.
#include "runtime/request_trace.h"
#include <assert.h>
#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <unistd.h>
#ifdef NDEBUG
#error Request publication regression checks require assertions
#endif
namespace R = mx5::runtime::request_trace;
namespace mx5 { namespace runtime { namespace request_trace {
struct RequestTraceTestAccess {
    static pthread_mutex_t* worker(Ledger& l) { return &l.worker_mutex_; }
    static const volatile void* pending(Ledger& l) { return &l.pending_loss_; }
};
} } }
static std::atomic<unsigned> paused(0), resume(0);
static const volatile void* watched;
static unsigned pause_phase;
static void checkpoint(unsigned phase, volatile void* address) {
    if (address != watched || phase != pause_phase) return;
    paused.store(1);
    while (!resume.load()) sched_yield();
}
extern "C" unsigned __real___atomic_exchange_4(volatile void*, unsigned, int);
extern "C" unsigned __wrap___atomic_exchange_4(volatile void* address, unsigned value, int order) {
    checkpoint(1, address);
    const unsigned result = __real___atomic_exchange_4(address, value, order);
    checkpoint(2, address);
    return result;
}
struct Job { R::Ledger* ledger; int* method; R::Result result; };
static void* writer(void* input) {
    Job& job = *static_cast<Job*>(input);
    errno = EDOM;
    job.result = job.ledger->request_end(job.method);
    assert(errno == EDOM);
    return 0;
}
static void miss_worker_event(R::Ledger& ledger, int* identity) {
    pthread_mutex_t* lock = R::RequestTraceTestAccess::worker(ledger);
    assert(!pthread_mutex_lock(lock));
    assert(ledger.worker_destroy(identity) == R::BUSY);
    assert(!pthread_mutex_unlock(lock));
}
static void run(unsigned phase, bool extra_loss) {
    R::Ledger ledger; int method = 0, worker = 0, position = 0, unknown = 0;
    R::Token token;
    assert(ledger.request_begin(&method, R::Issue(), &token) == R::OK);
    assert(ledger.reply_enter(&method, R::Reply(), &token) == R::OK);
    assert(ledger.worker_post(&worker, &position, token, &token) == R::OK);
    R::WorkerContext scope;
    assert(ledger.worker_enter(&worker, &scope) == R::OK);
    miss_worker_event(ledger, &unknown);
    R::Status before;
    assert(ledger.status(&before) == R::OK && before.loss_epoch == 2);
    watched = R::RequestTraceTestAccess::pending(ledger);
    pause_phase = phase; paused.store(0); resume.store(0);
    Job job = {&ledger, &method, R::NOT_FOUND}; pthread_t thread;
    assert(!pthread_create(&thread, 0, writer, &job));
    while (!paused.load()) sched_yield();
    if (extra_loss) miss_worker_event(ledger, &unknown);
    R::Status during;
    errno = EDOM;
    const R::Result read = ledger.status(&during);
    R::Trace trace;
    const R::Result take = ledger.position_take(&scope, &position, &trace);
    assert(errno == EDOM);
    printf("actual pending exchange phase=%s extra_loss=%u status=%s epoch=%llu take=%s\n",
           phase == 1 ? "before" : "after", unsigned(extra_loss), R::result_name(read),
           (unsigned long long)during.loss_epoch, R::result_name(take));
    resume.store(1); assert(!pthread_join(thread, 0)); watched = 0;
    assert(read == R::BUSY && !during.loss_epoch && !during.requests && !during.workers &&
           !during.loss_reasons && !during.exhausted);
    assert(take == R::STALE && !trace.request.id && !trace.worker.id);
    assert(ledger.position_take(&scope, &position, &trace) == R::USED);
    const bool next_loss = phase == 2 && extra_loss;
    assert(job.result == (next_loss ? R::STALE : R::OK)); // Actual cleanup happened in either case.
    R::Status done;
    assert(ledger.status(&done) == R::OK && !done.requests && !done.workers);
    assert(done.loss_epoch == (next_loss ? 3u : 2u) && done.loss_reasons == R::LOSS_CONTENTION);
    assert(ledger.request_begin(&method, R::Issue(), &token) == R::OK);
    assert(token.epoch == done.loss_epoch); // Committing pending must not regress or double-count it.
    assert(ledger.request_end(&method) == R::OK);
}
int main() {
    alarm(30);
    for (unsigned phase = 1; phase != 3; ++phase) {
        run(phase, false); run(phase, true);
    }
    alarm(0);
    puts("PASS actual epoch publication, pending clear and concurrent losses on both sides of clear");
}
