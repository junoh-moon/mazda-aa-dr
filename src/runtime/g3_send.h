#ifndef MX5_RUNTIME_G3_SEND_H
#define MX5_RUNTIME_G3_SEND_H
// G3 first-connection bearing SEND (validation/G3_SEND_2026-10-13.md). BETA
// mode only. For the first moments of the first AA session after a restore
// the stock LOCATION bearing is 0 (placeholder) or GPS noise while the car
// stands (G3_FIRST_CONNECTION_HEADING_2026-10-11.md). While the G3 tracker
// (g3_heading.h) carries the genuinely restored heading (CARRIED with reason
// chain_ok) and the stock course is not yet trusted, a FIX LOCATION keeps
// every original byte except hasBearing (40) and bearing (44..47), which take
// the carried heading advanced by the G3 yaw integral
// (adapter::encode_bearing_overlay).
//  * One window per worker. After the session start (the first LOCATION
//    send) it opens only while the tracker is CARRIED/chain_ok (never
//    CARRIED_UNVERIFIED, same_boot, chain_unknown or a state raised by the
//    first-fix position check), the reverse latch is KNOWN forward, the wheel
//    stream is fresh and the send session is observed; it waits for those
//    while the car stands (at most G3_SEND_OPEN_WAIT_NS) and otherwise does
//    not open. It ends for good (never re-armed) when the stock course is
//    trusted or conflicts with the carried heading, the tracker leaves
//    CARRIED, a cap passes (G3_SEND_STATIONARY_CAP_NS from the open,
//    G3_SEND_MOTION_CAP_NS from the first stock fix at walking speed), the
//    send session, bus or cadence fence changes, the reverse latch leaves
//    forward, or on any fault (journal failure, writer lag, BETA not live,
//    adapter fault or hold, clock regression, a stale wheel stream, a yaw
//    chain break while moving, a failed overlaid send, MODEL tick abort).
//  * The worker publishes a bearing_only candidate only while BETA is ARMED
//    (no BETA candidate in the slot), the newest POSITION is class FIX and
//    its generation is still current; adapter::set_bearing_overlay() is the
//    OEM-side gate. A send that read the gate open just before it closed may
//    still be overlaid once (the call in progress). Rows: g3_send (start,
//    none, count, end with the deciding call) and g3_send_conflict;
//    diagnostic class, never j.fail().
//  * Garage carry (G3_GARAGE_*, validation/G3_GARAGE_EXIT_FIX_2026-10-17.md,
//    owner switch g3-garage-off): the window also opens for CARRIED with
//    reason chain_depart, also after the car moved since boot (a garage
//    start reaches its first FIX only at the exit), provided no yaw chain
//    break happened after the first moving wheel of the boot while the car
//    was not standing (G3_SEND_PARKED_BREAK_M test) since the first tick; it
//    waits (no time limit) while the latch is not usable (below), the wheel
//    stream is stale or the session is not observed.
//    Reverse latch (review C1): the latch is NOT VALID ("unknown") not only
//    when no 0x118 change arrived since boot but also after a message before
//    the session/bus admission was excluded and after EVERY pipeline reset
//    or latch clear. Usable for this window: a valid latch saying forward;
//    or a latch that is not valid while G3Motion::reverse_unsure is zero (no
//    0x118 change reached the runtime except excluded forward values, so
//    none was lost) AND the car has moved G3_SEND_UNKNOWN_LATCH_MIN_M since
//    boot (driving that far without any 0x118 change means driving forward,
//    unless the car reversed that far from power-on). Not usable: a valid
//    latch saying reverse, or a latch that is not valid after lost 0x118
//    information (reverse_unsure > 0). An open window ends ("latch") when
//    the latch says reverse or becomes not valid after lost information.
//    Residual: a car in reverse since before the product started (its
//    0x118 change before the product ran) that reverses more than 20 m.
// MODEL evidence only: no qualified claim, no Domain::QUALIFIED.
#include "adapter/adapter.h"
#include "runtime/beta_controller.h"
#include "runtime/g3_heading.h"
#include <cmath>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace mx5 { namespace runtime {

// ---- named constants (reasons in validation/G3_SEND_2026-10-13.md) ----------
// Caps. While the car stands the carried heading is the restored parked
// heading of the same car (wrong only if it was moved while off): up to
// 900 s from the open. Once a stock fix reports walking speed the stock
// course should become trustworthy within seconds (44-80 s after the session
// start in the measured outdoor starts were mostly standing time): 60 s from
// the first stock fix at >= G3_SEND_MIN_KMH.
static const uint64_t G3_SEND_STATIONARY_CAP_NS=900000000000ULL;
static const uint64_t G3_SEND_MOTION_CAP_NS=60000000000ULL;
// After the session start the window may wait this long (car standing) for
// a known forward latch, fresh wheels and an observed send session.
static const uint64_t G3_SEND_OPEN_WAIT_NS=30000000000ULL;
// Trust gate (NNG GOOD_H, NNG_G2_G3_LESSONS_2026-10-12.md): a stock fix
// counts when its speed is >= 3.6 km/h, the wheel speed >= 1 m/s and
// |v_gps/v_wheel - 1| < 0.1.
static const double G3_SEND_MIN_KMH=3.6;
static const double G3_SEND_MIN_WHEEL_MPS=1.0;
static const double G3_SEND_RATIO_MAX=0.1;
// Agreement with the carried heading: within max(10 deg, the honest
// propagated uncertainty, which is <= G3_UNC_MAX_DEG while CARRIED).
static const double G3_SEND_AGREE_MIN_DEG=10.0;
// Consecutive gated fixes: agreeing ones make the stock course trusted,
// disagreeing ones make the carried heading wrong (the stock course wins).
static const unsigned G3_SEND_TRUST_FIXES=4;
static const unsigned G3_SEND_CONFLICT_FIXES=8;
// Periodic g3_send count row while the window is open.
static const uint64_t G3_SEND_COUNT_ROW_NS=10000000000ULL;
// A yaw chain break (G3Motion::breaks: a pipeline reset or a lost yaw window)
// while the car stands (fresh wheels, no moving wheel at this and the
// previous tick, wheel distance unchanged within this) cannot hide a
// rotation and is tolerated; any other break ends the window.
static const double G3_SEND_PARKED_BREAK_M=0.01;
// Candidate lease (the BETA profile lease and Options.max_snapshot_age_ns).
static const uint64_t G3_SEND_LEASE_NS=500000000ULL;
// Garage carry (review C1): a latch that is not valid, without any lost 0x118
// information, counts as forward only after this much wheel distance since
// boot (a reverse out of a parking space is a few metres; a garage exit is
// hundreds), and g3_send "wait" rows per boot are bounded by WAIT_ROWS_MAX.
static const double G3_SEND_UNKNOWN_LATCH_MIN_M=20.0;
static const unsigned G3_SEND_WAIT_ROWS_MAX=8;

enum G3SendPhase { G3_SEND_OFF=0, G3_SEND_IDLE, G3_SEND_ACTIVE, G3_SEND_ENDED };
inline const char* g3_send_phase_name(G3SendPhase p) {
    static const char* const names[]={"OFF","IDLE","ACTIVE","ENDED"};
    return unsigned(p)<sizeof names/sizeof names[0]?names[p]:"INVALID";
}

// The worker's view at one MODEL tick.
struct G3SendView {
    uint64_t now;
    bool journal_ok;        // the journal has not failed
    bool journal_current;   // the journal lag guard holds the journal current
    bool beta_live;         // BetaController::live()
    bool beta_armed;        // BetaController state ARMED (no BETA candidate in the slot)
    bool adapter_ok;        // Mode::BETA, no sticky adapter fault, no send-result hold
    uint32_t source_epoch, session_epoch;   // BetaShared epochs (0: none)
    uint64_t cadence_fences;                // BetaController::cadence_fences()
};

// Worker-owned. J must provide line(const char*).
class G3Send {
public:
    G3Send() { clear(); }
    // Worker start. enabled: the G3 tracker is active (BETA, no g3-off marker).
    // garage: the garage carry rules (no g3-garage-off marker).
    void start(bool enabled,bool garage=false) {
        clear();phase_=enabled?G3_SEND_IDLE:G3_SEND_OFF;garage_=enabled && garage;
    }
    G3SendPhase phase() const { return phase_; }
    bool active() const { return phase_==G3_SEND_ACTIVE; }
    const char* end_reason() const { return end_reason_; }
    const char* end_why() const { return end_why_; }
    uint64_t overlaid() const { return sent_; }
    uint64_t rows_dropped() const { return dropped_; }
    double heading() const { return travel_; }

    // Every POSITION observation, after BetaController::position: the class,
    // generation and call of the newest POSITION, and the trust gate on FIX.
    // wheel_mps: the newest drained mean wheel speed (G3Motion::speed_mps);
    // now: the worker clock (end rows carry the worker time and the call).
    template<class J> void position(J& j,const adapter::Observation& o,double wheel_mps,uint64_t now) {
        if(o.kind!=adapter::Observation::POSITION || phase_==G3_SEND_OFF)return;
        have_position_=true;last_class_=o.position_class;last_generation_=o.prediction_generation;
        last_call_=o.call_sequence;
        if(!active() || o.position_class!=adapter::POSITION_FIX)return;
        const adapter::PositionInput& p=o.position;
        const double kmh=p.velocity_kmh,course=p.heading_deg;
        if(!motion_ns_ && std::isfinite(kmh) && kmh>=G3_SEND_MIN_KMH)motion_ns_=now?now:1;
        const bool gated=std::isfinite(kmh) && kmh>=G3_SEND_MIN_KMH && std::isfinite(wheel_mps) &&
            wheel_mps>=G3_SEND_MIN_WHEEL_MPS && fabs(kmh/3.6/wheel_mps-1.0)<G3_SEND_RATIO_MAX &&
            std::isfinite(course) && course>=0 && course<=360;
        if(!gated) { agree_=conflict_=0;return; }
        ++gated_;
        const double d=fabs(g3_wrap180(course-travel_));
        if(d<=tolerance()) {
            ++agree_;conflict_=0;
            if(agree_>=G3_SEND_TRUST_FIXES)end(j,now,"trusted","stock_course",true,course,kmh,d);
        } else {
            ++conflict_;agree_=0;
            if(conflict_>=G3_SEND_CONFLICT_FIXES) {
                row(j,",\"mono_ns\":%llu,\"call\":%lu,\"t_s\":%.1f,\"h\":%ld,\"unc\":%ld,\"tol\":%ld,"
                      "\"course\":[%ld,%ld],\"d\":%ld,\"fixes\":%u}",
                    "g3_send_conflict",(unsigned long long)now,(unsigned long)last_call_,session_s(now),
                    lround(travel_*10),lround(unc_*10),lround(tolerance()*10),lround(g3_wrap360(course)*10),
                    lround(kmh*10),lround(d*10),conflict_);
                end(j,now,"conflict","stock_course",true,course,kmh,d);
            }
        }
    }
    // Every SEND observation: counts inside the window; a failed overlaid
    // send ends it (the adapter also sets its hold).
    template<class J> void send(J& j,const adapter::Observation& o,uint64_t now) {
        if(!active() || o.kind!=adapter::Observation::SEND || o.type!=1 || !o.has_payload)return;
        if(o.choice==adapter::BETA_BEARING_OVERLAY) {
            ++sent_;
            if(o.result)end(j,now,"fault","send_result",false,NAN,NAN,NAN);
        } else if(o.position_class==adapter::POSITION_FIX)++stock_;
    }
    // MODEL tick, after G3Tracker::tick (the propagated heading is current).
    template<class J> void tick(J& j,const G3Tracker& g3,const G3Sample& s,const G3SendView& v) {
        if(phase_==G3_SEND_OFF)return;
        if(phase_==G3_SEND_ENDED) { withdraw(v);return; }
        if(phase_==G3_SEND_IDLE && !try_open(j,g3,s,v))return;
        // ACTIVE: fail closed first, then the other ends, then publish.
        const bool parked=s.wheels_fresh && !s.wheels_moving && last_parked_ && std::isfinite(s.motion.distance_m) &&
            std::isfinite(last_m_) && fabs(s.motion.distance_m-last_m_)<G3_SEND_PARKED_BREAK_M;
        if(s.motion.breaks!=breaks_ && parked) { breaks_=s.motion.breaks;++parked_breaks_; }
        const char* fault=!v.journal_ok?"journal_failed":!v.journal_current?"writer_lag":
            !v.beta_live?"beta_not_live":!v.adapter_ok?"adapter":
            v.now<last_now_ || v.now<session_ns_?"clock":!s.wheels_fresh?"wheel_stale":
            s.motion.breaks!=breaks_?"yaw_break":0;
        last_now_=v.now>last_now_?v.now:last_now_;
        last_parked_=s.wheels_fresh && !s.wheels_moving;last_m_=s.motion.distance_m;
        const char* reason=0;const char* why=0;
        if(fault) { reason="fault";why=fault; }
        else if(v.session_epoch!=session_epoch_ || v.source_epoch!=source_epoch_ || v.cadence_fences!=fences_) {
            reason="session";why=v.session_epoch!=session_epoch_?"send_session":
                v.source_epoch!=source_epoch_?"source":"cadence_fence";
        }
        else if(garage_?(s.reverse==1 || (s.reverse<0 && s.motion.reverse_unsure)):s.reverse!=0) {
            reason="latch";why=s.reverse==1?"reverse":"unknown";
        }
        else if(g3.state()==G3_CONFIRMED) { reason="trusted";why="m_course"; }
        else if(g3.state()!=G3_CARRIED) { reason="rejected";why=g3.reason(); }
        else if(v.now-open_ns_>=G3_SEND_STATIONARY_CAP_NS) { reason="cap";why="stationary_900s"; }
        else if(motion_ns_ && v.now>=motion_ns_ && v.now-motion_ns_>=G3_SEND_MOTION_CAP_NS) { reason="cap";why="motion_60s"; }
        if(reason) { end(j,v.now,reason,why,false,NAN,NAN,NAN);withdraw(v);return; }
        travel_=g3.carried_travel(s);unc_=g3.propagated_unc();
        if(!finite_heading()) { end(j,v.now,"fault","heading",false,NAN,NAN,NAN);withdraw(v);return; }
        if(v.now-last_count_>=G3_SEND_COUNT_ROW_NS) {
            last_count_=v.now;
            row(j,",\"mono_ns\":%llu,\"event\":\"count\",\"t_s\":%.1f,\"sent\":%llu,\"stock\":%llu,\"h\":%ld,"
                  "\"unc\":%ld,\"gated\":%u,\"agree\":%u,\"conflict\":%u,\"m_b\":%.1f}",
                "g3_send",(unsigned long long)v.now,session_s(v.now),(unsigned long long)sent_,
                (unsigned long long)stock_,lround(travel_*10),lround(unc_*10),gated_,agree_,conflict_,
                std::isfinite(s.motion.distance_m)?s.motion.distance_m:0.0);
        }
        publish(v);
    }
    // The worker cannot run the MODEL tick any more (journal failure, model
    // disabled, audit fault): end at once.
    template<class J> void fail(J& j,uint64_t now,const char* why) {
        if(active())end(j,now,"fault",why,false,NAN,NAN,NAN);
    }

private:
    G3SendPhase phase_;
    const char* end_reason_;
    const char* end_why_;
    uint64_t session_ns_,open_ns_,motion_ns_,last_now_,last_count_,sent_,stock_,dropped_,fences_;
    double travel_,unc_,session_m_;
    uint32_t breaks_,session_epoch_,source_epoch_,last_generation_,last_call_;
    unsigned gated_,agree_,conflict_,parked_breaks_;
    bool have_position_,published_,withdraw_pending_,last_parked_;
    double last_m_;
    // Garage carry: switch; IDLE break watch (breaks seen at the previous
    // tick, a break while not standing, the previous tick stood).
    bool garage_,idle_seen_,idle_moving_break_,idle_parked_;
    uint32_t idle_breaks_;
    double idle_m_;
    // Garage carry: the last journaled wait reason and the wait rows so far.
    const char* wait_why_;
    unsigned wait_rows_;
    adapter::PositionClass last_class_;

    void clear() {
        phase_=G3_SEND_OFF;end_reason_=end_why_="none";
        session_ns_=open_ns_=motion_ns_=last_now_=last_count_=sent_=stock_=dropped_=fences_=0;
        travel_=unc_=session_m_=0;breaks_=session_epoch_=source_epoch_=last_generation_=last_call_=0;
        gated_=agree_=conflict_=parked_breaks_=0;
        have_position_=published_=withdraw_pending_=last_parked_=false;last_m_=0;
        garage_=idle_seen_=idle_moving_break_=idle_parked_=false;idle_breaks_=0;idle_m_=0;
        wait_why_="none";wait_rows_=0;
        last_class_=adapter::POSITION_UNDECODED;
    }
    double tolerance() const { return unc_>G3_SEND_AGREE_MIN_DEG?unc_:G3_SEND_AGREE_MIN_DEG; }
    bool finite_heading() const { return std::isfinite(travel_) && std::isfinite(unc_) && unc_>=0; }
    double session_s(uint64_t now) const { return now>=session_ns_?(now-session_ns_)/1e9:0.0; }
    template<class J> void none(J& j,uint64_t now,const char* state,const char* reason) {
        phase_=G3_SEND_ENDED;end_reason_="none";end_why_=reason;
        row(j,",\"mono_ns\":%llu,\"event\":\"none\",\"g3\":\"%s\",\"reason\":\"%s\",\"t_s\":%.1f}","g3_send",
            (unsigned long long)now,state,reason,session_s(now));
    }
    // IDLE: after the session start, open only for the genuinely restored
    // heading, a known forward latch, fresh wheels and an observed session;
    // wait for the last three while the car stands, at most OPEN_WAIT.
    template<class J> bool try_open(J& j,const G3Tracker& g3,const G3Sample& s,const G3SendView& v) {
        if(garage_) {
            // Same standing test as the ACTIVE break tolerance.
            const bool standing=s.wheels_fresh && !s.wheels_moving && idle_parked_ && std::isfinite(s.motion.distance_m) &&
                fabs(s.motion.distance_m-idle_m_)<G3_SEND_PARKED_BREAK_M;
            // Before the first moving wheel of the boot a break hides no rotation
            // (unobserved motion before it is the tracker's moving_at_boot check).
            if(idle_seen_ && s.motion.breaks!=idle_breaks_ && !standing && s.motion.first_move_ns)
                idle_moving_break_=true;
            idle_seen_=true;idle_breaks_=s.motion.breaks;
            idle_parked_=s.wheels_fresh && !s.wheels_moving;idle_m_=s.motion.distance_m;
        }
        if(!session_ns_) {
            if(!g3.session_ns())return false;
            session_ns_=g3.session_ns();last_now_=v.now;
            session_m_=std::isfinite(s.motion.distance_m)?s.motion.distance_m:0;
        }
        const bool depart=garage_ && !strcmp(g3.reason(),"chain_depart");
        if(g3.state()!=G3_CARRIED || !g3.have_carried() || (strcmp(g3.reason(),"chain_ok") && !depart)) {
            none(j,v.now,g3_state_name(g3.state()),g3.reason());return false;
        }
        const bool moved=s.wheels_moving || (std::isfinite(s.motion.distance_m) &&
            s.motion.distance_m-session_m_>=G3_SEND_PARKED_BREAK_M);
        const bool session=v.session_epoch && v.session_epoch!=UINT32_MAX && v.source_epoch;
        if(garage_) {
            if(idle_moving_break_) { none(j,v.now,g3_state_name(g3.state()),"yaw_break");return false; }
            // Review C1: the latch rule above; wait (no time limit) otherwise.
            const char* why=s.reverse==1?"reverse":
                s.reverse<0 && s.motion.reverse_unsure?"latch_unsure":
                s.reverse<0 && !(std::isfinite(s.motion.distance_m) &&
                                 s.motion.distance_m>=G3_SEND_UNKNOWN_LATCH_MIN_M)?"latch_unknown":
                !s.wheels_fresh?"wheel_stale":!session?"session":0;
            if(why) { wait(j,v.now,s,why);return false; }
        } else if(s.reverse!=0 || !s.wheels_fresh || !session) {
            if(moved || v.now<session_ns_ || v.now-session_ns_>=G3_SEND_OPEN_WAIT_NS)
                none(j,v.now,g3_state_name(g3.state()),s.reverse!=0?"latch":!s.wheels_fresh?"wheel_stale":"session");
            return false;
        }
        travel_=g3.carried_travel(s);unc_=g3.propagated_unc();breaks_=s.motion.breaks;
        if(!finite_heading()) { none(j,v.now,g3_state_name(g3.state()),"heading");return false; }
        last_parked_=!s.wheels_moving;last_m_=s.motion.distance_m;last_now_=v.now>last_now_?v.now:last_now_;
        session_epoch_=v.session_epoch;source_epoch_=v.source_epoch;fences_=v.cadence_fences;
        phase_=G3_SEND_ACTIVE;open_ns_=last_count_=v.now;
        char age[32],gar[64];
        if(g3.age_known())snprintf(age,sizeof age,"%llu",(unsigned long long)g3.age_s());else strcpy(age,"null");
        if(garage_)snprintf(gar,sizeof gar,",\"garage\":1,\"moved\":%d,\"rlost\":%lu",moved?1:0,
                            (unsigned long)s.motion.reverse_unsure);
        else gar[0]=0;
        row(j,",\"mono_ns\":%llu,\"event\":\"start\",\"g3\":\"%s\",\"t_s\":%.1f,\"h\":%ld,\"carried\":[%ld,%ld],"
              "\"unc\":%ld,\"tol\":%ld,\"age_s\":%s,\"rev\":%d,\"m_b\":%.1f,\"session\":%lu%s}",
            "g3_send",(unsigned long long)v.now,g3_state_name(g3.state()),session_s(v.now),lround(travel_*10),
            lround(g3.carried_deg()*10),lround(g3.carried_unc()*10),lround(unc_*10),lround(tolerance()*10),
            age,s.reverse,std::isfinite(s.motion.distance_m)?s.motion.distance_m:0.0,(unsigned long)session_epoch_,gar);
        if(garage_)wait_why_="open";
        adapter::set_bearing_overlay(true);
        return true;
    }
    // Garage carry: one g3_send "wait" row when the reason the window waits
    // changes (bounded per boot): reverse, latch_unsure, latch_unknown,
    // wheel_stale, session. Diagnostic only.
    template<class J> void wait(J& j,uint64_t now,const G3Sample& s,const char* why) {
        if(!strcmp(why,wait_why_) || wait_rows_>=G3_SEND_WAIT_ROWS_MAX)return;
        wait_why_=why;++wait_rows_;
        row(j,",\"mono_ns\":%llu,\"event\":\"wait\",\"reason\":\"%s\",\"t_s\":%.1f,\"rev\":%d,\"rlost\":%lu,\"m_b\":%.1f}",
            "g3_send",(unsigned long long)now,why,session_s(now),s.reverse,(unsigned long)s.motion.reverse_unsure,
            std::isfinite(s.motion.distance_m)?s.motion.distance_m:0.0);
    }
    // by_fix: decided while processing the POSITION of call last_call_ (the
    // send of that call was decided before and may be overlaid); otherwise a
    // tick decision (the call in progress, at most last_call_ + 1, may be).
    template<class J> void end(J& j,uint64_t now,const char* reason,const char* why,bool by_fix,double course,
                               double kmh,double d) {
        if(!active())return;
        adapter::set_bearing_overlay(false);
        phase_=G3_SEND_ENDED;end_reason_=reason;end_why_=why;withdraw_pending_=published_;
        char crs[64];
        if(std::isfinite(course) && std::isfinite(kmh) && std::isfinite(d))
            snprintf(crs,sizeof crs,",\"course\":[%ld,%ld],\"d\":%ld",lround(g3_wrap360(course)*10),lround(kmh*10),
                     lround(d*10));
        else strcpy(crs,",\"course\":null,\"d\":null");
        row(j,",\"mono_ns\":%llu,\"event\":\"end\",\"reason\":\"%s\",\"why\":\"%s\",\"by\":\"%s\",\"call\":%lu,"
              "\"t_s\":%.1f,\"sent\":%llu,\"stock\":%llu,\"h\":%ld,\"unc\":%ld,\"gated\":%u,\"pbreaks\":%u%s}",
            "g3_send",(unsigned long long)now,reason,why,by_fix?"fix":"tick",(unsigned long)last_call_,
            session_s(now),(unsigned long long)sent_,(unsigned long long)stock_,lround(travel_*10),lround(unc_*10),
            gated_,parked_breaks_,crs);
    }
    // Hygiene after the end: the OEM gate is already closed; while BETA is
    // ARMED the slot still holds this window's candidate, so overwrite it
    // with an empty one (no generation change). Outside ARMED the slot
    // belongs to BETA (the class change that left ARMED revoked ours).
    void withdraw(const G3SendView& v) {
        if(!withdraw_pending_)return;
        withdraw_pending_=false;
        if(!v.beta_armed)return;
        adapter::DrSnapshot empty=adapter::DrSnapshot();
        empty.prediction_generation=adapter::generation();
        adapter::publish_snapshot(empty);
    }
    void publish(const G3SendView& v) {
        if(!v.beta_armed || !have_position_ || last_class_!=adapter::POSITION_FIX)return;
        const uint32_t generation=adapter::generation();
        // The worker must have seen a FIX POSITION of this very generation.
        if(last_generation_!=generation || v.session_epoch!=session_epoch_ || v.source_epoch!=source_epoch_)return;
        adapter::DrSnapshot c=adapter::DrSnapshot();
        c.prediction_generation=generation;c.source_epoch=source_epoch_;c.session_epoch=session_epoch_;
        c.frontier_mono_ns=v.now;c.valid_until_mono_ns=v.now+G3_SEND_LEASE_NS;
        c.travel_bearing_deg=g3_wrap360(travel_);
        // No position, speed or accuracy claim; qualified flags stay false.
        c.ready=true;c.beta=true;c.bearing_only=true;
        if(adapter::publish_snapshot(c))published_=true;
    }
    template<class J> void row(J& j,const char* fmt,const char* kind,...) __attribute__((format(printf,3,5)));
};
template<class J> void G3Send::row(J& j,const char* fmt,const char* kind,...) {
    char line[512];
    const int head=snprintf(line,sizeof line,"{\"kind\":\"%s\"",kind);
    va_list ap;
    va_start(ap,kind);
    const int n=head>0 && size_t(head)<sizeof line?vsnprintf(line+head,sizeof line-size_t(head),fmt,ap):-1;
    va_end(ap);
    if(n<=0 || size_t(head)+size_t(n)>=sizeof line) { ++dropped_;return; }
    j.line(line);
}

} }
#endif
