#include "runtime/shadow_log.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <limits>
using namespace mx5;
static size_t reference_bounds() {
    navigation::HoldoutResult r=navigation::HoldoutResult();
    char line[2200];
    r.event=navigation::HOLDOUT_COMPARED;r.reason=navigation::HOLDOUT_REFERENCE_OVERFLOW;
    r.window_id=r.anchor_ns=r.reference_ns=r.prediction_frontier_ns=UINT64_MAX;
    r.calibration_version=r.wheel_scale_version=UINT64_MAX;
    r.has_reference_observation=true;r.reference_call=r.reference_generation=UINT32_MAX;
    r.prediction.model_valid=1;r.has_heading_error=true;
    r.prediction.latitude_deg=r.prediction.longitude_deg=r.reference.latitude_deg=
        r.reference.longitude_deg=r.position_error_m=r.heading_error_rad=
        r.applied_yaw_zero=r.applied_wheel_scale=-std::numeric_limits<double>::max();
    assert(runtime::format_shadow_holdout(line,sizeof line,UINT64_MAX,r));
    assert(std::strstr(line,"\"reference_call\":4294967295,"));
    assert(std::strstr(line,"\"reference_generation\":4294967295,"));
    const size_t required=std::strlen(line)+1;
    char guarded[2202];std::memset(guarded,0x5a,sizeof guarded);
    assert(runtime::format_shadow_holdout(guarded+1,required,UINT64_MAX,r));
    assert(!std::strcmp(guarded+1,line)&&guarded[0]==0x5a&&guarded[required+1]==0x5a);
    std::memset(guarded,0x5a,sizeof guarded);
    assert(!runtime::format_shadow_holdout(guarded+1,required-1,UINT64_MAX,r));
    assert(guarded[required-1]==0&&guarded[0]==0x5a&&guarded[required]==0x5a);
    // The formatter follows actual reference presence, including overflow's
    // event rewrite; it never guesses from event names or stale numeric IDs.
    r.event=navigation::HOLDOUT_ABORT;r.reason=navigation::HOLDOUT_OUTPUT_OVERFLOW;
    r.reference_call=r.reference_generation=0;
    assert(runtime::format_shadow_holdout(line,sizeof line,UINT64_MAX,r));
    assert(std::strstr(line,"\"reference_call\":0,"));
    assert(std::strstr(line,"\"reference_generation\":0,"));
    r.has_reference_observation=false;r.reference_call=r.reference_generation=UINT32_MAX;
    assert(runtime::format_shadow_holdout(line,sizeof line,UINT64_MAX,r));
    assert(std::strstr(line,"\"reference_call\":null,"));
    assert(std::strstr(line,"\"reference_generation\":null,"));
    return required;
}
int main(int argc,char** argv) {
    const size_t maximum_holdout=reference_bounds();
    char line[2200];
    const bool emit=argc==2 && !std::strcmp(argv[1],"--emit");
    const bool emit_skipped=argc==2 && !std::strcmp(argv[1],"--emit-skipped");
    const bool emit_overflow_reference=argc==2 && !std::strcmp(argv[1],"--emit-overflow-reference");
    navigation::GyroBiasStatus s=navigation::GyroBiasStatus();
    s.enabled=true; s.state=navigation::GYRO_BIAS_APPLIED;
    s.active_zero=s.candidate_zero=2050; s.samples=40;
    s.evidence_start_ns=1000000000ULL; s.evidence_end_ns=5000000000ULL;
    s.calibration_version=1;
    navigation::WheelScaleStatus w=navigation::WheelScaleStatus();
    w.enabled=true;w.active_scale=w.candidate_scale=1.02;w.calibration_version=1;
    assert(runtime::format_shadow_calibration(line,sizeof line,6000000000ULL,s,w,"ACCEPTED"));
    if(emit) puts(line);
    assert(!runtime::format_shadow_calibration(line,8,6000000000ULL,s,w,"ACCEPTED"));
    navigation::HoldoutResult r=navigation::HoldoutResult();
    r.event=navigation::HOLDOUT_BEGIN; r.reason=navigation::HOLDOUT_NONE;
    r.window_id=1; r.anchor_ns=6000000000ULL; r.applied_yaw_zero=2050;
    r.reference_ns=r.prediction_frontier_ns=r.anchor_ns;
    r.has_reference_observation=true;r.reference_call=101;r.reference_generation=7;
    r.calibration_version=1;
    r.applied_wheel_scale=1.02;r.wheel_scale_version=1;
    assert(runtime::format_shadow_holdout(line,sizeof line,6000000000ULL,r));
    if(emit) puts(line);
    r.event=navigation::HOLDOUT_COMPARED;
    r.reference_call=202;r.reference_generation=8;
    r.reference_ns=r.prediction_frontier_ns=7000000000ULL;
    r.prediction.domain=MX5_DR_MODEL_DOMAIN; r.prediction.model_valid=1;
    r.prediction.latitude_deg=35.00001; r.prediction.longitude_deg=135.00002;
    r.reference.latitude_deg=35; r.reference.longitude_deg=135;
    r.position_error_m=3; r.has_heading_error=true; r.heading_error_rad=-0.02;
    assert(runtime::format_shadow_holdout(line,sizeof line,7100000000ULL,r));
    if(emit) puts(line);
    assert(std::strstr(line,"\"position_error_m\":3"));
    assert(!runtime::format_shadow_holdout(line,1,7100000000ULL,r));
    r.event=navigation::HOLDOUT_END; r.reason=navigation::HOLDOUT_COMPLETE;
    r.has_reference_observation=false;
    r.reference_ns=0; r.prediction_frontier_ns=16000000000ULL;
    assert(runtime::format_shadow_holdout(line,sizeof line,16000000000ULL,r));
    if(emit) puts(line);
    assert(std::strstr(line,"\"position_error_m\":null"));
    assert(std::strstr(line,"\"reference_call\":null,"));
    assert(std::strstr(line,"\"reference_generation\":null,"));
    r.event=navigation::HOLDOUT_COMPARED; r.position_error_m=std::numeric_limits<double>::quiet_NaN();
    assert(runtime::format_shadow_holdout(line,sizeof line,16000000000ULL,r));
    assert(std::strstr(line,"\"position_error_m\":null"));
    r=navigation::HoldoutResult();r.event=navigation::HOLDOUT_SKIPPED;
    r.reason=navigation::HOLDOUT_STALE_REFERENCE;r.reference_ns=1000000000ULL;
    r.has_reference_observation=true;r.reference_call=0;r.reference_generation=UINT32_MAX;
    r.applied_yaw_zero=2050;r.applied_wheel_scale=1.02;
    assert(runtime::format_shadow_holdout(line,sizeof line,16000000000ULL,r));
    assert(std::strstr(line,"\"event\":\"SKIPPED\""));
    if(emit_skipped)puts(line);
    r.event=navigation::HOLDOUT_ABORT;r.reason=navigation::HOLDOUT_OUTPUT_OVERFLOW;
    assert(runtime::format_shadow_holdout(line,sizeof line,1100000000ULL,r));
    if(emit_overflow_reference)puts(line);
    if(!emit&&!emit_skipped&&!emit_overflow_reference) {
        puts("SHADOW log formatter: bounds, MODEL labels, finite/null fields passed");
        printf("Maximum holdout JSON: %lu bytes including NUL, capacity 2200\n",(unsigned long)maximum_holdout);
        printf("Worker MODEL storage: Pipeline=%lu bytes, GpsHoldout=%lu bytes\n",
               (unsigned long)sizeof(navigation::Pipeline),(unsigned long)sizeof(navigation::GpsHoldout));
    }
}
