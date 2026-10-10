// Synthetic host/ARM ABI checks. Never execute proprietary libraries.
#define MX5_VIM_TAP_TESTING
#include "../../src/sensors/vim_tap.cpp"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
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
// ---- VIM side channel (validation/VIM_CHANNEL_CAPTURE_2026-10-10.md) ----
RawEvent stream[64];unsigned streamed=0;
bool record_send(const RawEvent& e) { assert(streamed<64);stream[streamed++]=e;errno=EIO;return false; }
unsigned char datagrams[16][CHAN_DATAGRAM_MAX];size_t datagram_size[16];unsigned datagram_count=0;
bool chan_accept=true;
bool record_chan(const unsigned char* p,size_t n) {
    assert(datagram_count<16 && n<=CHAN_DATAGRAM_MAX);
    memcpy(datagrams[datagram_count],p,n);datagram_size[datagram_count++]=n;errno=EPIPE;return chan_accept;
}
unsigned chan_called=0;
const VimMessage* chan_wanted=0;
void chan_callback(uint64_t c,uint64_t s,const VimMessage* m) {
    assert(c==CLIENT && s==SERVER && m==chan_wanted && errno==EDOM);
    ++chan_called;if(m->data && m->length>1)const_cast<unsigned char*>(m->data)[1]^=0x5a;   // OEM mutation after the copy
    errno=ERANGE;
}
// A mixed message sequence: the three motion kinds, the side-channel ids
// (valid, short, oversized, missing data) and an unrelated id.
struct Mixed { uint32_t id,length;unsigned char bytes[20];bool null_data; };
const Mixed MIXED[]={
    {0x100,9,{0,0x10,0x27,0x74,0x27,0xd8,0x27,0x3c,0x28},false},
    {0x116,10,{7,0xfa,0x0f,2,3,0x40,0x1f,0x12,0x00,3},false},
    {0x169,4,{2,3,0x34,0x12},false},
    {0x15b,12,{0x2f,0xe8,0x03,0xd0,0x07,0,0,0,0,0,0,3},false},
    {0x118,2,{0,1},false},
    {0x116,4,{7,0xfe,0x0f,2},false},          // motion-valid, no extra fields
    {0x169,20,{2,3,0x34,0x12},false},         // oversized: copied as invalid
    {0x15b,12,{0},true},                      // no data
    {0x144,3,{1,2,3},false},                  // not observed by either path
    {0x100,1,{0},false},                      // motion-malformed: sequence consumed
    {0x116,10,{7,0xf0,0x0f,2,1,0x00,0x20,0x00,0x20,7},false},
    {0x169,4,{2,0,0xff,0x1f},false},
};
const unsigned MIXED_N=sizeof MIXED/sizeof MIXED[0];
void run_mixed(const VimCallbacks* table,bool chan) {
    sequence=0;streamed=0;datagram_count=0;chan_enabled=chan;chan_batch=ChanBatch();chan_last_t=chan_first_t=0;
    chan_lost=0;chan_busy=0;test_send=record_send;
    unsigned char copies[MIXED_N][20];
    for(unsigned i=0;i<MIXED_N;++i) {
        memcpy(copies[i],MIXED[i].bytes,20);
        VimMessage m={MIXED[i].id,MIXED[i].length,MIXED[i].null_data?0:copies[i]};
        chan_wanted=&m;const unsigned before=chan_called;errno=EDOM;
        table->app_event(CLIENT,SERVER,&m);
        assert(chan_called==before+1 && errno==ERANGE);   // exactly once, OEM errno kept
    }
}
void chan_tests(const VimCallbacks* table) {
    // The motion event stream is identical with the side channel on and off.
    test_chan_send=record_chan;
    run_mixed(table,false);
    RawEvent off[64];const unsigned off_n=streamed;memcpy(off,stream,sizeof stream);
    assert(!datagram_count && off_n==5);
    run_mixed(table,true);
    assert(streamed==off_n);
    for(unsigned i=0;i<off_n;++i) {
        assert(stream[i].kind==off[i].kind && stream[i].epoch==off[i].epoch &&
               stream[i].receive_seq==off[i].receive_seq && stream[i].source_mono_ms==off[i].source_mono_ms &&
               !memcmp(stream[i].raw,off[i].raw,sizeof off[i].raw) && stream[i].count==off[i].count &&
               stream[i].reverse==off[i].reverse && stream[i].received_ns && off[i].received_ns);
    }
    // receive_seq: 0x169/0x15B/0x144 consume none; the malformed 0x100 one.
    const uint64_t seqs[]={1,2,3,4,6};
    for(unsigned i=0;i<off_n;++i)assert(off[i].receive_seq==seqs[i]);
    assert(sequence==6);
    // Seven side-channel records so far (0x116 x3, 0x169 x3, 0x15B x2 = 8):
    // the eighth flushed one batch of CHAN_RECORDS.
    assert(datagram_count==1 && chan_batch.count==0 && !chan_lost);
    ChanBatch b;assert(decode_chan_batch(datagrams[0],datagram_size[0],&b));
    assert(b.count==8 && b.batch==1 && b.lost==0 && b.epoch==epoch);
    const uint16_t ids[]={0x116,0x169,0x15b,0x116,0x169,0x15b,0x116,0x169};
    const uint8_t lengths[]={10,4,12,4,CHAN_LENGTH_INVALID,CHAN_LENGTH_INVALID,10,4};
    const unsigned source[]={1,2,3,5,6,7,10,11};
    for(unsigned i=0;i<8;++i) {
        assert(b.records[i].id==ids[i] && b.records[i].length==lengths[i]);
        if(lengths[i]!=CHAN_LENGTH_INVALID)   // copied before the OEM callback mutated byte 1
            assert(!memcmp(b.records[i].data,MIXED[source[i]].bytes,lengths[i]));
        else for(unsigned k=0;k<CHAN_PAYLOAD;++k)assert(!b.records[i].data[k]);
        if(i)assert(b.records[i].dt_ms>=b.records[i-1].dt_ms);
    }
    // 0x169/0x15B carry the receipt time of the latest motion message.
    assert(b.records[1].dt_ms==b.records[0].dt_ms && b.records[2].dt_ms==b.records[0].dt_ms);
    // Disabled side channel: nothing copied, nothing counted.
    run_mixed(table,false);assert(!datagram_count && !chan_lost && !chan_batch.count);
    // A concurrent holder of the try-only flag: the copy is skipped and
    // counted; the OEM callback and the motion path are unaffected.
    run_mixed(table,true);
    chan_busy=1;
    unsigned char lat[4]={2,3,0x34,0x12};VimMessage m={0x169,4,lat};chan_wanted=&m;errno=EDOM;
    const unsigned before=chan_called,streamed_before=streamed;
    table->app_event(CLIENT,SERVER,&m);
    assert(chan_called==before+1 && errno==ERANGE && chan_lost==1 && streamed==streamed_before);
    chan_busy=0;
    // A failed send counts the batch's records as lost; the next batch
    // reports the cumulative count.
    chan_accept=false;
    for(unsigned i=0;i<8;++i) { lat[0]=2;m.data=lat;errno=EDOM;table->app_event(CLIENT,SERVER,&m); }
    assert(chan_lost==1+8 && !chan_batch.count);
    chan_accept=true;
    for(unsigned i=0;i<8;++i) { errno=EDOM;table->app_event(CLIENT,SERVER,&m); }
    ChanBatch last_batch;assert(decode_chan_batch(datagrams[datagram_count-1],datagram_size[datagram_count-1],&last_batch));
    assert(last_batch.lost==9 && last_batch.count==8 && last_batch.batch==3);
    // Age flush: a pending record goes out once a later motion message is
    // CHAN_FLUSH_NS newer (here forced through the cached time).
    errno=EDOM;table->app_event(CLIENT,SERVER,&m);assert(chan_batch.count==1);
    const unsigned sent_before=datagram_count;
    chan_last_t+=CHAN_FLUSH_NS;
    unsigned char wheels[9]={0,0x10,0x27,0x74,0x27,0xd8,0x27,0x3c,0x28};
    VimMessage w={0x100,9,wheels};chan_wanted=&w;errno=EDOM;table->app_event(CLIENT,SERVER,&w);
    assert(datagram_count==sent_before+1 && !chan_batch.count);
    // No receipt time yet: a side-channel record is dropped and counted.
    chan_batch=ChanBatch();chan_last_t=0;chan_lost=0;chan_wanted=&m;errno=EDOM;
    table->app_event(CLIENT,SERVER,&m);assert(chan_lost==1 && !chan_batch.count);
    // Owner switch: while the marker exists nothing is opened and nothing
    // is sent; the motion stream equals the run with the side channel off.
    char marker[64];snprintf(marker,sizeof marker,"/tmp/mx5dr-vimchan-off.%ld",(long)getpid());
    FILE* f=fopen(marker,"w");assert(f);fclose(f);
    chan_sender=0;
    const bool switched=chan_start(marker,"mx5dr.taptest");
    assert(!switched && chan_sender==0);
    test_chan_send=record_chan;run_mixed(table,switched);
    assert(!datagram_count && !chan_lost && streamed==off_n);
    for(unsigned i=0;i<off_n;++i)assert(stream[i].receive_seq==off[i].receive_seq && stream[i].kind==off[i].kind &&
                                        !memcmp(stream[i].raw,off[i].raw,sizeof off[i].raw));
    unlink(marker);
    assert(chan_start(marker,"mx5dr.taptest") && chan_sender!=0);    // absent: on, socket opened
    chan_sender->~ChanSender();chan_sender=0;
    assert(chan_switch_on("/dev/null/x"));                            // not checkable (ENOTDIR): on
    assert(chan_switch_on(MOTION_CHANNEL_NAME) && !chan_switch_on("/"));
    chan_enabled=false;test_chan_send=0;test_send=fake_send;
    puts("VIM side channel: motion stream unchanged on/off, sequence untouched, raw copies, loss accounting passed");
}
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
    // Side channel on a fresh route of its own callback.
    {
        VimCallbacks side={chan_callback};
        for(unsigned i=0;i<ROUTES;++i)routes[i].used=false;
        errno=EDOM;VIMC_AddClient(CLIENT,SERVER,&side);
        assert(registered!=&side && registered->app_event==wrappers[0]);
        chan_tests(registered);
    }
    test_accept_tables=false;errno=EDOM;VIMC_AddClient(CLIENT,SERVER,&cb);assert(registered==&cb);
    real_add=0;errno=EDOM;assert(VIMC_AddClient(CLIENT,SERVER,0)==102 && errno==EDOM);
    puts("VIM tap registration routing, forwarding, pre-mutation copy, errno and loss checks passed");
}
