#ifndef MX5_RUNTIME_LDS_ASSOCIATION_H
#define MX5_RUNTIME_LDS_ASSOCIATION_H

#include "sensors/lds_lineage.h"
#include <stdint.h>

namespace mx5 { namespace runtime { namespace lds_association {

// Request/snapshot observation only. These values do not qualify physical
// measurement time, provider, receiver, calibration, or ASSIST enablement.
enum Result : uint32_t {
    UNAVAILABLE=0, MATCHED_LOCKED_FOR_SEND=1, CONFLICT=2, PAYLOAD_MISMATCH=3
};
enum Stage : uint32_t { NO_STAGE=0, LOCKED_FOR_SEND=1 };
struct Owned {
    Result result;
    Stage stage;
    uint32_t call_sequence,prediction_generation,view_revision,layout_version;
    uint64_t source_instance,record_sequence,locked_observed_ns,map_loss_epoch;
    uint64_t request_id,request_epoch,worker_id,worker_epoch;
    uint64_t cache_lifetime,write_sequence;
    sensors::lds_lineage::FieldOrigin fields[sensors::lds_lineage::FIELD_COUNT];
    sensors::nmea_course_token::Presence heading_presence;
    // Lexical RMC status owned by this HEADING assignment, never receiver quality.
    sensors::nmea_course_token::RmcStatus heading_rmc_status;
};
static_assert(sizeof(Owned)<=256,"Bounded per-callback LDS association");

} } }
#endif
