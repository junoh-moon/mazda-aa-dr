#include "adapter.h"
#include "bus_hooks.h"
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <limits>
#include <pthread.h>

namespace mx5 { namespace adapter {
namespace {
static_assert(ATOMIC_INT_LOCK_FREE == 2, "Adapter requires lock-free 32-bit control atomics");
#if defined(__arm__)
static_assert(sizeof(VehicleData) == 12, "OEM wrapper is ARM32 only");
static_assert(offsetof(VehicleData, payload) == 4, "OEM payload offset");
static_assert(offsetof(VehicleData, length) == 8, "OEM length offset");
#endif
static_assert(sizeof(double) == 8, "OEM double width");

struct Context {
    uint32_t sequence, generation, location_count;
    int32_t original_mode;
    Provenance provenance;
    runtime::request_trace::Result request_result;
    runtime::request_trace::Trace request_trace;
    runtime::lds_association::Owned lds_association;
    bool decoded;
};
// Each active POSITION owns exactly one slot. A flat pool preserves the full
// request trace across the OEM call without allocating it on a small OEM stack
// or reserving eight large frames in every thread's initial-exec TLS image.
enum { CONTEXT_DEPTH_LIMIT = 8, CONTEXT_SLOT_COUNT = 64 };
struct ContextSlot { uint32_t occupied; Context frame; };
static ContextSlot context_slots[CONTEXT_SLOT_COUNT];
struct FailureContext { uint32_t sequence, generation; int32_t mode; };
struct ThreadState {
    uint32_t depth, send_depth;
    ContextSlot* slots[CONTEXT_DEPTH_LIMIT];
    // Retain the first overflow frame's identity for its corresponding SEND.
    FailureContext failures[CONTEXT_DEPTH_LIMIT+1];
    bool unavailable[CONTEXT_DEPTH_LIMIT];
};
// The shim must be loaded at process startup; only bounded slot references
// and counters live in initial-exec TLS. No dynamic allocation occurs in hooks.
static __thread ThreadState tls __attribute__((tls_model("initial-exec")));
void context_after_fork() {
    // fork copies the process pool, but only the calling thread survives.
    // Keep its live frames and reclaim orphan slots in the child's private
    // copy. This is pool bookkeeping, not general post-fork OEM readiness.
    const unsigned depth=tls.depth<CONTEXT_DEPTH_LIMIT?tls.depth:unsigned(CONTEXT_DEPTH_LIMIT);
    for(unsigned i=0;i<CONTEXT_SLOT_COUNT;++i) {
        bool retained=false;
        for(unsigned n=0;n<depth;++n)
            if(tls.slots[n]==&context_slots[i])retained=true;
        if(!retained)__atomic_store_n(&context_slots[i].occupied,0,__ATOMIC_RELEASE);
    }
    // A copied parent prediction is not a child-process candidate. Keep the
    // captured raw frame for forwarding, but revoke its selection generation.
    invalidate();
}
ContextSlot* acquire_context_slot() {
    // One bounded pass: if every claim loses, retain OEM forwarding and report
    // unavailable observation instead of waiting inside an OEM callback.
    for (unsigned i=0;i<CONTEXT_SLOT_COUNT;++i) {
        uint32_t expected=0;
        if (__atomic_compare_exchange_n(&context_slots[i].occupied,&expected,1,
                false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))
            return &context_slots[i];
    }
    return 0;
}
struct SendScope {
    SendScope() { ++tls.send_depth; }
    ~SendScope() { --tls.send_depth; }
};
static SendFunction next_send = 0;
static Options options = Options();
static bool configured = false; // Written before producers start, then immutable.
static std::atomic<unsigned> run_mode(OBSERVE), prediction_generation(1), sequence(0);
static std::atomic<int> previous_mode(-1);
static std::atomic<unsigned> fault(0);
static pthread_mutex_t snapshot_mutex = PTHREAD_MUTEX_INITIALIZER;
static DrSnapshot candidate = DrSnapshot();

uint32_t get32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint64_t get64(const uint8_t* p) {
    return uint64_t(get32(p)) | (uint64_t(get32(p + 4)) << 32);
}
double get_double(const uint8_t* p) {
    const uint64_t bits = get64(p); double value;
    std::memcpy(&value, &bits, sizeof value); return value;
}
void put32(uint8_t* p, uint32_t v) {
    for (unsigned i = 0; i != 4; ++i) p[i] = uint8_t(v >> (8 * i));
}
void put64(uint8_t* p, uint64_t v) {
    for (unsigned i = 0; i != 8; ++i) p[i] = uint8_t(v >> (8 * i));
}
bool scaled(double v, double scale, int32_t* out) {
    if (!std::isfinite(v)) return false;
    const double rounded = ::round(v * scale);
    if (!std::isfinite(rounded) || rounded < -2147483648.0 || rounded > 2147483647.0)
        return false;
    *out = static_cast<int32_t>(rounded); return true;
}
uint64_t now() { return options.clock ? options.clock(options.user) : 0; }
void emit(const Observation& event) {
    if (options.sink) options.sink(&event, options.user);
}
struct ObservationCompletion {
    bool complete;
    ObservationCompletion():complete(false) {}
    ~ObservationCompletion() {
        // A thrown callback or cancelled OEM send cannot yield a journal row.
        // Leave a sticky failure even though the veneer still frees its slot.
        if(!complete) {
            fault.store(1,std::memory_order_release);
            invalidate();
        }
    }
};
void unavailable_position(const void* input) {
    fault.store(1,std::memory_order_release);
    invalidate();
    Observation event=Observation();
    event.kind=Observation::POSITION;event.choice=ORIGINAL;
    event.reason=CONTEXT_UNAVAILABLE;
    event.request_result=runtime::request_trace::NOT_FOUND;
    if(options.request_reader)
        event.request_result=options.request_reader(input,&event.request_trace,options.user);
    if(event.request_result!=runtime::request_trace::OK)
        event.request_trace=runtime::request_trace::Trace();
    event.call_sequence=sequence.fetch_add(1,std::memory_order_relaxed)+1;
    const bool decoded=decode_position(input,&event.position);
    event.original_mode=decoded?event.position.mode:-1;
    previous_mode.exchange(event.original_mode,std::memory_order_acq_rel);
    event.prediction_generation=generation();event.mono_ns=now();
    if(tls.depth<=CONTEXT_DEPTH_LIMIT+1)
        tls.failures[tls.depth-1]={event.call_sequence,event.prediction_generation,event.original_mode};
    if(mode()!=OFF)emit(event);
}
Context* context() {
    if (!tls.depth || tls.depth>CONTEXT_DEPTH_LIMIT) return 0;
    ContextSlot* slot=tls.slots[tls.depth-1];
    return slot?&slot->frame:0;
}
bool association_matches(const PositionContext& c,const runtime::lds_association::Owned& value) {
    namespace L=runtime::lds_association;
    return c.request_result==runtime::request_trace::OK &&
        value.result==L::MATCHED_LOCKED_FOR_SEND && value.stage==L::LOCKED_FOR_SEND &&
        value.call_sequence==c.call_sequence && value.prediction_generation==c.prediction_generation &&
        value.view_revision && value.layout_version==1 && value.source_instance && value.record_sequence &&
        value.locked_observed_ns && value.request_id && value.request_epoch && value.worker_id && value.worker_epoch &&
        value.request_id==c.request_trace.request.id && value.request_epoch==c.request_trace.request.epoch &&
        value.worker_id==c.request_trace.worker.id && value.worker_epoch==c.request_trace.worker.epoch;
}
Reason choose_dr(Context& ctx, uint64_t time,
                 const runtime::session_trace::Snapshot& session, uint8_t bytes[48]) {
    if (!options.allow_assist || !options.clock || !ctx.provenance.exact_request ||
        !ctx.provenance.verified_lds || !ctx.provenance.legacy_receiver)
        return BAD_PROVENANCE;
    // Negative lifecycle guard only: an observed handle grants no provenance.
    // A candidate published inside a lifecycle call must not become selectable
    // just because it carries the newly revoked generation.
    if (options.session_reader && session.result != runtime::session_trace::OBSERVED)
        return EPOCH_MISMATCH;
    if (pthread_mutex_trylock(&snapshot_mutex) != 0) return LOCK_BUSY;
    const DrSnapshot s = candidate;
    pthread_mutex_unlock(&snapshot_mutex);
    const uint32_t live_generation = prediction_generation.load(std::memory_order_acquire);
    if (ctx.generation != live_generation || s.prediction_generation != live_generation ||
        s.source_epoch != ctx.provenance.source_epoch ||
        s.session_epoch != ctx.provenance.session_epoch) return EPOCH_MISMATCH;
    if (!s.ready || !s.profile_verified || !s.input_quality_verified || !s.limits_ok)
        return NOT_READY;
    if (time < s.frontier_mono_ns || time > s.valid_until_mono_ns ||
        time - s.frontier_mono_ns > options.max_snapshot_age_ns) return EXPIRED;
    if (!encode_location(s, bytes)) return BAD_ENCODING;
    // This is the selection linearization check. It does not synchronize OEM teardown.
    if (prediction_generation.load(std::memory_order_acquire) != live_generation)
        return EPOCH_MISMATCH;
    return PASS;
}
}

bool configure(SendFunction next, const Options& opt) {
    if (configured || !next || next == &send_vehicle_data ||
        next == &mx5_send_vehicle_data) return false;
    if (opt.allow_assist && (!opt.clock || !opt.provenance || !opt.max_snapshot_age_ns))
        return false;
    const int saved_errno=errno;
    // Best effort at initialization, outside OEM hooks. If libc cannot
    // register the handler, a fork child may retain orphan slots; exhaustion
    // still records CONTEXT_UNAVAILABLE and forwards the OEM call unchanged.
    (void)pthread_atfork(0,0,context_after_fork);
    errno=saved_errno;
    next_send = next; options = opt; configured = true; return true;
}
bool prepare_bus_hooks(const BusBindings& bindings) {
    return prepare_bus_hooks(bindings,&invalidate);
}
bool set_mode(Mode requested) {
    if (requested < OFF || requested > ASSIST ||
        (requested == ASSIST && !options.allow_assist)) return false;
    invalidate();
    run_mode.store(static_cast<unsigned>(requested), std::memory_order_release);
    return true;
}
Mode mode() { return static_cast<Mode>(run_mode.load(std::memory_order_acquire)); }
uint32_t invalidate() {
    unsigned old = prediction_generation.fetch_add(1, std::memory_order_acq_rel);
    // Never allow wraparound to revive a previous generation.
    if (old == std::numeric_limits<unsigned>::max()) fault.store(1, std::memory_order_release);
    return old + 1;
}
uint32_t invalidate_if_generation(uint32_t owned) {
    if (!owned || owned == std::numeric_limits<unsigned>::max()) {
        fault.store(1, std::memory_order_release);return 0;
    }
    unsigned current=prediction_generation.load(std::memory_order_acquire);
    for (;;) {
        if (current>owned) return current;
        if (current<owned) return 0;
        if (prediction_generation.compare_exchange_weak(current,owned+1,
                std::memory_order_acq_rel,std::memory_order_acquire)) return owned+1;
    }
}
uint32_t generation() { return prediction_generation.load(std::memory_order_acquire); }
bool faulted() { return fault.load(std::memory_order_acquire)!=0; }
bool publish_snapshot(const DrSnapshot& snapshot) {
    if (!configured || snapshot.prediction_generation != generation() ||
        fault.load(std::memory_order_acquire)) return false;
    pthread_mutex_lock(&snapshot_mutex); // Worker API, never used by the hook.
    const bool current = snapshot.prediction_generation == generation();
    if (current) candidate = snapshot;
    pthread_mutex_unlock(&snapshot_mutex);
    return current;
}
bool decode_position(const void* raw, PositionInput* out) {
    if (!raw || !out) return false;
    const uint8_t* p = static_cast<const uint8_t*>(raw);
    // Borrowed OEM input: exact verified call ABI, not arbitrary-pointer probing.
    out->mode = static_cast<int32_t>(get32(p)); out->utc_seconds = get64(p + 8);
    out->latitude_deg = get_double(p + 16); out->longitude_deg = get_double(p + 24);
    out->altitude_m = static_cast<int32_t>(get32(p + 32));
    out->heading_deg = get_double(p + 40); out->velocity_kmh = get_double(p + 48);
    out->horizontal = get_double(p + 56); out->vertical = get_double(p + 64);
    return true;
}
bool encode_location(const DrSnapshot& in, uint8_t out[48]) {
    if (!out || !std::isfinite(in.latitude_deg) || in.latitude_deg < -90 ||
        in.latitude_deg > 90 || !std::isfinite(in.longitude_deg) ||
        !std::isfinite(in.speed_mps) || in.speed_mps < 0 || !in.derived_utc_ns)
        return false;
    double lon = std::fmod(in.longitude_deg + 180.0, 360.0);
    if (lon < 0) lon += 360.0;
    lon -= 180.0;
    int32_t lat_e7, lon_e7, speed_e3, bearing_e6 = 0;
    if (!scaled(in.latitude_deg, 1e7, &lat_e7) || !scaled(lon, 1e7, &lon_e7) ||
        !scaled(in.stopped ? 0.0 : in.speed_mps, 1000, &speed_e3)) return false;
    if (!in.stopped) {
        if (!std::isfinite(in.travel_bearing_deg)) return false;
        double bearing = std::fmod(in.travel_bearing_deg, 360.0);
        if (bearing < 0) bearing += 360.0;
        if (!scaled(bearing, 1e6, &bearing_e6)) return false;
        if (bearing_e6 == 360000000) bearing_e6 = 0;
    }
    if (lon_e7 == 1800000000) lon_e7 = -1800000000;
    std::memset(out, 0, 48);
    put64(out, in.derived_utc_ns); put32(out + 8, uint32_t(lat_e7));
    put32(out + 12, uint32_t(lon_e7)); out[32] = 1;
    put32(out + 36, uint32_t(speed_e3)); out[40] = in.stopped ? 0 : 1;
    put32(out + 44, uint32_t(bearing_e6));
    return true;
}
void position_enter(void* manager, const void* input) {
    const int saved_errno = errno;
    ++tls.depth;
    if (tls.depth<=CONTEXT_DEPTH_LIMIT) {
        tls.slots[tls.depth-1]=0;
        tls.failures[tls.depth-1]=FailureContext();
        tls.unavailable[tls.depth-1]=false;
    }
    if (!configured) { errno = saved_errno; return; }
    if (tls.depth>CONTEXT_DEPTH_LIMIT) {
        if(tls.depth==CONTEXT_DEPTH_LIMIT+1)
            tls.failures[tls.depth-1]=FailureContext();
        unavailable_position(input);errno=saved_errno;return;
    }
    ContextSlot* slot=acquire_context_slot();
    if (!slot) {
        tls.unavailable[tls.depth-1]=true;
        unavailable_position(input);
        errno=saved_errno;return;
    }
    tls.slots[tls.depth-1]=slot;
    ObservationCompletion completion_guard;
    Context& ctx = slot->frame;
    std::memset(&ctx, 0, sizeof ctx);
    Observation event = Observation();
    event.kind = Observation::POSITION;
    ctx.request_result = runtime::request_trace::NOT_FOUND;
    if (options.request_reader)
        ctx.request_result = options.request_reader(input, &ctx.request_trace, options.user);
    if (ctx.request_result != runtime::request_trace::OK)
        ctx.request_trace = runtime::request_trace::Trace();
    event.request_result = ctx.request_result;
    event.request_trace = ctx.request_trace;
    ctx.sequence = sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    ctx.decoded = decode_position(input, &event.position);
    ctx.original_mode = ctx.decoded ? event.position.mode : -1;
    // Revoke before queuing the observation: async estimator cannot preserve an
    // old DR_ACTIVE candidate across GPS/native-DR return and another outage.
    const int before = previous_mode.exchange(ctx.original_mode, std::memory_order_acq_rel);
    if (before != ctx.original_mode) invalidate();
    ctx.generation = generation();
    if (ctx.decoded) {
        const PositionContext input_context={event.position,ctx.request_result,
            ctx.request_trace,ctx.sequence,ctx.generation,0};
        if (options.association_reader) {
            const bool matched=options.association_reader(input_context,&ctx.lds_association,options.user);
            if (!matched || !association_matches(input_context,ctx.lds_association)) {
                namespace L=runtime::lds_association;
                const L::Result result=matched?L::CONFLICT:ctx.lds_association.result;
                ctx.lds_association=L::Owned();
                if(result==L::CONFLICT || result==L::PAYLOAD_MISMATCH)ctx.lds_association.result=result;
            }
        }
        if (options.provenance) {
            const PositionContext qualified_context={event.position,ctx.request_result,
                ctx.request_trace,ctx.sequence,ctx.generation,&ctx.lds_association};
            if (!options.provenance(manager,qualified_context,&ctx.provenance,options.user))
                ctx.provenance = Provenance();
        }
    }
    event.call_sequence = ctx.sequence; event.prediction_generation = ctx.generation;
    event.original_mode = ctx.original_mode; event.provenance = ctx.provenance;
    event.lds_association = ctx.lds_association;
    event.mono_ns = now();
    if (mode() != OFF) emit(event);
    completion_guard.complete=true;
    errno = saved_errno;
}
void position_aborted() {
    const int saved_errno=errno;
    fault.store(1,std::memory_order_release);
    invalidate();
    errno=saved_errno;
}
void position_leave() {
    const int saved_errno = errno;
    if (!tls.depth) {
        fault.store(1,std::memory_order_release);invalidate();
    } else if (tls.depth>CONTEXT_DEPTH_LIMIT) {
        if(tls.depth==CONTEXT_DEPTH_LIMIT+1)
            tls.failures[tls.depth-1]=FailureContext();
        --tls.depth;
    } else {
        const unsigned index=tls.depth-1;
        ContextSlot* slot=tls.slots[index];
        tls.slots[index]=0;tls.failures[index]=FailureContext();tls.unavailable[index]=false;
        --tls.depth;
        // TLS no longer references the frame before another thread may claim
        // it. The acquire CAS above observes this release after our last read.
        if(slot)__atomic_store_n(&slot->occupied,0,__ATOMIC_RELEASE);
    }
    errno = saved_errno;
}
int32_t send_vehicle_data(void* session_storage, VehicleData* data) {
    const int entry_errno = errno;
    // configure() is an installation precondition, not a fallback-return policy.
    // An unconfigured hook is never installed by install_v74().
    SendFunction next = next_send;
    if (!next) __builtin_trap();
    const SendScope send_scope;
    ObservationCompletion completion_guard;
    const bool reentrant = tls.send_depth > 1;
    Observation event = Observation();
    event.kind = Observation::SEND; event.original_mode = -1;
    if(options.session_reader)
        options.session_reader(session_storage,&event.send_session,options.user);
    event.request_result = runtime::request_trace::NOT_FOUND;
    event.choice = ORIGINAL;
    const bool pool_unavailable=tls.depth && (tls.depth>CONTEXT_DEPTH_LIMIT ||
        tls.unavailable[tls.depth-1]);
    event.reason = pool_unavailable?CONTEXT_UNAVAILABLE:NO_CONTEXT;
    if(pool_unavailable && tls.depth<=CONTEXT_DEPTH_LIMIT+1) {
        const FailureContext& failed=tls.failures[tls.depth-1];
        event.call_sequence=failed.sequence;
        event.prediction_generation=failed.generation;
        event.original_mode=failed.mode;
    }
    Context* ctx = context();
    if (ctx) {
        event.call_sequence = ctx->sequence; event.original_mode = ctx->original_mode;
        event.prediction_generation = ctx->generation; event.provenance = ctx->provenance;
        event.request_result = ctx->request_result; event.request_trace = ctx->request_trace;
        event.lds_association = ctx->lds_association;
    }
    uint8_t replacement[48];
    VehicleData local = VehicleData();
    VehicleData* selected = data;
    const Mode current = mode();
    if (data) {
        event.type = data->type; event.length = data->length;
        if (data->type == 1 && ctx) {
            ++ctx->location_count;
            if (ctx->location_count > 1) {
                fault.store(1, std::memory_order_release); invalidate();
                event.reason = EXTRA_LOCATION;
            }
        }
        if (data->type == 1 && data->length == 48 && data->payload) {
            event.has_payload = true;
            std::memcpy(event.original, data->payload, 48);
            std::memcpy(event.outgoing, data->payload, 48);
            if (!ctx || !ctx->decoded)
                event.reason = pool_unavailable?CONTEXT_UNAVAILABLE:NO_CONTEXT;
            else if (ctx->location_count > 1) event.reason = EXTRA_LOCATION;
            else if (reentrant || tls.depth != 1) event.reason = NESTED_CALL;
            else if (ctx->original_mode != 0) event.reason = NOT_UNKNOWN;
            else if (fault.load(std::memory_order_acquire) || current < SCRUB_STALE)
                event.reason = DISABLED;
            else if (current == SCRUB_STALE) {
                std::memcpy(replacement, data->payload, 48);
                replacement[32] = 0; replacement[40] = 0;
                std::memset(replacement + 36, 0, 4);
                std::memset(replacement + 44, 0, 4);
                event.choice = SCRUBBED; event.reason = PASS;
            } else {
                event.mono_ns = now();
                event.reason = choose_dr(*ctx, event.mono_ns, event.send_session, replacement);
                if (event.reason == PASS) event.choice = DR_REPLACEMENT;
            }
            if (event.choice != ORIGINAL) {
                local = *data; local.payload = replacement; selected = &local;
                std::memcpy(event.outgoing, replacement, 48);
            }
        } else if (data->type == 1)
            event.reason = pool_unavailable?CONTEXT_UNAVAILABLE:BAD_LENGTH;
    }
    errno = entry_errno;
    const int32_t result = next(session_storage, selected); // Exactly one call.
    const int result_errno = errno;
    event.result = result;
    if (!event.mono_ns) event.mono_ns = now();
    if (current != OFF && !reentrant) emit(event);
    completion_guard.complete=true;
    errno = result_errno;
    return result;
}

} }

extern "C" void mx5_position_enter(void* m, const void* p) { mx5::adapter::position_enter(m, p); }
extern "C" void mx5_position_leave() { mx5::adapter::position_leave(); }
extern "C" int32_t mx5_send_vehicle_data(void* s, mx5::adapter::VehicleData* d) {
    return mx5::adapter::send_vehicle_data(s, d);
}
