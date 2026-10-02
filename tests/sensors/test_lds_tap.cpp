// Authored installer witness; actual config parser, Sender/Receiver and tap.
// The separate loader fixture exercises the real dlopen interposer/lease.
#include <limits.h>
static char tap_config[PATH_MAX],tap_disable[PATH_MAX],tap_channel[80],association_channel[80],association_directory[PATH_MAX];
#define MX5_LDS_CONFIG_PATH tap_config
#define MX5_LDS_DISABLE_PATH tap_disable
#define MX5_LDS_CHANNEL tap_channel
#define MX5_LDS_ASSOCIATION_CHANNEL association_channel
#define MX5_LDS_ASSOCIATION_DIRECTORY association_directory
#include "runtime/lds_association_channel.h"
#include "runtime/lds_association_protocol.h"
#include "../../src/sensors/lds_tap.cpp"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
namespace TestAdapter=mx5::adapter;
namespace TestSideband=mx5::runtime::lds_sideband;
namespace TestTap=mx5::sensors;
namespace TestAssociation=mx5::runtime::lds_association;
namespace TestRequest=mx5::runtime::request_trace;
namespace mx5 { namespace runtime { namespace lds_association {
struct AssociationTestAccess {
    static const protocol::Map* map(const Registry& registry) {
        const unsigned token=registry.active_.load();
        return token?static_cast<const protocol::Map*>(registry.views_[(token&3)-1].mapping):0;
    }
};
} } }
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
TestAdapter::LdsLockedSend locked_example() {
    TestAdapter::LdsLockedSend r=TestAdapter::LdsLockedSend();
    r.stage=TestAssociation::LOCKED_FOR_SEND;r.reply_type=2;r.observed_ns=captured.clock(captured.user);
    TestSideband::copy_text(&r.wire.server_guid,"authored-transport-guid");
    TestSideband::copy_text(&r.wire.client_unique,":1.2");
    TestSideband::copy_text(&r.wire.server_unique,":1.1");r.wire.destination=r.wire.client_unique;
    r.wire.request_serial=r.wire.reply_serial=7;r.wire.response_serial=19;
    const TestSideband::Record source=example();r.field_lineage=source.field_lineage;r.position=source.position;
    return r;
}
bool read_locked(TestAssociation::Registry& registry,const TestAdapter::LdsLockedSend& r,TestAssociation::Owned* out) {
    TestRequest::Trace trace=TestRequest::Trace();trace.request=TestRequest::Token{11,2};trace.worker=TestRequest::Token{12,2};
    trace.issue.observed_ns=r.observed_ns-1;trace.issue.wire.observed_ns=trace.issue.observed_ns;
    trace.issue.wire.known=trace.issue.wire.endpoint_matched=true;trace.issue.wire.serial=r.wire.request_serial;
    trace.issue.endpoint.server_guid=r.wire.server_guid;trace.issue.endpoint.unique_name=r.wire.client_unique;
    TestSideband::copy_text(&trace.issue.route.destination,"com.jci.lds.data");
    TestSideband::copy_text(&trace.issue.route.path,"/com/jci/lds/data");
    TestSideband::copy_text(&trace.issue.route.interface_name,"com.jci.lds.data");
    TestSideband::copy_text(&trace.issue.route.member,"GetPosition");
    trace.reply.wire.known=true;trace.reply.wire.type=2;trace.reply.wire.serial=r.wire.response_serial;
    trace.reply.wire.reply_serial=r.wire.reply_serial;trace.reply.wire.sender=r.wire.server_unique;
    trace.reply.wire.observed_ns=r.observed_ns+1;
    const TestAdapter::PositionContext context={r.position,TestRequest::OK,trace,5,9,0};
    return registry.read(context,out);
}
void check_association(const char* name,TestAssociation::Registry& registry,TestSideband::Receiver& receiver,uint64_t instance) {
    const bool failed=!strcmp(name,"association_failed");
    registry.drain(captured.clock(captured.user));
    const TestAdapter::LdsLockedSend r=locked_example();TestAssociation::Owned out;
    // Before implementation the actual tap supplies no association callback
    // and the real Registry cannot adopt/read anything: a behavioral RED.
    if(captured.publish_locked)captured.publish_locked(r,captured.user);
    errno=E2BIG;const bool matched=read_locked(registry,r,&out);assert(errno==E2BIG);
    assert(matched==!failed);
    assert(captured.publish_locked && captured.invalidate_locked);
    if(failed) {
        assert(out.result==TestAssociation::UNAVAILABLE);
        emit_witness();TestSideband::Record side;TestSideband::Diagnostic d;
        assert(receiver.receive(&side,&d)==TestSideband::RECORD && side.source_instance==instance && side.sequence==2);
        return;
    }
    assert(out.result==TestAssociation::MATCHED_LOCKED_FOR_SEND && out.stage==TestAssociation::LOCKED_FOR_SEND);
    assert(out.source_instance==instance && out.record_sequence==1 && out.cache_lifetime==3 && out.write_sequence==4);
    assert(out.fields[0].write_sequence==4 && out.fields[1].write_sequence==0);
    auto wrong=r;wrong.position.latitude_deg+=1;assert(!read_locked(registry,wrong,&out) && out.result==TestAssociation::PAYLOAD_MISMATCH);
    assert(read_locked(registry,r,&out));
    if(!strcmp(name,"association_invalidate")) {
        errno=E2BIG;captured.invalidate_locked(TestAdapter::LOCKED_CHAIN_CONFLICT,captured.user);assert(errno==E2BIG);
        assert(!read_locked(registry,r,&out));return;
    }
    if(!strcmp(name,"association_fork")) {
        const auto* map=TestAssociation::AssociationTestAccess::map(registry);assert(map);
        uint32_t before[TestAssociation::protocol::WORDS];
        for(unsigned i=0;i<TestAssociation::protocol::WORDS;++i)before[i]=map->words[i].load();
        const pid_t child=fork();assert(child>=0);
        if(!child) {
            auto other=r;other.wire.request_serial=other.wire.reply_serial=8;other.wire.response_serial=20;++other.observed_ns;
            errno=E2BIG;captured.publish_locked(other,captured.user);
            captured.invalidate_locked(TestAdapter::LOCKED_SEND_FAILED,captured.user);captured.emit(example(),captured.user);
            _exit(errno==E2BIG?0:9);
        }
        int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
        for(unsigned i=0;i<TestAssociation::protocol::WORDS;++i)assert(map->words[i].load()==before[i]);
        assert(read_locked(registry,r,&out));
        TestSideband::Record side;TestSideband::Diagnostic d;
        assert(receiver.receive(&side,&d)==TestSideband::EMPTY);
        emit_witness();assert(receiver.receive(&side,&d)==TestSideband::RECORD && side.source_instance==instance && side.sequence==2);
    }
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
    assert(snprintf(association_channel,sizeof association_channel,"mx5dr.lds.assoc.tap.%ld",(long)getpid())>0);
    assert(snprintf(association_directory,sizeof association_directory,"%s%s",root,
        !strcmp(name,"association_failed")?"/missing":"")>0);
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
                !strcmp(name,"unrequested")||!strcmp(name,"null_handle")||!strcmp(name,"repeated")||
                !strcmp(name,"association")||!strcmp(name,"association_fork")||
                !strcmp(name,"association_failed")||!strcmp(name,"association_invalidate"));

    const bool unrequested=!strcmp(name,"unrequested");
    if(!unrequested) {
        errno=EDOM;const bool enabled=mx5::runtime::loader_enabled();
        assert(enabled==expected_enabled); // RED: unimplemented role cannot enable the actual cold installer.
        assert(errno==EDOM);
    }
    TestSideband::Receiver receiver;const bool late=!strcmp(name,"late_receiver");
    TestAssociation::Registry registry;const bool association=!strncmp(name,"association",11);
    if(association)assert(registry.open_channel(association_channel,geteuid()));
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
            if(association)check_association(name,registry,receiver,result.source_instance);
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
