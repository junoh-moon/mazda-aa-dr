#include "navigation/channel.h"
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <climits>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
using namespace mx5::navigation;
static uint64_t monotonic_ns() {
    timespec now;assert(clock_gettime(CLOCK_MONOTONIC,&now)==0);
    return uint64_t(now.tv_sec)*1000000000ULL+now.tv_nsec;
}
struct DelayedInput { MotionSender* sender;RawEvent event; };
static void* delayed_input(void* context) {
    DelayedInput* input=static_cast<DelayedInput*>(context);
    usleep(20000);
    assert(input->sender->send_event(input->event));return 0;
}
static void wait_tests() {
    MotionReceiver receiver;MotionSender sender;
    assert(receiver.wait_for_input(0)==-1 && errno==EBADF);
    char name[80];snprintf(name,sizeof name,"mx5dr.wait.%ld",(long)getpid());
    assert(receiver.open_channel(name)&&sender.open_channel(name));
    assert(receiver.wait_for_input(UINT_MAX)==-1 && errno==EINVAL);
    assert(receiver.wait_for_input(0)==0);
    const uint64_t idle=monotonic_ns();assert(receiver.wait_for_input(10)==0);
    assert(monotonic_ns()-idle>=1000000ULL); // A real wait, not a constant stub.
    DelayedInput input={&sender,RawEvent()};
    input.event.kind=REVERSE;input.event.epoch=1;input.event.receive_seq=1;
    input.event.received_ns=1000000000;input.event.reverse=1;
    uint64_t fastest=UINT64_MAX;
    for(unsigned trial=0;trial<4;++trial) {
        input.event.receive_seq=trial+1;
        pthread_t thread;assert(pthread_create(&thread,0,delayed_input,&input)==0);
        const uint64_t begin=monotonic_ns();
        assert(receiver.wait_for_input(1000)==1);
        const uint64_t elapsed=monotonic_ns()-begin;
        if(elapsed<fastest)fastest=elapsed;
        assert(pthread_join(thread,0)==0);
        assert(receiver.wait_for_input(0)==1); // Readiness leaves the packet queued.
        RawEvent out;assert(receiver.receive(input.event.received_ns,&out)==CHANNEL_EVENT);
        assert(out.receive_seq==trial+1&&out.received_ns==input.event.received_ns&&out.reverse==1);
    }
    // A fixed 50ms sleep followed by poll(0) must fail. Allow individual
    // scheduler outliers; this is a regression check, not a real-time promise.
    assert(fastest<40000000ULL);
    assert(receiver.wait_for_input(0)==0);
    puts("motion wait: bounded idle, delayed wake, untouched packet, invalid descriptor/timeout passed");
}
static void inspect_tests() {
    RawEvent e=RawEvent(),out; e.kind=REVERSE;e.epoch=9;e.receive_seq=1;
    e.received_ns=1000000000;e.reverse=1;
    unsigned char bytes[MOTION_RECORD_SIZE];assert(encode_motion(e,bytes));
    MotionDatagram packet={bytes,sizeof bytes,false,true,42,7};
    MotionCursor cursor;ReceiveDiagnostic d;
    auto inspect=[&](uint64_t now,ReceiveFault reason,bool retained) {
        out=e;
        assert(inspect_motion_datagram(packet,7,now,cursor,&out,&d)==
               (reason==RECEIVE_OK?CHANNEL_EVENT:CHANNEL_FAULT));
        assert(d.reason==reason && d.checked_ns==now && d.authenticated_decoded==retained);
        if(reason!=RECEIVE_OK) {
            assert(out.received_ns==0 && out.receive_seq==0);
            assert(d.rejected.receive_seq==(retained?e.receive_seq:0));
            if(retained)assert(d.rejected.received_ns==e.received_ns && d.rejected.reverse==1);
        }
    };
    packet.truncated=true;inspect(e.received_ns,RECEIVE_TRUNCATED,false);packet.truncated=false;
    packet.credentials_present=false;inspect(e.received_ns,RECEIVE_CREDENTIALS_MISSING,false);
    packet.credentials_present=true;packet.sender_uid=8;
    inspect(e.received_ns,RECEIVE_CREDENTIALS_MISMATCH,false);packet.sender_uid=7;
    packet.sender_pid=0;inspect(e.received_ns,RECEIVE_CREDENTIALS_MISMATCH,false);packet.sender_pid=42;
    bytes[63]=1;inspect(e.received_ns,RECEIVE_DECODE,false);bytes[63]=0;
    inspect(0,RECEIVE_CLOCK_UNAVAILABLE,true);
    inspect(e.received_ns-1,RECEIVE_FUTURE,true);
    inspect(e.received_ns+250000001,RECEIVE_STALE,true);
    inspect(e.received_ns+250000000,RECEIVE_OK,true);
    e.receive_seq=3;assert(encode_motion(e,bytes));inspect(e.received_ns,RECEIVE_SEQUENCE,true);
    e.receive_seq=4;assert(encode_motion(e,bytes));inspect(e.received_ns,RECEIVE_OK,true);
    e.receive_seq=2;assert(encode_motion(e,bytes));inspect(e.received_ns,RECEIVE_SEQUENCE,true);
    e.receive_seq=5;assert(encode_motion(e,bytes));inspect(e.received_ns,RECEIVE_OK,true);
    ++e.epoch;e.receive_seq=1;assert(encode_motion(e,bytes));inspect(e.received_ns,RECEIVE_SOURCE_CHANGED,true);
    ++e.receive_seq;assert(encode_motion(e,bytes));inspect(e.received_ns,RECEIVE_OK,true);
    ++packet.sender_pid;inspect(e.received_ns,RECEIVE_SOURCE_CHANGED,true);
    MotionReceiver unopened;
    assert(unopened.receive(e.received_ns,&out,&d)==CHANNEL_FAULT);
    assert(d.reason==RECEIVE_SYSCALL && d.syscall_errno==EBADF && !d.authenticated_decoded);
    assert(out.received_ns==0 && out.receive_seq==0);
    puts("motion inspection: authenticated rejection retention and distinct bounded faults passed");
}
int main() {
    inspect_tests();
    char name[80];snprintf(name,sizeof name,"mx5dr.test.%ld",(long)getpid());
    MotionReceiver receiver,duplicate;MotionSender sender;
    RawEvent e=RawEvent();e.kind=YAW;e.epoch=9;e.receive_seq=1;e.received_ns=1000000000;
    e.raw[0]=6141;e.count=3;
    unsigned char bytes[MOTION_RECORD_SIZE];RawEvent decoded;
    assert(encode_motion(e,bytes));assert(decode_motion(bytes,sizeof bytes,&decoded));
    assert(decoded.source_mono_ms==0 && decoded.raw[0]==6141 && decoded.count==3);
    assert(!decode_motion(bytes,sizeof bytes-1,&decoded));
    bytes[63]=1;assert(!decode_motion(bytes,sizeof bytes,&decoded));bytes[63]=0;
    e.source_mono_ms=-1;assert(encode_motion(e,bytes));assert(decode_motion(bytes,sizeof bytes,&decoded));
    assert(decoded.source_mono_ms==-1); // preserve rollback, pipeline rejects it
    e.source_mono_ms=0;
    MotionCursor cursor;
    assert(cursor.accept(10,1,100));
    assert(!cursor.accept(10,1,50));assert(!cursor.accept(10,1,51));
    assert(cursor.accept(10,1,101));
    assert(!cursor.accept(10,1,105));assert(cursor.accept(10,1,106));
    assert(!cursor.accept(10,2,1));assert(cursor.accept(10,2,2));
    assert(!cursor.accept(11,2,1));assert(cursor.accept(11,2,2));
    if(!receiver.open_channel(name)) {
        if(errno==EPERM || errno==EACCES) {
            puts("motion codec/cursor passed; SKIP kernel socket integration: socket permission denied");
            return 77;
        }
        assert(false);
    }
    assert(!duplicate.open_channel(name));assert(sender.open_channel(name));
    wait_tests();
    assert(receiver.receive(e.received_ns,&decoded)==CHANNEL_EMPTY);
    assert(sender.send_event(e));assert(receiver.receive(e.received_ns,&decoded)==CHANNEL_EVENT);
    assert(decoded.receive_seq==1 && decoded.epoch==9);
    e.receive_seq=3;assert(sender.send_event(e));
    assert(receiver.receive(e.received_ns,&decoded)==CHANNEL_FAULT); // lost datagram
    e.receive_seq=4;assert(sender.send_event(e));
    assert(receiver.receive(e.received_ns,&decoded)==CHANNEL_EVENT);
    assert(sender.send_event(e));assert(receiver.receive(e.received_ns,&decoded)==CHANNEL_FAULT);
    e.epoch=10;e.receive_seq=1;assert(sender.send_event(e));
    assert(receiver.receive(e.received_ns,&decoded)==CHANNEL_FAULT); // source restart
    ++e.receive_seq;assert(sender.send_event(e));
    assert(receiver.receive(e.received_ns,&decoded)==CHANNEL_EVENT);
    ++e.receive_seq;assert(sender.send_event(e));
    assert(receiver.receive(e.received_ns+250000001,&decoded)==CHANNEL_FAULT); // queued stale
    ++e.receive_seq;assert(sender.send_event(e));
    assert(receiver.receive(e.received_ns-1,&decoded)==CHANNEL_FAULT); // future clock
    puts("motion channel: exact encoding, credentials, singleton, datagram loss/restart/age passed");
}
