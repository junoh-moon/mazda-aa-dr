#include "core_bridge.h"
#include <cmath>
#include <cstring>
#include <limits>

namespace mx5 { namespace runtime {
namespace {
const double PI = 3.14159265358979323846264338327950288;
uint64_t minimum(uint64_t a, uint64_t b) { return a < b ? a : b; }
uint64_t saturating_add(uint64_t a, uint64_t b) {
    return std::numeric_limits<uint64_t>::max()-a < b ? std::numeric_limits<uint64_t>::max() : a+b;
}
bool representable(uint64_t value) { return value && value <= std::numeric_limits<uint32_t>::max(); }
bool nonnegative(double v) { return std::isfinite(v) && v >= 0.0; }
bool bounded(double v, double maximum) { return nonnegative(v) && v <= maximum; }
bool context_equal(const mx5_dr_context& a, const mx5_dr_context& b) {
    return a.source_epoch==b.source_epoch && a.session_epoch==b.session_epoch && a.generation==b.generation;
}
}
CoreBridgeResult map_core_snapshot(const mx5_dr_snapshot& s,
                                  const CoreBridgeQualification& q,
                                  adapter::DrSnapshot* out) {
    if (!out) return CORE_BRIDGE_NO_OUTPUT;
    std::memset(out,0,sizeof(*out));
    if (s.domain!=MX5_DR_QUALIFIED_DOMAIN || s.model_valid || s.valid!=1 || s.state!=MX5_DR_ACTIVE || s.reason!=MX5_DR_OK ||
        !q.profile_verified || !q.input_quality_verified)
        return CORE_BRIDGE_UNQUALIFIED;
    if (!s.context.source_epoch || !s.context.session_epoch || !s.context.generation)
        return CORE_BRIDGE_CONTEXT;
    if (!representable(s.context.source_epoch) || !representable(s.context.session_epoch) ||
        !representable(s.context.generation)) return CORE_BRIDGE_OVERFLOW;
    if (!context_equal(s.context,q.expected_context)) return CORE_BRIDGE_CONTEXT;
    if (!s.anchor_id || !s.solution_seq || !s.processed_position_seq)
        return CORE_BRIDGE_UNQUALIFIED;
    if (!s.frontier_ns || !s.derived_utc_ns || q.now_mono_ns<s.frontier_ns ||
        !q.max_snapshot_age_ns || q.max_snapshot_age_ns>150000000 ||
        q.now_mono_ns-s.frontier_ns>q.max_snapshot_age_ns ||
        s.sensor_lease_until_ns<q.now_mono_ns ||
        q.limits_verified_until_mono_ns<q.now_mono_ns) return CORE_BRIDGE_TIME;
    if (!bounded(q.duration_max_s,60.0) || q.duration_max_s==0.0 ||
        !bounded(q.distance_max_m,1500.0) || q.distance_max_m==0.0 ||
        !bounded(q.error_max_m,100.0) || q.error_max_m==0.0 ||
        !bounded(s.elapsed_s,q.duration_max_s) || !bounded(s.distance_m,q.distance_max_m) ||
        !bounded(s.error_budget_m,q.error_max_m) || !nonnegative(s.heading_budget_rad))
        return CORE_BRIDGE_LIMIT;
    const double age=double(q.now_mono_ns-s.frontier_ns)/1e9;
    if (s.elapsed_s+age>q.duration_max_s) return CORE_BRIDGE_LIMIT;
    if (!std::isfinite(s.latitude_deg) || std::fabs(s.latitude_deg)>=85.0 ||
        !std::isfinite(s.longitude_deg) || s.longitude_deg < -180.0 || s.longitude_deg>=180.0 ||
        !bounded(s.speed_mps,100.0) || !bounded(s.body_heading_rad,2.0*PI) || s.body_heading_rad==2.0*PI)
        return CORE_BRIDGE_NUMERIC;
    if ((s.stopped!=0 && s.stopped!=1) || (s.has_bearing!=0 && s.has_bearing!=1))
        return CORE_BRIDGE_BEARING;
    if (s.stopped) {
        if (s.speed_mps!=0.0 || s.has_bearing) return CORE_BRIDGE_BEARING;
    } else {
        // Adapter has no independent hasBearing flag. Reject an unrepresentable
        // moving/not-yet-confirmed-stop estimate rather than invent a bearing.
        if (!s.has_bearing || s.speed_mps==0.0 || !bounded(s.travel_bearing_rad,2.0*PI) ||
            s.travel_bearing_rad==2.0*PI) return CORE_BRIDGE_BEARING;
    }
    uint64_t deadline=minimum(s.sensor_lease_until_ns,q.limits_verified_until_mono_ns);
    deadline=minimum(deadline,saturating_add(s.frontier_ns,q.max_snapshot_age_ns));
    const double remaining_ns=std::floor((q.duration_max_s-s.elapsed_s)*1e9);
    deadline=minimum(deadline,saturating_add(s.frontier_ns,static_cast<uint64_t>(remaining_ns)));
    if (deadline<q.now_mono_ns) return CORE_BRIDGE_TIME;
    adapter::DrSnapshot mapped=adapter::DrSnapshot();
    mapped.source_epoch=static_cast<uint32_t>(s.context.source_epoch);
    mapped.session_epoch=static_cast<uint32_t>(s.context.session_epoch);
    mapped.prediction_generation=static_cast<uint32_t>(s.context.generation);
    mapped.frontier_mono_ns=s.frontier_ns; mapped.valid_until_mono_ns=deadline;
    mapped.derived_utc_ns=s.derived_utc_ns;
    mapped.latitude_deg=s.latitude_deg; mapped.longitude_deg=s.longitude_deg;
    mapped.speed_mps=s.speed_mps; mapped.travel_bearing_deg=s.stopped ? 0.0 : s.travel_bearing_rad*180.0/PI;
    mapped.ready=true; mapped.profile_verified=q.profile_verified;
    mapped.input_quality_verified=q.input_quality_verified; mapped.limits_ok=true;
    mapped.stopped=s.stopped!=0;
    // Run the real adapter serializer as a final representability check.
    uint8_t bytes[48];
    if (!adapter::encode_location(mapped,bytes)) return CORE_BRIDGE_NUMERIC;
    *out=mapped;
    return CORE_BRIDGE_OK;
}
const char* core_bridge_result_name(CoreBridgeResult r) {
    static const char* const names[]={"OK","NO_OUTPUT","UNQUALIFIED","CONTEXT","OVERFLOW",
        "TIME","LIMIT","NUMERIC","BEARING"};
    return static_cast<unsigned>(r)<sizeof(names)/sizeof(names[0]) ? names[r] : "UNKNOWN";
}
bool encode_model_location_preview(const mx5_dr_snapshot& s,uint8_t out[48]) {
    if(!out)return false;
    std::memset(out,0,48);
    if(s.domain!=MX5_DR_MODEL_DOMAIN || !s.model_valid || s.valid ||
       s.state!=MX5_DR_ACTIVE || s.reason!=MX5_DR_OK || !s.solution_seq ||
       (!s.stopped && !s.has_bearing))return false;
    adapter::DrSnapshot preview=adapter::DrSnapshot();
    preview.latitude_deg=s.latitude_deg;preview.longitude_deg=s.longitude_deg;
    preview.derived_utc_ns=s.derived_utc_ns;preview.speed_mps=s.speed_mps;
    preview.travel_bearing_deg=s.travel_bearing_rad*180.0/PI;preview.stopped=s.stopped!=0;
    return adapter::encode_location(preview,out);
}
} }
