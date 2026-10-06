#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "channel.h"
#include "runtime/motion_gap.h"
#include <cerrno>
#include <climits>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

namespace mx5 { namespace navigation {
namespace {
uint64_t checked_time() {
    timespec now;
    if(clock_gettime(CLOCK_MONOTONIC,&now) || now.tv_sec<0)return 0;
    return uint64_t(now.tv_sec)*1000000000ULL+now.tv_nsec;
}
void put(unsigned char* p,uint64_t v,size_t n) {
    for(size_t i=0;i<n;++i) p[i]=static_cast<unsigned char>(v>>(8*i));
}
uint64_t get(const unsigned char* p,size_t n) {
    uint64_t v=0;for(size_t i=0;i<n;++i)v|=uint64_t(p[i])<<(8*i);return v;
}
bool address(const char* name,sockaddr_un* a,socklen_t* n) {
    if(!name || !*name || strlen(name)>95)return false;
    memset(a,0,sizeof(*a));a->sun_family=AF_UNIX;
    memcpy(a->sun_path+1,name,strlen(name));
    *n=static_cast<socklen_t>(offsetof(sockaddr_un,sun_path)+1+strlen(name));return true;
}
}
bool encode_motion(const RawEvent& e,unsigned char out[MOTION_RECORD_SIZE]) {
    if(!out || e.kind<WHEELS || e.kind>REVERSE || !e.epoch || !e.receive_seq ||
       !e.received_ns || e.reverse<0 || e.reverse>65535)return false;
    memset(out,0,MOTION_RECORD_SIZE);memcpy(out,"MDR1",4);
    put(out+4,1,2);put(out+6,e.kind,2);put(out+8,e.epoch,8);
    put(out+16,e.receive_seq,8);put(out+24,e.received_ns,8);
    uint64_t source;memcpy(&source,&e.source_mono_ms,8);put(out+32,source,8);
    for(size_t i=0;i<4;++i)put(out+40+2*i,e.raw[i],2);
    put(out+48,e.count,2);put(out+50,static_cast<unsigned>(e.reverse),2);
    return true;
}
bool decode_motion(const unsigned char* p,size_t n,RawEvent* out) {
    if(!out)return false;
    *out=RawEvent();
    if(!p || n!=MOTION_RECORD_SIZE || memcmp(p,"MDR1",4) || get(p+4,2)!=1)return false;
    for(size_t i=52;i<64;++i)if(p[i])return false;
    const uint64_t kind=get(p+6,2);
    if(kind<WHEELS || kind>REVERSE)return false;
    RawEvent e=RawEvent();e.kind=static_cast<SensorKind>(kind);
    e.epoch=get(p+8,8);e.receive_seq=get(p+16,8);e.received_ns=get(p+24,8);
    uint64_t source=get(p+32,8);memcpy(&e.source_mono_ms,&source,8);
    for(size_t i=0;i<4;++i)e.raw[i]=static_cast<uint16_t>(get(p+40+2*i,2));
    e.count=static_cast<uint16_t>(get(p+48,2));e.reverse=static_cast<int>(get(p+50,2));
    if(!e.epoch || !e.receive_seq || !e.received_ns)return false;
    *out=e;return true;
}
bool MotionCursor::accept(pid_t pid,uint64_t epoch,uint64_t sequence) {
    return check(pid,epoch,sequence)==RECEIVE_OK;
}
ReceiveFault MotionCursor::check(pid_t pid,uint64_t epoch,uint64_t sequence,uint64_t received_ns) {
    if(pid<=0 || !epoch || !sequence)return RECEIVE_SEQUENCE;
    if(!pid_) { pid_=pid;epoch_=epoch;sequence_=sequence;received_ns_=received_ns;return RECEIVE_OK; }
    if(pid_!=pid || epoch_!=epoch) {
        pid_=pid;epoch_=epoch;sequence_=sequence;received_ns_=received_ns;return RECEIVE_SOURCE_CHANGED;
    }
    // A replay must never rewind the same source's high-water mark.
    if(sequence_==UINT64_MAX || sequence<=sequence_)return RECEIVE_SEQUENCE;
    const bool contiguous=sequence==sequence_+1;
    sequence_=sequence;
    if(received_ns>received_ns_)received_ns_=received_ns;
    return contiguous?RECEIVE_OK:RECEIVE_SEQUENCE;
}
bool MotionCursor::late_admissible(pid_t pid,uint64_t epoch,uint64_t sequence,uint64_t received_ns) const {
    return pid_>0 && pid==pid_ && epoch && epoch==epoch_ && sequence_!=UINT64_MAX &&
           sequence==sequence_+1 && received_ns>=received_ns_;
}
void MotionCursor::advance_stale(pid_t pid,uint64_t epoch,uint64_t sequence,uint64_t received_ns) {
    if(pid_<=0 || pid!=pid_ || !epoch || epoch!=epoch_ || sequence<=sequence_)return;
    sequence_=sequence;
    if(received_ns>received_ns_)received_ns_=received_ns;
}
const char* receive_fault_name(ReceiveFault reason) {
    static const char* const names[]={"ok","syscall","truncated","credentials_missing",
        "credentials_mismatch","decode","clock_unavailable","future","stale",
        "source_changed","sequence_discontinuity"};
    return unsigned(reason)<sizeof names/sizeof names[0]?names[reason]:"unknown";
}
ReceiveResult inspect_motion_datagram(const MotionDatagram& packet,uid_t expected_uid,
    uint64_t now,MotionCursor& cursor,RawEvent* out,ReceiveDiagnostic* diagnostic) {
    ReceiveDiagnostic d=ReceiveDiagnostic();d.checked_ns=now;
    d.credentials_present=packet.credentials_present;
    d.sender_pid=packet.sender_pid;d.sender_uid=packet.sender_uid;
    RawEvent e=RawEvent();
    if(out)*out=RawEvent();
    if(packet.truncated)d.reason=RECEIVE_TRUNCATED;
    else if(!packet.credentials_present)d.reason=RECEIVE_CREDENTIALS_MISSING;
    else if(packet.sender_uid!=expected_uid || packet.sender_pid<=0)d.reason=RECEIVE_CREDENTIALS_MISMATCH;
    else if(!out || !decode_motion(packet.bytes,packet.size,&e))d.reason=RECEIVE_DECODE;
    else {
        d.authenticated_decoded=true;
        if(!now)d.reason=RECEIVE_CLOCK_UNAVAILABLE;
        else if(now<e.received_ns)d.reason=RECEIVE_FUTURE;
        else {
            d.age_ns=now-e.received_ns;
            if(d.age_ns<=MOTION_FRESH_NS)
                d.reason=cursor.check(packet.sender_pid,e.epoch,e.receive_seq,e.received_ns);
            // Late arrival (validation/WORKER_STALL_STALE_2026-10-06.md): the
            // record waited in the socket queue while this consumer was not
            // receiving. The data are intact, so up to the reverse-latch gap
            // limit (2 s) the next contiguous record of the same producer pid
            // and epoch with a monotonic receipt time is accepted. received_ns
            // is not rewritten: the pipeline keeps using the producer time,
            // and its own LATE/sensor-age checks and the BETA lease still
            // bound output freshness. The age is diagnostic only.
            else if(d.age_ns<=mx5::runtime::MotionGapTracker::KEEP_NS &&
                    cursor.late_admissible(packet.sender_pid,e.epoch,e.receive_seq,e.received_ns)) {
                d.reason=cursor.check(packet.sender_pid,e.epoch,e.receive_seq,e.received_ns);
                d.late=d.reason==RECEIVE_OK;
            } else {
                d.reason=RECEIVE_STALE;
                cursor.advance_stale(packet.sender_pid,e.epoch,e.receive_seq,e.received_ns);
            }
        }
        if(d.reason==RECEIVE_OK)*out=e;
        else d.rejected=e;
    }
    if(diagnostic)*diagnostic=d;
    return d.reason==RECEIVE_OK?CHANNEL_EVENT:CHANNEL_FAULT;
}
MotionReceiver::MotionReceiver():fd_(-1) {}
MotionReceiver::~MotionReceiver(){if(fd_>=0)close(fd_);}
bool MotionReceiver::open_channel(const char* name) {
    if(fd_>=0)return false;
    sockaddr_un a;socklen_t n;if(!address(name,&a,&n))return false;
    int fd=socket(AF_UNIX,SOCK_DGRAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
    if(fd<0)return false;
    int one=1;
    if(setsockopt(fd,SOL_SOCKET,SO_PASSCRED,&one,sizeof(one)) ||
       bind(fd,reinterpret_cast<sockaddr*>(&a),n)){close(fd);return false;}
    fd_=fd;return true;
}
int MotionReceiver::wait_for_input(unsigned timeout_ms) const {
    if(fd_<0 || timeout_ms>INT_MAX) {
        errno=fd_<0?EBADF:EINVAL;return -1;
    }
    pollfd waiting={fd_,POLLIN,0};
    const int result=poll(&waiting,1,static_cast<int>(timeout_ms));
    if(result<0)return errno==EINTR?0:-1;
    if(!result)return 0;
    if(waiting.revents&POLLIN)return 1;
    errno=(waiting.revents&POLLNVAL)?EBADF:EIO;return -1;
}
ReceiveResult MotionReceiver::receive(RawEvent* out,ReceiveDiagnostic* diagnostic) {
    if(diagnostic)*diagnostic=ReceiveDiagnostic();
    if(out)*out=RawEvent();
    if(!out || fd_<0) {
        if(diagnostic) { diagnostic->reason=RECEIVE_SYSCALL;diagnostic->syscall_errno=EBADF;
                        diagnostic->checked_ns=checked_time(); }
        return CHANNEL_FAULT;
    }
    unsigned char bytes[MOTION_RECORD_SIZE];
    union { cmsghdr alignment; unsigned char bytes[CMSG_SPACE(sizeof(ucred))]; } ancillary;
    memset(&ancillary,0,sizeof(ancillary));
    iovec iov={bytes,sizeof(bytes)};msghdr msg;memset(&msg,0,sizeof(msg));
    msg.msg_iov=&iov;msg.msg_iovlen=1;msg.msg_control=ancillary.bytes;
    msg.msg_controllen=sizeof(ancillary.bytes);
    ssize_t n=recvmsg(fd_,&msg,MSG_DONTWAIT);
    const int receive_errno=errno;
    if(n<0 && (receive_errno==EAGAIN || receive_errno==EWOULDBLOCK || receive_errno==EINTR))return CHANNEL_EMPTY;
    const uint64_t now=checked_time();
    if(n<0) {
        if(diagnostic) { diagnostic->reason=RECEIVE_SYSCALL;diagnostic->syscall_errno=receive_errno;
                        diagnostic->checked_ns=now; }
        return CHANNEL_FAULT;
    }
    bool credentials=false;ucred credential;memset(&credential,0,sizeof(credential));
    for(cmsghdr* c=CMSG_FIRSTHDR(&msg);n>=0 && c;c=CMSG_NXTHDR(&msg,c)) {
        if(c->cmsg_level==SOL_SOCKET && c->cmsg_type==SCM_CREDENTIALS &&
           c->cmsg_len==CMSG_LEN(sizeof(ucred))) {
            memcpy(&credential,CMSG_DATA(c),sizeof(credential));credentials=true;
        }
    }
    const MotionDatagram packet={bytes,static_cast<size_t>(n),
        (msg.msg_flags&(MSG_TRUNC|MSG_CTRUNC))!=0,credentials,credential.pid,credential.uid};
    return inspect_motion_datagram(packet,getuid(),now,cursor_,out,diagnostic);
}
MotionSender::MotionSender():fd_(-1){name_[0]=0;}
MotionSender::~MotionSender(){if(fd_>=0)close(fd_);}
bool MotionSender::open_channel(const char* name) {
    if(fd_>=0)return false;
    sockaddr_un a;socklen_t n;if(!address(name,&a,&n))return false;
    fd_=socket(AF_UNIX,SOCK_DGRAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
    if(fd_<0)return false;
    strcpy(name_,name);return true;
}
bool MotionSender::send_event(const RawEvent& e) {
    unsigned char bytes[MOTION_RECORD_SIZE];sockaddr_un a;socklen_t n;
    if(fd_<0 || !encode_motion(e,bytes) || !address(name_,&a,&n))return false;
    return sendto(fd_,bytes,sizeof(bytes),MSG_DONTWAIT|MSG_NOSIGNAL,
                  reinterpret_cast<sockaddr*>(&a),n)==static_cast<ssize_t>(sizeof(bytes));
}
} }
