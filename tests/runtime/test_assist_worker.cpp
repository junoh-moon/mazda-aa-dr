// Authored qualification and normalized input. These tests grant no live
// sensor, request-provider, receiver, or vehicle qualification.
#include "runtime/assist_worker.h"
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace A=mx5::adapter;
namespace N=mx5::navigation;
namespace R=mx5::runtime;
static unsigned checks, sends;
#define CHECK(x) do { ++checks; if(!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::exit(1); } } while(0)
static uint64_t now_ns=1000000000ULL;
static uint32_t source_epoch=11,session_epoch=12;
static A::Observation last_position, last_send;
static uint8_t original[48], forwarded[48];
static uint64_t clock_fn(void*) { return now_ns; }
static bool provenance(void*,const A::PositionContext&,A::Provenance* p,void*) {
    p->source_epoch=source_epoch;p->session_epoch=session_epoch;
    p->exact_request=p->verified_lds=p->legacy_receiver=true;return true;
}
static void observe(const A::Observation* o,void*) {
    if(o->kind==A::Observation::POSITION)last_position=*o;else last_send=*o;
    errno=E2BIG;
}
static int32_t endpoint(void*,A::VehicleData* data) {
    CHECK(errno==EDOM&&data->length==48);std::memcpy(forwarded,data->payload,48);
    ++sends;errno=ERANGE;return -731;
}
static void put32(uint8_t* p,uint32_t v) { for(unsigned n=0;n<4;++n)p[n]=uint8_t(v>>(8*n)); }
static void put64(uint8_t* p,uint64_t v) { for(unsigned n=0;n<8;++n)p[n]=uint8_t(v>>(8*n)); }
static A::Observation callback(int mode) {
    uint8_t raw[72]={};put32(raw,uint32_t(mode));put64(raw+8,1700000000ULL);
    const double latitude=37,longitude=127;
    std::memcpy(raw+16,&latitude,8);std::memcpy(raw+24,&longitude,8);
    A::VehicleData data={1,original,48};const unsigned before=sends;errno=EDOM;
    A::position_enter(0,raw);CHECK(errno==EDOM);
    CHECK(A::send_vehicle_data(0,&data)==-731&&errno==ERANGE);
    A::position_leave();CHECK(errno==ERANGE&&sends==before+1);
    for(unsigned n=0;n<48;++n)CHECK(original[n]==uint8_t(n+1));
    return last_position;
}
static mx5_dr_context context() { return mx5_dr_context{source_epoch,session_epoch,A::generation()}; }
struct Source {
    R::AssistInput queue[512];size_t count,index;
    bool qualified,fault;unsigned reads,polls;
    unsigned fail_on_read,invalidate_on_read;
    uint64_t watermark,clock_advance_on_read,clock_advance_on_pop;
    double error_max_m;
    Source():count(0),index(0),qualified(true),fault(false),reads(0),polls(0),
        fail_on_read(0),invalidate_on_read(0),watermark(0),clock_advance_on_read(0),clock_advance_on_pop(0),
        error_max_m(100) {}
    void add(const R::AssistInput& v) { CHECK(count<512);queue[count++]=v; }
    static R::AssistPoll pop(void* user,R::AssistInput* out) {
        Source& s=*static_cast<Source*>(user);++s.polls;
        if(s.fault)return R::ASSIST_FAULT;
        if(s.index==s.count)return R::ASSIST_EMPTY;
        if(s.clock_advance_on_pop) {
            now_ns+=s.clock_advance_on_pop;s.watermark=now_ns;s.clock_advance_on_pop=0;
        }
        *out=s.queue[s.index++];return R::ASSIST_INPUT;
    }
    static bool readiness(void* user,uint64_t time,R::AssistReadiness* out) {
        Source& s=*static_cast<Source*>(user);++s.reads;
        if(s.invalidate_on_read==s.reads)A::invalidate();
        *out=R::AssistReadiness();R::CoreBridgeQualification& q=out->qualification;
        q.expected_context=context();q.now_mono_ns=q.limits_verified_until_mono_ns=time;
        q.max_snapshot_age_ns=150000000;q.duration_max_s=60;q.distance_max_m=1500;
        q.error_max_m=s.error_max_m;
        q.profile_verified=q.input_quality_verified=s.qualified&&s.fail_on_read!=s.reads;
        out->watermark_ns=s.watermark;out->requested_until_ns=time+150000000;
        if(s.clock_advance_on_read&&s.reads%2)now_ns+=s.clock_advance_on_read;
        return true;
    }
    R::AssistSource api() { return R::AssistSource{pop,readiness,this}; }
};
static R::AssistInput input(R::AssistInputKind kind) {
    R::AssistInput i=R::AssistInput();i.kind=kind;i.context=context();return i;
}
static mx5_dr_evidence evidence(unsigned id,uint64_t time,uint64_t received,unsigned seq=1) {
    mx5_dr_evidence e=mx5_dr_evidence();e.source_id=id;e.source_epoch=1;
    e.producer_seq=seq;e.measured_ns=time;e.received_ns=received;
    e.lease_until_ns=time+250000000;e.quality=MX5_DR_VALID;e.freshness=MX5_DR_PRODUCER_TIME;
    return e;
}
static void motion(Source& s,uint64_t start,uint64_t end,unsigned sequence=1) {
    R::AssistInput i=input(R::ASSIST_SPEED);i.evidence=evidence(1,start,end,sequence);i.value=10;s.add(i);
    i=input(R::ASSIST_REVERSE);i.evidence=evidence(3,start,end,sequence);s.add(i);
    i=input(R::ASSIST_YAW);i.evidence=evidence(2,end,end,sequence);i.raw_yaw=2047;i.yaw_count=1;
    i.window_start_ns=start;i.window_end_ns=end;s.add(i);
}
static void startup(Source& s,bool begin=true) {
    now_ns+=1000000000ULL;const uint64_t start=now_ns;
    const A::Observation gps=callback(1);
    R::AssistInput i=input(R::ASSIST_BEGIN);i.received_ns=start;if(begin)s.add(i);
    i=input(R::ASSIST_POSITION);i.observation=gps;s.add(i);
    i=input(R::ASSIST_ANCHOR);i.received_ns=start;i.anchor.context=context();
    i.position_call_sequence=gps.call_sequence;
    i.anchor.anchor_id=gps.call_sequence;i.anchor.position_seq=uint64_t(gps.call_sequence)*4;
    i.anchor.measured_ns=start;i.anchor.utc_ns=1700000000000000000ULL;
    i.anchor.latitude_deg=37;i.anchor.longitude_deg=127;
    i.anchor.position_error_m=1;i.anchor.heading_error_rad=0.01;
    i.anchor.validated=i.anchor.heading_valid=i.anchor.calibration_verified=1;
    i.anchor.quality=MX5_DR_VALID;s.add(i);
    now_ns=start+10000000;i=input(R::ASSIST_POSITION);i.observation=callback(0);i.context=context();s.add(i);
    motion(s,start,start+100000000);now_ns=s.watermark=start+100000000;
}
static void expect_ready(R::AssistWorker& w) {
    w.tick(clock_fn,0);CHECK(w.status().state==R::ASSIST_PUBLISHED);
    CHECK(w.status().last_publication.ready&&w.status().last_publication.prediction_generation==A::generation());
    CHECK(w.status().last_publication.frontier_mono_ns==now_ns);
    CHECK(w.status().last_publication.valid_until_mono_ns==now_ns+150000000);
    CHECK(w.status().last_publication.source_epoch==source_epoch&&w.status().last_publication.session_epoch==session_epoch);
    CHECK(w.status().last_publication.latitude_deg>37.0000089&&w.status().last_publication.latitude_deg<37.0000091);
    callback(0);CHECK(last_send.choice==A::DR_REPLACEMENT);
    uint8_t expected[48];CHECK(A::encode_location(w.status().last_publication,expected));
    CHECK(!std::memcmp(expected,forwarded,48));
}
static void expect_withdrawn(uint64_t old_deadline) {
    CHECK(now_ns<old_deadline);callback(0);
    CHECK(last_send.choice==A::ORIGINAL&&last_send.reason==A::NOT_READY);
    CHECK(!std::memcmp(original,forwarded,48));
}
static void publication_and_stop() {
    Source s;R::AssistWorker w(mx5_dr_default_config(),s.api());startup(s);expect_ready(w);
    const A::DrSnapshot original_snapshot=w.status().last_publication;
    now_ns+=10000000;w.tick(clock_fn,0);
    CHECK(w.status().state==R::ASSIST_PUBLISHED);
    CHECK(w.status().last_publication.frontier_mono_ns==original_snapshot.frontier_mono_ns);
    CHECK(w.status().last_publication.derived_utc_ns==original_snapshot.derived_utc_ns);
    CHECK(w.status().last_publication.valid_until_mono_ns==original_snapshot.valid_until_mono_ns);
    const uint32_t generation=A::generation();w.stop();CHECK(A::generation()==generation+1);
    CHECK(w.status().state==R::ASSIST_STOPPED);expect_withdrawn(original_snapshot.valid_until_mono_ns);
    const unsigned polls=s.polls;w.tick(clock_fn,0);CHECK(s.polls==polls);
}
static void loss_withdraws_and_requires_new_begin(bool poll_fault,bool after_calculation) {
    Source s;R::AssistWorker w(mx5_dr_default_config(),s.api());startup(s);expect_ready(w);
    const uint64_t until=w.status().last_publication.valid_until_mono_ns;
    now_ns+=10000000;
    if(poll_fault)s.fault=true;
    else if(after_calculation)s.fail_on_read=s.reads+2;
    else s.qualified=false;
    w.tick(clock_fn,0);expect_withdrawn(until);
    s.fault=false;s.qualified=true;s.fail_on_read=0;w.tick(clock_fn,0);
    CHECK(w.status().state==R::ASSIST_WAITING_BEGIN);expect_withdrawn(until);
    // Previously accepted queue bytes, including BEGIN, cannot revive a seed
    // which predates the observed loss, even while its sensor leases are live.
    s.index=0;w.tick(clock_fn,0);expect_withdrawn(until);
    CHECK(w.status().state!=R::ASSIST_PUBLISHED);
    s.index=s.count;startup(s);expect_ready(w);w.stop();
}
static void generation_changes_are_not_retagged() {
    Source s;R::AssistWorker w(mx5_dr_default_config(),s.api());startup(s);expect_ready(w);
    const uint64_t until=w.status().last_publication.valid_until_mono_ns;
    const uint32_t generation=A::generation();now_ns+=10000000;
    s.invalidate_on_read=s.reads+2;w.tick(clock_fn,0);
    CHECK(A::generation()==generation+1);CHECK(w.status().state==R::ASSIST_CONTEXT_CHANGED);
    CHECK(w.status().published==1);expect_withdrawn(until);w.stop();
}
static void healthy_reacquisition_keeps_the_same_worker() {
    Source s;R::AssistWorker w(mx5_dr_default_config(),s.api());startup(s);expect_ready(w);
    CHECK(w.status().begins==1);
    // New captured GPS/anchor/GAP advances the same calculator. Readiness
    // recovery is not simulated by reconstructing the worker or BEGIN.
    startup(s,false);expect_ready(w);CHECK(w.status().begins==1);
    CHECK(w.status().published==2);w.stop();
}
static void delayed_anchor_after_gps_return_keeps_the_worker() {
    Source s;R::AssistWorker w(mx5_dr_default_config(),s.api());startup(s);expect_ready(w);
    now_ns+=10000000ULL;const A::Observation gps=callback(1);
    R::AssistInput position=input(R::ASSIST_POSITION);position.observation=gps;s.add(position);
    // Complete-through cannot cross the GPS measurement until its matching
    // verified anchor arrives, even though POSITION is already owned.
    s.watermark=gps.mono_ns-1;
    const uint32_t generation=A::generation();w.tick(clock_fn,0);
    CHECK(w.status().state==R::ASSIST_CONTEXT_CHANGED);
    CHECK(A::generation()==generation);
    R::AssistInput anchor=input(R::ASSIST_ANCHOR);anchor.received_ns=gps.mono_ns;
    anchor.position_call_sequence=gps.call_sequence;
    anchor.anchor.context=anchor.context;anchor.anchor.anchor_id=gps.call_sequence;
    anchor.anchor.position_seq=uint64_t(gps.call_sequence)*4;
    anchor.anchor.measured_ns=gps.mono_ns;
    anchor.anchor.utc_ns=1700000000000000000ULL;
    anchor.anchor.latitude_deg=37;anchor.anchor.longitude_deg=127;
    anchor.anchor.position_error_m=1;anchor.anchor.heading_error_rad=.01;
    anchor.anchor.validated=anchor.anchor.heading_valid=anchor.anchor.calibration_verified=1;
    anchor.anchor.quality=MX5_DR_VALID;s.add(anchor);
    now_ns+=10000000ULL;position=input(R::ASSIST_POSITION);
    position.observation=callback(0);position.context=context();s.add(position);
    motion(s,gps.mono_ns,gps.mono_ns+100000000ULL,2);
    now_ns=s.watermark=gps.mono_ns+100000000ULL;
    expect_ready(w);CHECK(w.status().begins==1);w.stop();
}
static void unpaired_gps_fix_cannot_reactivate_an_old_anchor() {
    for(unsigned quality_change=0;quality_change<2;++quality_change) {
        Source s;R::AssistWorker w(mx5_dr_default_config(),s.api());startup(s);expect_ready(w);
        const uint64_t old_deadline=w.status().last_publication.valid_until_mono_ns;
        now_ns+=10000000ULL;const A::Observation paired=callback(1);
        R::AssistInput anchor=input(R::ASSIST_ANCHOR);
        anchor.received_ns=paired.mono_ns;anchor.position_call_sequence=paired.call_sequence;
        anchor.anchor.context=anchor.context;anchor.anchor.anchor_id=paired.call_sequence;
        anchor.anchor.position_seq=uint64_t(paired.call_sequence)*4;
        anchor.anchor.measured_ns=paired.mono_ns;
        anchor.anchor.utc_ns=1700000000000000000ULL;
        anchor.anchor.latitude_deg=37;anchor.anchor.longitude_deg=127;
        anchor.anchor.position_error_m=1;anchor.anchor.heading_error_rad=.01;
        anchor.anchor.validated=anchor.anchor.heading_valid=anchor.anchor.calibration_verified=1;
        anchor.anchor.quality=MX5_DR_VALID;s.add(anchor);
        R::AssistInput i=input(R::ASSIST_POSITION);i.observation=paired;s.add(i);
        now_ns+=10000000ULL;const A::Observation unpaired=callback(quality_change?2:1);
        i=input(R::ASSIST_POSITION);i.observation=unpaired;s.add(i); // no matching anchor
        now_ns+=10000000ULL;const A::Observation gap=callback(0);
        i=input(R::ASSIST_POSITION);i.observation=gap;s.add(i);
        motion(s,paired.mono_ns,paired.mono_ns+100000000ULL,2);
        now_ns=s.watermark=paired.mono_ns+100000000ULL;
        w.tick(clock_fn,0);
        if(w.status().state!=R::ASSIST_WAITING_INPUT)
            std::fprintf(stderr,"unpaired worker: state=%u result=%u generation=%u published=%llu inputs=%llu index=%zu count=%zu\n",
                unsigned(w.status().state),unsigned(w.status().pipeline_result),A::generation(),
                (unsigned long long)w.status().published,
                (unsigned long long)w.status().inputs,s.index,s.count);
        CHECK(w.status().state==R::ASSIST_WAITING_INPUT);
        CHECK(w.status().pipeline_result==N::PIPELINE_NO_ANCHOR);
        CHECK(w.status().unpaired_positions==1);
        CHECK(w.status().published==1);
        expect_withdrawn(old_deadline);w.stop();
    }
}
static void stale_position_batching_keeps_verified_recovery_available() {
    for(unsigned scenario=0;scenario<4;++scenario) {
        const bool delayed=scenario!=0;
        Source s;R::AssistWorker w(mx5_dr_default_config(),s.api());startup(s);expect_ready(w);
        const uint64_t old_deadline=w.status().last_publication.valid_until_mono_ns;
        now_ns+=10000000ULL;const A::Observation paired=callback(1);
        R::AssistInput anchor=input(R::ASSIST_ANCHOR);
        anchor.received_ns=paired.mono_ns;anchor.position_call_sequence=paired.call_sequence;
        anchor.anchor.context=anchor.context;anchor.anchor.anchor_id=paired.call_sequence;
        anchor.anchor.position_seq=uint64_t(paired.call_sequence)*4;
        anchor.anchor.measured_ns=paired.mono_ns;
        anchor.anchor.utc_ns=1700000000000000000ULL;
        anchor.anchor.latitude_deg=37;anchor.anchor.longitude_deg=127;
        anchor.anchor.position_error_m=1;anchor.anchor.heading_error_rad=.01;
        anchor.anchor.validated=anchor.anchor.heading_valid=anchor.anchor.calibration_verified=1;
        anchor.anchor.quality=MX5_DR_VALID;s.add(anchor);
        R::AssistInput position=input(R::ASSIST_POSITION);position.observation=paired;s.add(position);
        now_ns+=10000000ULL;const A::Observation unpaired=callback(1);
        R::AssistInput first=input(R::ASSIST_POSITION);first.observation=unpaired;
        now_ns+=10000000ULL;const A::Observation stale=callback(1);
        R::AssistInput second=input(R::ASSIST_POSITION);second.observation=stale;
        CHECK(stale.prediction_generation==unpaired.prediction_generation&&
              paired.prediction_generation==unpaired.prediction_generation);
        R::AssistInput third=R::AssistInput();
        if(scenario==3) {
            now_ns+=10000000ULL;const A::Observation latest=callback(1);
            third=input(R::ASSIST_POSITION);third.observation=latest;
            CHECK(latest.prediction_generation==stale.prediction_generation);
        }
        s.add(first);if(!delayed)s.add(second);
        s.watermark=now_ns;w.tick(clock_fn,0);
        CHECK(w.status().state==R::ASSIST_WAITING_INPUT);
        CHECK(w.status().unpaired_positions==1&&w.status().begins==1);
        const uint32_t retired_generation=A::generation();
        if(scenario==3)s.add(third); // Distinct callbacks may arrive in reverse order.
        if(delayed)s.add(second);
        if(scenario==2)s.add(second); // Repeated discarded callback is idempotent.
        w.tick(clock_fn,0);
        CHECK(w.status().state==R::ASSIST_WAITING_INPUT);
        CHECK(w.status().begins==1&&A::generation()==retired_generation);
        const unsigned ignored=scenario==0?0:scenario==1?1:2;
        CHECK(w.status().unpaired_positions==1&&w.status().ignored==ignored);
        CHECK(now_ns<old_deadline);callback(0);
        CHECK(last_send.choice==A::ORIGINAL&&last_send.reason==A::EPOCH_MISMATCH);
        CHECK(!std::memcmp(original,forwarded,48));
        startup(s,false);expect_ready(w);CHECK(w.status().begins==1);w.stop();
    }
}
static R::AssistInput queued_cutoff_anchor(const A::Observation& gps,uint64_t measured) {
    R::AssistInput a=input(R::ASSIST_ANCHOR);a.context.generation=gps.prediction_generation;
    a.received_ns=gps.mono_ns;a.position_call_sequence=gps.call_sequence;
    a.anchor.context=a.context;a.anchor.anchor_id=gps.call_sequence;
    a.anchor.position_seq=uint64_t(gps.call_sequence)*4;a.anchor.measured_ns=measured;
    a.anchor.utc_ns=1700000000000000000ULL;a.anchor.latitude_deg=37;a.anchor.longitude_deg=127;
    a.anchor.position_error_m=1;a.anchor.heading_error_rad=.01;
    a.anchor.validated=a.anchor.heading_valid=a.anchor.calibration_verified=1;
    a.anchor.quality=MX5_DR_VALID;return a;
}
static R::AssistInput queued_cutoff_position(const A::Observation& gps) {
    R::AssistInput p=input(R::ASSIST_POSITION);
    p.context.generation=gps.prediction_generation;p.observation=gps;return p;
}
static void queued_retirement_keeps_the_same_negative_time_boundary() {
    for(unsigned next_tick=0;next_tick<2;++next_tick)for(unsigned boundary=0;boundary<3;++boundary) {
        Source s;R::AssistWorker w(mx5_dr_default_config(),s.api());startup(s);expect_ready(w);
        now_ns+=10000000ULL;const A::Observation paired=callback(1);
        s.add(queued_cutoff_anchor(paired,paired.mono_ns));s.add(queued_cutoff_position(paired));
        now_ns+=10000000ULL;const A::Observation unpaired=callback(1);
        s.add(queued_cutoff_position(unpaired));
        now_ns+=10000000ULL;const A::Observation discarded=callback(1);
        CHECK(discarded.prediction_generation==unpaired.prediction_generation);
        if(!next_tick)s.add(queued_cutoff_position(discarded));
        // The newer callback can already be queued beyond complete-through.
        // A later anchor between these times does not violate the watermark.
        s.watermark=unpaired.mono_ns;w.tick(clock_fn,0);
        CHECK(w.status().state==R::ASSIST_WAITING_INPUT&&w.status().unpaired_positions==1);
        const uint32_t retired=A::generation();
        CHECK(retired>discarded.prediction_generation);
        if(next_tick)s.add(queued_cutoff_position(discarded));
        w.tick(clock_fn,0);CHECK(w.status().state==R::ASSIST_WAITING_INPUT);
        CHECK(A::generation()==retired&&w.status().ignored==next_tick);

        now_ns+=10000000ULL;const A::Observation fresh=callback(1);
        const uint64_t measured=discarded.mono_ns+uint64_t(boundary)*5000000ULL-5000000ULL;
        CHECK(measured>s.watermark&&measured<=fresh.mono_ns);
        CHECK(fresh.prediction_generation==retired&&fresh.call_sequence>discarded.call_sequence);
        s.add(queued_cutoff_anchor(fresh,measured));s.add(queued_cutoff_position(fresh));
        now_ns+=10000000ULL;s.add(queued_cutoff_position(callback(0)));
        const uint64_t end=measured+100000000ULL;
        motion(s,measured,end,2);now_ns=s.watermark=end;w.tick(clock_fn,0);
        const R::AssistStatus result=w.status();callback(0);
        if(boundary<2) {
            if(result.state!=R::ASSIST_INPUT_FAULT||last_send.choice!=A::ORIGINAL)
                std::fprintf(stderr,"queued cutoff: next_tick=%u boundary=%u state=%u published=%llu choice=%u\n",
                    next_tick,boundary,unsigned(result.state),(unsigned long long)result.published,
                    unsigned(last_send.choice));
            CHECK(result.state==R::ASSIST_INPUT_FAULT&&result.published==1);
            CHECK(last_send.choice==A::ORIGINAL&&!std::memcmp(original,forwarded,48));
            startup(s,false);w.tick(clock_fn,0);CHECK(w.status().state==R::ASSIST_WAITING_BEGIN);
            startup(s);expect_ready(w);CHECK(w.status().begins==2);
        } else {
            // A genuinely later measurement can recover without a new BEGIN.
            CHECK(result.state==R::ASSIST_PUBLISHED&&result.published==2);
            CHECK(last_send.choice==A::DR_REPLACEMENT&&result.begins==1);
        }
        w.stop();
    }
}
static void unseen_old_gps_cannot_survive_a_new_unpublished_seed() {
    Source s;R::AssistWorker w(mx5_dr_default_config(),s.api());startup(s);expect_ready(w);
    const uint64_t old_deadline=w.status().last_publication.valid_until_mono_ns;
    now_ns+=10000000ULL;const A::Observation gps=callback(1);
    R::AssistInput anchor=input(R::ASSIST_ANCHOR);
    anchor.received_ns=gps.mono_ns;anchor.position_call_sequence=gps.call_sequence;
    anchor.anchor.context=anchor.context;anchor.anchor.anchor_id=gps.call_sequence;
    anchor.anchor.position_seq=uint64_t(gps.call_sequence)*4;
    anchor.anchor.measured_ns=gps.mono_ns;anchor.anchor.utc_ns=1700000000000000000ULL;
    anchor.anchor.latitude_deg=37;anchor.anchor.longitude_deg=127;
    anchor.anchor.position_error_m=1;anchor.anchor.heading_error_rad=.01;
    anchor.anchor.validated=anchor.anchor.heading_valid=anchor.anchor.calibration_verified=1;
    anchor.anchor.quality=MX5_DR_VALID;s.add(anchor);
    R::AssistInput position=input(R::ASSIST_POSITION);position.observation=gps;s.add(position);
    now_ns+=10000000ULL;const A::Observation gap=callback(0);
    R::AssistInput gap_input=input(R::ASSIST_POSITION);gap_input.observation=gap;s.add(gap_input);
    motion(s,gps.mono_ns,gap.mono_ns,2);
    // Keep the new ACTIVE calculator alive but withhold its first publication
    // under a legitimate, temporarily tighter qualified error budget.
    s.error_max_m=.1;s.watermark=now_ns;w.tick(clock_fn,0);
    CHECK(w.status().state==R::ASSIST_WAITING_INPUT);
    CHECK(w.status().begins==1&&w.status().published==1);
    CHECK(now_ns<old_deadline);callback(0);
    CHECK(last_send.choice==A::ORIGINAL);

    // The adapter assigns the call sequence before exchanging the previous
    // mode. Concurrent callbacks can capture the old GPS generation at K+1,
    // while a GAP with sequence K invalidates it and reaches the worker first.
    R::AssistInput late=input(R::ASSIST_POSITION);
    late.context=position.context;
    late.observation=gps;
    late.observation.call_sequence=gap.call_sequence+1;
    late.observation.mono_ns=now_ns+1;
    now_ns+=10000000ULL;s.error_max_m=100;s.watermark=now_ns;s.add(late);
    w.tick(clock_fn,0);
    CHECK(w.status().state==R::ASSIST_INPUT_FAULT);
    CHECK(w.status().begins==1&&w.status().ignored==0);
    CHECK(A::generation()>gap.prediction_generation);
    callback(0);CHECK(last_send.choice==A::ORIGINAL);
    w.stop();
}
static void discarded_old_gps_excludes_earlier_future_inputs() {
    for(unsigned scenario=0;scenario<4;++scenario) {
        Source s;R::AssistWorker w(mx5_dr_default_config(),s.api());startup(s);expect_ready(w);
        now_ns+=10000000ULL;const A::Observation paired=callback(1);
        R::AssistInput pair_anchor=input(R::ASSIST_ANCHOR);
        pair_anchor.received_ns=paired.mono_ns;
        pair_anchor.position_call_sequence=paired.call_sequence;
        pair_anchor.anchor.context=pair_anchor.context;
        pair_anchor.anchor.anchor_id=paired.call_sequence;
        pair_anchor.anchor.position_seq=uint64_t(paired.call_sequence)*4;
        pair_anchor.anchor.measured_ns=paired.mono_ns;
        pair_anchor.anchor.utc_ns=1700000000000000000ULL;
        pair_anchor.anchor.latitude_deg=37;pair_anchor.anchor.longitude_deg=127;
        pair_anchor.anchor.position_error_m=1;pair_anchor.anchor.heading_error_rad=.01;
        pair_anchor.anchor.validated=pair_anchor.anchor.heading_valid=
            pair_anchor.anchor.calibration_verified=1;
        pair_anchor.anchor.quality=MX5_DR_VALID;s.add(pair_anchor);
        R::AssistInput pair_position=input(R::ASSIST_POSITION);
        pair_position.observation=paired;s.add(pair_position);
        now_ns+=10000000ULL;const A::Observation unpaired=callback(1);
        R::AssistInput first=input(R::ASSIST_POSITION);first.observation=unpaired;s.add(first);
        now_ns+=10000000ULL;const A::Observation old=callback(1);
        R::AssistInput delayed=input(R::ASSIST_POSITION);delayed.observation=old;
        s.watermark=now_ns;w.tick(clock_fn,0);
        CHECK(w.status().state==R::ASSIST_WAITING_INPUT);
        const uint32_t retired_generation=A::generation();
        now_ns+=10000000ULL;const A::Observation future=callback(1);
        R::AssistInput position=input(R::ASSIST_POSITION);position.observation=future;
        CHECK(future.prediction_generation==retired_generation);
        CHECK(old.prediction_generation<future.prediction_generation);
        if(scenario<2) {
            // The old callback captured its generation before retirement,
            // then its observer timestamp after this newer callback's.
            delayed.observation.mono_ns=future.mono_ns+1;
        } else {
            // Callback entry sequence can invert generation capture under
            // concurrency. This unpaired old fix entered after the new fix.
            delayed.observation.call_sequence=future.call_sequence+1;
            delayed.observation.mono_ns=future.mono_ns-1;
        }
        now_ns+=10000000ULL;s.watermark=now_ns;s.add(delayed);
        w.tick(clock_fn,0);
        CHECK(w.status().state==R::ASSIST_WAITING_INPUT&&w.status().ignored==1);
        if(scenario%2==0) {
            R::AssistInput anchor=input(R::ASSIST_ANCHOR);
            anchor.received_ns=future.mono_ns;
            anchor.position_call_sequence=future.call_sequence;
            anchor.anchor.context=anchor.context;
            anchor.anchor.anchor_id=future.call_sequence;
            anchor.anchor.position_seq=uint64_t(future.call_sequence)*4;
            anchor.anchor.measured_ns=future.mono_ns;
            anchor.anchor.utc_ns=1700000000000000000ULL;
            anchor.anchor.latitude_deg=37;anchor.anchor.longitude_deg=127;
            anchor.anchor.position_error_m=1;anchor.anchor.heading_error_rad=.01;
            anchor.anchor.validated=anchor.anchor.heading_valid=anchor.anchor.calibration_verified=1;
            anchor.anchor.quality=MX5_DR_VALID;s.add(anchor);
        } else s.add(position);
        w.tick(clock_fn,0);
        CHECK(w.status().state==R::ASSIST_INPUT_FAULT);
        CHECK(w.status().begins==1&&w.status().ignored==1);
        callback(0);CHECK(last_send.choice==A::ORIGINAL);w.stop();
    }
}
static void late_anchor_for_observed_gps_requires_recovery() {
    Source s;R::AssistWorker w(mx5_dr_default_config(),s.api());startup(s);expect_ready(w);
    now_ns+=10000000ULL;const A::Observation gps=callback(1);
    R::AssistInput position=input(R::ASSIST_POSITION);position.observation=gps;s.add(position);
    s.watermark=gps.mono_ns;w.tick(clock_fn,0);
    CHECK(w.status().state==R::ASSIST_WAITING_INPUT&&w.status().unpaired_positions==1);
    const uint32_t generation=A::generation();
    R::AssistInput anchor=input(R::ASSIST_ANCHOR);
    anchor.received_ns=gps.mono_ns;anchor.position_call_sequence=gps.call_sequence;
    anchor.anchor.context=anchor.context;anchor.anchor.anchor_id=gps.call_sequence;
    anchor.anchor.position_seq=uint64_t(gps.call_sequence)*4;
    anchor.anchor.measured_ns=gps.mono_ns;anchor.anchor.utc_ns=1700000000000000000ULL;
    anchor.anchor.latitude_deg=37;anchor.anchor.longitude_deg=127;
    anchor.anchor.position_error_m=1;anchor.anchor.heading_error_rad=.01;
    anchor.anchor.validated=anchor.anchor.heading_valid=anchor.anchor.calibration_verified=1;
    anchor.anchor.quality=MX5_DR_VALID;s.add(anchor);
    now_ns+=10000000ULL;s.watermark=now_ns;w.tick(clock_fn,0);
    CHECK(w.status().state==R::ASSIST_INPUT_FAULT&&w.status().begins==1);
    CHECK(w.status().ignored==0&&A::generation()==generation);
    callback(0);CHECK(last_send.choice==A::ORIGINAL);
    startup(s,false);w.tick(clock_fn,0);
    CHECK(w.status().state==R::ASSIST_WAITING_BEGIN);
    callback(0);CHECK(last_send.choice==A::ORIGINAL);w.stop();
}
static void replay_or_malformed_old_control_revokes_a_live_candidate() {
    for(unsigned scenario=0;scenario<2;++scenario) {
        Source s;R::AssistWorker w(mx5_dr_default_config(),s.api());startup(s);expect_ready(w);
        const uint64_t deadline=w.status().last_publication.valid_until_mono_ns;
        now_ns+=10000000ULL;
        if(!scenario) {
            // This exact GPS callback was already consumed before the GAP.
            s.add(s.queue[1]);
        } else {
            R::AssistInput anchor=input(R::ASSIST_ANCHOR);
            anchor.received_ns=now_ns;
            anchor.position_call_sequence=s.queue[1].observation.call_sequence;
            anchor.anchor.context=anchor.context;
            anchor.anchor.measured_ns=now_ns+1; // Impossible producer time.
            s.add(anchor);
        }
        s.watermark=now_ns;w.tick(clock_fn,0);
        CHECK(w.status().state==R::ASSIST_INPUT_FAULT&&w.status().ignored==0);
        CHECK(now_ns<deadline);callback(0);
        CHECK(last_send.choice==A::ORIGINAL&&!std::memcmp(original,forwarded,48));
        w.stop();
    }
}
static void old_control_after_new_begin_is_a_source_fault() {
    Source s;R::AssistWorker w(mx5_dr_default_config(),s.api());startup(s);expect_ready(w);
    const R::AssistInput old_position=s.queue[1];
    now_ns+=10000000ULL;s.qualified=false;w.tick(clock_fn,0);
    CHECK(w.status().state==R::ASSIST_WAITING_SOURCE);
    s.qualified=true;now_ns+=10000000ULL;
    R::AssistInput begin=input(R::ASSIST_BEGIN);begin.received_ns=now_ns;s.add(begin);
    s.add(old_position);s.watermark=now_ns;w.tick(clock_fn,0);
    CHECK(w.status().state==R::ASSIST_INPUT_FAULT&&w.status().begins==2);
    CHECK(w.status().ignored==0);callback(0);CHECK(last_send.choice==A::ORIGINAL);
    w.stop();
}
static void verified_epoch_rollover_uses_its_original_begin_time() {
    for(unsigned changed=0;changed<2;++changed) {
        Source s;R::AssistWorker w(mx5_dr_default_config(),s.api());startup(s);expect_ready(w);
        const uint64_t original_begins=w.status().begins;
        if(changed)++source_epoch;else ++session_epoch;
        A::invalidate();
        // The source establishes a new verified lifetime and receives its GPS
        // before the next worker tick. Receipt times must remain original.
        const size_t begin_index=s.count;startup(s);
        const uint64_t captured_begin=s.queue[begin_index].received_ns;
        CHECK(captured_begin<now_ns);
        expect_ready(w);CHECK(w.status().begins==original_begins+1);
        CHECK(s.queue[begin_index].received_ns==captured_begin);
        // A later loss in this new epoch still forbids its historical BEGIN.
        const uint64_t until=w.status().last_publication.valid_until_mono_ns;
        now_ns+=10000000;s.qualified=false;w.tick(clock_fn,0);expect_withdrawn(until);
        s.qualified=true;s.index=begin_index;w.tick(clock_fn,0);
        CHECK(w.status().state==R::ASSIST_INPUT_FAULT);expect_withdrawn(until);
        w.stop();source_epoch=11;session_epoch=12;
    }
}
static void clock_is_rechecked_after_calculation() {
    Source s;R::AssistWorker w(mx5_dr_default_config(),s.api());startup(s);
    s.clock_advance_on_read=160000000;w.tick(clock_fn,0);
    CHECK(s.reads==2);CHECK(w.status().published==0);
    callback(0);CHECK(last_send.choice==A::ORIGINAL);w.stop();
}
static void events_arriving_during_tick_keep_their_actual_timestamps() {
    Source s;R::AssistWorker w(mx5_dr_default_config(),s.api());startup(s);expect_ready(w);
    const uint64_t frontier=now_ns;
    motion(s,frontier,frontier+50000000,2);
    now_ns+=10000000;s.clock_advance_on_pop=40000000;w.tick(clock_fn,0);
    // These owned samples arrived after the first clock read, but before the
    // worker popped them. They are not future samples and must not reset DR.
    CHECK(w.status().state==R::ASSIST_PUBLISHED&&w.status().begins==1);
    CHECK(w.status().last_publication.frontier_mono_ns==frontier);
    CHECK(now_ns==frontier+50000000);
    // Only the next producer watermark permits advancing the prediction.
    w.tick(clock_fn,0);CHECK(w.status().state==R::ASSIST_PUBLISHED);
    CHECK(w.status().last_publication.frontier_mono_ns==now_ns);w.stop();
}
static void missing_begin_and_model_inputs_stay_unqualified() {
    Source no_begin;R::AssistWorker waiting(mx5_dr_default_config(),no_begin.api());startup(no_begin,false);
    waiting.tick(clock_fn,0);CHECK(waiting.status().state==R::ASSIST_WAITING_BEGIN);
    callback(0);CHECK(last_send.choice==A::ORIGINAL);waiting.stop();
    Source model;R::AssistWorker rejected(mx5_dr_default_config(),model.api());startup(model);
    model.queue[4].evidence.quality=MX5_DR_MODEL;model.queue[4].evidence.freshness=MX5_DR_MODEL_TIME;
    rejected.tick(clock_fn,0);CHECK(rejected.status().state==R::ASSIST_INPUT_FAULT);
    callback(0);CHECK(last_send.choice==A::ORIGINAL);rejected.stop();
}
static void invalid_identity_and_time_withdraw_immediately() {
    for(unsigned scenario=0;scenario<6;++scenario) {
        Source s;R::AssistWorker w(mx5_dr_default_config(),s.api());startup(s);expect_ready(w);
        const uint64_t until=w.status().last_publication.valid_until_mono_ns;
        now_ns+=10000000;
        R::AssistInput i=input(R::ASSIST_POSITION);i.observation=callback(0);
        if(scenario==0)i.context.session_epoch=99;
        if(scenario==1)i.observation.provenance.exact_request=false;
        if(scenario==2)i.observation.prediction_generation=0;
        if(scenario==3)i.observation.mono_ns=now_ns+1;
        if(scenario<4)s.add(i);
        if(scenario==4)now_ns=0;
        if(scenario==5)now_ns-=20000000;
        const uint32_t generation=A::generation();w.tick(clock_fn,0);
        CHECK(w.status().state==(scenario<4?R::ASSIST_INPUT_FAULT:R::ASSIST_CLOCK_FAULT));
        CHECK(A::generation()==generation+1);
        if(scenario>=4)now_ns=until-100000000;
        expect_withdrawn(until);w.stop();
    }
}
static void bounded_batch_does_not_publish_across_unread_inputs() {
    Source s;R::AssistWorker w(mx5_dr_default_config(),s.api());startup(s);expect_ready(w);
    const uint64_t until=w.status().last_publication.valid_until_mono_ns;
    now_ns+=10000000;
    for(size_t n=0;n<R::AssistWorker::INPUT_BUDGET;++n) {
        R::AssistInput i=input(R::ASSIST_POSITION);i.observation=callback(0);s.add(i);
    }
    const unsigned before=s.polls;w.tick(clock_fn,0);
    CHECK(s.polls-before==R::AssistWorker::INPUT_BUDGET);
    CHECK(w.status().state==R::ASSIST_BACKLOG);expect_withdrawn(until);
    // Bounded backlog withdraws a ready candidate across the send race.
    // EMPTY alone cannot revive the old seed or refresh its lease.
    w.tick(clock_fn,0);CHECK(w.status().state==R::ASSIST_WAITING_BEGIN);
    expect_withdrawn(until);s.index=s.count;startup(s);expect_ready(w);w.stop();
}
static void null_source_is_explicitly_unimplemented() {
    R::AssistSource absent=R::AssistSource();R::AssistWorker w(mx5_dr_default_config(),absent);
    w.tick(clock_fn,0);CHECK(w.status().state==R::ASSIST_WAITING_SOURCE);
    CHECK(w.status().inputs==0&&w.status().published==0);w.stop();
}
int main() {
    for(unsigned n=0;n<48;++n)original[n]=uint8_t(n+1);
    A::Options options=A::Options();options.clock=clock_fn;options.provenance=provenance;
    options.sink=observe;options.max_snapshot_age_ns=150000000;options.allow_assist=true;
    CHECK(A::configure(endpoint,options));CHECK(A::set_mode(A::ASSIST));
    publication_and_stop();loss_withdraws_and_requires_new_begin(true,false);
    loss_withdraws_and_requires_new_begin(false,false);loss_withdraws_and_requires_new_begin(false,true);
    generation_changes_are_not_retagged();healthy_reacquisition_keeps_the_same_worker();
    delayed_anchor_after_gps_return_keeps_the_worker();clock_is_rechecked_after_calculation();
    unpaired_gps_fix_cannot_reactivate_an_old_anchor();
    stale_position_batching_keeps_verified_recovery_available();
    queued_retirement_keeps_the_same_negative_time_boundary();
    unseen_old_gps_cannot_survive_a_new_unpublished_seed();
    discarded_old_gps_excludes_earlier_future_inputs();
    late_anchor_for_observed_gps_requires_recovery();
    replay_or_malformed_old_control_revokes_a_live_candidate();
    old_control_after_new_begin_is_a_source_fault();
    verified_epoch_rollover_uses_its_original_begin_time();
    events_arriving_during_tick_keep_their_actual_timestamps();
    missing_begin_and_model_inputs_stay_unqualified();bounded_batch_does_not_publish_across_unread_inputs();
    invalid_identity_and_time_withdraw_immediately();null_source_is_explicitly_unimplemented();
    std::printf("assist worker: %u checks passed\n",checks);return 0;
}
