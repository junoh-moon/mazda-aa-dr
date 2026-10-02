#include "runtime/lds_sideband.h"
#include <cassert>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <unistd.h>
namespace L=mx5::runtime::lds_sideband;
namespace N=mx5::sensors::nmea_course_token;
namespace mx5 { namespace runtime { namespace lds_sideband {
struct SenderTestAccess {
    static void dropped(Sender& s,uint32_t n) { s.dropped_.store(n); }
    static void busy(Sender& s,unsigned n) { s.busy_.store(n); }
};
} } }
static unsigned checks;
#define CHECK(e) do { ++checks;assert(e); } while(0)
static L::Record example() {
    L::Record r=L::Record();r.source_instance=1;r.sequence=1;r.observed_ns=104;r.flags=111;
    r.path_result=0;r.send_result=1;r.reply_type=2;
    L::copy_text(&r.wire.server_guid,"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    L::copy_text(&r.wire.client_unique,":1.42");L::copy_text(&r.wire.server_unique,":1.8");
    L::copy_text(&r.wire.destination,":1.42");
    r.wire.request_serial=23;r.wire.response_serial=31;r.wire.reply_serial=23;
    r.field_lineage.lifetime=2;r.field_lineage.write_sequence=3;
    const unsigned origin[9]={1,1,3,3,3,1,1,2,2};
    for(unsigned i=0;i<9;++i) { r.field_lineage.fields[i].write_sequence=origin[i];r.field_lineage.fields[i].observed_ns=100; }
    r.position.mode=1;r.position.utc_seconds=1;r.position.latitude_deg=35;r.position.longitude_deg=129;
    r.position.altitude_m=12;r.position.heading_deg=20;r.position.velocity_kmh=18;
    r.position.horizontal=1;r.position.vertical=1.5;return r;
}
static void codec() {
    L::Record r=example(),out=L::Record();unsigned char bytes[L::RECORD_SIZE],again[L::RECORD_SIZE];
    CHECK(L::encode(r,bytes));CHECK(!memcmp(bytes,"MXLD",4));
    CHECK(bytes[4]==2&&bytes[5]==0&&bytes[6]==128&&bytes[7]==2);
    CHECK(bytes[8]==1&&bytes[16]==1&&bytes[40]==111&&bytes[56]==23&&bytes[60]==31);
    for(unsigned i=568;i<L::RECORD_SIZE;++i)CHECK(bytes[i]==0);
    CHECK(L::decode(bytes,sizeof bytes,&out));CHECK(L::encode(out,again));CHECK(!memcmp(bytes,again,sizeof bytes));
    CHECK(!L::decode(bytes,sizeof bytes-1,&out));CHECK(out.sequence==0);
    CHECK(!L::decode(bytes,sizeof bytes+1,&out));CHECK(!L::decode(0,sizeof bytes,&out));
    CHECK(!L::decode(bytes,sizeof bytes,0));CHECK(!L::encode(r,0));
    const unsigned bad[]={0,4,6,68,69,70,71,72,74,75,568,639};
    for(unsigned i=0;i<sizeof bad/sizeof bad[0];++i) {
        memcpy(again,bytes,sizeof bytes);again[bad[i]]=0xff;
        CHECK(!L::decode(again,sizeof again,&out));CHECK(out.source_instance==0);
    }
    memcpy(again,bytes,sizeof bytes);again[72]=0;CHECK(!L::decode(again,sizeof again,&out));
    memcpy(again,bytes,sizeof bytes);memset(again+76,'x',64);CHECK(!L::decode(again,sizeof again,&out));
    r.source_instance=r.sequence=r.dropped_before=r.observed_ns=UINT64_MAX;r.flags=255;
    r.path_result=INT32_MIN;r.send_result=INT32_MAX;r.reply_type=-1;
    r.position.mode=INT32_MIN;r.position.altitude_m=INT32_MAX;r.position.utc_seconds=UINT64_MAX;
    r.position.latitude_deg=std::numeric_limits<double>::quiet_NaN();
    r.position.longitude_deg=-std::numeric_limits<double>::infinity();r.position.heading_deg=-0.0;
    CHECK(L::encode(r,bytes));CHECK(L::decode(bytes,sizeof bytes,&out));CHECK(L::encode(out,again));
    CHECK(!memcmp(bytes,again,sizeof bytes));CHECK(std::isnan(out.position.latitude_deg));
    CHECK(std::isinf(out.position.longitude_deg)&&out.position.longitude_deg<0);
    CHECK(std::signbit(out.position.heading_deg));CHECK(out.position.utc_seconds==UINT64_MAX);
    CHECK(out.path_result==INT32_MIN&&out.send_result==INT32_MAX);
    r.sequence=0;CHECK(!L::encode(r,bytes));r.sequence=1;r.flags=256;CHECK(!L::encode(r,bytes));
    L::Text t;char borrowed[80];memset(borrowed,'q',sizeof borrowed);borrowed[79]=0;
    L::copy_text(&t,borrowed);CHECK(t.known&&!t.complete&&strlen(t.bytes)==63);
    memset(borrowed,'r',63);CHECK(t.bytes[0]=='q');L::copy_text(&t,0);CHECK(!t.known&&!t.complete);
}
static void heading_presence() {
    for(unsigned value=0;value<=2;++value) {
        L::Record r=example(),out=L::Record();unsigned char bytes[L::RECORD_SIZE];
        r.field_lineage.heading_presence=N::Presence(value);r.position.heading_deg=0;
        CHECK(L::encode(r,bytes));CHECK(L::decode(bytes,sizeof bytes,&out));
        CHECK(out.field_lineage.heading_presence==N::Presence(value));
        CHECK(out.position.heading_deg==0&&out.field_lineage.fields[5].write_sequence==1);
        CHECK(bytes[4]==2&&bytes[568]==value);
        bytes[568]=3;CHECK(!L::decode(bytes,sizeof bytes,&out));
        CHECK(out.source_instance==0);
        // Legacy v1 really lacked the field; never infer presence from heading0.
        bytes[4]=1;memset(bytes+568,0,72);
        CHECK(L::decode(bytes,sizeof bytes,&out));CHECK(out.field_lineage.heading_presence==N::UNKNOWN);
        CHECK(out.position.heading_deg==0&&out.field_lineage.fields[5].write_sequence==1);
        bytes[568]=1;CHECK(!L::decode(bytes,sizeof bytes,&out));
    }
    L::Record invalid=example();unsigned char bytes[L::RECORD_SIZE];
    invalid.field_lineage.heading_presence=N::Presence(3);CHECK(!L::encode(invalid,bytes));
    invalid.field_lineage.heading_presence=N::EMPTY;invalid.field_lineage.fields[5]={0,0};
    CHECK(!L::encode(invalid,bytes));
}
static void credentials() {
    L::Record r=example(),out;unsigned char bytes[L::RECORD_SIZE];CHECK(L::encode(r,bytes));
    L::Diagnostic d;L::Datagram p={bytes,sizeof bytes,false,true,123,0};
    CHECK(L::inspect(p,0,&out,&d)==L::RECORD);CHECK(d.fault==L::RECEIVE_OK&&d.sender_pid==123);
    p.credentials_present=false;CHECK(L::inspect(p,0,&out,&d)==L::REJECTED);CHECK(d.fault==L::NO_CREDENTIALS);
    p.credentials_present=true;p.sender_uid=1001;
    CHECK(L::inspect(p,0,&out,&d)==L::REJECTED);CHECK(d.fault==L::WRONG_CREDENTIALS&&out.sequence==0);
    p.sender_uid=0;p.sender_pid=0;CHECK(L::inspect(p,0,&out,&d)==L::REJECTED);
    p.sender_pid=123;p.truncated=true;CHECK(L::inspect(p,0,&out,&d)==L::REJECTED);CHECK(d.fault==L::TRUNCATED);
    p.truncated=false;p.size=639;CHECK(L::inspect(p,0,&out,&d)==L::REJECTED);CHECK(d.fault==L::BAD_RECORD);
}
static void channel() {
    char name[80];snprintf(name,sizeof name,"mx5dr.lds.test.%ld",(long)getpid());
    L::Sender sender;L::Receiver receiver,second;L::Record r=example(),out;L::Diagnostic d;
    errno=E2BIG;CHECK(!sender.try_send(r));CHECK(errno==E2BIG);
    CHECK(sender.open_channel(name,44));CHECK(errno==E2BIG);
    CHECK(!sender.try_send(r));CHECK(errno==E2BIG);
    CHECK(receiver.open_channel(name,geteuid()));CHECK(!second.open_channel(name,geteuid()));
    CHECK(receiver.receive(&out,&d)==L::EMPTY);
    CHECK(sender.try_send(r));CHECK(receiver.receive(&out,&d)==L::RECORD);
    CHECK(out.source_instance==44&&out.sequence==2&&out.dropped_before==2);
    CHECK(d.sender_pid==getpid()&&d.sender_uid==geteuid()&&d.received_ns>0);
    unsigned accepted=0;while(accepted<1024&&sender.try_send(r))++accepted;
    CHECK(accepted>0&&accepted<1024);
    for(unsigned i=0;i<accepted;++i)CHECK(receiver.receive(&out,&d)==L::RECORD);
    CHECK(receiver.receive(&out,&d)==L::EMPTY);CHECK(sender.try_send(r));
    CHECK(receiver.receive(&out,&d)==L::RECORD);CHECK(out.dropped_before==3&&out.sequence==accepted+4);
    L::SenderTestAccess::busy(sender,1);errno=ERANGE;CHECK(!sender.try_send(r));CHECK(errno==ERANGE);
    L::SenderTestAccess::busy(sender,0);CHECK(sender.try_send(r));CHECK(receiver.receive(&out,&d)==L::RECORD);
    CHECK(out.dropped_before==4);
    L::SenderTestAccess::dropped(sender,UINT32_MAX);
    L::Record invalid=r;invalid.flags=256;CHECK(!sender.try_send(invalid));CHECK(errno==ERANGE);
    CHECK(sender.try_send(r));CHECK(receiver.receive(&out,&d)==L::RECORD);
    CHECK(out.dropped_before==UINT32_MAX);CHECK(out.flags&L::LOSS_COUNTER_SATURATED);
    receiver.close_channel();CHECK(!sender.try_send(r));
    CHECK(receiver.open_channel(name,geteuid()+1));CHECK(sender.try_send(r));
    CHECK(receiver.receive(&out,&d)==L::REJECTED);CHECK(d.fault==L::WRONG_CREDENTIALS);
}
static void formatter() {
    L::Record r=example();L::Diagnostic d={L::RECEIVE_OK,200,0,0,105};
    char output[L::JSON_CAPACITY];CHECK(L::format_record(output,sizeof output,r,d));
    CHECK(strstr(output,"\"association_only\":true"));CHECK(strstr(output,"\"assist_ready\":false"));
    CHECK(strstr(output,"\"heading_presence\":\"unknown\""));
    r.field_lineage.heading_presence=N::EMPTY;
    CHECK(L::format_record(output,sizeof output,r,d));CHECK(strstr(output,"\"heading_presence\":\"empty\""));
    r.field_lineage.heading_presence=N::PRESENT;
    CHECK(L::format_record(output,sizeof output,r,d));CHECK(strstr(output,"\"heading_presence\":\"present\""));
    r.source_instance=r.sequence=r.dropped_before=r.observed_ns=UINT64_MAX;r.flags=255;
    r.path_result=r.send_result=r.reply_type=INT32_MIN;
    r.wire.request_serial=r.wire.response_serial=r.wire.reply_serial=UINT32_MAX;
    L::Text* texts[]={&r.wire.server_guid,&r.wire.client_unique,&r.wire.server_unique,&r.wire.destination};
    for(unsigned i=0;i<4;++i) { texts[i]->known=texts[i]->complete=true;memset(texts[i]->bytes,0xff,63);texts[i]->bytes[63]=0; }
    r.field_lineage.lifetime=r.field_lineage.write_sequence=UINT64_MAX;
    for(unsigned i=0;i<9;++i)r.field_lineage.fields[i]={UINT64_MAX,UINT64_MAX};
    r.position.mode=r.position.altitude_m=INT32_MIN;r.position.utc_seconds=UINT64_MAX;
    r.position.latitude_deg=r.position.longitude_deg=r.position.heading_deg=-std::numeric_limits<double>::max();
    r.position.velocity_kmh=r.position.horizontal=r.position.vertical=std::numeric_limits<double>::denorm_min();
    d.sender_pid=INT32_MAX;d.sender_uid=UINT32_MAX;d.received_ns=UINT64_MAX;
    CHECK(L::format_record(output,sizeof output,r,d));const size_t n=strlen(output);
    char exact[L::JSON_CAPACITY+2];memset(exact,0x5a,sizeof exact);
    CHECK(L::format_record(exact+1,n+1,r,d));CHECK(exact[0]==0x5a&&exact[n+2]==0x5a);
    memset(exact,0x5a,sizeof exact);
    CHECK(!L::format_record(exact+1,n,r,d));CHECK(exact[0]==0x5a&&exact[n+1]==0x5a);
    CHECK(!L::format_record(0,0,r,d));
    r.position.latitude_deg=std::numeric_limits<double>::quiet_NaN();
    CHECK(L::format_record(output,sizeof output,r,d));CHECK(strstr(output,"\"lat\":null"));
    CHECK(L::format_status(output,sizeof output,"rejected",d,UINT64_MAX));
    printf("LDS formatter maximum fixture: %lu bytes / %u capacity\n",(unsigned long)n,unsigned(L::JSON_CAPACITY));
}
int main(int argc,char** argv) {
    if(argc==2&&!strcmp(argv[1],"--emit")) {
        const L::Record r=example();const L::Diagnostic d={L::RECEIVE_OK,200,0,0,105};
        char output[L::JSON_CAPACITY];CHECK(L::format_record(output,sizeof output,r,d));puts(output);return 0;
    }
    if(argc==2&&!strcmp(argv[1],"--heading")) {heading_presence();formatter();return 0;}
    CHECK(argc==1);alarm(10);heading_presence();codec();credentials();channel();formatter();
    printf("LDS sideband: %u checks passed\n",checks);return 0;
}
