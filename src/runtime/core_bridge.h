#ifndef MX5_AA_DR_CORE_BRIDGE_H
#define MX5_AA_DR_CORE_BRIDGE_H
#include "core/dr_core.h"
#include "adapter/adapter.h"
#include "runtime/beta_profile.h"

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
    // map_core_snapshot never extends this claim. prepare_core_publication
    // instead rechecks the immutable core to derive a future numeric lease.
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
// Single-owner worker API. Check the immutable qualified core through an
// explicitly requested publication deadline, capped by its original evidence
// leases, snapshot age and numeric budgets. Qualification must cover now; the
// helper checks later numeric validity itself. Never extrapolates coordinates
// or refreshes measurement/UTC timestamps, and does not publish or enable ASSIST.
CoreBridgeResult prepare_core_publication(const mx5_dr_core& core,
                                         const CoreBridgeQualification& qualification,
                                         uint64_t requested_until_mono_ns,
                                         adapter::DrSnapshot* out);
const char* core_bridge_result_name(CoreBridgeResult);
// Diagnostic serialization only: never returns a ready DrSnapshot. Model
// values cannot pass map_core_snapshot, even with externally forged q flags.
bool encode_model_location_preview(const mx5_dr_snapshot&, uint8_t out[48]);

// BETA domain input produced by navigation::Pipeline::model_publication. It is
// a MODEL calculation from the BETA core, never a qualified snapshot.
struct BetaModelInput {
    mx5_dr_snapshot snapshot;     // BETA core, queried at query_mono_ns
    mx5_dr_result result;         // mx5_dr_get_model_snapshot result (or gate)
    uint64_t now_mono_ns;         // caller's current time
    uint64_t query_mono_ns;       // earliest admissible query time >= frontier
    uint64_t lease_cap_mono_ns;   // queued GPS/anchor revocation - 1, else max
    double heading_budget_rad;    // rule 5 heading budget at query time
    // BETA_DECISIONS 3.3: cumulative |rotation| since the anchor (through the
    // frontier) and its position-budget part integral(v*c_rot*R) in metres.
    double rotation_rad, rotation_budget_m;
};
// BETA mapping (design decision 4, accuracy rule 2 and 5, BETA_DECISIONS 3.3).
// accuracy_m = error budget + rotation budget + (speed + sv) * lease
//   + speed * c_rot * rotation * (query age + lease),
// valid_until = frontier + lease (capped).
// accuracy_m > accuracy_max_m, heading budget > heading_budget_max_rad, an
// expired lease or any non-ACTIVE/non-MODEL input returns non-OK with *out
// zeroed (ready=false). Never clamps or lowers the accuracy. beta=true marks
// the result; profile/input verification claims stay false.
CoreBridgeResult map_model_publication(const BetaModelInput& input,
                                      const BetaProfile& profile,
                                      adapter::DrSnapshot* out,
                                      double* honest_accuracy_m=0);
// profile.unbounded (tunnel mode, v1.0.0-beta.6): the accuracy and heading
// limits above do not refuse; the reported accuracy is min(honest, max) and
// *honest_accuracy_m (when given) always carries the honest budget.

} }
#endif
