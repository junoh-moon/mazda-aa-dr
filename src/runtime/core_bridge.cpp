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
    *out=adapter::DrSnapshot();
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
CoreBridgeResult prepare_core_publication(const mx5_dr_core& core,
        const CoreBridgeQualification& q,uint64_t requested_until,adapter::DrSnapshot* out) {
    if (!out) return CORE_BRIDGE_NO_OUTPUT;
    *out=adapter::DrSnapshot();
    mx5_dr_snapshot current=mx5_dr_snapshot();
    if (mx5_dr_get_snapshot(&core,q.now_mono_ns,q.expected_context,&current)!=MX5_DR_OK)
        return CORE_BRIDGE_UNQUALIFIED;
    adapter::DrSnapshot mapped=adapter::DrSnapshot();
    const CoreBridgeResult result=map_core_snapshot(current,q,&mapped);
    if (result!=CORE_BRIDGE_OK) return result;
    if (requested_until<q.now_mono_ns) return CORE_BRIDGE_TIME;

    uint64_t end=minimum(requested_until,current.sensor_lease_until_ns);
    end=minimum(end,saturating_add(current.frontier_ns,q.max_snapshot_age_ns));
    end=minimum(end,saturating_add(current.frontier_ns,core.config.snapshot_age_max_ns));
    uint64_t begin=q.now_mono_ns;
    // The immutable core's age, elapsed time and error budget only increase.
    // Find the last eligible nanosecond using the actual core/bridge predicates,
    // including tighter caller limits, without rounding an error-derived lease
    // outwards. The validated 150 ms age bound limits this to 28 iterations.
    while (begin<end) {
        const uint64_t candidate=begin+(end-begin)/2+1;
        mx5_dr_snapshot future=mx5_dr_snapshot();
        CoreBridgeQualification at=q;
        at.now_mono_ns=at.limits_verified_until_mono_ns=candidate;
        adapter::DrSnapshot checked=adapter::DrSnapshot();
        if (mx5_dr_get_snapshot(&core,candidate,q.expected_context,&future)==MX5_DR_OK &&
            map_core_snapshot(future,at,&checked)==CORE_BRIDGE_OK)
            begin=candidate;
        else
            end=candidate-1;
    }
    // Retain the prediction and its measurement/UTC stamps. Only its numeric
    // publication lease is extended; final-send provenance is still mandatory.
    mapped.valid_until_mono_ns=begin;
    *out=mapped;
    return CORE_BRIDGE_OK;
}
CoreBridgeResult map_model_publication(const BetaModelInput& in,const BetaProfile& p,
                                      adapter::DrSnapshot* out,double* honest_accuracy_m) {
    if (!out) return CORE_BRIDGE_NO_OUTPUT;
    if (honest_accuracy_m) *honest_accuracy_m=0.0;
    *out=adapter::DrSnapshot();
    const mx5_dr_snapshot& s=in.snapshot;
    if (in.result!=MX5_DR_OK || s.domain!=MX5_DR_MODEL_DOMAIN || !s.model_valid || s.valid ||
        s.state!=MX5_DR_ACTIVE || s.reason!=MX5_DR_OK)
        return CORE_BRIDGE_UNQUALIFIED;
    if (!s.context.source_epoch || !s.context.session_epoch || !s.context.generation)
        return CORE_BRIDGE_CONTEXT;
    if (!representable(s.context.source_epoch) || !representable(s.context.session_epoch) ||
        !representable(s.context.generation)) return CORE_BRIDGE_OVERFLOW;
    if (!s.anchor_id || !s.solution_seq || !s.processed_position_seq)
        return CORE_BRIDGE_UNQUALIFIED;
    // Unbounded tunnel profile: the same numeric sanity caps as the core's
    // extended limits (dr_core.c), not behavioural limits.
    const double duration_cap=p.unbounded?21600.0:60.0, distance_cap=p.unbounded?1000000.0:1500.0;
    if (!p.lease_ns || p.lease_ns>1000000000ULL || !(p.accuracy_max_m>0.0) ||
        !std::isfinite(p.accuracy_max_m) || !nonnegative(p.speed_error_mps) ||
        !nonnegative(p.yaw_error_rad_s) || !(p.heading_budget_max_rad>0.0) ||
        !std::isfinite(p.heading_budget_max_rad) || !bounded(p.duration_max_s,duration_cap) ||
        p.duration_max_s==0.0 || !bounded(p.distance_max_m,distance_cap) || p.distance_max_m==0.0)
        return CORE_BRIDGE_LIMIT;
    if (!s.frontier_ns || !s.derived_utc_ns || in.query_mono_ns<s.frontier_ns ||
        in.now_mono_ns<s.frontier_ns) return CORE_BRIDGE_TIME;
    // Decision 4: the lease starts at the frontier; queued revocations cap it.
    const uint64_t valid_until=minimum(saturating_add(s.frontier_ns,p.lease_ns),in.lease_cap_mono_ns);
    if (valid_until<in.query_mono_ns || valid_until<in.now_mono_ns) return CORE_BRIDGE_TIME;
    const double lease_s=double(valid_until-s.frontier_ns)/1e9;
    if (!bounded(s.elapsed_s,p.duration_max_s) || s.elapsed_s+lease_s>p.duration_max_s ||
        !bounded(s.distance_m,p.distance_max_m) || !nonnegative(s.error_budget_m) ||
        !nonnegative(s.heading_budget_rad))
        return CORE_BRIDGE_LIMIT;
    if (!std::isfinite(s.latitude_deg) || std::fabs(s.latitude_deg)>=85.0 ||
        !std::isfinite(s.longitude_deg) || s.longitude_deg < -180.0 || s.longitude_deg>=180.0 ||
        !bounded(s.speed_mps,100.0) || !bounded(s.body_heading_rad,2.0*PI) || s.body_heading_rad==2.0*PI)
        return CORE_BRIDGE_NUMERIC;
    // The reported radius is the budget at lease end; no clamp, no rounding down.
    // The core already carries (v+sv)*age to the query time; the rotation part
    // of the heading budget is carried here through the query age and lease.
    if (!nonnegative(in.rotation_rad) || !nonnegative(in.rotation_budget_m) ||
        !nonnegative(p.rotation_budget_per_rad)) return CORE_BRIDGE_NUMERIC;
    const double age_s=double(in.query_mono_ns-s.frontier_ns)/1e9;
    const double accuracy=s.error_budget_m+in.rotation_budget_m+(s.speed_mps+p.speed_error_mps)*lease_s+
        s.speed_mps*p.rotation_budget_per_rad*in.rotation_rad*(age_s+lease_s);
    if (!std::isfinite(accuracy)) return CORE_BRIDGE_NUMERIC;
    if (honest_accuracy_m) *honest_accuracy_m=accuracy;
    // Unbounded tunnel mode reports min(honest budget, accuracy_max_m); the
    // honest value above is journaled by the caller. Bounded mode refuses.
    if (accuracy>p.accuracy_max_m && !p.unbounded) return CORE_BRIDGE_LIMIT;
    // Rule 5: a withdrawn bearing withdraws the whole first-beta snapshot.
    const double heading=in.heading_budget_rad+p.yaw_error_rad_s*lease_s;
    if (!std::isfinite(heading) || heading<0.0) return CORE_BRIDGE_NUMERIC;
    if (heading>p.heading_budget_max_rad && !p.unbounded) return CORE_BRIDGE_BEARING;
    if ((s.stopped!=0 && s.stopped!=1) || (s.has_bearing!=0 && s.has_bearing!=1))
        return CORE_BRIDGE_BEARING;
    if (s.stopped) {
        if (s.speed_mps!=0.0 || s.has_bearing) return CORE_BRIDGE_BEARING;
    } else if (!s.has_bearing || s.speed_mps==0.0 || !bounded(s.travel_bearing_rad,2.0*PI) ||
               s.travel_bearing_rad==2.0*PI) return CORE_BRIDGE_BEARING;
    adapter::DrSnapshot mapped=adapter::DrSnapshot();
    mapped.source_epoch=static_cast<uint32_t>(s.context.source_epoch);
    mapped.session_epoch=static_cast<uint32_t>(s.context.session_epoch);
    mapped.prediction_generation=static_cast<uint32_t>(s.context.generation);
    mapped.frontier_mono_ns=s.frontier_ns; mapped.valid_until_mono_ns=valid_until;
    mapped.derived_utc_ns=s.derived_utc_ns;
    mapped.latitude_deg=s.latitude_deg; mapped.longitude_deg=s.longitude_deg;
    mapped.speed_mps=s.speed_mps; mapped.travel_bearing_deg=s.stopped ? 0.0 : s.travel_bearing_rad*180.0/PI;
    mapped.ready=true; mapped.limits_ok=true; mapped.stopped=s.stopped!=0;
    // BETA is not a qualification: these external claims stay false.
    mapped.profile_verified=false; mapped.input_quality_verified=false;
    mapped.accuracy_m=accuracy>p.accuracy_max_m?p.accuracy_max_m:accuracy; mapped.beta=true;
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
