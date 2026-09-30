// Authored targets only; no OEM ELF, object or D-Bus service is executed.
#include "adapter/request_hooks.h"
#include "adapter/session_hooks.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
namespace A=mx5::adapter;
namespace R=mx5::runtime::request_trace;
extern "C" void request_test_call4(void*,uint32_t*);
extern "C" void request_test_post();
extern "C" void request_test_work();
extern "C" void request_test_destroy();
extern "C" void mx5_request_post_enter(const void*);
#if defined(MX5_REQUEST_DSO_TEST)
#include "request_dso_access.h"
#endif
static int connection,method,context;
alignas(8) static unsigned char worker[88];
static uint32_t shared[2];
static unsigned submits,notifies,frees,posts,works,destroys,cleanups;
static uint64_t time_ns=100;
enum { NORMAL, UNRELATED, FAILED_SUBMIT, DESTROY_QUEUED, THROW_NOTIFY, THROW_WORK,
       CANCEL_NOTIFY, CANCEL_WORK, MALFORMED, OTHER_WORKER, DELAYED_CALLBACK,
       THROW_GETTER, CANCEL_GETTER, SESSION_TRANSITION };
static unsigned behavior;
static A::RequestNotify delayed_callback;
static R::Trace trace;
static void* session_storage;
static int session_handle;
static unsigned session_creates,session_destroys,session_callbacks;
static void session_status(void* user,void* info) {
    assert(!user && info);++session_callbacks;
}
static int32_t session_create(const char*,void* user,const A::SessionCallbacks* cb,void** storage) {
    assert(storage==&session_storage && !user);
    const int32_t state=++session_creates==1?11:22;
    int32_t info[2]={state,-1};
    reinterpret_cast<A::SessionStatus>(cb->entry[1])(user,info);
    *storage=&session_handle;return 0;
}
static int32_t session_destroy(void** storage) {
    assert(storage==&session_storage);++session_destroys;*storage=0;return 0;
}
static void open_session() {
    A::SessionCallbacks cb=A::SessionCallbacks();cb.entry[1]=reinterpret_cast<uintptr_t>(session_status);
    assert(mx5_session_create("authored.xml",0,&cb,&session_storage)==0);
}
static void cancel_now() {
    assert(!pthread_cancel(pthread_self()));pthread_testcancel();assert(false);
}
static void* reply(void* m) { assert(m==&method);errno=EIO;return &context; }
static int type(void* r) {
    assert(r==&context);
    if(behavior==THROW_GETTER)throw 19;
    if(behavior==CANCEL_GETTER)cancel_now();
    errno=EIO;return 2;
}
static const char* sender(void* r) { assert(r==&context);errno=EIO;return ":1.42"; }
static const char* error(void* r) { assert(r==&context);errno=EIO;return "org.example.Error"; }
static int serial(void* r,uint32_t* v) { assert(r==&context);*v=77;errno=EIO;return -1; }
static const char* destination(void* m) { assert(m==&method);errno=EIO;return "com.jci.lds.data"; }
static const char* path(void* m) { assert(m==&method);errno=EIO;return "/com/jci/lds/data"; }
static const char* member(void* m) { assert(m==&method);errno=EIO;return "GetPosition"; }
static uint64_t clock_fn(void*) { errno=EIO;return ++time_ns; }
static void empty() {
    A::RequestHookHealth h=A::request_hook_health();
    assert(h.prepared && h.result==R::OK && !h.ledger.requests && !h.ledger.workers && !h.ledger.loss_reasons);
}
static void no_scope() {
    R::Trace t;errno=EDOM;
    assert(A::read_request_trace(worker+8,&t,0)==R::NOT_FOUND && errno==EDOM && !t.request.id);
}
static void invoke(void (*entry)(),void* argument) {
    uint32_t registers[]={reinterpret_cast<uint32_t>(argument),0xabc1,0xabc2,0xabc3};
    errno=EAGAIN;request_test_call4(reinterpret_cast<void*>(entry),registers);
    assert(registers[0]==0x11 && registers[1]==0x22 && registers[2]==0x33 && registers[3]==0x44);
    assert(errno==ERANGE);
}
static void arguments(void* actual,void* expected,uint32_t b,uint32_t c,uint32_t d) {
    assert(actual==expected && b==0xabc1 && c==0xabc2 && d==0xabc3 && errno==EAGAIN);
}
extern "C" void request_test_post_body(void* a,uint32_t b,uint32_t c,uint32_t d) {
    arguments(a,shared,b,c,d);++posts;
    assert(shared[0]==reinterpret_cast<uint32_t>(worker) && shared[1]==0xdead1234);
    errno=ERANGE;
}
extern "C" void request_test_work_body(void* a,uint32_t b,uint32_t c,uint32_t d) {
    arguments(a,worker,b,c,d);++works;
    if(behavior==THROW_WORK)throw 31;
    if(behavior==CANCEL_WORK)cancel_now();
    const R::Result result=A::read_request_trace(worker+8,&trace,0);
    if(behavior==MALFORMED || behavior==OTHER_WORKER)assert(result==R::NOT_FOUND);
    else {
        assert(result==R::OK && trace.request.id && trace.worker.id);
        assert(trace.reply.type_known && trace.reply.type==2);
        assert(!trace.reply.wire_serial_known && !trace.reply.wire_serial);
        assert(!strcmp(trace.reply.sender.bytes,":1.42") && !strcmp(trace.reply.error_name.bytes,"org.example.Error"));
        assert(trace.issue.observed_ns==101 && trace.reply.observed_ns==102 && !trace.issue.known);
        assert(trace.issue.route.destination.complete && !strcmp(trace.issue.route.destination.bytes,"com.jci.lds.data"));
        assert(trace.issue.route.path.complete && !strcmp(trace.issue.route.path.bytes,"/com/jci/lds/data"));
        assert(trace.issue.route.interface_name.complete && !strcmp(trace.issue.route.interface_name.bytes,"com.jci.lds.data"));
        assert(trace.issue.route.member.complete && !strcmp(trace.issue.route.member.bytes,"GetPosition"));
        if(behavior==SESSION_TRANSITION) {
            // A delayed original notification must retain the issue context;
            // looking at the current global session here would return 2/22.
            const A::S::Snapshot old=trace.issue.session_context;
            A::S::Snapshot target;A::read_send_session(&session_storage,&target,0);
            assert(old.result==A::S::OBSERVED && old.lifetime==1);
            assert(old.state_known && old.event==1 && old.state==11);
            assert(target.result==A::S::OBSERVED && target.lifetime==2);
            assert(target.state_known && target.event==1 && target.state==22);
            assert(!trace.issue.session_lifetime && !trace.issue.session_event);
        }
        R::Trace second;assert(A::read_request_trace(worker+8,&second,0)==R::USED);
    }
    errno=ERANGE;
}
extern "C" void request_test_destroy_body(void* a,uint32_t b,uint32_t c,uint32_t d) {
    arguments(a,worker,b,c,d);++destroys;errno=ERANGE;
}
static void original_notify(void* c,void* m,void* u) {
    assert(c==&connection && m==&method && u==&context && errno==EAGAIN);++notifies;
    if(behavior==THROW_NOTIFY)throw 19;
    if(behavior==CANCEL_NOTIFY)cancel_now();
    invoke(mx5_request_post_veneer,shared);
    errno=ERANGE;
}
static void unrelated(void*,void*,void*) { assert(false); }
static int32_t submit(void* c,void* m,A::RequestNotify cb,void* u,int timeout) {
    assert(c==&connection && m==&method && u==&context && timeout==-1 && errno==EAGAIN);++submits;
    if(behavior==UNRELATED)assert(cb==unrelated);
    else {
        assert(cb && cb!=original_notify);
        A::RequestHookHealth h=A::request_hook_health();assert(h.result==R::OK && h.ledger.requests==1);
        if(behavior==DELAYED_CALLBACK || behavior==SESSION_TRANSITION)delayed_callback=cb;
        else if(behavior!=FAILED_SUBMIT) { cb(c,m,u);assert(errno==ERANGE); }
    }
    errno=EDOM;return -123;
}
static int32_t free_method(void* m) { assert(m==&method && errno==EAGAIN);++frees;errno=ERANGE;return -321; }
static void free_once(bool only) {
    errno=EAGAIN;assert((only?mx5_request_free_only:mx5_request_free)(&method)==-321 && errno==ERANGE);
}
static void cleanup(void*) { no_scope();++cleanups; }
static void* cancel_thread(void*) {
    pthread_cleanup_push(cleanup,0);
    if(behavior==CANCEL_NOTIFY || behavior==CANCEL_GETTER) {
        errno=EAGAIN;mx5_request_submit(&connection,&method,original_notify,&context,-1);
    } else invoke(mx5_request_work_veneer,worker);
    pthread_cleanup_pop(0);
    assert(false);return 0;
}
int main(int argc,char** argv) {
    assert(argc==2);alarm(10);
#if defined(MX5_REQUEST_DSO_TEST)
    initialize_request_test_dso();
#endif
    const char* names[]={"normal","unrelated","failed_submit","destroy_queued","throw_notify", "throw_work",
                         "cancel_notify","cancel_work","malformed","other_worker",
                         "delayed_callback","throw_getter","cancel_getter","session_transition"};
    bool known=false;
    for(unsigned i=0;i<sizeof names/sizeof names[0];++i)if(!strcmp(argv[1],names[i])) { behavior=i;known=true; }
    assert(known && !A::request_hook_health().prepared);
    const uint32_t vptr=0x12345678,position=reinterpret_cast<uint32_t>(worker+8);
    memcpy(worker,&vptr,4);memcpy(worker+80,&position,4);
    if(behavior==MALFORMED)worker[80]^=1;
    if(behavior==OTHER_WORKER)worker[0]^=1;
    shared[0]=reinterpret_cast<uint32_t>(worker);shared[1]=0xdead1234;
    A::RequestBindings bindings=A::RequestBindings();
    R::ReplyApi api={reply,type,sender,error,serial};bindings.reply=api;
    const R::MethodApi method_api={destination,path,destination,member};bindings.method=method_api;
    bindings.submit=submit;bindings.notify=original_notify;bindings.free_method=bindings.free_method_only=free_method;
    bindings.position_vptr=vptr;bindings.post_trampoline=reinterpret_cast<void*>(request_test_post);
    bindings.work_trampoline=reinterpret_cast<void*>(request_test_work);bindings.destroy_trampoline=reinterpret_cast<void*>(request_test_destroy);
    A::RequestBindings missing=bindings;missing.method.get_path=0;
    errno=EDOM;assert(!A::prepare_request_hooks(missing,clock_fn,0) && errno==EDOM);
    assert(!A::request_hook_health().prepared);
    assert(A::prepare_request_hooks(bindings,clock_fn,0) && errno==EDOM);
    assert(!A::prepare_request_hooks(bindings,clock_fn,0));no_scope();
    if(behavior==SESSION_TRANSITION) {
        const A::SessionBindings sessions={session_create,session_destroy,session_status};
        assert(A::prepare_session_hooks(sessions));open_session();
    }
    if(behavior==CANCEL_NOTIFY || behavior==CANCEL_GETTER) {
        pthread_t thread;void* result=0;assert(!pthread_create(&thread,0,cancel_thread,0));
        assert(!pthread_join(thread,&result) && result==PTHREAD_CANCELED && cleanups==1);
    } else {
        bool caught=false;errno=EAGAIN;
        try { assert(mx5_request_submit(&connection,&method,behavior==UNRELATED?unrelated:original_notify,&context,-1)==-123 && errno==EDOM); }
        catch(int n) { assert(n==19);caught=true; }
        assert(caught==(behavior==THROW_NOTIFY || behavior==THROW_GETTER));
    }
    assert(A::request_hook_health().ledger.requests==(behavior==UNRELATED?0u:1u));
    if(behavior==SESSION_TRANSITION) {
        assert(mx5_session_destroy(&session_storage)==0 && !session_storage);
        open_session();assert(A::read_issue_session().lifetime==2);
        assert(A::request_hook_health().ledger.requests==1);
    }
    if(behavior==DELAYED_CALLBACK || behavior==SESSION_TRANSITION) {
        assert(delayed_callback && !notifies && !posts);
        errno=EAGAIN;delayed_callback(&connection,&method,&context);assert(errno==ERANGE);
    }
    assert(submits==1 && notifies==(behavior==UNRELATED || behavior==FAILED_SUBMIT || behavior==THROW_GETTER || behavior==CANCEL_GETTER?0u:1u));
    const A::RequestHookHealth before=A::request_hook_health();
    // While the real request is still live, a later unscoped PostWorker must
    // not borrow the ReplyScope that just returned/threw/was cancelled.
    errno=EAGAIN;mx5_request_post_enter(shared);assert(errno==EAGAIN);
    const A::RequestHookHealth after=A::request_hook_health();
    assert(after.result==R::OK && after.ledger.requests==before.ledger.requests &&
           after.ledger.workers==before.ledger.workers && !after.ledger.loss_reasons);
    free_once(behavior==FAILED_SUBMIT);
    if(posts && behavior!=DESTROY_QUEUED) {
        if(behavior==CANCEL_WORK) {
            pthread_t thread;void* result=0;assert(!pthread_create(&thread,0,cancel_thread,0));
            assert(!pthread_join(thread,&result) && result==PTHREAD_CANCELED && cleanups==1);
        } else {
            bool caught=false;
            try { invoke(mx5_request_work_veneer,worker); }
            catch(int n) { assert(n==31);caught=true; }
            assert(caught==(behavior==THROW_WORK));
        }
    }
    if(posts)invoke(mx5_request_destroy_veneer,worker);
    assert(frees==1 && posts<=1 && works<=1 && destroys==posts);
    no_scope();empty();
    if(behavior==SESSION_TRANSITION) {
        assert(mx5_session_destroy(&session_storage)==0 && !session_storage);
        assert(session_creates==2 && session_destroys==2 && session_callbacks==2);
        assert(!A::session_hook_health().faults);
    }
    assert(A::request_hook_health().abi_fault==(behavior==MALFORMED));
    printf("PASS ARM request wrappers %s: original calls/args/results/errno and scope cleanup\n",argv[1]);
}
