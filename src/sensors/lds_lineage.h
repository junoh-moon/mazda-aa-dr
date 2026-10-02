#ifndef MX5_LDS_LINEAGE_H
#define MX5_LDS_LINEAGE_H

#include <stdint.h>
#include "sensors/nmea_course_token.h"

namespace mx5 { namespace sensors { namespace lds_lineage {

enum Field {
    MODE, UTC, LATITUDE, LONGITUDE, ALTITUDE, HEADING, VELOCITY,
    HORIZONTAL, VERTICAL, FIELD_COUNT
};
static const uint32_t ALL_FIELDS = (uint32_t(1) << FIELD_COUNT) - 1;

// Identity of the last observed assignment, not a physical measurement/source.
// In particular, an RMC mode assignment can depend on earlier GGA/GSA state.
// observed_ns is the observer's time, never producer measurement time. Zero
// means no observer timestamp; write_sequence == 0 means unknown assignment.
struct FieldOrigin {
    uint64_t write_sequence;
    uint64_t observed_ns;
};

class Ledger;

// An owned metadata copy of the actual cache read or response snapshot. No OEM
// pointers or position values are retained. Consumers may keep this value after
// further writes. Do not construct/fill a read copy from guessed field values.
// Pass the unchanged snapshot captured with that actual read. Structural checks
// reject foreign/future copies; they cannot authenticate arbitrary field edits.
struct Snapshot {
    Snapshot();
    uint64_t lifetime;
    uint64_t write_sequence;
    FieldOrigin fields[FIELD_COUNT];
    // Lexical course presence for the HEADING assignment, never numeric quality.
    nmea_course_token::Presence heading_presence;
    // Lexical RMC status belonging to the HEADING assignment, not last receipt.
    nmea_course_token::RmcStatus heading_rmc_status;
private:
    const Ledger* owner_;
    friend class Ledger;
};

enum CommitResult {
    COMMITTED,       // Known assignments may coexist with inherited unknowns.
    UNKNOWN_READ,   // Missing, foreign, expired or inconsistent actual read.
    UNKNOWN_MASK,   // At least one assignment bit is outside the nine fields.
    INACTIVE,       // No accepted observation lifetime.
    EXHAUSTED       // No unique write identity remains in this lifetime.
};

// All calls, including snapshot(), MUST be serialized by the caller's original
// cache mutex. This object adds no locks, waits, allocation, I/O or callbacks.
// Call snapshot at the actual read-copy boundary, then commit at the actual
// write boundary with the caller-verified assignment mask. An older read copy
// remains valid after intervening writes: unassigned fields inherit THAT copy.
// For unsupported writers or a missing read, pass null and retain unknowns.
// Regardless of the result, the caller must preserve original OEM forwarding.
class Ledger {
public:
    Ledger();

    // The caller owns lifetime identity, unrelated to bus/receiver epochs.
    // A new nonzero, strictly increasing lifetime clears all assignment state.
    // Invalid/reused lifetime requests clear knowledge and leave this inactive.
    // Never reuse a lifetime across destruction/address reuse of this Ledger;
    // owner checks cannot distinguish two objects at the same address over time.
    bool begin_lifetime(uint64_t observation_lifetime);

    Snapshot snapshot() const;

    // Every actual cache write consumes one sequence, even if lineage is
    // unknown. assigned_mask == 0 is a verified copy-only write, not a no-write
    // query. An unsupported writer is not represented by an empty mask.
    // Both lexical values apply only when HEADING is assigned and must be bound
    // together to that actual parser/callback/write. Other masks inherit the
    // actual read's metadata. Missing binding stays UNKNOWN; no numeric inference.
    CommitResult commit(const Snapshot* actual_read_copy,
                        uint32_t assigned_mask, uint64_t observed_ns,
                        nmea_course_token::Presence heading_presence=nmea_course_token::UNKNOWN,
                        nmea_course_token::RmcStatus heading_rmc_status=nmea_course_token::RMC_UNKNOWN);

private:
    Ledger(const Ledger&) = delete;
    Ledger& operator=(const Ledger&) = delete;
    bool valid_read(const Snapshot&) const;
    void clear_fields();
    Snapshot current_;
    uint64_t lifetime_high_water_;
    bool exhausted_;
    friend struct LdsLineageTestAccess;
};

} } }
#endif
