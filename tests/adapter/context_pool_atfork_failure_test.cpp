// Source-linked registration failure control: --wrap=pthread_atfork injects
// ENOMEM before any OEM hook is installed. This is not an actual DSO symbol
// interposition or a claim of normal operation in an unregistered fork child.
#include "adapter/adapter.h"
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <unistd.h>

#ifdef NDEBUG
#error Registration fallback regressions require assertions
#endif
namespace A=mx5::adapter;
namespace R=mx5::runtime::request_trace;
namespace T=mx5::runtime::session_trace;
namespace {
unsigned registrations,requests,positions,sends,originals;
uint8_t raw[72],payload[48];
A::Observation position,sent;
A::VehicleData* borrowed;
uint64_t clock_fn(void*) {errno=EAGAIN;return 123;}
R::Result request(const void* p,R::Trace* out,void*) {
    assert(p==raw);++requests;*out=R::Trace();out->request=R::Token{12,13};
    out->worker=R::Token{14,15};errno=E2BIG;return R::OK;
}
void session(const void* p,T::Snapshot* out,void*) {
    assert(p==&originals);*out=T::Snapshot();errno=ENOTTY;
}
void sink(const A::Observation* o,void*) {
    if(o->kind==A::Observation::POSITION) {++positions;position=*o;}
    else {assert(o->kind==A::Observation::SEND);++sends;sent=*o;}
    errno=EIO;
}
int32_t original(void* storage,A::VehicleData* data) {
    assert(errno==EDOM&&storage==&originals&&data==borrowed);
    assert(data->payload==payload&&data->length==48&&data->type==1);
    for(unsigned i=0;i<48;++i)assert(payload[i]==uint8_t(i+1));
    ++originals;errno=ERANGE;return -77;
}
}
extern "C" int __wrap_pthread_atfork(void(*prepare)(),void(*parent)(),void(*child)()) {
    assert(!prepare&&!parent&&child);++registrations;errno=E2BIG;return ENOMEM;
}
int main() {
    alarm(10);A::Options options=A::Options();
    options.sink=sink;options.clock=clock_fn;options.request_reader=request;options.session_reader=session;
    errno=EDOM;assert(A::configure(original,options));
    assert(errno==EDOM&&registrations==1&&!A::faulted());
    for(unsigned i=0;i<48;++i)payload[i]=uint8_t(i+1);
    raw[8]=19; // Authored raw UTC sentinel; the remaining decoded values are zero.
    errno=EDOM;A::position_enter(0,raw);assert(errno==EDOM);
    A::VehicleData data={1,payload,48};borrowed=&data;
    errno=EDOM;assert(A::send_vehicle_data(&originals,&data)==-77&&errno==ERANGE);
    A::position_leave();assert(errno==ERANGE);
    assert(registrations==1&&requests==1&&positions==1&&sends==1&&originals==1);
    assert(position.position.utc_seconds==19&&position.call_sequence==sent.call_sequence);
    assert(position.prediction_generation==sent.prediction_generation);
    assert(position.request_result==R::OK&&sent.request_result==R::OK);
    assert(position.request_trace.request.id==12&&sent.request_trace.request.id==12);
    assert(position.request_trace.worker.epoch==15&&sent.request_trace.worker.epoch==15);
    assert(sent.choice==A::ORIGINAL&&sent.reason==A::DISABLED&&sent.result==-77);
    assert(sent.has_payload&&!std::memcmp(sent.original,payload,48)&&!std::memcmp(sent.outgoing,payload,48));
    assert(!sent.provenance.exact_request&&!sent.provenance.verified_lds&&!sent.provenance.legacy_receiver);
    assert(!A::faulted());alarm(0);
    std::puts("PASS context pool atfork failure: configure, errno, capture and original forwarding");
}
