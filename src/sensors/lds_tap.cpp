#include "runtime/loader.h"
#include "adapter/lds_install.h"
#include "runtime/config.h"
#include "runtime/sha256.h"
#include "runtime/lds_association_channel.h"
#include <atomic>
#include <errno.h>
#include <new>
#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#ifndef MX5_LDS_CONFIG_PATH
#define MX5_LDS_CONFIG_PATH "/data_persist/mx5-aa-dr/mx5dr.conf"
#endif
#ifndef MX5_LDS_DISABLE_PATH
#define MX5_LDS_DISABLE_PATH "/data_persist/mx5-aa-dr/logs/disable-next-start"
#endif
#ifndef MX5_LDS_CHANNEL
#define MX5_LDS_CHANNEL mx5::runtime::lds_sideband::CHANNEL_NAME
#endif
#ifndef MX5_LDS_ASSOCIATION_CHANNEL
#define MX5_LDS_ASSOCIATION_CHANNEL mx5::runtime::lds_association::CHANNEL_NAME
#endif
#ifndef MX5_LDS_ASSOCIATION_DIRECTORY
#define MX5_LDS_ASSOCIATION_DIRECTORY "/tmp"
#endif
namespace mx5 { namespace sensors {
struct LdsTapReport {
    enum State { UNSEEN,BYPASSED,ENABLED,SENDER_FAILED,INSTALLING,INSTALLED,INSTALL_FAILED } state;
    unsigned mode;
    bool install_attempted;
    adapter::InstallResult installation;
    int diagnostic_bytes,diagnostic_errno;
};
static LdsTapReport report={LdsTapReport::UNSEEN,0,false,adapter::CONFIGURATION_FAILED,0,0};
// Read only after the startup owner's target dlopen returns, with external
// synchronization against startup. This is not a concurrent telemetry API.
const LdsTapReport& lds_tap_report() { return report; }
} }
namespace {
namespace A=mx5::adapter;
namespace S=mx5::runtime::lds_sideband;
namespace L=mx5::runtime::lds_association;
namespace T=mx5::sensors;
bool policy_decided,allowed,bootstrap_attempted;
struct TapState {
    S::Sender sender;
    L::Publisher publisher;
    std::atomic<unsigned> child_disabled;
    TapState():sender(),publisher(),child_disabled(0) {}
};
alignas(TapState) unsigned char tap_storage[sizeof(TapState)];
std::atomic<TapState*> retained_state(0);
static_assert(ATOMIC_POINTER_LOCK_FREE==2 && ATOMIC_INT_LOCK_FREE==2,
              "Child fork fence must use lock-free local atomics");
struct Errno {
    const int value;
    Errno():value(errno) {}
    ~Errno() { errno=value; }
};
uint64_t clock_ns(void*) {
    const Errno saved;
    timespec time;
    if(clock_gettime(CLOCK_MONOTONIC,&time) || time.tv_sec<0)return 0;
    return uint64_t(time.tv_sec)*1000000000ULL+uint64_t(time.tv_nsec);
}
void child_after_fork() {
    // The storage is private to this process; the inherited mapping is shared.
    // Only revoke local admission: no parent-map write, close/unmap or lock.
    TapState* state=retained_state.load(std::memory_order_acquire);
    if(state) {
        state->child_disabled.store(1,std::memory_order_release);
        state->publisher.disable_after_fork();
    }
}
void publish_locked(const A::LdsLockedSend& record,void* user) {
    const Errno saved;
    TapState* state=static_cast<TapState*>(user);
    if(state && !state->child_disabled.load(std::memory_order_acquire))state->publisher.publish(record);
}
void invalidate_locked(A::LdsLockedLoss,void* user) {
    const Errno saved;
    TapState* state=static_cast<TapState*>(user);
    if(state && !state->child_disabled.load(std::memory_order_acquire))state->publisher.invalidate();
}
void emit(const S::Record& record,void* user) {
    // Actual hooks call only after the original Path returns and cache unlocks.
    // No retry, persistent file, worker thread or producer qualification here.
    const Errno saved;
    TapState* state=static_cast<TapState*>(user);
    if(!state || state->child_disabled.load(std::memory_order_acquire))return;
    state->sender.try_send(record);
    // FD transfer is post-Path only, never in the two lock observers. The
    // Publisher bounds offers to one attempt/second, including absent peers.
    state->publisher.offer(MX5_LDS_ASSOCIATION_CHANNEL,clock_ns(0));
}
void diagnostic(const char* state) {
    char line[192];
    const int n=snprintf(line,sizeof line,
        "mx5dr lds: state=%s mode=%u install_attempted=%u install_code=%d\n",state,
        T::report.mode,unsigned(T::report.install_attempted),int(T::report.installation));
    if(n<=0 || size_t(n)>=sizeof line)return;
    // One startup-only, fixed-size best-effort stderr diagnostic. This does not
    // claim durable logging or a wall-time bound if the OEM stderr sink blocks.
    const ssize_t written=write(STDERR_FILENO,line,size_t(n));
    T::report.diagnostic_bytes=int(written);
    T::report.diagnostic_errno=written<0?errno:0;
}
}
namespace mx5 { namespace runtime {
bool loader_enabled() {
    const Errno saved;
    if(policy_decided)return allowed;
    policy_decided=true;
    Config config;
    allowed=startup_enabled(MX5_LDS_CONFIG_PATH,MX5_LDS_DISABLE_PATH,&config);
    T::report.mode=config.mode;
    T::report.state=allowed?T::LdsTapReport::ENABLED:T::LdsTapReport::BYPASSED;
    return allowed;
}
void loader_bootstrap(void* handle) {
    const Errno saved;
    if(!allowed || bootstrap_attempted)return;
    bootstrap_attempted=true;
    if(!handle) {
        T::report.state=T::LdsTapReport::INSTALL_FAILED;
        T::report.installation=A::INVALID_INSTALL_ARGUMENT;
        diagnostic("invalid_handle");return;
    }
    // No destructor: immutable hook userdata survives partial publication and
    // rollback. Map/FD preparation and atfork registration occur off the lease.
    TapState* state=new(tap_storage)TapState();
    retained_state.store(state,std::memory_order_release);
    const uint64_t instance=clock_ns(0); // observation instance, not measurement time
    if(!instance || !state->sender.open_channel(MX5_LDS_CHANNEL,instance)) {
        T::report.state=T::LdsTapReport::SENDER_FAILED;
        diagnostic("sender_unavailable");return;
    }
    if(pthread_atfork(0,0,child_after_fork)==0)
        state->publisher.prepare(instance,MX5_LDS_ASSOCIATION_DIRECTORY);
    else state->publisher.disable_after_fork();
    // Missing map backing or fork admission disables only supplementary
    // association. The original installation and v1 Sender remain available.
    A::LdsInstallOptions options=A::LdsInstallOptions();
    options.service_handle=handle;options.verify_file_hash=mx5_verify_file_sha256;
    options.verified_cold_start=true;
    options.begin_patch=loader_begin_patch;options.end_patch=loader_end_patch;
    options.clock=clock_ns;options.emit=emit;options.user=state;
    options.publish_locked=publish_locked;options.invalidate_locked=invalidate_locked;
    T::report.install_attempted=true;T::report.state=T::LdsTapReport::INSTALLING;
    T::report.installation=A::install_lds_v74(options);
    const bool installed=T::report.installation==A::INSTALL_OK;
    T::report.state=installed?T::LdsTapReport::INSTALLED:T::LdsTapReport::INSTALL_FAILED;
    // The data-only installer always releases a begun lease with end(true),
    // including rollback errors. It never creates an executable-page fatal
    // failure, writes a new disable marker or terminates the OEM service here.
    diagnostic(installed?"installed":"install_failed");
}
} }
