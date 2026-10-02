#include "sensors/nmea_course_token.h"

namespace mx5 { namespace sensors { namespace nmea_course_token {

namespace {
bool hex(unsigned char value) {
    return (value>='0' && value<='9') || (value>='a' && value<='f') ||
           (value>='A' && value<='F');
}
}

Tokens classify_rmc(const char* input, size_t accessible) noexcept {
    Tokens tokens={UNKNOWN,RMC_UNKNOWN};
    if(!input || !accessible)return tokens;
    const size_t bound=accessible<SCAN_LIMIT?accessible:size_t(SCAN_LIMIT);
    size_t length=0;
    while(length<bound && input[length])++length;
    if(length==bound || length<10)return tokens;
    if(input[length-2]=='\r' && input[length-1]=='\n')length-=2;
    if(input[0]!='$' || input[1]<'A' || input[1]>'Z' ||
       input[2]<'A' || input[2]>'Z' || input[3]!='R' || input[4]!='M' ||
       input[5]!='C' || input[6]!=',')return tokens;

    // Validate the complete frame before returning a token result. Otherwise
    // a good prefix could hide a truncated trailer or a second sentence.
    size_t star=7;
    for(;star<length && input[star]!='*';++star) {
        const unsigned char value=static_cast<unsigned char>(input[star]);
        if(value<0x20 || value>0x7e || value=='$')return tokens;
    }
    if(star+3!=length || !hex(static_cast<unsigned char>(input[star+1])) ||
       !hex(static_cast<unsigned char>(input[star+2])))return tokens;

    unsigned field=1;
    size_t begin=7;
    for(size_t i=7;i<=star;++i) {
        if(i!=star && input[i]!=',')continue;
        if(field==2) {
            if(i==begin)tokens.status=RMC_EMPTY;
            else if(i==begin+1 && input[begin]=='A')tokens.status=RMC_A;
            else if(i==begin+1 && input[begin]=='V')tokens.status=RMC_V;
            else tokens.status=RMC_OTHER;
        }
        if(field==8) { tokens.course=i==begin?EMPTY:PRESENT;return tokens; }
        ++field;begin=i+1;
    }
    return tokens; // Reached status is independent of a missing eighth field.
}

Presence classify(const char* input, size_t accessible) noexcept {
    return classify_rmc(input,accessible).course;
}

} } }
