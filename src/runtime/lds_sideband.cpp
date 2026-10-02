#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "lds_sideband.h"
#include "request_log.h"
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

namespace mx5 { namespace runtime { namespace lds_sideband {
namespace {
struct Errno { int saved;Errno():saved(errno){} ~Errno(){errno=saved;} };
uint64_t now_ns() {
    timespec t;
    return clock_gettime(CLOCK_MONOTONIC,&t)?0:uint64_t(t.tv_sec)*1000000000ULL+t.tv_nsec;
}
void put(unsigned char* p,uint64_t n,unsigned width) {
    for(unsigned i=0;i<width;++i) { p[i]=static_cast<unsigned char>(n);n>>=8; }
}
uint64_t get(const unsigned char* p,unsigned width) {
    uint64_t n=0;for(unsigned i=0;i<width;++i)n|=uint64_t(p[i])<<(8*i);return n;
}
bool zero(const unsigned char* p,size_t n) {
    for(size_t i=0;i<n;++i)if(p[i])return false;
    return true;
}
bool put_text(unsigned char* p,const Text& t) {
    if(!t.known)return !t.complete;
    const size_t n=strnlen(t.bytes,sizeof t.bytes);
    if(n==sizeof t.bytes)return false;
    p[0]=1;p[1]=t.complete?1:0;memcpy(p+4,t.bytes,n);return true;
}
bool get_text(const unsigned char* p,Text* t) {
    if(p[0]>1||p[1]>1||p[2]||p[3])return false;
    if(!p[0])return !p[1]&&zero(p+4,64);
    const size_t n=strnlen(reinterpret_cast<const char*>(p+4),64);
    if(n==64||!zero(p+4+n,64-n))return false;
    t->known=true;t->complete=p[1]!=0;memcpy(t->bytes,p+4,n);return true;
}
void put_double(unsigned char* p,double value) {
    static_assert(sizeof(double)==8,"LDS wire requires IEEE binary64");
    uint64_t bits;memcpy(&bits,&value,8);put(p,bits,8);
}
double get_double(const unsigned char* p) {
    const uint64_t bits=get(p,8);double value;memcpy(&value,&bits,8);return value;
}
bool address(const char* name,sockaddr_un* out,socklen_t* length) {
    if(!name)return false;
    const size_t n=strnlen(name,96);
    if(!n||n==96)return false;
    memset(out,0,sizeof *out);out->sun_family=AF_UNIX;memcpy(out->sun_path+1,name,n);
    *length=static_cast<socklen_t>(offsetof(sockaddr_un,sun_path)+1+n);return true;
}
void add_number(request_log_detail::Json& j,const char* name,double value) {
    char text[48];
    if(std::isfinite(value))snprintf(text,sizeof text,"%.17g",value);
    else strcpy(text,"null");
    j.add(",\"");j.add(name);j.add("\":");j.add(text);
}
void array(request_log_detail::Json& j,const Record& r,bool clock) {
    j.add("[");
    for(unsigned i=0;i<sensors::lds_lineage::FIELD_COUNT;++i) {
        if(i)j.add(",");
        char text[32];snprintf(text,sizeof text,"%llu",(unsigned long long)
            (clock?r.field_lineage.fields[i].observed_ns:r.field_lineage.fields[i].write_sequence));
        j.add(text);
    }
    j.add("]");
}
}

void copy_text(Text* out,const char* borrowed) {
    if(!out)return;
    *out=Text();if(!borrowed)return;
    out->known=true;
    size_t n=0;while(n+1<sizeof out->bytes&&borrowed[n]) { out->bytes[n]=borrowed[n];++n; }
    out->complete=borrowed[n]==0;
}
bool encode(const Record& r,unsigned char out[RECORD_SIZE]) {
    if(!out||!r.source_instance||!r.sequence||r.flags>255)return false;
    const auto presence=r.field_lineage.heading_presence;
    if(presence>sensors::nmea_course_token::PRESENT ||
       (presence!=sensors::nmea_course_token::UNKNOWN&&!r.field_lineage.fields[sensors::lds_lineage::HEADING].write_sequence))return false;
    const auto status=r.field_lineage.heading_rmc_status;
    if(status>sensors::nmea_course_token::RMC_OTHER ||
       (status!=sensors::nmea_course_token::RMC_UNKNOWN&&!r.field_lineage.fields[sensors::lds_lineage::HEADING].write_sequence))return false;
    memset(out,0,RECORD_SIZE);memcpy(out,"MXLD",4);put(out+4,WIRE_VERSION,2);put(out+6,RECORD_SIZE,2);
    put(out+8,r.source_instance,8);put(out+16,r.sequence,8);put(out+24,r.dropped_before,8);
    put(out+32,r.observed_ns,8);put(out+40,r.flags,4);put(out+44,uint32_t(r.path_result),4);
    put(out+48,uint32_t(r.send_result),4);put(out+52,uint32_t(r.reply_type),4);
    put(out+56,r.wire.request_serial,4);put(out+60,r.wire.response_serial,4);put(out+64,r.wire.reply_serial,4);
    if(!put_text(out+72,r.wire.server_guid)||!put_text(out+140,r.wire.client_unique)||
       !put_text(out+208,r.wire.server_unique)||!put_text(out+276,r.wire.destination))return false;
    put(out+344,r.field_lineage.lifetime,8);put(out+352,r.field_lineage.write_sequence,8);
    for(unsigned i=0;i<sensors::lds_lineage::FIELD_COUNT;++i) {
        put(out+360+16*i,r.field_lineage.fields[i].write_sequence,8);
        put(out+368+16*i,r.field_lineage.fields[i].observed_ns,8);
    }
    put(out+504,uint32_t(r.position.mode),4);put(out+508,uint32_t(r.position.altitude_m),4);
    put(out+512,r.position.utc_seconds,8);
    put_double(out+520,r.position.latitude_deg);put_double(out+528,r.position.longitude_deg);
    put_double(out+536,r.position.heading_deg);put_double(out+544,r.position.velocity_kmh);
    put_double(out+552,r.position.horizontal);put_double(out+560,r.position.vertical);
    put(out+568,uint32_t(presence),4);put(out+572,uint32_t(status),4);
    return true;
}
bool decode(const unsigned char* p,size_t n,Record* out) {
    if(!out)return false;
    *out=Record();
    if(!p||n!=RECORD_SIZE||memcmp(p,"MXLD",4)||get(p+6,2)!=RECORD_SIZE||
       !zero(p+68,4))return false;
    const uint64_t version=get(p+4,2);
    if((version!=1&&version!=2&&version!=WIRE_VERSION)||!zero(p+576,64)||
       (version<3&&!zero(p+572,4))||(version==1&&!zero(p+568,4)))return false;
    Record r=Record();
    r.source_instance=get(p+8,8);r.sequence=get(p+16,8);r.dropped_before=get(p+24,8);
    r.observed_ns=get(p+32,8);r.flags=uint32_t(get(p+40,4));
    if(!r.source_instance||!r.sequence||r.flags>255)return false;
    r.path_result=int32_t(get(p+44,4));r.send_result=int32_t(get(p+48,4));r.reply_type=int32_t(get(p+52,4));
    r.wire.request_serial=uint32_t(get(p+56,4));r.wire.response_serial=uint32_t(get(p+60,4));r.wire.reply_serial=uint32_t(get(p+64,4));
    if(!get_text(p+72,&r.wire.server_guid)||!get_text(p+140,&r.wire.client_unique)||
       !get_text(p+208,&r.wire.server_unique)||!get_text(p+276,&r.wire.destination))return false;
    r.field_lineage.lifetime=get(p+344,8);r.field_lineage.write_sequence=get(p+352,8);
    for(unsigned i=0;i<sensors::lds_lineage::FIELD_COUNT;++i) {
        r.field_lineage.fields[i].write_sequence=get(p+360+16*i,8);
        r.field_lineage.fields[i].observed_ns=get(p+368+16*i,8);
    }
    const uint64_t presence=version==1?0:get(p+568,4);
    if(presence>sensors::nmea_course_token::PRESENT ||
       (presence&&!r.field_lineage.fields[sensors::lds_lineage::HEADING].write_sequence))return false;
    r.field_lineage.heading_presence=static_cast<sensors::nmea_course_token::Presence>(presence);
    const uint64_t status=version<3?0:get(p+572,4);
    if(status>sensors::nmea_course_token::RMC_OTHER ||
       (status&&!r.field_lineage.fields[sensors::lds_lineage::HEADING].write_sequence))return false;
    r.field_lineage.heading_rmc_status=static_cast<sensors::nmea_course_token::RmcStatus>(status);
    r.position.mode=int32_t(get(p+504,4));r.position.altitude_m=int32_t(get(p+508,4));
    r.position.utc_seconds=get(p+512,8);r.position.latitude_deg=get_double(p+520);
    r.position.longitude_deg=get_double(p+528);r.position.heading_deg=get_double(p+536);
    r.position.velocity_kmh=get_double(p+544);r.position.horizontal=get_double(p+552);
    r.position.vertical=get_double(p+560);*out=r;return true;
}
const char* receive_fault_name(ReceiveFault f) {
    switch(f) {
    case RECEIVE_OK:return "none";case SYSCALL_FAILED:return "syscall_failed";
    case TRUNCATED:return "truncated";case NO_CREDENTIALS:return "credentials_missing";
    case WRONG_CREDENTIALS:return "credentials_mismatch";case BAD_RECORD:return "bad_record";
    }
    return "unknown";
}
ReceiveResult inspect(const Datagram& p,uid_t expected_uid,Record* out,Diagnostic* diagnostic) {
    if(out)*out=Record();
    Diagnostic d=Diagnostic();d.sender_pid=p.sender_pid;d.sender_uid=p.sender_uid;
    if(p.truncated)d.fault=TRUNCATED;
    else if(!p.credentials_present)d.fault=NO_CREDENTIALS;
    else if(p.sender_pid<=0||p.sender_uid!=expected_uid)d.fault=WRONG_CREDENTIALS;
    else if(!decode(p.bytes,p.size,out))d.fault=BAD_RECORD;
    if(diagnostic)*diagnostic=d;
    return d.fault==RECEIVE_OK?RECORD:REJECTED;
}
Receiver::Receiver():fd_(-1),expected_uid_(LDS_UID) {}
Receiver::~Receiver() { close_channel(); }
void Receiver::close_channel() { Errno saved;if(fd_>=0)close(fd_);fd_=-1; }
bool Receiver::open_channel(const char* name,uid_t uid) {
    if(fd_>=0)return false;
    sockaddr_un addr;socklen_t len;
    if(!address(name,&addr,&len)) { errno=EINVAL;return false; }
    const int fd=socket(AF_UNIX,SOCK_DGRAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
    if(fd<0)return false;
    int yes=1;
    if(setsockopt(fd,SOL_SOCKET,SO_PASSCRED,&yes,sizeof yes)||bind(fd,reinterpret_cast<sockaddr*>(&addr),len)) {
        const int error=errno;close(fd);errno=error;return false;
    }
    fd_=fd;expected_uid_=uid;return true;
}
ReceiveResult Receiver::receive(Record* out,Diagnostic* diagnostic) {
    if(out)*out=Record();
    Diagnostic d=Diagnostic();
    unsigned char bytes[RECORD_SIZE];
    union { cmsghdr alignment;char bytes[CMSG_SPACE(sizeof(ucred))]; } control;
    iovec io={bytes,sizeof bytes};msghdr msg=msghdr();msg.msg_iov=&io;msg.msg_iovlen=1;
    msg.msg_control=control.bytes;msg.msg_controllen=sizeof control.bytes;
    const ssize_t n=recvmsg(fd_,&msg,MSG_DONTWAIT);
    const int receive_errno=n<0?errno:0;
    d.received_ns=now_ns();
    if(n<0) {
        d.syscall_errno=receive_errno;
        d.fault=SYSCALL_FAILED;
        if(diagnostic)*diagnostic=d;
        return receive_errno==EAGAIN||receive_errno==EWOULDBLOCK||receive_errno==EINTR?EMPTY:REJECTED;
    }
    Datagram packet={bytes,size_t(n),(msg.msg_flags&(MSG_TRUNC|MSG_CTRUNC))!=0,false,0,0};
    for(cmsghdr* c=CMSG_FIRSTHDR(&msg);c;c=CMSG_NXTHDR(&msg,c)) {
        if(c->cmsg_level==SOL_SOCKET&&c->cmsg_type==SCM_CREDENTIALS&&c->cmsg_len==CMSG_LEN(sizeof(ucred))) {
            ucred credentials;memcpy(&credentials,CMSG_DATA(c),sizeof credentials);
            packet.credentials_present=true;packet.sender_pid=credentials.pid;packet.sender_uid=credentials.uid;
        }
    }
    const uint64_t received=d.received_ns;
    const ReceiveResult result=inspect(packet,expected_uid_,out,&d);d.received_ns=received;
    if(diagnostic)*diagnostic=d;
    return result;
}
Sender::Sender():fd_(-1),name_(),instance_(0),sequence_(0),busy_(0),dropped_(0),saturated_(false) {}
Sender::~Sender() { Errno saved;if(fd_>=0)close(fd_); }
void Sender::loss() {
    // Bounded even under contention, and never wraps to an apparently small
    // complete count. The saturated marker means the count is no longer exact
    // (numeric exhaustion or this bounded accounting update could not finish).
    uint32_t previous=dropped_.load(std::memory_order_relaxed);
    for(unsigned attempt=0;attempt<4&&previous<UINT32_MAX;++attempt)
        if(dropped_.compare_exchange_weak(previous,previous+1,std::memory_order_relaxed))return;
    saturated_.store(true,std::memory_order_relaxed);
    dropped_.store(UINT32_MAX,std::memory_order_relaxed);
}
bool Sender::open_channel(const char* name,uint64_t instance) {
    Errno saved;
    if(fd_>=0)return false;
    sockaddr_un addr;socklen_t len;
    if(!address(name,&addr,&len))return false;
    if(!instance)instance=now_ns();
    if(!instance)return false;
    const int fd=socket(AF_UNIX,SOCK_DGRAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
    if(fd<0)return false;
    strcpy(name_,name);instance_=instance;sequence_=0;fd_=fd;return true;
}
bool Sender::try_send(const Record& input) {
    Errno saved;
    unsigned idle=0;
    if(!busy_.compare_exchange_strong(idle,1,std::memory_order_acquire)) { loss();return false; }
    struct Unlock { std::atomic<unsigned>& busy;~Unlock(){busy.store(0,std::memory_order_release);} } unlock={busy_};
    if(fd_<0||sequence_==UINT64_MAX) { loss();return false; }
    Record r=input;r.source_instance=instance_;r.sequence=++sequence_;
    r.dropped_before=dropped_.load(std::memory_order_relaxed);
    if(r.dropped_before==UINT32_MAX||saturated_.load(std::memory_order_relaxed))
        r.flags|=LOSS_COUNTER_SATURATED;
    unsigned char bytes[RECORD_SIZE];
    if(!encode(r,bytes)) { loss();return false; }
    sockaddr_un addr;socklen_t len;
    if(!address(name_,&addr,&len)) { loss();return false; }
    if(sendto(fd_,bytes,sizeof bytes,MSG_DONTWAIT|MSG_NOSIGNAL,
              reinterpret_cast<sockaddr*>(&addr),len)!=ssize_t(sizeof bytes)) { loss();return false; }
    return true;
}
bool format_record(char* out,size_t cap,const Record& r,const Diagnostic& d) {
    const auto state=r.field_lineage.heading_presence;
    const auto status=r.field_lineage.heading_rmc_status;
    if(state>sensors::nmea_course_token::PRESENT||status>sensors::nmea_course_token::RMC_OTHER)return false;
    const char* labels[]={"unknown","empty","a","v","other"};
    const char* rmc=r.field_lineage.fields[sensors::lds_lineage::HEADING].write_sequence?labels[status]:"unknown";
    const char* presence=state==sensors::nmea_course_token::EMPTY?"empty":
        state==sensors::nmea_course_token::PRESENT?"present":"unknown";
    request_log_detail::Json j(out,cap);
    j.add("{\"kind\":\"lds_sideband\",\"schema\":1,\"association_only\":true,\"assist_ready\":false,\"producer_time_status\":\"unknown\"");
    j.number("mono_ns",d.received_ns);j.signed_number("sender_pid",d.sender_pid,true);j.number("sender_uid",d.sender_uid);
    j.number("source_instance",r.source_instance);j.number("sequence",r.sequence);j.number("dropped_before",r.dropped_before);
    j.number("observed_ns",r.observed_ns);j.number("flags",r.flags);
    j.signed_number("path_result",r.path_result,true);j.signed_number("send_result",r.send_result,true);j.signed_number("reply_type",r.reply_type,true);
    j.add(",\"wire\":{");j.text("server_guid",r.wire.server_guid,true);j.text("client_unique",r.wire.client_unique);
    j.text("server_unique",r.wire.server_unique);j.text("destination",r.wire.destination);
    j.number("request_serial",r.wire.request_serial);j.number("response_serial",r.wire.response_serial);j.number("reply_serial",r.wire.reply_serial);
    j.add("},\"field_lineage\":{");
    // Json::number prefixes a comma; begin this object with its literal first key.
    j.add("\"association_only\":true");j.number("lifetime",r.field_lineage.lifetime);j.number("write_sequence",r.field_lineage.write_sequence);
    j.add(",\"field_write_sequences\":");array(j,r,false);j.add(",\"field_observed_ns\":");array(j,r,true);
    j.add(",\"heading_presence\":\"");j.add(presence);j.add("\"");
    j.add(",\"heading_rmc_status\":\"");j.add(rmc);j.add("\"");
    j.add("},\"position\":{\"snapshot_known\":");j.add(r.flags&SNAPSHOT_KNOWN?"true":"false");
    j.signed_number("mode",r.position.mode,true);j.number("utc_s",r.position.utc_seconds);
    add_number(j,"lat",r.position.latitude_deg);add_number(j,"lon",r.position.longitude_deg);
    j.signed_number("altitude_m",r.position.altitude_m,true);add_number(j,"heading",r.position.heading_deg);
    add_number(j,"kmh",r.position.velocity_kmh);add_number(j,"horizontal",r.position.horizontal);add_number(j,"vertical",r.position.vertical);
    j.add("}}");return j.ok();
}
bool format_status(char* out,size_t cap,const char* status,const Diagnostic& d,uint64_t count) {
    const int n=snprintf(out,cap,
        "{\"kind\":\"lds_sideband_status\",\"schema\":1,\"association_only\":true,\"assist_ready\":false,"
        "\"mono_ns\":%llu,\"status\":\"%s\",\"reason\":\"%s\",\"sender_pid\":%ld,\"sender_uid\":%lu,"
        "\"syscall_errno\":%d,\"count\":%llu}",
        (unsigned long long)d.received_ns,status,receive_fault_name(d.fault),(long)d.sender_pid,
        (unsigned long)d.sender_uid,d.syscall_errno,(unsigned long long)count);
    return n>0&&size_t(n)<cap;
}
} } }
