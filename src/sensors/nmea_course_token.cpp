#include "sensors/nmea_course_token.h"

namespace mx5 { namespace sensors { namespace nmea_course_token {

namespace {
bool hex(unsigned char value) {
    return (value>='0' && value<='9') || (value>='a' && value<='f') ||
           (value>='A' && value<='F');
}
}

Presence classify(const char* input, size_t accessible) noexcept {
    if(!input || !accessible)return UNKNOWN;
    const size_t bound=accessible<SCAN_LIMIT?accessible:size_t(SCAN_LIMIT);
    size_t length=0;
    while(length<bound && input[length])++length;
    if(length==bound || length<10)return UNKNOWN;
    if(input[length-2]=='\r' && input[length-1]=='\n')length-=2;
    if(input[0]!='$' || input[1]<'A' || input[1]>'Z' ||
       input[2]<'A' || input[2]>'Z' || input[3]!='R' || input[4]!='M' ||
       input[5]!='C' || input[6]!=',')return UNKNOWN;

    // Validate the complete frame before returning a token result. Otherwise
    // a good prefix could hide a truncated trailer or a second sentence.
    size_t star=7;
    for(;star<length && input[star]!='*';++star) {
        const unsigned char value=static_cast<unsigned char>(input[star]);
        if(value<0x20 || value>0x7e || value=='$')return UNKNOWN;
    }
    if(star+3!=length || !hex(static_cast<unsigned char>(input[star+1])) ||
       !hex(static_cast<unsigned char>(input[star+2])))return UNKNOWN;

    unsigned field=1;
    size_t begin=7;
    for(size_t i=7;i<=star;++i) {
        if(i!=star && input[i]!=',')continue;
        if(field==8)return i==begin?EMPTY:PRESENT;
        ++field;begin=i+1;
    }
    return UNKNOWN; // The eighth data field was never reached.
}

} } }
