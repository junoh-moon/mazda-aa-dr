#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "channel.h"
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace mx5 { namespace navigation {
namespace {
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
    if(pid<=0 || !epoch || !sequence)return false;
    if(!pid_) { pid_=pid;epoch_=epoch;sequence_=sequence;return true; }
    if(pid_!=pid || epoch_!=epoch) {
        pid_=pid;epoch_=epoch;sequence_=sequence;return false;
    }
    // A replay must never rewind the same source's high-water mark.
    if(sequence_==UINT64_MAX || sequence<=sequence_)return false;
    const bool contiguous=sequence==sequence_+1;
    sequence_=sequence;return contiguous;
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
ReceiveResult MotionReceiver::receive(uint64_t now,RawEvent* out) {
    if(!out || fd_<0)return CHANNEL_FAULT;
    *out=RawEvent();unsigned char bytes[MOTION_RECORD_SIZE];
    union { cmsghdr alignment; unsigned char bytes[CMSG_SPACE(sizeof(ucred))]; } ancillary;
    memset(&ancillary,0,sizeof(ancillary));
    iovec iov={bytes,sizeof(bytes)};msghdr msg;memset(&msg,0,sizeof(msg));
    msg.msg_iov=&iov;msg.msg_iovlen=1;msg.msg_control=ancillary.bytes;
    msg.msg_controllen=sizeof(ancillary.bytes);
    ssize_t n=recvmsg(fd_,&msg,MSG_DONTWAIT);
    if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR))return CHANNEL_EMPTY;
    bool credentials=false;ucred credential;memset(&credential,0,sizeof(credential));
    for(cmsghdr* c=CMSG_FIRSTHDR(&msg);n>=0 && c;c=CMSG_NXTHDR(&msg,c)) {
        if(c->cmsg_level==SOL_SOCKET && c->cmsg_type==SCM_CREDENTIALS &&
           c->cmsg_len==CMSG_LEN(sizeof(ucred))) {
            memcpy(&credential,CMSG_DATA(c),sizeof(credential));credentials=true;
        }
    }
    RawEvent e=RawEvent();
    if(n<0 || (msg.msg_flags&(MSG_TRUNC|MSG_CTRUNC)) || !credentials ||
       credential.uid!=getuid() || credential.pid<=0 ||
       !decode_motion(bytes,static_cast<size_t>(n),&e) || !now ||
       now<e.received_ns || now-e.received_ns>250000000ULL)return CHANNEL_FAULT;
    // Discard the discontinuity record as well. Runtime resets its navigation
    // state; the next record can resume acquisition but cannot revive an anchor.
    if(!cursor_.accept(credential.pid,e.epoch,e.receive_seq))return CHANNEL_FAULT;
    *out=e;return CHANNEL_EVENT;
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
