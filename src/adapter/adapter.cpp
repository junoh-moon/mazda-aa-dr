#include "adapter.h"
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
    bool decoded;
};
struct ThreadState { uint32_t depth, send_depth; Context frames[8]; };
// The shim must be loaded at process startup; no dynamic TLS allocation in hooks.
static __thread ThreadState tls __attribute__((tls_model("initial-exec")));
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
Context* context() {
    return tls.depth && tls.depth <= 8 ? &tls.frames[tls.depth - 1] : 0;
}
Reason choose_dr(Context& ctx, uint64_t time, uint8_t bytes[48]) {
    if (!options.allow_assist || !options.clock || !ctx.provenance.exact_request ||
        !ctx.provenance.verified_lds || !ctx.provenance.legacy_receiver)
        return BAD_PROVENANCE;
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
    next_send = next; options = opt; configured = true; return true;
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
uint32_t generation() { return prediction_generation.load(std::memory_order_acquire); }
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
    if (!configured || tls.depth > 8) { errno = saved_errno; return; }
    Context& ctx = tls.frames[tls.depth - 1];
    std::memset(&ctx, 0, sizeof ctx);
    Observation event = Observation();
    event.kind = Observation::POSITION;
    ctx.sequence = sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    ctx.decoded = decode_position(input, &event.position);
    ctx.original_mode = ctx.decoded ? event.position.mode : -1;
    // Revoke before queuing the observation: async estimator cannot preserve an
    // old DR_ACTIVE candidate across GPS/native-DR return and another outage.
    const int before = previous_mode.exchange(ctx.original_mode, std::memory_order_acq_rel);
    if (before != ctx.original_mode) invalidate();
    ctx.generation = generation();
    if (ctx.decoded && options.provenance &&
        !options.provenance(manager, &event.position, &ctx.provenance, options.user))
        ctx.provenance.exact_request = false;
    event.call_sequence = ctx.sequence; event.prediction_generation = ctx.generation;
    event.original_mode = ctx.original_mode; event.provenance = ctx.provenance;
    event.mono_ns = now();
    if (mode() != OFF) emit(event);
    errno = saved_errno;
}
void position_leave() {
    const int saved_errno = errno;
    if (tls.depth) --tls.depth;
    errno = saved_errno;
}
int32_t send_vehicle_data(void* session_storage, VehicleData* data) {
    const int entry_errno = errno;
    // configure() is an installation precondition, not a fallback-return policy.
    // An unconfigured hook is never installed by install_v74().
    SendFunction next = next_send;
    if (!next) __builtin_trap();
    ++tls.send_depth;
    const bool reentrant = tls.send_depth > 1;
    Observation event = Observation();
    event.kind = Observation::SEND; event.original_mode = -1;
    event.choice = ORIGINAL; event.reason = NO_CONTEXT;
    Context* ctx = context();
    if (ctx) {
        event.call_sequence = ctx->sequence; event.original_mode = ctx->original_mode;
        event.prediction_generation = ctx->generation; event.provenance = ctx->provenance;
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
            if (!ctx || !ctx->decoded) event.reason = NO_CONTEXT;
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
                event.reason = choose_dr(*ctx, event.mono_ns, replacement);
                if (event.reason == PASS) event.choice = DR_REPLACEMENT;
            }
            if (event.choice != ORIGINAL) {
                local = *data; local.payload = replacement; selected = &local;
                std::memcpy(event.outgoing, replacement, 48);
            }
        } else if (data->type == 1) event.reason = BAD_LENGTH;
    }
    errno = entry_errno;
    const int32_t result = next(session_storage, selected); // Exactly one call.
    const int result_errno = errno;
    event.result = result;
    if (!event.mono_ns) event.mono_ns = now();
    if (current != OFF && !reentrant) emit(event);
    --tls.send_depth;
    errno = result_errno;
    return result;
}

} }

extern "C" void mx5_position_enter(void* m, const void* p) { mx5::adapter::position_enter(m, p); }
extern "C" void mx5_position_leave() { mx5::adapter::position_leave(); }
extern "C" int32_t mx5_send_vehicle_data(void* s, mx5::adapter::VehicleData* d) {
    return mx5::adapter::send_vehicle_data(s, d);
}
