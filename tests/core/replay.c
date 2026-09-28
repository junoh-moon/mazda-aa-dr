/* Offline normalized-input replayer. Never reads CMU, never opens a socket.
 * Explicit evidence columns are caller claims, not verified live provenance. */
#include "dr_core.h"
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MAX_FIELDS 64
static int parse_ok;
static uint64_t integer(const char *s) {
    char *end; unsigned long long v;
    if (!s || !*s || *s=='-' || *s=='+') { parse_ok=0; return 0; }
    errno=0; v=strtoull(s,&end,10);
    if(errno || *end) parse_ok=0;
    return (uint64_t)v;
}
static double number(const char *s) {
    char *end; double v;
    if(!s || !*s) { parse_ok=0; return 0; }
    errno=0; v=strtod(s,&end);
    if(errno || *end || !isfinite(v)) parse_ok=0;
    return v;
}
static int small_integer(const char *s,unsigned max) {
    uint64_t v=integer(s); if(v>max) parse_ok=0; return (int)v;
}
static mx5_dr_context context(char **v) {
    mx5_dr_context x; x.source_epoch=integer(v[0]); x.session_epoch=integer(v[1]); x.generation=integer(v[2]); return x;
}
static mx5_dr_evidence evidence(char **v) {
    mx5_dr_evidence e;
    e.source_id=integer(v[0]); e.source_epoch=integer(v[1]); e.producer_seq=integer(v[2]);
    e.measured_ns=integer(v[3]); e.received_ns=integer(v[4]); e.lease_until_ns=integer(v[5]);
    e.time_uncertainty_ns=integer(v[6]); e.quality=(mx5_dr_quality)small_integer(v[7],2);
    e.freshness=(mx5_dr_freshness)small_integer(v[8],2); return e;
}
static void emit(unsigned line,const char *event,mx5_dr_result r,const mx5_dr_snapshot *s) {
    printf("%u,%s,%s,%d,%d,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
           ",%.12f,%.12f,%.12f,%.12f,%d,%.9f,%.9f,%.9f\n",
           line,event,mx5_dr_result_name(r),(int)s->state,s->valid,s->context.source_epoch,
           s->context.session_epoch,s->context.generation,s->frontier_ns,s->derived_utc_ns,
           s->latitude_deg,s->longitude_deg,s->body_heading_rad,s->travel_bearing_rad,
           s->has_bearing,s->speed_mps,s->distance_m,s->error_budget_m);
}
int main(int argc,char **argv) {
    FILE *input; char line[4096]; unsigned line_no=0; int initialized=0;
    mx5_dr_core core; mx5_dr_config config=mx5_dr_default_config();
    if(argc!=2) { fprintf(stderr,"usage: %s normalized.csv\n",argv[0]); return 2; }
    input=fopen(argv[1],"r"); if(!input) { perror(argv[1]); return 2; }
    memset(&core,0,sizeof(core));
    puts("line,event,result,state,valid,source_epoch,session_epoch,generation,frontier_ns,derived_utc_ns,latitude_deg,longitude_deg,body_heading_rad,travel_bearing_rad,has_bearing,speed_mps,distance_m,error_budget_m");
    while(fgets(line,sizeof(line),input)) {
        char *v[MAX_FIELDS], *p; size_t n=0,len; mx5_dr_result r=MX5_DR_E_CONFIG; mx5_dr_snapshot s;
        ++line_no; len=strlen(line);
        if(len==sizeof(line)-1 && line[len-1]!='\n') goto malformed;
        while(len && (line[len-1]=='\r'||line[len-1]=='\n')) line[--len]=0;
        if(!len || line[0]=='#') continue;
        p=line; v[n++]=p;
        while(*p) { if(*p==',') { *p=0; if(n==MAX_FIELDS) goto malformed; v[n++]=p+1; } ++p; }
        parse_ok=1;
        if(!strcmp(v[0],"RESET") && n==4) {
            mx5_dr_context x=context(v+1); if(!parse_ok) goto malformed;
            r=mx5_dr_init(&core,&config,x); initialized=r==MX5_DR_OK;
        } else if(!initialized) goto malformed;
        else if(!strcmp(v[0],"SEED") && n==18) {
            mx5_dr_anchor a; memset(&a,0,sizeof(a)); a.context=context(v+1);
            a.anchor_id=integer(v[4]); a.position_seq=integer(v[5]); a.measured_ns=integer(v[6]); a.utc_ns=integer(v[7]);
            a.latitude_deg=number(v[8]); a.longitude_deg=number(v[9]); a.body_heading_rad=number(v[10]);
            a.position_error_m=number(v[11]); a.heading_error_rad=number(v[12]);
            a.validated=small_integer(v[13],1); a.heading_valid=small_integer(v[14],1);
            a.calibration_verified=small_integer(v[15],1); a.quality=(mx5_dr_quality)small_integer(v[16],2);
            /* final explicit field identifies test/replay data; never a live gate */
            if(strcmp(v[17],"REPLAY")) parse_ok=0;
            if(!parse_ok) goto malformed;
            r=mx5_dr_seed(&core,&a);
        } else if(!strcmp(v[0],"CONTROL") && n==6) {
            mx5_dr_context x=context(v+1); uint64_t seq=integer(v[4]);
            int kind=small_integer(v[5],3); if(!parse_ok) goto malformed;
            r=mx5_dr_control(&core,(mx5_dr_control_kind)kind,x,seq);
        } else if(!strcmp(v[0],"MOTION") && n==43) {
            mx5_dr_interval i; memset(&i,0,sizeof(i)); i.context=context(v+1);
            i.interval_seq=integer(v[4]); i.start_ns=integer(v[5]); i.end_ns=integer(v[6]); i.received_ns=integer(v[7]);
            i.speed_mps=number(v[8]); i.yaw_rad_s=number(v[9]); i.reverse_active=small_integer(v[10],2);
            i.raw_yaw=(uint16_t)small_integer(v[11],65535); i.yaw_count=(uint16_t)small_integer(v[12],65535);
            i.yaw_is_mean=small_integer(v[13],1); i.yaw_window_start_ns=integer(v[14]); i.yaw_window_end_ns=integer(v[15]);
            i.speed=evidence(v+16); i.yaw=evidence(v+25); i.reverse=evidence(v+34);
            if(!parse_ok) goto malformed;
            r=mx5_dr_step(&core,&i);
        } else if(!strcmp(v[0],"SNAPSHOT") && n==5) {
            mx5_dr_context x=context(v+1); uint64_t now=integer(v[4]); if(!parse_ok) goto malformed;
            r=mx5_dr_get_snapshot(&core,now,x,&s); emit(line_no,v[0],r,&s); continue;
        } else goto malformed;
        emit(line_no,v[0],r,&core.estimate);
    }
    if(ferror(input)) { perror("read"); fclose(input); return 2; }
    fclose(input); return 0;
malformed:
    fprintf(stderr,"malformed CSV or missing RESET at line %u; no later events processed\n",line_no);
    fclose(input); return 2;
}
