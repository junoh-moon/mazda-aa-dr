// Independent authored receive-order regression. No OEM data or product clock injection.
#include "navigation/channel.h"
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
namespace channel_receive_clock_test {
namespace N=mx5::navigation;
static N::MotionSender* pending_sender;
static N::RawEvent injected;
static unsigned injections;
static int forced_errno;
static uint64_t now() {
    timespec time;assert(clock_gettime(CLOCK_MONOTONIC,&time)==0);
    return uint64_t(time.tv_sec)*1000000000ULL+time.tv_nsec;
}
static N::ReceiveResult receive(N::MotionReceiver& receiver,N::RawEvent* out,N::ReceiveDiagnostic* diagnostic) {
    return receiver.receive(out,diagnostic);
}
}
extern "C" ssize_t __real_recvmsg(int,struct msghdr*,int);
extern "C" ssize_t __wrap_recvmsg(int fd,struct msghdr* message,int flags) {
    using namespace channel_receive_clock_test;
    if(forced_errno) { const int error=forced_errno;forced_errno=0;errno=error;return -1; }
    if(pending_sender) {
        N::MotionSender* sender=pending_sender;pending_sender=0;
        // A real newer receipt after receive() has already entered recvmsg.
        // This is a scheduling point; there is no fabricated clock value.
        usleep(1000);injected.received_ns=now();
        assert(sender->send_event(injected));++injections;
    }
    return __real_recvmsg(fd,message,flags);
}
static void receive_clock_order_test() {
    using namespace channel_receive_clock_test;
    N::MotionReceiver receiver;N::MotionSender sender;
    char channel[80];snprintf(channel,sizeof channel,"mx5dr.recv.clock.%ld",(long)getpid());
    assert(receiver.open_channel(channel)&&sender.open_channel(channel));
    injected=N::RawEvent();injected.kind=N::REVERSE;injected.epoch=3;
    injected.receive_seq=1;injected.count=1;injected.reverse=1;
    for(unsigned i=0;i<4;++i)injected.raw[i]=uint16_t(11*(i+1));
    pending_sender=&sender;const uint64_t before=now();
    N::RawEvent out;N::ReceiveDiagnostic d;
    const N::ReceiveResult result=receive(receiver,&out,&d);
    printf("recv-clock: result=%u reason=%s before=%llu received=%llu checked=%llu injections=%u\n",
        unsigned(result),N::receive_fault_name(d.reason),(unsigned long long)before,
        (unsigned long long)injected.received_ns,(unsigned long long)d.checked_ns,injections);fflush(stdout);
    assert(result==N::CHANNEL_EVENT&&d.reason==N::RECEIVE_OK&&injections==1);
    assert(injected.received_ns>before&&d.checked_ns>=injected.received_ns);
    assert(out.received_ns==injected.received_ns&&out.epoch==3&&out.receive_seq==1&&out.reverse==1);
    for(unsigned i=0;i<4;++i)assert(out.raw[i]==11*(i+1));
    // Actual future data remains rejected; the cursor must not consume it.
    N::RawEvent future=injected;future.receive_seq=2;future.received_ns=now()+60000000000ULL;
    assert(sender.send_event(future));
    assert(receive(receiver,&out,&d)==N::CHANNEL_FAULT&&d.reason==N::RECEIVE_FUTURE);
    assert(d.authenticated_decoded&&d.rejected.receive_seq==2&&d.rejected.received_ns==future.received_ns);
    future.received_ns=now();assert(sender.send_event(future));
    assert(receive(receiver,&out,&d)==N::CHANNEL_EVENT&&out.receive_seq==2);
    forced_errno=EIO;
    assert(receive(receiver,&out,&d)==N::CHANNEL_FAULT&&d.reason==N::RECEIVE_SYSCALL&&d.syscall_errno==EIO);
    assert(!d.authenticated_decoded&&out.receive_seq==0);
    forced_errno=EINTR;assert(receive(receiver,&out,&d)==N::CHANNEL_EMPTY);
    puts("PASS recv-clock: post-receive time, exact accepted payload, real future rejection, cursor continuity and errno");
}
