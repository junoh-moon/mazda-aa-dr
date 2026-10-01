// Authored synthetic identities and original-call fixtures. No OEM library,
// request, callback, ABI hook or qualified Provenance is exercised here.
#include "runtime/request_trace.h"

#include <assert.h>
#include <errno.h>
#include <limits>
#include <stdio.h>
#include <string.h>

#ifdef NDEBUG
#error Request trace regression checks require assertions
#endif

namespace rt = mx5::runtime::request_trace;
namespace mx5 { namespace runtime { namespace request_trace {
struct RequestTraceTestAccess {
    static void lock(Ledger& ledger) { assert(!pthread_mutex_lock(&ledger.mutex_)); }
    static void unlock(Ledger& ledger) { assert(!pthread_mutex_unlock(&ledger.mutex_)); }
    static void lock_worker(Ledger& ledger) { assert(!pthread_mutex_lock(&ledger.worker_mutex_)); }
    static void unlock_worker(Ledger& ledger) { assert(!pthread_mutex_unlock(&ledger.worker_mutex_)); }
    static void next_id(Ledger& ledger, uint64_t value) { ledger.next_id_ = value; }
    static void epoch(Ledger& ledger, uint64_t value) {
        ledger.epoch_ = value; ledger.published_epoch_.store(value);
    }
};
} } }

static rt::Issue issue(uint64_t sequence) {
    rt::Issue value = rt::Issue();
    value.observed_ns = 1000 + sequence;
    value.bus_lifetime = 10;
    value.session_lifetime = 20;
    value.session_event = sequence;
    value.session_state = 3;
    value.known = rt::ISSUE_BUS_LIFETIME | rt::ISSUE_SESSION_LIFETIME | rt::ISSUE_SESSION_STATE;
    return value;
}
static rt::Reply reply(const char* sender = ":1.24") {
    rt::Reply value = rt::Reply();
    value.observed_ns = 9000;
    value.type = 2;
    value.type_known = true;
    value.sender = rt::copy_text(sender);
    return value;
}
static void empty(rt::Token token) { assert(token.id == 0 && token.epoch == 0); }
static void empty(const rt::Trace& trace) {
    empty(trace.request);
    empty(trace.worker);
    assert(trace.issue.known == 0 && !trace.reply.type_known && !trace.reply.sender.known);
}
static rt::Status status(rt::Ledger& ledger) {
    rt::Status value;
    assert(ledger.status(&value) == rt::OK);
    return value;
}
static rt::Token ready(rt::Ledger& ledger, const void* method, uint64_t sequence = 1) {
    rt::Token begun, delivered;
    assert(ledger.request_begin(method, issue(sequence), &begun) == rt::OK);
    assert(ledger.reply_enter(method, reply(), &delivered) == rt::OK);
    assert(begun.id == delivered.id && begun.epoch == delivered.epoch);
    return delivered;
}
static rt::Token post(rt::Ledger& ledger, const void* worker, const void* position,
                      rt::Token request) {
    rt::Token result;
    assert(ledger.worker_post(worker, position, request, &result) == rt::OK);
    return result;
}
static rt::Trace take(rt::Ledger& ledger, const void* worker, const void* position) {
    rt::WorkerContext context;
    rt::Trace trace;
    assert(ledger.worker_enter(worker, &context) == rt::OK);
    assert(ledger.position_take(&context, position, &trace) == rt::OK);
    rt::Trace duplicate = trace;
    assert(ledger.position_take(&context, position, &duplicate) == rt::USED);
    empty(duplicate);
    return trace;
}

static void identical_out_of_order() {
    rt::Ledger ledger;
    int methods[2] = {}, workers[2] = {};
    unsigned char positions[2][72] = {}; // Identical numeric payloads, distinct objects.
    assert(!memcmp(positions[0], positions[1], sizeof positions[0]));
    rt::Issue first = issue(101), second = issue(202);
    rt::Token requests[2], delivered[2];
    assert(ledger.request_begin(&methods[0], first, &requests[0]) == rt::OK);
    assert(ledger.request_begin(&methods[1], second, &requests[1]) == rt::OK);
    first.session_lifetime = second.session_lifetime = 999; // New global state is not retroactive.
    first.session_event = second.session_event = 999;
    rt::Reply response = reply(":1.200");
    assert(ledger.reply_enter(&methods[1], response, &delivered[1]) == rt::OK);
    post(ledger, &workers[1], positions[1], delivered[1]);
    response.sender = rt::copy_text(":1.100");
    assert(ledger.reply_enter(&methods[0], response, &delivered[0]) == rt::OK);
    post(ledger, &workers[0], positions[0], delivered[0]);
    memset(&response, 0, sizeof response); // Stored worker metadata is owned.
    assert(ledger.request_end(&methods[1]) == rt::OK);
    assert(ledger.request_end(&methods[0]) == rt::OK);
    const rt::Trace a = take(ledger, &workers[0], positions[0]);
    const rt::Trace b = take(ledger, &workers[1], positions[1]);
    assert(a.request.id == requests[0].id && b.request.id == requests[1].id);
    assert(a.request.id != b.request.id && a.worker.id != b.worker.id);
    assert(a.issue.session_lifetime == 20 && b.issue.session_lifetime == 20);
    assert(a.issue.session_event == 101 && b.issue.session_event == 202);
    assert(!strcmp(a.reply.sender.bytes, ":1.100"));
    assert(!strcmp(b.reply.sender.bytes, ":1.200"));
    assert(a.reply.wire_serial == 0 && !a.reply.wire_serial_known);
    assert(status(ledger).requests == 0 && status(ledger).workers == 0);
    puts("PASS identical values, out-of-order replies/workers, owned issue/reply snapshots");
}

static void pointer_reuse_and_scope() {
    rt::Ledger ledger;
    int method = 0, worker = 0, other_worker = 0;
    unsigned char position[72] = {}, equal_position[72] = {};
    const rt::Token old_request = ready(ledger, &method);
    const rt::Token old_worker = post(ledger, &worker, position, old_request);
    rt::WorkerContext old_scope;
    rt::Trace trace;
    assert(ledger.worker_enter(&worker, &old_scope) == rt::OK);
    assert(ledger.position_take(&old_scope, position, &trace) == rt::OK);
    assert(ledger.request_end(&method) == rt::OK);
    assert(ledger.worker_destroy(&worker) == rt::NOT_FOUND); // Already consumed.

    const rt::Token fresh_request = ready(ledger, &method, 2);
    const rt::Token fresh_worker = post(ledger, &worker, position, fresh_request);
    assert(old_request.id != fresh_request.id && old_worker.id != fresh_worker.id);
    rt::Token rejected = fresh_worker;
    assert(ledger.worker_post(&other_worker, position, old_request, &rejected) == rt::NOT_FOUND);
    empty(rejected);
    assert(ledger.position_take(&old_scope, position, &trace) == rt::USED);
    empty(trace);
    rt::WorkerContext scope;
    assert(ledger.worker_enter(&worker, &scope) == rt::OK);
    assert(ledger.position_take(&scope, equal_position, &trace) == rt::WRONG_POSITION);
    empty(trace);
    assert(ledger.position_take(&scope, position, &trace) == rt::USED);
    rt::WorkerContext duplicate;
    assert(ledger.worker_enter(&worker, &duplicate) == rt::NOT_FOUND);
    assert(ledger.request_end(&method) == rt::OK);

    post(ledger, &worker, position, ready(ledger, &method, 3));
    assert(ledger.worker_enter(&worker, &scope) == rt::OK);
    ledger.worker_leave(&scope);
    assert(ledger.position_take(&scope, position, &trace) == rt::USED);
    assert(ledger.request_end(&method) == rt::OK);
    post(ledger, &worker, position, ready(ledger, &method, 4));
    const rt::Trace fresh = take(ledger, &worker, position);
    assert(fresh.issue.session_event == 4 && fresh.worker.id != old_worker.id);
    assert(ledger.request_end(&method) == rt::OK);
    puts("PASS pointer reuse, stale request token, exact position pointer, single-use/ended scope");
}

static void pending_and_real_cleanup() {
    rt::Ledger ledger;
    int method = 0, worker = 0, position = 0;
    rt::Token request, token;
    assert(ledger.request_begin(&method, issue(1), &request) == rt::OK);
    token = request; // Failure must clear a caller's previous nonzero result.
    assert(ledger.worker_post(&worker, &position, rt::Token(), &token) == rt::BAD_INPUT);
    empty(token);
    assert(status(ledger).requests == 1 && status(ledger).workers == 0);
    assert(ledger.worker_post(&worker, &position, request, &token) == rt::NOT_READY);
    empty(token);
    // No cancel/timeout API exists: uncertainty cannot release the pending row.
    for (unsigned i = 0; i != 100; ++i) assert(status(ledger).requests == 1);
    rt::Reply very_late = reply();
    very_late.observed_ns = std::numeric_limits<uint64_t>::max() - 1;
    assert(ledger.reply_enter(&method, very_late, &token) == rt::OK);
    assert(status(ledger).requests == 1); // Even the reply does not assert method death.
    post(ledger, &worker, &position, token);
    assert(ledger.request_end(&method) == rt::OK); // Authored actual-end event.
    assert(status(ledger).workers == 1);
    assert(ledger.worker_destroy(&worker) == rt::OK); // Cancelled queued worker never ran.
    assert(ledger.worker_destroy(&worker) == rt::NOT_FOUND); // D0 -> D1 duplicate is harmless.
    rt::WorkerContext context;
    assert(ledger.worker_enter(&worker, &context) == rt::NOT_FOUND);
    assert(ledger.reply_enter(&method, reply(), &token) == rt::NOT_FOUND);

    assert(ledger.request_begin(&method, issue(2), &request) == rt::OK);
    assert(ledger.request_end(&method) == rt::OK); // Immediate failure / silent destruction.
    assert(ledger.reply_enter(&method, reply(), &token) == rt::NOT_FOUND);
    assert(status(ledger).requests == 0 && status(ledger).workers == 0);
    puts("PASS pending retained without proved cancellation, actual destruction and consume cleanup");
}

static void request_capacity() {
    rt::Ledger ledger;
    int methods[rt::Ledger::REQUEST_CAPACITY + 1] = {};
    rt::Token tokens[rt::Ledger::REQUEST_CAPACITY], extra;
    for (unsigned i = 0; i != rt::Ledger::REQUEST_CAPACITY; ++i)
        assert(ledger.request_begin(&methods[i], issue(i), &tokens[i]) == rt::OK);
    const rt::Status before = status(ledger);
    assert(ledger.request_begin(&methods[rt::Ledger::REQUEST_CAPACITY], issue(999), &extra) == rt::FULL);
    empty(extra);
    const rt::Status full = status(ledger);
    assert(full.requests == rt::Ledger::REQUEST_CAPACITY && full.loss_epoch > before.loss_epoch);
    assert(full.loss_reasons & rt::LOSS_CAPACITY);
    assert(ledger.reply_enter(&methods[0], reply(), &extra) == rt::STALE);
    empty(extra);
    assert(status(ledger).requests == rt::Ledger::REQUEST_CAPACITY); // No eviction.
    assert(ledger.request_end(&methods[0]) == rt::OK);
    assert(ledger.request_begin(&methods[rt::Ledger::REQUEST_CAPACITY], issue(999), &extra) == rt::OK);
    assert(extra.id != tokens[0].id && extra.epoch != tokens[0].epoch);
    for (unsigned i = 1; i != rt::Ledger::REQUEST_CAPACITY + 1; ++i)
        assert(ledger.request_end(&methods[i]) == rt::OK);
    assert(status(ledger).requests == 0);
    puts("PASS request capacity, loss epoch, no live eviction, recovery after actual end");
}

static void worker_capacity() {
    rt::Ledger ledger;
    int method = 0, workers[rt::Ledger::WORKER_CAPACITY + 1] = {}, position = 0;
    const rt::Token request = ready(ledger, &method);
    for (unsigned i = 0; i != rt::Ledger::WORKER_CAPACITY; ++i)
        post(ledger, &workers[i], &position, request);
    rt::Token extra;
    assert(ledger.worker_post(&workers[rt::Ledger::WORKER_CAPACITY], &position, request, &extra) == rt::FULL);
    empty(extra);
    assert(status(ledger).workers == rt::Ledger::WORKER_CAPACITY);
    rt::WorkerContext context;
    rt::Trace trace;
    assert(ledger.worker_enter(&workers[0], &context) == rt::STALE);
    assert(ledger.position_take(&context, &position, &trace) == rt::USED);
    empty(trace);
    assert(status(ledger).workers == rt::Ledger::WORKER_CAPACITY - 1);
    assert(ledger.request_end(&method) == rt::OK);
    post(ledger, &workers[0], &position, ready(ledger, &method, 2));
    assert(take(ledger, &workers[0], &position).issue.session_event == 2);
    for (unsigned i = 1; i != rt::Ledger::WORKER_CAPACITY; ++i)
        assert(ledger.worker_destroy(&workers[i]) == rt::OK);
    assert(ledger.request_end(&method) == rt::OK);
    assert(status(ledger).workers == 0);
    puts("PASS worker capacity invalidates old bindings; actual consume/destruction frees slots");
}

static void collisions() {
    {
        rt::Ledger ledger;
        int method = 0;
        rt::Token old, token;
        assert(ledger.request_begin(&method, issue(1), &old) == rt::OK);
        assert(ledger.request_begin(&method, issue(2), &token) == rt::CONFLICT);
        empty(token);
        assert(status(ledger).requests == 1);
        assert(ledger.reply_enter(&method, reply(), &token) == rt::STALE);
        assert(ledger.request_end(&method) == rt::OK);
        assert(ready(ledger, &method).id != old.id);
        assert(ledger.request_end(&method) == rt::OK);
    }
    {
        rt::Ledger ledger;
        int method = 0;
        ready(ledger, &method);
        rt::Token token;
        assert(ledger.reply_enter(&method, reply(), &token) == rt::CONFLICT);
        empty(token);
        assert(ledger.request_end(&method) == rt::OK);
    }
    {
        rt::Ledger ledger;
        int method = 0, worker = 0, position = 0;
        const rt::Token request = ready(ledger, &method);
        post(ledger, &worker, &position, request);
        rt::Token token;
        assert(ledger.worker_post(&worker, &position, request, &token) == rt::CONFLICT);
        empty(token);
        assert(status(ledger).workers == 1);
        rt::WorkerContext context;
        assert(ledger.worker_enter(&worker, &context) == rt::STALE);
        assert(status(ledger).loss_reasons & rt::LOSS_COLLISION);
        assert(ledger.request_end(&method) == rt::OK);
    }
    puts("PASS duplicate method, reply and worker identities are unknown, never overwritten");
}

struct BusyJob {
    rt::Ledger* ledger;
    const void* object;
    bool destroy_worker;
    rt::Result result;
};
static void* destroy_busy(void* input) {
    BusyJob& job = *static_cast<BusyJob*>(input);
    errno = EDOM;
    job.result = job.destroy_worker ? job.ledger->worker_destroy(job.object)
                                    : job.ledger->request_end(job.object);
    assert(errno == EDOM);
    return 0;
}
static void busy_cleanup_and_reuse() {
    rt::Ledger ledger;
    int method = 0, worker = 0, position = 0;
    post(ledger, &worker, &position, ready(ledger, &method));
    const rt::Status before = status(ledger);
    rt::RequestTraceTestAccess::lock(ledger);
    rt::Status unavailable;
    assert(ledger.status(&unavailable) == rt::OK); // Status never takes either table lock.
    assert(unavailable.requests == 1 && unavailable.workers == 1 && !unavailable.loss_reasons);
    rt::RequestTraceTestAccess::unlock(ledger);
    assert(status(ledger).loss_epoch == before.loss_epoch);

    rt::RequestTraceTestAccess::lock_worker(ledger);
    BusyJob job = {&ledger, &worker, true, rt::OK};
    pthread_t thread;
    assert(!pthread_create(&thread, 0, destroy_busy, &job));
    assert(!pthread_join(thread, 0)); // Completes while mutex stays held: no blocking/spin.
    assert(job.result == rt::BUSY);
    rt::RequestTraceTestAccess::unlock_worker(ledger);
    const rt::Status lost = status(ledger);
    assert(lost.loss_epoch > before.loss_epoch && lost.workers == 1 && lost.requests == 1);
    assert(lost.loss_reasons & rt::LOSS_CONTENTION);
    assert(ledger.request_end(&method) == rt::OK);
    const rt::Token fresh_request = ready(ledger, &method, 2);
    rt::Token token;
    assert(ledger.worker_post(&worker, &position, fresh_request, &token) == rt::CONFLICT);
    empty(token); // A reused address cannot inherit the missed destructor's old reply.
    rt::WorkerContext context;
    assert(ledger.worker_enter(&worker, &context) == rt::STALE);
    rt::Trace trace;
    assert(ledger.position_take(&context, &position, &trace) == rt::USED);
    empty(trace);
    assert(ledger.request_end(&method) == rt::OK);

    // Missed method destruction has the same anti-reuse rule.
    ready(ledger, &method, 3);
    rt::RequestTraceTestAccess::lock(ledger);
    job.object = &method;
    job.destroy_worker = false;
    assert(!pthread_create(&thread, 0, destroy_busy, &job));
    assert(!pthread_join(thread, 0));
    assert(job.result == rt::BUSY);
    rt::RequestTraceTestAccess::unlock(ledger);
    assert(ledger.request_begin(&method, issue(4), &token) == rt::CONFLICT);
    assert(ledger.reply_enter(&method, reply(), &token) == rt::STALE);
    assert(ledger.request_end(&method) == rt::OK);
    post(ledger, &worker, &position, ready(ledger, &method, 5));
    assert(take(ledger, &worker, &position).issue.session_event == 5);
    assert(ledger.request_end(&method) == rt::OK);

    // A consumed row is now an owned doWork scope, but loss before its position
    // call must still invalidate that old epoch rather than trusting the copy.
    post(ledger, &worker, &position, ready(ledger, &method, 6));
    assert(ledger.worker_enter(&worker, &context) == rt::OK);
    rt::RequestTraceTestAccess::lock(ledger);
    assert(!pthread_create(&thread, 0, destroy_busy, &job));
    assert(!pthread_join(thread, 0));
    assert(job.result == rt::BUSY);
    rt::RequestTraceTestAccess::unlock(ledger);
    assert(ledger.position_take(&context, &position, &trace) == rt::STALE);
    empty(trace);
    assert(ledger.request_end(&method) == rt::OK);

    post(ledger, &worker, &position, ready(ledger, &method, 7));
    assert(ledger.worker_enter(&worker, &context) == rt::OK);
    assert(ledger.position_take(&context, &position, 0) == rt::BAD_INPUT);
    // This is an owned scope: a request-table lock cannot obstruct its consume.
    rt::RequestTraceTestAccess::lock(ledger);
    trace.request.id = trace.worker.id = 123;
    errno = EDOM;
    assert(ledger.position_take(&context, &position, &trace) == rt::OK);
    assert(errno == EDOM);
    assert(trace.request.id && trace.worker.id && trace.issue.session_event == 7);
    rt::RequestTraceTestAccess::unlock(ledger);
    assert(ledger.position_take(&context, &position, &trace) == rt::USED);
    empty(trace);
    assert(status(ledger).requests == 1);
    assert(ledger.request_end(&method) == rt::OK);

    post(ledger, &worker, &position, ready(ledger, &method, 8));
    assert(ledger.worker_enter(&worker, &context) == rt::OK);
    rt::RequestTraceTestAccess::lock(ledger);
    assert(!pthread_create(&thread, 0, destroy_busy, &job));
    assert(!pthread_join(thread, 0) && job.result == rt::BUSY);
    assert(ledger.position_take(&context, &position, &trace) == rt::STALE);
    empty(trace);
    rt::RequestTraceTestAccess::unlock(ledger);
    assert(ledger.position_take(&context, &position, &trace) == rt::USED);
    empty(trace);
    assert(ledger.request_end(&method) == rt::OK);
    puts("PASS missed destructor/end under real thread contention cannot revive reused pointers");
}

static void generation_exhaustion() {
    {
        rt::Ledger ledger;
        int method = 0, other = 0;
        rt::Token token = ready(ledger, &method);
        rt::RequestTraceTestAccess::next_id(ledger, std::numeric_limits<uint64_t>::max());
        assert(ledger.request_begin(&other, issue(2), &token) == rt::EXHAUSTED);
        empty(token);
        assert(ledger.reply_enter(&method, reply(), &token) == rt::EXHAUSTED);
        assert(status(ledger).exhausted);
        assert(status(ledger).loss_reasons & rt::LOSS_ID_EXHAUSTED);
        assert(ledger.request_end(&method) == rt::OK); // Cleanup remains available.
        assert(status(ledger).requests == 0);
    }
    {
        rt::Ledger ledger;
        int method = 0, workers[2] = {}, position = 0;
        const rt::Token request = ready(ledger, &method);
        rt::RequestTraceTestAccess::next_id(ledger, std::numeric_limits<uint64_t>::max() - 1);
        const rt::Token last = post(ledger, &workers[0], &position, request);
        assert(last.id == std::numeric_limits<uint64_t>::max() - 1);
        rt::Token token;
        assert(ledger.worker_post(&workers[1], &position, request, &token) == rt::EXHAUSTED);
        empty(token);
        rt::WorkerContext scope;
        assert(ledger.worker_enter(&workers[0], &scope) == rt::EXHAUSTED);
        assert(status(ledger).workers == 0);
        assert(ledger.request_end(&method) == rt::OK);
    }
    {
        rt::Ledger ledger;
        int method = 0;
        rt::RequestTraceTestAccess::epoch(ledger, std::numeric_limits<uint64_t>::max());
        rt::Token token;
        assert(ledger.request_begin(&method, issue(1), &token) == rt::OK);
        assert(token.epoch == std::numeric_limits<uint64_t>::max());
        assert(ledger.request_begin(&method, issue(2), &token) == rt::CONFLICT);
        const rt::Status value = status(ledger);
        assert(value.exhausted && value.loss_epoch == std::numeric_limits<uint64_t>::max());
        assert(value.loss_reasons & rt::LOSS_EPOCH_EXHAUSTED);
        assert(ledger.reply_enter(&method, reply(), &token) == rt::EXHAUSTED);
        empty(token);
        assert(ledger.request_end(&method) == rt::OK);
    }
    puts("PASS ID/epoch exhaustion never wraps to a reusable identity; OEM cleanup stays possible");
}

static void metadata_unknown_and_errors() {
    rt::Ledger ledger;
    int method = 0, worker = 0, position = 0;
    rt::Issue unseen = issue(99);
    unseen.known = 0;
    rt::Reply response = reply();
    response.type = 3; // Synthetic observed error, independent of numeric position fields.
    response.wire_serial = 12345;
    response.error_name = rt::copy_text("org.example.Error");
    char long_sender[100];
    memset(long_sender, 'x', sizeof long_sender);
    long_sender[sizeof long_sender - 1] = 0;
    response.sender = rt::copy_text(long_sender);
    assert(response.sender.known && !response.sender.complete && strlen(response.sender.bytes) == 63);
    rt::Token token;
    assert(ledger.request_begin(&method, unseen, &token) == rt::OK);
    assert(ledger.reply_enter(&method, response, &token) == rt::OK);
    post(ledger, &worker, &position, token);
    const rt::Trace value = take(ledger, &worker, &position);
    assert(value.issue.known == 0 && value.issue.bus_lifetime == 0 && value.issue.session_lifetime == 0);
    assert(value.issue.session_event == 0 && value.issue.session_state == 0);
    assert(value.reply.type_known && value.reply.type == 3);
    assert(!value.reply.wire_serial_known && value.reply.wire_serial == 0);
    assert(value.reply.sender.known && !value.reply.sender.complete);
    assert(!strcmp(value.reply.error_name.bytes, "org.example.Error"));
    assert(ledger.request_end(&method) == rt::OK);
    const rt::Text absent = rt::copy_text(0), empty_string = rt::copy_text("");
    assert(!absent.known && !absent.complete && !absent.bytes[0]);
    assert(empty_string.known && empty_string.complete && !empty_string.bytes[0]);
    char exact[64]; memset(exact, 'a', sizeof exact); exact[63] = 0;
    assert(rt::copy_text(exact).complete);

    rt::Reply malformed = rt::Reply();
    memset(malformed.sender.bytes, 's', sizeof malformed.sender.bytes);
    malformed.sender.known = malformed.sender.complete = true;
    memset(malformed.error_name.bytes, 'e', sizeof malformed.error_name.bytes);
    malformed.error_name.known = malformed.error_name.complete = true;
    malformed.type = 99; // Explicitly unknown, so this dirty value must not survive.
    malformed.wire_serial_known = true;
    malformed.wire_serial = 57;
    unseen.route.destination=malformed.sender;
    unseen.route.path=malformed.error_name;
    unseen.route.path.known=false; // Stale bytes are not a known routing field.
    assert(ledger.request_begin(&method, unseen, &token) == rt::OK);
    assert(ledger.reply_enter(&method, malformed, &token) == rt::OK);
    post(ledger, &worker, &position, token);
    const rt::Trace normalized = take(ledger, &worker, &position);
    assert(normalized.issue.route.destination.known && !normalized.issue.route.destination.complete);
    assert(normalized.issue.route.destination.bytes[63]==0);
    assert(!normalized.issue.route.path.known && !normalized.issue.route.path.complete &&
           !normalized.issue.route.path.bytes[0]);
    assert(normalized.reply.sender.known && !normalized.reply.sender.complete);
    assert(normalized.reply.error_name.known && !normalized.reply.error_name.complete);
    assert(normalized.reply.sender.bytes[63] == 0 && normalized.reply.error_name.bytes[63] == 0);
    for (unsigned i = 0; i != 63; ++i) {
        assert(normalized.reply.sender.bytes[i] == 's');
        assert(normalized.reply.error_name.bytes[i] == 'e');
    }
    assert(!normalized.reply.type_known && normalized.reply.type == 0);
    assert(normalized.reply.wire_serial_known && normalized.reply.wire_serial == 57);
    assert(malformed.sender.complete && malformed.sender.bytes[63] == 's');
    assert(malformed.error_name.complete && malformed.error_name.bytes[63] == 'e');
    assert(ledger.request_end(&method) == rt::OK);
    puts("PASS unknown session/bus/serial, bounded sender truncation and actual error stay distinct");
}

struct CrossThread {
    rt::Ledger* ledger;
    int method, worker, position;
    rt::Token request;
    rt::Trace trace;
    bool submit_on_stack, post_on_stack;
    unsigned original_submits, original_posts;
};
static void* run_worker(void* input) {
    CrossThread& fixture = *static_cast<CrossThread*>(input);
    assert(fixture.submit_on_stack && fixture.post_on_stack);
    fixture.trace = take(*fixture.ledger, &fixture.worker, &fixture.position);
    return 0;
}
static int synthetic_original_post(CrossThread& fixture) {
    ++fixture.original_posts;
    fixture.post_on_stack = true;
    pthread_t worker_thread;
    assert(!pthread_create(&worker_thread, 0, run_worker, &fixture));
    assert(!pthread_join(worker_thread, 0));
    fixture.post_on_stack = false;
    return 31;
}
static void* reply_and_post(void* input) {
    CrossThread& fixture = *static_cast<CrossThread*>(input);
    assert(fixture.submit_on_stack);
    rt::Token delivered;
    assert(fixture.ledger->reply_enter(&fixture.method, reply(), &delivered) == rt::OK);
    assert(delivered.id == fixture.request.id);
    post(*fixture.ledger, &fixture.worker, &fixture.position, delivered);
    assert(synthetic_original_post(fixture) == 31);
    assert(fixture.ledger->request_end(&fixture.method) == rt::OK);
    return 0;
}
static int synthetic_original_submit(CrossThread& fixture) {
    ++fixture.original_submits;
    fixture.submit_on_stack = true;
    pthread_t callback_thread;
    assert(!pthread_create(&callback_thread, 0, reply_and_post, &fixture));
    assert(!pthread_join(callback_thread, 0));
    fixture.submit_on_stack = false;
    return 17;
}
static void cross_thread_and_early_work() {
    rt::Ledger ledger;
    CrossThread fixture = CrossThread();
    fixture.ledger = &ledger;
    assert(ledger.request_begin(&fixture.method, issue(77), &fixture.request) == rt::OK);
    // Both synthetic originals are actually on their stacks when reply/worker
    // run. Their distinct returns and one-call counts are also preserved.
    assert(synthetic_original_submit(fixture) == 17);
    assert(fixture.original_submits == 1 && fixture.original_posts == 1);
    assert(fixture.trace.request.id == fixture.request.id);
    assert(fixture.trace.issue.session_event == 77);
    assert(status(ledger).requests == 0 && status(ledger).workers == 0);
    puts("PASS reply before submit return and queued worker on a different thread without TLS identity");
}

struct StressJob {
    rt::Ledger* ledger;
    unsigned tag;
    std::atomic<unsigned>* calls;
    std::atomic<unsigned>* matches;
};
static void* stress(void* input) {
    StressJob& job = *static_cast<StressJob*>(input);
    int method = 0, worker = 0, position = 0;
    for (unsigned i = 0; i != 1000; ++i) {
        const uint64_t marker = uint64_t(job.tag) * 10000 + i;
        rt::Token request = rt::Token(), delivered = rt::Token(), posted = rt::Token();
        const rt::Result begun = job.ledger->request_begin(&method, issue(marker), &request);
        if (begun != rt::OK) empty(request);
        const rt::Result replied = job.ledger->reply_enter(&method, reply(), &delivered);
        if (replied != rt::OK) empty(delivered);
        const rt::Result queued = job.ledger->worker_post(&worker, &position, delivered, &posted);
        if (queued != rt::OK) empty(posted);
        rt::WorkerContext scope;
        const rt::Result entered = job.ledger->worker_enter(&worker, &scope);
        rt::Trace trace;
        const rt::Result consumed = job.ledger->position_take(&scope, &position, &trace);
        if (consumed == rt::OK) {
            assert(begun == rt::OK && replied == rt::OK && queued == rt::OK && entered == rt::OK);
            assert(trace.request.id == request.id && trace.worker.id == posted.id);
            assert(trace.issue.session_event == marker);
            job.matches->fetch_add(1, std::memory_order_relaxed);
        } else empty(trace);
        // Original invocation is unconditional even if every observation failed.
        job.calls->fetch_add(1, std::memory_order_relaxed);
        job.ledger->worker_destroy(&worker); // Authored live destruction boundary.
        job.ledger->request_end(&method);
    }
    return 0;
}
static void concurrency_stress() {
    rt::Ledger ledger;
    std::atomic<unsigned> calls(0), matches(0);
    StressJob jobs[4]; pthread_t threads[4];
    for (unsigned i = 0; i != 4; ++i) {
        jobs[i] = StressJob{&ledger, i + 1, &calls, &matches};
        assert(!pthread_create(&threads[i], 0, stress, &jobs[i]));
    }
    for (unsigned i = 0; i != 4; ++i) assert(!pthread_join(threads[i], 0));
    assert(calls.load() == 4000);
    const rt::Status value = status(ledger);
    // Failed destruction observations can leave quarantined rows. Their fixed
    // capacity is not a leak of OEM ownership, and we do not invent cleanup.
    assert(value.requests <= 4 && value.workers <= 4);
    printf("PASS concurrent callbacks/workers: original_calls=%u exact_observations=%u (schedule-dependent)\n",
           calls.load(), matches.load());
}

typedef int32_t (*OriginalCallback)(void*, const void*);
static unsigned original_callback_calls = 0;
struct OriginalFixture {
    unsigned calls;
    void* expected_user;
    const void* expected_payload;
    OriginalCallback expected_callback;
};
static int32_t original_callback(void* user, const void* payload) {
    assert(*static_cast<int*>(user) == 42);
    assert(static_cast<const unsigned char*>(payload)[0] == 1);
    ++original_callback_calls;
    return 7;
}
static int32_t original(OriginalFixture& fixture, OriginalCallback callback,
                        void* user, const void* payload) {
    assert(callback == fixture.expected_callback && user == fixture.expected_user);
    assert(payload == fixture.expected_payload && errno == EDOM);
    ++fixture.calls;
    assert(callback(user, payload) == 7);
    errno = EPIPE;
    return int32_t(-1234567);
}
static int32_t synthetic_forward(rt::Ledger& ledger, const void* method,
                                 OriginalFixture& fixture, rt::Result* observed) {
    rt::Token token;
    *observed = ledger.request_begin(method, issue(1), &token);
    // This is an authored contract fixture, not a claim of installed OEM hooks.
    const int32_t result = original(fixture, fixture.expected_callback,
                                    fixture.expected_user, fixture.expected_payload);
    ledger.request_end(method);
    return result;
}
static void forwarding_and_errno() {
    rt::Ledger ledger;
    int methods[rt::Ledger::REQUEST_CAPACITY + 1] = {}, userdata = 42;
    const unsigned char payload[72] = {1, 2, 3};
    unsigned char copy[sizeof payload]; memcpy(copy, payload, sizeof copy);
    OriginalFixture fixture = {0, &userdata, payload, original_callback};
    rt::Result observed;
    errno = EDOM;
    assert(synthetic_forward(ledger, &methods[0], fixture, &observed) == -1234567);
    assert(observed == rt::OK && errno == EPIPE && fixture.calls == 1);
    rt::RequestTraceTestAccess::lock(ledger);
    errno = EDOM;
    assert(synthetic_forward(ledger, &methods[0], fixture, &observed) == -1234567);
    assert(observed == rt::BUSY && errno == EPIPE && fixture.calls == 2);
    rt::RequestTraceTestAccess::unlock(ledger);
    rt::Token token;
    for (unsigned i = 0; i != rt::Ledger::REQUEST_CAPACITY; ++i)
        assert(ledger.request_begin(&methods[i], issue(i), &token) == rt::OK);
    errno = EDOM;
    assert(synthetic_forward(ledger, &methods[rt::Ledger::REQUEST_CAPACITY], fixture, &observed) == -1234567);
    assert(observed == rt::FULL && errno == EPIPE && fixture.calls == 3);
    errno = EDOM;
    assert(synthetic_forward(ledger, &methods[0], fixture, &observed) == -1234567);
    assert(observed == rt::CONFLICT && errno == EPIPE && fixture.calls == 4);
    assert(original_callback_calls == 4);
    assert(userdata == 42 && !memcmp(copy, payload, sizeof payload));
    for (unsigned i = 1; i != rt::Ledger::REQUEST_CAPACITY; ++i)
        assert(ledger.request_end(&methods[i]) == rt::OK);
    assert(status(ledger).requests == 0);
    puts("PASS synthetic exactly-once original callback/userdata/payload/return/errno for OK/busy/full/conflict");
}

static rt::WireIssue wire_issue(uint32_t serial,uint64_t when=100) {
    rt::WireIssue value=rt::WireIssue();value.known=true;value.serial=serial;value.observed_ns=when;
    return value;
}
static rt::WireReply wire_reply(uint32_t serial,uint32_t reply_serial) {
    rt::WireReply value=rt::WireReply();value.known=true;value.serial=serial;
    value.reply_serial=reply_serial;value.type=2;value.observed_ns=200;
    value.sender=rt::copy_text(":1.raw");return value;
}
static void raw_wire_identity_out_of_order() {
    rt::Ledger ledger;int methods[2]={},workers[2]={};unsigned char positions[2][72]={};
    rt::Token requests[2],delivered;
    for(unsigned n=0;n<2;++n) {
        assert(ledger.request_begin(&methods[n],issue(n),&requests[n])==rt::OK);
        errno=EDOM;
        assert(ledger.wire_issue(requests[n],wire_issue(41+n,110+n))==rt::OK&&errno==EDOM);
    }
    for(int n=1;n>=0;--n) {
        rt::Reply response=reply(":1.public");response.wire=wire_reply(81+unsigned(n),41+unsigned(n));
        assert(ledger.reply_enter(&methods[n],response,&delivered)==rt::OK);
        response.wire=rt::WireReply(); // No borrowed raw metadata survives.
        post(ledger,&workers[n],positions[n],delivered);
        assert(ledger.request_end(&methods[n])==rt::OK);
    }
    for(unsigned n=0;n<2;++n) {
        const rt::Trace trace=take(ledger,&workers[n],positions[n]);
        assert(trace.request.id==requests[n].id&&trace.issue.wire.known&&!trace.issue.wire.conflict);
        assert(trace.issue.wire.serial==41+n&&trace.issue.wire.observed_ns==110+n);
        assert(trace.reply.wire.known&&trace.reply.wire.serial==81+n&&trace.reply.wire.reply_serial==41+n);
        assert(trace.reply.wire.type==2&&!strcmp(trace.reply.wire.sender.bytes,":1.raw"));
        assert(!trace.reply.wire_serial_known&&trace.reply.wire_serial==0);
        assert(!strcmp(trace.reply.sender.bytes,":1.public"));
    }
    assert(status(ledger).loss_epoch==1&&!status(ledger).loss_reasons);
    puts("PASS raw wire identity survives identical in-flight payloads and reversed reply/work order");
}
static void raw_wire_duplicate_conflict_and_lifetime() {
    rt::Ledger ledger;int method=0,worker=0,position=0;rt::Token token,returned;
    assert(ledger.request_begin(&method,issue(1),&token)==rt::OK);
    assert(ledger.wire_issue(token,wire_issue(21,110))==rt::OK);
    assert(ledger.wire_issue(token,wire_issue(21,999))==rt::OK);
    assert(ledger.wire_issue(token,wire_issue(22,1000))==rt::CONFLICT);
    assert(ledger.wire_issue(token,wire_issue(21,1001))==rt::CONFLICT);
    assert(status(ledger).loss_epoch==1&&!status(ledger).loss_reasons);
    rt::Reply response=reply();response.wire=wire_reply(2,21);
    assert(ledger.reply_enter(&method,response,&returned)==rt::OK);
    assert(ledger.wire_issue(token,wire_issue(23))==rt::USED); // Already frozen.
    post(ledger,&worker,&position,returned);
    assert(ledger.request_end(&method)==rt::OK);
    rt::Token fresh;assert(ledger.request_begin(&method,issue(2),&fresh)==rt::OK);
    assert(ledger.wire_issue(token,wire_issue(99))==rt::NOT_FOUND);
    const rt::Trace old=take(ledger,&worker,&position);
    assert(old.issue.wire.known&&old.issue.wire.conflict&&old.issue.wire.serial==21);
    assert(old.issue.wire.observed_ns==110); // Repeated reads never retime it.
    assert(ledger.wire_issue(fresh,wire_issue(24))==rt::OK);
    assert(ledger.reply_enter(&method,reply(),&returned)==rt::OK);
    post(ledger,&worker,&position,returned);assert(ledger.request_end(&method)==rt::OK);
    const rt::Trace current=take(ledger,&worker,&position);
    assert(current.request.id==fresh.id&&current.issue.wire.serial==24&&!current.issue.wire.conflict);
    assert(!current.reply.wire.known&&!current.reply.wire.serial);
    assert(status(ledger).loss_epoch==1&&!status(ledger).loss_reasons);
    puts("PASS raw wire duplicate capture, sticky conflict, frozen reply and method address reuse");
}
static void raw_wire_contention_does_not_lose_lifetimes() {
    rt::Ledger ledger;int method=0,worker=0,position=0;rt::Token token,returned;
    assert(ledger.request_begin(&method,issue(1),&token)==rt::OK);
    rt::RequestTraceTestAccess::lock(ledger);errno=ERANGE;
    assert(ledger.wire_issue(token,wire_issue(31))==rt::BUSY&&errno==ERANGE);
    const rt::Status during=status(ledger);
    assert(during.loss_epoch==1&&!during.loss_reasons&&during.requests==1);
    rt::RequestTraceTestAccess::unlock(ledger);
    assert(ledger.reply_enter(&method,reply(),&returned)==rt::OK);
    post(ledger,&worker,&position,returned);assert(ledger.request_end(&method)==rt::OK);
    const rt::Trace trace=take(ledger,&worker,&position);
    assert(trace.request.id==token.id&&!trace.issue.wire.known);
    // An actual lost lifetime event still revokes the normal Ledger epoch.
    assert(ledger.request_begin(&method,issue(2),&token)==rt::OK);
    assert(ledger.request_begin(&method,issue(3),&returned)==rt::CONFLICT);
    const rt::Status lost=status(ledger);
    assert(ledger.wire_issue(token,wire_issue(32))==rt::STALE);
    assert(status(ledger).loss_epoch==lost.loss_epoch&&status(ledger).loss_reasons==lost.loss_reasons);
    assert(ledger.request_end(&method)==rt::OK);
    puts("PASS supplemental wire contention preserves request lifetimes and rejects a truly stale token");
}
static void raw_wire_unknown_and_local_error() {
    rt::Ledger ledger;int method=0,worker=0,position=0;rt::Token token,returned;
    rt::Issue request=issue(1);request.wire=wire_issue(77);request.wire.known=false;
    request.wire.conflict=true;
    assert(ledger.request_begin(&method,request,&token)==rt::OK);
    rt::Reply response=reply();response.wire=wire_reply(88,77);response.wire.known=false;
    response.wire.error_name=rt::copy_text("stale-error");
    assert(ledger.reply_enter(&method,response,&returned)==rt::OK);
    post(ledger,&worker,&position,returned);assert(ledger.request_end(&method)==rt::OK);
    const rt::Trace unknown=take(ledger,&worker,&position);
    assert(!unknown.issue.wire.known&&unknown.issue.wire.conflict&&!unknown.issue.wire.serial&&!unknown.issue.wire.observed_ns);
    assert(!unknown.reply.wire.known&&!unknown.reply.wire.observed_ns&&!unknown.reply.wire.serial&&
        !unknown.reply.wire.reply_serial&&!unknown.reply.wire.type&&!unknown.reply.wire.sender.known&&
        !unknown.reply.wire.sender.complete&&!unknown.reply.wire.sender.bytes[0]&&
        !unknown.reply.wire.error_name.known&&!unknown.reply.wire.error_name.bytes[0]);
    assert(ledger.request_begin(&method,issue(2),&token)==rt::OK);
    rt::WireIssue no_serial=wire_issue(0);
    assert(ledger.wire_issue(token,no_serial)==rt::NOT_READY);
    assert(ledger.wire_issue(token,wire_issue(42,0))==rt::OK); // Missing observation clock is separate.
    response=reply();response.wire=wire_reply(0,42);response.wire.type=3;
    response.wire.sender=rt::Text();response.wire.error_name=rt::copy_text("org.freedesktop.DBus.Error.NoReply");
    assert(ledger.reply_enter(&method,response,&returned)==rt::OK);
    post(ledger,&worker,&position,returned);assert(ledger.request_end(&method)==rt::OK);
    const rt::Trace timeout=take(ledger,&worker,&position);
    assert(timeout.issue.wire.known&&timeout.issue.wire.serial==42&&!timeout.issue.wire.observed_ns);
    assert(timeout.reply.wire.known&&!timeout.reply.wire.serial&&timeout.reply.wire.reply_serial==42&&
        timeout.reply.wire.type==3&&!timeout.reply.wire.sender.known);
    assert(!strcmp(timeout.reply.wire.error_name.bytes,"org.freedesktop.DBus.Error.NoReply"));
    assert(!timeout.reply.wire_serial_known); // Original getter result unchanged.
    puts("PASS unknown raw metadata normalization and locally created NoReply remain distinct");
}

int main() {
    raw_wire_identity_out_of_order();
    raw_wire_duplicate_conflict_and_lifetime();
    raw_wire_contention_does_not_lose_lifetimes();
    raw_wire_unknown_and_local_error();
    identical_out_of_order();
    pointer_reuse_and_scope();
    pending_and_real_cleanup();
    request_capacity();
    worker_capacity();
    collisions();
    busy_cleanup_and_reuse();
    generation_exhaustion();
    metadata_unknown_and_errors();
    cross_thread_and_early_work();
    concurrency_stress();
    forwarding_and_errno();
    puts("PASS 16 request-trace regression groups; synthetic only, OEM integration remains TODO");
    return 0;
}
