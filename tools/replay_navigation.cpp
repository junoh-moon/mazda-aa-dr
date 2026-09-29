// PC-only deterministic receipt-order replay of the production MODEL classes.
// This is not a reproduction of unrecorded runtime worker wakeups/watermarks.
#include "navigation/holdout.h"
#include "runtime/shadow_log.h"
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace N=mx5::navigation;
namespace A=mx5::adapter;
namespace {
const uint64_t MAX_RECORDS=2000000, REORDER_NS=100000000;
const size_t MAX_LINE=1024;
typedef unsigned long long U;
bool ok(N::PipelineResult r) {
    return r==N::PIPELINE_OK||r==N::PIPELINE_WAITING||r==N::PIPELINE_NO_ANCHOR;
}
bool unsigned_number(const char* s,uint64_t maximum,uint64_t& value) {
    value=0;if(!*s)return false;
    for(;*s;++s) {
        if(*s<'0'||*s>'9')return false;
        const unsigned digit=unsigned(*s-'0');
        if(value>maximum/10||(value==maximum/10&&digit>maximum%10))return false;
        value=value*10+digit;
    }
    return true;
}
bool signed_number(const char* s,int64_t minimum,int64_t maximum,int64_t& value) {
    const bool negative=*s=='-';if(negative)++s;
    uint64_t magnitude=0;
    const uint64_t limit=negative?uint64_t(-(minimum+1))+1:uint64_t(maximum);
    if(!unsigned_number(s,limit,magnitude))return false;
    value=negative?(magnitude? -int64_t(magnitude-1)-1:0):int64_t(magnitude);
    return true;
}
bool gps_number(const char* s,double& value) {
    if(std::strcmp(s,"null")==0) { value=std::numeric_limits<double>::quiet_NaN();return true; }
    // Decimal ASCII only: strtod alone would also admit hex, NaN and infinity.
    const char* p=s;if(*p=='-'||*p=='+')++p;
    unsigned digits=0;while(*p>='0'&&*p<='9') { ++digits;++p; }
    if(*p=='.') { ++p;while(*p>='0'&&*p<='9') { ++digits;++p; } }
    if(!digits)return false;
    if(*p=='e'||*p=='E') {
        ++p;if(*p=='-'||*p=='+')++p;
        unsigned exponent_digits=0;while(*p>='0'&&*p<='9') { ++exponent_digits;++p; }
        if(!exponent_digits)return false;
    }
    if(*p)return false;
    errno=0;char* end=0;value=std::strtod(s,&end);
    return !errno&&end&&!*end&&std::isfinite(value);
}
// 1=line, 0=EOF, -1=invalid/oversized/read failure; never store unbounded input.
int read_line(char (&line)[MAX_LINE+1]) {
    size_t n=0;int c;
    while((c=std::getchar())!=EOF) {
        if(c=='\n') { line[n]=0;return 1; }
        if(n==MAX_LINE||c==0||c>126||(c<32&&c!='\t'&&c!='\r'))return -1;
        line[n++]=char(c);
    }
    if(std::ferror(stdin))return -1;
    line[n]=0;return n?1:0;
}
struct Input {
    bool motion;
    N::RawEvent raw;
    A::Observation position;
    uint64_t received;
    Input():motion(false),raw(),position(),received(0) {}
};
bool parse(char* line,Input& input) {
    char* fields[13];size_t count=0;
    for(char* p=line;*p;) {
        while(*p==' '||*p=='\t'||*p=='\r')++p;
        if(!*p)break;
        if(count==13)return false;
        fields[count++]=p;
        while(*p&&*p!=' '&&*p!='\t'&&*p!='\r')++p;
        if(*p)*p++=0;
    }
    uint64_t u=0;int64_t s=0;
    if(count==12&&std::strcmp(fields[0],"M")==0) {
        input.motion=true;
        if(!unsigned_number(fields[1],3,u)||u<1)return false;
        input.raw.kind=N::SensorKind(u);
        if(!unsigned_number(fields[2],UINT64_MAX,input.raw.epoch)||
           !unsigned_number(fields[3],UINT64_MAX,input.raw.receive_seq)||
           !unsigned_number(fields[4],UINT64_MAX,input.raw.received_ns)||
           !signed_number(fields[5],INT64_MIN,INT64_MAX,input.raw.source_mono_ms))return false;
        for(unsigned j=0;j<4;++j) {
            if(!unsigned_number(fields[6+j],UINT16_MAX,u))return false;
            input.raw.raw[j]=uint16_t(u);
        }
        if(!unsigned_number(fields[10],UINT16_MAX,u))return false;
        input.raw.count=uint16_t(u);
        if(!signed_number(fields[11],INT32_MIN,INT32_MAX,s))return false;
        input.raw.reverse=int(s);input.received=input.raw.received_ns;
    } else if(count==8&&std::strcmp(fields[0],"P")==0) {
        input.position.kind=A::Observation::POSITION;
        if(!unsigned_number(fields[1],UINT64_MAX,input.position.mono_ns)||
           !signed_number(fields[2],INT32_MIN,INT32_MAX,s))return false;
        input.position.position.mode=int32_t(s);
        if(!unsigned_number(fields[3],UINT64_MAX,input.position.position.utc_seconds)||
           !gps_number(fields[4],input.position.position.latitude_deg)||
           !gps_number(fields[5],input.position.position.longitude_deg)||
           !gps_number(fields[6],input.position.position.heading_deg)||
           !gps_number(fields[7],input.position.position.velocity_kmh))return false;
        input.received=input.position.mono_ns;
    } else return false;
    return input.received!=0;
}
struct Variant {
    const char* name;
    N::Pipeline main;
    N::GpsHoldout holdout;
    uint64_t enqueue_faults,drain_faults,holdout_faults,holdout_aborts;
    uint64_t states[7],model_valid_groups;
    Variant():name(0),enqueue_faults(0),drain_faults(0),holdout_faults(0),holdout_aborts(0),
              model_valid_groups(0) { std::memset(states,0,sizeof states); }
    bool init(const char* n,bool learn) {
        name=n;mx5_dr_context x={1,1,1};
        const N::ModelProfile p=N::research_model_profile();
        const mx5_dr_config c=mx5_dr_default_config();
        return main.init_model(p,c,x,learn,true,learn)&&
            holdout.init_model(p,c,x,N::default_holdout_config(),learn);
    }
    void results() {
        N::HoldoutResult r;
        while(holdout.pop(&r)) {
            if(r.event==N::HOLDOUT_ABORT) {
                ++holdout_aborts;
                if(r.reason!=N::HOLDOUT_REAL_GAP&&r.reason!=N::HOLDOUT_NATIVE)++holdout_faults;
            }
            char error[48]="null",heading[48]="null",zero[48],scale[48];
            const bool compared=r.event==N::HOLDOUT_COMPARED;
            if(compared) {
                mx5::runtime::shadow_number(r.position_error_m,error);
                if(r.has_heading_error)mx5::runtime::shadow_number(r.heading_error_rad,heading);
            }
            mx5::runtime::shadow_number(r.applied_yaw_zero,zero);
            mx5::runtime::shadow_number(r.applied_wheel_scale,scale);
            std::printf("{\"kind\":\"holdout\",\"variant\":\"%s\",\"domain\":\"model\","
                "\"assist_ready\":false,\"time_basis\":\"receipt_model\",\"event\":\"%s\",\"reason\":\"%s\","
                "\"window_id\":%llu,\"anchor_ns\":%llu,\"reference_ns\":%llu,\"frontier_ns\":%llu,"
                "\"model_valid\":%s,\"position_error_m\":%s,\"heading_error_rad\":%s,"
                "\"yaw_zero\":%s,\"calibration_version\":%llu,\"wheel_scale\":%s,\"wheel_scale_version\":%llu}\n",
                name,N::holdout_event_name(r.event),N::holdout_reason_name(r.reason),
                U(r.window_id),U(r.anchor_ns),U(r.reference_ns),U(r.prediction_frontier_ns),
                compared&&r.prediction.model_valid?"true":"false",error,heading,zero,
                U(r.calibration_version),scale,U(r.wheel_scale_version));
        }
    }
    void enqueue(const Input& i) {
        const N::PipelineResult a=i.motion?main.enqueue_raw(i.raw):main.enqueue_position(i.position);
        const N::PipelineResult b=i.motion?holdout.enqueue_raw(i.raw):holdout.enqueue_position(i.position);
        if(!ok(a))++enqueue_faults;
        if(!ok(b))++holdout_faults;
        results();
    }
    void drain(uint64_t watermark,uint64_t received) {
        if(!ok(main.drain(watermark)))++drain_faults;
        holdout.drain(watermark);results();
        const N::Diagnostic d=main.diagnostic(received);
        if(unsigned(d.snapshot.state)<7)++states[d.snapshot.state];
        if(d.snapshot.model_valid)++model_valid_groups;
    }
    uint64_t faults() const { return enqueue_faults+drain_faults+holdout_faults; }
    void summary(uint64_t now) const {
        const N::Diagnostic d=main.diagnostic(now);const N::Status& s=main.status();
        char zero[48],scale[48],distance[48];
        mx5::runtime::shadow_number(main.calibration().active_zero,zero);
        mx5::runtime::shadow_number(main.wheel_calibration().active_scale,scale);
        mx5::runtime::shadow_number(d.snapshot.distance_m,distance);
        std::printf("{\"kind\":\"summary\",\"variant\":\"%s\",\"domain\":\"model\",\"assist_ready\":false,"
            "\"state\":%d,\"result\":%d,\"pipeline_result\":\"%s\",\"valid\":%s,\"model_valid\":%s,\"frontier_ns\":%llu,\"distance_m\":%s,"
            "\"events\":%llu,\"intervals\":%llu,\"resets\":%llu,\"rejected\":%llu,"
            "\"enqueue_faults\":%llu,\"drain_faults\":%llu,\"holdout_faults\":%llu,\"holdout_aborts\":%llu,"
            "\"holdout_phase\":%d,\"yaw_zero\":%s,\"calibration_version\":%llu,"
            "\"wheel_scale\":%s,\"wheel_scale_version\":%llu,\"gps_anchor_gate\":\"%s\","
            "\"model_valid_groups\":%llu,\"state_counts\":[%llu,%llu,%llu,%llu,%llu,%llu,%llu]}\n",
            name,int(d.snapshot.state),int(d.result),N::pipeline_result_name(s.result),
            d.snapshot.valid?"true":"false",d.snapshot.model_valid?"true":"false",
            U(d.snapshot.frontier_ns),distance,U(s.events),U(s.intervals),U(s.resets),U(s.rejected),
            U(enqueue_faults),U(drain_faults),U(holdout_faults),U(holdout_aborts),int(holdout.phase()),zero,
            U(main.calibration().calibration_version),scale,U(main.wheel_calibration().calibration_version),
            N::anchor_gate_name(main.anchor_gate()),U(model_valid_groups),U(states[0]),U(states[1]),U(states[2]),
            U(states[3]),U(states[4]),U(states[5]),U(states[6]));
    }
};
int fail() { std::fputs("navigation replay: invalid protocol or I/O failure\n",stderr);return 2; }
}
int main(int argc,char**) {
    if(argc!=1)return fail();
    Variant variants[2];if(!variants[0].init("fixed",false)||!variants[1].init("adaptive",true))return fail();
    uint64_t records=0,groups=0,last=0,watermark=0;char line[MAX_LINE+1];int read=0;
    while((read=read_line(line))==1) {
        Input i;if(++records>MAX_RECORDS||!parse(line,i)||i.received<last)return fail();
        if(last&&i.received!=last) {
            watermark=last>REORDER_NS?last-REORDER_NS:0;
            for(unsigned v=0;v<2;++v)variants[v].drain(watermark,last);
        }
        if(i.received!=last)++groups;
        last=i.received;
        for(unsigned v=0;v<2;++v)variants[v].enqueue(i);
        if(std::ferror(stdout))return fail();
    }
    if(read<0)return fail();
    if(records) {
        watermark=last>REORDER_NS?last-REORDER_NS:0;
        for(unsigned v=0;v<2;++v)variants[v].drain(watermark,last);
    }
    for(unsigned v=0;v<2;++v)variants[v].summary(last);
    const uint64_t faults=variants[0].faults()+variants[1].faults();
    std::printf("{\"kind\":\"final\",\"domain\":\"model\",\"assist_ready\":false,\"records\":%llu,"
        "\"groups\":%llu,\"last_received_ns\":%llu,\"watermark_ns\":%llu,\"faults\":%llu,\"inconclusive\":%s}\n",
        U(records),U(groups),U(last),U(watermark),U(faults),faults||!records?"true":"false");
    return std::fflush(stdout)||std::ferror(stdout)?fail():0;
}
