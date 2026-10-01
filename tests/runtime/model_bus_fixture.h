// Authored originals. The real submit wrapper observes which bus carries LDS
// requests; these fixtures do not load OEM code or qualify a provider.
#include "adapter/bus_hooks.h"
#include "adapter/request_hooks.h"
namespace bus_fixture {
namespace A=mx5::adapter;
namespace B=mx5::runtime::bus_trace;
static int handles[2],method,reply;
static unsigned selected;
static A::BusClosed callbacks[2];
static std::atomic<unsigned> blocked(0);
static int32_t closed(void*,void*) { return 0; }
static void* create(A::BusClosed callback,void*) { callbacks[selected]=callback;return &handles[selected]; }
static int32_t connect(void*,const char*,int32_t,uintptr_t) { return 1; }
static void end(void*) {
    if(blocked.load()) { blocked.store(2);while(blocked.load()!=3)usleep(1000); }
}
static int32_t signal(void*,void*) { return 0; }
static int32_t is_signal(void*,const char*,const char*) { return 1; }
static void* get_reply(void*) { return &reply; }
static int get_type(void*) { return 2; }
static const char* get_sender(void*) { return ":1.7"; }
static const char* get_error(void*) { return 0; }
static int get_serial(void*,uint32_t* out) { *out=7;return 0; }
static const char* destination(void*) { return "com.jci.lds.data"; }
static const char* path(void*) { return "/com/jci/lds/data"; }
static const char* interface_name(void*) { return "authored.route"; }
static const char* member(void*) { return "GetPosition"; }
static void notify(void*,void*,void*) {}
static int32_t submit(void*,void*,A::RequestNotify,void*,int) { return 42; }
static int32_t free_method(void*) { return 13; }
static void trampoline() {}
static uint64_t now(void*) {
    timespec t;assert(!clock_gettime(CLOCK_MONOTONIC,&t));
    return uint64_t(t.tv_sec)*1000000000ULL+t.tv_nsec;
}
static void mark(unsigned index=0) {
    assert(mx5_request_submit(&handles[index],&method,notify,0,-1)==42);
    assert(mx5_request_free(&method)==13);
}
static void open(unsigned index=0) {
    selected=index;assert(mx5_bus_create(closed,0)==&handles[index]);
    assert(mx5_bus_connect(&handles[index],"authored",0,0)==1);mark(index);
}
static void prepare() {
    const A::BusBindings bus={create,connect,end,end,signal,is_signal,{}};
    assert(A::prepare_bus_hooks(bus));
    A::RequestBindings r=A::RequestBindings();
    r.reply={get_reply,get_type,get_sender,get_error,get_serial};
    r.method={destination,path,interface_name,member};r.submit=submit;r.notify=notify;
    r.free_method=r.free_method_only=free_method;r.position_vptr=1;
    r.post_trampoline=r.work_trampoline=r.destroy_trampoline=reinterpret_cast<void*>(trampoline);
    assert(A::prepare_request_hooks(r,now,0));open();
}
}
