#include "sensors/lds_lineage.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

namespace L=mx5::sensors::lds_lineage;
namespace N=mx5::sensors::nmea_course_token;
static unsigned checks;
#define CHECK(x) do { ++checks;if(!(x)){fprintf(stderr,"line%d: %s\n",__LINE__,#x);exit(1);} } while(0)
static uint32_t bit(L::Field field) { return uint32_t(1)<<field; }
static void assignment() {
    L::Ledger ledger;CHECK(ledger.begin_lifetime(1));
    CHECK(ledger.snapshot().heading_presence==N::UNKNOWN);
    L::Snapshot read=ledger.snapshot();errno=EDOM;
    CHECK(ledger.commit(&read,L::ALL_FIELDS,123,N::EMPTY)==L::COMMITTED);
    CHECK(errno==EDOM);CHECK(ledger.snapshot().heading_presence==N::EMPTY);
    CHECK(ledger.snapshot().fields[L::HEADING].write_sequence==1);
    const L::Snapshot before=ledger.snapshot();read=before;
    CHECK(ledger.commit(&read,bit(L::HEADING),124,N::PRESENT)==L::COMMITTED);
    CHECK(ledger.snapshot().heading_presence==N::PRESENT);
    CHECK(ledger.snapshot().fields[L::HEADING].write_sequence==2);
    CHECK(before.heading_presence==N::EMPTY&&before.write_sequence==1);
}
static void exact_read() {
    L::Ledger ledger;CHECK(ledger.begin_lifetime(2));L::Snapshot read=ledger.snapshot();
    CHECK(ledger.commit(&read,L::ALL_FIELDS,100,N::EMPTY)==L::COMMITTED);
    const L::Snapshot paused=ledger.snapshot();read=paused;
    CHECK(ledger.commit(&read,bit(L::HEADING),110,N::PRESENT)==L::COMMITTED);
    CHECK(ledger.snapshot().heading_presence==N::PRESENT);
    // A later GGA-shaped write restores the exact earlier heading ancestry.
    CHECK(ledger.commit(&paused,bit(L::ALTITUDE),120,N::PRESENT)==L::COMMITTED);
    CHECK(ledger.snapshot().heading_presence==N::EMPTY);
    CHECK(ledger.snapshot().fields[L::HEADING].write_sequence==1);
    CHECK(ledger.snapshot().fields[L::ALTITUDE].write_sequence==3);
    read=ledger.snapshot();CHECK(ledger.commit(&read,0,121,N::UNKNOWN)==L::COMMITTED);
    CHECK(ledger.snapshot().heading_presence==N::EMPTY);
}
static void unknown_and_reset() {
    L::Ledger ledger;CHECK(ledger.begin_lifetime(3));L::Snapshot read=ledger.snapshot();
    CHECK(ledger.commit(&read,L::ALL_FIELDS,100,N::PRESENT)==L::COMMITTED);
    read=ledger.snapshot();CHECK(ledger.commit(&read,bit(L::HEADING),101)==L::COMMITTED);
    CHECK(ledger.snapshot().heading_presence==N::UNKNOWN);
    read=ledger.snapshot();CHECK(ledger.commit(&read,bit(L::HEADING),102,N::EMPTY)==L::COMMITTED);
    CHECK(ledger.snapshot().heading_presence==N::EMPTY);
    CHECK(ledger.commit(0,L::ALL_FIELDS,103,N::PRESENT)==L::UNKNOWN_READ);
    CHECK(ledger.snapshot().heading_presence==N::UNKNOWN);
    CHECK(ledger.snapshot().fields[L::HEADING].write_sequence==0);
    read=ledger.snapshot();CHECK(ledger.commit(&read,L::ALL_FIELDS,104,N::EMPTY)==L::COMMITTED);
    CHECK(ledger.snapshot().heading_presence==N::EMPTY);
    CHECK(ledger.begin_lifetime(4));CHECK(ledger.snapshot().heading_presence==N::UNKNOWN);
    CHECK(ledger.commit(&read,L::ALL_FIELDS,105,N::PRESENT)==L::UNKNOWN_READ);
    CHECK(ledger.snapshot().heading_presence==N::UNKNOWN);
}
static void invalid_metadata() {
    L::Ledger ledger;CHECK(ledger.begin_lifetime(5));L::Snapshot read=ledger.snapshot();
    CHECK(ledger.commit(&read,L::ALL_FIELDS,100,N::Presence(3))==L::COMMITTED);
    CHECK(ledger.snapshot().heading_presence==N::UNKNOWN);
    // A caller-corrupted saved read cannot carry presence without an origin.
    read=ledger.snapshot();read.fields[L::HEADING].write_sequence=0;
    read.fields[L::HEADING].observed_ns=0;read.heading_presence=N::EMPTY;
    CHECK(ledger.commit(&read,bit(L::ALTITUDE),101)==L::UNKNOWN_READ);
    CHECK(ledger.snapshot().heading_presence==N::UNKNOWN);
    read=ledger.snapshot();read.heading_presence=N::Presence(3);
    CHECK(ledger.commit(&read,bit(L::ALTITUDE),102)==L::UNKNOWN_READ);
    read=ledger.snapshot();CHECK(ledger.commit(&read,L::ALL_FIELDS,103,N::PRESENT)==L::COMMITTED);
    read=ledger.snapshot();CHECK(ledger.commit(&read,L::ALL_FIELDS|512,104,N::EMPTY)==L::UNKNOWN_MASK);
    CHECK(ledger.snapshot().heading_presence==N::UNKNOWN);
}
static void status_inheritance() {
    const uint32_t rmc=0x06f,gga=0x01d,gsa=0x181;
    L::Ledger ledger;CHECK(ledger.begin_lifetime(8));L::Snapshot read=ledger.snapshot();
    CHECK(read.heading_rmc_status==N::RMC_UNKNOWN);
    errno=EDOM;
    CHECK(ledger.commit(&read,rmc,100,N::EMPTY,N::RMC_A)==L::COMMITTED);
    CHECK(errno==EDOM);
    const L::Snapshot old_a=ledger.snapshot();
    CHECK(old_a.heading_rmc_status==N::RMC_A);
    CHECK(ledger.commit(&old_a,rmc,110,N::PRESENT,N::RMC_V)==L::COMMITTED);
    const L::Snapshot newer_v=ledger.snapshot();
    CHECK(newer_v.heading_rmc_status==N::RMC_V);
    // An older actual read restores its own A ancestry, not ambient last V.
    CHECK(ledger.commit(&old_a,gga,120,N::PRESENT,N::RMC_V)==L::COMMITTED);
    read=ledger.snapshot();
    CHECK(read.heading_presence==N::EMPTY && read.heading_rmc_status==N::RMC_A);
    CHECK(read.fields[L::HEADING].write_sequence==1 && read.fields[L::MODE].write_sequence==3);
    // A later write from the captured V read retains V through GGA/GSA/GGA.
    CHECK(ledger.commit(&newer_v,gga,130,N::UNKNOWN,N::RMC_UNKNOWN)==L::COMMITTED);
    read=ledger.snapshot();CHECK(ledger.commit(&read,gsa,140)==L::COMMITTED);
    read=ledger.snapshot();CHECK(ledger.commit(&read,gga,150)==L::COMMITTED);
    read=ledger.snapshot();CHECK(read.heading_rmc_status==N::RMC_V && read.heading_presence==N::PRESENT);
    for(unsigned f=0;f<L::FIELD_COUNT;++f) {
        if(f==L::UTC||f==L::HEADING||f==L::VELOCITY)
            CHECK(read.fields[f].write_sequence==2 && read.fields[f].observed_ns==110);
    }
    CHECK(read.write_sequence==6);CHECK(old_a.heading_rmc_status==N::RMC_A);
    CHECK(ledger.commit(&read,0,151)==L::COMMITTED);
    read=ledger.snapshot();CHECK(read.heading_rmc_status==N::RMC_V);
    CHECK(ledger.commit(&read,rmc,160,N::PRESENT)==L::COMMITTED);
    CHECK(ledger.snapshot().heading_rmc_status==N::RMC_UNKNOWN);
}
static void status_invalid() {
    L::Ledger ledger;CHECK(ledger.begin_lifetime(9));L::Snapshot read=ledger.snapshot();
    CHECK(ledger.commit(&read,L::ALL_FIELDS,100,N::UNKNOWN,N::RMC_A)==L::COMMITTED);
    read=ledger.snapshot();CHECK(read.heading_presence==N::UNKNOWN && read.heading_rmc_status==N::RMC_A);
    // The two lexical fields are independent, but each known status needs origin.
    read.fields[L::HEADING]=L::FieldOrigin{0,0};
    CHECK(ledger.commit(&read,bit(L::ALTITUDE),101)==L::UNKNOWN_READ);
    CHECK(ledger.snapshot().heading_rmc_status==N::RMC_UNKNOWN);
    read=ledger.snapshot();CHECK(ledger.commit(&read,L::ALL_FIELDS,102,N::EMPTY,N::RMC_EMPTY)==L::COMMITTED);
    read=ledger.snapshot();CHECK(read.heading_rmc_status==N::RMC_EMPTY);
    read.heading_rmc_status=N::RmcStatus(5);
    CHECK(ledger.commit(&read,bit(L::ALTITUDE),103)==L::UNKNOWN_READ);
    read=ledger.snapshot();CHECK(ledger.commit(&read,L::ALL_FIELDS,104,N::PRESENT,N::RmcStatus(5))==L::COMMITTED);
    read=ledger.snapshot();CHECK(read.heading_rmc_status==N::RMC_UNKNOWN);
    CHECK(ledger.commit(&read,L::ALL_FIELDS,105,N::PRESENT,N::RMC_OTHER)==L::COMMITTED);
    read=ledger.snapshot();CHECK(read.heading_rmc_status==N::RMC_OTHER);
    CHECK(ledger.commit(0,L::ALL_FIELDS,106,N::PRESENT,N::RMC_A)==L::UNKNOWN_READ);
    CHECK(ledger.snapshot().heading_rmc_status==N::RMC_UNKNOWN);
    read=ledger.snapshot();CHECK(ledger.commit(&read,L::ALL_FIELDS,107,N::PRESENT,N::RMC_V)==L::COMMITTED);
    read=ledger.snapshot();CHECK(ledger.commit(&read,L::ALL_FIELDS|512,108,N::PRESENT,N::RMC_A)==L::UNKNOWN_MASK);
    CHECK(ledger.snapshot().heading_rmc_status==N::RMC_UNKNOWN);
    CHECK(ledger.begin_lifetime(10));CHECK(ledger.snapshot().heading_rmc_status==N::RMC_UNKNOWN);
    CHECK(ledger.commit(&read,L::ALL_FIELDS,109,N::PRESENT,N::RMC_A)==L::UNKNOWN_READ);
}
struct Case {const char* name;void(*run)();};
static const Case cases[]={{"assignment",assignment},{"exact_read",exact_read},
    {"unknown_reset",unknown_and_reset},{"invalid",invalid_metadata},
    {"status_inheritance",status_inheritance},{"status_invalid",status_invalid}};
int main(int argc,char** argv) {
    if(argc>2)return 2;
    unsigned ran=0;
    for(unsigned i=0;i<sizeof cases/sizeof *cases;++i) {
        if(argc==2&&strcmp(argv[1],cases[i].name))continue;
        cases[i].run();++ran;
    }
    if(!ran)return 2;
    printf("LDS course lineage: %u cases, %u checks PASS\n",ran,checks);return 0;
}
