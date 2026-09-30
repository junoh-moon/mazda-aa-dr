#include "runtime/journal_queue.h"
#include "adapter/adapter.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <unistd.h>

#ifdef NDEBUG
#error Journal queue checks require assertions
#endif
namespace R=mx5::runtime;
namespace mx5 { namespace runtime {
struct JournalQueueTestAccess {
    template<class T,unsigned N> static void wrap(JournalQueue<T,N>& q,uint32_t start) {
        q.ticket_.store(start);q.head_=start;
    }
};
} }
static std::atomic<unsigned> paused(0),resume_copy(0);
static thread_local bool pause_copy;
struct Value {
    uint32_t producer,sequence;
    uint64_t bytes[32];
    Value(unsigned p=0,unsigned s=0):producer(p),sequence(s) {
        for(unsigned i=0;i<32;++i)bytes[i]=(uint64_t(p)<<32)^s^i;
    }
    Value(const Value&)=default;
    Value& operator=(const Value& other) noexcept {
        producer=other.producer;sequence=other.sequence;
        for(unsigned i=0;i<32;++i)bytes[i]=other.bytes[i];
        if(pause_copy) {
            paused.store(1);
            while(!resume_copy.load())sched_yield();
        }
        return *this;
    }
    void check() const {
        for(unsigned i=0;i<32;++i)assert(bytes[i]==((uint64_t(producer)<<32)^sequence^i));
    }
};
typedef R::JournalQueue<Value,8> Queue;
struct Job { Queue* queue;bool push;Value value;bool ok; };
static void* copying(void* input) {
    Job& j=*static_cast<Job*>(input);pause_copy=true;errno=EDOM;
    j.ok=j.push?j.queue->push(j.value)==Queue::QUEUED:j.queue->pop(&j.value);
    assert(errno==EDOM);return 0;
}
static pthread_t start(Job& j) {
    paused.store(0);resume_copy.store(0);pthread_t t;
    assert(!pthread_create(&t,0,copying,&j));
    while(!paused.load())sched_yield();
    return t;
}
static void finish(pthread_t t,Job& j) {
    resume_copy.store(1);assert(!pthread_join(t,0)&&j.ok);
}
static void consumer_independence() {
    Queue q;assert(q.push(Value(1,1))==Queue::QUEUED);
    Job j={&q,false,Value(),false};pthread_t t=start(j);
    // The consumer owns the first slot and has not returned its credit.
    for(unsigned i=2;i<=8;++i)assert(q.push(Value(1,i))==Queue::QUEUED);
    assert(!q.dropped()&&!q.lost());
    // The held consumer slot is still occupied. Returning its credit before
    // copy completion would let this ninth item overwrite consumer storage.
    assert(q.push(Value(1,9))==Queue::FULL&&q.dropped()==1&&q.lost());
    finish(t,j);j.value.check();assert(j.value.sequence==1);
    for(unsigned i=2;i<=8;++i) {
        Value v;assert(q.pop(&v));v.check();assert(v.sequence==i);
    }
    Value v;assert(!q.pop(&v));
    puts("PASS paused consumer copy does not obstruct producers with available capacity");
}
static void unpublished_head_and_stop() {
    Queue q;Job j={&q,true,Value(1,1),false};pthread_t t=start(j);
    assert(q.push(Value(2,2))==Queue::QUEUED);
    Value v;assert(!q.pop(&v)); // Ready second slot cannot overtake the first.
    q.close();assert(q.closed()&&!q.drained());
    assert(q.push(Value(3,3))==Queue::STOPPED&&!q.dropped()&&!q.lost());
    finish(t,j);
    assert(q.pop(&v));v.check();assert(v.producer==1&&v.sequence==1);
    assert(q.pop(&v));v.check();assert(v.producer==2&&v.sequence==2);
    assert(!q.pop(&v)&&q.drained());
    puts("PASS delayed publication preserves FIFO and prevents premature close completion");
}
static void capacity_and_wrap() {
    Queue q;R::JournalQueueTestAccess::wrap(q,UINT32_MAX-2);
    for(unsigned lap=0;lap<4;++lap) {
        for(unsigned i=0;i<8;++i)assert(q.push(Value(9,lap*8+i))==Queue::QUEUED);
        assert(q.push(Value())==Queue::FULL&&q.dropped()==lap+1&&q.lost());
        for(unsigned i=0;i<8;++i) {
            Value v;assert(q.pop(&v));v.check();assert(v.sequence==lap*8+i);
        }
        Value v;assert(!q.pop(&v));
    }
    q.close();assert(q.drained());
    for(unsigned i=0;i<100;++i)assert(q.push(Value())==Queue::STOPPED);
    assert(q.drained()&&q.dropped()==4&&q.lost());
    puts("PASS exact capacity, natural uint32 ticket wrap, reclamation and stable stopped losses");
}
static void observation_ownership() {
    typedef R::JournalQueue<mx5::adapter::Observation,256> ProductQueue;
    constexpr ProductQueue constant_initialization_probe;
    (void)constant_initialization_probe;
    ProductQueue q;mx5::adapter::Observation source=mx5::adapter::Observation(),read;
    source.call_sequence=42;source.position.latitude_deg=37.5;
    source.request_trace.issue.observed_ns=123;source.request_trace.reply.sender.bytes[0]='x';
    source.original[47]=7;source.outgoing[47]=9;
    assert(q.push(source)==ProductQueue::QUEUED);
    source=mx5::adapter::Observation();
    assert(q.pop(&read));
    assert(read.call_sequence==42&&read.position.latitude_deg==37.5&&
           read.request_trace.issue.observed_ns==123&&read.request_trace.reply.sender.bytes[0]=='x'&&
           read.original[47]==7&&read.outgoing[47]==9);
    puts("PASS production Observation owns request, position and payload bytes");
}
typedef R::JournalQueue<Value,256> ConcurrentQueue;
static const unsigned ROUNDS=200,PER_PRODUCER=32,PRODUCERS=4;
// pthread_barrier_t is absent on Darwin. Synchronize only round boundaries;
// producers and consumer run concurrently without this lock within a round.
struct Barrier {
    pthread_mutex_t mutex;pthread_cond_t condition;unsigned arrivals,generation;
    Barrier():arrivals(0),generation(0) {
        assert(!pthread_mutex_init(&mutex,0)&&!pthread_cond_init(&condition,0));
    }
    ~Barrier() { assert(!pthread_cond_destroy(&condition)&&!pthread_mutex_destroy(&mutex)); }
    void wait() {
        assert(!pthread_mutex_lock(&mutex));const unsigned before=generation;
        if(++arrivals==PRODUCERS+1) {
            arrivals=0;++generation;assert(!pthread_cond_broadcast(&condition));
        } else while(generation==before)assert(!pthread_cond_wait(&condition,&mutex));
        assert(!pthread_mutex_unlock(&mutex));
    }
};
struct Producer { ConcurrentQueue* q;unsigned id;Barrier* barrier; };
static void* produce(void* p) {
    Producer& a=*static_cast<Producer*>(p);
    for(unsigned round=0;round<ROUNDS;++round) {
        a.barrier->wait();
        for(unsigned i=0;i<PER_PRODUCER;++i) {
            errno=EDOM;
            assert(a.q->push(Value(a.id,round*PER_PRODUCER+i))==ConcurrentQueue::QUEUED);
            assert(errno==EDOM);
        }
        a.barrier->wait();
    }
    return 0;
}
static void concurrent_laps() {
    ConcurrentQueue q;Barrier b;
    Producer args[PRODUCERS];pthread_t threads[PRODUCERS];unsigned expected[PRODUCERS]={};
    for(unsigned i=0;i<PRODUCERS;++i) {
        args[i]={&q,i,&b};assert(!pthread_create(&threads[i],0,produce,&args[i]));
    }
    for(unsigned round=0;round<ROUNDS;++round) {
        b.wait();
        for(unsigned n=0;n<PRODUCERS*PER_PRODUCER;) {
            Value v;
            if(!q.pop(&v)){sched_yield();continue;}
            v.check();assert(v.producer<PRODUCERS&&v.sequence==expected[v.producer]++);++n;
        }
        b.wait();
    }
    for(unsigned i=0;i<PRODUCERS;++i)assert(!pthread_join(threads[i],0));
    q.close();
    assert(q.drained()&&!q.dropped()&&!q.lost());
    printf("PASS %u concurrent owned copies across ring laps, exact per-producer order and no loss\n",
           ROUNDS*PER_PRODUCER*PRODUCERS);
}
int main() {
    alarm(60);
    consumer_independence();unpublished_head_and_stop();capacity_and_wrap();
    observation_ownership();concurrent_laps();
    alarm(0);
}
