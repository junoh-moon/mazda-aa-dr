// Authored qualified inputs and OEM endpoint; no vehicle/provider qualification.
// The DSO build calls product functions by their own ELF symbols. It never
// recompiles the estimator/adapter or changes the library under test.
#include "navigation/pipeline.h"
#include <cassert>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <pthread.h>
namespace A=mx5::adapter;
namespace N=mx5::navigation;
namespace R=mx5::runtime;
#ifdef MX5_ASSIST_DSO_TEST
#include "assist_dso_access.h"
#else
typedef N::Pipeline ProductPipeline;
static uint32_t current_generation() { return A::generation(); }
#endif

// A persistent fixture worker owns the Pipeline. OEM callbacks run on main.
// The handshakes make scheduling deterministic without moving calculation into
// the OEM callback. This is not the live runtime's still-unimplemented input pump.
class Worker {
    ProductPipeline pipeline_;
    pthread_mutex_t mutex_;
    pthread_cond_t condition_;
    pthread_t thread_;
    void (*job_)(ProductPipeline&,void*);
    void* argument_;
    bool stopping_;
    static void* run(void* self) {
        Worker& w=*static_cast<Worker*>(self);
        assert(!pthread_mutex_lock(&w.mutex_));
        for(;;) {
            while(!w.job_&&!w.stopping_)assert(!pthread_cond_wait(&w.condition_,&w.mutex_));
            if(w.stopping_)break;
            void (*job)(ProductPipeline&,void*)=w.job_;void* argument=w.argument_;
            assert(!pthread_mutex_unlock(&w.mutex_));
            job(w.pipeline_,argument);
            assert(!pthread_mutex_lock(&w.mutex_));w.job_=0;
            assert(!pthread_cond_broadcast(&w.condition_));
        }
        assert(!pthread_mutex_unlock(&w.mutex_));return 0;
    }
public:
    Worker():job_(0),argument_(0),stopping_(false) {
        assert(!pthread_mutex_init(&mutex_,0));assert(!pthread_cond_init(&condition_,0));
        assert(!pthread_create(&thread_,0,run,this));
    }
    ~Worker() {
        assert(!pthread_mutex_lock(&mutex_));stopping_=true;
        assert(!pthread_cond_broadcast(&condition_));assert(!pthread_mutex_unlock(&mutex_));
        assert(!pthread_join(thread_,0));
        assert(!pthread_cond_destroy(&condition_));assert(!pthread_mutex_destroy(&mutex_));
    }
    void call(void (*job)(ProductPipeline&,void*),void* argument) {
        assert(!pthread_mutex_lock(&mutex_));assert(!job_);
        job_=job;argument_=argument;assert(!pthread_cond_broadcast(&condition_));
        while(job_)assert(!pthread_cond_wait(&condition_,&mutex_));
        assert(!pthread_mutex_unlock(&mutex_));
    }
};
static uint64_t now_ns=1000000000ULL;
static unsigned sends,replacements;
static A::Observation position,selected;
static A::DrSnapshot last_publication;
static uint64_t previous_intervals,previous_solution;
static double input_latitude=37,input_longitude=127;
static uint64_t input_utc=1700000000ULL;
static A::VehicleData* borrowed;
static bool expect_original;
static uint8_t sent[48],original[48];
static uint64_t clock_fn(void*) { return now_ns; }
static bool provenance(void*,const A::PositionInput*,A::Provenance* out,void*) {
    out->source_epoch=11;out->session_epoch=12;
    out->exact_request=out->verified_lds=out->legacy_receiver=true; // synthetic only
    return true;
}
static void observe(const A::Observation* event,void*) {
    if(event->kind==A::Observation::POSITION)position=*event;else selected=*event;
    errno=E2BIG; // The product hook must restore the OEM-visible errno.
}
static uint64_t revoke_candidate(void*) { return A::invalidate(); }
static int32_t endpoint(void* session,A::VehicleData* data) {
    assert(session==&sends&&errno==EDOM);
    assert((data==borrowed)==expect_original&&data->length==48);
    std::memcpy(sent,data->payload,48);++sends;
    errno=ERANGE;return -731;
}
static void put32(uint8_t* p,uint32_t value) {
    for(unsigned i=0;i<4;++i)p[i]=uint8_t(value>>(8*i));
}
static void put64(uint8_t* p,uint64_t value) {
    for(unsigned i=0;i<8;++i)p[i]=uint8_t(value>>(8*i));
}
static uint64_t get64(const uint8_t* p) {
    uint64_t value=0;for(unsigned i=0;i<8;++i)value|=uint64_t(p[i])<<(8*i);return value;
}
static int32_t get32(const uint8_t* p) {
    uint32_t value=0;for(unsigned i=0;i<4;++i)value|=uint32_t(p[i])<<(8*i);return int32_t(value);
}
static void callback(int mode,uint64_t when,bool replacement) {
    now_ns=when;uint8_t raw[72]={};put32(raw,uint32_t(mode));put64(raw+8,input_utc);
    std::memcpy(raw+16,&input_latitude,8);std::memcpy(raw+24,&input_longitude,8);
    A::VehicleData data={1,original,48};borrowed=&data;expect_original=!replacement;
    const unsigned before=sends;errno=EDOM;
    A::position_enter(0,raw);assert(errno==EDOM);
    assert(A::send_vehicle_data(&sends,&data)==-731&&errno==ERANGE);
    A::position_leave();assert(errno==ERANGE&&sends==before+1);
    assert(selected.choice==(replacement?A::DR_REPLACEMENT:A::ORIGINAL));
    if(replacement)++replacements;else assert(!std::memcmp(sent,original,48));
    for(unsigned i=0;i<48;++i)assert(original[i]==uint8_t(i+1));
}
static mx5_dr_anchor authored_anchor(const A::Observation& gps,uint64_t id) {
    mx5_dr_anchor anchor=mx5_dr_anchor();anchor.context=mx5_dr_context{11,12,gps.prediction_generation};
    // The core needs a distinct control sequence between adjacent callback
    // anchors. This authored fixture reserves headroom for GAP/GPS_RETURN.
    anchor.anchor_id=id;anchor.position_seq=uint64_t(gps.call_sequence)*4;
    anchor.measured_ns=gps.mono_ns;
    anchor.utc_ns=gps.position.utc_seconds*1000000000ULL;
    anchor.latitude_deg=gps.position.latitude_deg;anchor.longitude_deg=gps.position.longitude_deg;
    anchor.position_error_m=1;anchor.heading_error_rad=0.01;
    anchor.validated=anchor.heading_valid=anchor.calibration_verified=1;anchor.quality=MX5_DR_VALID;
    return anchor;
}
static void seed(ProductPipeline& pipeline,void* argument) {
    const A::Observation& gps=*static_cast<A::Observation*>(argument);
    const mx5_dr_anchor anchor=authored_anchor(gps,1);
    assert(pipeline.init_qualified(mx5_dr_default_config(),anchor.context));
    assert(pipeline.bind_qualified_revoker(revoke_candidate,0));
    assert(pipeline.diagnostic(gps.mono_ns).snapshot.context.generation==current_generation());
    assert(pipeline.enqueue_anchor(anchor,gps.mono_ns)==N::PIPELINE_OK);
    assert(pipeline.enqueue_position(gps)==N::PIPELINE_OK);
}
static void enqueue_position(ProductPipeline& pipeline,void* argument) {
    assert(pipeline.enqueue_position(*static_cast<A::Observation*>(argument))==N::PIPELINE_OK);
}
struct Step {
    uint64_t start,end,now;
    unsigned sequence;
    double yaw;
    int reverse;
    bool verified;
    N::Diagnostic diagnostic;
    R::CoreBridgeResult result;
    A::DrSnapshot mapped;
    bool published;
};
static mx5_dr_evidence evidence(unsigned id,const Step& s,bool mean) {
    mx5_dr_evidence e=mx5_dr_evidence();e.source_id=id;e.source_epoch=1;
    e.producer_seq=s.sequence;e.measured_ns=mean?s.end:s.start;e.received_ns=s.end;
    e.lease_until_ns=e.measured_ns+250000000ULL;
    e.quality=MX5_DR_VALID;e.freshness=MX5_DR_PRODUCER_TIME;return e;
}
static void calculate(ProductPipeline& pipeline,void* argument) {
    Step& s=*static_cast<Step*>(argument);
    assert(pipeline.enqueue_speed(evidence(1,s,false),10)==N::PIPELINE_OK);
    assert(pipeline.enqueue_reverse(evidence(3,s,false),s.reverse)==N::PIPELINE_OK);
    assert(pipeline.enqueue_yaw(evidence(2,s,true),s.yaw,2047,1,s.start,s.end)==N::PIPELINE_OK);
    assert(pipeline.drain(s.end)==N::PIPELINE_OK);
    s.diagnostic=pipeline.diagnostic(s.now);
    R::CoreBridgeQualification q=R::CoreBridgeQualification();
    // Expected identity comes from the real adapter's control generation,
    // not from copying whatever generation the calculator happened to produce.
    q.expected_context=mx5_dr_context{11,12,current_generation()};
    q.now_mono_ns=q.limits_verified_until_mono_ns=s.now;q.max_snapshot_age_ns=150000000;
    q.duration_max_s=60;q.distance_max_m=1500;q.error_max_m=100;
    q.profile_verified=q.input_quality_verified=s.verified;
    s.result=pipeline.qualified_publication(s.now,q,s.now+50000000,&s.mapped);
    s.published=s.result==R::CORE_BRIDGE_OK&&A::publish_snapshot(s.mapped);
}
static void reject_return(ProductPipeline& pipeline,void* argument) {
    const A::Observation& gps=*static_cast<A::Observation*>(argument);
    assert(pipeline.enqueue_position(gps)==N::PIPELINE_OK);
    assert(!pipeline.diagnostic(gps.mono_ns).snapshot.valid); // even before drain
    pipeline.drain(gps.mono_ns);
    const N::Diagnostic d=pipeline.diagnostic(gps.mono_ns);
    assert(!d.snapshot.valid);
    if(d.snapshot.context.generation!=gps.prediction_generation)
        std::fprintf(stderr,"return: core_generation=%llu adapter_generation=%u\n",
            (unsigned long long)d.snapshot.context.generation,gps.prediction_generation);
    assert(d.snapshot.context.generation==gps.prediction_generation);
}
static void reject_stale_control(ProductPipeline& pipeline,void* argument) {
    A::Observation gps=*static_cast<A::Observation*>(argument);--gps.prediction_generation;
    assert(pipeline.enqueue_position(gps)==N::PIPELINE_OK);
    assert(pipeline.drain(gps.mono_ns)==N::PIPELINE_BAD_INPUT);
    const N::Diagnostic d=pipeline.diagnostic(gps.mono_ns);
    assert(!d.snapshot.valid&&d.result!=MX5_DR_OK&&d.status.resets==1);
}
static void fault_after_publication(ProductPipeline& pipeline,void*) {
    const uint32_t before=current_generation();
    const mx5_dr_evidence invalid=mx5_dr_evidence();
    assert(pipeline.enqueue_yaw(invalid,0,2047,1,2050000000ULL,2050000000ULL)==N::PIPELINE_BAD_INPUT);
    assert(current_generation()==before+1);
    const N::Diagnostic d=pipeline.diagnostic(2050000000ULL);
    assert(d.status.resets==1&&!d.snapshot.valid);
    assert(d.snapshot.context.generation==current_generation());
    assert(!A::publish_snapshot(last_publication));
}
static void reject_core_after_publication(ProductPipeline& pipeline,void*) {
    const uint32_t before=current_generation();
    mx5_dr_anchor anchor=mx5_dr_anchor();
    anchor.context=pipeline.diagnostic(2050000000ULL).snapshot.context;
    anchor.anchor_id=2;anchor.position_seq=4;anchor.measured_ns=2050000000ULL;
    anchor.utc_ns=1700000001050000000ULL;
    anchor.latitude_deg=37;anchor.longitude_deg=127;
    anchor.position_error_m=1;anchor.heading_error_rad=0.01;
    anchor.validated=anchor.heading_valid=anchor.calibration_verified=1;
    anchor.quality=MX5_DR_VALID;
    assert(pipeline.enqueue_anchor(anchor,anchor.measured_ns)==N::PIPELINE_OK);
    assert(pipeline.drain(anchor.measured_ns)==N::PIPELINE_CORE_REJECTED);
    assert(current_generation()==before+1);
    const N::Diagnostic d=pipeline.diagnostic(anchor.measured_ns);
    assert(d.status.resets==1&&!d.snapshot.valid);
    assert(d.snapshot.context.generation==current_generation());
    assert(!A::publish_snapshot(last_publication));
}
static void reinit_after_publication(ProductPipeline& pipeline,void*) {
    const uint32_t before=current_generation();
    const mx5_dr_context requested={11,12,before};
    assert(pipeline.init_qualified(mx5_dr_default_config(),requested));
    assert(current_generation()==before+1);
    assert(pipeline.diagnostic(2050000000ULL).snapshot.context.generation==current_generation());
    assert(pipeline.bind_qualified_revoker(revoke_candidate,0));
    assert(!A::publish_snapshot(last_publication));
}
static void failed_reinit_after_publication(ProductPipeline& pipeline,void*) {
    const uint32_t before=current_generation();
    mx5_dr_config invalid=mx5_dr_default_config();invalid.sample_age_max_ns=0;
    const mx5_dr_context requested={11,12,before};
    assert(!pipeline.init_qualified(invalid,requested));
    assert(current_generation()==before+1);
    assert(!pipeline.diagnostic(2050000000ULL).snapshot.valid);
    assert(!A::publish_snapshot(last_publication));
}
static void failed_model_reinit_after_publication(ProductPipeline& pipeline,void*) {
    const uint32_t before=current_generation();
    N::ModelProfile invalid=N::ModelProfile();
    const mx5_dr_context requested={11,12,before};
    assert(!pipeline.init_model(invalid,mx5_dr_default_config(),requested));
    assert(current_generation()==before+1);
    assert(!pipeline.diagnostic(2050000000ULL).snapshot.valid);
    assert(!A::publish_snapshot(last_publication));
}
static void recover_anchor_after_fault(ProductPipeline& pipeline,void* argument) {
    const A::Observation& gps=*static_cast<A::Observation*>(argument);
    const mx5_dr_anchor anchor=authored_anchor(gps,1);
    assert(pipeline.enqueue_anchor(anchor,gps.mono_ns)==N::PIPELINE_OK);
    assert(pipeline.enqueue_position(gps)==N::PIPELINE_OK);
    assert(pipeline.drain(gps.mono_ns)==N::PIPELINE_OK);
    const N::Diagnostic d=pipeline.diagnostic(gps.mono_ns);
    assert(d.snapshot.state==MX5_DR_READY&&d.status.resets==1);
    assert(d.snapshot.context.generation==current_generation());
}
static void reseed_after_reinit(ProductPipeline& pipeline,void* argument) {
    const A::Observation& gps=*static_cast<A::Observation*>(argument);
    const mx5_dr_anchor anchor=authored_anchor(gps,1);
    assert(pipeline.enqueue_anchor(anchor,gps.mono_ns)==N::PIPELINE_OK);
    assert(pipeline.enqueue_position(gps)==N::PIPELINE_OK);
}
static void reject_old_publication(ProductPipeline&,void* argument) {
    assert(!A::publish_snapshot(*static_cast<A::DrSnapshot*>(argument)));
}
struct Reanchor { A::Observation gps;bool anchor_first,separate_drain; };
static void continuous_anchor(ProductPipeline& pipeline,void* argument) {
    const Reanchor& r=*static_cast<Reanchor*>(argument);
    const N::Diagnostic before=pipeline.diagnostic(r.gps.mono_ns);
    const mx5_dr_anchor anchor=authored_anchor(r.gps,before.snapshot.anchor_id+1);
    assert(before.status.resets==0&&before.status.intervals==previous_intervals&&previous_intervals>=10);
    if(r.anchor_first)assert(pipeline.enqueue_anchor(anchor,r.gps.mono_ns)==N::PIPELINE_OK);
    assert(pipeline.enqueue_position(r.gps)==N::PIPELINE_OK);
    if(r.separate_drain)pipeline.drain(r.gps.mono_ns);
    if(!r.anchor_first)assert(pipeline.enqueue_anchor(anchor,r.gps.mono_ns)==N::PIPELINE_OK);
    assert(!pipeline.diagnostic(r.gps.mono_ns).snapshot.valid);
    const N::PipelineResult result=pipeline.drain(r.gps.mono_ns);
    const N::Diagnostic after=pipeline.diagnostic(r.gps.mono_ns);
    if(result!=N::PIPELINE_OK||after.snapshot.state!=MX5_DR_READY)
        std::fprintf(stderr,"reanchor: result=%u core=%u state=%u generation=%llu expected=%u resets=%llu\n",
            unsigned(result),unsigned(after.result),unsigned(after.snapshot.state),
            (unsigned long long)after.snapshot.context.generation,r.gps.prediction_generation,
            (unsigned long long)after.status.resets);
    assert(result==N::PIPELINE_OK&&after.snapshot.state==MX5_DR_READY);
    assert(!after.snapshot.valid&&after.status.resets==0&&after.status.intervals==previous_intervals);
    assert(after.snapshot.anchor_id==2&&
           after.snapshot.processed_position_seq==uint64_t(r.gps.call_sequence)*4);
    assert(after.snapshot.context.generation==r.gps.prediction_generation);
    assert(std::fabs(after.snapshot.latitude_deg-anchor.latitude_deg)<1e-10);
    assert(std::fabs(after.snapshot.longitude_deg-anchor.longitude_deg)<1e-10);
    assert(after.snapshot.derived_utc_ns==anchor.utc_ns);
}
static void drain_initial_anchor(ProductPipeline& pipeline,void* argument) {
    const A::Observation& gps=*static_cast<A::Observation*>(argument);
    assert(pipeline.drain(gps.mono_ns)==N::PIPELINE_OK);
    assert(pipeline.diagnostic(gps.mono_ns).snapshot.state==MX5_DR_READY);
}
static void short_gap(ProductPipeline& pipeline,void* argument) {
    const A::Observation& gap=*static_cast<A::Observation*>(argument);
    Step s=Step();s.start=1000000000ULL;s.end=gap.mono_ns;s.sequence=1;
    assert(pipeline.enqueue_speed(evidence(1,s,false),10)==N::PIPELINE_OK);
    assert(pipeline.enqueue_reverse(evidence(3,s,false),0)==N::PIPELINE_OK);
    assert(pipeline.enqueue_yaw(evidence(2,s,true),0,2047,1,s.start,s.end)==N::PIPELINE_OK);
    assert(pipeline.enqueue_position(gap)==N::PIPELINE_OK);
    assert(pipeline.drain(gap.mono_ns)==N::PIPELINE_OK);
    assert(pipeline.diagnostic(gap.mono_ns).snapshot.state==MX5_DR_ACTIVE);
}
static void short_reanchor(ProductPipeline& pipeline,void* argument) {
    const A::Observation& gps=*static_cast<A::Observation*>(argument);
    const mx5_dr_anchor anchor=authored_anchor(gps,2);
    assert(pipeline.enqueue_anchor(anchor,gps.mono_ns)==N::PIPELINE_OK);
    assert(pipeline.enqueue_position(gps)==N::PIPELINE_OK);
    assert(pipeline.drain(gps.mono_ns)==N::PIPELINE_OK);
    const N::Diagnostic d=pipeline.diagnostic(gps.mono_ns);
    assert(d.snapshot.state==MX5_DR_READY&&d.snapshot.anchor_id==2);
    assert(d.snapshot.context.generation==current_generation()&&d.status.resets==0);
    assert(d.snapshot.processed_position_seq==anchor.position_seq);
}
static void trajectory(Worker& worker,const char* name,uint64_t start,bool quality_change,
                       bool initialize=true,unsigned sequence_base=0) {
    if(initialize) { callback(1,start,false);worker.call(seed,&position); }
    if(quality_change) {
        callback(2,start+5000000,false);worker.call(enqueue_position,&position);
        if(!std::strcmp(name,"quality_cycle")) {
            callback(1,start+6000000,false);worker.call(enqueue_position,&position);
        }
    }
    callback(0,start+10000000,false);worker.call(enqueue_position,&position);
    const bool reverse=!std::strcmp(name,"reverse"),turn=reverse||!std::strcmp(name,"turn");
    const bool verified=std::strcmp(name,"unverified")!=0;
    Step step=Step();
    for(unsigned i=0;i<10;++i) {
        step=Step();step.start=start+uint64_t(i)*100000000;step.end=step.start+100000000;
        step.now=step.end+20000000;step.sequence=sequence_base+i+1;step.yaw=turn?0.2:0;
        step.reverse=reverse;step.verified=verified;now_ns=step.now;worker.call(calculate,&step);
        if(verified&&(!step.published||step.result!=R::CORE_BRIDGE_OK)) {
            std::fprintf(stderr,"publication: core_generation=%llu adapter_generation=%u bridge=%u core=%u\n",
                (unsigned long long)step.diagnostic.snapshot.context.generation,current_generation(),
                unsigned(step.result),unsigned(step.diagnostic.result));
            assert(step.published&&step.result==R::CORE_BRIDGE_OK);
        }
        if(!verified)assert(!step.published&&step.result==R::CORE_BRIDGE_UNQUALIFIED);
        callback(0,step.end+40000000,verified);
        if(verified) {
            assert(get64(sent)==step.mapped.derived_utc_ns);
            assert(get32(sent+36)==10000&&sent[32]==1&&sent[40]==1);
            assert(std::fabs(double(get32(sent+8))/1e7-step.mapped.latitude_deg)<0.000000051);
            assert(std::fabs(double(get32(sent+12))/1e7-step.mapped.longitude_deg)<0.000000051);
        }
        if(i!=9)worker.call(enqueue_position,&position);
    }
    // Independent constant-turn displacement, not a copy of the integrator.
    const double sign=reverse?-1:1;
    const double east=turn?sign*50*(1-std::cos(0.2)):0;
    const double north=turn?sign*50*std::sin(0.2):10;
    assert(std::fabs(step.diagnostic.snapshot.accumulated_east_m-east)<0.0001);
    assert(std::fabs(step.diagnostic.snapshot.accumulated_north_m-north)<0.0001);
    assert(step.diagnostic.status.resets==0&&step.diagnostic.status.intervals>=10);
    if(!initialize) {
        assert(step.diagnostic.status.intervals>previous_intervals);
        assert(step.diagnostic.snapshot.solution_seq>previous_solution);
    }
    previous_intervals=step.diagnostic.status.intervals;
    previous_solution=step.diagnostic.snapshot.solution_seq;
    last_publication=step.mapped;
    if(verified&&!std::strcmp(name,"expiry")) {
        callback(0,step.mapped.valid_until_mono_ns,true);
        callback(0,step.mapped.valid_until_mono_ns+1,false);assert(selected.reason==A::EXPIRED);
    }
}
int main(int argc,char** argv) {
    assert(argc==2);
    const char* const cases[]={"straight","quality_gap","quality_cycle","turn","reverse","expiry",
        "reacquire","native_return","stale_control","fault_recovery","core_reject",
        "reinit","failed_reinit","failed_model_reinit","owner_exit","unverified","continuous_reacquire",
        "anchor_first_reacquire","separate_reacquire","native_reacquire","quality_reacquire",
        "single_gap_reacquire","ready_quality_reanchor"};
    bool known=false;for(unsigned i=0;i<sizeof cases/sizeof cases[0];++i)known|=!std::strcmp(argv[1],cases[i]);
    assert(known);
#ifdef MX5_ASSIST_DSO_TEST
    initialize_assist_test_dso();
#endif
    for(unsigned i=0;i<48;++i)original[i]=uint8_t(i+1);
    A::Options options=A::Options();options.clock=clock_fn;options.provenance=provenance;
    options.sink=observe;options.allow_assist=true;options.max_snapshot_age_ns=150000000;
    assert(A::configure(endpoint,options)&&A::set_mode(A::ASSIST)); // synthetic only
    if(!std::strcmp(argv[1],"owner_exit")) {
        { Worker owner;trajectory(owner,"straight",1000000000ULL,false); }
        callback(0,2050000000ULL,false);assert(selected.reason==A::EPOCH_MISMATCH);
        std::printf("PASS assist publication %s: %u sends, %u replacements; authored inputs, product path\n",
            argv[1],sends,replacements);
        return 0;
    }
    Worker worker;
    if(!std::strcmp(argv[1],"single_gap_reacquire")||
       !std::strcmp(argv[1],"ready_quality_reanchor")) {
        callback(1,1000000000ULL,false);worker.call(seed,&position);
        worker.call(drain_initial_anchor,&position);
        if(!std::strcmp(argv[1],"single_gap_reacquire")) {
            callback(0,1010000000ULL,false);worker.call(short_gap,&position);
            callback(1,1020000000ULL,false);
        } else callback(2,1010000000ULL,false);
        worker.call(short_reanchor,&position);
        std::printf("PASS assist publication %s: %u sends, %u replacements; authored inputs, product path\n",
            argv[1],sends,replacements);
        return 0;
    }
    const bool quality=!std::strcmp(argv[1],"quality_gap")||!std::strcmp(argv[1],"quality_cycle");
    trajectory(worker,argv[1],1000000000ULL,quality);
    if(!std::strcmp(argv[1],"fault_recovery")) {
        worker.call(fault_after_publication,0);
        // The candidate was still within its lease. A same-mode OEM send must
        // use the original immediately after the worker fault revokes it.
        callback(0,2050000000ULL,false);assert(selected.reason==A::EPOCH_MISMATCH);
        worker.call(reject_return,&position);
        callback(3,2060000000ULL,false);worker.call(reject_return,&position);
        callback(1,2070000000ULL,false);worker.call(recover_anchor_after_fault,&position);
        callback(0,2080000000ULL,false);worker.call(enqueue_position,&position);
        Step recovered=Step();recovered.start=2070000000ULL;recovered.end=2170000000ULL;
        recovered.now=2190000000ULL;recovered.sequence=1;recovered.verified=true;
        now_ns=recovered.now;worker.call(calculate,&recovered);
        assert(recovered.published&&recovered.result==R::CORE_BRIDGE_OK);
        callback(0,2190000000ULL,true);
        std::printf("PASS assist publication %s: %u sends, %u replacements; authored inputs, product path\n",
            argv[1],sends,replacements);
        return 0;
    }
    if(!std::strcmp(argv[1],"core_reject")) {
        worker.call(reject_core_after_publication,0);
        callback(0,2050000000ULL,false);assert(selected.reason==A::EPOCH_MISMATCH);
        std::printf("PASS assist publication %s: %u sends, %u replacements; authored inputs, product path\n",
            argv[1],sends,replacements);
        return 0;
    }
    if(!std::strcmp(argv[1],"reinit")||!std::strcmp(argv[1],"failed_reinit")||
       !std::strcmp(argv[1],"failed_model_reinit")) {
        worker.call(!std::strcmp(argv[1],"reinit")?reinit_after_publication:
            !std::strcmp(argv[1],"failed_reinit")?failed_reinit_after_publication:
            failed_model_reinit_after_publication,0);
        callback(0,2050000000ULL,false);assert(selected.reason==A::EPOCH_MISMATCH);
        std::printf("PASS assist publication %s: %u sends, %u replacements; authored inputs, product path\n",
            argv[1],sends,replacements);
        return 0;
    }
    if(!std::strcmp(argv[1],"reacquire")) {
        callback(1,2050000000ULL,false);
        worker.call(reinit_after_publication,0);
        callback(1,2200000000ULL,false);
        worker.call(reseed_after_reinit,&position);
        previous_intervals=previous_solution=0;
        trajectory(worker,"straight",2200000000ULL,true,false);
        std::printf("PASS assist publication %s: %u sends, %u replacements; authored inputs, product path\n",
            argv[1],sends,replacements);
        return 0;
    }
    const bool expired=!std::strcmp(argv[1],"expiry");
    // Except in the expiry case, the previous lease still covers both sends:
    // only real GPS/native revocation can suppress the old prediction here.
    uint64_t returned=expired?2080000000ULL:2050000000ULL;
    const bool native_reacquire=!std::strcmp(argv[1],"native_reacquire");
    const bool quality_reacquire=!std::strcmp(argv[1],"quality_reacquire");
    const bool continuous=!std::strcmp(argv[1],"continuous_reacquire")||
        !std::strcmp(argv[1],"anchor_first_reacquire")||!std::strcmp(argv[1],"separate_reacquire")||
        native_reacquire||quality_reacquire;
    if(native_reacquire) {
        callback(3,returned,false);worker.call(reject_return,&position);returned+=10000000;
    }
    if(continuous) { input_latitude=37.0002;input_longitude=127.0003;input_utc=1700000002ULL; }
    callback(!std::strcmp(argv[1],"native_return")?3:quality_reacquire?2:1,returned,false);
    if(std::strcmp(argv[1],"unverified"))worker.call(reject_old_publication,&last_publication);
    if(continuous) {
        Reanchor r={position,!std::strcmp(argv[1],"anchor_first_reacquire"),!std::strcmp(argv[1],"separate_reacquire")};
        worker.call(continuous_anchor,&r);
        trajectory(worker,"straight",returned,true,false,10);
    } else {
        worker.call(!std::strcmp(argv[1],"stale_control")?reject_stale_control:reject_return,&position);
        callback(0,returned+10000000,false);assert(selected.reason==A::EPOCH_MISMATCH);
    }
    std::printf("PASS assist publication %s: %u sends, %u replacements; authored inputs, product path\n",
                argv[1],sends,replacements);
}
