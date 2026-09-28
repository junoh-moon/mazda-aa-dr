#ifndef MX5_AA_DR_CORE_BRIDGE_H
#define MX5_AA_DR_CORE_BRIDGE_H
#include "core/dr_core.h"
#include "adapter/adapter.h"

namespace mx5 { namespace runtime {

enum CoreBridgeResult {
    CORE_BRIDGE_OK = 0, CORE_BRIDGE_NO_OUTPUT, CORE_BRIDGE_UNQUALIFIED,
    CORE_BRIDGE_CONTEXT, CORE_BRIDGE_OVERFLOW, CORE_BRIDGE_TIME,
    CORE_BRIDGE_LIMIT, CORE_BRIDGE_NUMERIC, CORE_BRIDGE_BEARING
};

// Explicit external assertions. No constructor/default confers qualification.
// Current live runtime has no qualifying sensor source and must keep ASSIST off.
struct CoreBridgeQualification {
    mx5_dr_context expected_context;
    uint64_t now_mono_ns;
    uint64_t max_snapshot_age_ns;
    // Caller has checked error/time/distance limits THROUGH this deadline.
    // If only mx5_dr_get_snapshot(now) has been checked, set this to now exactly.
    // The bridge never creates a future validity claim from a current sample.
    uint64_t limits_verified_until_mono_ns;
    double duration_max_s, distance_max_m, error_max_m;
    bool profile_verified, input_quality_verified;
};

// Side-effect-free mapping; does not configure, publish, or enable the adapter.
// Input must be a successful mx5_dr_get_snapshot result for qualification.now.
// On every rejection *out is zeroed and non-ready. Runtime must still recheck
// generation/readiness/provenance at final adapter send selection.
CoreBridgeResult map_core_snapshot(const mx5_dr_snapshot& source,
                                  const CoreBridgeQualification& qualification,
                                  adapter::DrSnapshot* out);
const char* core_bridge_result_name(CoreBridgeResult);

} }
#endif
