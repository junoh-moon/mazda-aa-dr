// Independent authored test of MODEL receipt/event lower bounds.
// Receiver is synthetic: this isolates worker admission, not socket authentication.
#include "../../src/runtime/runtime.cpp"
#include <assert.h>
#include <vector>
#include <fstream>
#include <string>
struct EventReceiver {
    std::vector<N::RawEvent> rows;size_t at;
    EventReceiver():at(0) {}
    N::ReceiveResult receive(uint64_t,N::RawEvent* raw,N::ReceiveDiagnostic*) {
        if(at==rows.size())return N::CHANNEL_EMPTY;
        *raw=rows[at++];return N::CHANNEL_EVENT;
    }
};
static uint64_t T(unsigned ms) { return 1000000000ULL+uint64_t(ms)*1000000ULL; }
static N::RawEvent wheel(unsigned received_ms,int64_t source_ms,uint64_t sequence=1) {
    N::RawEvent r=N::RawEvent();r.kind=N::WHEELS;r.epoch=1;r.receive_seq=sequence;
    r.received_ns=T(received_ms);r.source_mono_ms=source_ms;r.count=1;
    for(unsigned i=0;i<4;++i)r.raw[i]=10000;
    return r;
}
static void check(Journal& journal,const char* label,EventReceiver receiver,uint64_t events,uint64_t resets) {
    N::Pipeline nav;N::GpsHoldout hold;const mx5_dr_context c={1,1,1};
    assert(nav.init_model(N::research_model_profile(),mx5_dr_default_config(),c,true,true));
    assert(hold.init_model(N::research_model_profile(),mx5_dr_default_config(),c));
    mx5::runtime::MotionBatch batch;
    drain_motion(journal,batch,receiver,nav,hold,true,T(1000));
    printf("%s: events=%llu resets=%llu rejected=%llu result=%s\n",label,
           (unsigned long long)nav.status().events,(unsigned long long)nav.status().resets,
           (unsigned long long)nav.status().rejected,N::pipeline_result_name(nav.status().result));fflush(stdout);
    assert(nav.status().events==events&&nav.status().resets==resets&&nav.status().rejected==resets);
    if(resets)assert(nav.status().result==N::PIPELINE_CLOCK_RESET);
    assert(!nav.diagnostic(T(1400)).snapshot.model_valid);
    assert(!nav.calibration().candidate_ready&&!nav.calibration().calibration_version);
}
int main() {
    char root[]="/tmp/mx5dr-event-fence-XXXXXX";assert(mkdtemp(root));
    char logs[200];snprintf(logs,sizeof logs,"%s/logs",root);assert(!mkdir(logs,0700));
    config.max_log_bytes=8388608;
    { Journal journal(root);
    struct Case { const char* label;unsigned receipt;int64_t source;unsigned events,resets; };
    const Case cases[]={
        {"old_receipt",900,int64_t(T(900)/1000000ULL),0,0},
        {"old_transport_new_receipt",1100,int64_t(T(950)/1000000ULL),0,0},
        {"exact_transport_boundary",1050,int64_t(T(1000)/1000000ULL),1,0},
        {"new_transport",1100,int64_t(T(1050)/1000000ULL),1,0},
        {"new_receipt_only",1100,0,1,0},
        {"negative_transport",1100,-1,0,1},
        {"overflow_transport",1100,INT64_MAX,0,1},
        {"future_transport",1100,int64_t(T(1200)/1000000ULL),0,1}
    };
    for(size_t i=0;i<sizeof cases/sizeof cases[0];++i) {
        EventReceiver receiver;receiver.rows.push_back(wheel(cases[i].receipt,cases[i].source));
        check(journal,cases[i].label,receiver,cases[i].events,cases[i].resets);
    }
    EventReceiver mixed;mixed.rows.push_back(wheel(1100,0));
    mixed.rows.push_back(wheel(1200,int64_t(T(1150)/1000000ULL),2));
    check(journal,"post_boundary_receipt_to_transport_clock_change",mixed,1,1);
    EventReceiver old_clock;
    old_clock.rows.push_back(wheel(1100,0));
    old_clock.rows.push_back(wheel(1200,int64_t(T(950)/1000000ULL),2));
    old_clock.rows.push_back(wheel(1300,0,3));
    check(journal,"excluded_old_transport_does_not_change_new_clock",old_clock,2,0);
    journal.flush();assert(!journal.failed);
    }
    std::ifstream input(std::string(logs)+"/trace.0.jsonl");
    // Independent, literal wire expectations include every field of all 13 inputs.
    // In particular, the three excluded inputs must survive exactly in input order.
    const char* raw_expected[]={
        "[1,1,1900000000,1900,10000,10000,10000,10000,1,0]",
        "[1,1,2100000000,1950,10000,10000,10000,10000,1,0]",
        "[1,1,2050000000,2000,10000,10000,10000,10000,1,0]",
        "[1,1,2100000000,2050,10000,10000,10000,10000,1,0]",
        "[1,1,2100000000,0,10000,10000,10000,10000,1,0]",
        "[1,1,2100000000,-1,10000,10000,10000,10000,1,0]",
        "[1,1,2100000000,9223372036854775807,10000,10000,10000,10000,1,0]",
        "[1,1,2100000000,2200,10000,10000,10000,10000,1,0]",
        "[1,1,2100000000,0,10000,10000,10000,10000,1,0]",
        "[1,2,2200000000,2150,10000,10000,10000,10000,1,0]",
        "[1,1,2100000000,0,10000,10000,10000,10000,1,0]",
        "[1,2,2200000000,1950,10000,10000,10000,10000,1,0]",
        "[1,3,2300000000,0,10000,10000,10000,10000,1,0]"
    };
    const size_t expected_count=sizeof raw_expected/sizeof raw_expected[0];
    std::string line;unsigned excluded=0,receipt=0,transport=0;size_t raw_rows=0;
    while(std::getline(input,line)) {
        if(line.find("\"kind\":\"motion_batch\"")!=std::string::npos) {
            assert(line.find("\"schema\":1,")!=std::string::npos);
            assert(line.find("\"epoch\":1,")!=std::string::npos);
            assert(line.find("\"producer_time_status\":\"unknown\"")!=std::string::npos);
            size_t at=line.find("\"events\":[");assert(at!=std::string::npos);at+=10;
            while(at<line.size()&&line[at]=='[') {
                const size_t end=line.find(']',at);assert(end!=std::string::npos);
                assert(raw_rows<expected_count);
                assert(line.substr(at,end-at+1)==raw_expected[raw_rows++]);
                at=end+1;if(at<line.size()&&line[at]==',')++at;
            }
            assert(line.substr(at)=="]}");
        }
        if(line.find("\"kind\":\"shadow_motion_excluded\"")==std::string::npos)continue;
        ++excluded;
        if(line.find("receipt_before_session")!=std::string::npos)++receipt;
        if(line.find("transport_before_session")!=std::string::npos)++transport;
    }
    assert(excluded==3 && receipt==1 && transport==2);
    assert(raw_rows==expected_count);
    input.close();assert(!unlink((std::string(logs)+"/trace.0.jsonl").c_str()));
    assert(!rmdir(logs) && !rmdir(root));
    puts("MODEL session input: old receipt/event exclusions recorded; malformed/current clock faults preserved");
    return 0;
}
