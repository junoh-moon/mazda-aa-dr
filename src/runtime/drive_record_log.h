#ifndef MX5_RUNTIME_DRIVE_RECORD_LOG_H
#define MX5_RUNTIME_DRIVE_RECORD_LOG_H
// Drive data rows of the persistent journal profile (v1.0.0-beta.14
// candidate, validation/RELEASE_V100_BETA14_CANDIDATE_2026-10-13.md).
// RECORD-ONLY: nothing here feeds the Pipeline, the BETA controller, the
// adapter or any decision. It reads the accepted motion events and the
// POSITION/SEND observations the PersistentLog already sees and writes:
//  * fix1: one compact row per new stock fix (decoded mode 1/2/3, utc > 0,
//    not a re-poll of the same fix) while moving (fix or wheel speed > 5
//    km/h): fix values at full precision, the accuracy of the same call's
//    LOCATION send, the satellites in use of the same call's GPS_SATELLITE
//    send (observed only in the diagnostic build; -1 otherwise), and the
//    yaw/wheel sums received since the previous stock fix (the 2026-10-13
//    studies only had these inside RAW windows);
//  * exit_jump: at the first stock fix (FIX or NATIVE_DR) after a GPS-lost
//    episode in which at least one LOCATION was sent: the jump between the
//    LAST LOCATION sent before that fix (of any kind: a BETA replacement, or
//    the stock LOCATION with its frozen coordinates after a withdrawal; it
//    is what the phone last received) and the fix, split along/across the
//    sent bearing, and the two Naver jump predicates R1/R2 of
//    validation/EXIT_JUMP_BLEND_2026-10-13.md section 0 with their inputs;
//  * yaw_learn: the GPS-learned yaw zero of
//    validation/YAW_DEADBAND_FABLE_2026-10-13.md (W = 40 s of contiguous
//    fixes >= 10 km/h, course deviation <= 10 deg, rate corrected), written
//    at each GPS-lost episode start and every 60 s while it is valid. It is
//    never used by the DR (the BETA zero stays the profile constant);
//  * rec_drop: counts of the rows above that the gate refused (with the
//    reason) or a rate limit suppressed, at most one row per 60 s.
// Every row kind has a rate limit, so the worst case (GPS flapping every
// second) stays within the stated budget (worst_case_bytes_per_hour()).
// Rows go through a Sink that returns a Gate reason (GATE_OPEN: written;
// otherwise the row was dropped, never queued, and is counted).
// Times: motion by producer receipt (received_ns), POSITION/SEND by hook
// time (mono_ns), rows by the worker clock; all CLOCK_MONOTONIC.
// Worker thread only; no allocation, no I/O, bounded work per call (at most
// FIXES fixes and BINS bins per estimate).
#include "adapter/adapter.h"
#include "navigation/pipeline.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace mx5 { namespace runtime {

class DriveRecordLog {
public:
    enum Row { FIX1=0, EXIT=1, LEARN=2, DROP=3, ROWS=4 };
    // Why a row was not written: the journal failed or is not current, no
    // writer thread (synchronous journal: rows never written from the
    // worker), the writer lags, its queue is above the limit.
    enum Gate { GATE_OPEN=0, GATE_JOURNAL=1, GATE_NO_WRITER=2, GATE_LAG=3, GATE_QUEUE=4, GATES=5 };
    typedef unsigned (*Sink)(void* context,const char* row);
    static const uint64_t MS=1000000ULL, SEC=1000000000ULL;
    static const uint64_t PAIR_NS=1500*MS;        // a POSITION waits this long for its LOCATION send
    static const uint64_t BIN_NS=100*MS;          // yaw history bins
    static const unsigned BINS=512;               // 51.2 s of yaw history
    static const unsigned FIXES=64;               // learned-zero fix history
    static const uint64_t YAW_COVER_CAP_NS=250*MS;
    static const uint64_t LEARN_W_NS=40*SEC, LEARN_SLACK_NS=3*SEC, LEARN_GAP_NS=2500*MS;
    static const uint64_t LEARN_PERIOD_NS=60*SEC, DROP_PERIOD_NS=60*SEC, DROP_RETRY_NS=SEC;
    // Rate limits (token buckets): refill period and burst per limited kind.
    enum Limit { L_FIX1=0, L_EXIT=1, L_LOST=2, LIMITS=3 };
    // fix1: the 1 Hz GPS rate with a burst for receipt jitter.
    static uint64_t refill_ns(unsigned l) { return l==L_FIX1?SEC:60*SEC; }
    static unsigned burst(unsigned l) { return l==L_FIX1?8:3; }
    static const size_t ROW_CAPACITY=640;
    static double moving_kmh() { return 5.0; }
    static double learn_kmh() { return 10.0; }
    static double learn_dev_deg() { return 10.0; }
    static double learn_limit_counts() { return 10.0; }
    // Longest row of each kind with its newline (checked by the unit test
    // with extreme values) and the upper bound of the bytes per hour from
    // the rate limits: the periodic yaw_learn rows are at most one per
    // LEARN_PERIOD_NS, rec_drop one per DROP_PERIOD_NS.
    static size_t max_row_bytes(unsigned r) { return r==FIX1?130:r==EXIT?ROW_CAPACITY:360; }
    static double worst_case_bytes_per_hour() {
        double rows[LIMITS];
        for(unsigned l=0;l<LIMITS;++l)rows[l]=3600.0*SEC/double(refill_ns(l))+burst(l);
        return rows[L_FIX1]*max_row_bytes(FIX1)+rows[L_EXIT]*max_row_bytes(EXIT)+rows[L_LOST]*max_row_bytes(LEARN)+
               (3600.0*SEC/LEARN_PERIOD_NS+1)*max_row_bytes(LEARN)+(3600.0*SEC/DROP_PERIOD_NS+1)*max_row_bytes(DROP);
    }

    DriveRecordLog() { reset(); }
    void reset() {
        for(unsigned i=0;i<BINS;++i) { bins_[i].idx=UINT64_MAX;bins_[i].ysum=0;bins_[i].yn=0;bins_[i].cover=0; }
        newest_bin_=0;have_yaw_=false;last_yaw_t_=0;
        acc_=Acc();fix_n_=fix_head_=0;learn_=Learn();learn_ok_=0;last_learn_row_=0;
        pending_=Pending();ep_=Episode();episodes_=0;sent_=Sent();
        have_cls_=false;last_cls_=0;have_key_=false;memset(&key_,0,sizeof key_);
        diag_seen_=false;zero_in_use_=2048.0;rad_per_count_=-0.000658615;
        for(unsigned l=0;l<LIMITS;++l) { tokens_[l]=burst(l)*1000ULL;refilled_[l]=0;rate_[l]=0; }
        for(unsigned r=0;r<ROWS;++r)drops_[r]=0;
        for(unsigned g=0;g<GATES;++g)why_[g]=0;
        next_drop_=0;
    }
    // The yaw zero the BETA dead reckoning uses (journaled as "used").
    void set_zero_in_use(double counts) { zero_in_use_=counts; }
    // The model's signed yaw scale (rad per count) for the rate correction.
    void set_model(const navigation::ModelProfile& m) { rad_per_count_=m.yaw_rad_per_count; }
    static const char* name(unsigned r) {
        static const char* const names[]={"fix1","exit_jump","yaw_learn","rec_drop"};
        return r<ROWS?names[r]:"other";
    }
    static const char* gate_name(unsigned g) {
        static const char* const names[]={"open","journal","no_writer","lag","queue"};
        return g<GATES?names[g]:"other";
    }
    uint64_t dropped(unsigned r) const { return r<ROWS?drops_[r]:0; }
    uint64_t dropped_why(unsigned g) const { return g<GATES?why_[g]:0; }
    uint64_t rate_limited(unsigned l=L_FIX1) const { return l<LIMITS?rate_[l]:0; }

    // ---- inputs ----
    void motion(const navigation::RawEvent& e,const navigation::ModelProfile& model) {
        if(e.kind==navigation::WHEELS) {
            double v=0;
            for(unsigned i=0;i<4;++i)v+=(e.raw[i]*model.wheel_kmh_per_count+model.wheel_zero_kmh)*0.25;
            acc_.wsum+=v;++acc_.wn;
            return;
        }
        if(e.kind!=navigation::YAW)return;
        // Same validity as the yaw study rows (no 4095 marker, representable).
        const bool valid=e.count && e.count<=255 && uint32_t(e.raw[0])+65536U>uint32_t(e.count)*4095U &&
                         unsigned(e.raw[0])/e.count<4094;
        const uint64_t t=e.received_ns;
        uint64_t cover=0;
        if(have_yaw_ && t>last_yaw_t_)cover=t-last_yaw_t_<YAW_COVER_CAP_NS?t-last_yaw_t_:YAW_COVER_CAP_NS;
        if(!have_yaw_ || t>last_yaw_t_)last_yaw_t_=t;
        have_yaw_=true;
        if(!valid)return;
        const int64_t dev=int64_t(e.raw[0])-2048*int64_t(e.count);
        acc_.ysum+=dev;acc_.yn+=e.count;
        if(Bin* b=bin(t)) { b->ysum+=dev;b->yn+=e.count;b->cover+=cover; }
    }
    // Every POSITION/SEND observation in queue order. honest_m: the BETA
    // controller's honest budget of its latest publication when the worker
    // reads this observation (-1: none); it may be one tick newer than the
    // send it is stored with.
    void observation(const adapter::Observation& o,double honest_m,uint64_t now,Sink sink,void* context) {
        if(o.kind==adapter::Observation::SEND) { send(o,honest_m,now,sink,context);return; }
        if(o.kind!=adapter::Observation::POSITION)return;
        if(pending_.have)finish(now,sink,context);
        if(o.position_class==adapter::POSITION_UNDECODED)return;
        const adapter::PositionInput& p=o.position;
        const unsigned cls=unsigned(o.position_class);
        const uint64_t t=o.mono_ns;
        if(o.position_class==adapter::POSITION_LOST) {
            if(!have_cls_ || last_cls_!=unsigned(adapter::POSITION_LOST)) {
                if(allow(L_LOST,now))write_learn(now,t,"lost",sink,context);
                else ++rate_[L_LOST];
                if(!ep_.open) { ep_=Episode();ep_.open=true;ep_.lost_ns=t; }
            }
        } else if(p.mode>=1 && p.mode<=3 && p.utc_seconds && is_finite(p.latitude_deg) && is_finite(p.longitude_deg)) {
            Key k;memset(&k,0,sizeof k);
            k.utc=p.utc_seconds;k.lat=p.latitude_deg;k.lon=p.longitude_deg;k.heading=p.heading_deg;
            const bool repoll=have_key_ && !memcmp(&k,&key_,sizeof k);
            key_=k;have_key_=true;
            const bool ends=ep_.open && (o.position_class==adapter::POSITION_FIX ||
                                         o.position_class==adapter::POSITION_NATIVE_DR);
            if(!repoll || ends) {
                pending_=Pending();pending_.have=true;pending_.call=o.call_sequence;pending_.t=t;
                pending_.mode=p.mode;pending_.lat=p.latitude_deg;pending_.lon=p.longitude_deg;
                pending_.heading=p.heading_deg;pending_.kmh=p.velocity_kmh;pending_.repoll=repoll;pending_.sat=-1;
                pending_.acc=acc_;
                if(!repoll) { acc_=Acc();acc_.since=t; }
                if(ends) {
                    // The episode ends here; the anchor is the last LOCATION
                    // sent before this fix (its own LOCATION follows it).
                    pending_.exit=(ep_.reps || ep_.stock) && sent_.have;
                    pending_.ep=ep_;pending_.anchor=sent_;ep_=Episode();
                }
            }
            if(!repoll && o.position_class==adapter::POSITION_FIX) {
                learn(t,p.heading_deg,p.velocity_kmh);
                if(learn_.have && (!last_learn_row_ || t>=last_learn_row_+LEARN_PERIOD_NS))
                    write_learn(now,t,"periodic",sink,context);
            }
        }
        have_cls_=true;last_cls_=cls;
    }
    // Worker turn: an unpaired POSITION is written from its own values; the
    // drop counter row is due.
    void poll(uint64_t now,Sink sink,void* context) {
        if(pending_.have && now>pending_.t && now-pending_.t>PAIR_NS)finish(now,sink,context);
        if(due_drops() && now>=next_drop_)write_drops(now,sink,context);
    }
    // Capture stop.
    void flush(uint64_t now,Sink sink,void* context) {
        if(pending_.have)finish(now,sink,context);
        if(due_drops())write_drops(now,sink,context);
    }

    // ---- row builders (public for the unit tests) ----
    struct Jump {
        double jump_m,along_m,cross_m,dbrg_deg,implied_kmh;
        bool has_brg,r1_acc,r1_dt,r1_dist,r1_speed,r1,r2;
    };
    // R1 (GpsAdjuster.p): previous accuracy 0 <= a < 30 m, 500 <= dt <= 1500
    // ms, distance >= 50 m and > 2 x max(speeds) m/s. R2 (ValidGpsSelector):
    // the implied speed over dt <= 3 s above 200 km/h. Distances on a local
    // equirectangular plane; along/cross relative to the sent bearing (cross
    // positive to the right).
    static Jump jump(double dr_lat,double dr_lon,double fix_lat,double fix_lon,bool has_brg,double dr_brg_deg,
                     double fix_brg_deg,double dr_acc_m,double dr_mps,double fix_mps,double dt_ms) {
        static const double PI=3.14159265358979323846,M_PER_DEG=6371008.8*PI/180.0;
        Jump j;memset(&j,0,sizeof j);
        const double north=(fix_lat-dr_lat)*M_PER_DEG;
        double dlon=fix_lon-dr_lon;
        if(dlon>180)dlon-=360;else if(dlon<-180)dlon+=360;
        const double east=dlon*M_PER_DEG*cos(0.5*(fix_lat+dr_lat)*PI/180.0);
        j.jump_m=sqrt(north*north+east*east);
        j.has_brg=has_brg;
        if(has_brg) {
            const double b=dr_brg_deg*PI/180.0;
            j.along_m=north*cos(b)+east*sin(b);j.cross_m=-north*sin(b)+east*cos(b);
            j.dbrg_deg=wrap180(fix_brg_deg-dr_brg_deg);
        }
        const double vmax=(dr_mps>fix_mps?dr_mps:fix_mps);
        j.r1_acc=dr_acc_m>=0 && dr_acc_m<30.0;
        j.r1_dt=dt_ms>=500 && dt_ms<=1500;
        j.r1_dist=j.jump_m>=50.0;
        j.r1_speed=j.jump_m>2.0*(vmax>0?vmax:0);
        j.r1=j.r1_acc && j.r1_dt && j.r1_dist && j.r1_speed;
        j.implied_kmh=dt_ms>0?j.jump_m/(dt_ms/1000.0)*3.6:-1;
        j.r2=dt_ms>0 && dt_ms<=3000 && j.implied_kmh>200.0;
        return j;
    }
    static double wrap180(double d) {
        if(!is_finite(d))return d;
        d=fmod(d,360.0);if(d>180)d-=360;if(d<=-180)d+=360;
        return d;
    }
    // The newest learned-zero estimate.
    struct Learn {
        bool have; double zero,mean,span_s,dc_deg,dev_deg,cover; uint64_t at; unsigned fixes;
    };
    const Learn& learned() const { return learn_; }

private:
    struct Bin { uint64_t idx; int64_t ysum; uint64_t yn; uint64_t cover; };
    struct Acc { int64_t ysum; uint64_t yn; double wsum; uint64_t wn; uint64_t since; };
    struct Fix { uint64_t t; double course,kmh; bool valid; };
    struct Key { uint64_t utc; double lat,lon,heading; };
    // The last LOCATION (type 1) sent, as it left (outgoing bytes).
    struct Sent {
        bool have,has_brg; uint64_t t; int choice; double lat,lon,acc_m,mps,brg;
    };
    struct Episode {
        bool open; uint64_t lost_ns,first_rep,last_rep; unsigned reps,stock,stock_after; double honest;
    };
    struct Pending {
        bool have,paired,repoll,exit; uint32_t call; uint64_t t,send_t; int32_t mode; int64_t sat;
        double lat,lon,heading,kmh,acc_m; Acc acc; Episode ep; Sent anchor;
    };
    static bool is_finite(double v) { return v==v && v<1e300 && v>-1e300; }
    static uint32_t get32(const uint8_t* p) {
        return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);
    }
    void send(const adapter::Observation& o,double honest_m,uint64_t now,Sink sink,void* context) {
        // Diagnostic build: GPS_SATELLITE follows the LOCATION of the same
        // call (RequestSendPosition); once one was seen, a fix waits for it.
        if(o.diag_sat) {
            diag_seen_=true;
            if(pending_.have && o.call_sequence==pending_.call) {
                pending_.sat=o.diag_sat_short?-1:int64_t(o.diag_sat_before);
                finish(now,sink,context);
            }
            return;
        }
        if(o.type!=1 || !o.has_payload)return;
        if(pending_.have && o.call_sequence==pending_.call) {
            pending_.paired=true;pending_.send_t=o.mono_ns;
            pending_.acc_m=o.original[16]?get32(o.original+20)/1000.0:-1;
        }
        // What left: the replacement or the original bytes.
        const uint8_t* b=o.outgoing;
        sent_.have=true;sent_.t=o.mono_ns;sent_.choice=int(o.choice);
        sent_.lat=int32_t(get32(b+8))/1e7;sent_.lon=int32_t(get32(b+12))/1e7;
        sent_.acc_m=b[16]?get32(b+20)/1000.0:-1;
        sent_.mps=b[32]?int32_t(get32(b+36))/1000.0:-1;
        sent_.has_brg=b[40]!=0;sent_.brg=get32(b+44)/1e6;
        if(ep_.open) {
            if(o.choice==adapter::BETA_REPLACEMENT) {
                if(!ep_.reps)ep_.first_rep=o.mono_ns;
                ++ep_.reps;ep_.last_rep=o.mono_ns;ep_.stock_after=0;ep_.honest=honest_m;
            } else { ++ep_.stock;++ep_.stock_after; }
        }
        if(pending_.have && pending_.paired && o.call_sequence==pending_.call && !diag_seen_)finish(now,sink,context);
    }
    Bin* bin(uint64_t t) {
        const uint64_t idx=t/BIN_NS;
        if(newest_bin_>=BINS && idx<=newest_bin_-BINS)return 0;
        if(idx>newest_bin_)newest_bin_=idx;
        Bin& b=bins_[idx%BINS];
        if(b.idx!=idx) { b.idx=idx;b.ysum=0;b.yn=0;b.cover=0; }
        return &b;
    }
    // Fable 2026-10-13 G W=40 S=10 r1 >= 10 km/h, at every new FIX: walk
    // back through contiguous fixes (gaps <= 2.5 s) all >= 10 km/h to the
    // first one with a valid course >= W old; the course deviation from the
    // current course must stay <= 10 deg (fixes without a valid course keep
    // the chain but are not compared); the yaw bins inside the span must
    // cover >= 90 % of it. zero = mean counts - (course change / span) /
    // yaw_rad_per_count, with the model's signed scale; |zero - 2048| > 10
    // counts is rejected (the previous estimate stays).
    void learn(uint64_t t,double course,double kmh) {
        const bool valid=is_finite(course) && course>=0 && course<360;
        Fix& f=fixes_[(fix_head_+fix_n_)%FIXES];
        if(fix_n_<FIXES)++fix_n_;else fix_head_=(fix_head_+1)%FIXES;
        f.t=t;f.course=course;f.kmh=kmh;f.valid=valid;
        if(!valid || !(kmh>=learn_kmh()))return;
        const Fix* f0=0;double maxdev=0;uint64_t prev=t;unsigned n=1;
        for(unsigned j=1;j<fix_n_;++j) {
            const Fix& g=fixes_[(fix_head_+fix_n_-1-j)%FIXES];
            if(g.t>=prev || prev-g.t>LEARN_GAP_NS || !(g.kmh>=learn_kmh()) || t-g.t>LEARN_W_NS+LEARN_SLACK_NS)break;
            prev=g.t;++n;
            if(!g.valid)continue;
            const double dev=fabs(wrap180(g.course-course));
            if(dev>maxdev)maxdev=dev;
            if(t-g.t>=LEARN_W_NS) { f0=&g;break; }
        }
        if(!f0 || maxdev>learn_dev_deg())return;
        const double span=double(t-f0->t)/1e9;
        // Bins wholly inside [f0, t].
        const uint64_t first=(f0->t+BIN_NS-1)/BIN_NS,last=t/BIN_NS;
        int64_t ysum=0;uint64_t yn=0,cover=0;
        for(uint64_t i=first;i<last;++i) {
            const Bin& b=bins_[i%BINS];
            if(b.idx!=i)continue;
            ysum+=b.ysum;yn+=b.yn;cover+=b.cover;
        }
        if(!yn || double(cover)<0.9*span*1e9)return;
        const double mean=2048.0+double(ysum)/double(yn);
        const double dc=wrap180(course-f0->course);
        const double zero=mean-(dc*3.14159265358979323846/180.0/span)/rad_per_count_;
        if(fabs(zero-2048.0)>learn_limit_counts())return;
        learn_.have=true;learn_.zero=zero;learn_.mean=mean;learn_.span_s=span;learn_.dc_deg=dc;
        learn_.dev_deg=maxdev;learn_.cover=double(cover)/(span*1e9);learn_.at=t;learn_.fixes=n;
        ++learn_ok_;
    }
    bool emit(unsigned r,const char* row,Sink sink,void* context) {
        const unsigned g=sink(context,row);
        if(g==GATE_OPEN)return true;
        ++drops_[r];++why_[g<unsigned(GATES)?g:unsigned(GATE_JOURNAL)];return false;
    }
    static void centi(char out[24],double counts) {
        if(!is_finite(counts)) { strcpy(out,"null");return; }
        snprintf(out,24,"%ld",lround((counts-2048.0)*100.0));
    }
    static void num(char out[24],double v,bool ok,const char* fmt="%.1f") {
        if(!ok || !is_finite(v) || v>=1e12 || v<=-1e12) { strcpy(out,"null");return; }
        snprintf(out,24,fmt,v);
    }
    void write_learn(uint64_t now,uint64_t t,const char* why,Sink sink,void* context) {
        char z[24],mean[24],used[24],span[24],dc[24],dev[24],cover[24],age[24];
        const Learn& l=learn_;
        if(l.have) {
            centi(z,l.zero);centi(mean,l.mean);
            snprintf(span,sizeof span,"%llu",(unsigned long long)lround(l.span_s*1000.0));
            num(dc,l.dc_deg,true);num(dev,l.dev_deg,true);
            snprintf(cover,sizeof cover,"%ld",lround(l.cover*1000.0));
            snprintf(age,sizeof age,"%llu",(unsigned long long)(t>l.at?(t-l.at)/MS:0));
        } else { strcpy(z,"null");strcpy(mean,"null");strcpy(span,"null");strcpy(dc,"null");
                 strcpy(dev,"null");strcpy(cover,"null");strcpy(age,"null"); }
        centi(used,zero_in_use_);
        char row[ROW_CAPACITY];
        const int n=snprintf(row,sizeof row,
            "{\"kind\":\"yaw_learn\",\"schema\":1,\"mono_ns\":%llu,\"at_ms\":%llu,\"ev\":\"%s\",\"z\":%s,"
            "\"used\":%s,\"mean\":%s,\"span_ms\":%s,\"dc\":%s,\"dev\":%s,\"cover\":%s,\"fixes\":%u,"
            "\"age_ms\":%s,\"n\":%llu}",
            (unsigned long long)now,(unsigned long long)(t/MS),why,z,used,mean,span,dc,dev,cover,
            l.have?l.fixes:0U,age,(unsigned long long)learn_ok_);
        learn_ok_=0;
        if(!strcmp(why,"periodic") || l.have)last_learn_row_=t?t:1;
        if(n>0 && size_t(n)<sizeof row)emit(LEARN,row,sink,context);
    }
    void finish(uint64_t now,Sink sink,void* context) {
        const Pending p=pending_;pending_=Pending();
        if(p.exit) {
            ++episodes_;
            if(allow(L_EXIT,now))write_exit(now,p,sink,context);else ++rate_[L_EXIT];
        }
        if(p.repoll)return;
        const double wheel=p.acc.wn?p.acc.wsum/double(p.acc.wn):-1;
        if(!(p.kmh>moving_kmh()) && !(wheel>moving_kmh()))return;
        if(!allow(L_FIX1,now)) { ++rate_[L_FIX1];return; }
        write_fix1(p,sink,context);
    }
    bool allow(unsigned l,uint64_t now) {
        if(!refilled_[l] || now<refilled_[l]) refilled_[l]=now?now:1;
        else {
            const uint64_t add=(now-refilled_[l])*1000ULL/refill_ns(l);
            if(add) {
                refilled_[l]+=add*refill_ns(l)/1000ULL;
                tokens_[l]=tokens_[l]+add>burst(l)*1000ULL?burst(l)*1000ULL:tokens_[l]+add;
            }
        }
        if(tokens_[l]<1000ULL)return false;
        tokens_[l]-=1000ULL;return true;
    }
    static long long scaled(double v,double scale) {
        const double x=v*scale;
        return x==x && x<9.0e18 && x>-9.0e18?llround(x):-1LL;
    }
    // fix1 "d": [t_ms, mode, lat_e7, lon_e7, speed_mm_s, bearing_e6,
    // accuracy_mm (-1 unpaired), satellites (-1 unknown), acc_ms (window of
    // the sums below, ms), yaw_sum (sum of counts - 2048 per sample), yaw_n
    // (samples), wheel_kmh_e1 (mean, -1 none), wheel_n (events)].
    void write_fix1(const Pending& p,Sink sink,void* context) {
        const int sat=p.sat>=0 && p.sat<=255?int(p.sat):-1;
        const double wheel=p.acc.wn?p.acc.wsum/double(p.acc.wn):-1;
        double brg=p.heading;
        if(is_finite(brg)) { brg=fmod(brg,360.0);if(brg<0)brg+=360.0; }
        long long brg_e6=scaled(brg,1e6);
        if(brg_e6>=360000000LL)brg_e6-=360000000LL;   // 359.9999996 rounds to 360: 0 (as the adapter)
        char row[ROW_CAPACITY];
        const int n=snprintf(row,sizeof row,
            "{\"kind\":\"fix1\",\"d\":[%llu,%d,%lld,%lld,%lld,%lld,%lld,%d,%llu,%lld,%llu,%lld,%llu]}",
            (unsigned long long)(p.t/MS),int(p.mode),scaled(p.lat,1e7),scaled(p.lon,1e7),
            is_finite(p.kmh) && p.kmh>=0?scaled(p.kmh/3.6,1000.0):-1LL,brg_e6,
            p.paired && p.acc_m>=0?scaled(p.acc_m,1000.0):-1LL,sat,
            (unsigned long long)(p.acc.since && p.t>p.acc.since?(p.t-p.acc.since)/MS:0),
            (long long)p.acc.ysum,(unsigned long long)p.acc.yn,wheel>=0?scaled(wheel,10.0):-1LL,
            (unsigned long long)p.acc.wn);
        if(n>0 && size_t(n)<sizeof row)emit(FIX1,row,sink,context);
    }
    // dt_ms: POSITION hook time of the fix minus the hook time of the last
    // LOCATION sent (fix_send_ms: the hook time of the fix's own LOCATION,
    // when paired). Naver's getTime() semantics are not confirmed.
    void write_exit(uint64_t now,const Pending& p,Sink sink,void* context) {
        const Episode& q=p.ep;const Sent& a=p.anchor;
        const double dt_ms=p.t>a.t?double((p.t-a.t)/MS):0;
        const double fix_mps=is_finite(p.kmh) && p.kmh>0?p.kmh/3.6:0;
        const Jump j=jump(a.lat,a.lon,p.lat,p.lon,a.has_brg,a.brg,p.heading,a.acc_m,a.mps>0?a.mps:0,fix_mps,dt_ms);
        char along[24],cross[24],dbrg[24],sb[24],fixb[24],fixk[24],sk[24],fixa[24],sa[24],hon[24],imp[24],fs[24];
        num(along,j.along_m,j.has_brg);num(cross,j.cross_m,j.has_brg);num(dbrg,j.dbrg_deg,j.has_brg);
        num(sb,a.brg,a.has_brg);num(fixb,p.heading,is_finite(p.heading));num(fixk,p.kmh,is_finite(p.kmh));
        num(sk,a.mps*3.6,a.mps>=0);num(fixa,p.acc_m,p.paired && p.acc_m>=0,"%.2f");
        num(sa,a.acc_m,a.acc_m>=0,"%.2f");num(hon,q.honest,q.reps && q.honest>=0,"%.2f");
        num(imp,j.implied_kmh,j.implied_kmh>=0);
        if(p.paired)snprintf(fs,sizeof fs,"%llu",(unsigned long long)(p.send_t/MS));else strcpy(fs,"null");
        char row[ROW_CAPACITY];
        const int n=snprintf(row,sizeof row,
            "{\"kind\":\"exit_jump\",\"schema\":1,\"mono_ns\":%llu,\"ep\":%u,\"fix_ms\":%llu,\"fix_send_ms\":%s,"
            "\"mode\":%d,\"lost_ms\":%llu,\"reps\":%u,\"rep_ms\":%llu,\"stock\":%u,\"stock_after\":%u,"
            "\"sent_choice\":%d,\"sent_ms\":%llu,\"dt_ms\":%.0f,\"jump_m\":%.1f,\"along_m\":%s,"
            "\"cross_m\":%s,\"sent_brg\":%s,\"fix_brg\":%s,\"dbrg\":%s,\"sent_kmh\":%s,\"fix_kmh\":%s,"
            "\"sent_acc\":%s,\"fix_acc\":%s,\"honest\":%s,\"r1\":%s,\"r1_acc\":%s,\"r1_dt\":%s,\"r1_dist\":%s,"
            "\"r1_speed\":%s,\"r2\":%s,\"r2_kmh\":%s}",
            (unsigned long long)now,episodes_,(unsigned long long)(p.t/MS),fs,int(p.mode),
            (unsigned long long)(p.t>q.lost_ns?(p.t-q.lost_ns)/MS:0),q.reps,
            (unsigned long long)(q.last_rep>q.first_rep?(q.last_rep-q.first_rep)/MS:0),q.stock,q.stock_after,
            a.choice,(unsigned long long)(a.t/MS),dt_ms,j.jump_m,
            along,cross,sb,fixb,dbrg,sk,fixk,sa,fixa,hon,b(j.r1),b(j.r1_acc),b(j.r1_dt),b(j.r1_dist),
            b(j.r1_speed),b(j.r2),imp);
        if(n>0 && size_t(n)<sizeof row)emit(EXIT,row,sink,context);
    }
    static const char* b(bool v) { return v?"true":"false"; }
    bool due_drops() const {
        for(unsigned r=0;r<ROWS-1;++r)if(drops_[r])return true;
        for(unsigned l=0;l<LIMITS;++l)if(rate_[l])return true;
        return false;
    }
    void write_drops(uint64_t now,Sink sink,void* context) {
        char row[ROW_CAPACITY];
        const int n=snprintf(row,sizeof row,
            "{\"kind\":\"rec_drop\",\"schema\":1,\"mono_ns\":%llu,\"fix1\":%llu,\"exit_jump\":%llu,"
            "\"yaw_learn\":%llu,\"rec_drop\":%llu,\"fix1_rate\":%llu,\"exit_rate\":%llu,\"lost_rate\":%llu,"
            "\"why\":{\"journal\":%llu,\"no_writer\":%llu,\"lag\":%llu,\"queue\":%llu}}",
            (unsigned long long)now,(unsigned long long)drops_[FIX1],(unsigned long long)drops_[EXIT],
            (unsigned long long)drops_[LEARN],(unsigned long long)drops_[DROP],(unsigned long long)rate_[L_FIX1],
            (unsigned long long)rate_[L_EXIT],(unsigned long long)rate_[L_LOST],
            (unsigned long long)why_[GATE_JOURNAL],(unsigned long long)why_[GATE_NO_WRITER],
            (unsigned long long)why_[GATE_LAG],(unsigned long long)why_[GATE_QUEUE]);
        // A written row resets the counts and waits DROP_PERIOD_NS; a refused
        // one counts itself and is retried after DROP_RETRY_NS.
        const unsigned g=n>0 && size_t(n)<sizeof row?sink(context,row):unsigned(GATE_JOURNAL);
        if(g==GATE_OPEN) {
            for(unsigned r=0;r<ROWS;++r)drops_[r]=0;
            for(unsigned l=0;l<LIMITS;++l)rate_[l]=0;
            for(unsigned w=0;w<GATES;++w)why_[w]=0;
            next_drop_=now+DROP_PERIOD_NS;
        } else { ++drops_[DROP];++why_[g<unsigned(GATES)?g:unsigned(GATE_JOURNAL)];next_drop_=now+DROP_RETRY_NS; }
    }

    Bin bins_[BINS]; uint64_t newest_bin_;
    bool have_yaw_; uint64_t last_yaw_t_;
    Acc acc_;
    Fix fixes_[FIXES]; unsigned fix_n_,fix_head_;
    Learn learn_; uint64_t learn_ok_,last_learn_row_;
    Pending pending_; Episode ep_; unsigned episodes_; Sent sent_;
    bool have_cls_; unsigned last_cls_;
    bool have_key_; Key key_;
    bool diag_seen_;
    double zero_in_use_,rad_per_count_;
    uint64_t tokens_[LIMITS],refilled_[LIMITS],rate_[LIMITS];
    uint64_t drops_[ROWS],why_[GATES],next_drop_;
};

} }
#endif
