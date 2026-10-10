#ifndef MX5_RUNTIME_LOG_PROFILE_H
#define MX5_RUNTIME_LOG_PROFILE_H
// Quiet journal profile for the always-on BETA product (log_profile=
// persistent; validation/PERSISTENT_LOGGING_2026-10-06.md). The full profile
// writes about 28-37 KB/s, mostly raw motion batches and ORIGINAL SEND rows.
// This filter sits in front of the journal writer and keeps:
//  * always written: boot/shadow_boot/lifecycle rows, every beta_state,
//    beta_anchor, beta_reverse_latch, beta_hold and beta_session_storage row,
//    every SEND with choice != ORIGINAL, POSITION rows of the BETA candidate
//    classes (LOST, NO_FIX) and every POSITION whose class changed. Other
//    POSITION rows (FIX) are RAW context (window/raw period below); while
//    BETA is live (last beta_state not DISABLED/FAULT) and the journal lag
//    guard is not engaged they are queued in the journal ring's evidence
//    class so a write backlog cannot drop them;
//  * rate-limited: beta_summary (1/s while BETA is in a live state, else 1 per
//    10 s), health/shadow/shadow_calibration (1 per 10 s), faults and
//    rejections (5 per kind per 10 s, the rest counted);
//  * instead of raw motion batches and ORIGINAL sends: one log_digest row per
//    10 s (speed/yaw statistics, event counts, sequence gaps, send/position
//    counts, suppressed-row counters);
//  * a preallocated in-memory RAW window (raw motion batches and ORIGINAL
//    LOCATION sends/positions of at most the last 60 s, and at most
//    WINDOW_BYTES: 60 s at the vehicle's ~10 Hz wheel/yaw cadence, about
//    23 s at 50 Hz) written only around an event (BETA state change, hold,
//    session storage change, GPS class transition, fault, late-arrival
//    burst, capture stop), followed by 30 s of raw rows written directly.
//    Each such raw period starts with a raw_window marker row whose span_ms
//    is how far back the written rows really reach. Without an event (CMU
//    reset, process death) the window in memory is lost.
//    Paced drain (2026-10-08): the first persistent BETA drive flushed the
//    ~400 KiB window (about 1100 rows) in one burst at every GPS loss and
//    return; the writer drained it at about 350-630 rows/s, so the journal
//    lag guard withdrew BETA provenance exactly when BETA should engage.
//    Now the marker is written at the event and the window rows follow at
//    most DRAIN_ROWS_PER_S (token bucket, DRAIN_BURST rows) from pump(),
//    which the worker calls every turn and which waits while the journal
//    still has prompt rows or earlier window rows queued (so the drain
//    also follows a slower writer). Current raw rows arriving during the
//    drain are written directly as in any raw period (untagged; evidence
//    POSITION rows in the evidence class), so they are timed by the journal
//    lag and never wait in profile RAM; the older tagged window rows are
//    interleaved after them (2026-10-08 review M1). A capture
//    stop writes the rest at once. Each row written from the window carries
//    "raw_window":true right after its kind (the analyzer then knows it is
//    older context, interleaved with current rows) and is queued in the
//    journal ring's BULK class, which the journal lag does not time; window
//    POSITION rows that are evidence (BETA live, journal current) keep the
//    evidence class.
//  * yaw-zero data collection (2026-10-09, yaw_study_log.h and
//    validation/YAW_DATA_COLLECTION_2026-10-09.md): yaw_stop, yaw_edge and
//    yaw_reinit rows and extra log_digest fields. Logging only; written as
//    diagnostic rows (ROW_RAW, never evidence and never into the RAW window),
//    rate-limited per kind with the rest counted in the digest's suppressed
//    map. set_yaw_rows(false) turns them off (tests: everything else is
//    then byte-identical).
//  * VIM side-channel rows (2026-10-10, chan_digest_log.h and
//    validation/VIM_CHANNEL_CAPTURE_2026-10-10.md): one chan_digest row per
//    20 s with raw statistics of the longitudinal/lateral acceleration,
//    brake pressure, Qf bits, speed and rpm from channels(). Logging only,
//    diagnostic rows (ROW_RAW), written only once side-channel input
//    arrived. set_chan_rows(false) turns them off.
// Worker thread only. No allocation after init(), no I/O except through the
// emit callback, bounded work per row. OEM threads never reach this code.
#include "adapter/adapter.h"
#include "navigation/pipeline.h"
#include "runtime/yaw_study_log.h"
#include "runtime/chan_digest_log.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace mx5 { namespace runtime {

class PersistentLog {
public:
    // How the journal queues a row: KEEP by its kind (evidence or
    // diagnostic, journal_evidence_row), RAW as diagnostic (RAW context:
    // window or post-event raw period, never replacement evidence), BULK in
    // the low-priority class that the journal lag does not time (paced
    // window rows).
    enum RowClass { ROW_KEEP=0, ROW_RAW=1, ROW_BULK=2 };
    typedef void (*Emit)(void* context,const char* row,unsigned row_class);
    // pump(): false while rows that must be durable promptly, or window rows
    // of an earlier pump, still wait in the journal.
    typedef bool (*Ready)(void* context);
    static const uint64_t DIGEST_NS=10000000000ULL;
    static const uint64_t PERIODIC_NS=10000000000ULL;   // health, shadow, calibration
    static const uint64_t SUMMARY_LIVE_NS=1000000000ULL;
    static const uint64_t SUMMARY_IDLE_NS=10000000000ULL;
    static const uint64_t FAULT_WINDOW_NS=10000000000ULL;
    static const unsigned FAULT_ROWS=5;
    static const uint64_t RAW_PRE_NS=60000000000ULL;
    static const uint64_t RAW_POST_NS=30000000000ULL;
    // 400 KiB: about 60 s at the vehicle cadence (2026-10-05: wheels and yaw
    // about 10 Hz each, 1 Hz POSITION+LOCATION; measured 6.2 KB/s of RAW
    // rows with tests/runtime/log_rate.cpp). It must stay below the journal
    // ring's diagnostic capacity (512 KiB) so a whole flush can be queued.
    static const size_t WINDOW_BYTES=409600;
    static const size_t ROW_BYTES=8193;
    // Paced window drain (2026-10-08): well below the 350-630 rows/s the
    // vehicle writer reached in the burst, so prompt rows never queue behind
    // more than DRAIN_BURST window rows. 1100 rows take about 7.3 s.
    static const unsigned DRAIN_ROWS_PER_S=150;
    static const unsigned DRAIN_BURST=16;
    // ,"raw_window":true after the kind of a row written from the window.
    static const size_t WINDOW_TAG_BYTES=18;

    PersistentLog():window_(0),cap_(0),scratch_(0),paced_(DRAIN_ROWS_PER_S),yaw_rows_(true),chan_rows_(true) { reset_all(); }
    // Yaw-zero data rows and digest fields (default on).
    void set_yaw_rows(bool on) { yaw_rows_=on; }
    // VIM side-channel rows (default on; written only after channels() input).
    void set_chan_rows(bool on) { chan_rows_=on; }
    // One VIM side-channel datagram (decoded batch, or 0 when rejected).
    // Logging only: never motion, never a decision input.
    void channels(const navigation::ChanBatch* b) { if(chan_rows_)chan_.batch(b); }
    // Rows per second of the window drain; 0 writes the whole window at the
    // event (the behaviour before 2026-10-08; unit tests of window contents).
    void set_paced(unsigned rows_per_s) { paced_=rows_per_s; }
    bool draining() const { return draining_; }
    // Appends the yaw digest fields and the closing brace to a formatted
    // digest of length n. The yaw fields give way first: when they do not
    // fit, ",\"yaw_dropped\":1" (or nothing) is written instead, so the
    // digest itself is never lost to them. 0: the digest alone does not fit.
    static size_t close_digest(char* line,size_t capacity,size_t n,const char* fields) {
        static const char dropped[]=",\"yaw_dropped\":1";
        if(n+2>capacity)return 0;
        const size_t f=strlen(fields);
        if(n+f+2<=capacity) { memcpy(line+n,fields,f);n+=f; }
        else if(f && n+sizeof dropped-1+2<=capacity) { memcpy(line+n,dropped,sizeof dropped-1);n+=sizeof dropped-1; }
        line[n++]='}';line[n]=0;
        return n;
    }
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
        case K_TRIGGER:
            if(!strncmp(s,"{\"kind\":\"beta_state\"",20)) {
                // Follow BETA liveness: while BETA may act, every POSITION
                // row that is written is evidence (NO_FIX overlay checks,
                // GPS-return measurement), even a raw-context one.
                beta_live_=!strstr(s,"\"to\":\"DISABLED\"") && !strstr(s,"\"to\":\"FAULT\"");
            }
            trigger(kind_name(s),now,emit,context);keep(s,emit,context);return;
        case K_CAPTURE_END:
            trigger("capture_end",now,emit,context,true);
            if(yaw_rows_) { YawThunk t={emit,context};yaw_.flush(now,yaw_sink,&t); }
            if(chan_rows_) { YawThunk t={emit,context};chan_.flush(now,yaw_sink,&t); }
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
        if(yaw_rows_)yaw_.position(o);
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
        raw(s,now,emit,context,beta_live_);
    }
    bool beta_live() const { return beta_live_; }
    // The journal lag guard's state (runtime.cpp journal_current). While it
    // is false, raw-context POSITION rows are diagnostic class even while
    // BETA is live: no replacement can be selected then, and a long storage
    // stall must not fill the evidence ring with FIX rows (2026-10-07). Read
    // when a row is emitted, so window rows buffered earlier follow it too.
    void set_journal_current(bool current) { journal_current_=current; }
    // Every accepted motion event, for the digest statistics.
    void motion(const navigation::RawEvent& e) {
        if(total_events_!=UINT64_MAX)++total_events_;
        if(yaw_rows_)yaw_.motion(e,model_);
        if(chan_rows_)chan_.wheels(e,model_);
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
    // Every worker turn: write window rows of a paced drain, at most the
    // token bucket allows, and none while ready() reports rows still queued
    // in the journal. Bounded work: at most DRAIN_BURST rows.
    void pump(uint64_t now,Emit emit,void* context,Ready ready=0,void* ready_context=0) {
        if(!draining_)return;
        if(now>drain_last_ns_) {
            const uint64_t add=(now-drain_last_ns_)/1000000ULL*paced_;   // milli-rows
            drain_tokens_=drain_tokens_+add>DRAIN_BURST*1000ULL?DRAIN_BURST*1000ULL:drain_tokens_+add;
            drain_last_ns_=now;
        }
        if(ready && !ready(ready_context))return;
        while(rows_ && drain_tokens_>=1000ULL) { drain_tokens_-=1000ULL;emit_window_row(emit,context,ROW_BULK); }
        if(!rows_) { draining_=false;++drains_done_; }
    }

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
    static void keep(const char* s,Emit emit,void* context) { emit(context,s,ROW_KEEP); }
    // The oldest window row, tagged "raw_window":true after its kind. An
    // evidence row (BETA live, journal current at write time) keeps its
    // class; any other goes as `other` (RAW: diagnostic, BULK: paced).
    void emit_window_row(Emit emit,void* context,unsigned other) {
        uint32_t len;uint64_t at;bool evidence;header(&len,&at,&evidence);
        get((head_+HEADER)%cap_,scratch_,len);scratch_[len]=0;
        consume(len);
        static const char prefix[]="{\"kind\":\"";
        static const char tag[]=",\"raw_window\":true";
        const size_t p=sizeof prefix-1;
        if(len>p && !memcmp(scratch_,prefix,p) && len+WINDOW_TAG_BYTES<ROW_BYTES) {
            const char* end=static_cast<const char*>(memchr(scratch_+p,'"',len-p));
            if(end) {
                const size_t at_tag=size_t(end-scratch_)+1;
                memmove(scratch_+at_tag+WINDOW_TAG_BYTES,scratch_+at_tag,len-at_tag+1);
                memcpy(scratch_+at_tag,tag,WINDOW_TAG_BYTES);
            }
        }
        ++window_written_;
        emit(context,scratch_,evidence && journal_current_?unsigned(ROW_KEEP):other);
    }
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
    // evidence: written in the journal ring's evidence class (POSITION rows
    // while BETA is live); otherwise RAW context is diagnostic class.
    void raw(const char* s,uint64_t now,Emit emit,void* context,bool evidence=false) {
        // Current rows are direct during the raw period, also while the
        // older window rows are still being drained (review M1: queuing
        // them behind the window hid them from the lag guard for ~7 s).
        if(now<raw_until_) {
            ++raw_direct_;emit(context,s,evidence && journal_current_?ROW_KEEP:ROW_RAW);return;
        }
        if(!cap_) { ++suppressed_[S_RAW_DROPPED];return; }
        const size_t n=strlen(s),total=HEADER+n;
        if(n+WINDOW_TAG_BYTES>=ROW_BYTES || total>cap_) { ++suppressed_[S_RAW_DROPPED];return; }
        if(!draining_)expire(now);   // rows being drained are older than 60 s by design
        while(cap_-used_<total)drop_oldest();
        unsigned char h[HEADER];const uint32_t len=uint32_t(n)|(evidence?EVIDENCE_BIT:0);
        memcpy(h,&len,4);memcpy(h+4,&now,8);
        put(h,HEADER);put(s,n);++rows_;
    }
    // Event: a raw_window marker, then the window (oldest first): paced by
    // pump(), or at once when not paced or `immediate` (capture stop); raw
    // rows are then written directly for RAW_POST_NS. An event during a
    // paced drain extends the raw period (and finishes the drain at once
    // when immediate); its rows were already announced by the marker.
    void trigger(const char* why,uint64_t now,Emit emit,void* context,bool immediate=false) {
        if(draining_) {
            if(immediate) { while(rows_)emit_window_row(emit,context,ROW_RAW);draining_=false;++drains_done_; }
            const uint64_t until=now>UINT64_MAX-RAW_POST_NS?UINT64_MAX:now+RAW_POST_NS;
            if(until>raw_until_)raw_until_=until;
            return;
        }
        expire(now);
        const bool new_period=now>=raw_until_;
        const bool paced=paced_ && !immediate && rows_;
        if(new_period || rows_) {
            char line[400];
            const int n=snprintf(line,sizeof line,
                "{\"kind\":\"raw_window\",\"schema\":1,\"mono_ns\":%llu,\"profile\":\"persistent\","
                "\"trigger\":\"%s\",\"rows\":%llu,\"bytes\":%llu,\"overwritten_rows\":%llu,"
                "\"span_ms\":%llu,\"pre_limit_ms\":%llu,\"post_ms\":%llu,\"window\":\"%s\","
                "\"drain\":\"%s\",\"drain_rows_per_s\":%u}",
                (unsigned long long)now,why,(unsigned long long)rows_,(unsigned long long)used_,
                (unsigned long long)overwritten_,(unsigned long long)span_ms(now),(unsigned long long)(RAW_PRE_NS/1000000ULL),
                (unsigned long long)(RAW_POST_NS/1000000ULL),cap_?"available":"unavailable",
                paced?"paced":"immediate",paced?paced_:0U);
            if(n>0 && size_t(n)<sizeof line)emit(context,line,ROW_KEEP);
            if(paced) { draining_=true;drain_last_ns_=now;drain_tokens_=DRAIN_BURST*1000ULL; }
            else while(rows_)emit_window_row(emit,context,ROW_RAW);
            overwritten_=0;++flushes_;
        }
        const uint64_t until=now>UINT64_MAX-RAW_POST_NS?UINT64_MAX:now+RAW_POST_NS;
        if(until>raw_until_)raw_until_=until;
    }
    void digest_due(uint64_t now,Emit emit,void* context) {
        if(!since_) { since_=now?now:1;yaw_.period(since_);return; }
        if(now>=since_ && now-since_>=DIGEST_NS)digest(now,"periodic",emit,context);
        if(yaw_rows_) { YawThunk t={emit,context};yaw_.poll(now,yaw_sink,&t); }
        if(chan_rows_) { YawThunk t={emit,context};chan_.poll(now,yaw_sink,&t); }
    }
    struct YawThunk { Emit emit;void* context; };
    // Yaw and chan_digest rows are diagnostic class whatever their kind (ROW_RAW).
    static void yaw_sink(void* thunk,const char* row) {
        const YawThunk* t=static_cast<const YawThunk*>(thunk);t->emit(t->context,row,ROW_RAW);
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
        char types[400],suppressed[800],classes[200];
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
        if(yaw_rows_) for(unsigned i=0;i<YawStudyLog::ROWS;++i) if(yaw_.suppressed(i)) {
            const int n=snprintf(suppressed+u,sizeof suppressed-u,"%s\"%s\":%llu",u>1?",":"",
                                 YawStudyLog::name(i),(unsigned long long)yaw_.suppressed(i));
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
        char yaw[YawStudyLog::DIGEST_CAPACITY];yaw[0]=0;
        if(yaw_rows_ && !yaw_.digest_fields(yaw,sizeof yaw))yaw[0]=0;
        char line[2400+YawStudyLog::DIGEST_CAPACITY];
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
            "\"raw_window_flushes\":%llu,\"raw_direct_rows\":%llu,\"raw_window_draining\":%s,"
            "\"raw_window_written\":%llu",
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
            (unsigned long long)flushes_,(unsigned long long)raw_direct_,draining_?"true":"false",
            (unsigned long long)window_written_);
        if(n>0 && close_digest(line,sizeof line,size_t(n),yaw))emit(context,line,ROW_KEEP);
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
        yaw_.period(since_);yaw_.clear_suppressed();
    }
    void reset_all() {
        head_=used_=0;rows_=overwritten_=flushes_=0;raw_until_=0;
        draining_=false;drain_last_ns_=drain_tokens_=0;drains_done_=window_written_=0;
        last_health_=last_shadow_=last_calibration_=last_summary_=0;
        for(unsigned i=0;i<S_COUNT;++i) { fault_since_[i]=0;fault_rows_[i]=0; }
        have_class_=false;last_class_=0;beta_live_=false;journal_current_=true;have_motion_=false;epoch_=last_seq_=last_motion_ns_=0;total_events_=0;
        for(unsigned i=0;i<3;++i)last_received_[i]=0;
        reverse_value_=-1;trigger_[0]=0;
        yaw_.reset();chan_.reset();
        reset_period(0);since_=0;
    }
    // ---- RAW window: rows [u32 length|evidence bit][u64 mono_ns][bytes], oldest first ----
    static const size_t HEADER=12;
    static const uint32_t EVIDENCE_BIT=0x80000000U;
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
    void header(uint32_t* len,uint64_t* at,bool* evidence=0) const {
        unsigned char h[HEADER];get(head_,h,HEADER);memcpy(len,h,4);memcpy(at,h+4,8);
        if(evidence)*evidence=(*len&EVIDENCE_BIT)!=0;
        *len&=~EVIDENCE_BIT;
    }
    // How far back the held rows reach (the real pre-event span).
    uint64_t span_ms(uint64_t now) const {
        if(!rows_)return 0;
        uint32_t len;uint64_t at;header(&len,&at);
        return now>at?(now-at)/1000000ULL:0;
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
    unsigned paced_;
    bool draining_;
    uint64_t drain_last_ns_,drain_tokens_,drains_done_,window_written_;   // tokens in milli-rows
    navigation::ModelProfile model_;
    size_t head_,used_;
    uint64_t rows_,overwritten_,flushes_,raw_until_,raw_direct_;
    uint64_t last_health_,last_shadow_,last_calibration_,last_summary_;
    uint64_t fault_since_[S_COUNT];unsigned fault_rows_[S_COUNT];
    uint64_t suppressed_[S_COUNT];
    bool have_class_;unsigned last_class_;bool beta_live_;bool journal_current_;
    // digest period
    uint64_t total_events_,since_,period_events_,kind_events_[3],seq_gaps_,max_gap_ns_,first_seq_;
    bool have_motion_;uint64_t epoch_,last_seq_,last_motion_ns_,last_received_[3];
    Stat speed_,yaw_raw_;double yaw_rate_sum_,yaw_rate_abs_max_;int reverse_value_;
    uint64_t sends_,send_location_,send_changed_,send_nonzero_,send_types_[TYPE_SLOTS];
    uint64_t positions_,position_modes_[4],position_classes_[CLASS_SLOTS];
    char trigger_[40];
    bool yaw_rows_;
    YawStudyLog yaw_;
    bool chan_rows_;
    ChanDigestLog chan_;
    PersistentLog(const PersistentLog&);
    PersistentLog& operator=(const PersistentLog&);
};

} }
#endif
