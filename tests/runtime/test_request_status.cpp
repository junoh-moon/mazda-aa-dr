// Authored identities only. Concurrent diagnostics must not lose lifecycle events.
#include "runtime/request_trace.h"
#include <assert.h>
#include <errno.h>
#include <sched.h>
#include <stdio.h>

#ifdef NDEBUG
#error Request status regression requires assertions
#endif

namespace rt = mx5::runtime::request_trace;
static rt::Issue issue(uint64_t sequence) {
    rt::Issue value=rt::Issue();
    value.session_event=sequence;
    value.known=rt::ISSUE_SESSION_STATE;
    return value;
}
static rt::Reply reply() {
    rt::Reply value=rt::Reply();
    value.type=1;
    value.type_known=true;
    return value;
}
static rt::Status status(rt::Ledger& ledger) {
    rt::Status value;
    assert(ledger.status(&value)==rt::OK);
    return value;
}

struct HealthPoll {
    rt::Ledger* ledger;
    std::atomic<unsigned> ready, reads, bad;
    std::atomic<bool> stop;
    explicit HealthPoll(rt::Ledger& value)
        : ledger(&value), ready(0), reads(0), bad(0), stop(false) {}
};
static void* poll_health(void* input) {
    HealthPoll& job = *static_cast<HealthPoll*>(input);
    job.ready.fetch_add(1);
    do {
        rt::Status value;
        errno = EDOM;
        const rt::Result result = job.ledger->status(&value);
        if (errno != EDOM || (result != rt::OK && result != rt::BUSY && result != rt::STALE))
            job.bad.fetch_add(1);
        if (result == rt::OK) {
            if (value.loss_epoch != 1 || value.loss_reasons || value.exhausted ||
                value.requests > 1 || value.workers > 1) job.bad.fetch_add(1);
        } else if (value.loss_epoch || value.requests || value.workers ||
                   value.loss_reasons || value.exhausted) job.bad.fetch_add(1);
        job.reads.fetch_add(1);
    } while (!job.stop.load());
    return 0;
}
static void health_does_not_lose_events() {
    rt::Ledger ledger;
    HealthPoll job(ledger);
    pthread_t readers[2];
    for (unsigned i=0;i!=2;++i) assert(!pthread_create(&readers[i],0,poll_health,&job));
    while (job.ready.load()!=2 || job.reads.load()<2) sched_yield();
    unsigned failures=0, completed=0;
    int method=0, worker=0, position=0;
    for (unsigned i=0;i!=2000 && !failures;++i) {
        rt::Token request, delivered, posted;
        rt::WorkerContext scope;
        rt::Trace trace;
        failures += ledger.request_begin(&method,issue(i),&request)!=rt::OK;
        failures += ledger.reply_enter(&method,reply(),&delivered)!=rt::OK;
        failures += ledger.worker_post(&worker,&position,delivered,&posted)!=rt::OK;
        failures += ledger.worker_enter(&worker,&scope)!=rt::OK;
        failures += ledger.position_take(&scope,&position,&trace)!=rt::OK;
        failures += ledger.request_end(&method)!=rt::OK;
        if (!failures) {
            assert(trace.request.id==request.id && trace.worker.id==posted.id);
            assert(trace.issue.session_event==i);
            ++completed;
        }
    }
    job.stop.store(true);
    for (unsigned i=0;i!=2;++i) assert(!pthread_join(readers[i],0));
    const rt::Status final=status(ledger);
    printf("health/event overlap: cycles=%u reads=%u failures=%u bad_status=%u loss=%u\n",
           completed,job.reads.load(),failures,job.bad.load(),final.loss_reasons);
    fflush(stdout);
    assert(!failures && completed==2000 && !job.bad.load());
    assert(final.loss_epoch==1 && !final.loss_reasons && !final.requests && !final.workers);
    puts("PASS concurrent health readers preserve every lifecycle event and owned trace");
}

int main() { health_does_not_lose_events(); return 0; }
