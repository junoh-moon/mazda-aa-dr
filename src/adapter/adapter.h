#ifndef MX5_AA_DR_ADAPTER_H
#define MX5_AA_DR_ADAPTER_H

#include <stddef.h>
#include <stdint.h>
#include "runtime/request_trace.h"
#include "runtime/lds_association.h"

namespace mx5 { namespace adapter {

// BETA is the explicit opt-in MODEL-domain replacement. It is never the
// qualified ASSIST gate and ASSIST stays unreachable without allow_assist.
enum Mode { OFF = 0, OBSERVE = 1, SCRUB_STALE = 2, ASSIST = 3, BETA = 4 };
// BETA_SPEED_OVERLAY (BETA_DECISIONS_2026-10-05.md 2, NO_FIX only): the
// original 48 bytes with only hasSpeed (32) and speed_e3 (36..39) replaced.
// Choice 5 (DIAG_SATELLITE) was the beta.12 S5 satellite rewrite
// (validation/DIAG_BUILD_A20_S5_2026-10-12.md). Retired by the diagnostic
// version 2 (validation/TUNNEL_REPORT_CAP_2026-10-12.md): never produced; the
// value stays reserved so old journals keep their meaning.
enum Choice { ORIGINAL = 0, SCRUBBED = 1, DR_REPLACEMENT = 2, BETA_REPLACEMENT = 3,
              BETA_SPEED_OVERLAY = 4, DIAG_SATELLITE_RETIRED = 5 };
// Diagnostic arms (one per GPS-lost episode, runtime/diag_arms.h). NONE: the
// diagnostic marker is absent or no episode is open. Values 2 (A20) and 3
// (S5) were the beta.12 arms; retired, never assigned (journal names only).
enum DiagArm { DIAG_ARM_NONE = 0, DIAG_ARM_CONTROL = 1, DIAG_ARM_CAP = 4 };
// Outcome of one type-21 send while the diagnostic is on (journal only).
// Observation only: every outcome forwards the original wrapper and payload
// untouched. Values 1 and 7..14 were beta.12 S5 rewrite outcomes (retired,
// never produced; names kept for old journals).
enum DiagSatOutcome { DIAG_SAT_NONE = 0, DIAG_SAT_ARM_CONTROL = 2, DIAG_SAT_NO_REPLACEMENT = 4,
                      DIAG_SAT_NO_CONTEXT = 5, DIAG_SAT_NESTED = 6, DIAG_SAT_ARM_CAP = 15 };
const char* diag_sat_outcome_name(DiagSatOutcome);
const char* diag_arm_name(DiagArm);
// GPS_SATELLITE (AA sensor type 21) as RequestSendPosition sends it on the
// v74 BLM (verified statically, validation/DIAG_BUILD_A20_S5_2026-10-12.md):
// an 864-byte structure; bytes 0..3 number_in_use (int32), bytes 4..7 the
// number_in_view presence flag (only byte 4 is read), then number_in_view and
// the satellite arrays. For mode 0 the BLM zeroes all 864 bytes and writes
// number_in_use 0; for mode 1 it writes 5. Observed (read) only.
enum { DIAG_SAT_TYPE = 21, DIAG_SAT_LENGTH = 864 };
enum Reason { PASS = 0, NO_CONTEXT, NESTED_CALL, EXTRA_LOCATION,
              BAD_LENGTH, DISABLED, LOCK_BUSY, NOT_UNKNOWN, NOT_READY,
              EPOCH_MISMATCH, EXPIRED, BAD_ENCODING, BAD_PROVENANCE,
              CONTEXT_UNAVAILABLE,
              HELD, // BETA: a replaced send returned non-zero; awaiting an ORIGINAL 0.
              // BETA overlay: the LOCATION does not carry this POSITION's
              // coordinates, or neither the original nor the wheels show speed.
              OVERLAY_MISMATCH, OVERLAY_NOT_NEEDED };
// Send-time position class (BETA_DECISIONS_2026-10-05.md 1), computed right
// after decode_position from mode and GetPosition utc_s only (HDOP is not a
// key). NO_FIX_STALE: mode 1/2 with utc 0 (stored value, unknown age).
// FIX: mode 1/2, utc > 0, increased (or unchanged for at most 3 s).
// LOST: mode 0. NATIVE_DR: mode 3. UTC_STALL: mode 1/2 whose utc went
// backwards or did not increase for more than 3 s. UNDECODED: anything else,
// including a concurrent classification. Only NO_FIX_STALE and LOST can ever
// be replaced in Mode::BETA; everything else passes the original.
enum PositionClass { POSITION_UNDECODED = 0, POSITION_NO_FIX_STALE = 1, POSITION_FIX = 2,
                     POSITION_LOST = 3, POSITION_NATIVE_DR = 4, POSITION_UTC_STALL = 5 };
const char* position_class_name(PositionClass);

// Native OEM wrapper. Exactly 12 bytes only on the ARM32 target.
struct VehicleData { uint32_t type; void* payload; uint32_t length; };
// A 32-bit ABI return-register carrier; preserves the OEM status bit pattern.
typedef int32_t (*SendFunction)(void* session_storage, VehicleData* data);

struct PositionInput {
    int32_t mode;
    uint64_t utc_seconds;
    double latitude_deg, longitude_deg;
    int32_t altitude_m;
    double heading_deg, velocity_kmh, horizontal, vertical;
};

struct Provenance {
    uint32_t source_epoch, session_epoch;
    // Must describe THIS request before the OEM callback/worker boundary.
    // A freshly polled global owner is not sufficient.
    bool exact_request, verified_lds, legacy_receiver;
    // Which evidence domain a snapshot for this request may come from. Zero
    // (value-initialized) is NONE. QUALIFIED/NONE may only feed ASSIST and BETA
    // may only feed Mode::BETA. No member initializer: C++11 aggregate
    // initialization and the trivially constructed context pool rely on it.
    enum class Domain { NONE, QUALIFIED, BETA } domain;
};

struct DrSnapshot {
    uint32_t source_epoch, session_epoch, prediction_generation;
    uint64_t frontier_mono_ns, valid_until_mono_ns, derived_utc_ns;
    double latitude_deg, longitude_deg, speed_mps, travel_bearing_deg;
    bool ready, profile_verified, input_quality_verified, limits_ok, stopped;
    // BETA only (Mode::BETA): reported radius and the MODEL-domain marker. No
    // member initializers: value-initialize (DrSnapshot()) like the rest; the
    // adapter static_asserts trivial default construction.
    double accuracy_m;
    bool beta;
    // BETA speed overlay candidate: only speed_mps/stopped and the identity,
    // lease fields are meaningful. choose_dr/choose_beta reject it and the
    // overlay accepts nothing else. No accuracy is claimed.
    bool speed_only;
    // Diagnostic build only: the DiagArm of the GPS-lost episode this BETA
    // candidate belongs to (0 when the diagnostic marker is absent). Read by
    // send_vehicle_data only while set_diag(true) is in effect.
    uint8_t diag_arm;
};

struct Observation {
    enum Kind { POSITION = 1, SEND = 2 } kind;
    uint32_t call_sequence, prediction_generation, type, length;
    int32_t original_mode, result;
    Choice choice;
    Reason reason;
    uint64_t mono_ns;
    PositionClass position_class; // POSITION and its SEND (UNDECODED without context)
    PositionInput position;
    Provenance provenance;
    // Observation only; successful association grants no ASSIST qualification.
    runtime::request_trace::Result request_result;
    runtime::request_trace::Trace request_trace;
    runtime::lds_association::Owned lds_association;
    runtime::session_trace::Snapshot send_session; // Actual send storage lookup.
    bool has_payload;
    uint8_t original[48], outgoing[48];
    // AA GEAR shape only (type 8, length 4) and only with
    // Options.journal_gear_payload: a copy of the bytes passed to next() for
    // the journal (decision G). Observation only; never used as evidence.
    uint8_t small_length;
    uint8_t small_payload[16];
    // Diagnostic build only (set_diag(true)); all zero otherwise, so the
    // journal row stays byte-identical. diag_arm: the arm latched by this
    // call's BETA LOCATION replacement (DiagArm). diag_sat: a type-21 send
    // with its outcome, number_in_use before/after (always equal since the
    // diagnostic version 2: nothing is rewritten) and the 32-bit word at
    // offset 4 (presence flag of number_in_view); values are 0 when the
    // payload is shorter than 8 bytes (diag_sat_short).
    uint8_t diag_arm;
    bool diag_sat, diag_sat_short;
    uint8_t diag_sat_outcome;
    uint32_t diag_sat_before, diag_sat_after, diag_sat_presence;
};

// All callbacks run in an OEM call. They MUST be bounded, nonblocking,
// allocation-free, noexcept, and never retain borrowed input pointers.
typedef void (*ObservationSink)(const Observation*, void* user);
typedef uint64_t (*MonotonicClock)(void* user);
// Borrowed only for this provenance callback. The adapter has already consumed
// the one-shot request reader and captured this call's post-mode-change
// generation. Do not repeat that lookup, retain these references, or replace
// them with a later global/session/sideband observation. Nested calls have
// separate frames. A non-OK request_result always accompanies an empty trace.
// These are observation identities, not provider/receiver or sensor evidence.
struct PositionContext {
    const PositionInput& position;
    runtime::request_trace::Result request_result;
    const runtime::request_trace::Trace& request_trace;
    uint32_t call_sequence,prediction_generation;
    // Borrowed only while the provenance reader runs. No mapped/OEM pointer is
    // retained; POSITION and this frame's SEND own the same value.
    const runtime::lds_association::Owned* lds_association;
};
typedef bool (*AssociationReader)(const PositionContext&,
                                  runtime::lds_association::Owned*,void* user);
typedef bool (*ProvenanceReader)(void* manager, const PositionContext&,
                                 Provenance*, void* user);
typedef runtime::request_trace::Result (*RequestReader)(const void* oem_position,
                                  runtime::request_trace::Trace*, void* user);
typedef void (*SessionReader)(const void* storage,runtime::session_trace::Snapshot*,void* user);
struct Options {
    ObservationSink sink;
    MonotonicClock clock;
    ProvenanceReader provenance;
    void* user;
    uint64_t max_snapshot_age_ns;
    bool allow_assist; // Explicit verified deployment gate, false by default.
    RequestReader request_reader; // Optional live raw-pointer association.
    SessionReader session_reader; // Optional actual send argument observation.
    AssociationReader association_reader; // Optional memory-only before-reply lookup.
    bool allow_beta; // Explicit MODEL-domain BETA opt-in, false by default.
    // BETA hold transition hook (required when allow_beta) ("hold_set"/"hold_cleared", static
    // strings). Runs inside the OEM send after next() returned; it MUST be
    // bounded, nonblocking, allocation-free and noexcept. errno is restored.
    void (*beta_event)(void* user, const char* what);
    // Optional BETA send-storage fence. Runs inside every OEM send right after
    // session_reader and before the replacement decision, with the actual
    // session_storage argument as an identity only (never dereferenced). Same
    // constraints as beta_event. Kept separate from session_reader because the
    // production installer accepts only the product session reader
    // (validation/BETA_DECISIONS_2026-10-05.md 3.7).
    void (*send_storage)(void* user, const void* session_storage);
    // BETA: the installation explicitly declined session observation because a
    // known third-party shim owns the session slots. Only then may choose_beta
    // accept an UNOBSERVED send session. install_v74 overwrites this from its
    // own decision; a direct configure() caller asserts the same fact.
    bool sessions_declined;
    // Decision G: copy the 4 bytes of a type-8 length-4 send (AA GEAR shape)
    // into its observation. Only the journaling SHADOW/BETA configurations
    // set it; otherwise (and in Mode OFF) the send path does no extra work.
    bool journal_gear_payload;
};

// Initialization only: before installation / before OEM producers start.
bool configure(SendFunction next, const Options& options);
bool set_mode(Mode mode);
// Diagnostic BETA build (owner marker <root>/diag, read once by the worker
// before BETA is enabled). false (default): no diagnostic work on the send
// path. true: a BETA LOCATION replacement records its episode arm and
// type-21 sends are observed (diag_sat). No payload is ever rewritten: every
// non-LOCATION send forwards the original wrapper and payload.
void set_diag(bool on);
bool diag();
Mode mode();
uint32_t invalidate(); // Bounded atomic revocation; no waits.
// Retire one owned generation only if it is still current. A newer adapter
// generation has already made that candidate unselectable.
uint32_t invalidate_if_generation(uint32_t owned);
uint32_t generation();
bool faulted(); // Sticky observation/contract failure; never qualifies input.
// BETA send-result hold: set when a BETA-replaced send returned non-zero,
// cleared only after an ORIGINAL LOCATION send returned 0.
bool beta_held();
// Worker-side only; copies values, never borrowed OEM pointers.
bool publish_snapshot(const DrSnapshot& snapshot);

// Testable logic shared by the real ARM veneer and host fake OEM tests.
void position_enter(void* manager, const void* oem_position);
void position_aborted(); // OEM callback did not return; capture cannot qualify.
void position_leave();
int32_t send_vehicle_data(void* session_storage, VehicleData* data);
bool decode_position(const void* oem_position, PositionInput* out);
bool encode_location(const DrSnapshot& in, uint8_t out[48]);
// BETA: copy the original 48 bytes, then overwrite only lat/lon (8..15),
// accuracy (16, 20..23), speed (32, 36..39) and bearing (40, 44..47). The
// timestamp (0..7), altitude (24..31) and padding stay original. Rejects
// rather than clamps out-of-range values (accuracy must be in (0, 40] m).
bool encode_beta_location(const DrSnapshot& in, const uint8_t original[48], uint8_t out[48]);
// BETA speed overlay: copy the original 48 bytes, then set only hasSpeed
// (32) = 1 and speed_e3 (36..39); 0 when stopped. Everything else, including
// the original accuracy, stays byte-identical. Rejects non-finite, negative
// or > 100 m/s speeds.
bool encode_speed_overlay(const DrSnapshot& in, const uint8_t original[48], uint8_t out[48]);

enum InstallResult {
    INSTALL_OK = 0, ALREADY_INSTALLED, UNSUPPORTED_ARCH,
    INVALID_INSTALL_ARGUMENT, FILE_IDENTITY_MISMATCH, MODULE_MISMATCH,
    ORIGINAL_BYTES_MISMATCH, NEXT_CHAIN_MISMATCH, MEMORY_PROTECTION_FAILED,
    TRAMPOLINE_ALLOCATION_FAILED, CONFIGURATION_FAILED, RESTORE_FAILED_FATAL, COLD_START_LOST
};
typedef bool (*VerifyFileHash)(const char* path, const char* expected_sha256);
// Diagnostic output of install_v74; never influences the decision. Records the
// first slot comparison that failed and which loaded module owns the value found
// there (the BLM's own PLT stub = lazy binding, a preload = interposition),
// as module-relative offsets so no absolute address is exposed.
struct InstallReport {
    unsigned declined_stage;       // 0 none; 1 send slot; 2 request/bus/JCIDBUS slot; 3 session slot
    uintptr_t slot_offset;         // mismatching slot, relative to the BLM load bias
    uintptr_t slot_expected_offset;// expected target, relative to the interface load bias
    uintptr_t observed_offset;     // value found, relative to its owner's load base
    char owner[96];                // dladdr path of the owner of the value found (sanitized)
    char symbol[48];               // dladdr symbol name, if any (sanitized)
    bool sessions_declined;        // session observation skipped: third-party shim owns the session slots
};
struct InstallOptions {
    uintptr_t blm_load_bias, interface_load_bias;
    const char* blm_path;
    const char* interface_path;
    VerifyFileHash verify_file_hash;
    Options runtime;
    // Bootstrap must establish this before GetServiceInterfaces initializes
    // objects/producer threads. This is not an arbitrary hot-patch API.
    bool verified_cold_start;
    // Optional pair for loader-coordinated cold installation. Acquired only
    // after all dynamic-loader/hash queries; released after the final patch.
    // end(false) requires the caller to stop the process on fatal RX failure.
    bool (*begin_patch)();
    void (*end_patch)(bool executable_restored);
    // Production includes request/worker observation in this SAME transaction.
    // The existing BLM handle supplies its dependency scope during preflight.
    bool observe_requests;
    void* blm_handle;
    InstallReport* report; // optional, may be null
};
// The observe_requests installation accepts only the product request/session
// readers (host-testable part of install_v74's argument check).
bool install_readers_supported(const Options&);
InstallResult install_v74(const InstallOptions&);
const char* install_result_name(InstallResult);

} }

extern "C" void mx5_position_enter(void* manager, const void* position);
extern "C" void mx5_position_leave();
extern "C" int32_t mx5_send_vehicle_data(void*, mx5::adapter::VehicleData*);

#endif
