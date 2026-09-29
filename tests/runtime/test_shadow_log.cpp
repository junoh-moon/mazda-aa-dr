#include "runtime/shadow_log.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <limits>
using namespace mx5;
int main(int argc,char** argv) {
    char line[2200];
    const bool emit=argc==2 && !std::strcmp(argv[1],"--emit");
    navigation::GyroBiasStatus s=navigation::GyroBiasStatus();
    s.enabled=true; s.state=navigation::GYRO_BIAS_APPLIED;
    s.active_zero=s.candidate_zero=2050; s.samples=40;
    s.evidence_start_ns=1000000000ULL; s.evidence_end_ns=5000000000ULL;
    s.calibration_version=1;
    assert(runtime::format_shadow_calibration(line,sizeof line,6000000000ULL,s));
    if(emit) puts(line);
    assert(!runtime::format_shadow_calibration(line,8,6000000000ULL,s));
    navigation::HoldoutResult r=navigation::HoldoutResult();
    r.event=navigation::HOLDOUT_BEGIN; r.reason=navigation::HOLDOUT_NONE;
    r.window_id=1; r.anchor_ns=6000000000ULL; r.applied_yaw_zero=2050;
    r.reference_ns=r.prediction_frontier_ns=r.anchor_ns;
    r.calibration_version=1;
    assert(runtime::format_shadow_holdout(line,sizeof line,6000000000ULL,r));
    if(emit) puts(line);
    r.event=navigation::HOLDOUT_COMPARED;
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
    r.reference_ns=0; r.prediction_frontier_ns=16000000000ULL;
    assert(runtime::format_shadow_holdout(line,sizeof line,16000000000ULL,r));
    if(emit) puts(line);
    assert(std::strstr(line,"\"position_error_m\":null"));
    r.event=navigation::HOLDOUT_COMPARED; r.position_error_m=std::numeric_limits<double>::quiet_NaN();
    assert(runtime::format_shadow_holdout(line,sizeof line,16000000000ULL,r));
    assert(std::strstr(line,"\"position_error_m\":null"));
    if(!emit) {
        puts("SHADOW log formatter: bounds, MODEL labels, finite/null fields passed");
        printf("Worker MODEL storage: Pipeline=%lu bytes, GpsHoldout=%lu bytes\n",
               (unsigned long)sizeof(navigation::Pipeline),(unsigned long)sizeof(navigation::GpsHoldout));
    }
}
