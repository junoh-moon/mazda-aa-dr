#ifndef MX5_RUNTIME_SHADOW_LOG_H
#define MX5_RUNTIME_SHADOW_LOG_H
#include "navigation/holdout.h"
#include <cmath>
#include <cstdio>

namespace mx5 { namespace runtime {
inline const char* gyro_bias_state_name(navigation::GyroBiasState state) {
    static const char* const names[]={"DISABLED","WAITING","COLLECTING","READY","APPLIED"};
    return unsigned(state)<sizeof names/sizeof names[0]?names[state]:"UNKNOWN";
}
inline void shadow_number(double value,char out[48]) {
    if (std::isfinite(value)) ::snprintf(out,48,"%.17g",value);
    else ::snprintf(out,48,"null");
}
// The worker supplies its C numeric locale. No allocation or wire publication.
inline bool format_shadow_calibration(char* out,size_t capacity,uint64_t now,
                                      const navigation::GyroBiasStatus& s,
                                      const navigation::WheelScaleStatus& w,
                                      const char* anchor_gate) {
    char active[48],candidate[48],variance[48];
    char scale[48],wheel_candidate[48],gps_distance[48],wheel_distance[48];
    shadow_number(s.active_zero,active); shadow_number(s.candidate_zero,candidate);
    shadow_number(s.variance_counts2,variance);
    shadow_number(w.active_scale,scale);shadow_number(w.candidate_scale,wheel_candidate);
    shadow_number(w.gps_distance_m,gps_distance);shadow_number(w.wheel_distance_m,wheel_distance);
    int n=::snprintf(out,capacity,
        "{\"kind\":\"shadow_calibration\",\"mono_ns\":%llu,\"domain\":\"model\","
        "\"assist_ready\":false,\"enabled\":%s,\"state\":\"%s\",\"candidate_ready\":%s,"
        "\"active_zero\":%s,\"candidate_zero\":%s,\"variance_counts2\":%s,"
        "\"samples\":%llu,\"evidence_start_ns\":%llu,\"evidence_end_ns\":%llu,"
        "\"calibration_version\":%llu,\"wheel_enabled\":%s,\"wheel_candidate_ready\":%s,"
        "\"wheel_scale\":%s,\"wheel_candidate_scale\":%s,\"wheel_scale_version\":%llu,"
        "\"wheel_segments\":%llu,\"wheel_gps_distance_m\":%s,\"wheel_distance_m\":%s,"
        "\"wheel_evidence_end_ns\":%llu,\"gps_anchor_gate\":\"%s\"}",
        (unsigned long long)now,s.enabled?"true":"false",gyro_bias_state_name(s.state),
        s.candidate_ready?"true":"false",active,candidate,variance,
        (unsigned long long)s.samples,(unsigned long long)s.evidence_start_ns,
        (unsigned long long)s.evidence_end_ns,(unsigned long long)s.calibration_version,
        w.enabled?"true":"false",w.candidate_ready?"true":"false",scale,wheel_candidate,
        (unsigned long long)w.calibration_version,(unsigned long long)w.segments,
        gps_distance,wheel_distance,(unsigned long long)w.evidence_end_ns,anchor_gate);
    return n>0 && size_t(n)<capacity;
}
inline bool format_shadow_holdout(char* out,size_t capacity,uint64_t now,
                                  const navigation::HoldoutResult& r) {
    const bool compared=r.event==navigation::HOLDOUT_COMPARED;
    char lat[48]="null",lon[48]="null",ref_lat[48]="null",ref_lon[48]="null";
    char error[48]="null",heading[48]="null",zero[48],scale[48];
    char reference_call[16]="null",reference_generation[16]="null";
    if(r.has_reference_observation) {
        ::snprintf(reference_call,sizeof reference_call,"%u",r.reference_call);
        ::snprintf(reference_generation,sizeof reference_generation,"%u",r.reference_generation);
    }
    if (compared) {
        shadow_number(r.prediction.latitude_deg,lat); shadow_number(r.prediction.longitude_deg,lon);
        shadow_number(r.reference.latitude_deg,ref_lat); shadow_number(r.reference.longitude_deg,ref_lon);
        shadow_number(r.position_error_m,error);
        if (r.has_heading_error) shadow_number(r.heading_error_rad,heading);
    }
    shadow_number(r.applied_yaw_zero,zero);
    shadow_number(r.applied_wheel_scale,scale);
    int n=::snprintf(out,capacity,
        "{\"kind\":\"shadow_holdout\",\"mono_ns\":%llu,\"domain\":\"model\","
        "\"assist_ready\":false,\"time_basis\":\"receipt_model\",\"event\":\"%s\",\"reason\":\"%s\","
        "\"window_id\":%llu,\"anchor_ns\":%llu,\"reference_ns\":%llu,\"frontier_ns\":%llu,"
        "\"reference_call\":%s,\"reference_generation\":%s,"
        "\"model_valid\":%s,\"lat\":%s,\"lon\":%s,\"ref_lat\":%s,\"ref_lon\":%s,"
        "\"position_error_m\":%s,\"heading_error_rad\":%s,\"yaw_zero\":%s,\"calibration_version\":%llu,"
        "\"wheel_scale\":%s,\"wheel_scale_version\":%llu}",
        (unsigned long long)now,navigation::holdout_event_name(r.event),navigation::holdout_reason_name(r.reason),
        (unsigned long long)r.window_id,(unsigned long long)r.anchor_ns,
        (unsigned long long)r.reference_ns,(unsigned long long)r.prediction_frontier_ns,
        reference_call,reference_generation,
        compared&&r.prediction.model_valid?"true":"false",lat,lon,ref_lat,ref_lon,error,heading,zero,
        (unsigned long long)r.calibration_version,scale,(unsigned long long)r.wheel_scale_version);
    return n>0 && size_t(n)<capacity;
}
} }
#endif
