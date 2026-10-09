#ifndef MX5_RUNTIME_YAW_STUDY_LOG_H
#define MX5_RUNTIME_YAW_STUDY_LOG_H
// Yaw-zero data collection rows of the persistent journal profile
// (validation/YAW_DATA_COLLECTION_2026-10-09.md). LOGGING ONLY: nothing here
// feeds the Pipeline, the BETA controller, the adapter or any decision; it
// only reads the accepted motion events and POSITION observations the
// PersistentLog already sees and writes compact diagnostic rows:
//  * yaw_stop: one row per standstill episode (all four wheels exactly zero
//    for >= 1 s; episodes separated by < 5 s are merged), written when it
//    ends: duration, stationary yaw sum/count/sd (without the windows of
//    its first 0.2 s and after its last zero wheel event), the mean of its
//    first and last second, the mean and wheel speed of the 2 s before it
//    (null with pre_bad after a gap, reinit or earlier zero period);
//  * yaw_edge: at every GPS loss (FIX -> another decoded class) and return
//    (-> FIX): yaw mean, wheel speed and GPS course of the 5 s before and
//    the 5 s after;
//  * yaw_reinit: a yaw receipt gap > 1 s or an invalid yaw event (count 0,
//    unrepresentable sum, mean >= 4094, i.e. the 4095 marker): the
//    stationary mean of the last standstill before and of the first one
//    after it;
//  * extra log_digest fields: ten 1 s sums of moving yaw samples (sum minus
//    2048*count) with their counts, the stationary sum/count, the integer
//    GPS course of every new fix, and the mean left-right wheel difference
//    of slot pairs 0-1 and 2-3.
// The VIM tap provides only wheel speeds (0x100), yaw window sums/counts
// (0x116) and the reverse lamp (0x118): no acceleration, steering,
// temperature, rpm or A/C signal exists to log.
// Times: wheel/yaw events by their producer receipt time (received_ns),
// POSITION rows by their hook time, rows by the worker clock; all are
// CLOCK_MONOTONIC (seconds since kernel boot).
// Worker thread only; no allocation, no I/O, bounded work per call (at most
// BINS bins or FIXES fixes per query). Rows are rate-limited per kind (token
// bucket) and the rest counted; the PersistentLog queues them as diagnostic
// rows (never evidence), so the journal ring drops them first under a
// backlog with its usual counters.
#include "adapter/adapter.h"
#include "navigation/pipeline.h"
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace mx5 { namespace runtime {

class YawStudyLog {
public:
    enum Row { STOP=0, EDGE=1, REINIT=2, ROWS=3 };
    typedef void (*Sink)(void* context,const char* row);
    static const uint64_t MS=1000000ULL, SEC=1000000000ULL;
    static const uint64_t BIN_NS=100*MS;          // history bins
    static const unsigned BINS=64;                // 6.4 s of history
    static const uint64_t STOP_MIN_NS=SEC;        // standstill episode
    static const uint64_t MERGE_NS=5*SEC;         // merge episodes closer than this
    static const uint64_t WHEEL_GAP_NS=500*MS;    // a longer wheel gap ends a standstill
    static const uint64_t YAW_GAP_NS=250*MS;      // counted as a gap in an episode
    static const uint64_t REINIT_GAP_NS=SEC;      // a longer yaw gap is a reinit trigger
    static const uint64_t EDGE_NS=5*SEC;          // edge windows
    static const uint64_t PRE_NS=2*SEC;           // before a stop
    // A yaw window received within this time after the first zero wheel
    // event still holds pre-stop samples (the 2026-10-09 study skipped
    // 0.2 s): not counted in a standstill's statistics.
    static const uint64_t SETTLE_NS=200*MS;
    static const unsigned RECENT=64;              // run events kept for the last-second mean
    static const uint64_t SLACK_NS=300*MS;        // wait for late events before a window closes
    static const uint64_t REINIT_TIMEOUT_NS=1800*SEC;
    static const unsigned DIGEST_BINS=10;         // 1 s bins per 10 s digest
    static const unsigned DIGEST_FIXES=12;        // 10 s at 1 Hz is 10 or 11 fixes
    static const unsigned FIXES=16;
    static const unsigned READY=4;
    static const size_t ROW_CAPACITY=400;
    static const size_t DIGEST_CAPACITY=640;
    // Rate limits (token bucket, per kind): refill period and burst.
    static uint64_t refill_ns(unsigned r) { return r==STOP?10*SEC:30*SEC; }
    static unsigned burst(unsigned r) { return r==STOP?3:4; }

    YawStudyLog() { reset(); }
    void reset() {
        memset(bins_,0,sizeof bins_);for(unsigned i=0;i<BINS;++i)bins_[i].idx=UINT64_MAX;
        newest_bin_=0;
        have_wheel_=stationary_=false;last_wheel_t_=0;
        have_yaw_=false;last_yaw_t_=0;
        run_=Episode();pending_=Episode();ready_n_=0;
        tail_=Acc();tail_first_=Acc();memset(recent_,0,sizeof recent_);recent_head_=0;
        last_wheel_gap_t_=last_yaw_gap_t_=last_trigger_t_=last_zero_t_=0;have_zero_=false;
        have_last_=false;last_mean_=0;last_end_=0;
        reinit_=Reinit();reinit_ready_=Reinit();edge_=Edge();edge_ready_=Edge();
        have_cls_=false;last_fix_=false;last_cls_=0;
        have_key_=false;memset(&key_,0,sizeof key_);
        fix_n_=0;fix_head_=0;
        for(unsigned r=0;r<ROWS;++r) { tokens_[r]=burst(r)*1000ULL;refilled_[r]=0;suppressed_[r]=0; }
        period(0);
    }
    uint64_t suppressed(unsigned r) const { return suppressed_[r]; }
    void clear_suppressed() { for(unsigned r=0;r<ROWS;++r)suppressed_[r]=0; }
    static const char* name(unsigned r) { return r==STOP?"yaw_stop":r==EDGE?"yaw_edge":"yaw_reinit"; }

    // ---- inputs ----
    void motion(const navigation::RawEvent& e,const navigation::ModelProfile& model) {
        if(e.kind==navigation::WHEELS)wheels(e,model);
        else if(e.kind==navigation::YAW)yaw(e);
    }
    void position(const adapter::Observation& o) {
        if(o.kind!=adapter::Observation::POSITION || o.position_class==adapter::POSITION_UNDECODED)return;
        const uint64_t t=o.mono_ns;
        const bool fix=o.position_class==adapter::POSITION_FIX;
        if(fix) {
            Key k;k.utc=o.position.utc_seconds;k.lat=o.position.latitude_deg;k.lon=o.position.longitude_deg;
            k.heading=o.position.heading_deg;
            if(!have_key_ || memcmp(&k,&key_,sizeof k)) {
                key_=k;have_key_=true;
                new_fix(t,o.position.heading_deg,o.position.velocity_kmh,o.position.horizontal);
            }
        }
        const unsigned cls=unsigned(o.position_class);
        if(have_cls_ && fix!=last_fix_)edge(t,fix,last_cls_,cls);
        have_cls_=true;last_fix_=fix;last_cls_=cls;
    }

    // ---- digest period (PersistentLog's 10 s digest) ----
    void period(uint64_t since) {
        since_=since;
        for(unsigned i=0;i<DIGEST_BINS;++i) { y1_[i]=0;y1n_[i]=0; }
        yst_=0;ystn_=0;gc_n_=0;gc_more_=0;gq_=0;gv_=0;
        d01_=d23_=0;dn_=0;
    }
    // Appends the digest fields (",\"y1\":[...]..."), at most DIGEST_CAPACITY bytes.
    size_t digest_fields(char* out,size_t capacity) const {
        size_t n=0;
        n=put(out,capacity,n,",\"y1\":[");
        for(unsigned i=0;i<DIGEST_BINS;++i)n=putf(out,capacity,n,"%s%lld",i?",":"",(long long)y1_[i]);
        n=put(out,capacity,n,"],\"y1n\":[");
        for(unsigned i=0;i<DIGEST_BINS;++i)n=putf(out,capacity,n,"%s%llu",i?",":"",(unsigned long long)y1n_[i]);
        n=putf(out,capacity,n,"],\"yst\":%lld,\"ystn\":%llu,\"gc\":[",(long long)yst_,(unsigned long long)ystn_);
        for(unsigned i=0;i<gc_n_;++i)n=putf(out,capacity,n,"%s%u",i?",":"",gc_[i]);
        n=put(out,capacity,n,"]");
        if(gc_n_)n=putf(out,capacity,n,",\"gq\":%u,\"gv\":%u",gq_,gv_);
        if(gc_more_)n=putf(out,capacity,n,",\"gc_more\":%u",gc_more_);
        if(dn_)n=putf(out,capacity,n,",\"dw01\":%.2f,\"dw23\":%.2f",d01_/double(dn_),d23_/double(dn_));
        return n<capacity?n:0;
    }

    // ---- rows: due rows are written through sink (worker clock now) ----
    void poll(uint64_t now,Sink sink,void* context) {
        refill(now);
        if(pending_.have && !(run_.active && run_.start<pending_.end+MERGE_NS) &&
           now>=pending_.end+MERGE_NS+SLACK_NS)finalize_pending();
        for(unsigned i=0;i<ready_n_;++i)write_stop(ready_[i],now,sink,context);
        ready_n_=0;
        if(reinit_.have && now>reinit_.at && now-reinit_.at>=REINIT_TIMEOUT_NS)reinit_done();
        if(reinit_ready_.have)write_reinit(now,sink,context);
        if(edge_ready_.have) { write_edge(edge_ready_,now,sink,context);edge_ready_.have=false; }
        if(edge_.have && now>=edge_.at+EDGE_NS+SLACK_NS) {
            close_edge(edge_.at+EDGE_NS,false);write_edge(edge_,now,sink,context);edge_.have=false;
        }
    }
    // Capture stop: everything open is written now (an ongoing standstill
    // as "open":1, an edge with a truncated after window, a reinit without
    // its after value).
    void flush(uint64_t now,Sink sink,void* context) {
        refill(now);
        if(run_.active) { run_.open=true;end_run();run_.active=false; }
        if(pending_.have)finalize_pending();
        for(unsigned i=0;i<ready_n_;++i)write_stop(ready_[i],now,sink,context);
        ready_n_=0;
        if(reinit_.have)reinit_done();
        if(reinit_ready_.have)write_reinit(now,sink,context);
        if(edge_ready_.have) { write_edge(edge_ready_,now,sink,context);edge_ready_.have=false; }
        if(edge_.have) {
            const uint64_t end=now<edge_.at+EDGE_NS?now:edge_.at+EDGE_NS;
            close_edge(end,end<edge_.at+EDGE_NS);write_edge(edge_,now,sink,context);edge_.have=false;
        }
    }

private:
    struct Bin { uint64_t idx; int64_t ysum; uint64_t yn; double kmh; uint32_t wn; };
    struct Sum { int64_t s; uint64_t n; double kmh; uint32_t wn; };
    struct Acc {
        int64_t s; uint64_t n; uint64_t ev; double m1,m2;
        void add(int64_t dev,unsigned count) {
            s+=dev;n+=count;++ev;const double m=double(dev)/count;m1+=m;m2+=m*m;
        }
        void merge(const Acc& o) { s+=o.s;n+=o.n;ev+=o.ev;m1+=o.m1;m2+=o.m2; }
    };
    struct Episode {
        bool have,active,open;
        uint64_t start,last,end,last_yaw;
        Acc acc,first; Sum last1,pre;
        unsigned bad,merged,pre_bad;
    };
    struct Recent { uint64_t t; int64_t dev; unsigned count; };
    struct Reinit {
        bool have,after_set,stationary;
        uint64_t at,gap_ms,after_delay_ns,before_age_ns;
        unsigned n,causes;   // causes: 1 gap, 2 invalid
        bool before_set; double before,after;
    };
    struct Edge {
        bool have,loss,cut,lost;
        uint64_t at;
        unsigned from,to;
        Sum b,a; int cb[4],ca[4];   // first course, last course, span 0.1 s, fixes
    };
    struct Key { uint64_t utc; double lat,lon,heading; };
    struct Fix { uint64_t t; int course; };

    static size_t put(char* out,size_t cap,size_t n,const char* s) {
        const size_t len=strlen(s);
        if(n<cap && len<cap-n) { memcpy(out+n,s,len+1);return n+len; }
        return cap;
    }
    static size_t putf(char* out,size_t cap,size_t n,const char* fmt,...)
#if defined(__GNUC__)
        __attribute__((format(printf,4,5)))
#endif
        ;
    static int course_int(double heading) {
        if(!(heading==heading))return -1;
        long c=lround(fmod(heading,360.0));if(c<0)c+=360;if(c>=360)c-=360;
        return int(c);
    }
    // Yaw means in rows: integer 0.01 counts relative to 2048 (-125 is
    // 2046.75), null without samples.
    static void centi(char out[24],double deviation) {
        snprintf(out,24,"%ld",lround(deviation*100.0));
    }
    static void mean(char out[24],const Sum& s) {
        if(!s.n) { strcpy(out,"null");return; }
        centi(out,double(s.s)/double(s.n));
    }
    static void kmh(char out[24],const Sum& s) {
        if(!s.wn) { strcpy(out,"null");return; }
        snprintf(out,24,"%.1f",s.kmh/s.wn);
    }
    static Sum sum_of(const Acc& a) { Sum s={a.s,a.n,0,0};return s; }

    // History bins by receipt time.
    Bin* bin(uint64_t t) {
        const uint64_t idx=t/BIN_NS;
        if(newest_bin_>=BINS && idx<=newest_bin_-BINS)return 0;   // older than the history
        if(idx>newest_bin_)newest_bin_=idx;
        Bin& b=bins_[idx%BINS];
        if(b.idx!=idx) { b.idx=idx;b.ysum=0;b.yn=0;b.kmh=0;b.wn=0; }
        return &b;
    }
    Sum window(uint64_t a,uint64_t b) const {
        Sum s={0,0,0,0};
        if(b<=a)return s;
        uint64_t first=a/BIN_NS,last=(b-1)/BIN_NS;
        if(last-first>=BINS)first=last-BINS+1;
        for(uint64_t i=first;i<=last;++i) {
            const Bin& x=bins_[i%BINS];
            if(x.idx!=i)continue;
            s.s+=x.ysum;s.n+=x.yn;s.kmh+=x.kmh;s.wn+=x.wn;
        }
        return s;
    }
    void wheels(const navigation::RawEvent& e,const navigation::ModelProfile& model) {
        const uint64_t t=e.received_ns;
        double v=0;bool zero=true;
        for(unsigned i=0;i<4;++i) {
            const double w=e.raw[i]*model.wheel_kmh_per_count+model.wheel_zero_kmh;
            if(fabs(w)>=1e-9)zero=false;
            v+=w*0.25;
        }
        if(Bin* b=bin(t)) { b->kmh+=v;++b->wn; }
        if(have_wheel_ && t<last_wheel_t_)return;   // out of order: history only
        if(!zero) {
            d01_+=(double(e.raw[0])-double(e.raw[1]))*model.wheel_kmh_per_count;
            d23_+=(double(e.raw[2])-double(e.raw[3]))*model.wheel_kmh_per_count;++dn_;
        }
        const bool gap=have_wheel_ && t-last_wheel_t_>WHEEL_GAP_NS;
        if(gap)last_wheel_gap_t_=t;
        if(run_.active && (!zero || gap)) { end_run();run_.active=false; }
        if(zero && !run_.active)start_run(t);
        if(zero) {
            // Yaw windows received since the previous zero wheel event are
            // stationary now; without this confirmation they are dropped.
            run_.last=t;run_.acc.merge(tail_);run_.first.merge(tail_first_);
            tail_=Acc();tail_first_=Acc();
        }
        stationary_=zero;have_wheel_=true;last_wheel_t_=t;
    }
    void yaw(const navigation::RawEvent& e) {
        const uint64_t t=e.received_ns;
        const bool valid=e.count && e.count<=255 && uint32_t(e.raw[0])+65536U>uint32_t(e.count)*4095U &&
                         unsigned(e.raw[0])/e.count<4094;
        bool gap=false;uint64_t gap_ns=0;
        if(have_yaw_ && t>last_yaw_t_) {
            gap_ns=t-last_yaw_t_;gap=gap_ns>REINIT_GAP_NS;
            if(gap_ns>YAW_GAP_NS) { last_yaw_gap_t_=t;if(run_.active)++run_.bad; }
        }
        if(!have_yaw_ || t>last_yaw_t_)last_yaw_t_=t;
        have_yaw_=true;
        if(!valid) {
            if(run_.active)++run_.bad;
            trigger(t,2,gap?gap_ns:0);
            return;
        }
        if(gap)trigger(t,1,gap_ns);
        const int64_t dev=int64_t(e.raw[0])-2048*int64_t(e.count);
        if(Bin* b=bin(t)) { b->ysum+=dev;b->yn+=e.count; }
        const bool still=stationary_ && have_wheel_ && (t<=last_wheel_t_ || t-last_wheel_t_<=WHEEL_GAP_NS);
        if(still) { yst_+=dev;ystn_+=e.count; }
        else {
            const uint64_t j=t>since_?(t-since_)/SEC:0;
            const unsigned k=j<DIGEST_BINS?unsigned(j):DIGEST_BINS-1;
            y1_[k]+=dev;y1n_[k]+=e.count;
        }
        if(run_.active && still && t>=run_.start+SETTLE_NS) {
            const bool first=t<run_.start+SETTLE_NS+SEC;
            if(t<=run_.last) { run_.acc.add(dev,e.count);if(first)run_.first.add(dev,e.count); }
            else { tail_.add(dev,e.count);if(first)tail_first_.add(dev,e.count); }
            Recent& r=recent_[recent_head_++%RECENT];r.t=t;r.dev=dev;r.count=e.count;
        }
    }
    void start_run(uint64_t t) {
        run_=Episode();run_.active=true;run_.start=run_.last=t;run_.merged=1;
        tail_=Acc();tail_first_=Acc();recent_head_=0;
        // The 2 s before the stop, without the bin of its first zero event.
        // Invalid (pre_bad bits) after a wheel or yaw gap (1), a reinit
        // trigger (2) or another standstill (4) inside it, or without
        // samples (8).
        const uint64_t from=t>PRE_NS?t-PRE_NS:0;
        const Sum p=window(from,t/BIN_NS*BIN_NS);run_.pre=p;
        unsigned bad=0;
        if((last_wheel_gap_t_ && last_wheel_gap_t_>=from) || (last_yaw_gap_t_ && last_yaw_gap_t_>=from))bad|=1;
        if(last_trigger_t_ && last_trigger_t_>=from)bad|=2;
        if(have_zero_ && last_zero_t_>=from)bad|=4;
        if(!p.n || !p.wn)bad|=8;
        run_.pre_bad=bad;
    }
    // The current zero run ended at run_.last: a standstill episode when
    // long enough; merged into the pending one when close enough.
    // Yaw windows after the last zero wheel event (tail_) are discarded.
    // The merge-window check below is also made by poll(); it matters when
    // no poll runs between two runs (callers feeding events in a burst).
    void end_run() {
        have_zero_=true;last_zero_t_=run_.last;
        tail_=Acc();tail_first_=Acc();
        if(run_.last-run_.start<STOP_MIN_NS)return;
        run_.end=run_.last;
        // Last second: the newest RECENT stationary windows of this run.
        Sum l={0,0,0,0};
        const uint64_t from=run_.end>SEC?run_.end-SEC:0;
        const unsigned n=recent_head_<RECENT?recent_head_:RECENT;
        for(unsigned i=0;i<n;++i) {
            const Recent& r=recent_[(recent_head_-1-i)%RECENT];
            if(r.t>run_.end)continue;
            if(r.t<from)break;
            l.s+=r.dev;l.n+=r.count;
        }
        run_.last1=l;
        if(pending_.have && !run_.open && run_.start<pending_.end+MERGE_NS) {
            pending_.end=run_.end;pending_.acc.merge(run_.acc);pending_.last1=run_.last1;
            pending_.bad+=run_.bad;++pending_.merged;
            return;
        }
        if(pending_.have)finalize_pending();
        pending_=run_;pending_.have=true;pending_.active=false;
        if(run_.open)finalize_pending();
    }
    void finalize_pending() {
        if(!pending_.have)return;
        if(pending_.acc.n) {
            have_last_=true;last_mean_=2048.0+double(pending_.acc.s)/double(pending_.acc.n);last_end_=pending_.end;
            if(reinit_.have && pending_.start>=reinit_.at) {
                reinit_.after_set=true;reinit_.after=last_mean_;reinit_.after_delay_ns=pending_.start-reinit_.at;
                reinit_done();
            }
        }
        if(ready_n_<READY)ready_[ready_n_++]=pending_;else ++suppressed_[STOP];
        pending_=Episode();
    }
    // A yaw reinit candidate: an ongoing standstill ends here and is never
    // merged across it; repeated triggers before the after value coalesce.
    void trigger(uint64_t t,unsigned cause,uint64_t gap_ns) {
        last_trigger_t_=t;
        if(run_.active) { end_run();run_.active=false; }
        if(pending_.have)finalize_pending();
        if(reinit_.have) {   // still waiting for its after value: coalesce
            ++reinit_.n;reinit_.causes|=cause;
            if(gap_ns/MS>reinit_.gap_ms)reinit_.gap_ms=gap_ns/MS;
            return;
        }
        reinit_=Reinit();reinit_.have=true;reinit_.at=t;reinit_.n=1;reinit_.causes=cause;
        reinit_.gap_ms=gap_ns/MS;reinit_.stationary=stationary_;
        if(have_last_) { reinit_.before_set=true;reinit_.before=last_mean_;reinit_.before_age_ns=t>last_end_?t-last_end_:0; }
    }
    // The waiting reinit is complete (after value, timeout or capture stop).
    void reinit_done() {
        if(reinit_ready_.have)++suppressed_[REINIT];   // never written: no poll in between
        reinit_ready_=reinit_;reinit_=Reinit();
    }
    void new_fix(uint64_t t,double heading,double kmh_value,double hacc) {
        const int c=course_int(heading);
        if(c<0)return;
        Fix& f=fixes_[(fix_head_+fix_n_)%FIXES];
        if(fix_n_<FIXES)++fix_n_;else fix_head_=(fix_head_+1)%FIXES;
        f.t=t;f.course=c;
        if(gc_n_<DIGEST_FIXES) {
            const uint64_t ds=t>since_?(t-since_)/(100*MS):0;
            gc_[gc_n_]=unsigned(ds<99?ds:99)*1000U+unsigned(c);
            const double q=hacc==hacc && hacc>0?ceil(hacc):0,v=kmh_value==kmh_value && kmh_value>0?floor(kmh_value):0;
            const unsigned qi=q<999?unsigned(q):999,vi=v<999?unsigned(v):999;
            if(!gc_n_ || qi>gq_)gq_=qi;
            if(!gc_n_ || vi<gv_)gv_=vi;
            ++gc_n_;
        } else ++gc_more_;
    }
    void courses(int out[4],uint64_t a,uint64_t b) const {
        out[0]=out[1]=-1;out[2]=0;out[3]=0;
        uint64_t first=0,last=0;
        for(unsigned i=0;i<fix_n_;++i) {
            const Fix& f=fixes_[(fix_head_+i)%FIXES];
            if(f.t<a || f.t>=b)continue;
            if(!out[3]) { out[0]=f.course;first=f.t; }
            out[1]=f.course;last=f.t;++out[3];
        }
        out[2]=out[3]?int((last-first)/(100*MS)):0;
    }
    void edge(uint64_t t,bool to_fix,unsigned from,unsigned to) {
        if(edge_.have) {
            close_edge(t<edge_.at+EDGE_NS?t:edge_.at+EDGE_NS,t<edge_.at+EDGE_NS);
            if(edge_ready_.have)++suppressed_[EDGE];   // never written: no poll in between
            edge_ready_=edge_;edge_.have=false;
        }
        edge_=Edge();edge_.have=true;edge_.at=t;edge_.loss=!to_fix;edge_.from=from;edge_.to=to;
        edge_.b=window(t>EDGE_NS?t-EDGE_NS:0,t);courses(edge_.cb,t>EDGE_NS?t-EDGE_NS:0,t);
    }
    void close_edge(uint64_t end,bool cut) {
        edge_.a=window(edge_.at,end);courses(edge_.ca,edge_.at,end);edge_.cut=cut;
        // A late close (history BINS*BIN_NS) lost the front of the after window.
        edge_.lost=newest_bin_>=BINS && edge_.at/BIN_NS<=newest_bin_-BINS;
    }
    void refill(uint64_t now) {
        for(unsigned r=0;r<ROWS;++r) {
            if(!refilled_[r] || now<refilled_[r]) { refilled_[r]=now?now:1;continue; }
            const uint64_t add=(now-refilled_[r])*1000ULL/refill_ns(r);
            if(!add)continue;
            refilled_[r]+=add*refill_ns(r)/1000ULL;
            tokens_[r]=tokens_[r]+add>burst(r)*1000ULL?burst(r)*1000ULL:tokens_[r]+add;
        }
    }
    bool allow(unsigned r) {
        if(tokens_[r]<1000ULL) { ++suppressed_[r];return false; }
        tokens_[r]-=1000ULL;return true;
    }
    void write_stop(const Episode& e,uint64_t now,Sink sink,void* context) {
        if(!allow(STOP))return;
        char sd[24],first[24],last[24],pre[24],pre_kmh[24];
        if(e.acc.ev) {
            const double m=e.acc.m1/e.acc.ev,var=e.acc.m2/e.acc.ev-m*m;
            centi(sd,var>0?sqrt(var):0.0);
        } else strcpy(sd,"null");
        mean(first,sum_of(e.first));mean(last,e.last1);
        if(e.pre_bad) { strcpy(pre,"null");strcpy(pre_kmh,"null"); } else { mean(pre,e.pre);kmh(pre_kmh,e.pre); }
        char extra[48];int x=0;extra[0]=0;
        if(e.bad)x+=snprintf(extra+x,sizeof extra-x,",\"bad\":%u",e.bad);
        if(e.merged>1)x+=snprintf(extra+x,sizeof extra-x,",\"merged\":%u",e.merged);
        if(e.pre_bad)snprintf(extra+x,sizeof extra-x,",\"pre_bad\":%u",e.pre_bad);
        char row[ROW_CAPACITY];
        const int n=snprintf(row,sizeof row,
            "{\"kind\":\"yaw_stop\",\"schema\":1,\"mono_ns\":%llu,\"boot_s\":%.1f,\"dur_ms\":%llu,"
            "\"ys\":%lld,\"yn\":%llu,\"sd\":%s,\"first\":%s,\"last\":%s,\"pre\":%s,\"pre_kmh\":%s%s%s}",
            (unsigned long long)now,double(e.start/(100*MS))/10.0,(unsigned long long)((e.end-e.start)/MS),
            (long long)e.acc.s,(unsigned long long)e.acc.n,sd,first,last,pre,pre_kmh,extra,
            e.open?",\"open\":1":"");
        if(n>0 && size_t(n)<sizeof row)sink(context,row);
    }
    void write_reinit(uint64_t now,Sink sink,void* context) {
        const Reinit r=reinit_ready_;reinit_ready_=Reinit();
        if(!allow(REINIT))return;
        char before[24],after[24],age[24],delay[24];
        if(r.before_set) { centi(before,r.before-2048.0);
                           snprintf(age,sizeof age,"%.1f",double(r.before_age_ns/(100*MS))/10.0); }
        else { strcpy(before,"null");strcpy(age,"null"); }
        if(r.after_set) { centi(after,r.after-2048.0);
                          snprintf(delay,sizeof delay,"%.1f",double(r.after_delay_ns/(100*MS))/10.0); }
        else { strcpy(after,"null");strcpy(delay,"null"); }
        char row[ROW_CAPACITY];
        const int n=snprintf(row,sizeof row,
            "{\"kind\":\"yaw_reinit\",\"schema\":1,\"mono_ns\":%llu,\"at_s\":%.1f,\"cause\":%u,\"n\":%u,"
            "\"gap_ms\":%llu,\"still\":%d,\"before\":%s,\"before_age_s\":%s,\"after\":%s,\"after_delay_s\":%s}",
            (unsigned long long)now,double(r.at/(100*MS))/10.0,r.causes,r.n,(unsigned long long)r.gap_ms,
            r.stationary?1:0,before,age,after,delay);
        if(n>0 && size_t(n)<sizeof row)sink(context,row);
    }
    static void course_list(char out[48],const int c[4]) {
        if(!c[3]) { strcpy(out,"null");return; }
        snprintf(out,48,"[%d,%d,%d,%d]",c[0],c[1],c[2],c[3]);
    }
    static const char* class_name(unsigned c) {
        static const char* const names[]={"UNDECODED","NO_FIX","FIX","LOST","NATIVE_DR","UTC_STALL"};
        return c<6?names[c]:"OTHER";
    }
    void write_edge(const Edge& e,uint64_t now,Sink sink,void* context) {
        if(!allow(EDGE))return;
        char yb[24],ya[24],vb[24],va[24],cb[48],ca[48];
        mean(yb,e.b);mean(ya,e.a);kmh(vb,e.b);kmh(va,e.a);course_list(cb,e.cb);course_list(ca,e.ca);
        char row[ROW_CAPACITY];
        const int n=snprintf(row,sizeof row,
            "{\"kind\":\"yaw_edge\",\"schema\":1,\"mono_ns\":%llu,\"at_s\":%.1f,\"ev\":\"%s\",\"from\":\"%s\","
            "\"to\":\"%s\",\"yb\":%s,\"nb\":%llu,\"vb\":%s,\"cb\":%s,\"ya\":%s,\"na\":%llu,\"va\":%s,\"ca\":%s,"
            "\"cut\":%d%s}",
            (unsigned long long)now,double(e.at/(100*MS))/10.0,e.loss?"loss":"return",class_name(e.from),
            class_name(e.to),yb,(unsigned long long)e.b.n,vb,cb,ya,(unsigned long long)e.a.n,va,ca,e.cut?1:0,
            e.lost?",\"hist_lost\":1":"");
        if(n>0 && size_t(n)<sizeof row)sink(context,row);
    }

    Bin bins_[BINS]; uint64_t newest_bin_;
    bool have_wheel_,stationary_; uint64_t last_wheel_t_;
    bool have_yaw_; uint64_t last_yaw_t_;
    Episode run_,pending_,ready_[READY]; unsigned ready_n_;
    Acc tail_,tail_first_; Recent recent_[RECENT]; unsigned recent_head_;
    uint64_t last_wheel_gap_t_,last_yaw_gap_t_,last_trigger_t_,last_zero_t_; bool have_zero_;
    bool have_last_; double last_mean_; uint64_t last_end_;
    Reinit reinit_,reinit_ready_; Edge edge_,edge_ready_;
    bool have_cls_,last_fix_; unsigned last_cls_;
    bool have_key_; Key key_;
    Fix fixes_[FIXES]; unsigned fix_n_,fix_head_;
    uint64_t tokens_[ROWS],refilled_[ROWS],suppressed_[ROWS];
    // digest period
    uint64_t since_;
    int64_t y1_[DIGEST_BINS]; uint64_t y1n_[DIGEST_BINS];
    int64_t yst_; uint64_t ystn_;
    unsigned gc_[DIGEST_FIXES],gc_n_,gc_more_,gq_,gv_;
    double d01_,d23_; uint64_t dn_;   // km/h sums (model wheel scale)
};

inline size_t YawStudyLog::putf(char* out,size_t cap,size_t n,const char* fmt,...) {
    if(n>=cap)return cap;
    va_list ap;va_start(ap,fmt);
    const int w=vsnprintf(out+n,cap-n,fmt,ap);
    va_end(ap);
    if(w<0 || size_t(w)>=cap-n)return cap;
    return n+size_t(w);
}

} }
#endif
