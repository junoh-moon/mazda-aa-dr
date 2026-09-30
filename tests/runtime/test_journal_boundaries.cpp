#include <atomic>
#include <assert.h>
#include <errno.h>
#include <fstream>
#include <iterator>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#ifdef NDEBUG
#error Journal boundary checks require assertions
#endif
extern "C" {
void jq_reset();const volatile void* jq_address(unsigned);
void jq_push(unsigned);bool jq_pop(unsigned*);void jq_close();
bool jq_drained();bool jq_lost();unsigned jq_audit();uint64_t jq_dropped();bool jq_finish(const char*,bool);
uint64_t __real___atomic_fetch_add_8(volatile void*,uint64_t,int);
uint64_t __real___atomic_fetch_sub_8(volatile void*,uint64_t,int);
void __real___atomic_store_1(volatile void*,unsigned char,int);
}
static std::atomic<unsigned> paused(0),resume(0);
static thread_local bool scheduled;
static const volatile void* watched;
static unsigned watched_phase,watched_operation;
static void checkpoint(unsigned operation,unsigned phase,volatile void* address) {
    if(!scheduled||address!=watched||phase!=watched_phase||operation!=watched_operation)return;
    paused.store(1);while(!resume.load())sched_yield();
}
extern "C" uint64_t __wrap___atomic_fetch_add_8(volatile void* p,uint64_t n,int order) {
    checkpoint(1,1,p);const uint64_t result=__real___atomic_fetch_add_8(p,n,order);
    checkpoint(1,2,p);return result;
}
extern "C" uint64_t __wrap___atomic_fetch_sub_8(volatile void* p,uint64_t n,int order) {
    checkpoint(3,1,p);const uint64_t result=__real___atomic_fetch_sub_8(p,n,order);
    checkpoint(3,2,p);return result;
}
extern "C" void __wrap___atomic_store_1(volatile void* p,unsigned char n,int order) {
    checkpoint(2,1,p);__real___atomic_store_1(p,n,order);checkpoint(2,2,p);
}
static void* emit(void*) { scheduled=true;errno=EDOM;jq_push(99);assert(errno==EDOM);return 0; }
static pthread_t start(unsigned address,unsigned phase,unsigned operation=0) {
    watched=jq_address(address);watched_phase=phase;paused.store(0);resume.store(0);
    watched_operation=operation?operation:(address==0?1:2);
    pthread_t t;assert(!pthread_create(&t,0,emit,0));
    while(!paused.load())sched_yield();
    return t;
}
static void finish(pthread_t t) { resume.store(1);assert(!pthread_join(t,0));watched=0; }
static void delayed_ticket() {
    jq_reset();pthread_t t=start(0,2);unsigned read;
    for(unsigned i=0;i<1025;++i) { jq_push(i);assert(jq_pop(&read)&&read==i); }
    assert(!jq_pop(&read)&&!jq_lost());
    jq_close();assert(!jq_drained());finish(t);
    assert(jq_pop(&read)&&read==99&&jq_drained()&&!jq_lost());
    puts("PASS actual capacity reservation delayed across multiple ticket/slot laps");
}
static void full_before_loss() {
    jq_reset();for(unsigned i=0;i<256;++i)jq_push(i);
    // Pause immediately BEFORE the real sticky-loss store. Returning the
    // failed reservation early would allow a false drained() result here.
    pthread_t t=start(2,1);jq_close();unsigned read;
    for(unsigned i=0;i<256;++i)assert(jq_pop(&read)&&read==i);
    assert(!jq_drained()&&!jq_lost()&&!jq_dropped());finish(t);
    assert(jq_drained()&&jq_lost()&&jq_dropped()==1);
    puts("PASS actual FULL loss is published before final reservation return");
}
static void published_before_return() {
    jq_reset();pthread_t t=start(3,2);jq_close();unsigned read;
    assert(jq_pop(&read)&&read==99&&jq_drained()&&!jq_lost());finish(t);
    assert(jq_drained()&&!jq_dropped());
    puts("PASS actual ready publication permits final drain before producer returns");
}
static void close_before_reservation() {
    jq_reset();pthread_t t=start(0,1);jq_close();assert(jq_drained());finish(t);
    unsigned read;assert(jq_drained()&&!jq_pop(&read)&&!jq_lost()&&!jq_dropped());
    puts("PASS close between early check and actual reservation rejects the straddling push");
}
static void pending_capture() {
    char root[]="/tmp/mx5dr-journal-close-XXXXXX";assert(mkdtemp(root));
    const std::string logs=std::string(root)+"/logs",ack=logs+"/capture.done";
    assert(!mkdir(logs.c_str(),0700));
    jq_reset();pthread_t t=start(0,2);jq_close();
    assert(!jq_finish(root,false)&&access(ack.c_str(),F_OK)!=0);
    assert(!jq_finish(root,true)&&access(ack.c_str(),F_OK)!=0);
    {
        std::ifstream f((logs+"/trace.0.jsonl").c_str());
        const std::string text((std::istreambuf_iterator<char>(f)),std::istreambuf_iterator<char>());
        assert(text.find("capture_incomplete")!=std::string::npos);
        assert(text.find("capture_end")==std::string::npos);
    }
    finish(t);unsigned read;assert(jq_pop(&read)&&read==99&&jq_drained());
    // Separate successful quiescent session; do not turn the failed session
    // above into a completed capture by retrying its acknowledgement.
    jq_reset();jq_push(3);jq_close();assert(jq_finish(root,true));
    assert(access(ack.c_str(),F_OK)==0);
    unlink(ack.c_str());
    for(unsigned i=0;i<3;++i)unlink((logs+"/trace."+char('0'+i)+".jsonl").c_str());
    assert(!rmdir(logs.c_str())&&!rmdir(root));
    puts("PASS real runtime pending-reservation stop is incomplete/no-ack; closed drained session completes");
}
static void late_sink_failure() {
    char root[]="/tmp/mx5dr-journal-loss-XXXXXX";assert(mkdtemp(root));
    const std::string logs=std::string(root)+"/logs",ack=logs+"/capture.done";
    assert(!mkdir(logs.c_str(),0700));
    jq_reset();for(unsigned i=0;i<256;++i)jq_push(i);
    pthread_t t=start(0,2,3); // Actual failed reservation returned; sink has not disabled mutation.
    assert(jq_lost()&&jq_dropped()==1&&!jq_audit());
    jq_close();assert(jq_finish(root,true));
    assert(jq_drained()&&jq_audit()==1&&access(ack.c_str(),F_OK)==0);
    {
        std::ifstream f((logs+"/trace.0.jsonl").c_str());
        const std::string text((std::istreambuf_iterator<char>(f)),std::istreambuf_iterator<char>());
        assert(text.find("\"audit_fault\":1")!=std::string::npos);
    }
    finish(t);assert(jq_drained()&&jq_dropped()==1&&jq_audit()==1);
    unlink(ack.c_str());
    for(unsigned i=0;i<3;++i)unlink((logs+"/trace."+char('0'+i)+".jsonl").c_str());
    assert(!rmdir(logs.c_str())&&!rmdir(root));
    puts("PASS final health reconciles sticky queue loss before the delayed sink fault call");
}
int main() {
    alarm(45);unsigned early;
    assert(jq_pop(&early)&&early==77&&!jq_pop(&early));
    puts("PASS early preload observation survives later dynamic constructor phase");
    delayed_ticket();full_before_loss();published_before_return();
    close_before_reservation();pending_capture();late_sink_failure();alarm(0);
}
