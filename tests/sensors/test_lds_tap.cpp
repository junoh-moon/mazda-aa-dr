// Authored installer witness; actual config parser, Sender/Receiver and tap.
// The separate loader fixture exercises the real dlopen interposer/lease.
#include <limits.h>
static char tap_config[PATH_MAX],tap_disable[PATH_MAX],tap_channel[80];
#define MX5_LDS_CONFIG_PATH tap_config
#define MX5_LDS_DISABLE_PATH tap_disable
#define MX5_LDS_CHANNEL tap_channel
#include "../../src/sensors/lds_tap.cpp"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
namespace TestAdapter=mx5::adapter;
namespace TestSideband=mx5::runtime::lds_sideband;
namespace TestTap=mx5::sensors;
namespace {
unsigned installs,begins,ends;
bool cold=true;
TestAdapter::InstallResult install_result=TestAdapter::INSTALL_OK;
TestAdapter::LdsInstallOptions captured;
int service;
TestSideband::Record example() {
    TestSideband::Record r=TestSideband::Record();r.flags=TestSideband::SNAPSHOT_KNOWN;
    r.field_lineage.lifetime=3;r.field_lineage.write_sequence=4;
    r.field_lineage.fields[0].write_sequence=4;
    r.position.mode=1;r.position.utc_seconds=123;r.position.latitude_deg=12.5;
    return r;
}
void emit_witness() {
    TestSideband::Record r=example();r.observed_ns=captured.clock(captured.user);
    assert(r.observed_ns>0);
    errno=E2BIG;captured.emit(r,captured.user);assert(errno==E2BIG);
}
void write_file(const char* path,const char* value) {
    FILE* out=fopen(path,"w");assert(out);assert(fputs(value,out)>=0);assert(!fclose(out));
}
}
namespace mx5 { namespace runtime {
bool loader_begin_patch() { ++begins;return cold; }
void loader_end_patch(bool safe) { assert(safe);++ends; }
} }
namespace mx5 { namespace adapter {
InstallResult install_lds_v74(const LdsInstallOptions& options) {
    ++installs;captured=options;
    assert(options.service_handle==&service && options.verified_cold_start);
    assert(options.verify_file_hash==mx5_verify_file_sha256 && options.clock && options.emit && options.user);
    if(!options.begin_patch())return COLD_START_LOST;
    options.end_patch(true);
    if(install_result==INSTALL_OK)emit_witness();
    errno=ERANGE;return install_result;
}
} }
int main(int argc,char** argv) {
    assert(argc==2);alarm(10);const char* name=argv[1];
    char root[]="/tmp/mx5dr-lds-tap-XXXXXX";assert(mkdtemp(root));
    assert(snprintf(tap_config,sizeof tap_config,"%s/config",root)>0);
    assert(snprintf(tap_disable,sizeof tap_disable,"%s/disabled",root)>0);
    assert(snprintf(tap_channel,sizeof tap_channel,"mx5dr.lds.tap.%ld",(long)getpid())>0);
    write_file(tap_config,"mode=SHADOW\n");
    bool expected_enabled=true;
    if(!strcmp(name,"off")) { write_file(tap_config,"mode=OFF\n");expected_enabled=false; }
    else if(!strcmp(name,"invalid")) { write_file(tap_config,"mode=ASSIST\n");expected_enabled=false; }
    else if(!strcmp(name,"missing_mode")) { write_file(tap_config,"sample_ms=500\n");expected_enabled=false; }
    else if(!strcmp(name,"missing_config")) { assert(!unlink(tap_config));expected_enabled=false; }
    else if(!strcmp(name,"disabled")) { write_file(tap_disable,"disabled\n");expected_enabled=false; }
    else if(!strcmp(name,"marker_symlink")) { assert(!symlink("nonexistent",tap_disable));expected_enabled=false; }
    else if(!strcmp(name,"marker_error")) {
        assert(snprintf(tap_disable,sizeof tap_disable,"%s/config/invalid",root)>0);expected_enabled=false;
    }
    else if(!strcmp(name,"observe"))write_file(tap_config,"mode=OBSERVE\n");
    else if(!strcmp(name,"scrub"))write_file(tap_config,"mode=SCRUB\n");
    else if(!strcmp(name,"install_failed"))install_result=TestAdapter::FILE_IDENTITY_MISMATCH;
    else if(!strcmp(name,"rollback_failed"))install_result=TestAdapter::NEXT_CHAIN_MISMATCH;
    else if(!strcmp(name,"cold_lost"))cold=false;
    else assert(!strcmp(name,"normal")||!strcmp(name,"late_receiver")||!strcmp(name,"sender_failed")||
                !strcmp(name,"unrequested")||!strcmp(name,"null_handle")||!strcmp(name,"repeated"));

    const bool unrequested=!strcmp(name,"unrequested");
    if(!unrequested) {
        errno=EDOM;const bool enabled=mx5::runtime::loader_enabled();
        assert(enabled==expected_enabled); // RED: unimplemented role cannot enable the actual cold installer.
        assert(errno==EDOM);
    }
    TestSideband::Receiver receiver;const bool late=!strcmp(name,"late_receiver");
    const bool sender_fail=!strcmp(name,"sender_failed");
    const bool null_handle=!strcmp(name,"null_handle");
    if(expected_enabled&&!unrequested&&!late&&!sender_fail)assert(receiver.open_channel(tap_channel,geteuid()));
    int output[2];assert(!pipe(output));
    const int saved_stderr=dup(STDERR_FILENO);assert(saved_stderr>=0);
    assert(dup2(output[1],STDERR_FILENO)==STDERR_FILENO);assert(!close(output[1]));
    rlimit before=rlimit();
    if(sender_fail) { assert(!getrlimit(RLIMIT_NOFILE,&before));rlimit limit=before;limit.rlim_cur=0;assert(!setrlimit(RLIMIT_NOFILE,&limit)); }
    errno=EDOM;mx5::runtime::loader_bootstrap(null_handle?0:&service);assert(errno==EDOM);
    if(sender_fail)assert(!setrlimit(RLIMIT_NOFILE,&before));
    const bool attempted=expected_enabled&&!unrequested&&!sender_fail&&!null_handle;
    assert(installs==unsigned(attempted));
    const TestTap::LdsTapReport& report=TestTap::lds_tap_report();
    assert(report.install_attempted==attempted);
    if(sender_fail)assert(report.state==TestTap::LdsTapReport::SENDER_FAILED);
    if(attempted) {
        assert(begins==1 && ends==unsigned(cold));
        assert(report.installation==(cold?install_result:TestAdapter::COLD_START_LOST));
        const bool installed=cold&&install_result==TestAdapter::INSTALL_OK;
        assert(report.state==(installed?TestTap::LdsTapReport::INSTALLED:TestTap::LdsTapReport::INSTALL_FAILED));
        if(installed) {
            if(late) { assert(receiver.open_channel(tap_channel,geteuid()));emit_witness(); }
            TestSideband::Record result;TestSideband::Diagnostic diagnostic;
            assert(receiver.receive(&result,&diagnostic)==TestSideband::RECORD);
            assert(result.position.utc_seconds==123 && result.field_lineage.lifetime==3);
            assert(result.source_instance>0 && result.sequence==(late?2u:1u));
            assert(result.dropped_before==unsigned(late) && result.observed_ns>0);
            assert(result.field_lineage.fields[0].write_sequence==4);
            assert(diagnostic.sender_pid==getpid() && diagnostic.sender_uid==geteuid());
        }
        if(!strcmp(name,"repeated")) {
            errno=E2BIG;mx5::runtime::loader_bootstrap(&service);assert(errno==E2BIG && installs==1);
        }
    }
    assert(dup2(saved_stderr,STDERR_FILENO)==STDERR_FILENO);assert(!close(saved_stderr));
    char actual[384]={0};const ssize_t bytes=read(output[0],actual,sizeof actual-1);
    assert(bytes>=0);assert(!close(output[0]));
    char expected[192]={0};
    if(expected_enabled&&!unrequested) {
        const char* state=null_handle?"invalid_handle":sender_fail?"sender_unavailable":
            (cold&&install_result==TestAdapter::INSTALL_OK)?"installed":"install_failed";
        const unsigned mode=!strcmp(name,"observe")?1u:!strcmp(name,"scrub")?2u:4u;
        const TestAdapter::InstallResult code=null_handle?TestAdapter::INVALID_INSTALL_ARGUMENT:sender_fail?
            TestAdapter::CONFIGURATION_FAILED:cold?install_result:TestAdapter::COLD_START_LOST;
        const int length=snprintf(expected,sizeof expected,
            "mx5dr lds: state=%s mode=%u install_attempted=%u install_code=%d\n",
            state,mode,unsigned(attempted),int(code));
        assert(length>0&&size_t(length)<sizeof expected);
    }
    if(strcmp(actual,expected))fprintf(stderr,"expected diagnostic: %sactual diagnostic: %s",expected,actual);
    assert(!strcmp(actual,expected));
    assert(report.diagnostic_bytes==bytes&&report.diagnostic_errno==0);
    unlink(tap_config);
    if(strcmp(name,"marker_error"))unlink(tap_disable);
    assert(!rmdir(root));printf("LDS tap %s PASS\n",name);
}
