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
                                      const navigation::GyroBiasStatus& s) {
    char active[48],candidate[48],variance[48];
    shadow_number(s.active_zero,active); shadow_number(s.candidate_zero,candidate);
    shadow_number(s.variance_counts2,variance);
    int n=::snprintf(out,capacity,
        "{\"kind\":\"shadow_calibration\",\"mono_ns\":%llu,\"domain\":\"model\","
        "\"assist_ready\":false,\"enabled\":%s,\"state\":\"%s\",\"candidate_ready\":%s,"
        "\"active_zero\":%s,\"candidate_zero\":%s,\"variance_counts2\":%s,"
        "\"samples\":%llu,\"evidence_start_ns\":%llu,\"evidence_end_ns\":%llu,"
        "\"calibration_version\":%llu}",
        (unsigned long long)now,s.enabled?"true":"false",gyro_bias_state_name(s.state),
        s.candidate_ready?"true":"false",active,candidate,variance,
        (unsigned long long)s.samples,(unsigned long long)s.evidence_start_ns,
        (unsigned long long)s.evidence_end_ns,(unsigned long long)s.calibration_version);
    return n>0 && size_t(n)<capacity;
}
inline bool format_shadow_holdout(char* out,size_t capacity,uint64_t now,
                                  const navigation::HoldoutResult& r) {
    const bool compared=r.event==navigation::HOLDOUT_COMPARED;
    char lat[48]="null",lon[48]="null",ref_lat[48]="null",ref_lon[48]="null";
    char error[48]="null",heading[48]="null",zero[48];
    if (compared) {
        shadow_number(r.prediction.latitude_deg,lat); shadow_number(r.prediction.longitude_deg,lon);
        shadow_number(r.reference.latitude_deg,ref_lat); shadow_number(r.reference.longitude_deg,ref_lon);
        shadow_number(r.position_error_m,error);
        if (r.has_heading_error) shadow_number(r.heading_error_rad,heading);
    }
    shadow_number(r.applied_yaw_zero,zero);
    int n=::snprintf(out,capacity,
        "{\"kind\":\"shadow_holdout\",\"mono_ns\":%llu,\"domain\":\"model\","
        "\"assist_ready\":false,\"time_basis\":\"receipt_model\",\"event\":\"%s\",\"reason\":\"%s\","
        "\"window_id\":%llu,\"anchor_ns\":%llu,\"reference_ns\":%llu,\"frontier_ns\":%llu,"
        "\"model_valid\":%s,\"lat\":%s,\"lon\":%s,\"ref_lat\":%s,\"ref_lon\":%s,"
        "\"position_error_m\":%s,\"heading_error_rad\":%s,\"yaw_zero\":%s,\"calibration_version\":%llu}",
        (unsigned long long)now,navigation::holdout_event_name(r.event),navigation::holdout_reason_name(r.reason),
        (unsigned long long)r.window_id,(unsigned long long)r.anchor_ns,
        (unsigned long long)r.reference_ns,(unsigned long long)r.prediction_frontier_ns,
        compared&&r.prediction.model_valid?"true":"false",lat,lon,ref_lat,ref_lon,error,heading,zero,
        (unsigned long long)r.calibration_version);
    return n>0 && size_t(n)<capacity;
}
} }
#endif
