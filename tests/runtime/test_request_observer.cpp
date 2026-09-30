#include "runtime/request_observer.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <thread>

namespace R = mx5::runtime::request_trace;
struct Reply {
    int type, serial_result;
    uint32_t serial;
    const char* sender;
    const char* error;
};
struct Method { Reply* reply; };
static void* reply(void* p) { errno=EIO; return static_cast<Method*>(p)->reply; }
static int type(void* p) { errno=EIO; return static_cast<Reply*>(p)->type; }
static const char* sender(void* p) { errno=EIO; return static_cast<Reply*>(p)->sender; }
static const char* error(void* p) { errno=EIO; return static_cast<Reply*>(p)->error; }
static int serial(void* p, uint32_t* out) {
    errno=EIO; Reply& r=*static_cast<Reply*>(p); *out=r.serial; return r.serial_result;
}
static uint64_t clock_fn(void* p) { errno=EIO; return *static_cast<uint64_t*>(p); }
static R::ReplyApi api() {
    R::ReplyApi a={reply,type,sender,error,serial}; return a;
}
static void empty(R::Observer& o) {
    R::Status s; assert(o.status(&s)==R::OK);
    assert(!s.requests && !s.workers && !s.loss_reasons);
}
static void metadata() {
    uint64_t time=11;
    R::Observer o(api(),clock_fn,&time);
    char name[] = ":1.23", message[] = "org.example.Error";
    Reply r={2,-1,987,name,message}; Method m={&r};
    int worker=0, position=0;
    R::Token request, work;
    errno=EDOM;
    assert(o.request_begin(&m,&request)==R::OK && errno==EDOM);
    time=21;
    {
        R::ReplyScope scope(o,&m);
        assert(scope.result()==R::OK && scope.token().id==request.id);
        assert(errno==EDOM);
        assert(o.worker_post(&worker,&position,&work)==R::OK);
    }
    assert(errno==EDOM);
    assert(o.request_end(&m)==R::OK);
    memset(name,'x',sizeof name-1); memset(message,'x',sizeof message-1);
    R::Trace t;
    {
        R::WorkerScope scope(o,&worker);
        assert(scope.result()==R::OK);
        assert(o.position_take(&position,&t)==R::OK);
        assert(t.request.id==request.id && t.worker.id==work.id);
        assert(t.issue.observed_ns==11 && t.reply.observed_ns==21);
        assert(!t.issue.known && !t.issue.bus_lifetime && !t.issue.session_lifetime);
        assert(t.reply.type_known && t.reply.type==2);
        assert(!t.reply.wire_serial_known && !t.reply.wire_serial);
        assert(t.reply.sender.complete && !strcmp(t.reply.sender.bytes,":1.23"));
        assert(!strcmp(t.reply.error_name.bytes,"org.example.Error"));
        assert(o.position_take(&position,&t)==R::USED && !t.request.id);
    }
    assert(errno==EDOM);
    assert(o.worker_destroy(&worker)==R::NOT_FOUND);
    empty(o);
    puts("PASS observer owned metadata and failed serial getter");
}
static void nested() {
    R::Observer a(api()), b(api());
    Reply r={1,0,42,":1.2",0}; Method m={&r}, unknown={&r}, n={&r};
    int w=0,w2=0,p=0,p2=0; R::Token q,t;
    assert(a.request_begin(&m,&q)==R::OK);
    {
        R::ReplyScope outer(a,&m);
        {
            R::ReplyScope missing(a,&unknown);
            assert(a.worker_post(&w,&p,&t)==R::NOT_FOUND && !t.id);
        }
        assert(b.request_begin(&n,&t)==R::OK);
        {
            R::ReplyScope other(b,&n);
            assert(a.worker_post(&w,&p,&t)==R::NOT_FOUND);
            assert(b.worker_post(&w2,&p2,&t)==R::OK);
        }
        assert(a.worker_post(&w,&p,&t)==R::OK);
    }
    assert(a.request_end(&m)==R::OK && b.request_end(&n)==R::OK);
    R::Trace out;
    {
        R::WorkerScope outer(a,&w);
        {
            R::WorkerScope missing(a,&unknown);
            assert(a.position_take(&p,&out)==R::NOT_FOUND);
        }
        {
            R::WorkerScope other(b,&w2);
            assert(a.position_take(&p,&out)==R::NOT_FOUND);
            assert(b.position_take(&p2,&out)==R::OK);
        }
        assert(a.position_take(&p,&out)==R::OK && out.request.id==q.id);
        assert(out.reply.wire_serial_known && out.reply.wire_serial==42);
        assert(out.reply.type_known && out.reply.type==1);
    }
    assert(a.position_take(&p,&out)==R::NOT_FOUND && !out.request.id);
    empty(a);empty(b);
    puts("PASS observer nested missing and cross-instance scopes");
}
static void lifetime() {
    R::Observer o(api());
    Reply r={0,0,0,0,0}; Method m={&r};
    int w=0,p=0; R::Token first,next,work;
    assert(o.request_begin(&m,&first)==R::OK);
    assert(o.request_end(&m)==R::OK); // observed cancellation, no notify
    assert(o.request_begin(&m,&next)==R::OK && first.id!=next.id);
    {
        R::ReplyScope scope(o,&m);
        assert(o.worker_post(&w,&p,&work)==R::OK);
    }
    R::Status s;
    assert(o.status(&s)==R::OK && s.requests==1 && s.workers==1);
    assert(o.worker_destroy(&w)==R::OK); // queued work destroyed without doWork
    assert(o.request_end(&m)==R::OK);
    assert(o.request_begin(&m,&next)==R::OK);
    {
        R::ReplyScope scope(o,&m);
        assert(o.worker_post(&w,&p,&work)==R::OK);
    }
    assert(o.request_end(&m)==R::OK);
    {
        R::WorkerScope scope(o,&w); R::Trace t;
        assert(o.position_take(&p,&t)==R::OK);
        assert(!t.reply.type_known && !t.reply.wire_serial_known);
        assert(!t.reply.sender.known && !t.reply.error_name.known);
    }
    empty(o);
    puts("PASS observer cancellation, destruction and address reuse");
}
static void cross_thread() {
    R::Observer o(api());
    Reply r={1,0,7,":1.7",0}; Method m={&r};
    int w=0,p=0; R::Token q,t;
    assert(o.request_begin(&m,&q)==R::OK);
    {
        R::ReplyScope scope(o,&m);
        assert(o.worker_post(&w,&p,&t)==R::OK);
        // Work may start and finish while notify/submit has not returned.
        std::thread consumer([&]() {
            R::Trace out;
            assert(o.position_take(&p,&out)==R::NOT_FOUND);
            R::WorkerScope work(o,&w);
            assert(o.position_take(&m,&out)==R::WRONG_POSITION);
            assert(o.position_take(&p,&out)==R::USED && !out.request.id);
        });
        consumer.join();
    }
    assert(o.request_end(&m)==R::OK);
    empty(o);
    puts("PASS observer concurrent worker and single pointer-matched take");
}
static void invalid() {
    R::ReplyApi bad=R::ReplyApi(); R::Observer invalid(bad);
    Method m={0}; R::Token t={8,9}; R::Trace out;
    assert(!invalid.valid() && invalid.request_begin(&m,&t)==R::BAD_INPUT && !t.id);
    { R::ReplyScope scope(invalid,&m); assert(scope.result()==R::BAD_INPUT); }
    R::Observer valid(api());
    assert(valid.request_begin(&m,&t)==R::OK);
    int w=0,p=0;
    {
        R::ReplyScope scope(valid,&m);
        assert(scope.result()==R::OK);
        assert(valid.worker_post(&w,&p,&t)==R::OK);
    }
    { R::WorkerScope scope(valid,&w);
      assert(valid.position_take(&p,&out)==R::OK && !out.reply.type_known); }
    assert(valid.request_end(&m)==R::OK);empty(valid);
    puts("PASS observer invalid API and absent reply");
}
static void overlapping_replies() {
    R::Observer o(api());
    Reply replies[2]={{1,0,71,":1.71",0},{2,0,72,":1.72","error"}};
    Method methods[2]={{&replies[0]},{&replies[1]}};
    int workers[2]={},positions[2]={};R::Token requests[2],work[2];
    for(unsigned i=0;i<2;++i)assert(o.request_begin(&methods[i],&requests[i])==R::OK);
    std::atomic<unsigned> step(0);
    std::thread first([&]() {
        R::ReplyScope scope(o,&methods[0]);step.store(1);
        while(step.load()!=2)std::this_thread::yield();
        assert(o.worker_post(&workers[0],&positions[0],&work[0])==R::OK);
        step.store(3);
        while(step.load()!=4)std::this_thread::yield();
    });
    std::thread second([&]() {
        while(step.load()!=1)std::this_thread::yield();
        {
            R::ReplyScope scope(o,&methods[1]);step.store(2);
            while(step.load()!=3)std::this_thread::yield();
            assert(o.worker_post(&workers[1],&positions[1],&work[1])==R::OK);
        }
        step.store(4);
    });
    first.join();second.join();
    for(int i=1;i>=0;--i) {
        assert(o.request_end(&methods[i])==R::OK);
        R::WorkerScope scope(o,&workers[i]);R::Trace t;
        assert(o.position_take(&positions[i],&t)==R::OK);
        assert(t.request.id==requests[i].id && t.worker.id==work[i].id);
        assert(t.reply.wire_serial==unsigned(71+i));
    }
    empty(o);puts("PASS observer overlapping reply threads retain their own request");
}
static int throwing_type(void* p) {
    if(static_cast<Reply*>(p)->type==99)throw 29;
    return type(p);
}
static void failed_scope_construction() {
    R::ReplyApi functions=api();functions.get_type=throwing_type;
    R::Observer o(functions);
    Reply good={1,0,23,":1.23",0},bad={99,0,0,0,0};Method outer={&good},inner={&bad};
    R::Token q,t;int w=0,p=0;
    assert(o.request_begin(&outer,&q)==R::OK && o.request_begin(&inner,&t)==R::OK);
    for(unsigned nested_case=0;nested_case<2;++nested_case) {
        if(!nested_case) {
            bool caught=false;errno=EDOM;
            try { R::ReplyScope fail(o,&inner); } catch(int value) { caught=value==29; }
            assert(caught && errno==EDOM);
            assert(o.worker_post(&w,&p,&t)==R::NOT_FOUND); // Detects dangling failed scope.
        } else {
            R::ReplyScope enclosing(o,&outer);
            bool caught=false;
            try { R::ReplyScope fail(o,&inner); } catch(int value) { caught=value==29; }
            assert(caught && o.worker_post(&w,&p,&t)==R::OK); // Restores the actual outer scope.
        }
    }
    { R::WorkerScope scope(o,&w);R::Trace trace;
      assert(o.position_take(&p,&trace)==R::OK && trace.request.id==q.id); }
    assert(o.request_end(&outer)==R::OK && o.request_end(&inner)==R::OK);empty(o);
    puts("PASS observer constructor failure restores absent and outer reply scopes");
}
static void session_context_copy() {
    namespace S=mx5::runtime::session_trace;
    R::Observer o(api());Reply r={1,0,17,":1.7",0};Method m={&r};
    R::Token q,t;int w=0,p=0;
    S::Snapshot ambient={S::OBSERVED,41,5,-7,true,80};
    assert(o.request_begin(&m,&q,ambient)==R::OK);
    ambient.lifetime=42;ambient.event=6;ambient.state=3;ambient.revision=81;
    { R::ReplyScope scope(o,&m);assert(o.worker_post(&w,&p,&t)==R::OK); }
    assert(o.request_end(&m)==R::OK);
    { R::WorkerScope scope(o,&w);R::Trace trace;
      assert(o.position_take(&p,&trace)==R::OK);
      const S::Snapshot& copy=trace.issue.session_context;
      assert(copy.result==S::OBSERVED && copy.lifetime==41 && copy.event==5);
      assert(copy.state_known && copy.state==-7 && copy.revision==80);
      assert(!trace.issue.known && !trace.issue.session_lifetime); }
    empty(o);puts("PASS observer retains issue-time ambient session without ownership promotion");
}
int main() { metadata();nested();lifetime();cross_thread();invalid();overlapping_replies();failed_scope_construction();session_context_copy(); }
