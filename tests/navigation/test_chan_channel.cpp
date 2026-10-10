// VIM side-channel datagram format and socket (validation/
// VIM_CHANNEL_CAPTURE_2026-10-10.md). Synthetic; logging-only channel.
#include "navigation/channel.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
using namespace mx5::navigation;

static ChanBatch sample(unsigned count) {
    ChanBatch b;memset(&b,0,sizeof b);
    b.epoch=123456789ULL;b.batch=7;b.lost=3;b.count=count;
    for(unsigned i=0;i<count;++i) {
        ChanRecord& r=b.records[i];
        r.id=uint16_t(i%3==0?0x116:i%3==1?0x169:0x15b);r.length=uint8_t(i%3==0?10:i%3==1?4:12);
        r.dt_ms=1000+i;for(unsigned k=0;k<r.length;++k)r.data[k]=uint8_t(i*16+k+1);
    }
    return b;
}
static void format_tests() {
    unsigned char bytes[CHAN_DATAGRAM_MAX];
    for(unsigned count=1;count<=CHAN_RECORDS;++count) {
        const ChanBatch b=sample(count);
        const size_t n=encode_chan_batch(b,bytes);
        assert(n==CHAN_HEADER_SIZE+count*CHAN_RECORD_SIZE && !memcmp(bytes,"MDC1",4));
        ChanBatch d;assert(decode_chan_batch(bytes,n,&d));
        assert(d.epoch==b.epoch && d.batch==b.batch && d.lost==b.lost && d.count==count);
        for(unsigned i=0;i<count;++i)
            assert(d.records[i].id==b.records[i].id && d.records[i].length==b.records[i].length &&
                   d.records[i].dt_ms==b.records[i].dt_ms && !memcmp(d.records[i].data,b.records[i].data,CHAN_PAYLOAD));
        // Every truncation and extension is rejected.
        for(size_t k=0;k<n;++k)assert(!decode_chan_batch(bytes,k,&d));
        assert(!decode_chan_batch(bytes,n+1,&d));
    }
    ChanBatch b=sample(2);const size_t n=encode_chan_batch(b,bytes);ChanBatch d;
    // The invalid-length marker carries no bytes.
    b.records[1].length=CHAN_LENGTH_INVALID;memset(b.records[1].data,0xee,CHAN_PAYLOAD);
    assert(encode_chan_batch(b,bytes)==n && decode_chan_batch(bytes,n,&d) && d.records[1].length==CHAN_LENGTH_INVALID);
    for(unsigned k=0;k<CHAN_PAYLOAD;++k)assert(!d.records[1].data[k]);
    // Header and record checks.
    b=sample(2);assert(encode_chan_batch(b,bytes)==n);
    unsigned char bad[CHAN_DATAGRAM_MAX];
    struct Mutation { size_t at;unsigned char value; } mutations[]={
        {0,'X'},{4,2},{6,0},{6,3},{8,0},{16,0},{CHAN_HEADER_SIZE+3,1},{CHAN_HEADER_SIZE+2,17},
        {CHAN_HEADER_SIZE+8+10,1},{CHAN_HEADER_SIZE+CHAN_RECORD_SIZE+8+4,9}};
    for(size_t i=0;i<sizeof mutations/sizeof mutations[0];++i) {
        memcpy(bad,bytes,n);
        if(mutations[i].at==8)memset(bad+8,0,8);else if(mutations[i].at==16)memset(bad+16,0,4);
        else bad[mutations[i].at]=mutations[i].value;
        assert(!decode_chan_batch(bad,n,&d));
    }
    assert(decode_chan_batch(bytes,n,&d));   // the original still decodes
    assert(!decode_chan_batch(0,n,&d) && !decode_chan_batch(bytes,n,0));
    // Encoder input checks.
    b=sample(2);b.count=0;assert(!encode_chan_batch(b,bytes));
    b=sample(2);b.count=CHAN_RECORDS+1;assert(!encode_chan_batch(b,bytes));
    b=sample(2);b.epoch=0;assert(!encode_chan_batch(b,bytes));
    b=sample(2);b.batch=0;assert(!encode_chan_batch(b,bytes));
    b=sample(2);b.records[0].length=17;assert(!encode_chan_batch(b,bytes));
    // Name derivation.
    char name[96];
    assert(chan_channel_name("mx5dr.motion.v1",name) && !strcmp(name,"mx5dr.motion.v1.ch"));
    char longest[93];memset(longest,'a',92);longest[92]=0;assert(chan_channel_name(longest,name));
    char too_long[94];memset(too_long,'a',93);too_long[93]=0;assert(!chan_channel_name(too_long,name));
    assert(!chan_channel_name("",name) && !chan_channel_name(0,name));
    puts("side channel format: round trip, truncation, header/record/padding checks, names passed");
}
static void socket_tests() {
    char name[80];snprintf(name,sizeof name,"mx5dr.chantest.%ld",(long)getpid());
    ChanReceiver receiver;ChanSender sender;
    ChanBatch out;assert(receiver.receive(&out)==CHANNEL_FAULT);   // not open
    assert(receiver.open_channel(name) && !receiver.open_channel(name) && sender.open_channel(name));
    ChanReceiver second;assert(!second.open_channel(name));          // one owner of the address
    assert(receiver.receive(&out)==CHANNEL_EMPTY);
    unsigned char bytes[CHAN_DATAGRAM_MAX];const ChanBatch b=sample(5);const size_t n=encode_chan_batch(b,bytes);
    assert(sender.send(bytes,n));
    assert(receiver.receive(&out)==CHANNEL_EVENT && out.count==5 && out.batch==7 && out.records[4].dt_ms==1004);
    assert(receiver.receive(&out)==CHANNEL_EMPTY);
    // A malformed datagram is a fault and the channel continues.
    unsigned char junk[40];memset(junk,0x55,sizeof junk);assert(sender.send(junk,sizeof junk));
    assert(receiver.receive(&out)==CHANNEL_FAULT && out.count==0);
    assert(sender.send(bytes,n) && receiver.receive(&out)==CHANNEL_EVENT);
    // An oversized send is refused by the sender itself.
    unsigned char big[CHAN_DATAGRAM_MAX+1];memset(big,0,sizeof big);assert(!sender.send(big,sizeof big));
    // A datagram without credentials (sender did not pass SO_PASSCRED; the
    // receiver's kernel adds them) is still authenticated: same uid.
    int raw=socket(AF_UNIX,SOCK_DGRAM,0);assert(raw>=0);
    sockaddr_un a;memset(&a,0,sizeof a);a.sun_family=AF_UNIX;memcpy(a.sun_path+1,name,strlen(name));
    const socklen_t len=socklen_t(offsetof(sockaddr_un,sun_path)+1+strlen(name));
    assert(sendto(raw,bytes,n,0,reinterpret_cast<sockaddr*>(&a),len)==ssize_t(n));
    assert(receiver.receive(&out)==CHANNEL_EVENT);
    close(raw);
    // No receiver: the nonblocking send fails without blocking.
    char other[80];snprintf(other,sizeof other,"mx5dr.chantest.none.%ld",(long)getpid());
    ChanSender lonely;assert(lonely.open_channel(other) && !lonely.send(bytes,n));
    // A full receiver queue: sends fail (EAGAIN) instead of blocking.
    unsigned sent=0;for(unsigned i=0;i<100000 && sender.send(bytes,n);++i)++sent;
    assert(sent>0 && sent<100000);
    unsigned drained=0;while(receiver.receive(&out)==CHANNEL_EVENT)++drained;
    assert(drained==sent);
    puts("side channel socket: event, empty, fault, credentials, no receiver, full queue passed");
}
int main() { format_tests();socket_tests();return 0; }
