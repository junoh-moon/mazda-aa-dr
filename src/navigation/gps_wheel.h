#ifndef MX5_NAVIGATION_GPS_WHEEL_H
#define MX5_NAVIGATION_GPS_WHEEL_H
#include "adapter/adapter.h"
#include <cmath>
#include <stdint.h>

namespace mx5 { namespace navigation {
enum GpsAnchorGate { GPS_GATE_DISABLED=0, GPS_GATE_WAITING, GPS_GATE_ACCEPTED,
    GPS_GATE_BAD_FIX, GPS_GATE_WHEELS, GPS_GATE_SPEED, GPS_GATE_DISPLACEMENT,
    GPS_GATE_COURSE, GPS_GATE_REVERSE };
inline const char* anchor_gate_name(GpsAnchorGate gate) {
    static const char* const names[]={"DISABLED","WAITING","ACCEPTED","BAD_FIX",
        "WHEELS","SPEED","DISPLACEMENT","COURSE","REVERSE"};
    return unsigned(gate)<sizeof names/sizeof names[0]?names[gate]:"UNKNOWN";
}
struct WheelScaleStatus {
    bool enabled, candidate_ready;
    double active_scale, candidate_scale, gps_distance_m, wheel_distance_m;
    // Accepted straight wheel distance since the current training baseline.
    // A reset during the first segment must not erase this attempt silently.
    double partial_training_distance_m;
    uint64_t calibration_version, segments, evidence_end_ns;
};
// Original MODEL samples selected for one GPS anchor. This does not replace
// the chronological wheel/reverse inputs used by distance learning.
struct GpsAnchorSupport {
    uint64_t wheel_time_ns,wheel_received_ns,wheel_lease_ns;
    double wheel_speed_mps,wheel_spread_mps;
    uint64_t reverse_time_ns,reverse_received_ns,reverse_lease_ns;
    int reverse;
};
// A bounded MODEL experiment. GPS gates use travel bearing, never old DR
// geometry. Learning integrates raw wheel speed on the ordered source timeline.
// Defaults are hypotheses for synthetic/vehicle observation, not qualification:
// 1..2s/5m anchor pairs; >=3s/20m straight forward learning segments; >=3
// segments and 100m aggregate; absolute gain 0.95..1.05. No persistence.
// Tests must cover stale source coverage, reverse, outliers, anchor-only apply,
// and fault reset versus normal prediction restart.
class GpsWheel {
public:
    GpsWheel() { configure(false,250000000ULL); }
    void configure(bool enabled,uint64_t age) {
        enabled_=enabled; gap_=age<250000000ULL?age:250000000ULL; reset();
    }
    void reset() {
        status_=WheelScaleStatus(); status_.enabled=enabled_;
        status_.active_scale=status_.candidate_scale=1;
        gate_=enabled_?GPS_GATE_WAITING:GPS_GATE_DISABLED;
        time_=wheel_time_=wheel_received_=reverse_time_=reverse_received_=0;
        yaw_begin_=yaw_end_=yaw_received_=last_fix_time_=last_utc_=utc_changed_=0;
        speed_=spread_=yaw_=distance_=0; reverse_=base_reverse_=0;
        wheels_good_=false; have_base_=have_training_=false; broken_=base_broken_=0;
        training_distance_=0; distance_received_=candidate_received_=0;
    }
    void restart_prediction() {
        const double scale=status_.active_scale;
        const uint64_t version=status_.calibration_version;
        reset(); status_.active_scale=scale; status_.calibration_version=version;
    }
    const WheelScaleStatus& status() const { return status_; }
    GpsAnchorGate gate() const { return gate_; }
    void unavailable(GpsAnchorGate reason=GPS_GATE_WAITING) {
        if (!enabled_) return;
        have_base_=false; last_fix_time_=0; clear_training(); gate_=reason;
    }
    void advance(uint64_t end) {
        if (!enabled_ || end<=time_) return;
        if (time_) {
            bool clean=wheels_good_ && speed_>=2 && reverse_==0 &&
                fresh(wheel_time_,wheel_received_,end) &&
                fresh(reverse_time_,reverse_received_,end) &&
                yaw_begin_<=time_ && yaw_end_>=end &&
                yaw_received_>=yaw_end_ && yaw_received_-yaw_end_<=gap_ &&
                std::fabs(yaw_)<=0.03;
            if (clean) {
                distance_+=speed_*double(end-time_)/1e9;
                if (have_training_ && !status_.candidate_ready)
                    status_.partial_training_distance_m=distance_-training_distance_;
                // A completed MODEL mean window may cover shorter wheel/GPS
                // subintervals. Retain its actual receipt frontier so none of
                // this evidence can change an earlier anchor retrospectively.
                if (yaw_received_>distance_received_) distance_received_=yaw_received_;
                if (wheel_received_>distance_received_) distance_received_=wheel_received_;
                if (reverse_received_>distance_received_) distance_received_=reverse_received_;
            }
            else break_training();
        }
        time_=end;
    }
    void wheels(uint64_t time,uint64_t received,double speed,double spread) {
        if (!enabled_) return;
        const bool agreement=spread<=maximum(0.3,speed*0.05);
        if (!agreement || received<time || received-time>gap_ ||
            (wheel_time_ && (time<=wheel_time_ || time-wheel_time_>gap_ ||
            std::fabs(speed-speed_)/(double(time-wheel_time_)/1e9)>1.0)))
            break_training();
        speed_=speed; spread_=spread; wheel_time_=time; wheel_received_=received;
        wheels_good_=agreement;
    }
    void reverse(uint64_t time,uint64_t received,int value) {
        if (!enabled_) return;
        if (value || (reverse_time_ && reverse_!=value)) break_training();
        reverse_=value; reverse_time_=time; reverse_received_=received;
    }
    void yaw(uint64_t begin,uint64_t end,uint64_t received,double value) {
        if (!enabled_) return;
        yaw_begin_=begin; yaw_end_=end; yaw_received_=received; yaw_=value;
        if (std::fabs(value)>0.03) break_training();
    }
    bool fix(const adapter::Observation& o) {
        GpsAnchorSupport support=GpsAnchorSupport();
        support.wheel_time_ns=wheel_time_;support.wheel_received_ns=wheel_received_;
        support.wheel_lease_ns=lease(wheel_time_);
        support.wheel_speed_mps=speed_;support.wheel_spread_mps=spread_;
        support.reverse_time_ns=reverse_time_;support.reverse_received_ns=reverse_received_;
        support.reverse_lease_ns=lease(reverse_time_);support.reverse=reverse_;
        return fix(o,support);
    }
    bool fix(const adapter::Observation& o,const GpsAnchorSupport& support) {
        if (!enabled_) return false;
        if (support.wheel_spread_mps>maximum(0.3,support.wheel_speed_mps*0.05) ||
            !std::isfinite(support.wheel_speed_mps)||!std::isfinite(support.wheel_spread_mps)||
            support.wheel_lease_ns<o.mono_ns ||
            !fresh(support.wheel_time_ns,support.wheel_received_ns,o.mono_ns))
            return reject(GPS_GATE_WHEELS);
        if ((support.reverse!=0&&support.reverse!=1)||support.reverse_lease_ns<o.mono_ns ||
            !fresh(support.reverse_time_ns,support.reverse_received_ns,o.mono_ns))
            return reject(GPS_GATE_REVERSE);
        const double gps_speed=o.position.velocity_kmh/3.6;
        if (support.wheel_speed_mps<0.5 || std::fabs(gps_speed-support.wheel_speed_mps*status_.active_scale)>
            maximum(3.0,gps_speed*0.30)) return reject(GPS_GATE_SPEED);
        if (last_fix_time_ && (o.mono_ns<=last_fix_time_ ||
            o.mono_ns-last_fix_time_>2000000000ULL ||
            o.position.utc_seconds<last_utc_ ||
            o.position.utc_seconds-last_utc_>
                (o.mono_ns-last_fix_time_)/1000000000ULL+2)) {
            last_utc_=o.position.utc_seconds; utc_changed_=o.mono_ns;
            return reject(GPS_GATE_BAD_FIX);
        }
        if (!utc_changed_ || o.position.utc_seconds!=last_utc_) utc_changed_=o.mono_ns;
        else if (o.mono_ns-utc_changed_>2000000000ULL) return reject(GPS_GATE_BAD_FIX);
        last_fix_time_=o.mono_ns; last_utc_=o.position.utc_seconds;
        if (!have_base_) { base(o,support.reverse); gate_=GPS_GATE_WAITING; return false; }
        if (support.reverse!=base_reverse_) return reject(GPS_GATE_REVERSE);
        const uint64_t elapsed=o.mono_ns-base_.mono_ns;
        const double dt=double(elapsed)/1e9;
        double lon=o.position.longitude_deg-base_.position.longitude_deg;
        if (lon>180) lon-=360;
        if (lon< -180) lon+=360;
        const double north=(o.position.latitude_deg-base_.position.latitude_deg)*111320;
        const double east=lon*111320*std::cos((o.position.latitude_deg+
            base_.position.latitude_deg)*0.5*pi()/180);
        const double gps_distance=std::sqrt(north*north+east*east);
        const double expected=(gps_speed+base_.position.velocity_kmh/3.6)*0.5*dt;
        if (elapsed>2000000000ULL ||
            std::fabs(gps_distance-expected)>maximum(8.0,expected*0.40))
            return reject(GPS_GATE_DISPLACEMENT);
        const double course=std::atan2(east,north)*180/pi();
        if (gps_distance>=5 && (angle(course,o.position.heading_deg)>35 ||
            angle(course,base_.position.heading_deg)>35)) return reject(GPS_GATE_COURSE);
        // Gross jumps cannot shelter behind the minimum pair interval. Retain
        // the baseline on plausible faster callbacks; do not infer a bearing
        // from less than 5m of displacement.
        if (elapsed<1000000000ULL) { gate_=GPS_GATE_WAITING; return false; }
        if (gps_distance<5) return reject(GPS_GATE_DISPLACEMENT);
        const bool straight=angle(o.position.heading_deg,base_.position.heading_deg)<=5;
        if (support.reverse!=0 || base_broken_!=broken_ || !straight || gps_speed<2)
            clear_training();
        else if (have_training_ && !status_.candidate_ready) {
            // Longer independent endpoint segments avoid judging a 5% scale
            // from only 10m of GPS displacement at each anchor pair.
            const double raw_distance=distance_-training_distance_;
            if (angle(o.position.heading_deg,training_.position.heading_deg)>5)
                clear_training();
            else if (o.mono_ns-training_.mono_ns>=3000000000ULL && raw_distance>=20) {
                double training_lon=o.position.longitude_deg-training_.position.longitude_deg;
                if (training_lon>180) training_lon-=360;
                if (training_lon< -180) training_lon+=360;
                const double n=(o.position.latitude_deg-training_.position.latitude_deg)*111320;
                const double e=training_lon*111320*std::cos((o.position.latitude_deg+
                    training_.position.latitude_deg)*0.5*pi()/180);
                const double gps_distance=std::sqrt(n*n+e*e);
                const double ratio=gps_distance/raw_distance;
                if (ratio<0.95 || ratio>1.05) clear_training();
                else {
                    ++status_.segments; status_.gps_distance_m+=gps_distance;
                    status_.wheel_distance_m+=raw_distance; status_.evidence_end_ns=o.mono_ns;
                    if (status_.segments>=3 && status_.gps_distance_m>=100) {
                        status_.candidate_scale=status_.gps_distance_m/status_.wheel_distance_m;
                        status_.candidate_ready=true;
                        candidate_received_=distance_received_;
                    }
                    training_=o; training_distance_=distance_;
                    status_.partial_training_distance_m=0;
                }
            }
        }
        base(o,support.reverse); gate_=GPS_GATE_ACCEPTED; return true;
    }
    void apply_at_anchor(uint64_t time) {
        if (!enabled_ || !status_.candidate_ready || time<=status_.evidence_end_ns ||
            time<candidate_received_) return;
        if (time-status_.evidence_end_ns>30000000000ULL ||
            status_.calibration_version==UINT64_MAX) { clear_training(); return; }
        status_.active_scale=status_.candidate_scale; ++status_.calibration_version;
        clear_training();
    }
private:
    bool enabled_,wheels_good_,have_base_,have_training_;
    uint64_t gap_,time_,wheel_time_,wheel_received_,reverse_time_,reverse_received_;
    uint64_t yaw_begin_,yaw_end_,yaw_received_,last_fix_time_,last_utc_,utc_changed_,broken_,base_broken_;
    uint64_t distance_received_,candidate_received_;
    double speed_,spread_,yaw_,distance_,training_distance_;
    int reverse_,base_reverse_;
    adapter::Observation base_,training_;
    WheelScaleStatus status_;
    GpsAnchorGate gate_;
    static double pi() { return 3.14159265358979323846; }
    static double maximum(double a,double b) { return a>b?a:b; }
    static double angle(double a,double b) {
        double d=std::fmod(std::fabs(a-b),360.0); return d>180?360-d:d;
    }
    bool fresh(uint64_t time,uint64_t received,uint64_t now) const {
        return time && time<=now && received<=now && received>=time && now-time<=gap_;
    }
    uint64_t lease(uint64_t time) const { return UINT64_MAX-time<gap_?UINT64_MAX:time+gap_; }
    void clear_training() {
        have_training_=false; candidate_received_=0;
        status_.candidate_ready=false; status_.candidate_scale=1;
        status_.segments=0; status_.gps_distance_m=status_.wheel_distance_m=0;
        status_.evidence_end_ns=0;status_.partial_training_distance_m=0;
    }
    void break_training() { ++broken_; clear_training(); }
    bool reject(GpsAnchorGate reason) { unavailable(reason); return false; }
    void base(const adapter::Observation& o,int reverse) {
        base_=o; have_base_=true;
        base_broken_=broken_; base_reverse_=reverse;
        if (!have_training_) {
            training_=o; training_distance_=distance_; have_training_=true;
            status_.partial_training_distance_m=0;
        }
    }
};
} }
#endif
