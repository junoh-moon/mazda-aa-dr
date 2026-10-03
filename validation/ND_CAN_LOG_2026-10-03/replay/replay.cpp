// Offline host replay of a public MX-5 ND raw CAN log through the production
// MODEL-domain navigation Pipeline. Not vehicle validation; all GPS anchors are
// SYNTHETIC (fixed lat/lon, heading 0) because the log has no GPS.
//
// Input: windows.csv from prep.py (one VIP-like 100 ms window per row: rounded
// mean of 10 consecutive 0x215 wheel raw u16, and u16 sum + count of 5
// consecutive 0x078 yaw12 samples).
// Usage: replay windows.csv [--auto-bias 0|1] [--zero Z] [--latency-ms L]
//                           [--relaxed 0|1] [--gps-wheel 0|1] [--zero-seg K]
// --relaxed sets anchor/heading/speed/yaw error terms to 0 so that only the
// 60 s / 1500 m limits stop prediction (positions are unchanged; budget only).
// Output (stdout): one CSV row per window with pipeline/GyroBias/snapshot state.
#include "navigation/pipeline.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
using namespace mx5;
using namespace mx5::navigation;

struct Win { unsigned k,w[4],yaw_sum,yaw_count; double spd_mps; bool stopped; };

static const uint64_t T0=1000000000ULL;          // synthetic clock origin
static uint64_t wend(unsigned k){ return T0+uint64_t(k+1)*100000000ULL; }

int main(int argc,char** argv) {
    if(argc<2){std::fprintf(stderr,"usage\n");return 2;}
    bool auto_bias=true,relaxed=false,gps_wheel=false; double zero=2047; unsigned lat_ms=30;
    for(int i=2;i+1<argc;i+=2) {
        if(!std::strcmp(argv[i],"--auto-bias"))auto_bias=std::atoi(argv[i+1])!=0;
        else if(!std::strcmp(argv[i],"--zero"))zero=std::atof(argv[i+1]);
        else if(!std::strcmp(argv[i],"--latency-ms"))lat_ms=unsigned(std::atoi(argv[i+1]));
        else if(!std::strcmp(argv[i],"--relaxed"))relaxed=std::atoi(argv[i+1])!=0;
        else if(!std::strcmp(argv[i],"--gps-wheel"))gps_wheel=std::atoi(argv[i+1])!=0;
        else {std::fprintf(stderr,"bad arg %s\n",argv[i]);return 2;}
    }
    std::vector<Win> v;
    FILE* f=std::fopen(argv[1],"r"); if(!f){std::perror("open");return 2;}
    char line[512]; if(!std::fgets(line,sizeof line,f))return 2;
    while(std::fgets(line,sizeof line,f)) {
        Win x; if(std::sscanf(line,"%u,%u,%u,%u,%u,%u,%u",&x.k,&x.w[0],&x.w[1],&x.w[2],&x.w[3],
            &x.yaw_sum,&x.yaw_count)!=7)continue;
        double s=0; x.stopped=true;
        for(int j=0;j<4;++j){double m=(x.w[j]*0.01-100)/3.6; s+=m/4; if(m>0.05)x.stopped=false;}
        x.spd_mps=s; v.push_back(x);
    }
    std::fclose(f);
    // Synthetic anchor policy: first window with mean wheel speed > 5 m/s
    // after a window in which all four wheels were <= 0.05 m/s. Fixes at
    // windows a and a+1 (same lat/lon, heading 0), GAP (mode 0) at a+2.
    std::vector<int> anchor_at(v.size(),0);
    bool armed=true;
    for(size_t k=0;k<v.size();++k) {
        if(v[k].stopped)armed=true;
        else if(armed&&v[k].spd_mps>5.0&&k+2<v.size()) {
            anchor_at[k]=1;anchor_at[k+1]=2;anchor_at[k+2]=3;armed=false;
        }
    }
    ModelProfile p=research_model_profile(); p.yaw_zero=zero;
    mx5_dr_config c=mx5_dr_default_config();
    if(relaxed){p.anchor_error_m=0;p.heading_error_rad=0;c.speed_error_mps=0;c.yaw_error_rad_s=0;}
    Pipeline pl; mx5_dr_context x={1,1,1};
    if(!pl.init_model(p,c,x,auto_bias,gps_wheel)){std::fprintf(stderr,"init_model failed\n");return 1;}
    std::printf("k,stopped,spd,anchor,r_wheel,r_rev,r_yaw,r_pos,r_drain,gb_state,gb_ready,gb_cand,gb_n,gb_var,"
                "gb_active,gb_ver,gb_ev_start,gb_ev_end,d_result,state,east,north,heading,elapsed,dist,"
                "budget,hbudget,snap_stopped,events,intervals,resets,rejected\n");
    uint64_t seq=0;
    for(size_t k=0;k<v.size();++k) {
        const uint64_t rx=wend(unsigned(k))+uint64_t(lat_ms)*1000000ULL;
        RawEvent r=RawEvent(); r.epoch=1; r.received_ns=rx; r.source_mono_ms=0;
        r.kind=WHEELS; r.receive_seq=++seq; for(int j=0;j<4;++j)r.raw[j]=uint16_t(v[k].w[j]);
        PipelineResult rw=pl.enqueue_raw(r);
        r=RawEvent(); r.epoch=1; r.received_ns=rx; r.kind=REVERSE; r.receive_seq=++seq;
        r.reverse=p.reverse_forward_value;  // ASSUMED: log has no reverse; forward only
        PipelineResult rr=pl.enqueue_raw(r);
        r=RawEvent(); r.epoch=1; r.received_ns=rx; r.kind=YAW; r.receive_seq=++seq;
        r.raw[0]=uint16_t(v[k].yaw_sum); r.count=uint16_t(v[k].yaw_count);
        PipelineResult ry=pl.enqueue_raw(r);
        PipelineResult rp=PIPELINE_OK;
        if(anchor_at[k]) {
            adapter::Observation o=adapter::Observation(); o.kind=adapter::Observation::POSITION;
            o.mono_ns=rx; o.position.mode=anchor_at[k]==3?0:1;
            o.position.utc_seconds=1700000000ULL+k/10;
            o.position.latitude_deg=35; o.position.longitude_deg=135; o.position.heading_deg=0;
            o.position.velocity_kmh=v[k].spd_mps*3.6; o.position.horizontal=5;
            rp=pl.enqueue_position(o);
        }
        PipelineResult rd=pl.drain(rx-pl.reorder_ns());
        const GyroBiasStatus& g=pl.calibration();
        Diagnostic d=pl.diagnostic(rx);
        const mx5_dr_snapshot& s=d.snapshot;
        std::printf("%u,%d,%.4f,%d,%d,%d,%d,%d,%d,%d,%d,%.4f,%llu,%.4f,%.4f,%llu,%.3f,%.3f,%d,%d,"
                    "%.4f,%.4f,%.6f,%.3f,%.3f,%.4f,%.6f,%d,%llu,%llu,%llu,%llu\n",
            v[k].k,int(v[k].stopped),v[k].spd_mps,anchor_at[k],int(rw),int(rr),int(ry),int(rp),int(rd),
            int(g.state),int(g.candidate_ready),g.candidate_zero,(unsigned long long)g.samples,
            g.variance_counts2,g.active_zero,(unsigned long long)g.calibration_version,
            g.evidence_start_ns?double(g.evidence_start_ns-T0)/1e9:-1.0,
            g.evidence_end_ns?double(g.evidence_end_ns-T0)/1e9:-1.0,
            int(d.result),int(s.state),s.accumulated_east_m,s.accumulated_north_m,s.body_heading_rad,
            s.elapsed_s,s.distance_m,s.error_budget_m,s.heading_budget_rad,s.stopped,
            (unsigned long long)d.status.events,(unsigned long long)d.status.intervals,
            (unsigned long long)d.status.resets,(unsigned long long)d.status.rejected);
    }
    return 0;
}
