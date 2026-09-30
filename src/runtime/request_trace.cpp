#include "request_trace.h"

#include <errno.h>
#include <limits>
#include <string.h>

namespace mx5 { namespace runtime { namespace request_trace {
namespace {
static_assert(ATOMIC_INT_LOCK_FREE == 2,
              "Request trace requires lock-free control atomics");
static_assert(ATOMIC_LLONG_LOCK_FREE == 2 && ATOMIC_LONG_LOCK_FREE == 2,
              "Request trace requires lock-free epoch loads/stores");
static_assert(sizeof(unsigned) == 4, "Request trace control word is 32-bit");
struct PreserveErrno {
    const int saved;
    PreserveErrno() : saved(errno) {}
    ~PreserveErrno() { errno = saved; }
};
bool same(Token a, Token b) { return a.id == b.id && a.epoch == b.epoch; }
void normalize_text(Text& text) {
    // Only read the fixed owned value; a prefix cannot claim full identity.
    if (!text.known) text = Text();
    else if (!memchr(text.bytes, 0, sizeof text.bytes)) {
        text.bytes[Text::CAPACITY - 1] = 0;
        text.complete = false;
    }
}
Issue copy_issue(const Issue& input) {
    Issue result = input;
    result.known &= ISSUE_BUS_LIFETIME | ISSUE_SESSION_LIFETIME | ISSUE_SESSION_STATE;
    if (!(result.known & ISSUE_BUS_LIFETIME)) result.bus_lifetime = 0;
    if (!(result.known & ISSUE_SESSION_LIFETIME)) result.session_lifetime = 0;
    if (!(result.known & ISSUE_SESSION_STATE)) {
        result.session_event = 0;
        result.session_state = 0;
    }
    normalize_text(result.route.destination);normalize_text(result.route.path);
    normalize_text(result.route.interface_name);normalize_text(result.route.member);
    return result;
}
Reply copy_reply(const Reply& input) {
    Reply result = input;
    if (!result.type_known) result.type = 0;
    if (!result.wire_serial_known) result.wire_serial = 0;
    normalize_text(result.sender);normalize_text(result.error_name);
    return result;
}
}

Text copy_text(const char* input) {
    const PreserveErrno saved;
    Text result = Text();
    if (!input) return result;
    result.known = true;
    for (unsigned i = 0; i != Text::CAPACITY; ++i) {
        if (!input[i]) { result.complete = true; return result; }
        if (i + 1 < Text::CAPACITY) result.bytes[i] = input[i];
    }
    return result;
}

WorkerContext::WorkerContext() : trace_(), position_(0), active_(false) {}

Ledger::Ledger() : pending_loss_(0), published_epoch_(1), published_state_(0),
                  epoch_(1), next_id_(1), loss_reasons_(0),
                  request_initialized_(false), worker_initialized_(false),
                  initialized_(false), exhausted_(false),
                  requests_(), workers_() {
    const PreserveErrno saved;
    request_initialized_ = pthread_mutex_init(&mutex_, 0) == 0;
    worker_initialized_ = pthread_mutex_init(&worker_mutex_, 0) == 0;
    initialized_ = request_initialized_ && worker_initialized_;
    exhausted_ = !initialized_;
    if (exhausted_) published_state_.store(EXHAUSTED_BIT);
}
Ledger::~Ledger() {
    const PreserveErrno saved;
    if (request_initialized_) pthread_mutex_destroy(&mutex_);
    if (worker_initialized_) pthread_mutex_destroy(&worker_mutex_);
}

void Ledger::lose(unsigned reason, bool exhaust, bool consume_pending) {
    // Only a request-table owner can get here. The update bit makes status
    // reject this short multiword publication; all shared reads are atomic.
    published_state_.fetch_or(UPDATING_BIT);
    loss_reasons_ |= reason;
    exhausted_ = exhausted_ || exhaust;
    if (epoch_ == std::numeric_limits<uint64_t>::max()) {
        exhausted_ = true;
        loss_reasons_ |= LOSS_EPOCH_EXHAUSTED;
    } else {
        ++epoch_;
    }
    published_epoch_.store(epoch_);
    if (exhausted_) published_state_.fetch_or(EXHAUSTED_BIT);
    // A concurrent missed worker event before this clear also belongs to an
    // old token: only this request owner can issue any new-epoch token, later.
    if (consume_pending) loss_reasons_ |= pending_loss_.exchange(0);
    published_state_.fetch_or(loss_reasons_ << REASON_SHIFT);
    published_state_.fetch_and(~unsigned(UPDATING_BIT));
    // Do not reclaim potentially live objects after a missed event. Rows stay
    // occupied but their old epochs cannot match. Only actual end/consume
    // events may release them; time never proves OEM object death.
}

Result Ledger::enter(bool cleanup) {
    if (!initialized_) return EXHAUSTED;
    if (pthread_mutex_trylock(&mutex_) != 0) {
        pending_loss_.fetch_or(LOSS_CONTENTION);
        return BUSY;
    }
    const unsigned loss = pending_loss_.load();
    if (loss) lose(loss, false, true);
    if (exhausted_ && !cleanup) {
        pthread_mutex_unlock(&mutex_);
        return EXHAUSTED;
    }
    return OK;
}

Result Ledger::leave(Result result) {
    // A concurrent missed lifecycle event may occur while we hold the mutex.
    // Never return a usable old token in that case. A later event will advance
    // the epoch before it can look anything up. No OEM call runs under the lock.
    if (result == OK && pending_loss_.load()) result = STALE;
    pthread_mutex_unlock(&mutex_);
    return result;
}

Result Ledger::enter_worker() {
    if (!initialized_) return EXHAUSTED;
    if (pthread_mutex_trylock(&worker_mutex_) != 0) {
        pending_loss_.fetch_or(LOSS_CONTENTION);
        return BUSY;
    }
    return OK; // Cleanup is allowed even after generation/ID exhaustion.
}

Result Ledger::check_epoch(uint64_t expected) const {
    const unsigned before = published_state_.load();
    if (before & EXHAUSTED_BIT) return EXHAUSTED;
    if ((before & UPDATING_BIT) || pending_loss_.load()) return STALE;
    if (published_epoch_.load() != expected) return STALE;
    const unsigned after = published_state_.load();
    if (after & EXHAUSTED_BIT) return EXHAUSTED;
    if ((after & UPDATING_BIT) || pending_loss_.load() ||
        published_epoch_.load() != expected) return STALE;
    return OK;
}

Result Ledger::leave_worker(Result result, uint64_t expected_epoch) {
    if (result == OK) {
        if (expected_epoch) result = check_epoch(expected_epoch);
        else if (pending_loss_.load()) result = STALE;
    }
    pthread_mutex_unlock(&worker_mutex_);
    return result;
}

uint64_t Ledger::allocate_id() {
    if (next_id_ == std::numeric_limits<uint64_t>::max()) {
        lose(LOSS_ID_EXHAUSTED, true);
        return 0;
    }
    return next_id_++;
}

Ledger::RequestSlot* Ledger::request_at(uintptr_t method) {
    for (unsigned i = 0; i != REQUEST_CAPACITY; ++i)
        if (requests_[i].method == method) return &requests_[i];
    return 0;
}
Ledger::RequestSlot* Ledger::request_for(Token token) {
    for (unsigned i = 0; i != REQUEST_CAPACITY; ++i)
        if (requests_[i].method && same(requests_[i].token, token)) return &requests_[i];
    return 0;
}
Ledger::WorkerSlot* Ledger::worker_at(uintptr_t worker) {
    for (unsigned i = 0; i != WORKER_CAPACITY; ++i)
        if (workers_[i].worker == worker) return &workers_[i];
    return 0;
}

Result Ledger::request_begin(const void* method, const Issue& issue, Token* out) {
    const PreserveErrno saved;
    if (out) *out = Token();
    if (!method || !out) return BAD_INPUT;
    Result result = enter();
    if (result != OK) return result;
    const uintptr_t address = reinterpret_cast<uintptr_t>(method);
    if (request_at(address)) { lose(LOSS_COLLISION); return leave(CONFLICT); }
    RequestSlot* slot = request_at(0);
    if (!slot) { lose(LOSS_CAPACITY); return leave(FULL); }
    const uint64_t id = allocate_id();
    if (!id) return leave(EXHAUSTED);
    slot->method = address;
    slot->token.id = id;
    slot->token.epoch = epoch_;
    slot->issue = copy_issue(issue);
    published_state_.fetch_add(1);
    const Token token = slot->token;
    result = leave(OK);
    if (result == OK) *out = token;
    return result;
}

Result Ledger::reply_enter(const void* method, const Reply& reply, Token* out) {
    const PreserveErrno saved;
    if (out) *out = Token();
    if (!method || !out) return BAD_INPUT;
    Result result = enter();
    if (result != OK) return result;
    RequestSlot* slot = request_at(reinterpret_cast<uintptr_t>(method));
    if (!slot) return leave(NOT_FOUND);
    if (slot->token.epoch != epoch_) return leave(STALE);
    if (slot->replied) { lose(LOSS_COLLISION); return leave(CONFLICT); }
    slot->reply = copy_reply(reply);
    slot->replied = true;
    const Token token = slot->token;
    result = leave(OK);
    if (result == OK) *out = token;
    return result;
}

Result Ledger::request_end(const void* method) {
    const PreserveErrno saved;
    if (!method) return BAD_INPUT;
    Result result = enter(true);
    if (result != OK) return result;
    RequestSlot* slot = request_at(reinterpret_cast<uintptr_t>(method));
    if (!slot) return leave(NOT_FOUND);
    *slot = RequestSlot();
    published_state_.fetch_sub(1);
    return leave(OK);
}

Result Ledger::worker_post(const void* worker, const void* position,
                           Token request, Token* out) {
    const PreserveErrno saved;
    if (out) *out = Token();
    if (!worker || !position || !out || !request.id || !request.epoch) return BAD_INPUT;
    Result result = enter();
    if (result != OK) return result;
    if (request.epoch != epoch_) return leave(STALE);
    RequestSlot* source = request_for(request);
    if (!source) return leave(NOT_FOUND);
    if (!source->replied) return leave(NOT_READY);
    Trace trace = Trace();
    const uint64_t id = allocate_id();
    if (!id) return leave(EXHAUSTED);
    trace.request = source->token;
    trace.worker.id = id;
    trace.worker.epoch = epoch_;
    trace.issue = source->issue;
    trace.reply = source->reply;
    result = leave(OK);
    if (result != OK) return result;

    // The owned copy survives method destruction. Never hold both table
    // mutexes or let the request cleanup compete with worker consumption.
    result = enter_worker();
    if (result != OK) return result;
    result = check_epoch(trace.worker.epoch);
    if (result != OK) return leave_worker(result);
    const uintptr_t address = reinterpret_cast<uintptr_t>(worker);
    if (worker_at(address)) {
        pending_loss_.fetch_or(LOSS_COLLISION);
        return leave_worker(CONFLICT);
    }
    WorkerSlot* slot = worker_at(0);
    if (!slot) {
        pending_loss_.fetch_or(LOSS_CAPACITY);
        return leave_worker(FULL);
    }
    slot->worker = address;
    slot->position = reinterpret_cast<uintptr_t>(position);
    slot->trace = trace;
    published_state_.fetch_add(1 << WORKER_SHIFT);
    const Token token = slot->trace.worker;
    result = leave_worker(OK, token.epoch);
    if (result == OK) *out = token;
    return result;
}

Result Ledger::worker_enter(const void* worker, WorkerContext* out) {
    const PreserveErrno saved;
    if (out) worker_leave(out);
    if (!worker || !out) return BAD_INPUT;
    Result result = enter_worker();
    if (result != OK) return result;
    WorkerSlot* slot = worker_at(reinterpret_cast<uintptr_t>(worker));
    if (!slot) return leave_worker(NOT_FOUND);
    const Trace trace = slot->trace;
    const uintptr_t position = slot->position;
    *slot = WorkerSlot(); // The actual worker was consumed, even if trace is lost.
    published_state_.fetch_sub(1 << WORKER_SHIFT);
    result = leave_worker(OK, trace.worker.epoch);
    if (result == OK) {
        out->trace_ = trace;
        out->position_ = position;
        out->active_ = true;
    }
    return result;
}

Result Ledger::worker_destroy(const void* worker) {
    const PreserveErrno saved;
    if (!worker) return BAD_INPUT;
    Result result = enter_worker();
    if (result != OK) return result;
    WorkerSlot* slot = worker_at(reinterpret_cast<uintptr_t>(worker));
    if (!slot) return leave_worker(NOT_FOUND); // D0->D1 or already consumed is harmless.
    *slot = WorkerSlot();
    published_state_.fetch_sub(1 << WORKER_SHIFT);
    return leave_worker(OK);
}

Result Ledger::position_take(WorkerContext* scope, const void* position, Trace* out) {
    const PreserveErrno saved;
    if (out) *out = Trace();
    if (!scope || !out) return BAD_INPUT;
    if (!scope->active_) return USED;
    scope->active_ = false; // At most one attempt, including a mismatch or gap.
    const Trace trace = scope->trace_;
    const uintptr_t expected = scope->position_;
    scope->trace_ = Trace();
    scope->position_ = 0;
    if (!position || reinterpret_cast<uintptr_t>(position) != expected)
        return WRONG_POSITION;
    const Result result = check_epoch(trace.worker.epoch);
    if (result == OK) *out = trace;
    return result;
}

void Ledger::worker_leave(WorkerContext* scope) {
    const PreserveErrno saved;
    if (!scope) return;
    scope->trace_ = Trace();
    scope->position_ = 0;
    scope->active_ = false;
}

Result Ledger::status(Status* out) {
    const PreserveErrno saved;
    if (out) *out = Status();
    if (!out) return BAD_INPUT;
    if (!initialized_) return EXHAUSTED;
    const uint64_t before_epoch = published_epoch_.load();
    const unsigned before_pending = pending_loss_.load();
    const unsigned before_state = published_state_.load();
    const unsigned after_state = published_state_.load();
    const unsigned after_pending = pending_loss_.load();
    const uint64_t after_epoch = published_epoch_.load();
    if ((before_state | after_state) & UPDATING_BIT) return BUSY;
    if (before_epoch != after_epoch || before_pending != after_pending ||
        before_state != after_state) return STALE;
    Status copy = Status();
    copy.loss_epoch = before_epoch;
    copy.loss_reasons = ((before_state >> REASON_SHIFT) & REASON_MASK) | before_pending;
    copy.exhausted = (before_state & EXHAUSTED_BIT) != 0;
    copy.requests = before_state & COUNT_MASK;
    copy.workers = (before_state >> WORKER_SHIFT) & COUNT_MASK;
    if (before_pending) {
        // Pending already forbids every committed-epoch token. The next
        // request writer will publish this same effective epoch before clear.
        if (copy.loss_epoch == std::numeric_limits<uint64_t>::max()) {
            copy.exhausted = true;
            copy.loss_reasons |= LOSS_EPOCH_EXHAUSTED;
        } else ++copy.loss_epoch;
    }
    *out = copy;
    return OK;
}

const char* result_name(Result value) {
    switch (value) {
    case OK: return "observed";
    case NOT_FOUND: return "not_observed";
    case NOT_READY: return "reply_not_observed";
    case STALE: return "observation_gap";
    case BUSY: return "observation_busy";
    case FULL: return "observation_capacity";
    case CONFLICT: return "identity_conflict";
    case WRONG_POSITION: return "different_position_pointer";
    case USED: return "scope_consumed";
    case BAD_INPUT: return "invalid_observation_input";
    case EXHAUSTED: return "observation_ids_exhausted";
    }
    return "unknown_result";
}

} } }
