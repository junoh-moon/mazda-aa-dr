// Hold 64 concurrent POSITION calls, then verify that a 65th keeps the OEM
// contract, records lost observation, revokes prediction and frees no owner.
#include "adapter/adapter.h"
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <unistd.h>

#ifdef NDEBUG
#error Context pool regressions require assertions
#endif
#ifdef MX5_CONTEXT_POOL_DSO_TEST
#include "context_pool_dso_access.h"
#endif

namespace A=mx5::adapter;
namespace {
enum { ACTIVE_SLOTS=64, THREADS=ACTIVE_SLOTS+1 };
pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
unsigned arrived;
bool released;
unsigned char raw[72];
std::atomic<unsigned> sends(0), positions(0), send_events(0);
std::atomic<unsigned> unavailable_positions(0), unavailable_sends(0);
std::atomic<unsigned> trace_reads(0);
static __thread uint64_t thread_request_id;
static __thread uint32_t failed_call,failed_generation;
static __thread int32_t failed_mode;

int32_t original(void*,A::VehicleData* data) {
    assert(data&&data->type==1&&data->payload);
    assert(data->length==(failed_call?47U:48U));
    assert(errno==ERANGE);
    sends.fetch_add(1,std::memory_order_relaxed);
    errno=EAGAIN;return -37;
}
uint64_t clock_now(void*) { return 1; }
bool unqualified(void*,const A::PositionContext&,A::Provenance*,void*) {
    return false;
}
mx5::runtime::request_trace::Result read_request(
        const void*,mx5::runtime::request_trace::Trace* trace,void*) {
    thread_request_id=trace_reads.fetch_add(1,std::memory_order_relaxed)+1;
    trace->request.id=thread_request_id;
    return mx5::runtime::request_trace::OK;
}
void observe(const A::Observation* event,void*) {
    if(event->kind==A::Observation::POSITION) {
        assert(event->request_result==mx5::runtime::request_trace::OK);
        assert(event->request_trace.request.id==thread_request_id);
        positions.fetch_add(1,std::memory_order_relaxed);
        if(event->reason==A::CONTEXT_UNAVAILABLE) {
            failed_call=event->call_sequence;
            failed_generation=event->prediction_generation;
            failed_mode=event->original_mode;
            unavailable_positions.fetch_add(1,std::memory_order_relaxed);
        }
    } else {
        assert(event->kind==A::Observation::SEND&&event->choice==A::ORIGINAL);
        if(event->reason==A::CONTEXT_UNAVAILABLE) {
            assert(event->length==47 && !event->has_payload);
            assert(event->request_result==mx5::runtime::request_trace::NOT_FOUND);
            assert(!event->request_trace.request.id);
            assert(event->call_sequence==failed_call);
            assert(event->prediction_generation==failed_generation);
            assert(event->original_mode==failed_mode);
        } else {
            assert(event->request_result==mx5::runtime::request_trace::OK);
            assert(event->request_trace.request.id==thread_request_id);
        }
        send_events.fetch_add(1,std::memory_order_relaxed);
        if(event->reason==A::CONTEXT_UNAVAILABLE)
            unavailable_sends.fetch_add(1,std::memory_order_relaxed);
    }
}
void* worker(void*) {
    unsigned char bytes[48]={0};
    A::VehicleData data={1,bytes,48};
    errno=EDOM;
    A::position_enter(0,raw);
    assert(errno==EDOM);
    assert(!pthread_mutex_lock(&gate));
    ++arrived;
    assert(!pthread_cond_broadcast(&changed));
    while(!released)assert(!pthread_cond_wait(&changed,&gate));
    assert(!pthread_mutex_unlock(&gate));
    if(failed_call)data.length=47; // Failed context still forwards malformed OEM data.
    errno=ERANGE;
    assert(A::send_vehicle_data(0,&data)==-37&&errno==EAGAIN);
    A::position_leave();
    assert(errno==EAGAIN);
    return 0;
}
void await_arrival(unsigned target) {
    assert(!pthread_mutex_lock(&gate));
    while(arrived<target)assert(!pthread_cond_wait(&changed,&gate));
    assert(!pthread_mutex_unlock(&gate));
}
}
int main(int argc,char** argv) {
    assert(argc==2&&argv[1][0]=='s');
    alarm(50);
#ifdef MX5_CONTEXT_POOL_DSO_TEST
    initialize_context_pool_test_dso();
#endif
    A::Options options=A::Options();
    options.sink=observe;options.clock=clock_now;
    options.provenance=unqualified;options.max_snapshot_age_ns=1;
    options.allow_assist=true;
    options.request_reader=read_request;
    assert(A::configure(original,options)&&A::set_mode(A::OBSERVE));
    A::DrSnapshot candidate=A::DrSnapshot();
    candidate.prediction_generation=A::generation();
    assert(A::publish_snapshot(candidate));
    pthread_t threads[THREADS];
    for(unsigned i=0;i<ACTIVE_SLOTS;++i)
        assert(!pthread_create(&threads[i],0,worker,0));
    await_arrival(ACTIVE_SLOTS);
    assert(unavailable_positions.load(std::memory_order_relaxed)==0);
    assert(!pthread_create(&threads[ACTIVE_SLOTS],0,worker,0));
    await_arrival(THREADS);
    assert(unavailable_positions.load(std::memory_order_relaxed)==1);
    candidate.prediction_generation=A::generation();
    assert(!A::publish_snapshot(candidate));
    assert(!pthread_mutex_lock(&gate));
    released=true;assert(!pthread_cond_broadcast(&changed));
    assert(!pthread_mutex_unlock(&gate));
    for(unsigned i=0;i<THREADS;++i)
        assert(!pthread_join(threads[i],0));
    assert(sends.load(std::memory_order_relaxed)==THREADS);
    assert(positions.load(std::memory_order_relaxed)==THREADS);
    assert(send_events.load(std::memory_order_relaxed)==THREADS);
    assert(unavailable_positions.load(std::memory_order_relaxed)==1);
    assert(unavailable_sends.load(std::memory_order_relaxed)==1);
    // Reuse after every owner has returned, while the sticky fault remains.
    assert(!pthread_create(&threads[0],0,worker,0));
    assert(!pthread_join(threads[0],0));
    assert(sends.load(std::memory_order_relaxed)==THREADS+1);
    assert(trace_reads.load(std::memory_order_relaxed)==THREADS+1);
    assert(unavailable_positions.load(std::memory_order_relaxed)==1);
    assert(unavailable_sends.load(std::memory_order_relaxed)==1);
    std::printf("PASS context pool capacity %s: original forwarding, fault, reuse\n",argv[1]);
}
