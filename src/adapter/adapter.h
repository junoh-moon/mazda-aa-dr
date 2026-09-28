#ifndef MX5_AA_DR_ADAPTER_H
#define MX5_AA_DR_ADAPTER_H

#include <stddef.h>
#include <stdint.h>

namespace mx5 { namespace adapter {

enum Mode { OFF = 0, OBSERVE = 1, SCRUB_STALE = 2, ASSIST = 3 };
enum Choice { ORIGINAL = 0, SCRUBBED = 1, DR_REPLACEMENT = 2 };
enum Reason { PASS = 0, NO_CONTEXT, NESTED_CALL, EXTRA_LOCATION,
              BAD_LENGTH, DISABLED, LOCK_BUSY, NOT_UNKNOWN, NOT_READY,
              EPOCH_MISMATCH, EXPIRED, BAD_ENCODING, BAD_PROVENANCE };

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
};

struct DrSnapshot {
    uint32_t source_epoch, session_epoch, prediction_generation;
    uint64_t frontier_mono_ns, valid_until_mono_ns, derived_utc_ns;
    double latitude_deg, longitude_deg, speed_mps, travel_bearing_deg;
    bool ready, profile_verified, input_quality_verified, limits_ok, stopped;
};

struct Observation {
    enum Kind { POSITION = 1, SEND = 2 } kind;
    uint32_t call_sequence, prediction_generation, type, length;
    int32_t original_mode, result;
    Choice choice;
    Reason reason;
    uint64_t mono_ns;
    PositionInput position;
    Provenance provenance;
    bool has_payload;
    uint8_t original[48], outgoing[48];
};

// All callbacks run in an OEM call. They MUST be bounded, nonblocking,
// allocation-free, noexcept, and never retain borrowed input pointers.
typedef void (*ObservationSink)(const Observation*, void* user);
typedef uint64_t (*MonotonicClock)(void* user);
typedef bool (*ProvenanceReader)(void* manager, const PositionInput*,
                                 Provenance*, void* user);
struct Options {
    ObservationSink sink;
    MonotonicClock clock;
    ProvenanceReader provenance;
    void* user;
    uint64_t max_snapshot_age_ns;
    bool allow_assist; // Explicit verified deployment gate, false by default.
};

// Initialization only: before installation / before OEM producers start.
bool configure(SendFunction next, const Options& options);
bool set_mode(Mode mode);
Mode mode();
uint32_t invalidate(); // Bounded atomic revocation; no waits.
uint32_t generation();
// Worker-side only; copies values, never borrowed OEM pointers.
bool publish_snapshot(const DrSnapshot& snapshot);

// Testable logic shared by the real ARM veneer and host fake OEM tests.
void position_enter(void* manager, const void* oem_position);
void position_leave();
int32_t send_vehicle_data(void* session_storage, VehicleData* data);
bool decode_position(const void* oem_position, PositionInput* out);
bool encode_location(const DrSnapshot& in, uint8_t out[48]);

enum InstallResult {
    INSTALL_OK = 0, ALREADY_INSTALLED, UNSUPPORTED_ARCH,
    INVALID_INSTALL_ARGUMENT, FILE_IDENTITY_MISMATCH, MODULE_MISMATCH,
    ORIGINAL_BYTES_MISMATCH, NEXT_CHAIN_MISMATCH, MEMORY_PROTECTION_FAILED,
    TRAMPOLINE_ALLOCATION_FAILED, CONFIGURATION_FAILED, RESTORE_FAILED_FATAL
};
typedef bool (*VerifyFileHash)(const char* path, const char* expected_sha256);
struct InstallOptions {
    uintptr_t blm_load_bias, interface_load_bias;
    const char* blm_path;
    const char* interface_path;
    VerifyFileHash verify_file_hash;
    Options runtime;
    // Bootstrap must establish this before GetServiceInterfaces initializes
    // objects/producer threads. This is not an arbitrary hot-patch API.
    bool verified_cold_start;
};
InstallResult install_v74(const InstallOptions&);
const char* install_result_name(InstallResult);

} }

extern "C" void mx5_position_enter(void* manager, const void* position);
extern "C" void mx5_position_leave();
extern "C" int32_t mx5_send_vehicle_data(void*, mx5::adapter::VehicleData*);

#endif
