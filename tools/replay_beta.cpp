// BETA replay harness (validation/ASSIST_BETA_DESIGN_2026-10-05.md task T6).
//
// Drives the REAL product BETA path from a vehicle-style journal directory:
//   motion_batch rows        -> navigation::Pipeline (MODEL + BETA core),
//   POSITION/LOCATION calls  -> adapter::position_enter/send_vehicle_data/
//                               position_leave with a FAKE OEM send,
//   50 ms worker ticks       -> Pipeline::drain + runtime::BetaController
//                               (map_model_publication, publish_snapshot).
// The runtime glue it reproduces (provenance, send-session counter, hold
// events) uses the same inline helpers as src/runtime/runtime.cpp. What is
// NOT reproduced: worker threads/queues, the journal writer, ModelSession and
// ModelBus fences (always admitted here), LDS association, real OEM timing.
//
// Positions come from adapter "position"/"send" rows when present (hook
// installed), else from collector "position_poll" rows of the same boot.
// PSEUDO-OUTAGE windows: from T0 for D seconds the recorded callbacks are
// suppressed and synthetic mode-0 callbacks (frozen last fix, accuracy
// cleared, 99/99) are issued every --cadence-ms while the recording still
// has GPS. Each replaced LOCATION is compared with the recorded GPS. Each
// window runs in a fork()ed copy of the replay state at T0, so windows are
// independent and a crash inside the product path is reported, not hidden.
// Recorded mode-0 callbacks (real outages) are replayed as recorded.
//
// GPS is not ground truth and receipt time is not fix time. This is offline
// MODEL evidence only; it is not vehicle, phone or DHU validation.
#include "adapter/adapter.h"
#include "adapter/session_hooks.h"
#include "navigation/pipeline.h"
#include "runtime/beta_controller.h"
#include "runtime/beta_profile.h"
#include "runtime/core_bridge.h"
#include "runtime/motion_gap.h"
#include "runtime/worker_tick.h"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <map>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace A = mx5::adapter;
namespace N = mx5::navigation;
namespace R = mx5::runtime;

namespace {

const double PI = 3.14159265358979323846;
const uint64_t TICK_NS = 50000000ULL;      // runtime WorkerTick cadence
const uint64_t NONE = UINT64_MAX;

// ---------------------------------------------------------------- input ----
struct Raw {
    uint64_t t;          // ordering time (received_ns, or reset time)
    bool reset;          // shadow_input_reset: runtime resets the pipeline
    N::RawEvent event;
    // The motion_rejected row before the reset (task E gap rule), if any.
    bool rejected_decoded;
    N::ReceiveFault rejected_reason;
    N::RawEvent rejected;
};
struct Fix {
    uint64_t t;
    int32_t mode;
    uint64_t utc;
    double lat, lon, heading, kmh, horizontal, vertical;
    int32_t altitude;
    bool has_send, has_original;
    uint8_t original[48];
};
struct Wheel { uint64_t t; double mps; };

struct Input {
    std::vector<Raw> raws;
    std::vector<Fix> calls;   // OEM POSITION callbacks in time order
    std::vector<Fix> truth;   // recorded valid GPS (mode 1/2, utc, finite)
    std::vector<Wheel> wheels;
    std::string boot_id, position_source;
    unsigned files, input_resets, motion_events;
};

// Flat key lookup for the product's compact JSONL rows (whitespace after the
// colon is tolerated). Callers bound the search before nested objects.
bool key_pos(const std::string& l, const char* key, size_t limit, size_t* at) {
    const std::string k = std::string("\"") + key + "\"";
    for (size_t p = l.find(k); p != std::string::npos && p < limit; p = l.find(k, p + 1)) {
        size_t q = p + k.size();
        while (q < l.size() && l[q] == ' ') ++q;
        if (q >= l.size() || l[q] != ':') continue;
        ++q;
        while (q < l.size() && l[q] == ' ') ++q;
        *at = q;
        return true;
    }
    return false;
}
bool str_field(const std::string& l, const char* key, std::string* out,
               size_t limit = std::string::npos) {
    size_t p;
    if (!key_pos(l, key, limit, &p) || p >= l.size() || l[p] != '"') return false;
    const size_t e = l.find('"', p + 1);
    if (e == std::string::npos) return false;
    *out = l.substr(p + 1, e - p - 1);
    return true;
}
bool num_field(const std::string& l, const char* key, double* out,
               size_t limit = std::string::npos) {
    size_t p;
    if (!key_pos(l, key, limit, &p)) return false;
    if (!l.compare(p, 4, "null")) { *out = NAN; return true; }
    char* end = 0;
    *out = strtod(l.c_str() + p, &end);
    return end != l.c_str() + p;
}
bool u64_field(const std::string& l, const char* key, uint64_t* out,
               size_t limit = std::string::npos) {
    size_t p;
    if (!key_pos(l, key, limit, &p)) return false;
    char* end = 0;
    *out = strtoull(l.c_str() + p, &end, 10);
    return end != l.c_str() + p;
}
bool i64_field(const std::string& l, const char* key, int64_t* out,
               size_t limit = std::string::npos) {
    size_t p;
    if (!key_pos(l, key, limit, &p)) return false;
    char* end = 0;
    *out = strtoll(l.c_str() + p, &end, 10);
    return end != l.c_str() + p;
}
std::string kind_of(const std::string& l) {
    std::string k;
    str_field(l, "kind", &k);
    return k;
}
bool hex48(const std::string& s, uint8_t out[48]) {
    if (s.size() != 96) return false;
    for (unsigned i = 0; i < 48; ++i) {
        unsigned v;
        if (sscanf(s.c_str() + 2 * i, "%2x", &v) != 1) return false;
        out[i] = uint8_t(v);
    }
    return true;
}
void put32(uint8_t* p, uint32_t v) { for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i)); }
void put64(uint8_t* p, uint64_t v) { put32(p, uint32_t(v)); put32(p + 4, uint32_t(v >> 32)); }
void putd(uint8_t* p, double v) { uint64_t b; memcpy(&b, &v, 8); put64(p, b); }
uint32_t get32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
int32_t e(double v, double scale) { return int32_t(lround(v * scale)); }

// OEM-style LOCATION payload for a collector-poll recording (no original
// bytes were captured). Padding gets a fixed pattern so a changed padding
// byte is detectable.
void synth_original(const Fix& f, uint64_t mono, uint8_t out[48]) {
    memset(out, 0xA5, 48);
    put64(out, f.utc ? f.utc * 1000ULL + (mono / 1000000ULL) % 1000ULL : mono / 1000000ULL);
    put32(out + 8, uint32_t(e(f.lat, 1e7)));
    put32(out + 12, uint32_t(e(f.lon, 1e7)));
    out[16] = f.mode ? 1 : 0;
    put32(out + 20, f.mode ? 5000u : 0u);
    out[24] = 1;
    put32(out + 28, uint32_t(f.altitude * 1000));
    out[32] = 1;
    put32(out + 36, uint32_t(e(std::max(0.0, f.kmh / 3.6), 1000)));
    out[40] = 1;
    put32(out + 44, uint32_t(e(std::fmod(std::max(0.0, f.heading), 360.0), 1e6)));
}

std::vector<std::string> journal_files(const std::string& dir, const char* prefix) {
    std::vector<std::string> out;
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    while (struct dirent* e = readdir(d)) {
        const std::string n = e->d_name;
        if (n.compare(0, strlen(prefix), prefix) == 0 && n.size() > 6 &&
            n.compare(n.size() - 6, 6, ".jsonl") == 0)
            out.push_back(dir + "/" + n);
    }
    closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

// Rows attributed to the latest boot row of their file. Rows before any boot
// row are accepted only when a single boot is present.
struct Row { std::string boot; std::string line; };
std::vector<Row> read_rows(const std::vector<std::string>& files, const char* boot_kind,
                           std::vector<std::string>* boots) {
    std::vector<Row> rows;
    for (size_t i = 0; i < files.size(); ++i) {
        std::ifstream f(files[i].c_str());
        std::string line, boot;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] != '{') continue;
            if (kind_of(line) == boot_kind && str_field(line, "boot_id", &boot)) {
                if (std::find(boots->begin(), boots->end(), boot) == boots->end())
                    boots->push_back(boot);
            }
            Row r; r.boot = boot; r.line = line; rows.push_back(r);
        }
    }
    return rows;
}
bool same_boot(const std::string& row_boot, const std::string& want, size_t boots) {
    if (row_boot.empty()) return boots <= 1;
    return want.empty() || row_boot == want;
}

bool valid_truth(const Fix& f) {
    return (f.mode == 1 || f.mode == 2) && f.utc && std::isfinite(f.lat) && std::isfinite(f.lon) &&
           std::fabs(f.lat) < 85 && std::fabs(f.lon) <= 180 && std::isfinite(f.kmh) && f.kmh >= 0;
}

// source: "auto" (adapter rows when present, else collector polls),
// "adapter" or "poll". Polls also cover time without an AA session.
bool load(const std::string& dir, const std::string& want_boot, double poll_hdop,
          const std::string& source, Input* in, std::string* error) {
    *in = Input();
    const std::vector<std::string> trace = journal_files(dir, "trace.");
    const std::vector<std::string> collector = journal_files(dir, "collector.");
    in->files = unsigned(trace.size() + collector.size());
    if (trace.empty()) { *error = "no trace.*.jsonl in " + dir; return false; }
    std::vector<std::string> trace_boots, collector_boots;
    const std::vector<Row> rows = read_rows(trace, "boot", &trace_boots);
    std::string boot = want_boot;
    if (boot.empty()) {
        // Several boots (rotation keeps an older boot in trace.1): default to
        // the first boot row of the newest file, trace.0.jsonl.
        if (!trace_boots.empty()) boot = trace_boots[0];
    }
    in->boot_id = boot;
    std::map<uint64_t, Fix> positions;     // adapter POSITION rows by call
    Raw pending_rejected = Raw();          // last motion_rejected row
    std::map<uint64_t, std::string> sends; // LOCATION original_hex by call
    for (size_t i = 0; i < rows.size(); ++i) {
        if (!same_boot(rows[i].boot, boot, trace_boots.size())) continue;
        const std::string& l = rows[i].line;
        const std::string k = kind_of(l);
        if (k == "motion_batch") {
            uint64_t epoch = 0;
            u64_field(l, "epoch", &epoch);
            size_t p = l.find("\"events\":[");
            if (p == std::string::npos) continue;
            p += 10;
            while (p < l.size() && l[p] == '[') {
                long long v[10];
                const char* s = l.c_str() + p + 1;
                char* end = 0;
                unsigned n = 0;
                for (; n < 10; ++n) {
                    v[n] = strtoll(s, &end, 10);
                    if (end == s) break;
                    s = end;
                    if (*s == ',') ++s;
                }
                if (n != 10) { *error = "malformed motion_batch row"; return false; }
                Raw r = Raw();
                r.event.kind = N::SensorKind(v[0]);
                r.event.epoch = epoch;
                r.event.receive_seq = uint64_t(v[1]);
                r.event.received_ns = uint64_t(v[2]);
                r.event.source_mono_ms = v[3];
                for (unsigned j = 0; j < 4; ++j) r.event.raw[j] = uint16_t(v[4 + j]);
                r.event.count = uint16_t(v[8]);
                r.event.reverse = int(v[9]);
                r.t = r.event.received_ns;
                in->raws.push_back(r);
                ++in->motion_events;
                if (r.event.kind == N::WHEELS) {
                    double kmh = 0;
                    for (unsigned j = 0; j < 4; ++j) kmh += (r.event.raw[j] * 0.01 - 100.0) * 0.25;
                    Wheel w = {r.t, kmh / 3.6};
                    in->wheels.push_back(w);
                }
                const size_t close = l.find(']', p);
                if (close == std::string::npos) break;
                p = close + 1;
                if (p < l.size() && l[p] == ',') ++p;
            }
        } else if (k == "motion_rejected") {
            // Kept for the following shadow_input_reset (runtime order).
            std::string why;
            uint64_t sensor = 0, seq = 0, rec = 0, ep = 0;
            pending_rejected = Raw();
            if (str_field(l, "reason", &why) && u64_field(l, "sensor", &sensor) &&
                u64_field(l, "epoch", &ep) && u64_field(l, "receive_seq", &seq) &&
                u64_field(l, "received_ns", &rec) && l.find("\"authenticated_decoded\":true") != std::string::npos) {
                pending_rejected.rejected_decoded = true;
                pending_rejected.rejected_reason = why == "stale" ? N::RECEIVE_STALE :
                    why == "sequence_discontinuity" ? N::RECEIVE_SEQUENCE : N::RECEIVE_DECODE;
                pending_rejected.rejected.kind = N::SensorKind(sensor);
                pending_rejected.rejected.epoch = ep;
                pending_rejected.rejected.receive_seq = seq;
                pending_rejected.rejected.received_ns = rec;
            }
        } else if (k == "shadow_input_reset") {
            Raw r = pending_rejected;
            pending_rejected = Raw();
            if (!u64_field(l, "mono_ns", &r.t)) continue;
            // The runtime reads the rejected datagram in stream order, before
            // the events received after it: order the reset at its receipt.
            if (r.rejected_decoded && r.rejected.received_ns && r.rejected.received_ns < r.t)
                r.t = r.rejected.received_ns;
            r.reset = true;
            in->raws.push_back(r);
            ++in->input_resets;
        } else if (k == "position") {
            const size_t limit = l.find("\"request\":");
            Fix f = Fix();
            uint64_t call = 0, utc = 0;
            int64_t mode = -1, alt = 0;
            if (!u64_field(l, "call", &call, limit) || !u64_field(l, "mono_ns", &f.t, limit) ||
                !i64_field(l, "mode", &mode, limit))
                continue;
            u64_field(l, "utc_s", &utc, limit);
            num_field(l, "lat", &f.lat, limit); num_field(l, "lon", &f.lon, limit);
            num_field(l, "heading", &f.heading, limit); num_field(l, "kmh", &f.kmh, limit);
            num_field(l, "horizontal", &f.horizontal, limit);
            num_field(l, "vertical", &f.vertical, limit);
            i64_field(l, "altitude_m", &alt, limit);
            f.mode = int32_t(mode); f.utc = utc; f.altitude = int32_t(alt);
            positions[call] = f;
        } else if (k == "send") {
            const size_t limit = l.find("\"request\":");
            uint64_t call = 0, type = 0, length = 0;
            std::string hex;
            if (!u64_field(l, "call", &call, limit) || !u64_field(l, "type", &type, limit) ||
                !u64_field(l, "length", &length, limit) || type != 1 || length != 48 ||
                !str_field(l, "original_hex", &hex, limit))
                continue;
            sends[call] = hex;
        }
    }
    if (source == "adapter" || (source == "auto" && !positions.empty())) {
        in->position_source = "adapter_position";
        for (std::map<uint64_t, Fix>::iterator i = positions.begin(); i != positions.end(); ++i) {
            Fix f = i->second;
            const std::map<uint64_t, std::string>::const_iterator s = sends.find(i->first);
            f.has_send = s != sends.end();
            f.has_original = f.has_send && hex48(s->second, f.original);
            if (f.has_send && !f.has_original) { *error = "bad original_hex"; return false; }
            in->calls.push_back(f);
        }
    } else {
        in->position_source = "collector_position_poll";
        const std::vector<Row> crow = read_rows(collector, "collector_boot", &collector_boots);
        for (size_t i = 0; i < crow.size(); ++i) {
            if (!same_boot(crow[i].boot, boot, collector_boots.size())) continue;
            const std::string& l = crow[i].line;
            if (kind_of(l) != "position_poll") continue;
            Fix f = Fix();
            int64_t mode = -1;
            uint64_t utc = 0;
            if (!u64_field(l, "receipt_ns", &f.t) || !i64_field(l, "mode", &mode)) continue;
            u64_field(l, "utc_s", &utc);
            num_field(l, "lat", &f.lat); num_field(l, "lon", &f.lon);
            num_field(l, "heading", &f.heading); num_field(l, "kmh", &f.kmh);
            f.mode = int32_t(mode); f.utc = utc;
            // position_poll has no HDOP; --poll-hdop states the assumption
            // (a stricter anchor gate with an HDOP limit reads this value).
            f.horizontal = f.vertical = mode ? poll_hdop : 99.0;
            f.has_send = true;
            in->calls.push_back(f);
        }
    }
    std::stable_sort(in->raws.begin(), in->raws.end(),
                     [](const Raw& a, const Raw& b) { return a.t < b.t; });
    std::stable_sort(in->calls.begin(), in->calls.end(),
                     [](const Fix& a, const Fix& b) { return a.t < b.t; });
    std::stable_sort(in->wheels.begin(), in->wheels.end(),
                     [](const Wheel& a, const Wheel& b) { return a.t < b.t; });
    for (size_t i = 0; i < in->calls.size(); ++i)
        if (valid_truth(in->calls[i])) in->truth.push_back(in->calls[i]);
    if (in->raws.empty()) { *error = "no motion_batch events"; return false; }
    if (in->calls.empty()) { *error = "no position rows for this boot"; return false; }
    return true;
}

// ------------------------------------------------------------ options ----
struct Options {
    std::string trip, report, csv, boot_id, corrupt, journal, position_source;
    std::vector<double> t0s, durations;
    double sweep_from, sweep_to, sweep_step;
    bool sweep, check, real_only;
    uint64_t cadence_ns, grace_ns, truth_lag_ns, truth_gap_ns;
    double course_min_kmh, coverage_min, poll_hdop;
};
Options opt;

// --------------------------------------------------- product replay ----
Input in;
uint64_t g_now;
std::vector<A::Observation> pending;   // sink queue, popped by the worker turn
R::BetaShared shared;                  // zero-initialized like runtime.cpp
N::Pipeline nav;
int manager_object, storage_object;
uint32_t fake_calls;
uint8_t fake_sent[48];
bool fake_sent_valid;

uint64_t clock_fn(void*) { return g_now; }
void sink_fn(const A::Observation* o, void*) { pending.push_back(*o); }
// runtime.cpp provenance(): BETA first; the LDS association path cannot
// claim anything (returns false) and no association reader is configured.
bool provenance_fn(void*, const A::PositionContext&, A::Provenance* out, void*) {
    if (R::beta_provenance(shared, out)) return true;
    memset(out, 0, sizeof *out);
    return false;
}
// runtime.cpp observe_send_storage_beta(): the stock session reader stays
// configured (BETA_DECISIONS 3.7); the storage fence is Options.send_storage.
void send_storage_fn(void*, const void* storage) { R::beta_observe_storage(shared, storage); }
void beta_event_fn(void*, const char* what) { R::beta_count_event(shared, what); }
int32_t fake_send(void*, A::VehicleData* d) {
    ++fake_calls;
    fake_sent_valid = d && d->type == 1 && d->length == 48 && d->payload;
    if (fake_sent_valid) memcpy(fake_sent, d->payload, 48);
    return 0;
}

struct Journal {
    std::vector<std::string> lines;
    bool failed;
    Journal() : failed(false) {}
    void line(const char* s) { lines.push_back(s); }
    void fail() { failed = true; }
};
Journal journal;
R::BetaController* beta;
R::WorkerTick model_tick;

struct SendRecord {
    uint64_t t, utc;
    int32_t mode, choice, reason, result;
    bool synthetic, one_call;
    uint8_t original[48], outgoing[48];
};
std::vector<SendRecord> sends;

// One OEM POSITION callback with at most one LOCATION send.
void oem_call(const Fix& f, bool synthetic, uint64_t t, const uint8_t* original_or_null) {
    g_now = t;
    uint8_t position[72];
    memset(position, 0, sizeof position);
    put32(position, uint32_t(f.mode));
    put64(position + 8, f.utc);
    putd(position + 16, f.lat); putd(position + 24, f.lon);
    put32(position + 32, uint32_t(f.altitude));
    putd(position + 40, f.heading); putd(position + 48, f.kmh);
    putd(position + 56, f.horizontal); putd(position + 64, f.vertical);
    A::position_enter(&manager_object, position);
    if (f.has_send) {
        uint8_t original[48];
        if (original_or_null) memcpy(original, original_or_null, 48);
        else if (f.has_original) memcpy(original, f.original, 48);
        else synth_original(f, t, original);
        uint8_t payload[48];
        memcpy(payload, original, 48);
        A::VehicleData data = {1, payload, 48};
        fake_calls = 0; fake_sent_valid = false;
        const size_t before = pending.size();
        const int32_t result = A::send_vehicle_data(&storage_object, &data);
        SendRecord r = SendRecord();
        r.t = t; r.utc = f.utc; r.mode = f.mode; r.synthetic = synthetic; r.result = result;
        r.one_call = fake_calls == 1 && fake_sent_valid && result == 0 &&
                     !memcmp(payload, original, 48); // caller buffer untouched
        memcpy(r.original, original, 48);
        memcpy(r.outgoing, fake_sent_valid ? fake_sent : original, 48);
        r.choice = -1; r.reason = -1;
        for (size_t i = before; i < pending.size(); ++i)
            if (pending[i].kind == A::Observation::SEND) {
                r.choice = int32_t(pending[i].choice); r.reason = int32_t(pending[i].reason);
            }
        sends.push_back(r);
    }
    A::position_leave();
}

size_t raw_index;
R::MotionGapTracker motion_gap;
// Recorded-replay exposure of the MODEL reverse latch (design decision 7):
// BETA cannot seed or keep a DR anchor while it is unknown.
struct LatchExposure { uint64_t moving_ticks, moving_latched_ticks; };
LatchExposure latch_exposure;
bool count_latch = true;  // parent only; window children stop counting
// One worker turn at the tick grid, in runtime order: pop observations,
// receive motion, drain on the MODEL tick, then the BETA tick.
bool wheel_at(uint64_t t, double* mps);
void worker_turn(uint64_t now) {
    g_now = now;
    for (size_t i = 0; i < pending.size(); ++i) {
        const A::Observation& o = pending[i];
        if (o.kind == A::Observation::SEND) beta->send(o);
        if (o.kind == A::Observation::POSITION) {
            if (o.reason != A::CONTEXT_UNAVAILABLE) nav.enqueue_position(o);
            beta->position(journal, o, now);
        }
    }
    pending.clear();
    while (raw_index < in.raws.size() && in.raws[raw_index].t <= now) {
        const Raw& r = in.raws[raw_index++];
        if (r.reset) {
            mx5_dr_context c = nav.context();
            ++c.source_epoch; ++c.generation;
            // runtime.cpp drain_motion: a short same-producer gap keeps the
            // reverse latch (runtime/motion_gap.h), anything else clears it.
            uint64_t missing = 0, span = 0;
            if (motion_gap.reject(r.rejected_decoded, r.rejected_reason, r.rejected, &missing, &span))
                nav.reset_keep_reverse(c);
            else nav.reset(c);
        } else {
            uint64_t missing = 0, span = 0;
            if (motion_gap.accept(r.event, &missing, &span) == R::MotionGapTracker::TOO_LARGE)
                nav.exclude_reverse(N::LATCH_CLEAR_INPUT_GAP);
            nav.enqueue_raw(r.event);
        }
    }
    if (model_tick.due(now)) {
        if (now > nav.reorder_ns()) nav.drain(now - nav.reorder_ns());
        const char* fault = journal.failed ? "journal_failed" :
            A::faulted() ? "adapter_fault" :
            A::mode() != A::BETA ? "adapter_mode_changed" : 0;
        // runtime.cpp: the cadence fence (POSITION/SEND gap > 3 s) first.
        if (!fault && beta->cadence_fence(journal, now)) nav.fence_beta();
        beta->tick(journal, now, nav, true, nav.status().last_received_ns, 1, fault);
        double w;
        if (count_latch && wheel_at(now, &w) && w > 1.0) {
            ++latch_exposure.moving_ticks;
            if (nav.reverse_latched()) ++latch_exposure.moving_latched_ticks;
        }
    }
}

bool start_product(uint64_t t) {
    A::Options o = A::Options();
    o.sink = sink_fn; o.clock = clock_fn; o.provenance = provenance_fn;
    o.max_snapshot_age_ns = 500000000ULL; o.allow_assist = false;
    o.session_reader = A::read_send_session; o.send_storage = send_storage_fn;
    o.allow_beta = true; o.beta_event = beta_event_fn;
    // The vehicle case: libpatch owns the session slots and install_v74
    // declined session observation, so every send session is UNOBSERVED.
    o.sessions_declined = true;
    if (!A::configure(fake_send, o) || !A::set_mode(A::OBSERVE)) return false;
    const mx5_dr_context x = {1, 1, 1};
    if (!nav.init_model(N::research_model_profile(), mx5_dr_default_config(), x, true, true, true) ||
        !nav.enable_beta(R::beta_profile_tunnel()))
        return false;
    static R::BetaController controller(shared);
    beta = &controller;
    g_now = t;
    return beta->enable(journal, t, 0) && A::mode() == A::BETA;
}

// ----------------------------------------------------------- evaluation ----
// Send-time position classes (validation/BETA_DECISIONS_2026-10-05.md 1), from
// the original POSITION mode and GetPosition utc_s only:
//   LOST   mode 0 (lost after a fix: frozen fields, HDOP 99/99)
//   NO_FIX mode 1/2 with utc_s 0 (no fix since boot: stored stale fix)
//   FIX    mode 1/2 with utc_s > 0
//   OTHER  mode 3, undecodable, anything else
// What may change per class (the checker, independent of what the product
// currently implements): LOST the BETA replacement fields; NO_FIX only the
// speed (bytes 32..39, the planned speed overlay; position/accuracy/bearing
// never); FIX and OTHER nothing.
enum FixClass { CLASS_LOST = 0, CLASS_NO_FIX, CLASS_FIX, CLASS_OTHER, CLASS_COUNT };
const char* class_name(int c) {
    static const char* const names[] = {"LOST", "NO_FIX", "FIX", "OTHER"};
    return c >= 0 && c < CLASS_COUNT ? names[c] : "UNKNOWN";
}
int classify(int32_t mode, uint64_t utc) {
    if (mode == 0) return CLASS_LOST;
    if (mode == 1 || mode == 2) return utc ? CLASS_FIX : CLASS_NO_FIX;
    return CLASS_OTHER;
}
// change: 0 unchanged, 1 speed only (bytes 32..39), 2 replacement.
enum Change { CHANGE_NONE = 0, CHANGE_SPEED = 1, CHANGE_REPLACED = 2 };
struct SendEval {
    int32_t window;
    double t_s, elapsed_s;
    int32_t mode, choice, reason, result, cls, change;
    uint8_t synthetic, replaced, payload_ok, accuracy_ok, contract_ok, after_return,
            has_truth, has_course, has_wheel, speed_ok;
    double accuracy_m, error_m, speed_mps, wheel_mps, gps_kmh, bearing_deg, course_deg,
           bearing_error_deg, lat, lon;
};
struct WindowEval {
    int32_t id, exit_status;
    double t0_s, d_s;
    int32_t sends, mode0_sends, replaced, with_truth, covered;
    double first_replaced_s, longest_engaged_s;
    char withdrawals[512], never[128];
};

double dist_m(double lat1, double lon1, double lat2, double lon2) {
    const double m = 111132.954 - 559.822 * cos(2 * lat1 * PI / 180);
    const double p = 111412.84 * cos(lat1 * PI / 180);
    const double dn = (lat2 - lat1) * m, de = (lon2 - lon1) * p;
    return sqrt(dn * dn + de * de);
}
double wrap180(double a) {
    a = std::fmod(a, 360.0);
    if (a > 180) a -= 360;
    if (a <= -180) a += 360;
    return a;
}
bool truth_at(uint64_t t, double* lat, double* lon, double* kmh, double* course, bool* has_course) {
    const uint64_t q = t + opt.truth_lag_ns;
    const std::vector<Fix>& v = in.truth;
    size_t lo = 0, hi = v.size();
    while (lo < hi) { const size_t mid = (lo + hi) / 2; if (v[mid].t <= q) lo = mid + 1; else hi = mid; }
    if (!lo || lo >= v.size()) return false;
    const Fix& a = v[lo - 1]; const Fix& b = v[lo];
    if (b.t - a.t > opt.truth_gap_ns) return false;
    const double w = b.t > a.t ? double(q - a.t) / double(b.t - a.t) : 0;
    *lat = a.lat + (b.lat - a.lat) * w; *lon = a.lon + (b.lon - a.lon) * w;
    *kmh = a.kmh + (b.kmh - a.kmh) * w;
    const Fix& n = w < 0.5 ? a : b;
    *has_course = a.kmh >= opt.course_min_kmh && b.kmh >= opt.course_min_kmh &&
                  std::isfinite(n.heading);
    *course = n.heading;
    return true;
}
bool wheel_at(uint64_t t, double* mps) {
    const std::vector<Wheel>& v = in.wheels;
    size_t lo = 0, hi = v.size();
    while (lo < hi) { const size_t mid = (lo + hi) / 2; if (v[mid].t <= t) lo = mid + 1; else hi = mid; }
    if (!lo || t - v[lo - 1].t > 300000000ULL) return false;
    *mps = v[lo - 1].mps;
    return true;
}
// Only lat/lon (8..15), hasAccuracy (16), accuracy (20..23), hasSpeed (32),
// speed (36..39), hasBearing (40) and bearing (44..47) may change.
bool allowed_byte(unsigned i) {
    return (i >= 8 && i <= 16) || (i >= 20 && i <= 23) || i == 32 || (i >= 36 && i <= 40) ||
           (i >= 44 && i <= 47);
}

SendEval evaluate(const SendRecord& r, int window, uint64_t t0, uint64_t end_ns, uint64_t run_start) {
    SendEval s = SendEval();
    s.window = window; s.t_s = r.t / 1e9;
    s.elapsed_s = run_start != NONE && r.t >= run_start ? (r.t - run_start) / 1e9 : NAN;
    s.mode = r.mode; s.choice = r.choice; s.reason = r.reason; s.result = r.result;
    s.synthetic = r.synthetic;
    s.cls = classify(r.mode, r.utc);
    uint8_t out[48];
    memcpy(out, r.outgoing, 48);
    s.replaced = memcmp(out, r.original, 48) != 0;
    if (s.replaced && opt.corrupt == "payload") out[0] ^= 1;       // checker self-test only
    if (s.replaced && opt.corrupt == "accuracy") put32(out + 20, 41000);
    if (s.replaced && opt.corrupt == "mode") { s.mode = 1; s.cls = CLASS_FIX; }
    bool speed_only = s.replaced;
    for (unsigned i = 0; i < 48; ++i)
        if (out[i] != r.original[i] && (i < 32 || i > 39)) speed_only = false;
    s.change = !s.replaced ? CHANGE_NONE : speed_only ? CHANGE_SPEED : CHANGE_REPLACED;
    // Each change must report its own choice (BETA replacement / overlay).
    s.contract_ok = r.one_call && (s.change != CHANGE_REPLACED || r.choice == int32_t(A::BETA_REPLACEMENT)) &&
                    (s.change != CHANGE_SPEED || r.choice == int32_t(A::BETA_SPEED_OVERLAY)) &&
                    (s.change != CHANGE_NONE || r.choice == int32_t(A::ORIGINAL));
    s.payload_ok = 1;
    for (unsigned i = 0; i < 48; ++i) {
        if (out[i] == r.original[i]) continue;
        const bool allowed = s.cls == CLASS_LOST ? allowed_byte(i) :
                             s.cls == CLASS_NO_FIX ? (i >= 32 && i <= 39) : false;
        if (!allowed) s.payload_ok = 0;
    }
    s.speed_ok = 1;
    if (s.change == CHANGE_SPEED) {
        s.accuracy_ok = 1;
        s.accuracy_m = s.error_m = s.bearing_deg = NAN;
        s.speed_mps = int32_t(get32(out + 36)) / 1000.0;
    } else if (!s.replaced) {
        s.accuracy_ok = 1;
        s.accuracy_m = s.error_m = s.speed_mps = s.bearing_deg = NAN;
    } else {
        const uint32_t acc = get32(out + 20);
        s.accuracy_m = acc / 1000.0;
        s.accuracy_ok = out[16] == 1 && acc >= 1 && acc <= 40000;
        s.lat = int32_t(get32(out + 8)) / 1e7; s.lon = int32_t(get32(out + 12)) / 1e7;
        s.speed_mps = int32_t(get32(out + 36)) / 1000.0;
        s.bearing_deg = out[40] ? int32_t(get32(out + 44)) / 1e6 : NAN;
    }
    // After the pseudo return only recorded callbacks run. A recorded mode-0
    // run that starts inside the grace period is a real outage of its own;
    // any replacement of a GPS (mode != 0) callback after the return is not.
    s.after_return = s.replaced && window >= 0 && end_ns != NONE && r.t >= end_ns && s.cls != CLASS_LOST;
    (void)t0;
    double lat, lon, kmh, course;
    bool has_course = false;
    s.error_m = s.bearing_error_deg = s.course_deg = s.gps_kmh = s.wheel_mps = NAN;
    if (truth_at(r.t, &lat, &lon, &kmh, &course, &has_course)) {
        s.gps_kmh = kmh;
        if (s.change == CHANGE_REPLACED) {
            s.has_truth = 1;
            s.error_m = dist_m(lat, lon, s.lat, s.lon);
            if (has_course && std::isfinite(s.bearing_deg)) {
                s.has_course = 1; s.course_deg = course;
                s.bearing_error_deg = wrap180(s.bearing_deg - course);
            }
        }
    }
    double w;
    if (wheel_at(r.t, &w)) { s.has_wheel = 1; s.wheel_mps = w; }
    if (s.change == CHANGE_SPEED)
        s.speed_ok = out[32] == 1 && s.has_wheel &&
                     std::fabs(s.speed_mps - s.wheel_mps) <= std::max(0.5, 0.05 * s.wheel_mps);
    return s;
}

struct Transition { uint64_t t; std::string from, to, reason; };
std::vector<Transition> transitions_since(size_t first) {
    std::vector<Transition> out;
    for (size_t i = first; i < journal.lines.size(); ++i) {
        const std::string& l = journal.lines[i];
        if (kind_of(l) != "beta_state") continue;
        Transition t;
        u64_field(l, "mono_ns", &t.t);
        str_field(l, "from", &t.from); str_field(l, "to", &t.to); str_field(l, "reason", &t.reason);
        out.push_back(t);
    }
    return out;
}
std::string last_summary_field(size_t first, const char* key) {
    for (size_t i = journal.lines.size(); i-- > first;) {
        std::string v;
        if (kind_of(journal.lines[i]) == "beta_summary" && str_field(journal.lines[i], key, &v)) return v;
    }
    return "none";
}
// Engaged spans and the reasons that ended them, from beta_state rows.
void engaged(const std::vector<Transition>& tr, uint64_t end, double* longest,
             std::map<std::string, int>* reasons) {
    uint64_t since = NONE;
    *longest = 0;
    for (size_t i = 0; i < tr.size(); ++i) {
        if (tr[i].to == "ENGAGED" && tr[i].from != "ENGAGED") since = tr[i].t;
        else if (tr[i].from == "ENGAGED" && since != NONE) {
            *longest = std::max(*longest, (tr[i].t - since) / 1e9);
            ++(*reasons)[tr[i].to + ":" + tr[i].reason];
            since = NONE;
        }
    }
    if (since != NONE && end > since) {
        *longest = std::max(*longest, (end - since) / 1e9);
        ++(*reasons)["ENGAGED:end_of_replay"];
    }
}

// --------------------------------------------------------- main loop ----
struct Outage { bool active; int id; uint64_t t0, end, stop, next_synth; Fix frozen; uint8_t original[48]; };
Outage outage;
struct Window { int id; uint64_t t0, d; };
std::vector<Window> windows;
std::vector<SendEval> results;
std::vector<WindowEval> window_results;
unsigned skipped_no_gps;

bool write_all(int fd, const void* p, size_t n) {
    const char* c = static_cast<const char*>(p);
    while (n) {
        const ssize_t w = write(fd, c, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        c += w; n -= size_t(w);
    }
    return true;
}

// never: why a window had no replacement, as "<last skip>/<bridge>" at the
// outage end and the BETA anchor gate result at T0.
void child_report(int fd, const Window& w, size_t send_first, size_t line_first, const std::string& never) {
    WindowEval we = WindowEval();
    we.id = w.id; we.t0_s = w.t0 / 1e9; we.d_s = w.d / 1e9; we.first_replaced_s = NAN;
    std::vector<SendEval> ev;
    for (size_t i = send_first; i < sends.size(); ++i) {
        const SendEval s = evaluate(sends[i], w.id, w.t0, w.t0 + w.d, w.t0);
        ev.push_back(s);
        if (sends[i].t >= w.t0 + w.d) continue; // grace period: checks only
        ++we.sends;
        if (s.mode == 0) ++we.mode0_sends;
        if (s.replaced) {
            ++we.replaced;
            if (!std::isfinite(we.first_replaced_s)) we.first_replaced_s = s.elapsed_s;
            if (s.has_truth) { ++we.with_truth; if (s.error_m <= s.accuracy_m) ++we.covered; }
        }
    }
    std::map<std::string, int> reasons;
    engaged(transitions_since(line_first), g_now, &we.longest_engaged_s, &reasons);
    std::string text;
    for (std::map<std::string, int>::iterator i = reasons.begin(); i != reasons.end(); ++i)
        text += i->first + "=" + std::to_string(i->second) + ";";
    snprintf(we.withdrawals, sizeof we.withdrawals, "%s", text.c_str());
    snprintf(we.never, sizeof we.never, "%s", never.c_str());
    const uint32_t n = uint32_t(ev.size());
    if (!write_all(fd, &we, sizeof we) || !write_all(fd, &n, sizeof n) ||
        (n && !write_all(fd, ev.data(), n * sizeof(SendEval))))
        _exit(3);
}

// Last recorded callback strictly before t (the OEM state the outage freezes).
bool last_fix_before(uint64_t t, Fix* out) {
    const Fix* best = 0;
    for (size_t i = 0; i < in.calls.size() && in.calls[i].t < t; ++i) best = &in.calls[i];
    if (!best || !valid_truth(*best) || t - best->t > opt.truth_gap_ns) return false;
    *out = *best;
    return true;
}

// Replays [now, until). In the parent, forks a child at each window start.
struct Cursor { size_t call; uint64_t tick; };
void replay(Cursor& c, uint64_t until, bool parent, size_t window_index) {
    size_t& call_index = c.call;
    uint64_t& next_tick = c.tick;
    for (;;) {
        // Recorded callbacks inside an active outage are suppressed.
        while (outage.active && call_index < in.calls.size() && in.calls[call_index].t >= outage.t0 &&
               in.calls[call_index].t < outage.end)
            ++call_index;
        const uint64_t t_call = call_index < in.calls.size() ? in.calls[call_index].t : NONE;
        const uint64_t t_synth = outage.active && outage.next_synth < outage.end ? outage.next_synth : NONE;
        const uint64_t t_window = parent && window_index < windows.size() ? windows[window_index].t0 : NONE;
        uint64_t t = std::min(std::min(t_call, t_synth), std::min(next_tick, t_window));
        if (t == NONE || t >= until) { g_now = std::min(until, std::max(g_now, next_tick)); return; }
        if (t == t_window) {
            const Window& w = windows[window_index++];
            Fix frozen;
            if (!last_fix_before(w.t0, &frozen)) { ++skipped_no_gps; continue; }
            fflush(stdout); fflush(stderr);
            int fds[2];
            if (pipe(fds)) { perror("pipe"); exit(2); }
            const pid_t pid = fork();
            if (pid < 0) { perror("fork"); exit(2); }
            if (!pid) {
                close(fds[0]);
                count_latch = false;
                outage = Outage();
                outage.active = true; outage.id = w.id; outage.t0 = w.t0; outage.end = w.t0 + w.d;
                outage.next_synth = w.t0; outage.frozen = frozen;
                outage.frozen.mode = 0; outage.frozen.horizontal = outage.frozen.vertical = 99.0;
                outage.frozen.has_send = true;
                // Frozen OEM LOCATION: last original with the accuracy cleared.
                const Fix* last = 0;
                for (size_t i = 0; i < in.calls.size() && in.calls[i].t < w.t0; ++i)
                    if (in.calls[i].has_send) last = &in.calls[i];
                if (last && last->has_original) memcpy(outage.original, last->original, 48);
                else synth_original(frozen, last ? last->t : w.t0, outage.original);
                outage.original[16] = 0; put32(outage.original + 20, 0);
                const size_t send_first = sends.size(), line_first = journal.lines.size();
                const std::string gate = N::beta_anchor_gate_name(nav.beta_gate());
                Cursor child = c;
                replay(child, w.t0 + w.d, false, 0);
                const std::string never = last_summary_field(line_first, "last_skip") + "/" +
                    last_summary_field(line_first, "bridge") + "/gate_at_t0:" + gate;
                replay(child, w.t0 + w.d + opt.grace_ns, false, 0);
                if (!opt.journal.empty()) {
                    // Per-window BetaController rows: FILE.<window id>.
                    FILE* f = fopen((opt.journal + "." + std::to_string(w.id)).c_str(), "w");
                    if (!f) _exit(6);
                    for (size_t i = line_first; i < journal.lines.size(); ++i)
                        fprintf(f, "%s\n", journal.lines[i].c_str());
                    if (fclose(f)) _exit(6);
                }
                child_report(fds[1], w, send_first, line_first, never);
                close(fds[1]);
                _exit(0);
            }
            close(fds[1]);
            std::vector<char> buffer;
            char chunk[65536];
            for (;;) {
                const ssize_t n = read(fds[0], chunk, sizeof chunk);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) break;
                buffer.insert(buffer.end(), chunk, chunk + n);
            }
            close(fds[0]);
            int status = 0;
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            WindowEval we = WindowEval();
            we.id = w.id; we.t0_s = w.t0 / 1e9; we.d_s = w.d / 1e9;
            we.exit_status = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
            uint32_t n = 0;
            if (!we.exit_status && buffer.size() >= sizeof we + sizeof n) {
                memcpy(&we, buffer.data(), sizeof we);
                memcpy(&n, buffer.data() + sizeof we, sizeof n);
                if (buffer.size() == sizeof we + sizeof n + n * sizeof(SendEval)) {
                    const size_t old = results.size();
                    results.resize(old + n);
                    if (n) memcpy(&results[old], buffer.data() + sizeof we + sizeof n, n * sizeof(SendEval));
                } else we.exit_status = 4;
            } else if (!we.exit_status) we.exit_status = 5;
            window_results.push_back(we);
            continue;
        }
        if (t == next_tick) { worker_turn(t); next_tick += TICK_NS; continue; }
        if (t == t_synth) {
            Fix f = outage.frozen;
            uint8_t original[48];
            memcpy(original, outage.original, 48);
            // The OEM time stamp keeps advancing; position stays frozen.
            const uint64_t ms = (t - outage.t0) / 1000000ULL;
            uint64_t stamp = 0;
            for (unsigned i = 0; i < 8; ++i) stamp |= uint64_t(original[i]) << (8 * i);
            put64(original, stamp + ms);
            oem_call(f, true, t, original);
            outage.next_synth += opt.cadence_ns;
            continue;
        }
        oem_call(in.calls[call_index], false, t, 0);
        ++call_index;
    }
}

// ---------------------------------------------------------------- report ----
struct Stat { size_t n; double median, p90, max; };
Stat stat(std::vector<double> v) {
    Stat s = {v.size(), NAN, NAN, NAN};
    if (v.empty()) return s;
    std::sort(v.begin(), v.end());
    s.median = v.size() % 2 ? v[v.size() / 2] : 0.5 * (v[v.size() / 2 - 1] + v[v.size() / 2]);
    s.p90 = v[size_t(std::ceil(0.9 * v.size())) - 1];
    s.max = v.back();
    return s;
}
std::string num(double v) {
    if (!std::isfinite(v)) return "null";
    char b[48];
    snprintf(b, sizeof b, "%.3f", v);
    return b;
}
std::string stat_json(const Stat& s) {
    return "{\"n\":" + std::to_string(s.n) + ",\"median\":" + num(s.median) + ",\"p90\":" + num(s.p90) +
           ",\"max\":" + num(s.max) + "}";
}
std::string json_escape(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '"' || s[i] == '\\') o += '\\';
        if (static_cast<unsigned char>(s[i]) >= 0x20) o += s[i];
    }
    return o;
}
const char* reason_name(int r) {
    static const char* const names[] = {"PASS", "NO_CONTEXT", "NESTED_CALL", "EXTRA_LOCATION",
        "BAD_LENGTH", "DISABLED", "LOCK_BUSY", "NOT_UNKNOWN", "NOT_READY", "EPOCH_MISMATCH",
        "EXPIRED", "BAD_ENCODING", "BAD_PROVENANCE", "CONTEXT_UNAVAILABLE", "HELD"};
    return r >= 0 && unsigned(r) < sizeof names / sizeof names[0] ? names[r] : "UNKNOWN";
}

std::vector<double> parse_list(const char* s) {
    std::vector<double> v;
    const char* p = s;
    while (*p) {
        char* end = 0;
        const double x = strtod(p, &end);
        if (end == p) { fprintf(stderr, "bad number list: %s\n", s); exit(2); }
        v.push_back(x);
        p = end;
        if (*p == ',') ++p;
    }
    return v;
}
void usage() {
    fprintf(stderr,
        "usage: replay_beta --trip DIR [--t0 S,S..] [--sweep FROM:TO:STEP] [--durations S,S..]\n"
        "                   [--real-only] [--cadence-ms N] [--grace-s S] [--truth-lag-ms N]\n"
        "                   [--truth-gap-ms N] [--course-min-kmh K] [--coverage-min F]\n"
        "                   [--boot-id ID] [--poll-hdop H] [--position-source auto|adapter|poll] [--report FILE.json] [--csv FILE.csv] [--check]\n"
        "                   [--journal FILE] [--self-test-corrupt payload|accuracy|mode]\n"
        "Times are journal monotonic seconds. Without --t0/--sweep only recorded\n"
        "(real) outages are replayed.\n");
    exit(2);
}
uint64_t ns(double s) { return uint64_t(llround(s * 1e9)); }

} // namespace

int main(int argc, char** argv) {
    opt.cadence_ns = 1000000000ULL; opt.grace_ns = 10000000000ULL; opt.truth_lag_ns = 0;
    opt.truth_gap_ns = 3000000000ULL; opt.course_min_kmh = 15; opt.coverage_min = 0.95;
    opt.poll_hdop = 1.0; opt.position_source = "auto";
    opt.durations = parse_list("10,20,30,45,60");
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool more = i + 1 < argc;
        if (a == "--trip" && more) opt.trip = argv[++i];
        else if (a == "--report" && more) opt.report = argv[++i];
        else if (a == "--csv" && more) opt.csv = argv[++i];
        else if (a == "--boot-id" && more) opt.boot_id = argv[++i];
        else if (a == "--t0" && more) opt.t0s = parse_list(argv[++i]);
        else if (a == "--durations" && more) opt.durations = parse_list(argv[++i]);
        else if (a == "--sweep" && more) {
            if (sscanf(argv[++i], "%lf:%lf:%lf", &opt.sweep_from, &opt.sweep_to, &opt.sweep_step) != 3 ||
                !(opt.sweep_step > 0))
                usage();
            opt.sweep = true;
        } else if (a == "--cadence-ms" && more) opt.cadence_ns = uint64_t(atof(argv[++i]) * 1e6);
        else if (a == "--grace-s" && more) opt.grace_ns = ns(atof(argv[++i]));
        else if (a == "--truth-lag-ms" && more) opt.truth_lag_ns = uint64_t(atof(argv[++i]) * 1e6);
        else if (a == "--truth-gap-ms" && more) opt.truth_gap_ns = uint64_t(atof(argv[++i]) * 1e6);
        else if (a == "--course-min-kmh" && more) opt.course_min_kmh = atof(argv[++i]);
        else if (a == "--coverage-min" && more) opt.coverage_min = atof(argv[++i]);
        else if (a == "--poll-hdop" && more) opt.poll_hdop = atof(argv[++i]);
        else if (a == "--position-source" && more) {
            opt.position_source = argv[++i];
            if (opt.position_source != "auto" && opt.position_source != "adapter" &&
                opt.position_source != "poll") usage();
        }
        else if (a == "--self-test-corrupt" && more) opt.corrupt = argv[++i];
        else if (a == "--journal" && more) opt.journal = argv[++i];
        else if (a == "--check") opt.check = true;
        else if (a == "--real-only") opt.real_only = true;
        else usage();
    }
    if (opt.trip.empty() || !opt.cadence_ns) usage();
    for (size_t i = 0; i < opt.durations.size(); ++i)
        if (!(opt.durations[i] > 0)) usage();
    std::string error;
    if (!load(opt.trip, opt.boot_id, opt.poll_hdop, opt.position_source, &in, &error)) { fprintf(stderr, "replay_beta: %s\n", error.c_str()); return 2; }

    // Window list: every (T0, D) pair, ordered by T0.
    std::vector<double> t0s = opt.t0s;
    if (opt.sweep) {
        // A sweep is clamped to the recorded callbacks.
        const double from = std::max(opt.sweep_from, std::ceil(in.calls.front().t / 1e9));
        const double to = std::min(opt.sweep_to, in.calls.back().t / 1e9);
        for (double t = from; t <= to + 1e-9; t += opt.sweep_step) t0s.push_back(t);
    }
    if (opt.real_only) t0s.clear();
    std::sort(t0s.begin(), t0s.end());
    for (size_t i = 0; i < t0s.size(); ++i)
        for (size_t j = 0; j < opt.durations.size(); ++j) {
            Window w = {int(windows.size()), ns(t0s[i]), ns(opt.durations[j])};
            windows.push_back(w);
        }

    const uint64_t start = std::min(in.raws.front().t, in.calls.front().t);
    const uint64_t first_tick = (start / TICK_NS) * TICK_NS;
    if (!start_product(first_tick)) { fprintf(stderr, "replay_beta: product start failed\n"); return 2; }
    const uint64_t last = std::max(in.raws.back().t, in.calls.back().t) + TICK_NS;
    Cursor cursor = {0, first_tick + TICK_NS};
    replay(cursor, last, true, 0);
    worker_turn(last);
    const size_t windows_unreached = windows.size() - window_results.size() - skipped_no_gps;

    // Baseline (recorded modes): elapsed is measured from the first mode-0
    // callback of each recorded mode-0 run.
    std::vector<SendEval> baseline;
    uint64_t run_start = NONE;
    unsigned real_runs = 0;
    for (size_t i = 0; i < sends.size(); ++i) {
        if (sends[i].mode == 0) { if (run_start == NONE) { run_start = sends[i].t; ++real_runs; } }
        else run_start = NONE;
        baseline.push_back(evaluate(sends[i], -1, 0, NONE, run_start));
    }
    double baseline_longest = 0;
    std::map<std::string, int> baseline_reasons;
    engaged(transitions_since(0), g_now, &baseline_longest, &baseline_reasons);

    // ------------------------------------------------------- aggregate ----
    std::vector<std::string> violations;
    std::map<std::string, int> withdrawals = baseline_reasons, never, reasons_mode0;
    std::map<double, std::vector<double> > err_by_d, err_by_bucket, bearing_by_bucket;
    std::vector<double> first_latency;
    std::map<double, std::pair<int, int> > cov_by_d;
    std::vector<double> err_all, speed_err, bearing_all, bearing30, bearing60, gps_speed_err;
    size_t replaced = 0, with_truth = 0, covered = 0, engaged_windows = 0, crashed = 0;
    double longest = baseline_longest;
    struct Worst { double err, acc, t0, d, elapsed; };
    std::vector<Worst> worst;
    std::map<int, const WindowEval*> by_id;
    for (size_t i = 0; i < window_results.size(); ++i) {
        const WindowEval& w = window_results[i];
        by_id[w.id] = &w;
        if (w.exit_status) {
            ++crashed;
            violations.push_back("window t0=" + num(w.t0_s) + " d=" + num(w.d_s) +
                                 " replay child exit " + std::to_string(w.exit_status));
            continue;
        }
        longest = std::max(longest, w.longest_engaged_s);
        if (w.replaced) { ++engaged_windows; first_latency.push_back(w.first_replaced_s); }
        else ++never[w.never];
        std::string text = w.withdrawals;
        size_t p = 0;
        while (p < text.size()) {
            const size_t semi = text.find(';', p), eq = text.find('=', p);
            if (semi == std::string::npos || eq == std::string::npos || eq > semi) break;
            withdrawals[text.substr(p, eq - p)] += atoi(text.substr(eq + 1, semi - eq - 1).c_str());
            p = semi + 1;
        }
    }
    struct ClassCount { size_t sends, speed_only, replaced; };
    ClassCount classes[CLASS_COUNT] = {};
    std::vector<double> no_fix_speed_err;
    std::vector<SendEval> all = baseline;
    all.insert(all.end(), results.begin(), results.end());
    for (size_t i = 0; i < all.size(); ++i) {
        const SendEval& s = all[i];
        char where[96];
        if (s.window >= 0 && by_id.count(s.window))
            snprintf(where, sizeof where, "window t0=%.3f d=%.0f t=%.3f", by_id[s.window]->t0_s,
                     by_id[s.window]->d_s, s.t_s);
        else snprintf(where, sizeof where, "recorded t=%.3f", s.t_s);
        if (!s.contract_ok) violations.push_back(std::string(where) + ": OEM send contract (one call, result, choice) broken");
        if (s.window < 0) {
            ClassCount& c = classes[s.cls];
            ++c.sends;
            if (s.change == CHANGE_SPEED) ++c.speed_only;
            else if (s.change == CHANGE_REPLACED) ++c.replaced;
            if (s.cls == CLASS_NO_FIX && s.change == CHANGE_SPEED && s.has_wheel)
                no_fix_speed_err.push_back(std::fabs(s.speed_mps - s.wheel_mps));
        }
        if (s.change == CHANGE_REPLACED && s.cls != CLASS_LOST)
            violations.push_back(std::string(where) + ": position replaced on a " + class_name(s.cls) +
                                 " send (original mode " + std::to_string(s.mode) + ")");
        else if (!s.payload_ok)
            violations.push_back(std::string(where) + ": " + class_name(s.cls) +
                                 " send: bytes outside the fields allowed for this class differ");
        if (!s.speed_ok) violations.push_back(std::string(where) + ": speed-only change does not follow the wheel speed");
        if (s.mode == 0 && !s.replaced) ++reasons_mode0[reason_name(s.reason)];
        if (s.change != CHANGE_REPLACED) continue;
        ++replaced;
        if (!s.accuracy_ok) violations.push_back(std::string(where) + ": reported accuracy " + num(s.accuracy_m) + " m outside (0,40]");
        if (s.after_return) violations.push_back(std::string(where) + ": replaced after the (pseudo) GPS return");
        if (s.has_wheel) speed_err.push_back(std::fabs(s.speed_mps - s.wheel_mps));
        if (s.window < 0 || !by_id.count(s.window)) continue;
        const WindowEval& w = *by_id[s.window];
        // Statistics cover the pseudo outage only; a recorded outage inside
        // the grace period is evaluated with the recorded (baseline) runs.
        if (s.t_s >= w.t0_s + w.d_s) continue;
        if (std::isfinite(s.gps_kmh)) gps_speed_err.push_back(std::fabs(s.speed_mps * 3.6 - s.gps_kmh));
        if (s.has_truth) {
            ++with_truth;
            const bool ok = s.error_m <= s.accuracy_m;
            if (ok) ++covered;
            err_all.push_back(s.error_m);
            err_by_d[w.d_s].push_back(s.error_m);
            err_by_bucket[std::floor(s.elapsed_s / 10) * 10].push_back(s.error_m);
            cov_by_d[w.d_s].first += ok; cov_by_d[w.d_s].second += 1;
            Worst x = {s.error_m, s.accuracy_m, w.t0_s, w.d_s, s.elapsed_s};
            worst.push_back(x);
        }
        if (s.has_course) {
            bearing_all.push_back(std::fabs(s.bearing_error_deg));
            bearing_by_bucket[std::floor(s.elapsed_s / 10) * 10].push_back(std::fabs(s.bearing_error_deg));
            if (std::fabs(s.elapsed_s - 30) <= 0.5 + opt.cadence_ns / 2e9 && w.d_s > 30)
                bearing30.push_back(std::fabs(s.bearing_error_deg));
            if (std::fabs(s.elapsed_s - 59) <= 0.5 + opt.cadence_ns / 2e9 && w.d_s >= 60)
                bearing60.push_back(std::fabs(s.bearing_error_deg));
        }
    }
    const double coverage = with_truth ? double(covered) / with_truth : NAN;
    if (with_truth && coverage < opt.coverage_min)
        violations.push_back("coverage of reported accuracy " + num(coverage) + " < " + num(opt.coverage_min) +
                             " over " + std::to_string(with_truth) + " replaced sends with GPS");
    std::sort(worst.begin(), worst.end(), [](const Worst& a, const Worst& b) {
        return a.err - a.acc > b.err - b.acc; });

    // Real (recorded) outages.
    size_t real_mode0 = 0, real_replaced = 0;
    for (size_t i = 0; i < baseline.size(); ++i)
        if (baseline[i].mode == 0) { ++real_mode0; if (baseline[i].replaced) ++real_replaced; }
    // Only a DR replacement carries a position; a speed-only overlay keeps the
    // original coordinates (its lat/lon fields are not set), so it has no
    // return jump. No replacement at all: the statistic is null.
    std::vector<double> return_jumps;
    for (size_t i = 0; i + 1 < baseline.size(); ++i)
        if (baseline[i].change == CHANGE_REPLACED && baseline[i + 1].mode != 0) {
            double lat, lon, kmh, course; bool hc;
            if (truth_at(uint64_t(llround(baseline[i + 1].t_s * 1e9)), &lat, &lon, &kmh, &course, &hc))
                return_jumps.push_back(dist_m(lat, lon, baseline[i].lat, baseline[i].lon));
        }

    std::string j = "{\"tool\":\"replay_beta\",\"schema\":1,\"domain\":\"beta_model_replay\","
        "\"note\":\"GPS is not ground truth; offline MODEL replay, not vehicle evidence\",";
    j += "\"input\":{\"boot_id\":\"" + json_escape(in.boot_id) + "\",\"files\":" + std::to_string(in.files) +
         ",\"position_source\":\"" + in.position_source + "\",\"motion_events\":" + std::to_string(in.motion_events) +
         ",\"input_resets\":" + std::to_string(in.input_resets) + ",\"callbacks\":" + std::to_string(in.calls.size()) +
         ",\"valid_gps_fixes\":" + std::to_string(in.truth.size()) + "},";
    j += "\"config\":{\"durations_s\":[";
    for (size_t i = 0; i < opt.durations.size(); ++i) j += (i ? "," : "") + num(opt.durations[i]);
    j += "],\"cadence_ms\":" + num(opt.cadence_ns / 1e6) + ",\"grace_s\":" + num(opt.grace_ns / 1e9) +
         ",\"truth_lag_ms\":" + num(opt.truth_lag_ns / 1e6) + ",\"course_min_kmh\":" + num(opt.course_min_kmh) +
         ",\"coverage_min\":" + num(opt.coverage_min) + ",\"poll_hdop_assumed\":" + num(opt.poll_hdop) + "},";
    j += "\"windows\":{\"requested\":" + std::to_string(windows.size()) + ",\"skipped_no_gps\":" +
         std::to_string(skipped_no_gps) + ",\"beyond_recording\":" + std::to_string(windows_unreached) +
         ",\"evaluated\":" + std::to_string(window_results.size() - crashed) + ",\"engaged\":" +
         std::to_string(engaged_windows) + ",\"crashed\":" + std::to_string(crashed) + "},";
    j += "\"replaced_sends\":" + std::to_string(replaced) + ",\"replaced_with_gps\":" + std::to_string(with_truth) +
         ",\"coverage\":" + num(coverage) + ",";
    j += "\"position_error_m\":{\"all\":" + stat_json(stat(err_all)) + ",\"by_duration\":{";
    bool first = true;
    for (std::map<double, std::vector<double> >::iterator i = err_by_d.begin(); i != err_by_d.end(); ++i) {
        Stat s = stat(i->second);
        const std::pair<int, int> c = cov_by_d[i->first];
        std::string st = stat_json(s);
        st.insert(st.size() - 1, ",\"coverage\":" + num(c.second ? double(c.first) / c.second : NAN));
        j += std::string(first ? "" : ",") + "\"" + num(i->first) + "\":" + st;
        first = false;
    }
    j += "},\"by_elapsed_s\":{";
    first = true;
    for (std::map<double, std::vector<double> >::iterator i = err_by_bucket.begin(); i != err_by_bucket.end(); ++i) {
        j += std::string(first ? "" : ",") + "\"" + std::to_string(int(i->first)) + "-" +
             std::to_string(int(i->first) + 10) + "\":" + stat_json(stat(i->second));
        first = false;
    }
    j += "}},";
    j += "\"speed_error_vs_wheel_mps\":" + stat_json(stat(speed_err)) + ",";
    j += "\"speed_error_vs_gps_kmh\":" + stat_json(stat(gps_speed_err)) + ",";
    j += "\"bearing_error_deg\":{\"all\":" + stat_json(stat(bearing_all)) + ",\"at_30s\":" +
         stat_json(stat(bearing30)) + ",\"at_60s\":" + stat_json(stat(bearing60)) + ",\"by_elapsed_s\":{";
    first = true;
    for (std::map<double, std::vector<double> >::iterator i = bearing_by_bucket.begin(); i != bearing_by_bucket.end(); ++i) {
        j += std::string(first ? "" : ",") + "\"" + std::to_string(int(i->first)) + "-" +
             std::to_string(int(i->first) + 10) + "\":" + stat_json(stat(i->second));
        first = false;
    }
    j += "}},\"first_replacement_after_outage_start_s\":" + stat_json(stat(first_latency)) + ",";
    j += "\"longest_engaged_s\":" + num(longest) + ",\"engaged_exits\":{";
    first = true;
    for (std::map<std::string, int>::iterator i = withdrawals.begin(); i != withdrawals.end(); ++i) {
        j += std::string(first ? "" : ",") + "\"" + json_escape(i->first) + "\":" + std::to_string(i->second);
        first = false;
    }
    j += "},\"never_engaged\":{";
    first = true;
    for (std::map<std::string, int>::iterator i = never.begin(); i != never.end(); ++i) {
        j += std::string(first ? "" : ",") + "\"" + json_escape(i->first) + "\":" + std::to_string(i->second);
        first = false;
    }
    j += "},\"mode0_original_reasons\":{";
    first = true;
    for (std::map<std::string, int>::iterator i = reasons_mode0.begin(); i != reasons_mode0.end(); ++i) {
        j += std::string(first ? "" : ",") + "\"" + i->first + "\":" + std::to_string(i->second);
        first = false;
    }
    j += "},\"recorded_classes\":{";
    for (int c = 0; c < CLASS_COUNT; ++c)
        j += std::string(c ? "," : "") + "\"" + class_name(c) + "\":{\"sends\":" + std::to_string(classes[c].sends) +
             ",\"speed_only\":" + std::to_string(classes[c].speed_only) + ",\"replaced\":" +
             std::to_string(classes[c].replaced) + "}";
    j += "},\"no_fix_speed_error_vs_wheel_mps\":" + stat_json(stat(no_fix_speed_err));
    j += ",\"beta_transitions\":[";
    {
        const std::vector<Transition> tr = transitions_since(0);
        for (size_t i = 0; i < tr.size() && i < 200; ++i)
            j += std::string(i ? "," : "") + "{\"t_s\":" + num(tr[i].t / 1e9) + ",\"from\":\"" + tr[i].from +
                 "\",\"to\":\"" + tr[i].to + "\",\"reason\":\"" + json_escape(tr[i].reason) + "\"}";
    }
    j += "],\"reverse_latch\":{\"moving_s\":" + num(latch_exposure.moving_ticks * TICK_NS / 1e9) +
         ",\"moving_latched_s\":" + num(latch_exposure.moving_latched_ticks * TICK_NS / 1e9) + "}";
    j += ",\"real_outages\":{\"runs\":" + std::to_string(real_runs) + ",\"mode0_sends\":" +
         std::to_string(real_mode0) + ",\"replaced\":" + std::to_string(real_replaced) +
         ",\"longest_engaged_s\":" + num(baseline_longest) + ",\"return_jump_m\":" +
         (return_jumps.empty() ? std::string("null") : stat_json(stat(return_jumps))) + "},";
    j += "\"worst_margin\":[";
    for (size_t i = 0; i < worst.size() && i < 5; ++i)
        j += std::string(i ? "," : "") + "{\"t0_s\":" + num(worst[i].t0) + ",\"d_s\":" + num(worst[i].d) +
             ",\"elapsed_s\":" + num(worst[i].elapsed) + ",\"error_m\":" + num(worst[i].err) +
             ",\"accuracy_m\":" + num(worst[i].acc) + "}";
    j += "],\"violations\":[";
    for (size_t i = 0; i < violations.size() && i < 50; ++i)
        j += std::string(i ? "," : "") + "\"" + json_escape(violations[i]) + "\"";
    j += "],\"violation_count\":" + std::to_string(violations.size()) + ",\"check\":\"" +
         (opt.check ? (violations.empty() ? "pass" : "fail") : "not_requested") + "\"}\n";
    if (!opt.report.empty()) {
        FILE* f = fopen(opt.report.c_str(), "w");
        if (!f || fputs(j.c_str(), f) < 0 || fclose(f)) { perror(opt.report.c_str()); return 2; }
    } else fputs(j.c_str(), stdout);
    if (!opt.csv.empty()) {
        FILE* f = fopen(opt.csv.c_str(), "w");
        if (!f) { perror(opt.csv.c_str()); return 2; }
        fprintf(f, "window,t0_s,duration_s,t_s,elapsed_s,mode,synthetic,choice,reason,result,accuracy_m,"
                   "error_m,speed_mps,wheel_mps,gps_kmh,bearing_deg,course_deg,bearing_error_deg,payload_ok\n");
        for (size_t i = 0; i < all.size(); ++i) {
            const SendEval& s = all[i];
            const WindowEval* w = s.window >= 0 && by_id.count(s.window) ? by_id[s.window] : 0;
            fprintf(f, "%d,%s,%s,%.3f,%s,%d,%u,%d,%s,%d,%s,%s,%s,%s,%s,%s,%s,%s,%u\n", s.window,
                    w ? num(w->t0_s).c_str() : "", w ? num(w->d_s).c_str() : "", s.t_s, num(s.elapsed_s).c_str(),
                    s.mode, unsigned(s.synthetic), s.choice, reason_name(s.reason), s.result,
                    num(s.accuracy_m).c_str(), num(s.error_m).c_str(), num(s.speed_mps).c_str(),
                    num(s.wheel_mps).c_str(), num(s.gps_kmh).c_str(), num(s.bearing_deg).c_str(),
                    num(s.course_deg).c_str(), num(s.bearing_error_deg).c_str(), unsigned(s.payload_ok));
        }
        if (fclose(f)) { perror(opt.csv.c_str()); return 2; }
    }
    if (!opt.journal.empty()) {
        // Baseline BetaController rows (beta_state/beta_hold/beta_summary).
        FILE* f = fopen(opt.journal.c_str(), "w");
        if (!f) { perror(opt.journal.c_str()); return 2; }
        for (size_t i = 0; i < journal.lines.size(); ++i) fprintf(f, "%s\n", journal.lines[i].c_str());
        if (fclose(f)) { perror(opt.journal.c_str()); return 2; }
    }
    if (opt.check && !violations.empty()) {
        fprintf(stderr, "replay_beta --check failed: %zu violation(s)\n", violations.size());
        for (size_t i = 0; i < violations.size() && i < 20; ++i) fprintf(stderr, "  %s\n", violations[i].c_str());
        return 1;
    }
    return 0;
}
