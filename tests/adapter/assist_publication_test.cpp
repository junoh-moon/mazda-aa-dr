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
    now_ns=when;uint8_t raw[72]={};put32(raw,uint32_t(mode));put64(raw+8,1700000000ULL);
    const double lat=37,lon=127;std::memcpy(raw+16,&lat,8);std::memcpy(raw+24,&lon,8);
    A::VehicleData data={1,original,48};borrowed=&data;expect_original=!replacement;
    const unsigned before=sends;errno=EDOM;
    A::position_enter(0,raw);assert(errno==EDOM);
    assert(A::send_vehicle_data(&sends,&data)==-731&&errno==ERANGE);
    A::position_leave();assert(errno==ERANGE&&sends==before+1);
    assert(selected.choice==(replacement?A::DR_REPLACEMENT:A::ORIGINAL));
    if(replacement)++replacements;else assert(!std::memcmp(sent,original,48));
    for(unsigned i=0;i<48;++i)assert(original[i]==uint8_t(i+1));
}
static void seed(ProductPipeline& pipeline,void* argument) {
    const A::Observation& gps=*static_cast<A::Observation*>(argument);
    const mx5_dr_context context={11,12,gps.prediction_generation};
    assert(pipeline.init_qualified(mx5_dr_default_config(),context));
    mx5_dr_anchor anchor=mx5_dr_anchor();anchor.context=context;
    anchor.anchor_id=anchor.position_seq=1;anchor.measured_ns=gps.mono_ns;
    anchor.utc_ns=1700000000000000000ULL;
    anchor.latitude_deg=gps.position.latitude_deg;anchor.longitude_deg=gps.position.longitude_deg;
    anchor.position_error_m=1;anchor.heading_error_rad=0.01;
    anchor.validated=anchor.heading_valid=anchor.calibration_verified=1;anchor.quality=MX5_DR_VALID;
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
static void reject_old_publication(ProductPipeline&,void* argument) {
    assert(!A::publish_snapshot(*static_cast<A::DrSnapshot*>(argument)));
}
static void trajectory(Worker& worker,const char* name,uint64_t start,bool quality_change) {
    callback(1,start,false);worker.call(seed,&position);
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
        step.now=step.end+20000000;step.sequence=i+1;step.yaw=turn?0.2:0;
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
    last_publication=step.mapped;
    if(verified&&!std::strcmp(name,"expiry")) {
        callback(0,step.mapped.valid_until_mono_ns,true);
        callback(0,step.mapped.valid_until_mono_ns+1,false);assert(selected.reason==A::EXPIRED);
    }
}
int main(int argc,char** argv) {
    assert(argc==2);
    const char* const cases[]={"straight","quality_gap","quality_cycle","turn","reverse","expiry",
        "reacquire","native_return","stale_control","unverified"};
    bool known=false;for(unsigned i=0;i<sizeof cases/sizeof cases[0];++i)known|=!std::strcmp(argv[1],cases[i]);
    assert(known);
#ifdef MX5_ASSIST_DSO_TEST
    initialize_assist_test_dso();
#endif
    for(unsigned i=0;i<48;++i)original[i]=uint8_t(i+1);
    A::Options options=A::Options();options.clock=clock_fn;options.provenance=provenance;
    options.sink=observe;options.allow_assist=true;options.max_snapshot_age_ns=150000000;
    assert(A::configure(endpoint,options)&&A::set_mode(A::ASSIST)); // synthetic only
    Worker worker;const bool quality=!std::strcmp(argv[1],"quality_gap")||!std::strcmp(argv[1],"quality_cycle");
    trajectory(worker,argv[1],1000000000ULL,quality);
    const bool expired=!std::strcmp(argv[1],"expiry");
    // Except in the expiry case, the previous lease still covers both sends:
    // only real GPS/native revocation can suppress the old prediction here.
    const uint64_t returned=expired?2080000000ULL:2050000000ULL;
    callback(!std::strcmp(argv[1],"native_return")?3:1,returned,false);
    if(std::strcmp(argv[1],"unverified"))worker.call(reject_old_publication,&last_publication);
    worker.call(!std::strcmp(argv[1],"stale_control")?reject_stale_control:reject_return,&position);
    callback(0,returned+10000000,false);assert(selected.reason==A::EPOCH_MISMATCH);
    if(!std::strcmp(argv[1],"reacquire"))trajectory(worker,"straight",2200000000ULL,true);
    std::printf("PASS assist publication %s: %u sends, %u replacements; authored inputs, product path\n",
                argv[1],sends,replacements);
}
