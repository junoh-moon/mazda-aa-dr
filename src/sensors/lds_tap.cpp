#include "runtime/loader.h"
#include "adapter/lds_install.h"
#include "runtime/config.h"
#include "runtime/sha256.h"
#include <errno.h>
#include <new>
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
namespace T=mx5::sensors;
bool policy_decided,allowed,bootstrap_attempted;
alignas(S::Sender) unsigned char sender_storage[sizeof(S::Sender)];
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
void emit(const S::Record& record,void* user) {
    // Actual hooks call only after the original Path returns and cache unlocks.
    // No retry, persistent file, worker thread or producer qualification here.
    if(user)static_cast<S::Sender*>(user)->try_send(record);
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
    // Retain this object and its socket until process exit. A prepared target
    // and emit userdata can remain reachable after partial publication/rollback.
    S::Sender* sender=new(sender_storage)S::Sender();
    if(!sender->open_channel(MX5_LDS_CHANNEL)) {
        T::report.state=T::LdsTapReport::SENDER_FAILED;
        diagnostic("sender_unavailable");return;
    }
    A::LdsInstallOptions options=A::LdsInstallOptions();
    options.service_handle=handle;options.verify_file_hash=mx5_verify_file_sha256;
    options.verified_cold_start=true;
    options.begin_patch=loader_begin_patch;options.end_patch=loader_end_patch;
    options.clock=clock_ns;options.emit=emit;options.user=sender;
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
