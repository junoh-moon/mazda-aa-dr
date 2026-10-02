#include "runtime/lds_association_channel.h"
#include "adapter/lds_hooks.h"
#include "runtime/lds_association_protocol.h"
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <dirent.h>
#include <fcntl.h>
#include <thread>

static bool fail_pwrite;
static std::atomic<unsigned> pause_mapping(0),mapping_entered(0),mapping_release(0);
extern "C" void* __real_mmap(void*,size_t,int,int,int,off_t);
extern "C" void* __wrap_mmap(void* p,size_t n,int prot,int flags,int fd,off_t offset) {
    if(pause_mapping.load()&&prot==PROT_READ) {
        mapping_entered.store(1);
        while(!mapping_release.load())usleep(1000);
    }
    return __real_mmap(p,n,prot,flags,fd,offset);
}
extern "C" ssize_t __real_pwrite(int,const void*,size_t,off_t);
extern "C" ssize_t __wrap_pwrite(int fd,const void* p,size_t n,off_t o) {
    if(fail_pwrite) { errno=ENOSPC;return -1; }
    return __real_pwrite(fd,p,n,o);
}

namespace L=mx5::runtime::lds_association;
namespace A=mx5::adapter;
namespace Q=mx5::runtime::request_trace;
namespace P=mx5::runtime::lds_association::protocol;
namespace N=mx5::sensors::nmea_course_token;
namespace mx5 { namespace runtime { namespace lds_association {
struct AssociationTestAccess {
    static protocol::Map* map(Publisher& p) { return static_cast<protocol::Map*>(p.mapping_); }
    static int fd(Publisher& p) { return p.readonly_fd_; }
    static void busy(Publisher& p,unsigned n) { p.busy_.store(n); }
    static void sequence(Publisher& p,unsigned n) { p.sequence_=n; }
    static void revision(Registry& r,unsigned n) { r.revision_=n; }
    static unsigned hold(Registry& r) {
        unsigned i=(r.active_.load()&3)-1;
        if(i<2)r.views_[i].gate.fetch_add(1);
        return i;
    }
    static void release(Registry& r,unsigned i) { r.views_[i].gate.fetch_sub(1); }
    static unsigned mappings(Registry& r) { return unsigned(r.views_[0].mapping!=0)+unsigned(r.views_[1].mapping!=0); }
    static uint32_t gate_value(Registry& r,unsigned slot) { return r.views_[slot].gate.load(); }
    static void gate(Registry& r,uint32_t n) { unsigned i=(r.active_.load()&3)-1;if(i<2)r.views_[i].gate.store(n); }
    static void created(Publisher& p,uint64_t n) {
        p.created_ns_=n;map(p)->words[protocol::CREATED].store(uint32_t(n));map(p)->words[protocol::CREATED+1].store(uint32_t(n>>32));
    }
    static uint64_t created(Publisher& p) { return p.created_ns_; }
};
} } }
static unsigned checks;
#define CHECK(x) do { ++checks;if(!(x)){std::fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#x);std::abort();} } while(0)
static Q::Text text(const char* s) { Q::Text t=Q::Text();snprintf(t.bytes,sizeof t.bytes,"%s",s);t.known=t.complete=true;return t; }
static A::LdsLockedSend record(unsigned serial=7,uint64_t ns=1000) {
    A::LdsLockedSend r=A::LdsLockedSend();r.stage=L::LOCKED_FOR_SEND;r.reply_type=2;r.observed_ns=ns;
    r.wire.server_guid=text("transport-guid");r.wire.client_unique=text(":1.2");
    r.wire.server_unique=text(":1.1");r.wire.destination=r.wire.client_unique;
    r.wire.request_serial=r.wire.reply_serial=serial;r.wire.response_serial=serial+100;
    r.field_lineage.lifetime=2;r.field_lineage.write_sequence=3;
    for(unsigned i=0;i<9;++i) { r.field_lineage.fields[i].write_sequence=3;r.field_lineage.fields[i].observed_ns=ns-100; }
    r.position.mode=1;r.position.utc_seconds=123;r.position.latitude_deg=35;r.position.longitude_deg=135;
    r.position.altitude_m=12;r.position.heading_deg=40;r.position.velocity_kmh=37;r.position.horizontal=1;r.position.vertical=1.5;
    return r;
}
static Q::Trace trace(const A::LdsLockedSend& r) {
    Q::Trace t=Q::Trace();t.request=Q::Token{11,2};t.worker=Q::Token{12,2};
    t.issue.observed_ns=r.observed_ns-10;t.reply.observed_ns=r.observed_ns+10;
    t.issue.endpoint.server_guid=r.wire.server_guid;t.issue.endpoint.unique_name=r.wire.client_unique;
    t.issue.wire.known=t.issue.wire.endpoint_matched=true;t.issue.wire.serial=r.wire.request_serial;
    t.issue.wire.observed_ns=t.issue.observed_ns;
    t.issue.route.destination=text("com.jci.lds.data");t.issue.route.path=text("/com/jci/lds/data");
    t.issue.route.interface_name=text("com.jci.lds.data");t.issue.route.member=text("GetPosition");
    t.reply.wire.known=true;t.reply.wire.type=2;t.reply.wire.serial=r.wire.response_serial;
    t.reply.wire.reply_serial=r.wire.reply_serial;t.reply.wire.sender=r.wire.server_unique;
    t.reply.wire.observed_ns=t.reply.observed_ns;return t;
}
static bool lookup(L::Registry& g,const A::LdsLockedSend& r,L::Owned* out) {
    const Q::Trace t=trace(r);const A::PositionContext c={r.position,Q::OK,t,5,9,0};return g.read(c,out);
}
struct Fixture {
    char dir[128],channel[96];L::Publisher publisher;L::Registry registry;
    Fixture() { snprintf(dir,sizeof dir,"/tmp/mx5-assoc-XXXXXX");CHECK(mkdtemp(dir));
        snprintf(channel,sizeof channel,"mx5-assoc-test-%ld",long(getpid())); }
    ~Fixture() { registry.close_channel();CHECK(rmdir(dir)==0); }
    void setup() { CHECK(publisher.prepare(41,dir));CHECK(registry.open_channel(channel,geteuid())); }
    void adopt(uint64_t now=1010) { CHECK(publisher.offer(channel,now));registry.drain(now); }
};
static void basic() {
    Fixture f;L::Owned o;const auto r=record();CHECK(!lookup(f.registry,r,&o));CHECK(o.result==L::UNAVAILABLE);
    f.setup();CHECK(f.publisher.publish(r));CHECK(!lookup(f.registry,r,&o));f.adopt();
    errno=E2BIG;CHECK(lookup(f.registry,r,&o));CHECK(errno==E2BIG);
    CHECK(o.result==L::MATCHED_LOCKED_FOR_SEND&&o.stage==L::LOCKED_FOR_SEND);
    CHECK(o.call_sequence==5&&o.prediction_generation==9&&o.source_instance==41&&o.record_sequence==1);
    CHECK(o.request_id==11&&o.worker_id==12&&o.cache_lifetime==2&&o.write_sequence==3);
    for(unsigned i=0;i<9;++i)CHECK(o.fields[i].write_sequence==3&&o.fields[i].observed_ns==900);
    CHECK(!f.publisher.offer(f.channel,1011));
}
static void exact() {
    Fixture f;f.setup();auto r=record();CHECK(f.publisher.publish(r));f.adopt();L::Owned o;
    auto bad=r;bad.wire.server_guid=text("other");CHECK(!lookup(f.registry,bad,&o));CHECK(o.result==L::UNAVAILABLE);
    bad=r;bad.position.latitude_deg+=1;CHECK(!lookup(f.registry,bad,&o));CHECK(o.result==L::PAYLOAD_MISMATCH);
    CHECK(lookup(f.registry,r,&o));CHECK(f.publisher.publish(r));
    CHECK(!lookup(f.registry,r,&o));CHECK(o.result==L::CONFLICT);
}
static void capacity() {
    Fixture f;f.setup();auto r=record();CHECK(f.publisher.publish(r));f.adopt();L::Owned o;
    for(unsigned i=1;i<64;++i)CHECK(f.publisher.publish(record(7+i,1000+100*i)));
    CHECK(lookup(f.registry,r,&o));CHECK(!f.publisher.publish(record(80,8000)));
    CHECK(!lookup(f.registry,r,&o));CHECK(!f.publisher.publish(r));
    r=record(90,9000);CHECK(f.publisher.publish(r));CHECK(lookup(f.registry,r,&o));
}
static void invalidation() {
    Fixture f;f.setup();auto r=record();CHECK(f.publisher.publish(r));f.adopt();L::Owned o;
    CHECK(lookup(f.registry,r,&o));errno=EDOM;f.publisher.invalidate();CHECK(errno==EDOM);
    CHECK(!lookup(f.registry,r,&o));CHECK(!f.publisher.publish(record(8,2000)));
    r=record(9,3000);CHECK(f.publisher.publish(r));CHECK(lookup(f.registry,r,&o));
    f.registry.retire();CHECK(!lookup(f.registry,r,&o));
    CHECK(f.publisher.offer(f.channel,2000000000ULL));f.registry.drain(2000000000ULL);
    CHECK(!lookup(f.registry,r,&o));r=record(10,2000000100ULL);
    CHECK(f.publisher.publish(r));CHECK(lookup(f.registry,r,&o));
}
static void forked() {
    Fixture f;f.setup();const auto r=record();CHECK(f.publisher.publish(r));f.adopt();L::Owned o;
    auto m=L::AssociationTestAccess::map(f.publisher);const uint32_t sequence=m->words[P::SEQUENCE].load();
    m->words[P::SEQUENCE].store(sequence+1);uint32_t before[P::WORDS];
    for(unsigned i=0;i<P::WORDS;++i)before[i]=m->words[i].load();
    pid_t child=fork();CHECK(child>=0);
    if(!child) { f.publisher.disable_after_fork();f.registry.disable_after_fork();
        const bool bad=f.publisher.publish(record(9,2000))||lookup(f.registry,r,&o);
        f.publisher.invalidate();_exit(bad?9:0); }
    int status=0;CHECK(waitpid(child,&status,0)==child);CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==0);
    bool unchanged=true;for(unsigned i=0;i<P::WORDS;++i)unchanged=unchanged&&(m->words[i].load()==before[i]);
    CHECK(unchanged);m->words[P::SEQUENCE].store(sequence+2);
    CHECK(lookup(f.registry,r,&o));
    // The odd sequence above proves that child publisher operations cannot
    // write the parent's map. Test the registry independently with a readable
    // even map: the same inherited request matches without disable, and only
    // child-local disable must make that request unavailable.
    for(unsigned disable=0;disable<2;++disable) {
        CHECK(lookup(f.registry,r,&o)&&o.source_instance==41);
        child=fork();CHECK(child>=0);
        if(!child) {
            if(disable)f.registry.disable_after_fork();
            const bool matched=lookup(f.registry,r,&o);
            const bool correct=disable?(!matched&&o.result==L::UNAVAILABLE):
                (matched&&o.source_instance==41&&o.record_sequence==1);
            _exit(correct?0:10);
        }
        CHECK(waitpid(child,&status,0)==child);
        CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==0);
        CHECK(lookup(f.registry,r,&o)&&o.source_instance==41&&o.record_sequence==1);
    }
}
static void stopped() {
    Fixture f;f.setup();const auto r=record();CHECK(f.publisher.publish(r));f.adopt();L::Owned o;
    CHECK(lookup(f.registry,r,&o));f.registry.close_channel();CHECK(!lookup(f.registry,r,&o));
    CHECK(f.registry.open_channel(f.channel,geteuid()));CHECK(f.publisher.offer(f.channel,2000000000ULL));
    f.registry.drain(2000000000ULL);CHECK(!lookup(f.registry,r,&o));
}
static unsigned fd_count() {
    DIR* d=opendir("/proc/self/fd");CHECK(d);unsigned n=0;
    while(readdir(d))++n;
    CHECK(closedir(d)==0);return n;
}
static void allocation() {
    Fixture f;const unsigned before=fd_count();fail_pwrite=true;
    CHECK(!f.publisher.prepare(41,f.dir));fail_pwrite=false;
    CHECK(fd_count()==before);f.setup();CHECK(f.publisher.publish(record()));f.adopt();
    L::Owned o;CHECK(lookup(f.registry,record(),&o));
    errno=0;void* forbidden=mmap(0,P::MAP_BYTES,PROT_READ|PROT_WRITE,MAP_SHARED,L::AssociationTestAccess::fd(f.publisher),0);
    CHECK(forbidden==MAP_FAILED&&errno==EACCES);
}
static void scalar_bits() {
    Fixture f;f.setup();auto r=record();uint64_t nan=0x7ff8000000000021ULL;
    memcpy(&r.position.heading_deg,&nan,sizeof nan);r.position.latitude_deg=-0.0;
    // Bytes beyond a complete C string are not part of the wire identity.
    r.wire.server_guid.bytes[40]='x';CHECK(f.publisher.publish(r));f.adopt();L::Owned o;
    Q::Trace t=trace(r);t.issue.endpoint.server_guid.bytes[40]=0;
    A::PositionContext c={r.position,Q::OK,t,5,9,0};CHECK(f.registry.read(c,&o));
    auto changed=r;changed.position.latitude_deg=+0.0;
    CHECK(!lookup(f.registry,changed,&o)&&o.result==L::PAYLOAD_MISMATCH);
    ++nan;memcpy(&changed.position.heading_deg,&nan,sizeof nan);changed.position.latitude_deg=-0.0;
    CHECK(!lookup(f.registry,changed,&o)&&o.result==L::PAYLOAD_MISMATCH);
}
static void old_offer() {
    Fixture f;f.setup();CHECK(f.publisher.publish(record()));f.adopt();L::Publisher newer;
    CHECK(newer.prepare(42,f.dir));CHECK(newer.publish(record(8,3000)));
    CHECK(newer.offer(f.channel,2000));CHECK(f.publisher.offer(f.channel,2000000000ULL));
    f.registry.drain(2010);L::Owned o;
    CHECK(lookup(f.registry,record(8,3000),&o)&&o.source_instance==42);
    CHECK(!lookup(f.registry,record(),&o));
    L::Publisher equal;CHECK(equal.prepare(43,f.dir));CHECK(equal.publish(record(9,4000)));
    L::AssociationTestAccess::created(equal,L::AssociationTestAccess::created(newer));
    CHECK(equal.offer(f.channel,3010));f.registry.drain(3010);
    CHECK(lookup(f.registry,record(8,3000),&o)&&o.source_instance==42);
}
static void loss() {
    Fixture f;f.setup();CHECK(f.publisher.publish(record()));f.adopt();L::Owned o;
    L::AssociationTestAccess::busy(f.publisher,1);CHECK(!f.publisher.publish(record(8,2000)));
    CHECK(!lookup(f.registry,record(),&o));L::AssociationTestAccess::busy(f.publisher,0);
    CHECK(!f.publisher.publish(record(9,3000)));CHECK(!f.publisher.publish(record()));
    CHECK(f.publisher.publish(record(10,4000)));CHECK(lookup(f.registry,record(10,4000),&o));
}
static void borrowing() {
    Fixture f;f.setup();CHECK(f.publisher.publish(record()));f.adopt();L::Owned o;
    unsigned a=L::AssociationTestAccess::hold(f.registry);CHECK(a<2);
    L::Publisher b,c;CHECK(b.prepare(42,f.dir));CHECK(c.prepare(43,f.dir));
    CHECK(b.publish(record(8,3000)));CHECK(b.offer(f.channel,2000));f.registry.drain(2000);
    CHECK(lookup(f.registry,record(8,3000),&o));CHECK(L::AssociationTestAccess::mappings(f.registry)==2);
    unsigned second=L::AssociationTestAccess::hold(f.registry);CHECK(second<2&&second!=a);
    CHECK(c.publish(record(9,5000)));CHECK(c.offer(f.channel,4000));f.registry.drain(4000);
    CHECK(!lookup(f.registry,record(9,5000),&o));CHECK(L::AssociationTestAccess::mappings(f.registry)==2);
    L::AssociationTestAccess::release(f.registry,a);L::AssociationTestAccess::release(f.registry,second);
    f.registry.drain(5000);CHECK(L::AssociationTestAccess::mappings(f.registry)==0);
    CHECK(c.offer(f.channel,2000000000ULL));f.registry.drain(2000000000ULL);
    CHECK(!lookup(f.registry,record(9,5000),&o));CHECK(c.publish(record(10,2000000100ULL)));
    CHECK(lookup(f.registry,record(10,2000000100ULL),&o));
}
static void exhaustion() {
    { Fixture f;f.setup();CHECK(f.publisher.publish(record()));f.adopt();L::Owned o;
      L::AssociationTestAccess::sequence(f.publisher,UINT32_MAX);
      CHECK(!f.publisher.publish(record(8,2000)));CHECK(!lookup(f.registry,record(),&o)); }
    { Fixture f;f.setup();CHECK(f.publisher.publish(record()));f.adopt();L::Owned o;
      L::AssociationTestAccess::map(f.publisher)->words[P::SEQUENCE].store(UINT32_MAX-1);
      CHECK(!f.publisher.publish(record(8,2000)));CHECK(!lookup(f.registry,record(),&o)); }
    { Fixture f;f.setup();CHECK(f.publisher.publish(record()));f.adopt();L::Owned o;
      L::AssociationTestAccess::map(f.publisher)->words[P::LOSS].store(UINT32_MAX);f.publisher.invalidate();
      CHECK(!lookup(f.registry,record(),&o));CHECK(!f.publisher.publish(record(8,2000))); }
    { Fixture f;f.setup();CHECK(f.publisher.publish(record()));L::AssociationTestAccess::revision(f.registry,0x3fffffff);
      f.adopt();L::Owned o;CHECK(!lookup(f.registry,record(),&o)); }
    { Fixture f;f.setup();CHECK(f.publisher.publish(record()));f.adopt();L::Owned o;
      L::AssociationTestAccess::gate(f.registry,0x7fffffff);CHECK(!lookup(f.registry,record(),&o));
      L::AssociationTestAccess::gate(f.registry,0);CHECK(lookup(f.registry,record(),&o)); }
}
static void send_offer(const char* channel,L::Publisher& p,const int* fds,unsigned count,unsigned size=P::OFFER_BYTES,unsigned version=P::VERSION) {
    const auto m=L::AssociationTestAccess::map(p);
    uint32_t words[8]={P::OFFER_MAGIC,version,P::MAP_BYTES,m->words[P::INSTANCE].load(),m->words[P::INSTANCE+1].load(),
        uint32_t(getpid()),m->words[P::CREATED].load(),m->words[P::CREATED+1].load()};
    unsigned char bytes[P::OFFER_BYTES+1]={0};
    for(unsigned i=0;i<8;++i)for(unsigned j=0;j<4;++j)bytes[i*4+j]=static_cast<unsigned char>(words[i]>>(j*8));
    int sock=socket(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC,0);CHECK(sock>=0);
    sockaddr_un addr=sockaddr_un();addr.sun_family=AF_UNIX;memcpy(addr.sun_path+1,channel,strlen(channel));
    union { cmsghdr align;unsigned char bytes[CMSG_SPACE(P::MAX_RIGHTS*sizeof(int))]; } control;
    memset(&control,0,sizeof control);iovec io={bytes,size};msghdr message=msghdr();
    message.msg_iov=&io;message.msg_iovlen=1;message.msg_name=&addr;message.msg_namelen=offsetof(sockaddr_un,sun_path)+1+strlen(channel);
    if(count) {
        CHECK(count<=P::MAX_RIGHTS);message.msg_control=control.bytes;message.msg_controllen=CMSG_SPACE(count*sizeof(int));
        cmsghdr* c=CMSG_FIRSTHDR(&message);c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;c->cmsg_len=CMSG_LEN(count*sizeof(int));
        memcpy(CMSG_DATA(c),fds,count*sizeof(int));
    }
    CHECK(sendmsg(sock,&message,MSG_NOSIGNAL)==ssize_t(size));CHECK(close(sock)==0);
}
static void heading_presence() {
    // Each invalid input starts with its own healthy loss epoch. A rejection
    // must not pass merely because an earlier malformed input left a reset
    // pending on the next otherwise valid publish.
    for(unsigned fault=0;fault<2;++fault) {
        Fixture fresh;printf("owned_fixture_dir=%s\n",fresh.dir);fflush(stdout);
        fresh.setup();auto valid=record(41,4100);valid.position.heading_deg=0;
        valid.field_lineage.heading_presence=N::EMPTY;
        CHECK(fresh.publisher.publish(valid));fresh.adopt(4110);L::Owned current;
        CHECK(lookup(fresh.registry,valid,&current));CHECK(current.heading_presence==N::EMPTY);
        const L::Owned saved=current;auto m=L::AssociationTestAccess::map(fresh.publisher);
        const uint32_t loss=m->words[P::LOSS].load();CHECK(loss==1);
        auto invalid=record(42,4200);invalid.field_lineage.heading_presence=N::EMPTY;
        if(fault==0)invalid.field_lineage.heading_presence=N::Presence(3);
        else invalid.field_lineage.fields[mx5::sensors::lds_lineage::HEADING]={0,0};
        CHECK(!fresh.publisher.publish(invalid));CHECK(m->words[P::LOSS].load()==loss+1);
        CHECK(!lookup(fresh.registry,valid,&current));CHECK(current.result==L::UNAVAILABLE);
        CHECK(!fresh.publisher.publish(record(43,4300)));
        CHECK(m->words[P::COUNT].load()==0);CHECK(m->words[P::LOSS].load()==loss+1);
        auto recovered=record(44,4400);recovered.position.heading_deg=0;
        recovered.field_lineage.heading_presence=N::PRESENT;
        CHECK(fresh.publisher.publish(recovered));CHECK(lookup(fresh.registry,recovered,&current));
        CHECK(current.heading_presence==N::PRESENT&&current.map_loss_epoch==loss+1);
        CHECK(!lookup(fresh.registry,valid,&current));CHECK(current.result==L::UNAVAILABLE);
        CHECK(saved.heading_presence==N::EMPTY&&saved.map_loss_epoch==loss);
    }
    Fixture f;printf("owned_fixture_dir=%s\n",f.dir);fflush(stdout);
    f.setup();f.adopt();L::Owned o;
    for(unsigned value=0;value<=2;++value) {
        auto r=record(20+value,2000+100*value);r.position.heading_deg=0;
        r.field_lineage.heading_presence=N::Presence(value);
        CHECK(f.publisher.publish(r));CHECK(lookup(f.registry,r,&o));
        CHECK(o.heading_presence==N::Presence(value));CHECK(o.fields[5].write_sequence==3);
    }
    auto r=record(40,4000);r.field_lineage.heading_presence=N::EMPTY;
    CHECK(f.publisher.publish(r));CHECK(lookup(f.registry,r,&o));const L::Owned old=o;
    r.field_lineage.heading_presence=N::PRESENT;CHECK(f.publisher.publish(r));
    CHECK(!lookup(f.registry,r,&o));CHECK(o.result==L::CONFLICT);CHECK(old.heading_presence==N::EMPTY);
    r=record(41,4100);r.field_lineage.heading_presence=N::Presence(3);CHECK(!f.publisher.publish(r));
    r=record(42,4200);r.field_lineage.fields[5]={0,0};r.field_lineage.heading_presence=N::EMPTY;
    CHECK(!f.publisher.publish(r));
}
static void rmc_status() {
    // The producer and read-only adopter must retain every lexical state.
    {
        Fixture f;printf("owned_fixture_dir=%s\n",f.dir);fflush(stdout);
        f.setup();f.adopt();L::Owned o;
        for(unsigned value=0;value<5;++value) {
            auto r=record(20+value,2000+100*value);
            r.field_lineage.heading_rmc_status=N::RmcStatus(value);
            CHECK(f.publisher.publish(r));CHECK(lookup(f.registry,r,&o));
            CHECK(o.heading_rmc_status==N::RmcStatus(value));
            const auto m=L::AssociationTestAccess::map(f.publisher);
            CHECK(m->words[P::HEADER+value*P::RECORD_WORDS+130].load()==value);
            for(unsigned word=131;word<P::RECORD_WORDS;++word)
                CHECK(m->words[P::HEADER+value*P::RECORD_WORDS+word].load()==0);
        }
        auto r=record(40,4000);r.field_lineage.heading_rmc_status=N::RMC_A;
        CHECK(f.publisher.publish(r));CHECK(lookup(f.registry,r,&o));const L::Owned old=o;
        r.field_lineage.heading_rmc_status=N::RMC_V;CHECK(f.publisher.publish(r));
        CHECK(!lookup(f.registry,r,&o));CHECK(o.result==L::CONFLICT);
        CHECK(old.heading_rmc_status==N::RMC_A);
    }
    // Separate healthy windows expose either missing validation guard.
    for(unsigned fault=0;fault<2;++fault) {
        Fixture f;printf("owned_fixture_dir=%s\n",f.dir);fflush(stdout);
        f.setup();auto r=record(41,4100);r.field_lineage.heading_rmc_status=N::RMC_A;
        CHECK(f.publisher.publish(r));f.adopt(4110);L::Owned o;
        CHECK(lookup(f.registry,r,&o));const L::Owned saved=o;
        const auto m=L::AssociationTestAccess::map(f.publisher);const unsigned loss=m->words[P::LOSS].load();
        auto bad=record(42,4200);bad.field_lineage.heading_rmc_status=N::RMC_A;
        if(fault==0)bad.field_lineage.heading_rmc_status=N::RmcStatus(5);
        else bad.field_lineage.fields[5]={0,0};
        CHECK(!f.publisher.publish(bad));CHECK(m->words[P::LOSS].load()==loss+1);
        CHECK(!lookup(f.registry,r,&o));CHECK(o.result==L::UNAVAILABLE);
        CHECK(!f.publisher.publish(record(43,4300)));
        auto recovered=record(44,4400);recovered.field_lineage.heading_rmc_status=N::RMC_V;
        CHECK(f.publisher.publish(recovered));CHECK(lookup(f.registry,recovered,&o));
        CHECK(o.heading_rmc_status==N::RMC_V&&o.map_loss_epoch==loss+1);
        CHECK(saved.heading_rmc_status==N::RMC_A&&saved.map_loss_epoch==loss);
    }
    // A malformed shared record must also be rejected at the consumer boundary.
    for(unsigned fault=0;fault<2;++fault) {
        Fixture f;printf("owned_fixture_dir=%s\n",f.dir);fflush(stdout);
        f.setup();auto r=record();r.field_lineage.heading_rmc_status=N::RMC_A;
        CHECK(f.publisher.publish(r));f.adopt();L::Owned o;CHECK(lookup(f.registry,r,&o));
        auto m=L::AssociationTestAccess::map(f.publisher);
        if(fault==0)m->words[P::HEADER+130].store(5);
        else {m->words[P::HEADER+P::ORIGINS+4*5].store(0);m->words[P::HEADER+P::ORIGINS+4*5+1].store(0);}
        CHECK(!lookup(f.registry,r,&o));CHECK(o.result==L::UNAVAILABLE);
        CHECK(o.heading_rmc_status==N::RMC_UNKNOWN);
    }
}
static void protocol_version() {
    for(unsigned legacy=1;legacy<=2;++legacy) {
        Fixture f;printf("owned_fixture_dir=%s\n",f.dir);fflush(stdout);
        f.setup();CHECK(f.publisher.publish(record()));L::Owned o;
        const int fd=L::AssociationTestAccess::fd(f.publisher);
        send_offer(f.channel,f.publisher,&fd,1,P::OFFER_BYTES,legacy);f.registry.drain(1010);
        CHECK(!lookup(f.registry,record(),&o));
        auto map=L::AssociationTestAccess::map(f.publisher);map->words[P::LAYOUT].store(legacy);
        send_offer(f.channel,f.publisher,&fd,1);f.registry.drain(1020);CHECK(!lookup(f.registry,record(),&o));
        map->words[P::LAYOUT].store(P::VERSION);f.adopt(1030);
        CHECK(lookup(f.registry,record(),&o));CHECK(o.layout_version==3);
    }
}
enum BadFd { GOOD_FD,WRITABLE_FD,LINKED_FD,LARGE_FD,SHORT_FD,EMPTY_FD,MODE_FD };
static void fd_rejected_then_adopted(unsigned rights_count,BadFd kind) {
    Fixture f;f.setup();CHECK(f.publisher.publish(record()));f.adopt();L::Owned o;
    CHECK(lookup(f.registry,record(),&o)&&o.source_instance==41);
    L::Publisher next;CHECK(next.prepare(42,f.dir));const auto fresh=record(8,3000);
    CHECK(L::AssociationTestAccess::created(next)>L::AssociationTestAccess::created(f.publisher));
    CHECK(next.publish(fresh));const int good=L::AssociationTestAccess::fd(next);
    int offered=good,extra=-1;char path[256]={0};
    if(kind==WRITABLE_FD) {
        snprintf(path,sizeof path,"/proc/self/fd/%d",good);
        extra=open(path,O_RDWR|O_CLOEXEC);CHECK(extra>=0);offered=extra;
    } else if(kind==MODE_FD)CHECK(fchmod(good,0644)==0);
    else if(kind==LINKED_FD||kind==LARGE_FD||kind==SHORT_FD||kind==EMPTY_FD) {
        snprintf(path,sizeof path,"%s/candidate",f.dir);
        int writer=open(path,O_CREAT|O_EXCL|O_RDWR|O_CLOEXEC,0600);CHECK(writer>=0);
        const unsigned copied=kind==EMPTY_FD?0:kind==SHORT_FD?4096:unsigned(P::MAP_BYTES);
        const unsigned size=kind==LARGE_FD?unsigned(P::MAP_BYTES)+4096:copied;
        CHECK(ftruncate(writer,size)==0);
        // The exact newer identity, header and first record are valid. nlink
        // and size negatives must not be accidentally rejected as old offers
        // or zero-filled invalid headers before the guard under test matters.
        unsigned char bytes[4096];
        for(unsigned offset=0;offset<copied;offset+=sizeof bytes) {
            unsigned n=copied-offset;if(n>sizeof bytes)n=sizeof bytes;
            CHECK(pread(good,bytes,n,offset)==ssize_t(n));
            CHECK(pwrite(writer,bytes,n,offset)==ssize_t(n));
        }
        CHECK(close(writer)==0);extra=open(path,O_RDONLY|O_CLOEXEC);CHECK(extra>=0);offered=extra;
        if(kind!=LINKED_FD)CHECK(unlink(path)==0);
    }
    int descriptors[P::MAX_RIGHTS];CHECK(rights_count<=P::MAX_RIGHTS);
    for(unsigned i=0;i<rights_count;++i)descriptors[i]=offered;
    unsigned before=fd_count();
    std::printf("FD control count=%u kind=%u\n",rights_count,unsigned(kind));std::fflush(stdout);
    send_offer(f.channel,next,descriptors,rights_count);f.registry.drain(2000);
    CHECK(fd_count()==before);CHECK(lookup(f.registry,record(),&o)&&o.source_instance==41);
    CHECK(!lookup(f.registry,fresh,&o));
    if(extra>=0)CHECK(close(extra)==0);
    if(kind==LINKED_FD)CHECK(unlink(path)==0);
    if(kind==MODE_FD)CHECK(fchmod(good,0600)==0);
    // Positive control uses the SAME newer instance/creation/key that was
    // rejected above. No third identity or timestamp repair can hide a test
    // which never offered an otherwise-admissible map.
    before=fd_count();send_offer(f.channel,next,&good,1);f.registry.drain(2010);
    CHECK(fd_count()==before);CHECK(lookup(f.registry,fresh,&o)&&o.source_instance==42);
    CHECK(o.record_sequence==1&&o.cache_lifetime==2&&o.write_sequence==3);
    for(unsigned i=0;i<9;++i)CHECK(o.fields[i].write_sequence==3&&o.fields[i].observed_ns==2900);
    CHECK(!lookup(f.registry,record(),&o));
}
static void fd_validation() {
    for(unsigned count=0;count<=8;++count) {
        if(count==1)continue;
        fd_rejected_then_adopted(count,GOOD_FD);
    }
    fd_rejected_then_adopted(P::MAX_RIGHTS,GOOD_FD);
    fd_rejected_then_adopted(1,WRITABLE_FD);fd_rejected_then_adopted(1,MODE_FD);
    fd_rejected_then_adopted(1,LINKED_FD);fd_rejected_then_adopted(1,LARGE_FD);
    fd_rejected_then_adopted(1,SHORT_FD);fd_rejected_then_adopted(1,EMPTY_FD);
    Fixture f;f.setup();CHECK(f.publisher.publish(record()));f.adopt();L::Owned o;
    const int fd=L::AssociationTestAccess::fd(f.publisher);const unsigned before=fd_count();
    send_offer(f.channel,f.publisher,&fd,1,P::OFFER_BYTES+1);f.registry.drain(1040);
    CHECK(fd_count()==before);CHECK(lookup(f.registry,record(),&o));
    L::Publisher broken;CHECK(broken.prepare(99,f.dir));L::AssociationTestAccess::map(broken)->words[P::MAGIC].store(0);
    CHECK(broken.offer(f.channel,1080));f.registry.drain(1080);CHECK(lookup(f.registry,record(),&o));
    // Kernel-issued credentials disagree with this receiver's expected account.
    L::Registry denied;char name[96];snprintf(name,sizeof name,"mx5-assoc-wronguid-%ld",long(getpid()));
    CHECK(denied.open_channel(name,uid_t(geteuid()+1)));send_offer(name,f.publisher,&fd,1);denied.drain(1090);
    CHECK(!lookup(denied,record(),&o));denied.close_channel();
}
static void drain_bound() {
    Fixture f;f.setup();CHECK(f.publisher.publish(record()));L::Owned o;
    const int fd=L::AssociationTestAccess::fd(f.publisher);unsigned before=fd_count();
    send_offer(f.channel,f.publisher,0,0);send_offer(f.channel,f.publisher,0,0);send_offer(f.channel,f.publisher,&fd,1);
    f.registry.drain(1010);CHECK(!lookup(f.registry,record(),&o));CHECK(fd_count()==before);
    f.registry.drain(1011);CHECK(lookup(f.registry,record(),&o));CHECK(fd_count()==before);
}
static void malformed_map() {
    Fixture f;printf("owned_fixture_dir=%s\n",f.dir);fflush(stdout);
    auto r=record();r.position.heading_deg=0;r.field_lineage.heading_presence=N::EMPTY;
    f.setup();CHECK(f.publisher.publish(r));f.adopt();L::Owned o;
    auto m=L::AssociationTestAccess::map(f.publisher);
    const uint32_t good=m->words[P::SEQUENCE].load();m->words[P::SEQUENCE].store(good+1);
    CHECK(!lookup(f.registry,r,&o));m->words[P::SEQUENCE].store(good+2);CHECK(lookup(f.registry,r,&o));
    const uint32_t values[]={65,UINT32_MAX,0x80000000U};
    for(unsigned i=0;i<3;++i) { m->words[P::COUNT].store(values[i]);CHECK(!lookup(f.registry,r,&o)); }
    m->words[P::COUNT].store(1);CHECK(lookup(f.registry,r,&o));
    uint32_t normal[P::RECORD_WORDS];
    for(unsigned i=0;i<P::RECORD_WORDS;++i)normal[i]=m->words[P::HEADER+i].load();
    const L::Owned saved=o;
    for(unsigned fault=0;fault<2;++fault) {
        CHECK(lookup(f.registry,r,&o));CHECK(o.heading_presence==N::EMPTY);
        const uint32_t sequence=m->words[P::SEQUENCE].load();CHECK(!(sequence&1));
        m->words[P::SEQUENCE].store(sequence+1);
        if(fault==0)m->words[P::HEADER+P::HEADING_PRESENCE].store(3);
        else for(unsigned i=0;i<4;++i)
            m->words[P::HEADER+P::ORIGINS+4*mx5::sensors::lds_lineage::HEADING+i].store(0);
        m->words[P::SEQUENCE].store(sequence+2);
        // The record is stably readable; only the enum or known-presence
        // origin is malformed. It must fail in Registry rather than depend
        // on the later adapter gate or an odd writer sequence.
        CHECK(!lookup(f.registry,r,&o));CHECK(o.result==L::UNAVAILABLE);
        m->words[P::SEQUENCE].store(sequence+3);
        for(unsigned i=0;i<P::RECORD_WORDS;++i)m->words[P::HEADER+i].store(normal[i]);
        m->words[P::SEQUENCE].store(sequence+4);
        CHECK(lookup(f.registry,r,&o));CHECK(o.result==L::MATCHED_LOCKED_FOR_SEND);
        CHECK(o.heading_presence==N::EMPTY&&o.fields[5].write_sequence==3&&o.fields[5].observed_ns==900);
        CHECK(o.record_sequence==saved.record_sequence&&o.view_revision==saved.view_revision&&o.map_loss_epoch==saved.map_loss_epoch);
        for(unsigned i=0;i<P::RECORD_WORDS;++i)CHECK(m->words[P::HEADER+i].load()==normal[i]);
    }
    m->words[P::ENABLED].store(0);CHECK(!lookup(f.registry,r,&o));
}
static void concurrent() {
    Fixture f;f.setup();CHECK(f.publisher.publish(record()));f.adopt();L::Owned o;
    // Prove immediate retirement with a currently matchable record, before
    // capacity turnover can make this lookup fail independently of retire.
    for(unsigned i=0;i<20;++i)CHECK(lookup(f.registry,record(),&o)&&o.source_instance==41);
    f.registry.retire();for(unsigned i=0;i<20;++i)CHECK(!lookup(f.registry,record(),&o));
    CHECK(f.publisher.offer(f.channel,2000000000ULL));f.registry.drain(2000000000ULL);
    const auto watched=record(8,2000000100ULL);CHECK(f.publisher.publish(watched));
    CHECK(lookup(f.registry,watched,&o)&&o.record_sequence==2);
    // This remains concurrent read/turnover/reclamation coverage, not a proof
    // of ARM hardware ordering or of in-place torn-copy detection.
    std::atomic<unsigned> failures(0),started(0),done(0),reads(0);
    std::thread reader([&] {
        started.store(1);
        while(!done.load()) {
            L::Owned value;if(lookup(f.registry,watched,&value)&&(value.record_sequence!=2||value.source_instance!=41||value.fields[0].observed_ns!=2000000000ULL))++failures;
            ++reads;
        }
    });
    while(!started.load())std::this_thread::yield();
    for(unsigned i=0;i<512;++i)(void)f.publisher.publish(record(9+i,2000000200ULL+100*i));
    f.registry.retire();for(unsigned i=0;i<20;++i)CHECK(!lookup(f.registry,watched,&o));
    f.registry.drain(2000100000ULL);done.store(1);reader.join();CHECK(!failures.load());CHECK(reads.load()>0);
}
static void retirement_scheduled() {
    alarm(10);Fixture f;f.setup();CHECK(f.publisher.publish(record()));f.adopt();L::Owned o;
    unsigned held=L::AssociationTestAccess::hold(f.registry);CHECK(held<2);
    L::Publisher next;CHECK(next.prepare(42,f.dir));CHECK(next.publish(record(8,3000)));CHECK(next.offer(f.channel,2000));
    pause_mapping.store(1);std::thread worker([&]{f.registry.drain(2000);});
    while(!mapping_entered.load())usleep(1000);
    // Real retire interleaves after the worker has received the FD and captured
    // its old epoch. Retire owns only the atomic epoch; gate writes belong to
    // the worker, so it cannot close a newly reopened slot later in this call.
    f.registry.retire();CHECK(!lookup(f.registry,record(),&o));
    CHECK(L::AssociationTestAccess::gate_value(f.registry,held)==1);
    mapping_release.store(1);worker.join();pause_mapping.store(0);
    CHECK(!lookup(f.registry,record(8,3000),&o));
    L::AssociationTestAccess::release(f.registry,held);f.registry.drain(2010);
    CHECK(L::AssociationTestAccess::mappings(f.registry)==0);
    CHECK(next.offer(f.channel,2000000000ULL));f.registry.drain(2000000000ULL);
    CHECK(!lookup(f.registry,record(8,3000),&o));CHECK(next.publish(record(9,2000000100ULL)));
    CHECK(lookup(f.registry,record(9,2000000100ULL),&o));alarm(0);
}
int main(int argc,char** argv) {
    CHECK(argc==2);
    if(!std::strcmp(argv[1],"basic"))basic();else if(!std::strcmp(argv[1],"exact"))exact();
    else if(!std::strcmp(argv[1],"capacity"))capacity();else if(!std::strcmp(argv[1],"invalidation"))invalidation();
    else if(!std::strcmp(argv[1],"fork"))forked();else if(!std::strcmp(argv[1],"stop"))stopped();
    else if(!std::strcmp(argv[1],"allocation"))allocation();else if(!std::strcmp(argv[1],"scalar_bits"))scalar_bits();
    else if(!std::strcmp(argv[1],"old_offer"))old_offer();else if(!std::strcmp(argv[1],"loss"))loss();
    else if(!std::strcmp(argv[1],"borrowing"))borrowing();else if(!std::strcmp(argv[1],"exhaustion"))exhaustion();
    else if(!std::strcmp(argv[1],"fd_validation"))fd_validation();else if(!std::strcmp(argv[1],"drain_bound"))drain_bound();
    else if(!std::strcmp(argv[1],"malformed_map"))malformed_map();else if(!std::strcmp(argv[1],"concurrent"))concurrent();
    else if(!std::strcmp(argv[1],"retirement_scheduled"))retirement_scheduled();
    else if(!std::strcmp(argv[1],"heading_presence"))heading_presence();
    else if(!std::strcmp(argv[1],"rmc_status"))rmc_status();
    else if(!std::strcmp(argv[1],"protocol_version"))protocol_version();else CHECK(false);
    std::printf("PASS LDS association %s %u checks\n",argv[1],checks);
}
