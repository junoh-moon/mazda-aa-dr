#include "adapter/session_hooks.h"
#include "adapter/adapter.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <atomic>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>

namespace A=mx5::adapter;
namespace S=mx5::runtime::session_trace;
#ifdef MX5_SESSION_DSO_TEST
#include "session_dso_access.h"
#endif
static A::SessionCallbacks supplied, received[80];
static const A::SessionCallbacks* retained[80];
static void* users[80];
static void* handle;
static unsigned creates, destroys, notifications;
static int32_t create_result, destroy_result;
static bool early, throw_create, throw_destroy, throw_status, reenter_destroy, reenter_create;
static void* expected_info;
static void* expected_user;
static unsigned char expected_payload[2400];
static size_t expected_size;
static std::atomic<unsigned> block_create(0), block_callback(0), block_destroy(0);
static bool cancel_create, cancel_destroy, cancel_status;
static bool predict_in_create, predict_in_destroy, predict_in_status;
static void prediction_attempt(bool replacement,bool publish);
static bool null_success;
enum ConcurrentCase { SERIAL, OUTPUT_RACE, LATE_DESTROY, DISTINCT_STORAGE };
static ConcurrentCase concurrent_case;
static pthread_mutex_t api_mu=PTHREAD_MUTEX_INITIALIZER;
static std::atomic<unsigned> create_ready(0),destroy_entered(0),allow_destroy(0);
// Use the target's pthread ABI. GCC 4.9 std::thread's internal implementation
// is not compatible with the stock shared C++ runtime (even without our DSO).
template<class Call> static void* thread_body(void* arg) {
    (*static_cast<Call*>(arg))();return 0;
}
template<class Call> static pthread_t start_thread(Call& call) {
    pthread_t thread;assert(!pthread_create(&thread,0,thread_body<Call>,&call));return thread;
}
static void join_thread(pthread_t thread) { assert(!pthread_join(thread,0)); }
static void cancellation(std::atomic<unsigned>& flag) {
    flag.store(1);
    for(;;) { pthread_testcancel();sched_yield(); }
}
static void status(void* user,void* info) {
    assert(errno==EDOM);
    assert(user==expected_user && info==expected_info);
    assert(!expected_size || !memcmp(info,expected_payload,expected_size));
    ++notifications;
    if(predict_in_status)prediction_attempt(false,true);
    errno=ERANGE;
    if(cancel_status)cancellation(block_callback);
    if(throw_status)throw 37;
}
static int32_t create(const char* xml,void* user,const A::SessionCallbacks* cb,void** storage) {
    assert(errno==EDOM && !strcmp(xml,"original.xml"));
    const unsigned index=creates++;
    assert(cb && storage && index<80);
    received[index]=*cb;retained[index]=cb;users[index]=user;
    if(predict_in_create)prediction_attempt(false,true);
    for(unsigned i=0;i<19;++i)if(i!=1)assert(cb->entry[i]==supplied.entry[i]);
    if(early) {
        int32_t info[2]={-7,-1};expected_info=info;expected_user=user;
        expected_size=sizeof info;memcpy(expected_payload,info,expected_size);
        reinterpret_cast<A::SessionStatus>(cb->entry[1])(user,info);
        assert(errno==ERANGE && !memcmp(info,expected_payload,expected_size));
        assert(A::read_issue_session().result==S::TRANSITION);
    }
    if(cancel_create)cancellation(block_create);
    if(block_create.load()) {
        block_create.store(2);
        while(block_create.load()!=3)sched_yield();
    }
    if(throw_create) { errno=ERANGE;throw 23; }
    if(reenter_destroy) {
        errno=EDOM;
        assert(mx5_session_destroy(storage)==destroy_result);
    }
    if(concurrent_case!=SERIAL)assert(!pthread_mutex_lock(&api_mu));
    if(!create_result && !null_success)*storage=&handle;
    if(concurrent_case!=SERIAL) {
        assert(!pthread_mutex_unlock(&api_mu));
        create_ready.store(1);
        if(concurrent_case==OUTPUT_RACE) {
            while(!destroy_entered.load())sched_yield();
            // No happens-before edge from destroy's later write to our return.
            // TSan must detect an added wrapper read, even when it happens later.
            usleep(20000);
        }
    }
    errno=ERANGE;
    return create_result;
}
static int32_t destroy(void** storage) {
    assert(errno==EDOM);++destroys;
    if(predict_in_destroy)prediction_attempt(false,true);
    assert(A::read_issue_session().result==(A::session_hook_health().faults?S::FAULT:S::TRANSITION));
    if(cancel_destroy)cancellation(block_destroy);
    if(throw_destroy) { errno=ERANGE;throw 31; }
    if(reenter_create) {
        assert(mx5_session_create("original.xml",0,&supplied,storage)==create_result);
        assert(errno==ERANGE);
    }
    if(concurrent_case!=SERIAL) {
        destroy_entered.store(1);
        if(concurrent_case!=OUTPUT_RACE)while(!allow_destroy.load())sched_yield();
        assert(!pthread_mutex_lock(&api_mu));
    }
    if(!destroy_result)*storage=0;
    if(concurrent_case!=SERIAL) {
        assert(!pthread_mutex_unlock(&api_mu));
        if(concurrent_case==OUTPUT_RACE)while(!allow_destroy.load())sched_yield();
    }
    errno=ERANGE;return destroy_result;
}
static void prepare() {
    for(unsigned i=0;i<19;++i)supplied.entry[i]=0x1000+16*i;
    supplied.entry[1]=reinterpret_cast<uintptr_t>(status);
    A::SessionBindings b={create,destroy,status};
    assert(A::prepare_session_hooks(b) && "session observation not implemented");
    assert(!A::prepare_session_hooks(b));
    assert(A::session_hook_health().prepared);
}
static S::Snapshot send(const void* storage) {
    S::Snapshot out;errno=EDOM;A::read_send_session(storage,&out,0);assert(errno==EDOM);
    return out;
}
static S::Snapshot open(void** storage,void* user=0) {
    errno=EDOM;assert(mx5_session_create("original.xml",user,&supplied,storage)==create_result);
    assert(errno==ERANGE);return A::read_issue_session();
}
static void close(void** storage) {
    errno=EDOM;assert(mx5_session_destroy(storage)==destroy_result && errno==ERANGE);
}
static void notify(unsigned index,int32_t state,void* user=0) {
    int32_t full[600]={};full[0]=state;full[1]=-1;full[599]=77;
    expected_info=full;expected_user=user;errno=EDOM;
    expected_size=sizeof full;memcpy(expected_payload,full,expected_size);
    reinterpret_cast<A::SessionStatus>(received[index].entry[1])(user,full);
    assert(errno==ERANGE && !memcmp(full,expected_payload,expected_size));
}
static void normal() {
    assert(A::read_issue_session().result==S::NONE);
    void* storage=0;early=true;
    S::Snapshot first=open(&storage);
    assert(first.result==S::OBSERVED && first.lifetime==1 && first.event==1);
    assert(first.state_known && first.state==-7 && !users[0]);
    assert(first.revision==2); // Early status callback plus create completion.
    assert(retained[0]!=&supplied && retained[0]->entry[1]==received[0].entry[1]);
    assert(send(&storage).lifetime==first.lifetime);
    assert(send(&handle).result==S::NONE && send(0).result==S::NONE);
    notify(0,0);assert(A::read_issue_session().state==0);
    assert(A::read_issue_session().event==2);
    assert(A::read_issue_session().revision==3 && send(&storage).revision==3);
    close(&storage);assert(!storage && A::read_issue_session().result==S::NONE);
    assert(A::read_issue_session().revision==4);
    early=false;S::Snapshot second=open(&storage);
    assert(second.result==S::OBSERVED && second.lifetime==2 && !second.state_known);
    assert(received[0].entry[1]!=received[1].entry[1]);
    notify(0,99); // Old NULL-userdata callback cannot acquire the new lifetime.
    assert(!A::read_issue_session().state_known && send(&storage).lifetime==2);
    assert(A::read_issue_session().revision==6); // Includes the late old callback.
    notify(1,3);assert(send(&storage).state==3 && send(&storage).event==1);
    close(&storage);assert(destroys==2 && creates==2 && notifications==4);
    assert(!A::session_hook_health().faults);
}
static void failure() {
    void* storage=0;create_result=260;early=true;
    assert(open(&storage).result==S::NONE && !storage);
    create_result=0;early=false;assert(open(&storage).lifetime==2);
    destroy_result=264;close(&storage);assert(storage);
    assert(A::read_issue_session().result==S::NONE && send(&storage).result==S::NONE);
    notify(1,2);assert(A::read_issue_session().result==S::NONE);
    assert(destroys==1 && creates==2 && notifications==2);
}
static void overlap() {
    void* storage=0;open(&storage);
    void* other=0;block_create.store(1);
    auto create_other=[&]() {open(&other);};
    pthread_t thread=start_thread(create_other);
    while(block_create.load()!=2)sched_yield();
    assert(send(&storage).result==S::TRANSITION);
    assert(A::read_issue_session().result==S::TRANSITION);
    block_create.store(3);join_thread(thread);
    assert(A::read_issue_session().result==S::AMBIGUOUS);
    assert(send(&storage).lifetime==1 && send(&other).lifetime==2);
    close(&storage);assert(A::read_issue_session().lifetime==2);close(&other);
}
static void same_storage() {
    void* storage=0;open(&storage);open(&storage);
    assert(send(&storage).result==S::AMBIGUOUS);
    close(&storage);assert(send(&storage).result==S::NONE);
}
static void closing_create() {
    void* storage=0;reenter_destroy=true;open(&storage);
    assert(A::read_issue_session().result==S::FAULT && send(&storage).result==S::FAULT);
    assert(A::session_hook_health().faults==A::SESSION_CONTENTION);
}
static void creating_during_destroy() {
    void* storage=0;open(&storage);reenter_create=true;close(&storage);
    assert(!storage && creates==2 && destroys==1);
    assert(A::read_issue_session().result==S::FAULT && send(&storage).result==S::FAULT);
    assert(A::session_hook_health().faults==A::SESSION_CONTENTION);
    notify(1,7);assert(notifications==1); // Faults must never suppress forwarding.
}
static void successful_create_is_not_handle_validation() {
    void* storage=0;null_success=true;
    const S::Snapshot observed=open(&storage);
    // This authored API returns 0 without providing a handle. The wrapper
    // records that return; it must not inspect or validate OEM-owned storage.
    assert(!storage && observed.result==S::OBSERVED && observed.lifetime==1);
    assert(!observed.state_known && !A::session_hook_health().faults);
    close(&storage);assert(send(&storage).result==S::NONE);
}
static void concurrent_lifecycle(const char* which) {
    concurrent_case=!strcmp(which,"output_race")?OUTPUT_RACE:
        (!strcmp(which,"late_destroy")?LATE_DESTROY:DISTINCT_STORAGE);
    void* storage=0;void* other=0;
    auto create_call=[&]() {open(&storage);};
    auto destroy_call=[&]() {
        while(!create_ready.load())sched_yield();
        close(&storage);
    };
    if(concurrent_case==OUTPUT_RACE) {
        pthread_t creator=start_thread(create_call),destroyer=start_thread(destroy_call);
        join_thread(creator);allow_destroy.store(1);join_thread(destroyer);
        assert(creates==1 && destroys==1);
    } else {
        open(&storage);
        pthread_t destroyer=start_thread(destroy_call);
        while(!destroy_entered.load())sched_yield();
        open(concurrent_case==DISTINCT_STORAGE?&other:&storage);
        allow_destroy.store(1);join_thread(destroyer);
        assert(creates==2 && destroys==1);
        if(concurrent_case==DISTINCT_STORAGE)assert(other==&handle);
    }
    assert(!storage && A::read_issue_session().result==S::FAULT);
    assert(send(&storage).result==S::FAULT);
    assert(A::session_hook_health().faults==A::SESSION_CONTENTION);
}
static void capacity() {
    void* storage=0;
    for(unsigned i=0;i<A::SESSION_CONTEXT_CAPACITY;++i) { assert(open(&storage).lifetime==i+1);close(&storage); }
    assert(open(&storage).result==S::FAULT);
    assert(received[64].entry[1]==supplied.entry[1]);
    notify(0,7); // Old wrappers still forward even after the observation budget.
    assert(A::session_hook_health().faults&A::SESSION_CAPACITY);
    assert(A::session_hook_health().contexts==A::SESSION_CONTEXT_CAPACITY);
    close(&storage);assert(creates==65 && destroys==65 && notifications==1);
}
static void callback_bad() {
    void* storage=0;open(&storage);int user=3;
    notify(0,3,&user);assert(notifications==1);
    assert(A::read_issue_session().result==S::FAULT);
    assert(A::session_hook_health().faults&A::SESSION_CALLBACK);
}
static void callback_null() {
    void* storage=0;open(&storage);
    expected_info=0;expected_user=0;expected_size=0;errno=EDOM;
    reinterpret_cast<A::SessionStatus>(received[0].entry[1])(0,0);
    assert(errno==ERANGE && notifications==1);
    assert(A::read_issue_session().result==S::FAULT);
    assert(A::session_hook_health().faults==A::SESSION_CALLBACK);
}
static void readers() {
    void* storage=0;int user=7;open(&storage,&user);
    std::atomic<bool> done(false);
    std::atomic<unsigned> reads(0);
    auto observe=[&]() {
        unsigned local=0;
        do {
            const S::Snapshot s=A::read_issue_session();
            assert(s.result==S::OBSERVED || s.result==S::TRANSITION);
            if(s.result==S::OBSERVED) {
                assert(s.lifetime==1);
                if(s.state_known)assert(s.state>=0 && unsigned(s.state)+1==s.event);
            } else assert(!s.lifetime && !s.state_known);
            const S::Snapshot target=send(&storage);
            assert((target.result==S::OBSERVED && target.lifetime==1) ||
                   (target.result==S::TRANSITION && !target.lifetime));++local;
        }while(!done.load());
        reads.fetch_add(local);
    };
    pthread_t first=start_thread(observe),second=start_thread(observe);
    for(int i=0;i<5000;++i)notify(0,i,&user);
    done.store(true);join_thread(first);join_thread(second);
    assert(notifications==5000 && reads.load()>=2);
    const S::Snapshot last=A::read_issue_session();
    assert(last.event==5000 && last.state==4999 && !A::session_hook_health().faults);
    close(&storage);
    printf("SESSION_STATUS_READS=%u\n",reads.load());
}
static void exceptions(const char* which) {
    void* storage=0;
    if(!strcmp(which,"throw_create")) {
        throw_create=true;try { open(&storage);assert(false); }catch(int e) { assert(e==23 && errno==ERANGE); }
    } else {
        open(&storage);
        if(!strcmp(which,"throw_destroy")) {
            throw_destroy=true;try { close(&storage);assert(false); }catch(int e) {assert(e==31 && errno==ERANGE);}
        } else {
            throw_status=true;try {notify(0,0);assert(false);}catch(int e){assert(e==37 && errno==ERANGE);}
        }
    }
    assert(A::read_issue_session().result==S::FAULT);
    assert(A::session_hook_health().faults&A::SESSION_UNWIND);
}
static void cancellations(const char* which) {
    void* storage=0;
    const bool creating=!strcmp(which,"cancel_create"), destroying=!strcmp(which,"cancel_destroy");
    if(!creating)open(&storage);
    cancel_create=creating;cancel_destroy=destroying;cancel_status=!creating&&!destroying;
    std::atomic<unsigned>& flag=creating?block_create:(destroying?block_destroy:block_callback);
    auto call=[&]() { if(creating)open(&storage);else if(destroying)close(&storage);else notify(0,1); };
    pthread_t thread=start_thread(call);
    while(flag.load()!=1)sched_yield();
    assert(!pthread_cancel(thread));join_thread(thread);
    assert(A::read_issue_session().result==S::FAULT);
    assert(A::session_hook_health().faults&A::SESSION_UNWIND);
}

// Authored qualification only. Exercise the real adapter's candidate selection
// through the real lifecycle wrappers; no phone, sensor or OEM code is used.
static const uint64_t prediction_time=1000000000;
static void** prediction_storage;
static A::Observation prediction_event;
static A::DrSnapshot in_flight_candidate;
static unsigned prediction_sends;
static unsigned char prediction_input[72],prediction_payload[48],prediction_sent[48];
static A::VehicleData* prediction_original;
static bool prediction_original_forwarded;
static uint64_t prediction_clock(void*) { return prediction_time; }
static bool prediction_provenance(void*,const A::PositionInput*,A::Provenance* p,void*) {
    p->source_epoch=11;p->session_epoch=12;
    p->exact_request=p->verified_lds=p->legacy_receiver=true;return true;
}
static void prediction_sink(const A::Observation* o,void*) {
    if(o->kind==A::Observation::SEND)prediction_event=*o;
}
static int32_t prediction_next(void* storage,A::VehicleData* data) {
    assert(storage==prediction_storage && errno==EDOM);
    assert(data && data->payload && data->length==48);++prediction_sends;
    prediction_original_forwarded=data==prediction_original;
    memcpy(prediction_sent,data->payload,48);errno=EINPROGRESS;return -713;
}
static A::DrSnapshot prediction_candidate() {
    A::DrSnapshot s=A::DrSnapshot();
    s.source_epoch=11;s.session_epoch=12;s.prediction_generation=A::generation();
    s.frontier_mono_ns=prediction_time;s.valid_until_mono_ns=prediction_time+100000000;
    s.derived_utc_ns=1700000000000000000ULL;s.latitude_deg=35;s.longitude_deg=135;
    s.speed_mps=10;s.travel_bearing_deg=90;
    s.ready=s.profile_verified=s.input_quality_verified=s.limits_ok=true;return s;
}
static bool publish_prediction(const A::DrSnapshot& s) {
    // Keep publication on a distinct worker, as required by the adapter API.
    // The authored OEM call can remain in flight while that worker publishes.
    bool accepted=false;
    auto publish=[&]() { accepted=A::publish_snapshot(s); };
    const pthread_t worker=start_thread(publish);join_thread(worker);return accepted;
}
static void prediction_send(bool replacement) {
    const int saved_errno=errno;
    A::VehicleData data={1,prediction_payload,48};prediction_original=&data;
    const unsigned before=prediction_sends;errno=EDOM;
    assert(A::send_vehicle_data(prediction_storage,&data)==-713 && errno==EINPROGRESS);
    assert(prediction_sends==before+1);
    assert(prediction_event.choice==(replacement?A::DR_REPLACEMENT:A::ORIGINAL));
    assert(prediction_original_forwarded==!replacement);
    if(!replacement)assert(!memcmp(prediction_sent,prediction_payload,48));
    for(unsigned i=0;i<48;++i)assert(prediction_payload[i]==i+1);
    errno=saved_errno;
}
static void prediction_attempt(bool replacement,bool publish) {
    const int saved_errno=errno;
    A::position_enter(0,prediction_input);
    if(publish) {
        in_flight_candidate=prediction_candidate();
        assert(publish_prediction(in_flight_candidate));
    }
    prediction_send(replacement);A::position_leave();errno=saved_errno;
}
static void prediction_lifecycle(const char* which) {
    A::Options options=A::Options();options.clock=prediction_clock;
    options.sink=prediction_sink;options.provenance=prediction_provenance;
    options.allow_assist=true;options.max_snapshot_age_ns=150000000;
    options.session_reader=A::read_send_session;
    assert(A::configure(prediction_next,options) && A::set_mode(A::ASSIST));
    for(unsigned i=0;i<48;++i)prediction_payload[i]=i+1;
    void* storage=0;prediction_storage=&storage;open(&storage);
    prediction_attempt(true,true); // Positive control before each boundary.
    const A::DrSnapshot before=prediction_candidate();
    const bool in_flight=strstr(which,"_inflight")!=0;
    if(!in_flight)A::position_enter(0,prediction_input);
    if(!strcmp(which,"prediction_destroy") || !strcmp(which,"prediction_recreate"))close(&storage);
    else if(!strcmp(which,"prediction_create_failure")) { create_result=264;open(&storage);create_result=0; }
    else if(!strcmp(which,"prediction_destroy_failure")) { destroy_result=264;close(&storage);destroy_result=0; }
    else if(!strcmp(which,"prediction_status"))notify(0,7);
    else if(!strcmp(which,"prediction_create_inflight")) {
        close(&storage);predict_in_create=true;open(&storage);predict_in_create=false;
    } else if(!strcmp(which,"prediction_destroy_inflight")) {
        predict_in_destroy=true;close(&storage);predict_in_destroy=false;
    } else if(!strcmp(which,"prediction_status_inflight")) {
        predict_in_status=true;notify(0,7);predict_in_status=false;
    } else assert(false);
    if(!strcmp(which,"prediction_recreate"))open(&storage);
    assert(!publish_prediction(before) && "pre-transition prediction was not revoked");
    if(in_flight)assert(!publish_prediction(in_flight_candidate) && "in-flight prediction survived completion");
    else { prediction_send(false);A::position_leave(); }
    if(A::read_issue_session().result==S::NONE)open(&storage);
    prediction_attempt(false,false); // A new POSITION alone cannot relabel it.
    prediction_attempt(true,true);   // A freshly computed candidate can recover.
    close(&storage);assert(!A::session_hook_health().faults);
}
int main(int argc,char** argv) {
    alarm(30);
    assert(argc==2);
#if defined(__APPLE__)
    // Darwin's pthread cancellation does not unwind these C++ scopes. This
    // Linux-target contract is exercised by the GNU host and ARM DSO suites.
    if(!strcmp(argv[1],"cancel_create") || !strcmp(argv[1],"cancel_destroy") ||
            !strcmp(argv[1],"cancel_status")) {
        puts("SKIP session cancellation: requires Linux C++ forced unwind");
        return 77;
    }
#endif
#ifdef MX5_SESSION_DSO_TEST
    initialize_session_test_dso();
#endif
    prepare();
    const char* c=argv[1];
    if(!strcmp(c,"normal"))normal();
    else if(!strcmp(c,"failure"))failure();
    else if(!strcmp(c,"overlap"))overlap();
    else if(!strcmp(c,"same_storage"))same_storage();
    else if(!strcmp(c,"closing_create"))closing_create();
    else if(!strcmp(c,"creating_during_destroy"))creating_during_destroy();
    else if(!strcmp(c,"null_success"))successful_create_is_not_handle_validation();
    else if(!strcmp(c,"output_race") || !strcmp(c,"late_destroy") ||
            !strcmp(c,"distinct_storage"))concurrent_lifecycle(c);
    else if(!strcmp(c,"capacity"))capacity();
    else if(!strcmp(c,"callback_bad"))callback_bad();
    else if(!strcmp(c,"callback_null"))callback_null();
    else if(!strcmp(c,"readers"))readers();
    else if(!strncmp(c,"throw_",6))exceptions(c);
    else if(!strncmp(c,"cancel_",7))cancellations(c);
    else if(!strncmp(c,"prediction_",11))prediction_lifecycle(c);
    else assert(false);
    printf("PASS session wrappers %s: exact forwarding and lifetime observation\n",c);
}
