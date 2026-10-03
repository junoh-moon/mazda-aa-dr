#ifndef MX5_NAVIGATION_GYRO_BIAS_H
#define MX5_NAVIGATION_GYRO_BIAS_H
#include <stdint.h>
#include <cmath>

namespace mx5 { namespace navigation {
// All thresholds and evidence here are MODEL hypotheses, never a physical
// calibration claim. Raw yaw is the same integer sum/count mean used by DR.
enum GyroBiasState { GYRO_BIAS_DISABLED=0, GYRO_BIAS_WAITING,
    GYRO_BIAS_COLLECTING, GYRO_BIAS_READY, GYRO_BIAS_APPLIED };
struct GyroBiasStatus {
    bool enabled, candidate_ready;
    GyroBiasState state;
    double active_zero, candidate_zero, variance_counts2;
    uint64_t calibration_version, samples, evidence_start_ns, evidence_end_ns;
};
class GyroBias {
public:
    GyroBias() { configure(false,2047,250000000ULL); }
    void configure(bool enabled,double nominal,uint64_t age) {
        nominal_=nominal; gap_=age<250000000ULL?age:250000000ULL;
        enabled_=enabled; reset();
    }
    void reset() {
        status_=GyroBiasStatus(); status_.enabled=enabled_;
        status_.active_zero=status_.candidate_zero=nominal_;
        status_.state=enabled_?GYRO_BIAS_WAITING:GYRO_BIAS_DISABLED;
        wheel_time_=wheel_received_=stationary_since_=yaw_time_=yaw_received_=0;
        wheel_clock_=yaw_clock_=-1; candidate_received_=0; clear_collection();
    }
    // A completed holdout may discard prediction geometry without forgetting
    // the applied model. Partial and pending stationary evidence never carries.
    void restart_prediction() {
        const double active=status_.active_zero;
        const uint64_t version=status_.calibration_version;
        reset(); status_.active_zero=active; status_.calibration_version=version;
        if (enabled_ && version) status_.state=GYRO_BIAS_APPLIED;
    }
    const GyroBiasStatus& status() const { return status_; }
    void wheels(uint64_t time,uint64_t received,bool transport,const double mps[4]) {
        if (!enabled_) return;
        if (wheel_clock_!=-1 && wheel_clock_!=int(transport)) { reset(); }
        wheel_clock_=int(transport);
        bool fresh=received>=time && received-time<=gap_;
        if (wheel_time_ && (time<=wheel_time_ || time-wheel_time_>gap_ ||
            received<wheel_received_ || received-wheel_received_>gap_)) {
            invalidate(); stationary_since_=0;
        }
        wheel_time_=time; wheel_received_=received;
        bool stopped=fresh;
        for (unsigned i=0;i<4;++i)
            stopped=stopped&&std::isfinite(mps[i])&&mps[i]>=0&&mps[i]<=0.05;
        if (!fresh) invalidate();
        if (!stopped) { stationary_since_=0; clear_collection(); return; }
        if (!stationary_since_) stationary_since_=time;
    }
    void yaw(uint64_t begin,uint64_t end,uint64_t received,bool transport,double mean) {
        if (!enabled_) return;
        if (yaw_clock_!=-1 && yaw_clock_!=int(transport)) { reset(); }
        yaw_clock_=int(transport);
        bool fresh=begin && end>begin && end-begin<=gap_ && received>=end &&
            received-end<=gap_ && (!yaw_time_ || (begin==yaw_time_ &&
            received>=yaw_received_ && received-yaw_received_<=gap_));
        yaw_time_=end; yaw_received_=received;
        if (!fresh || !wheel_time_ || wheel_time_>end || end-wheel_time_>gap_) {
            invalidate(); return;
        }
        if (!stationary_since_ || stationary_since_>begin) { clear_collection(); return; }
        if (!std::isfinite(mean)||std::fabs(mean-nominal_)>64) { invalidate(); return; }
        // Robust accumulation (MODEL hypothesis tuned on one public ND log, not a
        // calibration claim). Once ROBUST_MIN windows agree, an isolated outlier is
        // skipped rather than discarding the evidence. A run of outliers or too
        // many of them is a disturbance: restart this collection, but keep a candidate
        // that already reached READY (its age is still bounded at the anchor).
        if (n_>=ROBUST_MIN && std::fabs(mean-mean_)>OUTLIER_COUNTS) {
            ++outliers_; ++outlier_run_;
            if (outlier_run_>=3 || outliers_>2+n_/10) clear_collection();
            return;
        }
        outlier_run_=0;
        if (!n_) { start_=begin; mean_=low_=high_=mean; m2_=0; n_=1; }
        else {
            const double low=mean<low_?mean:low_,high=mean>high_?mean:high_;
            const double delta=mean-mean_;
            const double next_mean=mean_+delta/double(n_+1);
            const double next_m2=m2_+delta*(mean-next_mean);
            // The spread/variance gates need a few windows; two windows alone say little.
            if (n_+1>=ROBUST_MIN && (high-low>4 || next_m2/double(n_+1)>1.0)) {
                clear_collection(); return;
            }
            low_=low; high_=high; mean_=next_mean; m2_=next_m2; ++n_;
        }
        const uint64_t received_through=received>wheel_received_?received:wheel_received_;
        if(received_through>collection_received_)collection_received_=received_through;
        status_.state=GYRO_BIAS_COLLECTING;
        if (n_>=20 && end-start_>=3000000000ULL) {
            status_.candidate_ready=true; status_.candidate_zero=mean_;
            status_.samples=n_; status_.variance_counts2=m2_/double(n_);
            status_.evidence_start_ns=start_; status_.evidence_end_ns=end;
            candidate_received_=collection_received_;
            status_.state=GYRO_BIAS_READY;
        }
        // Bound both memory and accumulator duration. A completed estimate is
        // retained while the next independent stationary window accumulates.
        if (n_>=4096) clear_collection();
    }
    bool apply_at_anchor(uint64_t time) {
        // Measurement endpoints alone do not establish when the candidate was
        // available. Preserve it and the active zero until all of its actual
        // wheel/yaw receipts are causal for the new anchor.
        if (!enabled_ || !status_.candidate_ready || time<status_.evidence_end_ns ||
            time<candidate_received_)
            return false;
        if (time-status_.evidence_end_ns>30000000000ULL) { invalidate(); return false; }
        if (status_.calibration_version==UINT64_MAX) { invalidate(); return false; }
        status_.active_zero=status_.candidate_zero; ++status_.calibration_version;
        status_.candidate_ready=false; status_.state=GYRO_BIAS_APPLIED;
        candidate_received_=0;
        return true;
    }
private:
    bool enabled_;
    double nominal_,mean_,m2_,low_,high_;
    uint64_t gap_,wheel_time_,wheel_received_,stationary_since_,yaw_time_,yaw_received_,start_,n_;
    uint64_t collection_received_,candidate_received_;
    unsigned outliers_,outlier_run_;
    static const unsigned ROBUST_MIN=8;
    static constexpr double OUTLIER_COUNTS=3.0;
    int wheel_clock_,yaw_clock_;
    GyroBiasStatus status_;
    void clear_collection() {
        n_=start_=collection_received_=0; mean_=m2_=low_=high_=0;
        outliers_=outlier_run_=0;
        if (enabled_) status_.state=status_.candidate_ready?GYRO_BIAS_READY:
            (status_.calibration_version?GYRO_BIAS_APPLIED:GYRO_BIAS_WAITING);
    }
    void invalidate() {
        status_.candidate_ready=false; status_.samples=0;
        candidate_received_=0;
        status_.evidence_start_ns=status_.evidence_end_ns=0;
        status_.variance_counts2=0; status_.candidate_zero=nominal_;
        clear_collection();
    }
};
} }
#endif
