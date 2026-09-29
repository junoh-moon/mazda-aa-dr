#include "navigation/channel.h"
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <unistd.h>
using namespace mx5::navigation;
int main() {
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
