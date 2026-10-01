#include "assist_worker.h"
#include <cmath>

namespace mx5 { namespace runtime {
namespace {
bool representable(uint64_t value) { return value&&value<=UINT32_MAX; }
bool context_valid(const mx5_dr_context& c) {
    return representable(c.source_epoch)&&representable(c.session_epoch)&&representable(c.generation);
}
bool same_epochs(const mx5_dr_context& a,const mx5_dr_context& b) {
    return a.source_epoch==b.source_epoch&&a.session_epoch==b.session_epoch;
}
bool same_context(const mx5_dr_context& a,const mx5_dr_context& b) {
    return same_epochs(a,b)&&a.generation==b.generation;
}
bool limit(double value,double maximum) { return std::isfinite(value)&&value>0&&value<=maximum; }
bool nonfatal(navigation::PipelineResult r) {
    return r==navigation::PIPELINE_OK||r==navigation::PIPELINE_WAITING||r==navigation::PIPELINE_NO_ANCHOR;
}
}
AssistWorker::AssistWorker(const mx5_dr_config& config,const AssistSource& source)
    : config_(config),source_(source),status_(),binding_(),recovery_binding_(),last_now_ns_(0),
      recovery_after_ns_(0),active_(false),stopped_(false) {}

bool AssistWorker::readiness(uint64_t now,AssistReadiness* out) {
    *out=AssistReadiness();
    if(!source_.pop||!source_.readiness||!source_.readiness(source_.user,now,out))return false;
    const CoreBridgeQualification& q=out->qualification;
    return context_valid(q.expected_context)&&q.now_mono_ns==now&&
        q.profile_verified&&q.input_quality_verified&&q.max_snapshot_age_ns&&
        q.max_snapshot_age_ns<=150000000&&q.limits_verified_until_mono_ns>=now&&
        limit(q.duration_max_s,60)&&limit(q.distance_max_m,1500)&&limit(q.error_max_m,100)&&
        out->watermark_ns&&out->watermark_ns<=now&&out->requested_until_ns>=now;
}
void AssistWorker::withdraw() {
    adapter::DrSnapshot empty=adapter::DrSnapshot();
    empty.source_epoch=static_cast<uint32_t>(binding_.source_epoch);
    empty.session_epoch=static_cast<uint32_t>(binding_.session_epoch);
    empty.prediction_generation=adapter::generation();
    // A generation race already revokes the previous ready candidate. Never
    // retag ready output or consume another generation just to clear it.
    if(adapter::publish_snapshot(empty))++status_.withdrawn;
}
void AssistWorker::revoke(AssistState state,uint64_t now,bool require_begin) {
    if(require_begin&&active_) {
        active_=false;
        if(!same_epochs(binding_,recovery_binding_)) {
            recovery_binding_=binding_;recovery_after_ns_=0;
        }
        if(now>recovery_after_ns_)recovery_after_ns_=now;
    }
    status_.state=state;withdraw();
}
bool AssistWorker::consume(const AssistInput& in,uint64_t now,const AssistReadiness& ready) {
    if(in.kind<ASSIST_BEGIN||in.kind>ASSIST_REVERSE||!context_valid(in.context)||
       !same_epochs(in.context,ready.qualification.expected_context)||
       in.context.generation>adapter::generation())return false;
    if(in.kind==ASSIST_BEGIN) {
        // This loss boundary belongs to the old source/session. A new epoch
        // can already have a verified BEGIN/anchor queued before this worker
        // first notices the transition; do not fabricate later receipt times.
        if(!in.received_ns||in.received_ns>now||
           (same_epochs(in.context,recovery_binding_)&&in.received_ns<recovery_after_ns_))return false;
        if(!pipeline_.init_qualified(config_,in.context))return false;
        binding_=in.context;active_=true;++status_.begins;return true;
    }
    if(!active_) { ++status_.ignored;return true; }
    if(!same_epochs(in.context,binding_))return false;
    navigation::PipelineResult result=navigation::PIPELINE_BAD_INPUT;
    if(in.kind==ASSIST_POSITION) {
        const adapter::Observation& o=in.observation;
        if(o.kind!=adapter::Observation::POSITION||!o.call_sequence||!o.mono_ns||o.mono_ns>now||
           o.prediction_generation!=in.context.generation||o.position.mode<0||o.position.mode>3||
           o.original_mode!=o.position.mode||o.provenance.source_epoch!=in.context.source_epoch||
           o.provenance.session_epoch!=in.context.session_epoch||!o.provenance.exact_request||
           !o.provenance.verified_lds||!o.provenance.legacy_receiver)return false;
        result=pipeline_.enqueue_position(o);
    } else if(in.kind==ASSIST_ANCHOR) {
        if(!same_context(in.context,in.anchor.context)||!in.received_ns||in.received_ns>now||
           (same_epochs(in.context,recovery_binding_)&&in.anchor.measured_ns<recovery_after_ns_))return false;
        result=pipeline_.enqueue_anchor(in.anchor,in.received_ns);
    } else {
        // Sensor stream epochs belong to each producer, independently of the
        // per-request provenance context. The core enforces their continuity.
        const mx5_dr_evidence& e=in.evidence;
        if(!e.source_id||!e.source_epoch||!e.producer_seq||!e.measured_ns||e.received_ns>now||
           e.quality!=MX5_DR_VALID||(e.freshness!=MX5_DR_PRODUCER_TIME&&
           e.freshness!=MX5_DR_SEQUENCE_WITH_BOUND))return false;
        if(in.kind==ASSIST_SPEED)result=pipeline_.enqueue_speed(e,in.value);
        else if(in.kind==ASSIST_YAW)result=pipeline_.enqueue_yaw(e,in.value,in.raw_yaw,
            in.yaw_count,in.window_start_ns,in.window_end_ns);
        else result=pipeline_.enqueue_reverse(e,in.reverse);
    }
    status_.pipeline_result=result;
    return nonfatal(result);
}
void AssistWorker::tick(adapter::MonotonicClock clock,void* clock_user) {
    if(stopped_)return;
    ++status_.ticks;
    const uint64_t before=clock?clock(clock_user):0;
    if(!before||before<last_now_ns_) {
        revoke(ASSIST_CLOCK_FAULT,last_now_ns_,true);return;
    }
    last_now_ns_=before;
    AssistReadiness initial;
    if(!readiness(before,&initial)) { revoke(ASSIST_WAITING_SOURCE,before,true);return; }
    if(active_&&!same_epochs(binding_,initial.qualification.expected_context)) {
        revoke(ASSIST_CONTEXT_CHANGED,before,true);
    }
    bool empty=false;
    for(size_t n=0;n<INPUT_BUDGET;++n) {
        AssistInput in=AssistInput();
        const AssistPoll result=source_.pop(source_.user,&in);
        // A producer can append a legitimate newly received sample after the
        // tick's first clock read. Compare its original time to a clock read
        // after obtaining ownership, without changing the drain watermark.
        const uint64_t acquired=clock(clock_user);
        if(!acquired||acquired<last_now_ns_) { revoke(ASSIST_CLOCK_FAULT,last_now_ns_,true);return; }
        last_now_ns_=acquired;
        if(result==ASSIST_EMPTY) { empty=true;break; }
        if(result!=ASSIST_INPUT) { revoke(ASSIST_SOURCE_FAULT,acquired,true);return; }
        ++status_.inputs;
        if(!consume(in,acquired,initial)) { revoke(ASSIST_INPUT_FAULT,acquired,true);return; }
    }
    // Bounded work is not evidence of an empty stream. A revocation may be the
    // very next item; do not publish across an unconsumed part of this batch.
    if(!empty) { revoke(ASSIST_BACKLOG,last_now_ns_,false);return; }
    if(active_)
        status_.pipeline_result=pipeline_.drain(initial.watermark_ns);
    const uint64_t after=clock(clock_user);
    if(!after||after<last_now_ns_) { revoke(ASSIST_CLOCK_FAULT,last_now_ns_,true);return; }
    last_now_ns_=after;
    if(active_&&!nonfatal(status_.pipeline_result)) { revoke(ASSIST_INPUT_FAULT,after,true);return; }
    AssistReadiness final;
    if(!readiness(after,&final)) { revoke(ASSIST_WAITING_SOURCE,after,true);return; }
    if(!same_epochs(initial.qualification.expected_context,final.qualification.expected_context)) {
        revoke(ASSIST_CONTEXT_CHANGED,after,true);return;
    }
    if(final.watermark_ns<initial.watermark_ns) { revoke(ASSIST_INPUT_FAULT,after,true);return; }
    if(!active_) { revoke(ASSIST_WAITING_BEGIN,after,false);return; }
    if(final.qualification.expected_context.generation!=adapter::generation()||
       !same_context(pipeline_.context(),final.qualification.expected_context)) {
        revoke(ASSIST_CONTEXT_CHANGED,after,false);return;
    }
    adapter::DrSnapshot output=adapter::DrSnapshot();
    status_.bridge_result=pipeline_.qualified_publication(after,final.qualification,
        final.requested_until_ns,&output);
    if(status_.bridge_result!=CORE_BRIDGE_OK) { revoke(ASSIST_WAITING_INPUT,after,false);return; }
    if(!adapter::publish_snapshot(output)) { revoke(ASSIST_CONTEXT_CHANGED,after,false);return; }
    status_.last_publication=output;++status_.published;status_.state=ASSIST_PUBLISHED;
}
void AssistWorker::stop() {
    if(stopped_)return;
    stopped_=true;revoke(ASSIST_STOPPED,last_now_ns_,true);
}
} }
