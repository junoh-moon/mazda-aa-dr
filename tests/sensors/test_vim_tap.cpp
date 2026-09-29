// Synthetic host/ARM ABI checks. Never execute proprietary libraries.
#define MX5_VIM_TAP_TESTING
#include "../../src/sensors/vim_tap.cpp"
#include <assert.h>
#include <stdio.h>
namespace {
const uint64_t CLIENT=uint64_t(0x80000068u)<<32, SERVER=uint64_t(0x80000067u)<<32;
unsigned forwarded=0,called=0,sent=0;
const VimCallbacks* registered=0;
const VimMessage* wanted_message=0;
RawEvent last;
unsigned char payload[9]={0,0x10,0x27,0x74,0x27,0xd8,0x27,0x3c,0x28};
int fake_add(uint64_t c,uint64_t s,const VimCallbacks* table) {
    assert(c==CLIENT && s==SERVER && errno==EDOM);++forwarded;registered=table;errno=ERANGE;return 137;
}
void fake_callback(uint64_t c,uint64_t s,const VimMessage* m) {
    assert(c==CLIENT && s==SERVER && m==wanted_message && errno==EDOM);
    ++called;payload[1]=0xff;errno=ERANGE;
}
template<unsigned N> void other_callback(uint64_t c,uint64_t s,const VimMessage* m) { fake_callback(c,s,m); }
bool fake_send(const RawEvent& e) { last=e;++sent;errno=EIO;return false; }
}
int main() {
    init_phase=2;real_add=fake_add;observation_enabled=false;
    VimCallbacks cb={fake_callback};errno=EDOM;
    assert(VIMC_AddClient(CLIENT,SERVER,&cb)==137 && errno==ERANGE && forwarded==1 && registered==&cb);
    errno=EDOM;assert(VIMC_AddClient(CLIENT,SERVER,0)==137 && errno==ERANGE && registered==0);
    VimCallbacks empty={0};errno=EDOM;VIMC_AddClient(CLIENT,SERVER,&empty);assert(registered==&empty);
    // Concurrent/reentrant initialization forwards without waiting or wrapping.
    init_phase=1;errno=EDOM;VIMC_AddClient(CLIENT,SERVER,&cb);assert(registered==&cb && errno==ERANGE);
    init_phase=2;
    // Production routing algorithm with explicit test-only ownership/send seams.
    epoch=1;observation_enabled=true;test_accept_tables=true;test_send=fake_send;
    errno=EDOM;assert(VIMC_AddClient(CLIENT,SERVER,&cb)==137 && errno==ERANGE);
    assert(registered!=&cb && registered->app_event==wrappers[0]);
    const VimCallbacks* first=registered;
    errno=EDOM;VIMC_AddClient(CLIENT,SERVER,&cb);assert(registered==first);
    VimMessage msg={0x100,9,payload};wanted_message=&msg;errno=EDOM;
    registered->app_event(CLIENT,SERVER,&msg);
    assert(called==1 && errno==ERANGE && sent==1 && last.raw[0]==10000 && payload[1]==0xff);
    assert(last.source_mono_ms==0 && last.receive_seq==1);
    pthread_mutex_lock(&sender_lock);errno=EDOM;registered->app_event(CLIENT,SERVER,&msg);pthread_mutex_unlock(&sender_lock);
    assert(called==2 && errno==ERANGE && sent==1);
    errno=EDOM;registered->app_event(CLIENT,SERVER,&msg);assert(called==3 && sent==2 && last.receive_seq==3);
    msg.length=1;errno=EDOM;registered->app_event(CLIENT,SERVER,&msg);assert(called==4 && errno==ERANGE && sent==2);
    VimCallbacks more[8]={{other_callback<0>},{other_callback<1>},{other_callback<2>},{other_callback<3>},
        {other_callback<4>},{other_callback<5>},{other_callback<6>},{other_callback<7>}};
    for(unsigned i=0;i<8;++i) {errno=EDOM;VIMC_AddClient(CLIENT,SERVER,&more[i]);assert(errno==ERANGE);
        assert(i<7 ? registered!=&more[i] : registered==&more[i]);}
    // An old queued route keeps its original callback after later registrations.
    errno=EDOM;first->app_event(CLIENT,SERVER,&msg);assert(called==5 && errno==ERANGE);
    test_accept_tables=false;errno=EDOM;VIMC_AddClient(CLIENT,SERVER,&cb);assert(registered==&cb);
    real_add=0;errno=EDOM;assert(VIMC_AddClient(CLIENT,SERVER,0)==102 && errno==EDOM);
    puts("VIM tap registration routing, forwarding, pre-mutation copy, errno and loss checks passed");
}
