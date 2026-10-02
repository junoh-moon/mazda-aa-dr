#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "lds_association_channel.h"
#include "lds_association_protocol.h"
#include "adapter/lds_hooks.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <new>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <time.h>

namespace mx5 { namespace runtime { namespace lds_association {
namespace {
namespace P=protocol;
namespace Q=request_trace;
struct Errno { int value;Errno():value(errno){} ~Errno(){errno=value;} };
struct Release { std::atomic<uint32_t>& flag;explicit Release(std::atomic<uint32_t>& f):flag(f){} ~Release(){flag.store(0,std::memory_order_release);} };
uint32_t load(const P::Map* m,unsigned i) { return m->words[i].load(std::memory_order_relaxed); }
void store(P::Map* m,unsigned i,uint32_t n) { m->words[i].store(n,std::memory_order_relaxed); }
uint64_t pair(const uint32_t* w,unsigned i) { return uint64_t(w[i])|(uint64_t(w[i+1])<<32); }
void put(uint32_t* w,unsigned i,uint64_t n) { w[i]=uint32_t(n);w[i+1]=uint32_t(n>>32); }
void store64(P::Map* m,unsigned i,uint64_t n) { store(m,i,uint32_t(n));store(m,i+1,uint32_t(n>>32)); }
uint64_t load64(const P::Map* m,unsigned i) { return uint64_t(load(m,i))|(uint64_t(load(m,i+1))<<32); }
bool complete(const Q::Text& t) { return t.known&&t.complete&&t.bytes[0]&&strnlen(t.bytes,sizeof t.bytes)<sizeof t.bytes; }
bool equal(const Q::Text& a,const Q::Text& b) { return complete(a)&&complete(b)&&!strncmp(a.bytes,b.bytes,sizeof a.bytes); }
bool text_is(const Q::Text& t,const char* s) { return complete(t)&&!strncmp(t.bytes,s,sizeof t.bytes); }
void text_words(uint32_t* w,const Q::Text& t) {
    const size_t size=strnlen(t.bytes,sizeof t.bytes);
    for(unsigned i=0;i<16;++i) {
        uint32_t value=0;
        for(unsigned j=0;j<4;++j)if(4*i+j<size)value|=uint32_t(static_cast<unsigned char>(t.bytes[4*i+j]))<<(8*j);
        w[i]=value;
    }
}
void number(uint32_t* w,unsigned i,double value) { uint64_t bits;memcpy(&bits,&value,sizeof bits);put(w,i,bits); }
void position_words(uint32_t* w,const adapter::PositionInput& p) {
    w[0]=uint32_t(p.mode);put(w,1,p.utc_seconds);number(w,3,p.latitude_deg);number(w,5,p.longitude_deg);
    w[7]=uint32_t(p.altitude_m);number(w,8,p.heading_deg);number(w,10,p.velocity_kmh);
    number(w,12,p.horizontal);number(w,14,p.vertical);
}
bool valid(const adapter::LdsLockedSend& r) {
    if(r.stage!=LOCKED_FOR_SEND||r.reply_type!=2||!r.observed_ns||
       !complete(r.wire.server_guid)||!complete(r.wire.client_unique)||!complete(r.wire.server_unique)||
       !equal(r.wire.client_unique,r.wire.destination)||!r.wire.request_serial||!r.wire.response_serial||
       r.wire.request_serial!=r.wire.reply_serial)return false;
    if(!r.field_lineage.lifetime&&r.field_lineage.write_sequence)return false;
    for(unsigned i=0;i<9;++i)if(r.field_lineage.fields[i].write_sequence>r.field_lineage.write_sequence||
        (!r.field_lineage.fields[i].write_sequence&&r.field_lineage.fields[i].observed_ns))return false;
    return true;
}
void encode(const adapter::LdsLockedSend& r,uint32_t* w) {
    memset(w,0,P::RECORD_WORDS*4);w[P::STATE]=P::LOCKED;put(w,P::OBSERVED,r.observed_ns);
    w[P::REPLY_TYPE]=uint32_t(r.reply_type);w[P::REQUEST_SERIAL]=r.wire.request_serial;
    w[P::RESPONSE_SERIAL]=r.wire.response_serial;w[P::REPLY_SERIAL]=r.wire.reply_serial;
    text_words(w+P::GUID,r.wire.server_guid);text_words(w+P::CLIENT,r.wire.client_unique);
    text_words(w+P::SERVER,r.wire.server_unique);text_words(w+P::DESTINATION,r.wire.destination);
    put(w,P::LIFETIME,r.field_lineage.lifetime);put(w,P::WRITE,r.field_lineage.write_sequence);
    for(unsigned i=0;i<9;++i) { put(w,P::ORIGINS+4*i,r.field_lineage.fields[i].write_sequence);put(w,P::ORIGINS+4*i+2,r.field_lineage.fields[i].observed_ns); }
    position_words(w+P::POSITION,r.position);
}
bool context(const adapter::PositionContext& c,uint32_t* key) {
    const Q::Trace& t=c.request_trace;
    if(c.request_result!=Q::OK||!c.call_sequence||!c.prediction_generation||!t.request.id||!t.worker.id||
       !t.request.epoch||t.worker.epoch!=t.request.epoch||!t.issue.observed_ns||!t.reply.wire.observed_ns||
       !t.issue.wire.known||t.issue.wire.conflict||!t.issue.wire.endpoint_matched||!t.reply.wire.known||
       t.reply.wire.type!=2||!t.issue.wire.serial||!t.reply.wire.serial||
       t.issue.wire.serial!=t.reply.wire.reply_serial||
       !complete(t.issue.endpoint.server_guid)||!complete(t.issue.endpoint.unique_name)||!complete(t.reply.wire.sender)||
       !text_is(t.issue.route.destination,"com.jci.lds.data")||!text_is(t.issue.route.path,"/com/jci/lds/data")||
       !text_is(t.issue.route.interface_name,"com.jci.lds.data")||!text_is(t.issue.route.member,"GetPosition"))return false;
    memset(key,0,P::RECORD_WORDS*4);key[P::REQUEST_SERIAL]=t.issue.wire.serial;
    key[P::RESPONSE_SERIAL]=t.reply.wire.serial;key[P::REPLY_SERIAL]=t.reply.wire.reply_serial;
    text_words(key+P::GUID,t.issue.endpoint.server_guid);text_words(key+P::CLIENT,t.issue.endpoint.unique_name);
    text_words(key+P::SERVER,t.reply.wire.sender);text_words(key+P::DESTINATION,t.issue.endpoint.unique_name);
    position_words(key+P::POSITION,c.position);return true;
}
bool same_key(const uint32_t* a,const uint32_t* b) {
    return !memcmp(a+P::REQUEST_SERIAL,b+P::REQUEST_SERIAL,(P::LIFETIME-P::REQUEST_SERIAL)*4);
}
bool address(const char* name,sockaddr_un* out,socklen_t* len) {
    if(!name)return false;
    const size_t n=strnlen(name,96);if(!n||n==96)return false;
    memset(out,0,sizeof *out);out->sun_family=AF_UNIX;memcpy(out->sun_path+1,name,n);
    *len=socklen_t(offsetof(sockaddr_un,sun_path)+1+n);return true;
}
bool increment(std::atomic<uint32_t>& value,uint32_t* result) {
    uint32_t before=value.load(std::memory_order_relaxed);
    for(unsigned n=0;n<4;++n) {
        if(before==UINT32_MAX)return false;
        if(value.compare_exchange_weak(before,before+1,std::memory_order_acq_rel,
                std::memory_order_acquire)) { if(result)*result=before+1;return true; }
    }
    return false;
}
bool begin_write(P::Map* m,uint32_t* sequence) {
    const uint32_t old=m->words[P::SEQUENCE].load(std::memory_order_relaxed);
    if((old&1)||old>UINT32_MAX-2) { m->words[P::ENABLED].store(0,std::memory_order_release);return false; }
    *sequence=old+2;m->words[P::SEQUENCE].store(old+1,std::memory_order_relaxed);
    // A reader seeing any following relaxed payload store synchronizes this
    // fence with its post-copy acquire fence. Its final version load cannot
    // then see the even version preceding this odd store (fence-fence HB).
    std::atomic_thread_fence(std::memory_order_release);return true;
}
void end_write(P::Map* m,uint32_t sequence) { m->words[P::SEQUENCE].store(sequence,std::memory_order_release); }
void offer_bytes(unsigned char* b,uint64_t instance,pid_t pid,uint64_t created) {
    const uint32_t w[8]={P::OFFER_MAGIC,P::VERSION,P::MAP_BYTES,uint32_t(instance),uint32_t(instance>>32),uint32_t(pid),uint32_t(created),uint32_t(created>>32)};
    for(unsigned i=0;i<8;++i)for(unsigned j=0;j<4;++j)b[4*i+j]=static_cast<unsigned char>(w[i]>>(8*j));
}
uint32_t get32(const unsigned char* b) { return uint32_t(b[0])|(uint32_t(b[1])<<8)|(uint32_t(b[2])<<16)|(uint32_t(b[3])<<24); }
bool header(const P::Map* m,uint64_t instance,pid_t pid,uint64_t created) {
    for(unsigned n=0;n<P::READ_ATTEMPTS;++n) {
        const uint32_t a=m->words[P::SEQUENCE].load(std::memory_order_acquire);if(a&1)continue;
        const bool ok=load(m,P::MAGIC)==P::MAP_MAGIC&&load(m,P::LAYOUT)==P::VERSION&&load(m,P::SIZE)==P::MAP_BYTES&&
            load(m,P::SLOTS)==P::CAPACITY&&load64(m,P::INSTANCE)==instance&&load(m,P::PID)==uint32_t(pid)&&
            load(m,P::COUNT)<=P::CAPACITY&&load(m,P::LOSS)!=0&&load64(m,P::CREATED)==created;
        std::atomic_thread_fence(std::memory_order_acquire);
        if(a==m->words[P::SEQUENCE].load(std::memory_order_acquire))return ok;
    }
    return false;
}
}

Publisher::Publisher():mapping_(0),readonly_fd_(-1),socket_(-1),instance_(0),created_ns_(0),last_offer_ns_(0),floor_ns_(0),latest_ns_(0),sequence_(0),count_(0),seen_loss_(1),busy_(0),offer_busy_(0),disabled_(0) {}
Publisher::~Publisher() {
    Errno saved;
    if(mapping_) { if(!disabled_.load())static_cast<P::Map*>(mapping_)->words[P::ENABLED].store(0,std::memory_order_release);munmap(mapping_,P::MAP_BYTES); }
    if(readonly_fd_>=0)close(readonly_fd_);
    if(socket_>=0)close(socket_);
}
bool Publisher::prepare(uint64_t instance,const char* directory) {
    Errno saved;if(mapping_||disabled_.load()||!instance||!directory)return false;
    char name[1024];const int n=snprintf(name,sizeof name,"%s/mx5-lds-association-XXXXXX",directory);
    if(n<=0||size_t(n)>=sizeof name)return false;
    int writer=mkstemp(name);if(writer<0)return false;
    int reader=-1,sock=-1;void* map=MAP_FAILED;bool ok=false;
    do {
        if(fcntl(writer,F_SETFD,FD_CLOEXEC)||fchmod(writer,0600)||ftruncate(writer,P::MAP_BYTES))break;
        // Allocate backing pages with error-returning I/O off-hook, before any
        // mapped access could turn sparse-file ENOSPC into process SIGBUS.
        const unsigned char zero[4096]={0};bool allocated=true;
        for(unsigned offset=0;offset<P::MAP_BYTES;offset+=sizeof zero) {
            if(pwrite(writer,zero,sizeof zero,offset)!=ssize_t(sizeof zero)) { allocated=false;break; }
        }
        if(!allocated)break;
        timespec now=timespec();
        if(clock_gettime(CLOCK_MONOTONIC,&now)||now.tv_sec<0||now.tv_nsec<0||now.tv_nsec>=1000000000L)break;
        const uint64_t created=uint64_t(now.tv_sec)*1000000000ULL+uint64_t(now.tv_nsec);if(!created)break;
        reader=open(name,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(reader<0)break;
        struct stat a,b;if(fstat(writer,&a)||fstat(reader,&b)||a.st_dev!=b.st_dev||a.st_ino!=b.st_ino||
            !S_ISREG(a.st_mode)||a.st_size!=P::MAP_BYTES||a.st_uid!=geteuid())break;
        map=mmap(0,P::MAP_BYTES,PROT_READ|PROT_WRITE,MAP_SHARED,writer,0);if(map==MAP_FAILED)break;
        P::Map* words=static_cast<P::Map*>(map);
        // Touch and construct every page off-hook. No pathname or writable FD is
        // exported, and the backing object is never truncated/reused afterward.
        for(unsigned i=0;i<P::WORDS;++i)new(&words->words[i])std::atomic<uint32_t>(0);
        store(words,P::MAGIC,P::MAP_MAGIC);store(words,P::LAYOUT,P::VERSION);store(words,P::SIZE,P::MAP_BYTES);
        store(words,P::SLOTS,P::CAPACITY);store(words,P::LOSS,1);store64(words,P::INSTANCE,instance);
        store64(words,P::CREATED,created);
        store(words,P::PID,uint32_t(getpid()));words->words[P::ENABLED].store(1,std::memory_order_release);
        sock=socket(AF_UNIX,SOCK_DGRAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);if(sock<0)break;
        if(unlink(name))break;
        mapping_=map;readonly_fd_=reader;socket_=sock;instance_=instance;created_ns_=created;ok=true;
    } while(false);
    close(writer);
    if(!ok) { unlink(name);if(reader>=0)close(reader);if(sock>=0)close(sock);if(map!=MAP_FAILED)munmap(map,P::MAP_BYTES); }
    return ok;
}
void Publisher::invalidate() {
    Errno saved;if(disabled_.load(std::memory_order_relaxed)||!mapping_)return;
    P::Map* m=static_cast<P::Map*>(mapping_);
    if(!increment(m->words[P::LOSS],0))m->words[P::ENABLED].store(0,std::memory_order_release);
}
void Publisher::disable_after_fork() { disabled_.store(1,std::memory_order_release); }
bool Publisher::publish(const adapter::LdsLockedSend& r) {
    Errno saved;if(disabled_.load(std::memory_order_acquire)||!mapping_)return false;
    P::Map* m=static_cast<P::Map*>(mapping_);if(!m->words[P::ENABLED].load(std::memory_order_acquire))return false;
    if(!valid(r)) { invalidate();return false; }
    uint32_t idle=0;
    if(!busy_.compare_exchange_strong(idle,1,std::memory_order_acquire,
            std::memory_order_relaxed)) { invalidate();return false; }
    Release release(busy_);
    const uint32_t loss=m->words[P::LOSS].load(std::memory_order_acquire);
    if(loss!=seen_loss_||count_==P::CAPACITY) {
        if(r.observed_ns>latest_ns_)latest_ns_=r.observed_ns;
        if(latest_ns_>floor_ns_)floor_ns_=latest_ns_;
        uint32_t version;if(!begin_write(m,&version))return false;
        count_=0;seen_loss_=loss;store64(m,P::FLOOR,floor_ns_);store(m,P::COUNT,0);end_write(m,version);
        // The triggering observation establishes the retirement floor. Only a
        // later request/locked reply may enter the next bounded window.
        return false;
    }
    if(r.observed_ns<=floor_ns_)return false;
    if(sequence_==UINT32_MAX) { m->words[P::ENABLED].store(0,std::memory_order_release);return false; }
    uint32_t w[P::RECORD_WORDS];encode(r,w);w[P::RECORD_SEQUENCE]=++sequence_;w[P::RECORD_LOSS]=loss;
    unsigned target=count_;uint32_t old[P::RECORD_WORDS];
    for(unsigned i=0;i<count_;++i) {
        for(unsigned j=0;j<P::RECORD_WORDS;++j)old[j]=load(m,P::HEADER+i*P::RECORD_WORDS+j);
        if(same_key(old,w)) { target=i;break; }
    }
    uint32_t version;if(!begin_write(m,&version))return false;
    if(target<count_)store(m,P::HEADER+target*P::RECORD_WORDS+P::STATE,P::CONTRADICTORY);
    else {
        for(unsigned i=0;i<P::RECORD_WORDS;++i)store(m,P::HEADER+target*P::RECORD_WORDS+i,w[i]);
        ++count_;store(m,P::COUNT,count_);
    }
    if(r.observed_ns>latest_ns_)latest_ns_=r.observed_ns;
    store(m,P::LAST_RECORD,sequence_);end_write(m,version);
    return m->words[P::LOSS].load(std::memory_order_acquire)==loss;
}
bool Publisher::offer(const char* channel,uint64_t now) {
    Errno saved;if(!mapping_||socket_<0||disabled_.load(std::memory_order_acquire)||!now)return false;
    uint32_t idle=0;if(!offer_busy_.compare_exchange_strong(idle,1,std::memory_order_acquire,
            std::memory_order_relaxed))return false;
    Release release(offer_busy_);
    if(last_offer_ns_&&(now<last_offer_ns_||now-last_offer_ns_<1000000000ULL))return false;
    last_offer_ns_=now;sockaddr_un addr;socklen_t len;if(!address(channel,&addr,&len))return false;
    unsigned char bytes[P::OFFER_BYTES];offer_bytes(bytes,instance_,pid_t(load(static_cast<P::Map*>(mapping_),P::PID)),created_ns_);
    union { cmsghdr align;unsigned char bytes[CMSG_SPACE(sizeof(int))]; } control;
    memset(&control,0,sizeof control);iovec io={bytes,sizeof bytes};msghdr msg=msghdr();
    msg.msg_name=&addr;msg.msg_namelen=len;msg.msg_iov=&io;msg.msg_iovlen=1;msg.msg_control=control.bytes;msg.msg_controllen=sizeof control.bytes;
    cmsghdr* c=CMSG_FIRSTHDR(&msg);c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;c->cmsg_len=CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c),&readonly_fd_,sizeof readonly_fd_);
    return sendmsg(socket_,&msg,MSG_DONTWAIT|MSG_NOSIGNAL)==P::OFFER_BYTES;
}

Registry::View::View():gate(P::CLOSED),mapping(0),device(0),inode(0),instance(0),floor_ns(0),revision(0),admission_epoch(0),pid(0),uid(0) {}
Registry::Registry():views_(),active_(0),disabled_(0),admission_epoch_(1),socket_(-1),expected_uid_(0),revision_(0),drained_epoch_(1),floor_ns_(0),last_drain_ns_(0),last_created_ns_(0),last_device_(0),last_inode_(0),last_instance_(0),last_pid_(0) {}
Registry::~Registry() { close_channel(); }
void Registry::reclaim() {
    for(unsigned i=0;i<2;++i)if(views_[i].mapping&&views_[i].gate.load(std::memory_order_acquire)==P::CLOSED) {
        munmap(views_[i].mapping,P::MAP_BYTES);views_[i].mapping=0;
    }
}
void Registry::retire() {
    // One atomic linearization point; a concurrent worker must never reopen a
    // gate which a delayed store in this callback subsequently closes. Only
    // the worker changes gates/active slots, after observing the new epoch.
    // Readers check this epoch before and after copying, even before drain.
    if(!increment(admission_epoch_,0))disabled_.store(1,std::memory_order_release);
}
void Registry::close_views() {
    active_.store(0,std::memory_order_release);
    for(unsigned i=0;i<2;++i)views_[i].gate.fetch_or(P::CLOSED,std::memory_order_acq_rel);
}
void Registry::close_channel() {
    Errno saved;retire();close_views();if(socket_>=0)close(socket_);socket_=-1;reclaim();
}
void Registry::disable_after_fork() {
    disabled_.store(1,std::memory_order_release);active_.store(0,std::memory_order_release);
    // An inherited borrower count may belong to a vanished parent thread.
    // Never repair it or write the shared map in the child handler.
}
bool Registry::open_channel(const char* channel,uid_t uid) {
    Errno saved;if(socket_>=0||disabled_.load(std::memory_order_acquire))return false;
    sockaddr_un addr;socklen_t len;if(!address(channel,&addr,&len))return false;
    const int fd=socket(AF_UNIX,SOCK_DGRAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);if(fd<0)return false;
    int yes=1;
    if(setsockopt(fd,SOL_SOCKET,SO_PASSCRED,&yes,sizeof yes)||bind(fd,reinterpret_cast<sockaddr*>(&addr),len)) { close(fd);return false; }
    socket_=fd;expected_uid_=uid;return true;
}
void Registry::drain(uint64_t now) {
    Errno saved;reclaim();if(socket_<0||disabled_.load(std::memory_order_acquire)||!now)return;
    if(now<last_drain_ns_) { retire();return; }
    last_drain_ns_=now;
    const uint32_t current_epoch=admission_epoch_.load(std::memory_order_acquire);
    if(current_epoch!=drained_epoch_) {
        close_views();reclaim();drained_epoch_=current_epoch;if(now>floor_ns_)floor_ns_=now;
    }
    for(unsigned attempt=0;attempt<P::DRAIN_LIMIT;++attempt) {
        const uint32_t epoch=admission_epoch_.load(std::memory_order_acquire);
        if(epoch!=drained_epoch_)break; // retirement since the worker boundary
        // Linux 3.0 SCM_MAX_FD is 253. Receive the whole legal FD array so a
        // guest ABI conversion cannot strand descriptors outside its buffer.
        // Acceptance still requires exactly one descriptor; all others close.
        unsigned char bytes[P::OFFER_BYTES];union { cmsghdr align;unsigned char bytes[CMSG_SPACE(sizeof(ucred))+CMSG_SPACE(P::MAX_RIGHTS*sizeof(int))]; } control;
        memset(&control,0,sizeof control);iovec io={bytes,sizeof bytes};msghdr msg=msghdr();
        msg.msg_iov=&io;msg.msg_iovlen=1;msg.msg_control=control.bytes;msg.msg_controllen=sizeof control.bytes;
        const ssize_t n=recvmsg(socket_,&msg,MSG_DONTWAIT|MSG_CMSG_CLOEXEC);
        if(n<0)break;
        int fd=-1;unsigned descriptors=0,credentials=0;ucred cred=ucred();bool bad=(msg.msg_flags&(MSG_TRUNC|MSG_CTRUNC))!=0;
        for(cmsghdr* c=CMSG_FIRSTHDR(&msg);c;c=CMSG_NXTHDR(&msg,c)) {
            if(c->cmsg_level!=SOL_SOCKET) { bad=true;continue; }
            if(c->cmsg_type==SCM_RIGHTS) {
                const size_t count=(c->cmsg_len-CMSG_LEN(0))/sizeof(int);
                if(c->cmsg_len!=CMSG_LEN(count*sizeof(int)))bad=true;
                for(size_t i=0;i<count;++i) { int received;memcpy(&received,CMSG_DATA(c)+i*sizeof(int),sizeof received);
                    ++descriptors;if(fd<0)fd=received;else close(received); }
            } else if(c->cmsg_type==SCM_CREDENTIALS&&c->cmsg_len==CMSG_LEN(sizeof(ucred))) {
                memcpy(&cred,CMSG_DATA(c),sizeof cred);++credentials;
            } else bad=true;
        }
        const uint64_t instance=n==P::OFFER_BYTES?uint64_t(get32(bytes+12))|(uint64_t(get32(bytes+16))<<32):0;
        const uint64_t created=n==P::OFFER_BYTES?uint64_t(get32(bytes+24))|(uint64_t(get32(bytes+28))<<32):0;
        if(n!=P::OFFER_BYTES||get32(bytes)!=P::OFFER_MAGIC||get32(bytes+4)!=P::VERSION||get32(bytes+8)!=P::MAP_BYTES||
           !instance||!created||credentials!=1||cred.pid<=0||cred.uid!=expected_uid_||
           get32(bytes+20)!=uint32_t(cred.pid)||descriptors!=1)bad=true;
        struct stat st;
        if(fd<0)continue;
        const int flags=fcntl(fd,F_GETFL);
        if(bad||flags<0||(flags&O_ACCMODE)!=O_RDONLY||fstat(fd,&st)||!S_ISREG(st.st_mode)||
           st.st_nlink!=0||st.st_size!=P::MAP_BYTES||st.st_uid!=cred.uid||(st.st_mode&0777)!=0600) { close(fd);continue; }
        const uint32_t active=active_.load(std::memory_order_acquire);
        const unsigned current=(active&3)-1;
        if(active&&current<2&&views_[current].device==uint64_t(st.st_dev)&&views_[current].inode==uint64_t(st.st_ino)&&
           views_[current].instance==instance&&views_[current].pid==cred.pid) { close(fd);continue; }
        const bool same=last_device_==uint64_t(st.st_dev)&&last_inode_==uint64_t(st.st_ino)&&
            last_instance_==instance&&last_pid_==cred.pid&&last_created_ns_==created;
        // This is only an object-adoption order, never sensor freshness. A
        // queued older offer must not evict the newer active producer. Reoffers
        // of the exact last object remain usable after a local retirement.
        if(!same&&(created<=last_created_ns_||instance==last_instance_)) { close(fd);continue; }
        void* mapping=mmap(0,P::MAP_BYTES,PROT_READ,MAP_SHARED,fd,0);close(fd);
        if(mapping==MAP_FAILED)continue;
        if(!header(static_cast<P::Map*>(mapping),instance,cred.pid,created)) { munmap(mapping,P::MAP_BYTES);continue; }
        // Prefault each receiver page before exposing the read-only view.
        for(unsigned i=0;i<P::WORDS;i+=1024)(void)load(static_cast<P::Map*>(mapping),i);
        uint32_t selected_epoch=epoch;
        if(active) {
            // Our replacement is conditional on the epoch seen before recv.
            // A concurrent external retirement cannot be undone by adoption.
            if(epoch==UINT32_MAX||!admission_epoch_.compare_exchange_strong(selected_epoch,epoch+1,
                    std::memory_order_acq_rel,std::memory_order_acquire)) {
                munmap(mapping,P::MAP_BYTES);continue;
            }
            selected_epoch=epoch+1;drained_epoch_=selected_epoch;close_views();
            if(now>floor_ns_)floor_ns_=now;
        }
        // Invalid messages never revoke an unrelated active view. Replacement
        // does, and cannot unmap a callback's borrowed view while it copies.
        reclaim();unsigned slot=2;
        for(unsigned i=0;i<2;++i)if(!views_[i].mapping) { slot=i;break; }
        if(slot==2||revision_>=0x3fffffffU||disabled_.load(std::memory_order_acquire)) {
            munmap(mapping,P::MAP_BYTES);if(revision_>=0x3fffffffU)disabled_.store(1,std::memory_order_release);continue;
        }
        if(admission_epoch_.load(std::memory_order_acquire)!=selected_epoch) { munmap(mapping,P::MAP_BYTES);continue; }
        // A delayed re-adoption after retired borrowers drained is still a
        // replacement. Only the very first accepted object has no local floor.
        if(last_created_ns_&&now>floor_ns_)floor_ns_=now;
        View& v=views_[slot];v.mapping=mapping;v.device=uint64_t(st.st_dev);v.inode=uint64_t(st.st_ino);
        v.instance=instance;v.pid=cred.pid;v.uid=cred.uid;v.floor_ns=floor_ns_;v.revision=++revision_;v.admission_epoch=selected_epoch;
        v.gate.store(0,std::memory_order_release);
        last_created_ns_=created;last_device_=uint64_t(st.st_dev);last_inode_=uint64_t(st.st_ino);last_instance_=instance;last_pid_=cred.pid;
        active_.store((v.revision<<2)|(slot+1),std::memory_order_release);
        if(admission_epoch_.load(std::memory_order_acquire)!=selected_epoch||disabled_.load(std::memory_order_acquire)) {
            active_.store(0,std::memory_order_release);v.gate.fetch_or(P::CLOSED,std::memory_order_acq_rel);
        }
    }
}
bool Registry::read(const adapter::PositionContext& c,Owned* out) {
    Errno saved;if(out)*out=Owned();if(!out||disabled_.load(std::memory_order_acquire))return false;
    uint32_t wanted[P::RECORD_WORDS];if(!context(c,wanted))return false;
    const uint32_t token=active_.load(std::memory_order_acquire);const unsigned slot=(token&3)-1;
    if(!token||slot>=2)return false;
    View& v=views_[slot];uint32_t gate=v.gate.load(std::memory_order_acquire);
    if((gate&P::CLOSED)||gate==P::CLOSED-1||!v.gate.compare_exchange_strong(gate,gate+1,
            std::memory_order_acq_rel,std::memory_order_acquire))return false;
    Owned answer=Owned();bool match=false;
    if(active_.load(std::memory_order_acquire)==token&&admission_epoch_.load(std::memory_order_acquire)==v.admission_epoch&&
       !disabled_.load(std::memory_order_acquire)) {
        const P::Map* m=static_cast<const P::Map*>(v.mapping);
        for(unsigned attempt=0;attempt<P::READ_ATTEMPTS;++attempt) {
            const uint32_t version=m->words[P::SEQUENCE].load(std::memory_order_acquire);
            if(version&1)continue;
            const uint32_t loss=m->words[P::LOSS].load(std::memory_order_acquire);
            if(!m->words[P::ENABLED].load(std::memory_order_acquire)||!loss)break;
            const uint64_t floor=load64(m,P::FLOOR);const unsigned count=load(m,P::COUNT);
            if(count>P::CAPACITY||load64(m,P::INSTANCE)!=v.instance||load(m,P::PID)!=uint32_t(v.pid))break;
            uint32_t selected[P::RECORD_WORDS]={0};unsigned found=0;
            if(c.request_trace.issue.observed_ns>floor&&c.request_trace.issue.observed_ns>v.floor_ns) {
                uint32_t record[P::RECORD_WORDS];
                for(unsigned i=0;i<count;++i) {
                    for(unsigned j=0;j<P::RECORD_WORDS;++j)record[j]=load(m,P::HEADER+i*P::RECORD_WORDS+j);
                    if(record[P::STATE]&&same_key(record,wanted)) { ++found;memcpy(selected,record,sizeof selected); }
                }
            }
            std::atomic_thread_fence(std::memory_order_acquire);
            if(version!=m->words[P::SEQUENCE].load(std::memory_order_acquire)||loss!=m->words[P::LOSS].load(std::memory_order_acquire))continue;
            if(!m->words[P::ENABLED].load(std::memory_order_acquire))break;
            if(!found||selected[P::RECORD_LOSS]!=loss)break;
            if(found!=1||selected[P::STATE]==P::CONTRADICTORY) { answer.result=CONFLICT;break; }
            const uint64_t observed=pair(selected,P::OBSERVED);
            if(selected[P::STATE]!=P::LOCKED||selected[P::REPLY_TYPE]!=2||!selected[P::RECORD_SEQUENCE]||
               observed<c.request_trace.issue.observed_ns||observed>c.request_trace.reply.wire.observed_ns||observed<=floor)break;
            if(memcmp(selected+P::POSITION,wanted+P::POSITION,16*4)) { answer.result=PAYLOAD_MISMATCH;break; }
            answer.result=MATCHED_LOCKED_FOR_SEND;answer.stage=LOCKED_FOR_SEND;
            answer.call_sequence=c.call_sequence;answer.prediction_generation=c.prediction_generation;
            answer.view_revision=v.revision;answer.layout_version=P::VERSION;answer.source_instance=v.instance;
            answer.record_sequence=selected[P::RECORD_SEQUENCE];answer.locked_observed_ns=observed;answer.map_loss_epoch=loss;
            answer.request_id=c.request_trace.request.id;answer.request_epoch=c.request_trace.request.epoch;
            answer.worker_id=c.request_trace.worker.id;answer.worker_epoch=c.request_trace.worker.epoch;
            answer.cache_lifetime=pair(selected,P::LIFETIME);answer.write_sequence=pair(selected,P::WRITE);
            for(unsigned i=0;i<9;++i) { answer.fields[i].write_sequence=pair(selected,P::ORIGINS+4*i);answer.fields[i].observed_ns=pair(selected,P::ORIGINS+4*i+2); }
            match=true;break;
        }
    }
    // This is a linearized observation copy, not a future/current physical lease.
    if(active_.load(std::memory_order_acquire)!=token||admission_epoch_.load(std::memory_order_acquire)!=v.admission_epoch||disabled_.load(std::memory_order_acquire)) { answer=Owned();match=false; }
    v.gate.fetch_sub(1,std::memory_order_release);*out=answer;return match;
}
} } }
