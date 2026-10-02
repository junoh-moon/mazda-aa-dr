#include "sensors/nmea_course_token.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>

namespace N=mx5::sensors::nmea_course_token;
static unsigned checks;
#define CHECK(expr) do { ++checks; if(!(expr)) { \
    fprintf(stderr,"line %d: %s\n",__LINE__,#expr);exit(1); } } while(0)

static std::string sentence(const std::string& token) {
    return "$GPRMC,120000,A,3500,N,13500,E,10,"+token+",011026,,,A*00";
}
static N::Presence classify(const std::string& value) {
    return N::classify(value.c_str(),value.size()+1);
}
static void distinguishes_presence() {
    CHECK(classify(sentence(""))==N::EMPTY);
    CHECK(classify(sentence("0"))==N::PRESENT);
    CHECK(classify(sentence("0.0"))==N::PRESENT);
    CHECK(classify(sentence("70"))==N::PRESENT);
}
static void presence_not_validity() {
    const char* tokens[]={"abc","nan","-1"," ","1e999999","+","inf",".0"};
    for(unsigned i=0;i<sizeof tokens/sizeof *tokens;++i)
        CHECK(classify(sentence(tokens[i]))==N::PRESENT);
    // Wrong checksum value and status V do not erase lexical presence.
    CHECK(classify("$GNRMC,t,V,x,N,y,E,s,0*ff")==N::PRESENT);
    CHECK(classify("$ZZRMC,t,V,x,N,y,E,s,*aB")==N::EMPTY);
}
static void field_boundaries() {
    CHECK(classify("$GPRMC,1,2,3,4,5,6,7*00")==N::UNKNOWN);
    CHECK(classify("$GPRMC,1,2,3,4,5,6,7,*00")==N::EMPTY);
    CHECK(classify("$GPRMC,1,2,3,4,5,6,7,0*00")==N::PRESENT);
    CHECK(classify("$GPRMC,1,2,3,4,5,6,0,,later*00")==N::EMPTY);
    CHECK(classify("$GPRMC,1,2,3,4,5,6,,x,*00")==N::PRESENT);
    CHECK(classify("$GPRMC,,,,,,,,*00")==N::EMPTY);
    CHECK(classify("$GPRMC,,,,,,,*00")==N::UNKNOWN);
}
static void frame_boundaries() {
    const std::string good=sentence("0");
    CHECK(classify(good+"\r\n")==N::PRESENT);
    const char* bad[]={"", "$GPRMC", "GPRMC,1,2,3,4,5,6,7,0*00",
        "$gpRMC,1,2,3,4,5,6,7,0*00", "$G1RMC,1,2,3,4,5,6,7,0*00",
        "$GPRMCA,1,2,3,4,5,6,7,0*00", "$GPGGA,1,2,3,4,5,6,7,0*00",
        "$GPRMC,1,2,3,4,5,6,7,0*0", "$GPRMC,1,2,3,4,5,6,7,0*GG",
        "$GPRMC,1,2,3,4,5,6,7,0*000", "$GPRMC,1,2,3,4,5,6,7,0**00",
        "$GPRMC,1,2,3,4,5,6,7,0,$x*00"};
    for(unsigned i=0;i<sizeof bad/sizeof *bad;++i)CHECK(classify(bad[i])==N::UNKNOWN);
    CHECK(classify(good+"\r")==N::UNKNOWN);
    CHECK(classify(good+"\n")==N::UNKNOWN);
    CHECK(classify(good+" ")==N::UNKNOWN);
    CHECK(classify(good+good)==N::UNKNOWN);
    CHECK(classify(good+"\r\n"+good)==N::UNKNOWN);
    CHECK(classify(sentence(std::string(1,'\t')))==N::UNKNOWN);
    CHECK(classify(sentence(std::string(1,char(0x80))))==N::UNKNOWN);
    CHECK(classify(sentence(std::string(1,char(0x7f))))==N::UNKNOWN);
}
static void truncated_and_capacity() {
    const std::string good=sentence("0");
    CHECK(N::classify(0,100)==N::UNKNOWN);
    CHECK(N::classify(good.c_str(),0)==N::UNKNOWN);
    for(size_t i=0;i<=good.size();++i) {
        CHECK(N::classify(good.c_str(),i)==N::UNKNOWN);
        CHECK(classify(good.substr(0,i))==(i==good.size()?N::PRESENT:N::UNKNOWN));
    }
    CHECK(N::classify(good.c_str(),good.size()+1)==N::PRESENT);
    char unterminated[N::SCAN_LIMIT];memset(unterminated,'x',sizeof unterminated);
    CHECK(N::classify(unterminated,sizeof unterminated)==N::UNKNOWN);
}
static void exact_scan_limit() {
    const std::string empty=sentence("");
    CHECK(empty.size()<N::SCAN_LIMIT-1);
    const std::string maximal=sentence(std::string(N::SCAN_LIMIT-1-empty.size(),'x'));
    CHECK(maximal.size()==N::SCAN_LIMIT-1);
    CHECK(classify(maximal)==N::PRESENT);
    const std::string overflow=sentence(std::string(N::SCAN_LIMIT-empty.size(),'x'));
    CHECK(overflow.size()==N::SCAN_LIMIT);
    CHECK(classify(overflow)==N::UNKNOWN);
}
static void first_nul_and_immutable() {
    const std::string good=sentence("");
    std::vector<char> bytes(good.begin(),good.end());bytes.push_back(0);
    const std::string next=sentence("0");bytes.insert(bytes.end(),next.begin(),next.end());bytes.push_back(0);
    const std::vector<char> before=bytes;
    errno=EINTR;
    CHECK(N::classify(bytes.data(),bytes.size())==N::EMPTY);
    CHECK(errno==EINTR);
    CHECK(bytes==before);
    CHECK(N::classify(bytes.data(),good.size()+1)==N::EMPTY);
    bytes[10]=0;
    CHECK(N::classify(bytes.data(),bytes.size())==N::UNKNOWN);
}
static void protected_read_bound() {
    const long page=sysconf(_SC_PAGESIZE);CHECK(page>0 && size_t(page)>N::SCAN_LIMIT);
    char* const memory=static_cast<char*>(mmap(0,size_t(page)*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    CHECK(memory!=MAP_FAILED);
    CHECK(mprotect(memory+page,size_t(page),PROT_NONE)==0);
    const std::string good=sentence("0");
    char* const terminated=memory+page-good.size()-1;
    memcpy(terminated,good.c_str(),good.size()+1);
    CHECK(N::classify(terminated,good.size()+1)==N::PRESENT);
    CHECK(N::classify_rmc(terminated,good.size()+1).status==N::RMC_A);
    char* const limited=memory+page-N::SCAN_LIMIT;
    memset(limited,'x',N::SCAN_LIMIT);
    // accessible is exactly the readable region; no probe beyond the cap.
    CHECK(N::classify(limited,N::SCAN_LIMIT)==N::UNKNOWN);
    CHECK(N::classify_rmc(limited,N::SCAN_LIMIT).status==N::RMC_UNKNOWN);
    char* const short_value=memory+page-8;
    memcpy(short_value,"$GPRMC,1",8);
    CHECK(N::classify(short_value,8)==N::UNKNOWN);
    CHECK(N::classify_rmc(short_value,8).status==N::RMC_UNKNOWN);
    CHECK(munmap(memory,size_t(page)*2)==0);
}
static void status_tokens() {
    const char* tokens[]={"A","V","","a","v","AA","V "," ","N","nan"};
    const N::RmcStatus expected[]={N::RMC_A,N::RMC_V,N::RMC_EMPTY,
        N::RMC_OTHER,N::RMC_OTHER,N::RMC_OTHER,N::RMC_OTHER,N::RMC_OTHER,
        N::RMC_OTHER,N::RMC_OTHER};
    for(unsigned i=0;i<sizeof tokens/sizeof *tokens;++i) {
        const std::string frame=std::string("$GPRMC,120000,")+tokens[i]+",3500,N,13500,E,10,0*00";
        errno=EDOM;const N::Tokens actual=N::classify_rmc(frame.c_str(),frame.size()+1);
        CHECK(errno==EDOM);CHECK(actual.status==expected[i]);CHECK(actual.course==N::PRESENT);
        CHECK(N::classify(frame.c_str(),frame.size()+1)==actual.course);
    }
    const std::string empty=sentence("");
    const N::Tokens value=N::classify_rmc(empty.c_str(),empty.size()+1);
    CHECK(value.course==N::EMPTY && value.status==N::RMC_A);
    CHECK(N::classify_rmc("$GNRMC,1,V*ff",14).status==N::RMC_V); // syntax, not XOR or numeric validity
}
static void status_boundaries() {
    const char* short_frames[]={"$GPRMC,1,A*00","$GPRMC,1,V*00","$GPRMC,1,*00","$GPRMC,1*00"};
    const N::RmcStatus expected[]={N::RMC_A,N::RMC_V,N::RMC_EMPTY,N::RMC_UNKNOWN};
    for(unsigned i=0;i<4;++i) {
        const N::Tokens value=N::classify_rmc(short_frames[i],strlen(short_frames[i])+1);
        CHECK(value.course==N::UNKNOWN);CHECK(value.status==expected[i]);
    }
    const std::string frame=sentence("0");
    for(size_t n=0;n<=frame.size();++n) {
        const N::Tokens value=N::classify_rmc(frame.c_str(),n);
        CHECK(value.course==N::UNKNOWN && value.status==N::RMC_UNKNOWN);
    }
    const std::string malformed[]={frame+"\n",frame+frame,frame+"\r\n"+frame,
        "$GPRMC,1,A*GG","$GPRMC,1,A*0","$GPGGA,1,A*00"};
    for(unsigned i=0;i<sizeof malformed/sizeof *malformed;++i) {
        const N::Tokens value=N::classify_rmc(malformed[i].c_str(),malformed[i].size()+1);
        CHECK(value.course==N::UNKNOWN && value.status==N::RMC_UNKNOWN);
    }
    std::string joined=frame;joined.push_back(0);joined+="$GPRMC,1,V*00";
    const std::string before=joined;
    CHECK(N::classify_rmc(joined.data(),joined.size()).status==N::RMC_A);
    CHECK(joined==before);
    const std::string limit=sentence(std::string(N::SCAN_LIMIT-1-sentence("").size(),'x'));
    CHECK(N::classify_rmc(limit.c_str(),limit.size()+1).status==N::RMC_A);
    const std::string over=limit+" ";
    CHECK(N::classify_rmc(over.c_str(),over.size()+1).status==N::RMC_UNKNOWN);
    CHECK(N::classify_rmc(0,100).status==N::RMC_UNKNOWN);
}
struct Case { const char* name;void(*run)(); };
static const Case cases[]={
    {"presence",distinguishes_presence},{"not_validity",presence_not_validity},
    {"fields",field_boundaries},{"frames",frame_boundaries},
    {"capacity",truncated_and_capacity},{"limit",exact_scan_limit},
    {"immutable",first_nul_and_immutable},{"guard_page",protected_read_bound},
    {"status",status_tokens},{"status_bounds",status_boundaries}
};
int main(int argc,char** argv) {
    if(argc>2)return 2;
    unsigned ran=0;
    for(unsigned i=0;i<sizeof cases/sizeof *cases;++i) {
        if(argc==2 && strcmp(argv[1],cases[i].name))continue;
        cases[i].run();++ran;
    }
    if(!ran)return 2;
    printf("nmea_course_token: %u cases, %u checks PASS\n",ran,checks);
    return 0;
}
