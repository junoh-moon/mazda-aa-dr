#ifndef MX5_RUNTIME_BETA_CONTROLLER_H
#define MX5_RUNTIME_BETA_CONTROLLER_H
// v1.0 beta runtime wiring (validation/ASSIST_BETA_DESIGN_2026-10-05.md
// decisions 5, 6 and 8). Two halves:
//  * BetaShared + the beta_* OEM-thread helpers: lock-free atomics only. They
//    run inside OEM POSITION/SEND calls and therefore never allocate, block,
//    throw, trap, retain or dereference OEM pointers.
//  * BetaController: owned by the single journal worker. It decides the state
//    machine, publishes/withdraws the BETA DrSnapshot and writes every
//    transition as a journal row. It never sets qualified flags or
//    Domain::QUALIFIED and never touches allow_assist.
#include "adapter/adapter.h"
#include "navigation/pipeline.h"
#include "runtime/beta_profile.h"
#include "runtime/core_bridge.h"
#include <atomic>
#include <cmath>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace mx5 { namespace runtime {

// Process-lifetime state shared with OEM threads. Zero-initialized static
// storage; every member is a lock-free 32-bit atomic on the ARM32 target.
struct BetaShared {
    std::atomic<unsigned> active;        // provenance may claim Domain::BETA
    std::atomic<uint32_t> source_epoch;  // worker copy of the ModelBus epoch (0: none)
    // Design S3/S6: count of distinct send-time session_storage values. It is
    // the BETA session_epoch. 0 = no send observed yet (never publishable).
    std::atomic<uint32_t> storage_epoch;
    std::atomic<uintptr_t> storage;      // identity only; never dereferenced
    std::atomic<uint32_t> hold_set, hold_cleared; // Options.beta_event counts
};
static_assert(ATOMIC_INT_LOCK_FREE==2 && (sizeof(uintptr_t)==sizeof(unsigned) || ATOMIC_LONG_LOCK_FREE==2),
              "BETA OEM-thread state must be lock-free");

// OEM SEND thread (Options.send_storage, after the session reader). A changed storage argument bumps
// the counter and then revokes the adapter generation, so the very send that
// observed the change, and every candidate published before it, passes the
// original. The counter saturates instead of wrapping (wrap could revive an
// old epoch); the worker treats saturation as a FAULT.
inline void beta_observe_storage(BetaShared& s,const void* storage) {
    const uintptr_t p=reinterpret_cast<uintptr_t>(storage);
    if(s.storage.load(std::memory_order_acquire)==p)return;
    if(s.storage.exchange(p,std::memory_order_acq_rel)==p)return;
    uint32_t c=s.storage_epoch.load(std::memory_order_acquire);
    if(c!=UINT32_MAX)
        s.storage_epoch.compare_exchange_strong(c,c+1,std::memory_order_acq_rel,
                                                std::memory_order_acquire);
    adapter::invalidate();
}
// OEM SEND thread (Options.beta_event). Counts for the worker journal. A
// hold_set also revokes the generation (count first, then revoke), so the
// failed candidate can never be selected again even after the adapter clears
// its hold on the next ORIGINAL 0; any later candidate needs a worker that
// has already seen this count and withdrawn (decision 8).
inline void beta_count_event(BetaShared& s,const char* what) {
    if(!what)return;
    if(!strcmp(what,"hold_set")) {
        s.hold_set.fetch_add(1,std::memory_order_acq_rel);adapter::invalidate();
    } else if(!strcmp(what,"hold_cleared"))s.hold_cleared.fetch_add(1,std::memory_order_acq_rel);
}
// OEM POSITION thread (provenance reader). BETA provenance (design decision 5):
//  * domain BETA: the snapshot may feed only Mode::BETA (choose_dr rejects it).
//  * source_epoch: the worker's ModelBus epoch (LDS bus lifetime fence).
//  * session_epoch: the send-time session_storage counter above.
//  * exact_request / verified_lds / legacy_receiver stay FALSE. They are the
//    qualified-domain claims (verified per-request provider, verified LDS
//    record, verified legacy receiver). BETA does not establish any of them
//    and choose_beta does not read them; the per-call evidence BETA relies on
//    is the original mode 0 of this same POSITION, checked by the adapter.
// Returns false (no BETA claim) unless the worker armed BETA and both epochs
// are non-zero. Nothing here can produce Domain::QUALIFIED.
inline bool beta_provenance(const BetaShared& s,adapter::Provenance* out) {
    *out=adapter::Provenance();
    if(!s.active.load(std::memory_order_acquire))return false;
    const uint32_t source=s.source_epoch.load(std::memory_order_acquire);
    const uint32_t session=s.storage_epoch.load(std::memory_order_acquire);
    if(!source || !session || session==UINT32_MAX)return false;
    out->source_epoch=source;out->session_epoch=session;
    out->exact_request=false;out->verified_lds=false;out->legacy_receiver=false;
    out->domain=adapter::Provenance::Domain::BETA;
    return true;
}

// NO_FIX / SPEED_ENGAGED (BETA_DECISIONS_2026-10-05.md 1-2): the stored
// no-fix position (mode 1/2, utc 0) may only receive the wheel speed overlay.
// GPS_LOST / ENGAGED: class LOST (mode 0), the DR replacement.
enum BetaState { BETA_DISABLED=0, BETA_ARMED, BETA_GPS_LOST, BETA_ENGAGED,
                 BETA_WITHDRAWN, BETA_FAULT, BETA_NO_FIX, BETA_SPEED_ENGAGED };
inline const char* beta_state_name(BetaState s) {
    static const char* const names[]={"DISABLED","ARMED","GPS_LOST","ENGAGED","WITHDRAWN","FAULT",
                                      "NO_FIX","SPEED_ENGAGED"};
    return unsigned(s)<sizeof names/sizeof names[0]?names[s]:"UNKNOWN";
}
// Sensor silence (design fault-injection list): no motion receipt for this long.
static const uint64_t BETA_SENSOR_SILENCE_NS=300000000ULL;

// Worker-owned. J must provide line(const char*), fail() and a bool failed.
class BetaController {
public:
    struct Counters {
        uint64_t publications, publish_skipped, withdrawals, replaced_sends,
                 replaced_nonzero, original_mode0_sends, transitions,
                 speed_publications, speed_overlay_sends, speed_overlay_nonzero,
                 original_nofix_sends;
    };
    explicit BetaController(BetaShared& shared)
        : shared_(shared),profile_(beta_profile()),state_(BETA_DISABLED),reason_("not_enabled"),
          counters_(),have_position_(false),position_mode_(-1),position_generation_(0),
          position_class_(adapter::POSITION_UNDECODED),withdrawn_class_(adapter::POSITION_UNDECODED),
          seen_storage_epoch_(0),seen_hold_set_(0),seen_hold_cleared_(0),last_summary_(0),
          last_bridge_(CORE_BRIDGE_NO_OUTPUT),last_accuracy_(0),last_valid_until_(0),
          last_frontier_(0),last_skip_("none"),payload_("none"),last_original_utc_(0),
          last_original_accuracy_(-1),last_speed_(-1),seen_anchor_seq_(0),anchor_rows_dropped_(0) {}
    BetaState state() const { return state_; }
    const char* reason() const { return reason_; }
    const Counters& counters() const { return counters_; }
    // Live means BETA may still publish in this boot.
    bool live() const { return state_!=BETA_DISABLED && state_!=BETA_FAULT; }

    // After the boot row is durable. blocked != 0 keeps BETA disabled and
    // journals why. Otherwise arms provenance, then requests Mode::BETA.
    template<class J> bool enable(J& j,uint64_t now,const char* blocked) {
        if(state_!=BETA_DISABLED || strcmp(reason_,"not_enabled")) return false;
        if(blocked) { transition(j,now,BETA_DISABLED,blocked);return false; }
        shared_.active.store(1,std::memory_order_release);
        if(!adapter::set_mode(adapter::BETA)) {
            shared_.active.store(0,std::memory_order_release);
            transition(j,now,BETA_DISABLED,"adapter_refused");return false;
        }
        transition(j,now,BETA_ARMED,"enabled");
        return true;
    }
    // Every popped POSITION observation, in queue order. The adapter has
    // already revoked the generation on every class change; this only
    // follows the class (BETA_DECISIONS 1).
    template<class J> void position(J& j,const adapter::Observation& o,uint64_t now) {
        if(!live())return;
        have_position_=true;position_mode_=o.original_mode;
        position_generation_=o.prediction_generation;
        const adapter::PositionClass cls=o.position_class;
        position_class_=cls;last_original_utc_=o.position.utc_seconds;
        // A withdrawal holds for the rest of the class episode it happened in.
        if(state_==BETA_WITHDRAWN && cls==withdrawn_class_)return;
        if(cls==adapter::POSITION_LOST) {
            if(state_==BETA_SPEED_ENGAGED) { withdraw();transition(j,now,BETA_GPS_LOST,"gps_lost"); }
            else if(state_==BETA_ARMED || state_==BETA_NO_FIX || state_==BETA_WITHDRAWN)
                transition(j,now,BETA_GPS_LOST,"gps_lost");
            return;
        }
        if(cls==adapter::POSITION_NO_FIX_STALE) {
            if(state_==BETA_ENGAGED) { withdraw();transition(j,now,BETA_NO_FIX,"no_fix"); }
            else if(state_==BETA_ARMED || state_==BETA_GPS_LOST || state_==BETA_WITHDRAWN)
                transition(j,now,BETA_NO_FIX,"no_fix");
            return;
        }
        // FIX, native DR, utc stall or undecodable: the adapter already passes
        // the original for this call; withdraw so no candidate survives.
        const char* reason=cls==adapter::POSITION_FIX?"gps_returned":
            cls==adapter::POSITION_NATIVE_DR?"native_dr":
            cls==adapter::POSITION_UTC_STALL?"utc_stall":"position_undecoded";
        if(state_==BETA_ENGAGED || state_==BETA_SPEED_ENGAGED) { withdraw();transition(j,now,BETA_ARMED,reason); }
        else if(state_==BETA_GPS_LOST || state_==BETA_NO_FIX || state_==BETA_WITHDRAWN)
            transition(j,now,BETA_ARMED,reason);
    }
    // Every popped SEND observation (diagnostic counters only).
    void send(const adapter::Observation& o) {
        if(o.kind!=adapter::Observation::SEND || o.type!=1 || !o.has_payload)return;
        last_original_accuracy_=o.original[16]?double(uint32_t(o.original[20])|(uint32_t(o.original[21])<<8)|
            (uint32_t(o.original[22])<<16)|(uint32_t(o.original[23])<<24))/1000.0:-1;
        if(o.choice==adapter::BETA_REPLACEMENT) {
            ++counters_.replaced_sends;if(o.result)++counters_.replaced_nonzero;
        } else if(o.choice==adapter::BETA_SPEED_OVERLAY) {
            ++counters_.speed_overlay_sends;if(o.result)++counters_.speed_overlay_nonzero;
        } else if(o.position_class==adapter::POSITION_NO_FIX_STALE)++counters_.original_nofix_sends;
        else if(o.original_mode==0)++counters_.original_mode0_sends;
    }
    // Permanent for this boot: capture.stop, disable-next-start.
    template<class J> void disable(J& j,uint64_t now,const char* reason) {
        if(!live())return;
        stop_adapter();transition(j,now,BETA_DISABLED,reason);summary(j,now,true);
    }
    // Sticky; adapter back to OBSERVE.
    template<class J> void fault(J& j,uint64_t now,const char* reason) {
        if(!live())return;
        stop_adapter();transition(j,now,BETA_FAULT,reason);summary(j,now,true);
    }
    // One worker tick. input_ready: MODEL input admitted (session+bus fences).
    // last_motion_ns: newest motion receipt admitted into the pipeline.
    template<class J> void tick(J& j,uint64_t now,const navigation::Pipeline& nav,
                                bool input_ready,uint64_t last_motion_ns,
                                uint64_t bus_epoch,const char* fault_reason) {
        if(!live())return;
        if(fault_reason) { fault(j,now,fault_reason);return; }
        const uint32_t source=bus_epoch && bus_epoch<=UINT32_MAX?uint32_t(bus_epoch):0;
        shared_.source_epoch.store(source,std::memory_order_release);
        journal_events(j,now);
        journal_anchors(j,nav);
        if(!live())return;
        if((state_==BETA_ENGAGED || state_==BETA_SPEED_ENGAGED) && adapter::beta_held())
            withdraw_episode(j,now,"send_result_hold");
        if(state_==BETA_GPS_LOST || state_==BETA_ENGAGED)
            publish(j,now,nav,input_ready,last_motion_ns,source);
        else if(state_==BETA_NO_FIX || state_==BETA_SPEED_ENGAGED)
            publish_speed(j,now,nav,input_ready,last_motion_ns,source);
        summary(j,now,false);
    }
private:
    BetaShared& shared_;
    BetaProfile profile_;
    BetaState state_;
    const char* reason_;
    Counters counters_;
    bool have_position_;
    int32_t position_mode_;
    uint32_t position_generation_;
    adapter::PositionClass position_class_,withdrawn_class_;
    uint32_t seen_storage_epoch_,seen_hold_set_,seen_hold_cleared_;
    uint64_t last_summary_;
    CoreBridgeResult last_bridge_;
    double last_accuracy_;
    uint64_t last_valid_until_,last_frontier_;
    const char* last_skip_;
    const char* payload_;          // "dr", "speed_only" or "none": what the candidate is
    uint64_t last_original_utc_;   // utc_s of the newest POSITION
    double last_original_accuracy_; // original LOCATION accuracy (m), -1 when absent
    double last_speed_;            // last published overlay speed (m/s), -1 none
    uint64_t seen_anchor_seq_,anchor_rows_dropped_;

    // BETA_DECISIONS 3.2: every anchor gate evaluation, including each
    // rejection reason, becomes one beta_anchor row (ring overrun is counted).
    template<class J> void journal_anchors(J& j,const navigation::Pipeline& nav) {
        const uint64_t latest=nav.beta_anchor_sequence();
        if(latest<seen_anchor_seq_)seen_anchor_seq_=0;
        for(uint64_t seq=seen_anchor_seq_+1;seq<=latest;++seq) {
            navigation::BetaAnchorRecord r;
            if(!nav.beta_anchor_record(seq,&r)) { ++anchor_rows_dropped_;continue; }
            char hdop[48],kmh[48],ratio[48],streak[48];
            finite_or_null(r.hdop,hdop);finite_or_null(r.kmh,kmh);
            finite_or_null(r.displacement_ratio,ratio);finite_or_null(r.streak_s,streak);
            char line[500];
            const int n=snprintf(line,sizeof line,
                "{\"kind\":\"beta_anchor\",\"mono_ns\":%llu,\"domain\":\"beta\",\"seq\":%llu,"
                "\"mode\":%d,\"utc_s\":%llu,\"gate\":\"%s\",\"hdop\":%s,\"kmh\":%s,"
                "\"displacement_ratio\":%s,\"streak_s\":%s,\"reverse_exit_seen\":%s,"
                "\"dropped\":%llu}",
                (unsigned long long)r.mono_ns,(unsigned long long)r.seq,r.mode,
                (unsigned long long)r.utc_s,navigation::beta_anchor_gate_name(r.gate),hdop,kmh,ratio,streak,
                nav.reverse_exit_seen()?"true":"false",(unsigned long long)anchor_rows_dropped_);
            if(n>0 && size_t(n)<sizeof line)j.line(line);else j.fail();
        }
        seen_anchor_seq_=latest;
    }
    static void finite_or_null(double v,char out[48]) {
        if(std::isfinite(v))snprintf(out,48,"%.9g",v);else strcpy(out,"null");
    }

    // Engaged -> WITHDRAWN for the rest of this class episode.
    template<class J> void withdraw_episode(J& j,uint64_t now,const char* reason) {
        withdraw();withdrawn_class_=position_class_;transition(j,now,BETA_WITHDRAWN,reason);
    }
    void stop_adapter() {
        shared_.active.store(0,std::memory_order_release);
        // set_mode revokes the generation even when it refuses nothing.
        adapter::set_mode(adapter::OBSERVE);
        withdraw();
    }
    // Revoke first (lock-free, immediately unselectable), then overwrite the
    // stored candidate with an empty one so no BETA value survives in memory.
    void withdraw() {
        adapter::invalidate();
        adapter::DrSnapshot empty=adapter::DrSnapshot();
        empty.prediction_generation=adapter::generation();
        adapter::publish_snapshot(empty);
        ++counters_.withdrawals;payload_="none";
    }
    template<class J> void journal_events(J& j,uint64_t now) {
        const uint32_t set=shared_.hold_set.load(std::memory_order_acquire);
        const uint32_t cleared=shared_.hold_cleared.load(std::memory_order_acquire);
        if(set!=seen_hold_set_) {
            seen_hold_set_=set;event(j,now,"hold_set",set);
            if(state_==BETA_ENGAGED || state_==BETA_SPEED_ENGAGED) withdraw_episode(j,now,"send_result_hold");
        }
        if(cleared!=seen_hold_cleared_) { seen_hold_cleared_=cleared;event(j,now,"hold_cleared",cleared); }
        const uint32_t storage=shared_.storage_epoch.load(std::memory_order_acquire);
        if(storage!=seen_storage_epoch_) {
            const uint32_t previous=seen_storage_epoch_;seen_storage_epoch_=storage;
            char line[300];
            const int n=snprintf(line,sizeof line,
                "{\"kind\":\"beta_session_storage\",\"mono_ns\":%llu,\"domain\":\"beta\","
                "\"session_epoch\":%u,\"previous\":%u,\"state\":\"%s\",\"generation\":%u}",
                (unsigned long long)now,storage,previous,beta_state_name(state_),adapter::generation());
            if(n>0 && size_t(n)<sizeof line)j.line(line);else j.fail();
            if(storage==UINT32_MAX) { fault(j,now,"session_epoch_exhausted");return; }
            // The send-time reader already revoked the generation; also drop
            // the stored values and keep this outage withdrawn (decision 8).
            if(state_==BETA_ENGAGED || state_==BETA_SPEED_ENGAGED)
                withdraw_episode(j,now,"session_storage_changed");
        }
    }
    template<class J> void event(J& j,uint64_t now,const char* what,uint32_t count) {
        char line[300];
        const int n=snprintf(line,sizeof line,
            "{\"kind\":\"beta_hold\",\"mono_ns\":%llu,\"domain\":\"beta\",\"event\":\"%s\","
            "\"count\":%u,\"held\":%s,\"state\":\"%s\"}",
            (unsigned long long)now,what,count,adapter::beta_held()?"true":"false",beta_state_name(state_));
        if(n>0 && size_t(n)<sizeof line)j.line(line);else j.fail();
    }
    static bool budget(CoreBridgeResult r,const BetaModelInput& in,const navigation::Pipeline& nav) {
        return r==CORE_BRIDGE_LIMIT || r==CORE_BRIDGE_BEARING ||
            in.snapshot.reason==MX5_DR_E_LIMIT || in.result==MX5_DR_E_LIMIT ||
            nav.beta_core_result()==MX5_DR_E_LIMIT;
    }
    template<class J> void publish(J& j,uint64_t now,const navigation::Pipeline& nav,
                                   bool input_ready,uint64_t last_motion_ns,uint32_t source) {
        const char* withdrawn=0;
        if(!input_ready)withdrawn="model_input_unavailable";
        else if(!last_motion_ns || now<last_motion_ns || now-last_motion_ns>BETA_SENSOR_SILENCE_NS)
            withdrawn="sensor_silence";
        adapter::DrSnapshot s=adapter::DrSnapshot();
        if(!withdrawn) {
            const BetaModelInput in=nav.model_publication(now);
            last_bridge_=map_model_publication(in,profile_,&s);
            // The MODEL sample-age guard (250 ms of the frontier) can fire
            // before the 300 ms receipt silence; both mean sensor silence.
            if(last_bridge_!=CORE_BRIDGE_OK)
                withdrawn=nav.status().result==navigation::PIPELINE_MISSING_SENSOR?"sensor_silence":
                    nav.beta_reverse_suspect()?"reverse_latch_suspect":
                    budget(last_bridge_,in,nav)?"budget_limit":
                    last_bridge_==CORE_BRIDGE_TIME?"lease_expired":"model_not_ready";
        }
        if(withdrawn) {
            last_skip_=withdrawn;
            if(state_==BETA_ENGAGED) withdraw_episode(j,now,withdrawn);
            return;
        }
        // Identity checks that only defer (no state change): the candidate
        // carries the CURRENT adapter generation only when this worker has
        // already seen a mode-0 POSITION of exactly that generation, so a
        // snapshot can never cross an unprocessed GPS return/loss.
        const uint32_t generation=adapter::generation();
        const uint32_t session=shared_.storage_epoch.load(std::memory_order_acquire);
        const char* skip=0;
        if(!source)skip="source_epoch_unavailable";
        else if(!session || session==UINT32_MAX)skip="session_storage_unobserved";
        else if(!have_position_ || position_mode_!=0 || position_class_!=adapter::POSITION_LOST ||
                position_generation_!=generation)
            skip="generation_unobserved";
        if(!skip) {
            s.prediction_generation=generation;s.source_epoch=source;s.session_epoch=session;
            if(!adapter::publish_snapshot(s))skip="publish_rejected";
        }
        if(skip) { last_skip_=skip;++counters_.publish_skipped;return; }
        ++counters_.publications;last_skip_="none";payload_="dr";
        last_accuracy_=s.accuracy_m;last_valid_until_=s.valid_until_mono_ns;
        last_frontier_=s.frontier_mono_ns;
        if(state_==BETA_GPS_LOST)transition(j,now,BETA_ENGAGED,"published");
    }
    // NO_FIX (BETA_DECISIONS 2): a speed-only candidate from the last wheel
    // SPEED event. No anchor, no core, no accuracy claim. A transient input
    // gap returns SPEED_ENGAGED to NO_FIX (the next fresh speed re-engages);
    // hold and storage changes withdraw the whole NO_FIX episode instead.
    template<class J> void publish_speed(J& j,uint64_t now,const navigation::Pipeline& nav,
                                         bool input_ready,uint64_t last_motion_ns,uint32_t source) {
        const char* gap=0;
        navigation::SpeedPublication sp=navigation::SpeedPublication();
        if(!input_ready)gap="model_input_unavailable";
        else if(!last_motion_ns || now<last_motion_ns || now-last_motion_ns>BETA_SENSOR_SILENCE_NS)
            gap="sensor_silence";
        else {
            sp=nav.speed_publication(now);
            if(!sp.ok)gap="speed_unavailable";
        }
        if(gap) {
            last_skip_=gap;
            if(state_==BETA_SPEED_ENGAGED) { withdraw();transition(j,now,BETA_NO_FIX,gap); }
            return;
        }
        const uint32_t generation=adapter::generation();
        const uint32_t session=shared_.storage_epoch.load(std::memory_order_acquire);
        const char* skip=0;
        adapter::DrSnapshot s=adapter::DrSnapshot();
        if(!source)skip="source_epoch_unavailable";
        else if(!session || session==UINT32_MAX)skip="session_storage_unobserved";
        else if(!have_position_ || position_class_!=adapter::POSITION_NO_FIX_STALE ||
                position_generation_!=generation)
            skip="generation_unobserved";
        if(!skip) {
            s.prediction_generation=generation;s.source_epoch=source;s.session_epoch=session;
            s.frontier_mono_ns=sp.measured_ns;
            s.valid_until_mono_ns=sp.measured_ns+profile_.lease_ns;
            s.speed_mps=sp.speed_mps;s.stopped=sp.stopped;
            s.ready=true;s.beta=true;s.speed_only=true;
            // No position, bearing or accuracy claim; qualified flags stay false.
            if(!adapter::publish_snapshot(s))skip="publish_rejected";
        }
        if(skip) { last_skip_=skip;++counters_.publish_skipped;return; }
        ++counters_.speed_publications;last_skip_="none";payload_="speed_only";
        last_speed_=s.speed_mps;last_valid_until_=s.valid_until_mono_ns;last_frontier_=s.frontier_mono_ns;
        if(state_==BETA_NO_FIX)transition(j,now,BETA_SPEED_ENGAGED,"speed_published");
    }
    static void number_or_null(double v,char out[48]) {
        if(std::isfinite(v) && v>=0)snprintf(out,48,"%.17g",v);else strcpy(out,"null");
    }
    template<class J> void transition(J& j,uint64_t now,BetaState to,const char* reason) {
        const BetaState from=state_;
        state_=to;reason_=reason;++counters_.transitions;
        char accuracy[48];
        if(std::isfinite(last_accuracy_))snprintf(accuracy,sizeof accuracy,"%.17g",last_accuracy_);
        else strcpy(accuracy,"null");
        char original_accuracy[48],speed[48];
        number_or_null(last_original_accuracy_,original_accuracy);number_or_null(last_speed_,speed);
        char line[800];
        const int n=snprintf(line,sizeof line,
            "{\"kind\":\"beta_state\",\"mono_ns\":%llu,\"domain\":\"beta\",\"assist_ready\":false,"
            "\"from\":\"%s\",\"to\":\"%s\",\"reason\":\"%s\",\"adapter_mode\":%u,"
            "\"generation\":%u,\"source_epoch\":%u,\"session_epoch\":%u,\"held\":%s,"
            "\"bridge\":\"%s\",\"accuracy_m\":%s,\"valid_until_ns\":%llu,"
            "\"position_class\":\"%s\",\"payload\":\"%s\",\"original_utc_s\":%llu,"
            "\"original_accuracy_m\":%s,\"speed_mps\":%s}",
            (unsigned long long)now,beta_state_name(from),beta_state_name(to),reason,
            unsigned(adapter::mode()),adapter::generation(),
            shared_.source_epoch.load(std::memory_order_acquire),
            shared_.storage_epoch.load(std::memory_order_acquire),
            adapter::beta_held()?"true":"false",core_bridge_result_name(last_bridge_),accuracy,
            (unsigned long long)last_valid_until_,adapter::position_class_name(position_class_),
            payload_,(unsigned long long)last_original_utc_,original_accuracy,speed);
        if(n>0 && size_t(n)<sizeof line)j.line(line);else j.fail();
    }
    // At most 1 Hz (forced rows bypass the limit once, e.g. at stop).
    template<class J> void summary(J& j,uint64_t now,bool force) {
        if(!force && last_summary_ && now>=last_summary_ && now-last_summary_<1000000000ULL)return;
        last_summary_=now?now:1;
        char accuracy[48];
        if(std::isfinite(last_accuracy_))snprintf(accuracy,sizeof accuracy,"%.17g",last_accuracy_);
        else strcpy(accuracy,"null");
        char original_accuracy[48],speed[48];
        number_or_null(last_original_accuracy_,original_accuracy);number_or_null(last_speed_,speed);
        char line[1200];
        const int n=snprintf(line,sizeof line,
            "{\"kind\":\"beta_summary\",\"mono_ns\":%llu,\"domain\":\"beta\",\"assist_ready\":false,"
            "\"state\":\"%s\",\"reason\":\"%s\",\"adapter_mode\":%u,\"held\":%s,"
            "\"publications\":%llu,\"publish_skipped\":%llu,\"last_skip\":\"%s\","
            "\"withdrawals\":%llu,\"replaced_sends\":%llu,\"replaced_nonzero\":%llu,"
            "\"original_mode0_sends\":%llu,\"transitions\":%llu,\"bridge\":\"%s\","
            "\"accuracy_m\":%s,\"frontier_ns\":%llu,\"valid_until_ns\":%llu,"
            "\"source_epoch\":%u,\"session_epoch\":%u,\"generation\":%u,"
            "\"position_class\":\"%s\",\"payload\":\"%s\",\"original_utc_s\":%llu,"
            "\"original_accuracy_m\":%s,\"speed_mps\":%s,\"speed_publications\":%llu,"
            "\"speed_overlay_sends\":%llu,\"speed_overlay_nonzero\":%llu,\"original_nofix_sends\":%llu}",
            (unsigned long long)now,beta_state_name(state_),reason_,unsigned(adapter::mode()),
            adapter::beta_held()?"true":"false",
            (unsigned long long)counters_.publications,(unsigned long long)counters_.publish_skipped,
            last_skip_,(unsigned long long)counters_.withdrawals,
            (unsigned long long)counters_.replaced_sends,(unsigned long long)counters_.replaced_nonzero,
            (unsigned long long)counters_.original_mode0_sends,(unsigned long long)counters_.transitions,
            core_bridge_result_name(last_bridge_),accuracy,(unsigned long long)last_frontier_,
            (unsigned long long)last_valid_until_,
            shared_.source_epoch.load(std::memory_order_acquire),
            shared_.storage_epoch.load(std::memory_order_acquire),adapter::generation(),
            adapter::position_class_name(position_class_),payload_,(unsigned long long)last_original_utc_,
            original_accuracy,speed,(unsigned long long)counters_.speed_publications,
            (unsigned long long)counters_.speed_overlay_sends,
            (unsigned long long)counters_.speed_overlay_nonzero,
            (unsigned long long)counters_.original_nofix_sends);
        if(n>0 && size_t(n)<sizeof line)j.line(line);else j.fail();
    }
};

} }
#endif
