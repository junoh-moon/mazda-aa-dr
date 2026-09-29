#include "sensors/vim_source.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
using namespace mx5::sensors;
using namespace mx5::navigation;
int main() {
    unsigned char bytes[16]={0xa5,0x10,0x27,0x74,0x27,0xd8,0x27,0x3c,0x28};
    VimMessage msg={0x100,9,bytes};RawEvent e=RawEvent();
    assert(decode_vim_message(msg,9,2,100,&e)==VIM_DECODED);
    assert(e.kind==WHEELS && e.raw[0]==10000 && e.raw[1]==10100 && e.raw[2]==10200 && e.raw[3]==10300);
    assert(e.epoch==9 && e.receive_seq==2 && e.received_ns==100 && e.source_mono_ms==0);
    // No arithmetic conversion, rounding, quality promotion, or yaw mutation.
    msg.id=0x116;msg.length=7;bytes[1]=0xfa;bytes[2]=0x0f;bytes[3]=2;
    assert(decode_vim_message(msg,9,3,110,&e)==VIM_DECODED);
    assert(e.kind==YAW && e.raw[0]==4090 && e.count==2 && bytes[1]==0xfa);
    bytes[3]=0;bytes[1]=0xff;
    assert(decode_vim_message(msg,9,4,120,&e)==VIM_DECODED);
    assert(e.count==0 && e.raw[0]==4095); // Estimator must reject, source must preserve.
    msg.id=0x118;msg.length=2;bytes[1]=255;
    assert(decode_vim_message(msg,9,5,130,&e)==VIM_DECODED && e.reverse==255);
    bytes[1]=1;assert(decode_vim_message(msg,9,6,140,&e)==VIM_DECODED && e.reverse==1);
    for(unsigned n=0;n<9;++n) {msg.id=0x100;msg.length=n;assert(decode_vim_message(msg,9,7,150,&e)==VIM_MALFORMED);}
    msg.length=17;assert(decode_vim_message(msg,9,7,150,&e)==VIM_MALFORMED);
    msg.length=9;msg.data=0;assert(decode_vim_message(msg,9,7,150,&e)==VIM_MALFORMED);
    msg.data=bytes;assert(decode_vim_message(msg,0,7,150,&e)==VIM_MALFORMED);
    assert(decode_vim_message(msg,9,0,150,&e)==VIM_MALFORMED);
    assert(decode_vim_message(msg,9,7,0,&e)==VIM_MALFORMED);
    assert(decode_vim_message(msg,9,7,150,0)==VIM_MALFORMED);
    msg.id=0x15b;assert(decode_vim_message(msg,9,7,150,&e)==VIM_IGNORED);
    puts("VIM source payload decoding checks passed");
}
