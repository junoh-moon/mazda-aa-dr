#include "navigation/channel.h"
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <unistd.h>
using namespace mx5::navigation;
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
