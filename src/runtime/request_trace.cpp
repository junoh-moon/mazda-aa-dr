#include "request_trace.h"

#include <errno.h>
#include <limits>
#include <string.h>

namespace mx5 { namespace runtime { namespace request_trace {
namespace {
static_assert(ATOMIC_INT_LOCK_FREE == 2,
              "Request trace requires lock-free control atomics");
static_assert(sizeof(unsigned) == 4, "Request trace control word is 32-bit");
struct PreserveErrno {
    const int saved;
    PreserveErrno() : saved(errno) {}
    ~PreserveErrno() { errno = saved; }
};
bool same(Token a, Token b) { return a.id == b.id && a.epoch == b.epoch; }
Issue copy_issue(const Issue& input) {
    Issue result = input;
    result.known &= ISSUE_BUS_LIFETIME | ISSUE_SESSION_LIFETIME | ISSUE_SESSION_STATE;
    if (!(result.known & ISSUE_BUS_LIFETIME)) result.bus_lifetime = 0;
    if (!(result.known & ISSUE_SESSION_LIFETIME)) result.session_lifetime = 0;
    if (!(result.known & ISSUE_SESSION_STATE)) {
        result.session_event = 0;
        result.session_state = 0;
    }
    return result;
}
Reply copy_reply(const Reply& input) {
    Reply result = input;
    if (!result.type_known) result.type = 0;
    if (!result.wire_serial_known) result.wire_serial = 0;
    // Do not let a malformed caller-provided Text claim an unterminated field
    // is complete. This reads only our fixed owned value, never OEM storage.
    Text* texts[] = {&result.sender, &result.error_name};
    for (unsigned i = 0; i != 2; ++i) {
        Text& text = *texts[i];
        if (!text.known) text = Text();
        else if (!memchr(text.bytes, 0, sizeof text.bytes)) {
            text.bytes[Text::CAPACITY - 1] = 0;
            text.complete = false;
        }
    }
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

Ledger::Ledger() : pending_loss_(0), epoch_(1), next_id_(1),
                  loss_reasons_(0), initialized_(false), exhausted_(false),
                  requests_(), workers_() {
    const PreserveErrno saved;
    initialized_ = pthread_mutex_init(&mutex_, 0) == 0;
    exhausted_ = !initialized_;
}
Ledger::~Ledger() {
    const PreserveErrno saved;
    if (initialized_) pthread_mutex_destroy(&mutex_);
}

void Ledger::lose(unsigned reason) {
    loss_reasons_ |= reason;
    if (epoch_ == std::numeric_limits<uint64_t>::max()) {
        exhausted_ = true;
        loss_reasons_ |= LOSS_EPOCH_EXHAUSTED;
    } else {
        ++epoch_;
    }
    // Do not reclaim potentially live objects after a missed event. Rows stay
    // occupied but their old epochs cannot match. Only actual end/consume
    // events may release them; time never proves OEM object death.
}

Result Ledger::enter(bool cleanup, bool mark_busy_loss) {
    if (!initialized_) return EXHAUSTED;
    if (pthread_mutex_trylock(&mutex_) != 0) {
        if (mark_busy_loss)
            pending_loss_.fetch_or(LOSS_CONTENTION, std::memory_order_release);
        return BUSY;
    }
    const unsigned loss = pending_loss_.exchange(0, std::memory_order_acq_rel);
    if (loss) lose(loss);
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
    if (result == OK && pending_loss_.load(std::memory_order_acquire)) result = STALE;
    pthread_mutex_unlock(&mutex_);
    return result;
}

uint64_t Ledger::allocate_id() {
    if (next_id_ == std::numeric_limits<uint64_t>::max()) {
        lose(LOSS_ID_EXHAUSTED);
        exhausted_ = true;
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
    const uintptr_t address = reinterpret_cast<uintptr_t>(worker);
    if (worker_at(address)) { lose(LOSS_COLLISION); return leave(CONFLICT); }
    WorkerSlot* slot = worker_at(0);
    if (!slot) { lose(LOSS_CAPACITY); return leave(FULL); }
    const uint64_t id = allocate_id();
    if (!id) return leave(EXHAUSTED);
    slot->worker = address;
    slot->position = reinterpret_cast<uintptr_t>(position);
    slot->trace.request = source->token;
    slot->trace.worker.id = id;
    slot->trace.worker.epoch = epoch_;
    slot->trace.issue = source->issue;
    slot->trace.reply = source->reply;
    const Token token = slot->trace.worker;
    result = leave(OK);
    if (result == OK) *out = token;
    return result;
}

Result Ledger::worker_enter(const void* worker, WorkerContext* out) {
    const PreserveErrno saved;
    if (out) worker_leave(out);
    if (!worker || !out) return BAD_INPUT;
    Result result = enter(true);
    if (result != OK) return result;
    WorkerSlot* slot = worker_at(reinterpret_cast<uintptr_t>(worker));
    if (!slot) return leave(NOT_FOUND);
    const Trace trace = slot->trace;
    const uintptr_t position = slot->position;
    *slot = WorkerSlot(); // The actual worker was consumed, even if trace is lost.
    result = leave(exhausted_ ? EXHAUSTED : trace.worker.epoch == epoch_ ? OK : STALE);
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
    Result result = enter(true);
    if (result != OK) return result;
    WorkerSlot* slot = worker_at(reinterpret_cast<uintptr_t>(worker));
    if (!slot) return leave(NOT_FOUND); // D0->D1 or already consumed is harmless.
    *slot = WorkerSlot();
    return leave(OK);
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
    Result result = enter();
    if (result != OK) return result;
    if (trace.worker.epoch != epoch_) return leave(STALE);
    if (!position || reinterpret_cast<uintptr_t>(position) != expected)
        return leave(WRONG_POSITION);
    result = leave(OK);
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
    Result result = enter(true, false);
    if (result != OK) return result;
    Status copy = Status();
    copy.loss_epoch = epoch_;
    copy.loss_reasons = loss_reasons_;
    copy.exhausted = exhausted_;
    for (unsigned i = 0; i != REQUEST_CAPACITY; ++i) if (requests_[i].method) ++copy.requests;
    for (unsigned i = 0; i != WORKER_CAPACITY; ++i) if (workers_[i].worker) ++copy.workers;
    result = leave(OK);
    if (result == OK) *out = copy;
    return result;
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
