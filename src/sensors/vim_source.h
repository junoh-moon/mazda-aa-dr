#ifndef MX5_VIM_SOURCE_H
#define MX5_VIM_SOURCE_H
#include "navigation/channel.h"
#include <stdint.h>
namespace mx5 { namespace sensors {
// VIMC callback view. On target ARM32: id+0, length+4, data pointer+8.
// Payload is copied from SPI+3, not the unrelated VDT public envelope.
struct VimMessage { uint32_t id, length; const unsigned char* data; };
enum DecodeResult { VIM_IGNORED, VIM_DECODED, VIM_MALFORMED };
DecodeResult decode_vim_message(const VimMessage&, uint64_t epoch,
    uint64_t sequence, uint64_t received_ns, navigation::RawEvent*);
typedef void (*VimCallback)(uint64_t,uint64_t,const VimMessage*);
struct VimCallbacks { VimCallback app_event; };
typedef int (*VimAddClient)(uint64_t,uint64_t,const VimCallbacks*);
} }
#endif
