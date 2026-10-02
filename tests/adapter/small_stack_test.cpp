// tests/adapter/small_stack_test.cpp
//
// Regression fixture for the unchanged production preload. The pre-pool AA
// product had a large static TLS image. Original OEM thread sizes are UNKNOWN;
// figures below describe only our authored pthread. We drive the product
// DSO (OBSERVE mode, allow_assist=false) through position_enter ->
// send_vehicle_data -> position_leave on a worker whose stack we size and
// measure. No adapter implementation is linked into this caller; every call
// enters the product via resolved offsets. No OEM execution is claimed.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include "adapter/adapter.h"
#include "unwind_offsets.h"
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <link.h>
#include <pthread.h>
#include <unistd.h>

#ifdef NDEBUG
#error Small-stack regressions require assertions
#endif

namespace A = mx5::adapter;

namespace {

// ---- Unchanged product DSO access. Same RTLD_NOLOAD + dlinfo + path-match
// pattern as the existing association accessor; the DSO is already loaded at
// startup. NO adapter implementation is linked into this caller.
uintptr_t product_base;
void* product_handle;
void initialize_product_dso() {
    const char* path = std::getenv("MX5_UNWIND_LIBRARY");
    assert(path && *path);
    void* handle = dlopen(path, RTLD_NOW | RTLD_NOLOAD);
    link_map* map = 0;
    assert(handle && !dlinfo(handle, RTLD_DI_LINKMAP, &map) && map && map->l_addr);
    assert(!std::strcmp(path, map->l_name));
    product_base = uintptr_t(map->l_addr);
    product_handle = handle;
}
template<class Fn> Fn product_fn(uintptr_t off) {
    return reinterpret_cast<Fn>(product_base + off);
}
bool product_configure(A::SendFunction f, const A::Options& o) {
    return product_fn<bool(*)(A::SendFunction, const A::Options&)>(TEST_CONFIGURE)(f, o);
}
bool product_set_mode(A::Mode m) {
    return product_fn<bool(*)(A::Mode)>(TEST_MODE)(m);
}
void product_position_enter(void* mgr, const void* pos) {
    product_fn<void(*)(void*, const void*)>(TEST_POSITION_ENTER)(mgr, pos);
}
void product_position_leave() {
    product_fn<void(*)()>(TEST_POSITION_LEAVE)();
}
int32_t product_send(void* storage, A::VehicleData* data) {
    return product_fn<A::SendFunction>(TEST_VEHICLE_SEND)(storage, data);
}

// ---- Static fixture state. Everything large lives in main-owned BSS so the
// single worker's TLS/stack stays small; the sink copies full observations here
// rather than onto the worker stack.
const uint32_t kType = 1;
unsigned char raw[72];           // OEM position input; product reads it only.
unsigned char raw_copy[72];      // snapshot proving the 72 bytes are untouched.
uint8_t payload[48];             // nonzero LOCATION payload the OEM would send.
uint8_t payload_copy[48];
int session_marker;              // stands in for OEM session storage.
void* const session_storage = &session_marker;

A::VehicleData* outgoing_ptr;    // the exact wrapper pointer handed to SEND.

// Original fallback bookkeeping (counts + pointer/byte identity).
unsigned next_calls;
void* next_session;
A::VehicleData* next_data;
void* next_payload;
uint32_t next_type, next_length;
uint8_t next_bytes[48];

// Saved observations live in BSS, never on the worker stack.
A::Observation obs_position, obs_send;
unsigned position_count, send_count;

// Stack diagnostics (authored thread only).
uintptr_t g_lowest_marker;      // sampled frame addresses, not deepest SP.
uintptr_t g_fixture_frame;       // base of our 1 KB volatile worker frame.
void* worker_stack_addr;
size_t worker_stack_size, worker_guard_size;
unsigned worker_frame_touch;

void note_sp(void* p) {
    uintptr_t a = uintptr_t(p);
    if (!g_lowest_marker || a < g_lowest_marker) g_lowest_marker = a;
}

// Monotonic clock must not perturb the caller's errno.
uint64_t clock_fn(void*) { return 1000000000ULL; }

// Original fallback. OBSERVE forwards the unchanged wrapper/payload here.
int32_t next_fn(void* storage, A::VehicleData* data) {
    assert(errno == EDOM);        // input errno preserved to the fallback
    assert(data && data->type == kType && data->length == 48 && data->payload);
    ++next_calls;
    next_session = storage;
    next_data = data;
    next_payload = data ? data->payload : 0;
    next_type = data->type;
    next_length = data->length;
    std::memcpy(next_bytes, data->payload, 48);
    errno = ERANGE;               // original return errno
    return -319;                  // original result
}

// Lightweight sink: copy into BSS, preserve errno so post-return checks are not
// confused by sink-local errno writes. Also records the deepest SP marker.
void sink_fn(const A::Observation* o, void*) {
    int saved = errno;
    note_sp(__builtin_frame_address(0));
    if (o->kind == A::Observation::POSITION) { obs_position = *o; ++position_count; }
    else if (o->kind == A::Observation::SEND) { obs_send = *o; ++send_count; }
    errno = saved;
}

// A 1 KB volatile frame kept live across the entire product callback. noinline
// guarantees the frame outlives the nested product/sink calls.
__attribute__((noinline))
void worker_body() {
    volatile unsigned char frame[1024];
    g_fixture_frame = uintptr_t(&frame[0]);
    note_sp(__builtin_frame_address(0));
    for (int i = 0; i < 1024; ++i) frame[i] = uint8_t(i);

    errno = EDOM;
    product_position_enter(0, raw);
    assert(errno == EDOM);        // position_enter preserves caller errno

    A::VehicleData data = { kType, payload, 48 };
    outgoing_ptr = &data;
    errno = EDOM;                 // input errno carried into the fallback
    int32_t r = product_send(session_storage, &data);
    assert(r == -319);            // original result preserved to caller
    assert(errno == ERANGE);      // original return errno preserved to caller

    product_position_leave();
    assert(errno == ERANGE);

    unsigned sum = 0;
    for (int i = 0; i < 1024; ++i) sum += frame[i];
    worker_frame_touch = sum;     // keep the 1 KB frame live past the calls
}

void* worker_entry(void*) {
    // Verify the ACTUAL authored thread size glibc realized on the target.
    pthread_attr_t got;
    int rc = pthread_getattr_np(pthread_self(), &got);
    assert(rc == 0);
    rc = pthread_attr_getstack(&got, &worker_stack_addr, &worker_stack_size);
    assert(rc == 0);
    rc = pthread_attr_getguardsize(&got, &worker_guard_size);
    assert(rc == 0);
    rc = pthread_attr_destroy(&got);
    assert(rc == 0);
    worker_body();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    alarm(10);                    // no unbounded waits
    assert(argc == 2);
    const char* kase = argv[1];
    size_t authored = 0;
    const char* marker = 0;
    if (!std::strcmp(kase, "small16")) { authored = 16u * 1024u; marker = "small16"; }
    else if (!std::strcmp(kase, "small24")) { authored = 24u * 1024u; marker = "small24"; }
    else { assert(!"unknown case"); return 2; }

    initialize_product_dso();

    // Nonzero fields and padding make unexpected input writes observable.
    std::memset(raw,0xa5,sizeof raw);
    const int32_t input_mode=0, altitude=-23;
    const uint64_t utc=1700000000ULL;
    const double latitude=37.25,longitude=127.5,heading=42.5,speed=36.25;
    const double horizontal=1.25,vertical=2.5;
    std::memcpy(raw,&input_mode,4);std::memcpy(raw+8,&utc,8);
    std::memcpy(raw+16,&latitude,8);std::memcpy(raw+24,&longitude,8);
    std::memcpy(raw+32,&altitude,4);std::memcpy(raw+40,&heading,8);
    std::memcpy(raw+48,&speed,8);std::memcpy(raw+56,&horizontal,8);
    std::memcpy(raw+64,&vertical,8);
    std::memcpy(raw_copy, raw, sizeof raw);
    for (unsigned i = 0; i < 48; ++i) payload[i] = uint8_t(i + 1);
    std::memcpy(payload_copy, payload, sizeof payload);

    A::Options options = A::Options();
    options.sink = sink_fn;
    options.clock = clock_fn;
    options.allow_assist = false;  // OBSERVE only; never set_mode(ASSIST)

    // Keep side effects out of assert() for clarity (build is not NDEBUG).
    bool configured = product_configure(next_fn, options);
    assert(configured);
    bool moded = product_set_mode(A::OBSERVE);
    assert(moded);

    const size_t guard = 4096;
    pthread_attr_t attr;
    int rc = pthread_attr_init(&attr);
    assert(rc == 0);
    rc = pthread_attr_setguardsize(&attr, guard);
    assert(rc == 0);
    rc = pthread_attr_setstacksize(&attr, authored);
    assert(rc == 0);

    pthread_t tid;
    rc = pthread_create(&tid, &attr, worker_entry, 0);
    if(rc)std::fprintf(stderr,"authored=%lu pthread_create error=%d\n",
                       static_cast<unsigned long>(authored),rc);
    assert(rc == 0);
    rc = pthread_join(tid, 0);     // join; no leaked thread
    assert(rc == 0);
    rc = pthread_attr_destroy(&attr);  // destroy attr; no leaked attr
    assert(rc == 0);

    // Authored thread size actually realized by glibc on the pinned target.
    assert(worker_stack_size == authored);
    assert(worker_guard_size == guard);
    assert(worker_frame_touch == 4u * 255u * 256u / 2u);

    // Original fallback: exactly one original send, unchanged wrapper/payload.
    assert(next_calls == 1);
    assert(next_session == session_storage);
    assert(next_data == outgoing_ptr);
    assert(next_payload == payload);
    assert(next_type == kType && next_length == 48);
    assert(!std::memcmp(next_bytes, payload_copy, 48));

    // The 72-byte OEM input and the nonzero 48-byte payload are byte-for-byte
    // intact after the full product transaction.
    assert(!std::memcmp(raw, raw_copy, sizeof raw));
    assert(!std::memcmp(payload, payload_copy, sizeof payload));

    // Observation kinds/counts: the product ran exactly one POSITION and one
    // SEND for this call.
    assert(position_count == 1 && send_count == 1);
    assert(obs_position.kind == A::Observation::POSITION);
    assert(obs_send.kind == A::Observation::SEND);

    // Same nonzero call sequence and prediction generation on both records.
    assert(obs_position.call_sequence != 0);
    assert(obs_position.call_sequence == obs_send.call_sequence);
    assert(obs_position.prediction_generation != 0);
    assert(obs_position.prediction_generation == obs_send.prediction_generation);

    // OBSERVE routes the original.
    assert(obs_send.choice == A::ORIGINAL);
    assert(obs_send.reason == A::DISABLED && obs_send.result == -319);
    assert(obs_position.position.mode == input_mode && obs_position.position.utc_seconds == utc);
    assert(obs_position.position.latitude_deg == latitude && obs_position.position.longitude_deg == longitude);
    assert(obs_position.position.altitude_m == altitude && obs_position.position.heading_deg == heading);
    assert(obs_position.position.velocity_kmh == speed && obs_position.position.horizontal == horizontal);
    assert(obs_position.position.vertical == vertical && obs_send.has_payload);
    assert(!std::memcmp(obs_send.original,payload_copy,48));
    assert(!std::memcmp(obs_send.outgoing,payload_copy,48));

    // All provenance flags/epochs false on both records.
    const A::Provenance& pp = obs_position.provenance;
    const A::Provenance& sp = obs_send.provenance;
    assert(!pp.source_epoch && !pp.session_epoch);
    assert(!pp.exact_request && !pp.verified_lds && !pp.legacy_receiver);
    assert(!sp.source_epoch && !sp.session_epoch);
    assert(!sp.exact_request && !sp.verified_lds && !sp.legacy_receiver);

    // Stack diagnostics for the AUTHORED fixture thread only. OEM thread sizes
    // are UNKNOWN; we make no minimum-stack or physical-vehicle adequacy claim.
    // We explicitly account for OUR fixture cost (the 1 KB volatile frame plus
    // our locals), not any OEM stack.
    const uintptr_t low = uintptr_t(worker_stack_addr);
    const uintptr_t top = low + worker_stack_size;
    // Report offsets within glibc's returned span. These sparse frame markers
    // do not measure the deepest call, usable guard boundary or OEM headroom.
    assert(g_lowest_marker && g_fixture_frame);
    assert(g_lowest_marker >= low && g_lowest_marker < top);
    assert(g_fixture_frame >= low && g_fixture_frame+1024 <= top);
    assert(g_lowest_marker <= g_fixture_frame);
    const unsigned long used = (unsigned long)(top - g_lowest_marker);
    const unsigned long margin = (unsigned long)(g_lowest_marker - low);
    const unsigned long fixture = (unsigned long)(top - g_fixture_frame);

    std::printf("PASS small stack %s: authored=%lu guard=%lu sampled_top_distance=%lu "
                "sampled_low_distance=%lu fixture_frame=%lu(incl 1024 volatile)\n",
                marker, (unsigned long)authored, (unsigned long)worker_guard_size,
                used, margin, fixture);
    assert(dlclose(product_handle)==0); // The original startup preload remains.
    alarm(0);
    return 0;
}
