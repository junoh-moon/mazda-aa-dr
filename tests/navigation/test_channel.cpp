#include "navigation/channel.h"
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <climits>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#ifdef MX5DR_CHANNEL_RECVMSG_WRAP
#include "receive_clock_fixture.h"
#endif
using namespace mx5::navigation;
static uint64_t monotonic_ns() {
    timespec now;assert(clock_gettime(CLOCK_MONOTONIC,&now)==0);
    return uint64_t(now.tv_sec)*1000000000ULL+now.tv_nsec;
}
struct DelayedInput { MotionSender* sender;RawEvent event; };
static void* delayed_input(void* context) {
    DelayedInput* input=static_cast<DelayedInput*>(context);
    usleep(20000);
    input->event.received_ns=monotonic_ns();
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
        RawEvent out;assert(receiver.receive(&out)==CHANNEL_EVENT);
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
    assert(unopened.receive(&out,&d)==CHANNEL_FAULT);
    assert(d.reason==RECEIVE_SYSCALL && d.syscall_errno==EBADF && !d.authenticated_decoded);
    assert(out.received_ns==0 && out.receive_seq==0);
    puts("motion inspection: authenticated rejection retention and distinct bounded faults passed");
}
// Late arrival (2026-10-06, validation/WORKER_STALL_STALE_2026-10-06.md):
// a record older than 250 ms is still accepted up to 2 s when it is the next
// contiguous record of the same producer pid and epoch and its receipt time
// is monotonic. Everything else stays stale; a stale record of the current
// source consumes its sequence number so the next fresh record is accepted.
static void late_arrival_tests() {
    const uint64_t R=1000000000ULL,MS=1000000ULL;
    MotionCursor cursor;ReceiveDiagnostic d;RawEvent out;
    uint64_t seq=0,received=R;pid_t pid=42;uint64_t epoch=9;
    // Sends the next record (seq+step) received at received+advance_ms,
    // checked age_ms later, and returns the inspection result.
    auto next=[&](unsigned step,unsigned advance_ms,unsigned age_ms_whole,unsigned age_extra_ns=0) {
        RawEvent e=RawEvent();e.kind=WHEELS;e.epoch=epoch;seq+=step;e.receive_seq=seq;
        received+=uint64_t(advance_ms)*MS;e.received_ns=received;
        unsigned char bytes[MOTION_RECORD_SIZE];assert(encode_motion(e,bytes));
        const MotionDatagram packet={bytes,sizeof bytes,false,true,pid,7};
        out=RawEvent();
        const uint64_t now=received+uint64_t(age_ms_whole)*MS+age_extra_ns;
        const ReceiveResult r=inspect_motion_datagram(packet,7,now,cursor,&out,&d);
        assert(d.checked_ns==now && d.authenticated_decoded);
        assert(d.age_ns==now-received);
        if(r==CHANNEL_EVENT)assert(out.receive_seq==seq && out.received_ns==received); // never rewritten
        return r;
    };
    assert(next(1,0,0)==CHANNEL_EVENT && !d.late);                         // establishes the cursor
    assert(next(1,10,249)==CHANNEL_EVENT && !d.late);
    assert(next(1,10,250)==CHANNEL_EVENT && !d.late);                      // 250 ms is still fresh
    assert(next(1,10,250,1)==CHANNEL_EVENT && d.late && d.reason==RECEIVE_OK);
    assert(next(1,10,251)==CHANNEL_EVENT && d.late);
    assert(next(1,10,1999)==CHANNEL_EVENT && d.late && d.age_ns==1999*MS);
    assert(next(1,10,2000)==CHANNEL_EVENT && d.late);                      // KEEP_NS is inclusive
    assert(next(1,10,2000,1)==CHANNEL_FAULT && d.reason==RECEIVE_STALE && !d.late);
    assert(d.rejected.receive_seq==seq);
    assert(next(1,10,2001)==CHANNEL_FAULT && d.reason==RECEIVE_STALE);
    // The stale records consumed their sequence numbers: no discontinuity.
    assert(next(1,10,0)==CHANNEL_EVENT && !d.late);
    // A gap in the sequence is never a late arrival.
    assert(next(2,10,500)==CHANNEL_FAULT && d.reason==RECEIVE_STALE);
    assert(next(1,10,0)==CHANNEL_EVENT);                                   // cursor advanced over the gap
    assert(next(3,10,0)==CHANNEL_FAULT && d.reason==RECEIVE_SEQUENCE);     // fresh gap: unchanged rule
    // Another producer pid or epoch: stale, and the cursor keeps the old
    // source, so the fresh record of the old source continues and a fresh
    // record of the new source is still a source change (today's rule).
    pid=43;assert(next(1,10,500)==CHANNEL_FAULT && d.reason==RECEIVE_STALE);
    pid=42;assert(next(0,10,0)==CHANNEL_EVENT);                            // same seq again, old pid
    epoch=10;assert(next(1,10,500)==CHANNEL_FAULT && d.reason==RECEIVE_STALE);
    epoch=9;assert(next(0,10,0)==CHANNEL_EVENT);
    pid=43;assert(next(1,10,0)==CHANNEL_FAULT && d.reason==RECEIVE_SOURCE_CHANGED);
    pid=42;assert(next(1,10,0)==CHANNEL_FAULT && d.reason==RECEIVE_SOURCE_CHANGED);
    assert(next(1,10,0)==CHANNEL_EVENT);
    // Receipt time going backwards: a late record is rejected...
    assert(next(1,10,0)==CHANNEL_EVENT);
    {
        RawEvent e=RawEvent();e.kind=YAW;e.epoch=epoch;e.receive_seq=++seq;e.received_ns=received-5*MS;
        e.count=1;unsigned char bytes[MOTION_RECORD_SIZE];assert(encode_motion(e,bytes));
        const MotionDatagram packet={bytes,sizeof bytes,false,true,pid,7};
        assert(inspect_motion_datagram(packet,7,e.received_ns+600*MS,cursor,&out,&d)==CHANNEL_FAULT);
        assert(d.reason==RECEIVE_STALE && !d.late);
        // ...but it consumed its sequence number; the next fresh one is fine.
        assert(next(1,10,0)==CHANNEL_EVENT);
        // A fresh record with a regressed receipt time is unchanged: accepted
        // here and left to the pipeline's per-sensor clock check.
        e.receive_seq=++seq;e.received_ns=received-5*MS;assert(encode_motion(e,bytes));
        assert(inspect_motion_datagram(packet,7,e.received_ns,cursor,&out,&d)==CHANNEL_EVENT && !d.late);
        // Still the high-water receipt: the next late one must not precede it.
        e.receive_seq=++seq;e.received_ns=received-1*MS;assert(encode_motion(e,bytes));
        assert(inspect_motion_datagram(packet,7,e.received_ns+300*MS,cursor,&out,&d)==CHANNEL_FAULT &&
               d.reason==RECEIVE_STALE);
        // Equal receipt time is monotonic (non-decreasing).
        e.receive_seq=++seq;e.received_ns=received;assert(encode_motion(e,bytes));
        assert(inspect_motion_datagram(packet,7,e.received_ns+300*MS,cursor,&out,&d)==CHANNEL_EVENT && d.late);
    }
    // A first record of a new receiver has no cursor: never a late arrival.
    MotionCursor fresh;
    {
        RawEvent e=RawEvent();e.kind=WHEELS;e.epoch=1;e.receive_seq=1;e.received_ns=R;
        unsigned char bytes[MOTION_RECORD_SIZE];assert(encode_motion(e,bytes));
        const MotionDatagram packet={bytes,sizeof bytes,false,true,42,7};
        assert(inspect_motion_datagram(packet,7,R+300*MS,fresh,&out,&d)==CHANNEL_FAULT && d.reason==RECEIVE_STALE);
        e.receive_seq=2;assert(encode_motion(e,bytes));
        // The stale first record did not establish a source either.
        assert(inspect_motion_datagram(packet,7,R,fresh,&out,&d)==CHANNEL_EVENT);
    }
    puts("motion late arrival: 250/251 ms and 2000/2001 ms bounds, gap, pid/epoch change, "
         "monotonic receipt and stale sequence advance passed");
}
int main() {
    inspect_tests();
    late_arrival_tests();
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
#ifdef MX5DR_CHANNEL_RECVMSG_WRAP
    receive_clock_order_test();
#endif
    wait_tests();
    assert(receiver.receive(&decoded)==CHANNEL_EMPTY);
    e.received_ns=monotonic_ns();assert(sender.send_event(e));assert(receiver.receive(&decoded)==CHANNEL_EVENT);
    assert(decoded.receive_seq==1 && decoded.epoch==9);
    e.receive_seq=3;assert(sender.send_event(e));
    assert(receiver.receive(&decoded)==CHANNEL_FAULT); // lost datagram
    e.receive_seq=4;assert(sender.send_event(e));
    assert(receiver.receive(&decoded)==CHANNEL_EVENT);
    assert(sender.send_event(e));assert(receiver.receive(&decoded)==CHANNEL_FAULT);
    e.epoch=10;e.receive_seq=1;assert(sender.send_event(e));
    assert(receiver.receive(&decoded)==CHANNEL_FAULT); // source restart
    ++e.receive_seq;assert(sender.send_event(e));
    assert(receiver.receive(&decoded)==CHANNEL_EVENT);
    ReceiveDiagnostic diagnostic;
    ++e.receive_seq;e.received_ns=monotonic_ns()-250000001ULL;assert(sender.send_event(e));
    assert(receiver.receive(&decoded,&diagnostic)==CHANNEL_FAULT && diagnostic.reason==RECEIVE_STALE);
    ++e.receive_seq;e.received_ns=UINT64_MAX;assert(sender.send_event(e));
    assert(receiver.receive(&decoded,&diagnostic)==CHANNEL_FAULT && diagnostic.reason==RECEIVE_FUTURE);
    puts("motion channel: exact encoding, credentials, singleton, datagram loss/restart/age passed");
}
