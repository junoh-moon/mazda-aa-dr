#include "adapter/session_hooks.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <atomic>
#include <pthread.h>
#include <sched.h>

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
static bool early, throw_create, throw_destroy, throw_status, reenter_destroy;
static void* expected_info;
static void* expected_user;
static std::atomic<unsigned> block_create(0), block_callback(0), block_destroy(0);
static bool cancel_create, cancel_destroy, cancel_status;
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
    ++notifications;
    errno=ERANGE;
    if(cancel_status)cancellation(block_callback);
    if(throw_status)throw 37;
}
static int32_t create(const char* xml,void* user,const A::SessionCallbacks* cb,void** storage) {
    assert(errno==EDOM && !strcmp(xml,"original.xml"));
    const unsigned index=creates++;
    assert(cb && storage && index<80);
    received[index]=*cb;retained[index]=cb;users[index]=user;
    for(unsigned i=0;i<19;++i)if(i!=1)assert(cb->entry[i]==supplied.entry[i]);
    if(early) {
        int32_t info[2]={-7,-1};expected_info=info;expected_user=user;
        reinterpret_cast<A::SessionStatus>(cb->entry[1])(user,info);
        assert(errno==ERANGE);
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
    if(!create_result)*storage=&handle;
    errno=ERANGE;
    return create_result;
}
static int32_t destroy(void** storage) {
    assert(errno==EDOM);++destroys;
    assert(A::read_issue_session().result==(A::session_hook_health().faults?S::FAULT:S::TRANSITION));
    if(cancel_destroy)cancellation(block_destroy);
    if(throw_destroy) { errno=ERANGE;throw 31; }
    if(!destroy_result)*storage=0;
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
    reinterpret_cast<A::SessionStatus>(received[index].entry[1])(user,full);
    assert(errno==ERANGE && full[599]==77);
}
static void normal() {
    assert(A::read_issue_session().result==S::NONE);
    void* storage=0;early=true;
    S::Snapshot first=open(&storage);
    assert(first.result==S::OBSERVED && first.lifetime==1 && first.event==1);
    assert(first.state_known && first.state==-7 && !users[0]);
    assert(retained[0]!=&supplied && retained[0]->entry[1]==received[0].entry[1]);
    assert(send(&storage).lifetime==first.lifetime);
    assert(send(&handle).result==S::NONE && send(0).result==S::NONE);
    notify(0,0);assert(A::read_issue_session().state==0);
    assert(A::read_issue_session().event==2);
    close(&storage);assert(!storage && A::read_issue_session().result==S::NONE);
    early=false;S::Snapshot second=open(&storage);
    assert(second.result==S::OBSERVED && second.lifetime==2 && !second.state_known);
    assert(received[0].entry[1]!=received[1].entry[1]);
    notify(0,99); // Old NULL-userdata callback cannot acquire the new lifetime.
    assert(!A::read_issue_session().state_known && send(&storage).lifetime==2);
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
    assert(A::read_issue_session().result==S::NONE && send(&storage).result==S::NONE);
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
static void readers() {
    void* storage=0;int user=7;open(&storage,&user);
    std::atomic<bool> done(false);
    std::atomic<unsigned> reads(0);
    auto observe=[&]() {
        unsigned local=0;
        do {
            const S::Snapshot s=A::read_issue_session();
            assert(s.result==S::OBSERVED && s.lifetime==1);
            if(s.state_known)assert(s.state>=0 && unsigned(s.state)+1==s.event);
            assert(send(&storage).lifetime==1);++local;
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
int main(int argc,char** argv) {
#ifdef MX5_SESSION_DSO_TEST
    initialize_session_test_dso();
#endif
    assert(argc==2);prepare();
    const char* c=argv[1];
    if(!strcmp(c,"normal"))normal();
    else if(!strcmp(c,"failure"))failure();
    else if(!strcmp(c,"overlap"))overlap();
    else if(!strcmp(c,"same_storage"))same_storage();
    else if(!strcmp(c,"closing_create"))closing_create();
    else if(!strcmp(c,"capacity"))capacity();
    else if(!strcmp(c,"callback_bad"))callback_bad();
    else if(!strcmp(c,"readers"))readers();
    else if(!strncmp(c,"throw_",6))exceptions(c);
    else if(!strncmp(c,"cancel_",7))cancellations(c);
    else assert(false);
    printf("PASS session wrappers %s: exact forwarding and lifetime observation\n",c);
}
