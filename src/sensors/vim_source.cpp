#include "vim_source.h"
namespace mx5 { namespace sensors {
namespace { uint16_t le16(const unsigned char* p) { return uint16_t(p[0]) | uint16_t(p[1])<<8; } }
DecodeResult decode_vim_message(const VimMessage& message,uint64_t epoch,
    uint64_t sequence,uint64_t received,navigation::RawEvent* out) {
    if(message.id!=0x100 && message.id!=0x116 && message.id!=0x118) return VIM_IGNORED;
    // Underlying VIM callback channel max application bytes is 16 (28-12).
    const unsigned minimum=message.id==0x100?9:(message.id==0x116?4:2);
    if(!out || !message.data || message.length<minimum || message.length>16 || !epoch || !sequence || !received) return VIM_MALFORMED;
    navigation::RawEvent e=navigation::RawEvent(); e.epoch=epoch;e.receive_seq=sequence;e.received_ns=received;
    // IPC carries original SPI bytes, not the separately generated TCP timestamp.
    e.source_mono_ms=0;
    if(message.id==0x100) { e.kind=navigation::WHEELS;for(unsigned i=0;i<4;++i)e.raw[i]=le16(message.data+1+2*i); }
    else if(message.id==0x116) { e.kind=navigation::YAW;e.raw[0]=le16(message.data+1);e.count=message.data[3]; }
    else { e.kind=navigation::REVERSE;e.reverse=message.data[1]; }
    *out=e;return VIM_DECODED;
}
} }
