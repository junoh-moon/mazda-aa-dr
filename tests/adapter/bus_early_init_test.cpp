#include "adapter/bus_hooks.h"
#include <cassert>
#include <cstdio>
namespace A=mx5::adapter;
namespace B=mx5::runtime::bus_trace;
static int object;
static A::BusClosed callback;
static int32_t closed(void* p,void* user) { assert(p==&object && !user);return -9; }
static void* create(A::BusClosed cb,void* user) { assert(!user);callback=cb;return &object; }
static int32_t connect(void*,const char*,int32_t,uintptr_t) { return 1; }
static void end(void*) {}
static int32_t signal(void*,void*) { return 0; }
static int32_t is_signal(void*,const char*,const char*) { return 0; }
__attribute__((constructor(101))) static void early_create() {
    const A::BusBindings bindings={create,connect,end,end,signal,is_signal,{}};
    assert(A::prepare_bus_hooks(bindings));
    assert(mx5_bus_create(closed,0)==&object);
    assert(mx5_bus_connect(&object,"early",0,0)==1);
    assert(A::read_bus_connection(&object).result==B::CONNECTED);
}
int main() {
    const B::Snapshot s=A::read_bus_connection(&object);
    assert(s.result==B::CONNECTED && s.object==1 && s.lifetime==1);
    assert(callback(&object,0)==-9);
    assert(A::read_bus_connection(&object).result==B::DISCONNECTED);
    mx5_bus_free(&object);
    assert(A::read_bus_connection(&object).result==B::UNOBSERVED);
    assert(!A::bus_hook_health().faults);
    puts("bus before global constructors: ok");
}
