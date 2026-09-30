#include "request_observer.h"
#include <errno.h>

namespace mx5 { namespace runtime { namespace request_trace {
namespace {
// Startup-loaded only: no dynamic TLS allocation on an OEM callback.
__thread ReplyScope* reply_scope __attribute__((tls_model("initial-exec")));
__thread WorkerScope* worker_scope __attribute__((tls_model("initial-exec")));
struct PreserveErrno {
    const int value;
    PreserveErrno() : value(errno) {}
    ~PreserveErrno() { errno = value; }
};
struct ReplyConstruction {
    ReplyScope* previous;
    bool complete;
    ~ReplyConstruction() { if(!complete)reply_scope=previous; }
};
}

Observer::Observer(const ReplyApi& api, ObservationClock clock, void* user)
    : api_(api), clock_(clock), clock_user_(user) {}

bool Observer::valid() const {
    return api_.get_reply && api_.get_type && api_.get_sender && api_.get_error
        && api_.get_reply_serial;
}
uint64_t Observer::now() const { return clock_ ? clock_(clock_user_) : 0; }

Result Observer::request_begin(void* method, Token* out, const session_trace::Snapshot& context) {
    const PreserveErrno saved;
    if (out) *out = Token();
    if (!valid()) return BAD_INPUT;
    Issue issue = Issue();
    issue.observed_ns = now();
    issue.session_context = context;
    // No ambient value is promoted into a request/receiver ownership claim.
    return ledger_.request_begin(method, issue, out);
}
Result Observer::request_end(void* method) { return ledger_.request_end(method); }

Reply Observer::read_reply(void* method) const {
    const PreserveErrno saved;
    Reply copy = Reply();
    copy.observed_ns = now();
    if (!method || !valid()) return copy;
    void* reply = api_.get_reply(method);
    if (!reply) return copy;
    copy.type = api_.get_type(reply);
    copy.type_known = copy.type == 1 || copy.type == 2;
    copy.sender = copy_text(api_.get_sender(reply));
    copy.error_name = copy_text(api_.get_error(reply));
    uint32_t serial = 0;
    // The original getter can fail when the backing DBus message is absent.
    // Never infer a wire serial from a method address or callback ordering.
    if (api_.get_reply_serial(reply, &serial) == 0 && serial) {
        copy.wire_serial = serial;
        copy.wire_serial_known = true;
    }
    return copy;
}

ReplyScope::ReplyScope(Observer& owner, void* method)
    : owner_(&owner), previous_(reply_scope), token_(), result_(BAD_INPUT) {
    const PreserveErrno saved;
    ReplyConstruction construction={previous_,false};
    reply_scope = this; // Mask the outer request even on NOT_FOUND/BUSY.
    if (owner.valid())
        result_ = owner.ledger_.reply_enter(method, owner.read_reply(method), &token_);
    construction.complete=true;
}
ReplyScope::~ReplyScope() {
    const PreserveErrno saved;
    reply_scope = previous_;
}

Result Observer::worker_post(void* worker, const void* position, Token* out) {
    const PreserveErrno saved;
    if (out) *out = Token();
    if (!worker || !position || !out) return BAD_INPUT;
    if (!reply_scope || reply_scope->owner_ != this) return NOT_FOUND;
    if (reply_scope->result_ != OK) return reply_scope->result_;
    return ledger_.worker_post(worker, position, reply_scope->token_, out);
}
Result Observer::worker_destroy(void* worker) { return ledger_.worker_destroy(worker); }

WorkerScope::WorkerScope(Observer& owner, void* worker)
    : owner_(&owner), previous_(worker_scope), context_(), result_(BAD_INPUT) {
    const PreserveErrno saved;
    worker_scope = this;
    result_ = owner.ledger_.worker_enter(worker, &context_);
}
WorkerScope::~WorkerScope() {
    const PreserveErrno saved;
    owner_->ledger_.worker_leave(&context_);
    worker_scope = previous_;
}
Result Observer::position_take(const void* position, Trace* out) {
    const PreserveErrno saved;
    if (out) *out = Trace();
    if (!out) return BAD_INPUT;
    if (!worker_scope || worker_scope->owner_ != this) return NOT_FOUND;
    if (worker_scope->result_ != OK) return worker_scope->result_;
    return ledger_.position_take(&worker_scope->context_, position, out);
}
Result Observer::status(Status* out) { return ledger_.status(out); }

} } }
