// Independent public-authored fixture. No OEM inputs or target execution.
#include "../../src/runtime/runtime.cpp"
#include <assert.h>
#include <cmath>
#include <cstdio>
#include <string>
namespace S=mx5::runtime::session_trace;
static int handle;
static const A::SessionCallbacks* wrapped;
static int32_t send_fixture(void*,A::VehicleData*) { return 0; }
static void status_fixture(void*,void*) {}
static int32_t create_fixture(const char*,void*,const A::SessionCallbacks* callbacks,void** out) {
    wrapped=callbacks;*out=&handle;return 0;
}
static int32_t destroy_fixture(void** out) { *out=0;return 0; }
static uint64_t T(unsigned ms) { return 1000000000ULL+uint64_t(ms)*1000000ULL; }
static N::RawEvent raw(N::SensorKind kind,unsigned ms,unsigned wheel=13600,unsigned yaw=2047) {
    N::RawEvent r=N::RawEvent();r.kind=kind;r.epoch=1;r.receive_seq=ms+1;r.received_ns=T(ms);r.count=1;
    for(unsigned i=0;i<4;++i)r.raw[i]=kind==N::YAW?yaw:wheel;
    return r;
}
static A::Observation fix(unsigned ms,double scale=1.03,int mode=1) {
    A::Observation o=A::Observation();o.kind=A::Observation::POSITION;o.mono_ns=T(ms);o.position.mode=mode;
    o.position.utc_seconds=1700000000+ms/1000;o.position.latitude_deg=double(ms)*0.01*scale/111320;
    o.position.longitude_deg=135;o.position.heading_deg=0;o.position.velocity_kmh=36*scale;return o;
}
static void feed(N::Pipeline& p,unsigned ms,unsigned wheel=13600,unsigned yaw=2067) {
    assert(p.enqueue_raw(raw(N::WHEELS,ms,wheel,yaw))==N::PIPELINE_OK);
    assert(p.enqueue_raw(raw(N::REVERSE,ms,wheel,yaw))==N::PIPELINE_OK);
    N::PipelineResult r=p.enqueue_raw(raw(N::YAW,ms,wheel,yaw));
    assert(r==N::PIPELINE_OK||r==N::PIPELINE_WAITING);
}
static void feed_holdout(N::GpsHoldout& h,unsigned ms,unsigned wheel=13600,unsigned yaw=2047) {
    for(unsigned k=1;k<=3;++k) {
        N::PipelineResult r=h.enqueue_raw(raw(static_cast<N::SensorKind>(k),ms,wheel,yaw));
        assert(r==N::PIPELINE_OK||r==N::PIPELINE_WAITING);
    }
}
static void calibration_reset(Journal& j) {
    N::Pipeline nav;N::GpsHoldout hold;mx5_dr_context c={1,1,1};
    assert(nav.init_model(N::research_model_profile(),mx5_dr_default_config(),c,true,true));
    assert(hold.init_model(N::research_model_profile(),mx5_dr_default_config(),c));
    mx5::runtime::ModelSession session;
    sync_model_session(j,session,nav,hold);assert(session.available());
    for(unsigned ms=0;ms<=3400;ms+=100) {
        feed(nav,ms,10000);assert(nav.drain(T(ms)-nav.reorder_ns())==N::PIPELINE_OK);
    }
    assert(nav.calibration().candidate_ready);
    for(unsigned ms=3500;ms<=18600;ms+=100) {
        feed(nav,ms);
        if((ms-3500)%1000==0)assert(nav.enqueue_position(fix(ms))==N::PIPELINE_OK);
        assert(nav.drain(T(ms)-nav.reorder_ns())==N::PIPELINE_OK);
    }
    printf("pre-reset applied zero=%.17g version=%llu scale=%.17g version=%llu\n",
        nav.calibration().active_zero,(unsigned long long)nav.calibration().calibration_version,
        nav.wheel_calibration().active_scale,(unsigned long long)nav.wheel_calibration().calibration_version);
    assert(nav.calibration().active_zero==2067&&nav.calibration().calibration_version==1);
    assert(std::fabs(nav.wheel_calibration().active_scale-1.03)<1e-9&&nav.wheel_calibration().calibration_version==1);
    bool learned_holdout=false;
    for(unsigned ms=0;ms<=47000;ms+=100) {
        feed_holdout(hold,ms,ms<=3400?10000:13600,2067);
        if(ms>=3500&&(ms-3500)%1000==0)
            assert(hold.enqueue_position(fix(ms,1.03))==N::PIPELINE_OK);
        hold.drain(T(ms)-nav.reorder_ns());
        N::HoldoutResult before;
        while(hold.pop(&before)) {
            if(before.event==N::HOLDOUT_BEGIN&&before.calibration_version>0&&before.wheel_scale_version>0) {
                assert(before.applied_yaw_zero==2067&&std::fabs(before.applied_wheel_scale-1.03)<1e-9);
                learned_holdout=true;
            }
        }
    }
    assert(learned_holdout&&hold.phase()==N::HOLDOUT_RUNNING);
    puts("pre-reset holdout BEGIN used learned zero=2067 and scale=1.03 after a completed first window and cooldown training");
    assert(hold.enqueue_position(fix(48000,1.03))==N::PIPELINE_OK);
    const uint64_t epoch=nav.context().session_epoch;
    int32_t info[2]={0,0};reinterpret_cast<A::SessionStatus>(wrapped->entry[1])(0,info);
    sync_model_session(j,session,nav,hold);
    assert(nav.context().session_epoch==epoch+1);
    assert(nav.calibration().active_zero==2047&&!nav.calibration().calibration_version&&!nav.calibration().candidate_ready);
    assert(nav.wheel_calibration().active_scale==1&&!nav.wheel_calibration().calibration_version&&!nav.wheel_calibration().candidate_ready);
    assert(hold.phase()==N::HOLDOUT_WARMUP);
    assert(!nav.diagnostic(T(48100)).snapshot.model_valid);
    hold.drain(T(48100));
    N::HoldoutResult output;assert(!hold.pop(&output)); // sync drains old outputs + reset abort itself
    // First new fix alone cannot recover; ordinary GAP must not revive old anchor.
    for(unsigned ms=49000;ms<=49300;ms+=100) {
        feed(nav,ms);
        if(ms==49000)assert(nav.enqueue_position(fix(ms))==N::PIPELINE_OK);
        if(ms==49100)assert(nav.enqueue_position(fix(ms,1.03,0))==N::PIPELINE_OK);
        assert(nav.drain(T(ms)-nav.reorder_ns())==N::PIPELINE_OK);
    }
    assert(!nav.diagnostic(T(49300)).snapshot.model_valid);
    bool recovered_holdout=false;
    for(unsigned ms=49000;ms<=50300;ms+=100) {
        feed_holdout(hold,ms);
        if(ms==49000||ms==50100)assert(hold.enqueue_position(fix(ms,1.0))==N::PIPELINE_OK);
        hold.drain(T(ms)-nav.reorder_ns());
        N::HoldoutResult after;
        while(hold.pop(&after))if(after.event==N::HOLDOUT_BEGIN) {
            assert(after.applied_yaw_zero==2047&&!after.calibration_version);
            assert(after.applied_wheel_scale==1&&!after.wheel_scale_version);
            recovered_holdout=true;
        }
    }
    assert(recovered_holdout);
    puts("session reset: both pipelines' learned calibrations cleared; queued holdout removed; fresh holdout recovered with nominal values");

}
int main() {
    alarm(20);
    A::Options options=A::Options();assert(A::configure(send_fixture,options));
    const A::SessionBindings bindings={create_fixture,destroy_fixture,status_fixture};
    assert(A::prepare_session_hooks(bindings));
    A::SessionCallbacks callbacks=A::SessionCallbacks();
    callbacks.entry[1]=reinterpret_cast<uintptr_t>(status_fixture);
    void* storage=0;assert(!mx5_session_create("authored",0,&callbacks,&storage));
    char root[]="/tmp/mx5dr-model-reset-XXXXXX";assert(mkdtemp(root));
    const std::string logs=std::string(root)+"/logs";assert(!mkdir(logs.c_str(),0700));
    config.max_log_bytes=8388608;
    { Journal j(root);calibration_reset(j);j.flush();assert(!j.failed); }
    assert(!mx5_session_destroy(&storage) && !A::session_hook_health().faults);
    assert(!unlink((logs+"/trace.0.jsonl").c_str()));
    assert(!rmdir(logs.c_str()) && !rmdir(root));
    alarm(0);
}
