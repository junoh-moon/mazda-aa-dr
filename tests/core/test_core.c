#include "dr_core.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define PI 3.14159265358979323846
#define T0 UINT64_C(1000000000)
#define DT UINT64_C(100000000)
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while(0)
static void near(double a,double b,double eps) { CHECK(fabs(a-b)<=eps); }
static mx5_dr_context ctx(unsigned gen) { mx5_dr_context x={1,1,0}; x.generation=gen; return x; }
static mx5_dr_anchor anchor(unsigned gen) {
    mx5_dr_anchor a; memset(&a,0,sizeof(a)); a.context=ctx(gen);
    a.anchor_id=1; a.position_seq=1; a.measured_ns=T0; a.utc_ns=UINT64_C(1700000000000000000);
    a.validated=1; a.heading_valid=1; a.calibration_verified=1; a.quality=MX5_DR_VALID;
    return a;
}
static mx5_dr_evidence evidence(uint64_t id,uint64_t seq,uint64_t t) {
    mx5_dr_evidence e; memset(&e,0,sizeof(e)); e.source_id=id; e.source_epoch=1;
    e.producer_seq=seq; e.measured_ns=t; e.received_ns=t; e.lease_until_ns=t+250000000;
    e.quality=MX5_DR_VALID; e.freshness=MX5_DR_PRODUCER_TIME; return e;
}
static mx5_dr_interval interval(mx5_dr_core *c,double v,double w,int reverse) {
    mx5_dr_interval i; memset(&i,0,sizeof(i)); i.context=c->estimate.context;
    i.interval_seq=c->have_interval ? c->last_interval.interval_seq+1 : 1;
    i.start_ns=c->estimate.frontier_ns; i.end_ns=i.start_ns+DT; i.received_ns=i.end_ns;
    i.speed=evidence(1,i.interval_seq,i.start_ns); i.yaw=evidence(2,i.interval_seq,i.start_ns);
    i.reverse=evidence(3,i.interval_seq,i.start_ns); i.speed_mps=v; i.yaw_rad_s=w;
    i.reverse_active=reverse; i.raw_yaw=2047; i.yaw_count=1; return i;
}
static void setup(mx5_dr_core *c,double heading) {
    mx5_dr_config p=mx5_dr_default_config(); mx5_dr_anchor a=anchor(1); a.body_heading_rad=heading;
    CHECK(mx5_dr_init(c,&p,ctx(1))==MX5_DR_OK); CHECK(mx5_dr_seed(c,&a)==MX5_DR_OK);
    CHECK(mx5_dr_control(c,MX5_DR_GAP,ctx(2),2)==MX5_DR_OK);
}
static void run(mx5_dr_core *c,unsigned n,double v,double w,int reverse) {
    unsigned j; for(j=0;j<n;++j) { mx5_dr_interval i=interval(c,v,w,reverse); CHECK(mx5_dr_step(c,&i)==MX5_DR_OK); }
}
static void geometry(void) {
    mx5_dr_core c; mx5_dr_snapshot s;
    setup(&c,PI/2); run(&c,50,20,0,0);
    near(c.estimate.accumulated_east_m,100,1e-9); near(c.estimate.accumulated_north_m,0,1e-9);
    /* Independent equatorial WGS84 longitude conversion. */
    near(c.estimate.longitude_deg,100.0/6378137.0*180/PI,1e-10);
    CHECK(mx5_dr_get_snapshot(&c,c.estimate.frontier_ns,ctx(2),&s)==MX5_DR_OK);
    CHECK(s.derived_utc_ns==c.anchor.utc_ns+UINT64_C(5000000000));
    setup(&c,0); run(&c,100,20,PI/20,0);
    near(c.estimate.accumulated_east_m,400/PI,1e-8); near(c.estimate.accumulated_north_m,400/PI,1e-8);
    near(c.estimate.body_heading_rad,PI/2,1e-12);
    setup(&c,0); run(&c,100,20,-PI/20,0);
    near(c.estimate.accumulated_east_m,-400/PI,1e-8); near(c.estimate.accumulated_north_m,400/PI,1e-8);
    setup(&c,0); run(&c,30,2,0,1);
    near(c.estimate.accumulated_north_m,-6,1e-9); near(c.estimate.travel_bearing_rad,PI,1e-12);
    near(c.estimate.body_heading_rad,0,1e-12); near(c.estimate.speed_mps,2,1e-12);
    setup(&c,0); run(&c,100,2,PI/20,1);
    near(c.estimate.accumulated_east_m,-40/PI,1e-8); near(c.estimate.accumulated_north_m,-40/PI,1e-8);
    near(c.estimate.body_heading_rad,PI/2,1e-12);
    /* Small nonzero yaw follows a continuous arc, not a zero-yaw branch. */
    setup(&c,0); run(&c,10,20,1e-9,0);
    near(c.estimate.accumulated_east_m,1e-8,1e-12); near(c.estimate.accumulated_north_m,20,1e-9);
}
static void invalid_inputs(void) {
    mx5_dr_core c; mx5_dr_interval i; mx5_dr_anchor a; mx5_dr_config p=mx5_dr_default_config();
    unsigned j;
    for(j=0;j<3;++j) {
        setup(&c,0); i=interval(&c,2,0,0);
        if(j<2) i.raw_yaw=(uint16_t)(4094+j); else i.yaw_count=0;
        CHECK(mx5_dr_step(&c,&i)==MX5_DR_E_QUALITY); CHECK(c.estimate.state==MX5_DR_INVALID);
    }
    setup(&c,0); i=interval(&c,2,0,0); i.speed.quality=MX5_DR_UNKNOWN;
    CHECK(mx5_dr_step(&c,&i)==MX5_DR_E_QUALITY);
    setup(&c,0); i=interval(&c,2,0,0); i.yaw.freshness=MX5_DR_UNPROVEN_POLL;
    CHECK(mx5_dr_step(&c,&i)==MX5_DR_E_QUALITY);
    setup(&c,0); i=interval(&c,2,0,2); CHECK(mx5_dr_step(&c,&i)==MX5_DR_E_FRAME);
    setup(&c,0); i=interval(&c,NAN,0,0); CHECK(mx5_dr_step(&c,&i)==MX5_DR_E_NUMERIC);
    setup(&c,0); i=interval(&c,-2,0,0); CHECK(mx5_dr_step(&c,&i)==MX5_DR_E_NUMERIC);
    setup(&c,0); i=interval(&c,2,INFINITY,0); CHECK(mx5_dr_step(&c,&i)==MX5_DR_E_NUMERIC);
    CHECK(mx5_dr_init(&c,&p,ctx(1))==MX5_DR_OK); a=anchor(1); a.calibration_verified=0;
    CHECK(mx5_dr_seed(&c,&a)==MX5_DR_E_QUALITY);
    CHECK(mx5_dr_init(&c,&p,ctx(1))==MX5_DR_OK); a=anchor(1); a.latitude_deg=85;
    CHECK(mx5_dr_seed(&c,&a)==MX5_DR_E_NUMERIC);
    p.integration_step_s=NAN; CHECK(mx5_dr_init(&c,&p,ctx(1))==MX5_DR_E_CONFIG);
}
static void timing_and_identity(void) {
    mx5_dr_core c; mx5_dr_interval i,old; mx5_dr_snapshot s,before;
    setup(&c,0); i=interval(&c,20,0,0); CHECK(mx5_dr_step(&c,&i)==MX5_DR_OK);
    before=c.estimate; CHECK(mx5_dr_step(&c,&i)==MX5_DR_DUPLICATE);
    CHECK(c.estimate.frontier_ns==before.frontier_ns); near(c.estimate.distance_m,before.distance_m,0);
    CHECK(mx5_dr_get_snapshot(&c,i.end_ns+150000000,ctx(2),&s)==MX5_DR_OK);
    CHECK(s.frontier_ns==before.frontier_ns && s.derived_utc_ns==before.derived_utc_ns);
    CHECK(s.error_budget_m>before.error_budget_m); near(c.estimate.error_budget_m,before.error_budget_m,0);
    CHECK(mx5_dr_get_snapshot(&c,i.end_ns+150000001,ctx(2),&s)==MX5_DR_E_STALE && !s.valid);
    CHECK(mx5_dr_get_snapshot(&c,i.end_ns-1,ctx(2),&s)==MX5_DR_E_TIME);
    old=i; old.context=ctx(1); CHECK(mx5_dr_step(&c,&old)==MX5_DR_E_CONTEXT);
    CHECK(c.estimate.valid); CHECK(mx5_dr_get_snapshot(&c,i.end_ns,ctx(1),&s)==MX5_DR_E_CONTEXT);
    i.speed_mps=21; CHECK(mx5_dr_step(&c,&i)==MX5_DR_E_SEQUENCE);
    setup(&c,0); i=interval(&c,2,0,0); i.start_ns++;
    CHECK(mx5_dr_step(&c,&i)==MX5_DR_E_TIME);
    setup(&c,0); i=interval(&c,2,0,0); i.end_ns=i.start_ns;
    CHECK(mx5_dr_step(&c,&i)==MX5_DR_E_TIME);
    setup(&c,0); i=interval(&c,2,0,0); i.end_ns+=250000000; i.received_ns=i.end_ns;
    CHECK(mx5_dr_step(&c,&i)==MX5_DR_E_TIME);
    setup(&c,0); i=interval(&c,2,0,0); i.yaw.lease_until_ns=i.end_ns-1;
    CHECK(mx5_dr_step(&c,&i)==MX5_DR_E_TIME);
    setup(&c,0); run(&c,1,2,0,0); i=interval(&c,2,0,0); i.yaw.source_epoch++;
    CHECK(mx5_dr_step(&c,&i)==MX5_DR_E_CONTEXT && c.estimate.state==MX5_DR_INVALID);
    setup(&c,0); run(&c,1,2,0,0); i=interval(&c,2,0,0); i.yaw.producer_seq=1;
    CHECK(mx5_dr_step(&c,&i)==MX5_DR_E_SEQUENCE);
}
static void mean_intervals(void) {
    mx5_dr_core c; mx5_dr_interval i; mx5_dr_snapshot s;
    setup(&c,0); i=interval(&c,10,0.2,0); i.yaw_is_mean=1;
    i.yaw_window_start_ns=i.start_ns; i.yaw_window_end_ns=i.start_ns+2*DT;
    i.yaw.measured_ns=i.yaw_window_end_ns; i.yaw.received_ns=i.yaw_window_end_ns;
    i.yaw.lease_until_ns=i.yaw_window_end_ns+250000000; i.received_ns=i.yaw_window_end_ns;
    CHECK(mx5_dr_step(&c,&i)==MX5_DR_OK);
    CHECK(mx5_dr_get_snapshot(&c,i.end_ns,ctx(2),&s)==MX5_DR_E_TIME); /* cannot send before arrival */
    CHECK(mx5_dr_get_snapshot(&c,i.received_ns,ctx(2),&s)==MX5_DR_OK);
    i.interval_seq++; i.start_ns=i.end_ns; i.end_ns+=DT;
    CHECK(mx5_dr_step(&c,&i)==MX5_DR_OK);
    near(c.estimate.body_heading_rad,0.04,1e-12);
    i.interval_seq++; i.start_ns=i.end_ns; i.end_ns+=DT; i.received_ns=i.end_ns;
    CHECK(mx5_dr_step(&c,&i)==MX5_DR_E_TIME); /* no hold of mean into future */
}
static void stops_and_limits(void) {
    mx5_dr_core c; mx5_dr_interval i; mx5_dr_config p; mx5_dr_anchor a; double old_error,old_heading;
    setup(&c,0); run(&c,15,0.1,0.001,0); CHECK(c.estimate.stopped && !c.estimate.has_bearing);
    old_error=c.estimate.error_budget_m; old_heading=c.estimate.body_heading_rad;
    run(&c,10,0.3,0.001,0); CHECK(c.estimate.stopped); near(c.estimate.body_heading_rad,old_heading,0);
    CHECK(c.estimate.error_budget_m>=old_error+0.3);
    run(&c,1,0.5,0.1,1); CHECK(!c.estimate.stopped); CHECK(c.estimate.has_bearing);
    setup(&c,0); run(&c,15,0,0,0); i=interval(&c,0,0.1,0);
    CHECK(mx5_dr_step(&c,&i)==MX5_DR_E_FRAME);
    setup(&c,0); run(&c,10,0,0,0); run(&c,1,1,0,0); run(&c,5,0,0,0);
    CHECK(!c.estimate.stopped); /* interrupted dwell is not accumulated */
    setup(&c,0); run(&c,600,1,0,0);
    near(c.estimate.elapsed_s,60,1e-12); i=interval(&c,1,0,0);
    CHECK(mx5_dr_step(&c,&i)==MX5_DR_E_LIMIT);
    setup(&c,0); run(&c,150,100,0,0);
    near(c.estimate.distance_m,1500,1e-9); i=interval(&c,100,0,0);
    CHECK(mx5_dr_step(&c,&i)==MX5_DR_E_LIMIT);
    p=mx5_dr_default_config(); p.duration_max_s=1; a=anchor(1);
    CHECK(mx5_dr_init(&c,&p,ctx(1))==MX5_DR_OK); CHECK(mx5_dr_seed(&c,&a)==MX5_DR_OK);
    CHECK(mx5_dr_control(&c,MX5_DR_GAP,ctx(2),2)==MX5_DR_OK); run(&c,10,1,0,0);
    i=interval(&c,1,0,0); CHECK(mx5_dr_step(&c,&i)==MX5_DR_E_LIMIT);
    CHECK(c.estimate.state==MX5_DR_LIMIT_REACHED);
    p=mx5_dr_default_config(); p.distance_max_m=2;
    CHECK(mx5_dr_init(&c,&p,ctx(1))==MX5_DR_OK); CHECK(mx5_dr_seed(&c,&a)==MX5_DR_OK);
    CHECK(mx5_dr_control(&c,MX5_DR_GAP,ctx(2),2)==MX5_DR_OK); run(&c,1,20,0,1);
    i=interval(&c,20,0,1); CHECK(mx5_dr_step(&c,&i)==MX5_DR_E_LIMIT);
    p=mx5_dr_default_config(); p.error_max_m=1; a.position_error_m=0.95;
    CHECK(mx5_dr_init(&c,&p,ctx(1))==MX5_DR_OK); CHECK(mx5_dr_seed(&c,&a)==MX5_DR_OK);
    CHECK(mx5_dr_control(&c,MX5_DR_GAP,ctx(2),2)==MX5_DR_OK); run(&c,1,0,0,0);
    i=interval(&c,0,0,0); CHECK(mx5_dr_step(&c,&i)==MX5_DR_E_LIMIT);
}
static void reacquisition_and_replay(void) {
    mx5_dr_core c,d; mx5_dr_config p=mx5_dr_default_config(); mx5_dr_anchor a; mx5_dr_snapshot s;
    setup(&c,0); run(&c,10,10,0,0);
    CHECK(mx5_dr_control(&c,MX5_DR_GPS_RETURN,ctx(3),3)==MX5_DR_OK);
    CHECK(mx5_dr_get_snapshot(&c,c.estimate.frontier_ns,ctx(3),&s)==MX5_DR_E_NO_SEED);
    CHECK(mx5_dr_control(&c,MX5_DR_GAP,ctx(4),4)==MX5_DR_E_NO_SEED);
    CHECK(c.estimate.state==MX5_DR_REACQUIRING);
    a=anchor(4); a.position_seq=5; a.anchor_id=2; a.measured_ns=T0+5*DT; a.utc_ns+=5*DT;
    CHECK(mx5_dr_seed(&c,&a)==MX5_DR_OK); /* upstream replay starts at this late anchor */
    run(&c,5,10,0,0); near(c.estimate.distance_m,5,1e-12);
    CHECK(mx5_dr_control(&c,MX5_DR_GAP,ctx(5),6)==MX5_DR_OK); run(&c,1,10,0,0);
    CHECK(mx5_dr_get_snapshot(&c,c.estimate.frontier_ns,ctx(5),&s)==MX5_DR_OK);
    CHECK(s.anchor_id==2 && s.processed_position_seq==6);
    CHECK(mx5_dr_control(&c,MX5_DR_GPS_RETURN,ctx(4),7)==MX5_DR_E_CONTEXT && c.estimate.valid);
    CHECK(mx5_dr_init(&d,&p,ctx(4))==MX5_DR_OK); CHECK(mx5_dr_seed(&d,&a)==MX5_DR_OK);
    run(&d,5,10,0,0); CHECK(mx5_dr_control(&d,MX5_DR_GAP,ctx(5),6)==MX5_DR_OK); run(&d,1,10,0,0);
    near(d.estimate.latitude_deg,c.estimate.latitude_deg,0); near(d.estimate.error_budget_m,c.estimate.error_budget_m,0);
    CHECK(mx5_dr_reset(&c,ctx(6))==MX5_DR_OK); CHECK(!c.seeded && c.estimate.state==MX5_DR_UNSEEDED);
    CHECK(mx5_dr_get_snapshot(&c,T0,ctx(6),&s)==MX5_DR_E_NO_SEED);
}
int main(void) {
    geometry(); invalid_inputs(); timing_and_identity(); mean_intervals(); stops_and_limits(); reacquisition_and_replay();
    printf("core tests: %u checks passed (synthetic fixtures; no vehicle claims)\n",checks); return 0;
}
