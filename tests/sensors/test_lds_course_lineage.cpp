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
struct Case {const char* name;void(*run)();};
static const Case cases[]={{"assignment",assignment},{"exact_read",exact_read},
    {"unknown_reset",unknown_and_reset},{"invalid",invalid_metadata}};
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
