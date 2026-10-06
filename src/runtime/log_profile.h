#ifndef MX5_RUNTIME_LOG_PROFILE_H
#define MX5_RUNTIME_LOG_PROFILE_H
// Quiet journal profile for the always-on BETA product (log_profile=
// persistent; validation/PERSISTENT_LOGGING_2026-10-06.md). The full profile
// writes about 28-37 KB/s, mostly raw motion batches and ORIGINAL SEND rows.
// This filter sits in front of the journal writer and keeps:
//  * always: boot/shadow_boot/lifecycle rows, every beta_state, beta_anchor,
//    beta_reverse_latch, beta_hold and beta_session_storage row, every SEND
//    with choice != ORIGINAL, POSITION rows of the BETA candidate classes
//    (LOST, NO_FIX) and every POSITION whose class changed;
//  * rate-limited: beta_summary (1/s while BETA is in a live state, else 1 per
//    10 s), health/shadow/shadow_calibration (1 per 10 s), faults and
//    rejections (5 per kind per 10 s, the rest counted);
//  * instead of raw motion batches and ORIGINAL sends: one log_digest row per
//    10 s (speed/yaw statistics, event counts, sequence gaps, send/position
//    counts, suppressed-row counters);
//  * a preallocated in-memory RAW window (the last <= 60 s of raw motion
//    batches and ORIGINAL LOCATION sends/positions) written only around an
//    event (BETA state change, hold, session storage change, GPS class
//    transition, fault, late-arrival burst, capture stop), followed by 30 s
//    of raw rows written directly, so the evidence around each event stays
//    complete. Each such raw period starts with a raw_window marker row.
// Worker thread only. No allocation after init(), no I/O except through the
// emit callback, bounded work per row. OEM threads never reach this code.
#include "adapter/adapter.h"
#include "navigation/pipeline.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace mx5 { namespace runtime {

class PersistentLog {
public:
    // raw: the row is RAW context (window or post-event raw period), never
    // replacement evidence, so the journal ring may treat it as diagnostic.
    typedef void (*Emit)(void* context,const char* row,bool raw);
    static const uint64_t DIGEST_NS=10000000000ULL;
    static const uint64_t PERIODIC_NS=10000000000ULL;   // health, shadow, calibration
    static const uint64_t SUMMARY_LIVE_NS=1000000000ULL;
    static const uint64_t SUMMARY_IDLE_NS=10000000000ULL;
    static const uint64_t FAULT_WINDOW_NS=10000000000ULL;
    static const unsigned FAULT_ROWS=5;
    static const uint64_t RAW_PRE_NS=60000000000ULL;
    static const uint64_t RAW_POST_NS=30000000000ULL;
    static const size_t WINDOW_BYTES=196608;
    static const size_t ROW_BYTES=8193;

    PersistentLog():window_(0),cap_(0),scratch_(0) { reset_all(); }
    // storage: WINDOW_BYTES + ROW_BYTES bytes owned by the caller for the
    // lifetime of this object. Without storage the RAW window is unavailable
    // (raw rows are then only counted in the digest).
    void init(unsigned char* storage,const navigation::ModelProfile& model) {
        window_=storage;cap_=storage?WINDOW_BYTES:0;
        scratch_=storage?reinterpret_cast<char*>(storage+WINDOW_BYTES):0;
        model_=model;reset_all();
    }
    bool window_available() const { return cap_!=0; }

    // Every journal row except POSITION/SEND observations.
    void row(const char* s,uint64_t now,Emit emit,void* context) {
        digest_due(now,emit,context);
        const Kind k=kind_of(s);
        switch(k) {
        case K_MOTION_BATCH: raw(s,now,emit,context);return;
        case K_HEALTH:
            // The terminal health row of a capture stop is always kept.
            if(strstr(s,"\"capture_active\":false") || periodic(&last_health_,now))keep(s,emit,context);
            else ++suppressed_[S_HEALTH];
            return;
        case K_SHADOW:
            if(periodic(&last_shadow_,now))keep(s,emit,context);else ++suppressed_[S_SHADOW];
            return;
        case K_SHADOW_CALIBRATION:
            if(periodic(&last_calibration_,now))keep(s,emit,context);else ++suppressed_[S_CALIBRATION];
            return;
        case K_SHADOW_HOLDOUT:
            // Window boundaries are kept; per-reference comparisons are counted.
            if(strstr(s,"\"event\":\"BEGIN\"") || strstr(s,"\"event\":\"END\"") ||
               strstr(s,"\"event\":\"ABORT\""))keep(s,emit,context);
            else ++suppressed_[S_HOLDOUT];
            return;
        case K_LDS_SIDEBAND: ++suppressed_[S_LDS];return;
        case K_LDS_STATUS:
            if(strstr(s,"\"status\":\"rejected\"") || strstr(s,"\"status\":\"drain_limit\""))
                fault(S_LDS_STATUS,false,s,now,emit,context);
            else keep(s,emit,context);
            return;
        case K_MOTION_REJECTED: fault(S_MOTION_REJECTED,true,s,now,emit,context);return;
        case K_INPUT_RESET: fault(S_INPUT_RESET,true,s,now,emit,context);return;
        case K_PIPELINE_RESET: fault(S_PIPELINE_RESET,true,s,now,emit,context);return;
        case K_POSITION_REJECTED: fault(S_POSITION_REJECTED,false,s,now,emit,context);return;
        case K_MOTION_EXCLUDED: fault(S_MOTION_EXCLUDED,false,s,now,emit,context);return;
        case K_BETA_SUMMARY: {
            const bool live=strstr(s,"\"state\":\"ENGAGED\"") || strstr(s,"\"state\":\"SPEED_ENGAGED\"") ||
                strstr(s,"\"state\":\"GPS_LOST\"") || strstr(s,"\"state\":\"NO_FIX\"");
            if(!last_summary_ || now<last_summary_ ||
               now-last_summary_>=(live?SUMMARY_LIVE_NS:SUMMARY_IDLE_NS) ||
               strstr(s,"\"state\":\"DISABLED\"") || strstr(s,"\"state\":\"FAULT\"")) {
                last_summary_=now?now:1;keep(s,emit,context);
            } else ++suppressed_[S_SUMMARY];
            return;
        }
        case K_TRIGGER: trigger(kind_name(s),now,emit,context);keep(s,emit,context);return;
        case K_CAPTURE_END:
            trigger("capture_end",now,emit,context);
            digest(now,"final",emit,context);
            keep(s,emit,context);
            return;
        default: keep(s,emit,context);return;   // boot, beta_anchor, lifecycle, unknown kinds
        }
    }
    // POSITION/SEND observations (the formatted row and its values).
    void observation(const char* s,const adapter::Observation& o,uint64_t now,Emit emit,void* context) {
        digest_due(now,emit,context);
        if(o.kind==adapter::Observation::SEND) {
            ++sends_;if(o.result)++send_nonzero_;
            ++send_types_[o.type<TYPE_SLOTS-1?o.type:TYPE_SLOTS-1];
            if(o.choice!=adapter::ORIGINAL) { ++send_changed_;keep(s,emit,context);return; }
            if(o.type==1) { ++send_location_;raw(s,now,emit,context); }
            else ++suppressed_[S_SEND];
            return;
        }
        ++positions_;
        const unsigned mode=o.original_mode>=0 && o.original_mode<3?unsigned(o.original_mode):3;
        ++position_modes_[mode];
        const unsigned cls=unsigned(o.position_class)<CLASS_SLOTS?unsigned(o.position_class):0;
        ++position_classes_[cls];
        const bool changed=!have_class_ || cls!=last_class_;
        have_class_=true;last_class_=cls;
        if(o.reason==adapter::CONTEXT_UNAVAILABLE) { keep(s,emit,context);return; }
        if(changed) { trigger("position_class",now,emit,context);keep(s,emit,context);return; }
        if(o.position_class==adapter::POSITION_LOST || o.position_class==adapter::POSITION_NO_FIX_STALE) {
            keep(s,emit,context);return;
        }
        raw(s,now,emit,context);
    }
    // Every accepted motion event, for the digest statistics.
    void motion(const navigation::RawEvent& e) {
        if(total_events_!=UINT64_MAX)++total_events_;
        if(e.kind>=navigation::WHEELS && e.kind<=navigation::REVERSE) {
            ++kind_events_[e.kind-1];last_received_[e.kind-1]=e.received_ns;
        }
        if(have_motion_ && e.epoch==epoch_ && e.receive_seq!=last_seq_+1)++seq_gaps_;
        if(have_motion_ && e.received_ns>last_motion_ns_) {
            const uint64_t gap=e.received_ns-last_motion_ns_;if(gap>max_gap_ns_)max_gap_ns_=gap;
        }
        if(!period_events_++) { first_seq_=e.receive_seq; }
        have_motion_=true;epoch_=e.epoch;last_seq_=e.receive_seq;
        if(e.received_ns>last_motion_ns_)last_motion_ns_=e.received_ns;
        if(e.kind==navigation::WHEELS) {
            double kmh=0;for(unsigned i=0;i<4;++i)kmh+=(e.raw[i]*model_.wheel_kmh_per_count+model_.wheel_zero_kmh)*0.25;
            stat(&speed_,kmh);
        } else if(e.kind==navigation::YAW && e.count) {
            const double mean=double(e.raw[0])/e.count;
            stat(&yaw_raw_,mean);
            const double rate=(mean-model_.yaw_zero)*model_.yaw_rad_per_count;
            yaw_rate_sum_+=rate;if(fabs(rate)>yaw_rate_abs_max_)yaw_rate_abs_max_=fabs(rate);
        } else if(e.kind==navigation::REVERSE) reverse_value_=e.reverse;
    }
    // Worker tick: the digest stays on its 10 s cadence even without rows.
    void tick(uint64_t now,Emit emit,void* context) { digest_due(now,emit,context); }

private:
    enum Kind { K_OTHER=0, K_MOTION_BATCH, K_HEALTH, K_SHADOW, K_SHADOW_CALIBRATION, K_SHADOW_HOLDOUT,
                K_LDS_SIDEBAND, K_LDS_STATUS, K_MOTION_REJECTED, K_INPUT_RESET, K_PIPELINE_RESET,
                K_POSITION_REJECTED, K_MOTION_EXCLUDED, K_BETA_SUMMARY, K_TRIGGER, K_CAPTURE_END };
    enum Suppressed { S_HEALTH=0, S_SHADOW, S_CALIBRATION, S_HOLDOUT, S_LDS, S_LDS_STATUS,
                      S_MOTION_REJECTED, S_INPUT_RESET, S_PIPELINE_RESET, S_POSITION_REJECTED,
                      S_MOTION_EXCLUDED, S_SUMMARY, S_SEND, S_RAW_DROPPED, S_COUNT };
    static const unsigned TYPE_SLOTS=16, CLASS_SLOTS=6;
    struct Stat { uint64_t n; double min,max,sum; };
    static void stat(Stat* s,double v) {
        if(!s->n || v<s->min)s->min=v;
        if(!s->n || v>s->max)s->max=v;
        s->sum+=v;++s->n;
    }
    static const char* suppressed_name(unsigned i) {
        static const char* const names[]={"health","shadow","shadow_calibration","shadow_holdout",
            "lds_sideband","lds_sideband_status","motion_rejected","shadow_input_reset",
            "shadow_pipeline_reset","shadow_position_rejected","shadow_motion_excluded",
            "beta_summary","send_original","raw_window_dropped"};
        return names[i];
    }
    static Kind kind_of(const char* s) {
        static const char prefix[]="{\"kind\":\"";
        if(strncmp(s,prefix,sizeof prefix-1))return K_OTHER;
        const char* k=s+sizeof prefix-1;
        struct Entry { const char* name;Kind kind; };
        static const Entry table[]={
            {"motion_batch\"",K_MOTION_BATCH},{"health\"",K_HEALTH},{"shadow\"",K_SHADOW},
            {"shadow_calibration\"",K_SHADOW_CALIBRATION},{"shadow_holdout\"",K_SHADOW_HOLDOUT},
            {"lds_sideband\"",K_LDS_SIDEBAND},{"lds_sideband_status\"",K_LDS_STATUS},
            {"motion_rejected\"",K_MOTION_REJECTED},{"shadow_input_reset\"",K_INPUT_RESET},
            {"shadow_pipeline_reset\"",K_PIPELINE_RESET},{"shadow_position_rejected\"",K_POSITION_REJECTED},
            {"shadow_motion_excluded\"",K_MOTION_EXCLUDED},{"beta_summary\"",K_BETA_SUMMARY},
            {"beta_state\"",K_TRIGGER},{"beta_hold\"",K_TRIGGER},{"beta_session_storage\"",K_TRIGGER},
            {"capture_incomplete\"",K_TRIGGER},{"shadow_disabled\"",K_TRIGGER},
            {"motion_late_accepted\"",K_TRIGGER},{"capture_end\"",K_CAPTURE_END}};
        for(size_t i=0;i<sizeof table/sizeof table[0];++i)
            if(!strncmp(k,table[i].name,strlen(table[i].name)))return table[i].kind;
        return K_OTHER;
    }
    // The row's kind as a bounded token (for the raw_window trigger field).
    const char* kind_name(const char* s) {
        static const char prefix[]="{\"kind\":\"";
        size_t n=0;const char* k=s+sizeof prefix-1;
        while(n<sizeof trigger_-1 && k[n] && k[n]!='"' && ((k[n]>='a' && k[n]<='z') || k[n]=='_'))
            { trigger_[n]=k[n];++n; }
        trigger_[n]=0;
        return trigger_;
    }
    static void keep(const char* s,Emit emit,void* context) { emit(context,s,false); }
    bool periodic(uint64_t* last,uint64_t now) {
        if(*last && now>=*last && now-*last<PERIODIC_NS)return false;
        *last=now?now:1;return true;
    }
    void fault(Suppressed which,bool triggers,const char* s,uint64_t now,Emit emit,void* context) {
        if(!fault_since_[which] || now<fault_since_[which] || now-fault_since_[which]>=FAULT_WINDOW_NS) {
            fault_since_[which]=now?now:1;fault_rows_[which]=0;
        }
        if(fault_rows_[which]>=FAULT_ROWS) { ++suppressed_[which];return; }
        if(triggers && !fault_rows_[which])trigger(kind_name(s),now,emit,context);
        ++fault_rows_[which];keep(s,emit,context);
    }
    // A raw row: written directly during a raw period, else into the window.
    void raw(const char* s,uint64_t now,Emit emit,void* context) {
        if(now<raw_until_) { ++raw_direct_;emit(context,s,true);return; }
        if(!cap_) { ++suppressed_[S_RAW_DROPPED];return; }
        const size_t n=strlen(s),total=HEADER+n;
        if(n>=ROW_BYTES || total>cap_) { ++suppressed_[S_RAW_DROPPED];return; }
        expire(now);
        while(cap_-used_<total)drop_oldest();
        unsigned char h[HEADER];const uint32_t len=uint32_t(n);
        memcpy(h,&len,4);memcpy(h+4,&now,8);
        put(h,HEADER);put(s,n);++rows_;
    }
    // Event: write the window (oldest first) behind a raw_window marker and
    // keep writing raw rows directly for RAW_POST_NS.
    void trigger(const char* why,uint64_t now,Emit emit,void* context) {
        expire(now);
        const bool new_period=now>=raw_until_;
        if(new_period || rows_) {
            char line[400];
            const int n=snprintf(line,sizeof line,
                "{\"kind\":\"raw_window\",\"schema\":1,\"mono_ns\":%llu,\"profile\":\"persistent\","
                "\"trigger\":\"%s\",\"rows\":%llu,\"bytes\":%llu,\"overwritten_rows\":%llu,"
                "\"pre_limit_ms\":%llu,\"post_ms\":%llu,\"window\":\"%s\"}",
                (unsigned long long)now,why,(unsigned long long)rows_,(unsigned long long)used_,
                (unsigned long long)overwritten_,(unsigned long long)(RAW_PRE_NS/1000000ULL),
                (unsigned long long)(RAW_POST_NS/1000000ULL),cap_?"available":"unavailable");
            if(n>0 && size_t(n)<sizeof line)emit(context,line,false);
            while(rows_) {
                uint32_t len;uint64_t at;header(&len,&at);
                get((head_+HEADER)%cap_,scratch_,len);scratch_[len]=0;
                consume(len);
                emit(context,scratch_,true);
            }
            overwritten_=0;++flushes_;
        }
        const uint64_t until=now>UINT64_MAX-RAW_POST_NS?UINT64_MAX:now+RAW_POST_NS;
        if(until>raw_until_)raw_until_=until;
    }
    void digest_due(uint64_t now,Emit emit,void* context) {
        if(!since_) { since_=now?now:1;return; }
        if(now>=since_ && now-since_>=DIGEST_NS)digest(now,"periodic",emit,context);
    }
    static void number(char out[32],const Stat& s,int which) {
        if(!s.n) { strcpy(out,"null");return; }
        const double v=which==0?s.min:which==1?s.max:s.sum/double(s.n);
        snprintf(out,32,"%.6g",v);
    }
    void digest(uint64_t now,const char* what,Emit emit,void* context) {
        char smin[32],smax[32],smean[32],ymin[32],ymax[32],ymean[32],rate[32],rate_max[32];
        number(smin,speed_,0);number(smax,speed_,1);number(smean,speed_,2);
        number(ymin,yaw_raw_,0);number(ymax,yaw_raw_,1);number(ymean,yaw_raw_,2);
        if(yaw_raw_.n) { snprintf(rate,sizeof rate,"%.6g",yaw_rate_sum_/double(yaw_raw_.n));
                         snprintf(rate_max,sizeof rate_max,"%.6g",yaw_rate_abs_max_); }
        else { strcpy(rate,"null");strcpy(rate_max,"null"); }
        char types[400],suppressed[700],classes[200];
        size_t t=0;types[t++]='{';
        for(unsigned i=0;i<TYPE_SLOTS;++i) if(send_types_[i]) {
            char key[16];
            if(i==TYPE_SLOTS-1)strcpy(key,"other");else snprintf(key,sizeof key,"%u",i);
            const int n=snprintf(types+t,sizeof types-t,"%s\"%s\":%llu",t>1?",":"",key,
                                 (unsigned long long)send_types_[i]);
            if(n<0 || size_t(n)>=sizeof types-t)break;
            t+=size_t(n);
        }
        types[t++]='}';types[t]=0;
        size_t u=0;suppressed[u++]='{';
        for(unsigned i=0;i<S_COUNT;++i) if(suppressed_[i]) {
            const int n=snprintf(suppressed+u,sizeof suppressed-u,"%s\"%s\":%llu",u>1?",":"",
                                 suppressed_name(i),(unsigned long long)suppressed_[i]);
            if(n<0 || size_t(n)>=sizeof suppressed-u)break;
            u+=size_t(n);
        }
        suppressed[u++]='}';suppressed[u]=0;
        static const char* const class_names[CLASS_SLOTS]={"UNDECODED","NO_FIX","FIX","LOST","NATIVE_DR","UTC_STALL"};
        size_t c=0;classes[c++]='{';
        for(unsigned i=0;i<CLASS_SLOTS;++i) if(position_classes_[i]) {
            const int n=snprintf(classes+c,sizeof classes-c,"%s\"%s\":%llu",c>1?",":"",class_names[i],
                                 (unsigned long long)position_classes_[i]);
            if(n<0 || size_t(n)>=sizeof classes-c)break;
            c+=size_t(n);
        }
        classes[c++]='}';classes[c]=0;
        char last[3][24];
        for(unsigned i=0;i<3;++i) {
            if(last_received_[i])snprintf(last[i],sizeof last[i],"%llu",(unsigned long long)last_received_[i]);
            else strcpy(last[i],"null");
        }
        char line[2400];
        const int n=snprintf(line,sizeof line,
            "{\"kind\":\"log_digest\",\"schema\":1,\"digest\":\"%s\",\"profile\":\"persistent\","
            "\"mono_ns\":%llu,\"since_ns\":%llu,\"domain\":\"model\",\"assist_ready\":false,"
            "\"motion_events\":%llu,\"wheels\":%llu,\"yaw\":%llu,\"reverse\":%llu,\"motion_epoch\":%llu,"
            "\"first_seq\":%llu,\"last_seq\":%llu,\"seq_gaps\":%llu,\"max_receipt_gap_ms\":%llu,"
            "\"speed_kmh_min\":%s,\"speed_kmh_max\":%s,\"speed_kmh_mean\":%s,"
            "\"yaw_raw_min\":%s,\"yaw_raw_max\":%s,\"yaw_raw_mean\":%s,"
            "\"yaw_rate_mean\":%s,\"yaw_rate_abs_max\":%s,\"reverse_value\":%d,"
            "\"wheels_last_ns\":%s,\"yaw_last_ns\":%s,\"reverse_last_ns\":%s,"
            "\"sends\":%llu,\"send_location\":%llu,\"send_changed\":%llu,\"send_nonzero\":%llu,"
            "\"send_types\":%s,\"positions\":%llu,\"position_modes\":[%llu,%llu,%llu,%llu],"
            "\"position_classes\":%s,\"suppressed\":%s,"
            "\"raw_window_rows\":%llu,\"raw_window_bytes\":%llu,\"raw_window_overwritten\":%llu,"
            "\"raw_window_flushes\":%llu,\"raw_direct_rows\":%llu}",
            what,(unsigned long long)now,(unsigned long long)since_,
            (unsigned long long)period_events_,(unsigned long long)kind_events_[0],
            (unsigned long long)kind_events_[1],(unsigned long long)kind_events_[2],
            (unsigned long long)epoch_,(unsigned long long)(period_events_?first_seq_:0),
            (unsigned long long)(period_events_?last_seq_:0),(unsigned long long)seq_gaps_,
            (unsigned long long)(max_gap_ns_/1000000ULL),smin,smax,smean,ymin,ymax,ymean,rate,rate_max,
            reverse_value_,last[0],last[1],last[2],
            (unsigned long long)sends_,(unsigned long long)send_location_,(unsigned long long)send_changed_,
            (unsigned long long)send_nonzero_,types,(unsigned long long)positions_,
            (unsigned long long)position_modes_[0],(unsigned long long)position_modes_[1],
            (unsigned long long)position_modes_[2],(unsigned long long)position_modes_[3],classes,suppressed,
            (unsigned long long)rows_,(unsigned long long)used_,(unsigned long long)overwritten_,
            (unsigned long long)flushes_,(unsigned long long)raw_direct_);
        if(n>0 && size_t(n)<sizeof line)emit(context,line,false);
        reset_period(now);
    }
    void reset_period(uint64_t now) {
        since_=now?now:1;period_events_=0;
        for(unsigned i=0;i<3;++i)kind_events_[i]=0;
        seq_gaps_=0;max_gap_ns_=0;first_seq_=0;
        speed_=Stat();yaw_raw_=Stat();yaw_rate_sum_=0;yaw_rate_abs_max_=0;
        sends_=send_location_=send_changed_=send_nonzero_=0;
        for(unsigned i=0;i<TYPE_SLOTS;++i)send_types_[i]=0;
        positions_=0;for(unsigned i=0;i<4;++i)position_modes_[i]=0;
        for(unsigned i=0;i<CLASS_SLOTS;++i)position_classes_[i]=0;
        for(unsigned i=0;i<S_COUNT;++i)suppressed_[i]=0;
        raw_direct_=0;
    }
    void reset_all() {
        head_=used_=0;rows_=overwritten_=flushes_=0;raw_until_=0;
        last_health_=last_shadow_=last_calibration_=last_summary_=0;
        for(unsigned i=0;i<S_COUNT;++i) { fault_since_[i]=0;fault_rows_[i]=0; }
        have_class_=false;last_class_=0;have_motion_=false;epoch_=last_seq_=last_motion_ns_=0;total_events_=0;
        for(unsigned i=0;i<3;++i)last_received_[i]=0;
        reverse_value_=-1;trigger_[0]=0;
        reset_period(0);since_=0;
    }
    // ---- RAW window: rows [u32 length][u64 mono_ns][bytes], oldest first ----
    static const size_t HEADER=12;
    void put(const void* p,size_t n) {
        const unsigned char* s=static_cast<const unsigned char*>(p);
        const size_t tail=(head_+used_)%cap_,first=n<cap_-tail?n:cap_-tail;
        memcpy(window_+tail,s,first);memcpy(window_,s+first,n-first);used_+=n;
    }
    void get(size_t at,void* p,size_t n) const {
        unsigned char* d=static_cast<unsigned char*>(p);
        const size_t first=n<cap_-at?n:cap_-at;
        memcpy(d,window_+at,first);memcpy(d+first,window_,n-first);
    }
    void header(uint32_t* len,uint64_t* at) const {
        unsigned char h[HEADER];get(head_,h,HEADER);memcpy(len,h,4);memcpy(at,h+4,8);
    }
    void consume(uint32_t len) { head_=(head_+HEADER+len)%cap_;used_-=HEADER+len;--rows_; }
    void drop_oldest() { uint32_t len;uint64_t at;header(&len,&at);consume(len);++overwritten_; }
    void expire(uint64_t now) {
        while(rows_) {
            uint32_t len;uint64_t at;header(&len,&at);
            if(now<RAW_PRE_NS || at>=now-RAW_PRE_NS)break;
            consume(len);++overwritten_;
        }
    }

    unsigned char* window_;size_t cap_;char* scratch_;
    navigation::ModelProfile model_;
    size_t head_,used_;
    uint64_t rows_,overwritten_,flushes_,raw_until_,raw_direct_;
    uint64_t last_health_,last_shadow_,last_calibration_,last_summary_;
    uint64_t fault_since_[S_COUNT];unsigned fault_rows_[S_COUNT];
    uint64_t suppressed_[S_COUNT];
    bool have_class_;unsigned last_class_;
    // digest period
    uint64_t total_events_,since_,period_events_,kind_events_[3],seq_gaps_,max_gap_ns_,first_seq_;
    bool have_motion_;uint64_t epoch_,last_seq_,last_motion_ns_,last_received_[3];
    Stat speed_,yaw_raw_;double yaw_rate_sum_,yaw_rate_abs_max_;int reverse_value_;
    uint64_t sends_,send_location_,send_changed_,send_nonzero_,send_types_[TYPE_SLOTS];
    uint64_t positions_,position_modes_[4],position_classes_[CLASS_SLOTS];
    char trigger_[40];
    PersistentLog(const PersistentLog&);
    PersistentLog& operator=(const PersistentLog&);
};

} }
#endif
