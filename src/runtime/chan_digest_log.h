#ifndef MX5_RUNTIME_CHAN_DIGEST_LOG_H
#define MX5_RUNTIME_CHAN_DIGEST_LOG_H
// VIM side-channel rows of the persistent journal profile
// (validation/VIM_CHANNEL_CAPTURE_2026-10-10.md). LOGGING ONLY: nothing here
// feeds the Pipeline, the BETA controller, the adapter or any decision. It
// reads the side-channel batches (raw payload copies of VIM 0x116, 0x169 and
// 0x15B made by the tap) and the accepted wheel events the PersistentLog
// already sees, and writes one compact chan_digest row per PERIOD_NS:
//   "n":[0x116, 0x169, 0x15B messages],
//   "ax":[min,max,mean,stationary mean,stationary n] longitudinal accel raw,
//   "bp":[min,max,nonzero samples] brake pressure raw,
//   "ay":[min,max,mean,stationary mean,stationary n] lateral accel raw,
//   "q":[seen-value bit masks of the 0x116 Qf a, 0x116 Qf b, 0x169 Qf],
//   "v":[last speed raw, value changes, last 0x202 status (-1 unknown)],
//   "rpm":[last rpm raw, value changes],
//   "bad":[short, odd, invalid length, rejected datagrams] (when not all 0),
//   "lost":records the tap reported as dropped in the period (when > 0),
//   "lost0":the tap's cumulative count at the first batch (or after a tap
//     restart): drops before this worker listened, not a loss of the
//     period (written once, in the next row),
//   "wrap":bit 0 ax, bit 1 ay when min < 200 and max > 7900 (a signed
//     13-bit value may have wrapped 8191 <-> 0; min/max/mean are then not
//     meaningful; a hint only, when > 0). Brake pressure is not flagged: it
//     legitimately spans from 0 (released) upwards.
// Raw integers only: no scale, sign or zero is assumed (candidate scales are
// documented in the validation record). A channel without samples is null;
// a mean without samples is null. Stationary: the newest accepted wheel
// event at or before the sample's tap receipt time has all four wheels at
// exactly 0 km/h, is at most WHEEL_GAP_NS older than the sample, and its
// zero run started at least SETTLE_NS before the sample (the yaw data rows'
// definition and settling time: the samples right after the stop still
// hold the deceleration). The tap stamps 0x169/0x15B with the receipt time
// of the latest motion message (at most ~100 ms older than the real one).
// Rows start with the first side-channel batch: without that input the
// profile is byte-identical to the one before this feature.
// Worker thread only; no allocation, no I/O, bounded work per batch.
#include "navigation/channel.h"
#include "navigation/pipeline.h"
#include "sensors/vim_channels.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace mx5 { namespace runtime {

class ChanDigestLog {
public:
    typedef void (*Sink)(void* context,const char* row);
    static const uint64_t PERIOD_NS=20000000000ULL;
    static const uint64_t WHEEL_GAP_NS=500000000ULL;
    static const uint64_t SETTLE_NS=200000000ULL;
    static const unsigned WHEELS=32;              // 3.2 s at 10 Hz
    static const size_t ROW_CAPACITY=400;

    ChanDigestLog() { reset(); }
    void reset() {
        active_=false;since_=0;have_lost_=false;last_lost_=0;lost0_pending_=false;lost0_=0;
        wheel_n_=0;wheel_head_=0;memset(wheels_,0,sizeof wheels_);
        have_v_=have_rpm_=false;prev_v_=prev_rpm_=0;status_=-1;
        period();
    }
    bool active() const { return active_; }

    // Every accepted wheel event (the stationary reference).
    void wheels(const navigation::RawEvent& e,const navigation::ModelProfile& model) {
        if(e.kind!=navigation::WHEELS)return;
        bool zero=true;
        for(unsigned i=0;i<4;++i)if(fabs(e.raw[i]*model.wheel_kmh_per_count+model.wheel_zero_kmh)>=1e-9)zero=false;
        uint64_t since=e.received_ns;
        if(zero && wheel_n_) {
            const Wheel& p=wheels_[(wheel_head_+wheel_n_-1)%WHEELS];
            if(p.zero && e.received_ns>=p.t && e.received_ns-p.t<=WHEEL_GAP_NS)since=p.zero_since;
        }
        Wheel& w=wheels_[(wheel_head_+wheel_n_)%WHEELS];
        if(wheel_n_<WHEELS)++wheel_n_;else wheel_head_=(wheel_head_+1)%WHEELS;
        w.t=e.received_ns;w.zero=zero;w.zero_since=since;
    }
    // One side-channel datagram: decoded, or 0 when the receiver rejected it.
    void batch(const navigation::ChanBatch* b) {
        active_=true;
        if(!b) { ++rejected_;return; }
        // The tap counts since its start (before this worker bound the
        // socket, or across a worker restart): the first value and a value
        // after a tap restart (smaller) are a baseline, not period loss.
        if(have_lost_ && b->lost>=last_lost_)lost_+=b->lost-last_lost_;
        else { lost0_pending_=true;lost0_=b->lost; }
        have_lost_=true;last_lost_=b->lost;
        for(unsigned i=0;i<b->count && i<navigation::CHAN_RECORDS;++i)
            record(b->records[i],b->epoch+uint64_t(b->records[i].dt_ms)*1000000ULL);
    }
    // Worker clock: a row every PERIOD_NS once active.
    void poll(uint64_t now,Sink sink,void* context) {
        if(!active_)return;
        if(!since_) { since_=now?now:1;return; }
        if(now>=since_ && now-since_>=PERIOD_NS)write(now,sink,context);
    }
    // Capture stop: the open period.
    void flush(uint64_t now,Sink sink,void* context) { if(active_)write(now,sink,context); }

private:
    struct Wheel { uint64_t t,zero_since; bool zero; };
    struct Acc {
        uint64_t n,st_n; unsigned min,max; uint64_t sum,st_sum;
        void add(unsigned v,bool still) {
            if(!n || v<min)min=v;
            if(!n || v>max)max=v;
            ++n;sum+=v;
            if(still) { ++st_n;st_sum+=v; }
        }
    };
    void period() {
        n116_=n169_=n15b_=0;ax_=Acc();ay_=Acc();bp_=Acc();bp_nz_=0;
        qa_=qb_=ql_=0;v_chg_=rpm_chg_=0;v_n_=0;
        short_=odd_=invalid_=rejected_=0;lost_=0;
    }
    bool stationary(uint64_t t) const {
        for(unsigned i=0;i<wheel_n_;++i) {
            const Wheel& w=wheels_[(wheel_head_+wheel_n_-1-i)%WHEELS];
            if(w.t>t)continue;
            return w.zero && t-w.t<=WHEEL_GAP_NS && t-w.zero_since>=SETTLE_NS;
        }
        return false;
    }
    void record(const navigation::ChanRecord& r,uint64_t t) {
        if(r.id==0x116)++n116_;else if(r.id==0x169)++n169_;else if(r.id==0x15b)++n15b_;
        else { ++invalid_;return; }
        if(r.length>navigation::CHAN_PAYLOAD) { ++invalid_;return; }
        if(r.id==0x116) {
            sensors::Vim116Extra x;
            const sensors::ChannelParse p=sensors::parse_vim116_extra(r.data,r.length,&x);
            if(p==sensors::CHANNEL_SHORT) { ++short_;return; }
            if(p==sensors::CHANNEL_ODD) { ++odd_;return; }
            const bool still=stationary(t);
            ax_.add(x.accel_long,still);bp_.add(x.brake,false);if(x.brake)++bp_nz_;
            qa_|=1U<<x.qf_a;qb_|=1U<<x.qf_b;
        } else if(r.id==0x169) {
            sensors::Vim169 x;
            const sensors::ChannelParse p=sensors::parse_vim169(r.data,r.length,&x);
            if(p==sensors::CHANNEL_SHORT) { ++short_;return; }
            if(p==sensors::CHANNEL_ODD) { ++odd_;return; }
            ay_.add(x.accel_lat,stationary(t));ql_|=1U<<x.qf;
        } else {
            sensors::Vim15b x;
            const sensors::ChannelParse p=sensors::parse_vim15b(r.data,r.length,&x);
            if(p==sensors::CHANNEL_SHORT) { ++short_;return; }
            if(p==sensors::CHANNEL_ODD) { ++odd_;return; }
            if(have_v_ && x.speed!=prev_v_)++v_chg_;
            if(have_rpm_ && x.rpm!=prev_rpm_)++rpm_chg_;
            have_v_=have_rpm_=true;prev_v_=x.speed;prev_rpm_=x.rpm;status_=x.status;++v_n_;
        }
    }
    static bool wrapped(const Acc& a) { return a.n && a.min<200 && a.max>7900; }
    static void mean(char out[24],uint64_t sum,uint64_t n) {
        if(!n) { strcpy(out,"null");return; }
        snprintf(out,24,"%.1f",double(sum)/double(n));
    }
    static void accel(char out[80],const Acc& a) {
        if(!a.n) { strcpy(out,"null");return; }
        char m[24],s[24];mean(m,a.sum,a.n);mean(s,a.st_sum,a.st_n);
        snprintf(out,80,"[%u,%u,%s,%s,%llu]",a.min,a.max,m,s,(unsigned long long)a.st_n);
    }
    void write(uint64_t now,Sink sink,void* context) {
        char ax[80],ay[80],bp[64],v[64],rpm[48],bad[112],lost[64];
        accel(ax,ax_);accel(ay,ay_);
        if(bp_.n)snprintf(bp,sizeof bp,"[%u,%u,%llu]",bp_.min,bp_.max,(unsigned long long)bp_nz_);
        else strcpy(bp,"null");
        // Last values persist across periods (0x15B is sent only on a change);
        // null before the first 0x15B of the capture.
        if(have_v_) {
            snprintf(v,sizeof v,"[%u,%llu,%d]",prev_v_,(unsigned long long)v_chg_,status_);
            snprintf(rpm,sizeof rpm,"[%u,%llu]",prev_rpm_,(unsigned long long)rpm_chg_);
        } else { strcpy(v,"null");strcpy(rpm,"null"); }
        bad[0]=0;
        if(short_ || odd_ || invalid_ || rejected_)
            snprintf(bad,sizeof bad,",\"bad\":[%llu,%llu,%llu,%llu]",(unsigned long long)short_,
                     (unsigned long long)odd_,(unsigned long long)invalid_,(unsigned long long)rejected_);
        lost[0]=0;
        int k=0;
        if(lost_)k=snprintf(lost,sizeof lost,",\"lost\":%llu",(unsigned long long)lost_);
        if(lost0_pending_ && k>=0 && size_t(k)<sizeof lost)
            snprintf(lost+k,sizeof lost-k,",\"lost0\":%u",lost0_);
        lost0_pending_=false;
        const unsigned wrap=(wrapped(ax_)?1U:0U)|(wrapped(ay_)?2U:0U);
        if(wrap)snprintf(bad+strlen(bad),sizeof bad-strlen(bad),",\"wrap\":%u",wrap);
        char row[ROW_CAPACITY];
        const int n=snprintf(row,sizeof row,
            "{\"kind\":\"chan_digest\",\"schema\":1,\"mono_ns\":%llu,\"n\":[%llu,%llu,%llu],"
            "\"ax\":%s,\"bp\":%s,\"ay\":%s,\"q\":[%u,%u,%u],\"v\":%s,\"rpm\":%s%s%s}",
            (unsigned long long)now,(unsigned long long)n116_,(unsigned long long)n169_,(unsigned long long)n15b_,
            ax,bp,ay,qa_,qb_,ql_,v,rpm,bad,lost);
        if(n>0 && size_t(n)<sizeof row)sink(context,row);
        since_=now?now:1;period();
    }

    bool active_; uint64_t since_;
    bool have_lost_; uint32_t last_lost_; bool lost0_pending_; uint32_t lost0_;
    Wheel wheels_[WHEELS]; unsigned wheel_n_,wheel_head_;
    bool have_v_,have_rpm_; unsigned prev_v_,prev_rpm_; int status_;
    // period
    uint64_t n116_,n169_,n15b_;
    Acc ax_,ay_,bp_; uint64_t bp_nz_;
    unsigned qa_,qb_,ql_;
    uint64_t v_chg_,rpm_chg_,v_n_;
    uint64_t short_,odd_,invalid_,rejected_,lost_;
};

} }
#endif
