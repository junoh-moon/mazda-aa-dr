#ifndef MX5_NAVIGATION_TUNNEL_MAP_H
#define MX5_NAVIGATION_TUNNEL_MAP_H
// Map-constrained tunnel dead reckoning (BETA tunnel replacement).
//
// A compact, on-device derived database of OSM tunnel centrelines
// (tools/osm_tunnels.py, ODbL 1.0; built into the release bundle as
// tunnels-v1.bin). While a BETA GPS-lost episode is inside one uniquely
// matched tunnel the replaced LOCATION keeps the road centreline heading: the
// position is advanced from the entry fix along the chain tangent by the wheel
// distance travelled since that fix (the TAN variant of
// validation/MAP_AIDED_TUNNEL_HEADING_2026-10-14.md). Nothing here runs when
// there is no unique match: callers fail closed to the ordinary BETA DR
// position. No I/O, allocation or lock in match/tangent/point; load() runs
// once at worker start.
//
// The entry gates are those of the counterfactual evaluator
// (tools/map_aid_eval.py): lateral <= TUNNEL_MAP_LATERAL_M, along-position in
// [TUNNEL_MAP_S_MIN_M, min(TUNNEL_MAP_S_MAX_M, length - TUNNEL_MAP_END_MARGIN_M)],
// |entry course - chain tangent| <= TUNNEL_MAP_HEADING_DEG, one candidate only,
// fix fresher than TUNNEL_MAP_MAX_AGE_S and above TUNNEL_MAP_MIN_KMH.
#include <cmath>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace mx5 { namespace navigation {

// Binary format written by tools/osm_tunnels.py bin_image(): little-endian.
static const char TUNNEL_MAP_MAGIC[8]={'M','X','5','T','U','N','0','1'};
static const uint32_t TUNNEL_MAP_VERSION=1;
static const double TUNNEL_MAP_PI=3.14159265358979323846;
static const double TUNNEL_MAP_DEG=180.0/TUNNEL_MAP_PI;

struct TunnelMapPoint { float lat, lon; };
struct TunnelMapChain {
    uint32_t first;      // index of the first point
    uint16_t count;      // number of points, >= 2
    uint16_t flags;      // bit0: one-way
    float length_m;
    float lat_min, lat_max, lon_min, lon_max;
    uint32_t reserved;
};
struct TunnelMap {
    TunnelMapChain* chains;
    TunnelMapPoint* points;
    uint32_t nchains, npoints;
    void* allocation;    // one block owned by the map
};
struct TunnelMatch {
    uint32_t chain_index;
    double s_m;          // along-position of the entry projection (m)
    double lateral_m;    // distance from the centreline at entry (m)
    double dh_deg;       // entry course minus chain tangent (deg, [-180,180))
};

// Entry gates: the evaluator's values (tools/map_aid_eval.py). The device
// speed gate is 10 km/h, not the evaluator's 5 km/h: the false-match scan that
// found the 2026-10-14 parking-ramp cases used >=10 km/h, so the 5-10 km/h
// band stays on the ordinary DR (a tunnel entry below 10 km/h loses the map
// aid, never gains a false match).
static const double TUNNEL_MAP_LATERAL_M=15.0;
static const double TUNNEL_MAP_S_MIN_M=-60.0;
static const double TUNNEL_MAP_S_MAX_M=150.0;
static const double TUNNEL_MAP_END_MARGIN_M=100.0;
static const double TUNNEL_MAP_HEADING_DEG=20.0;
static const double TUNNEL_MAP_MIN_KMH=10.0;
static const double TUNNEL_MAP_MAX_AGE_S=2.0;
// Chain flags (tools/osm_tunnels.py bin_image): bit0 one-way, bit1 the chain
// does not end at a portal (fork/loop). A non-portal end must not be
// extrapolated: the constraint clears at the chain end and the ordinary DR
// resumes (no branch selection is attempted).
static const uint16_t TUNNEL_MAP_FLAG_ONEWAY=1u;
static const uint16_t TUNNEL_MAP_FLAG_NO_PAST_END=2u;
// Yaw-mismatch release: if the core's dead-reckoned heading change differs
// from the chain-tangent change by more than this, the map match is suspect
// (the 2026-10-14 record's yaw check with a fixed, tighter 15 deg instead of
// max(15 deg, k*t); it only falls back to the ordinary DR).
static const double TUNNEL_MAP_YAW_RELEASE_DEG=15.0;
// The constraint stays active while the wheel distance is inside the chain
// plus this margin; past it the ordinary DR resumes (the tangent extrapolation
// is only safe near the portal).
static const double TUNNEL_MAP_PAST_END_M=150.0;
// The evaluator's local metric frame.
static const double TUNNEL_MAP_EARTH_M_PER_DEG_LAT=111132.954;
static const double TUNNEL_MAP_EARTH_M_PER_DEG_LON=111412.84;

inline double tunnel_map_ang_deg(double a) {
    double r=fmod(a+180.0,360.0);
    if (r<0.0) r+=360.0;
    return r-180.0;
}
inline double tunnel_map_bearing_deg(double east,double north) {
    double b=atan2(east,north)*TUNNEL_MAP_DEG;
    if (b<0.0) b+=360.0;
    return b;
}
inline double tunnel_map_m_per_deg_lat(double lat_deg) {
    return TUNNEL_MAP_EARTH_M_PER_DEG_LAT-559.822*cos(2.0*lat_deg/TUNNEL_MAP_DEG);
}
inline double tunnel_map_m_per_deg_lon(double lat_deg) {
    return TUNNEL_MAP_EARTH_M_PER_DEG_LON*cos(lat_deg/TUNNEL_MAP_DEG);
}

// Closest point of the fix (at the local origin) to one chain. Distances in
// metres; a 60 m extension before the first point matches the evaluator.
inline bool tunnel_map_project(const TunnelMap* m,const TunnelMapChain& c,
                               double lat,double lon,double mLat,double mLon,
                               double* out_s,double* out_dist,double* out_tangent_deg) {
    if (!m||!m->points||c.count<2) return false;
    const uint32_t n=c.count;
    bool have=false;
    double best=0.0,best_s=0.0,best_tan=0.0,cum=0.0;
    for (uint32_t i=0;i+1<n;++i) {
        const TunnelMapPoint& a=m->points[c.first+i];
        const TunnelMapPoint& b=m->points[c.first+i+1];
        const double ax=(a.lon-lon)*mLon, ay=(a.lat-lat)*mLat;
        const double bx=(b.lon-lon)*mLon, by=(b.lat-lat)*mLat;
        const double dx=bx-ax, dy=by-ay;
        const double seg=sqrt(dx*dx+dy*dy);
        if (seg<1e-9) { cum+=0.0; continue; }
        const double lo=(i==0)?TUNNEL_MAP_S_MIN_M:0.0;
        double t=-(ax*dx+ay*dy)/(seg*seg);
        if (t<lo/seg) t=lo/seg;
        if (t>1.0) t=1.0;
        const double qx=ax+dx*t, qy=ay+dy*t;
        const double dist=sqrt(qx*qx+qy*qy);
        if (!have||dist<best) {
            have=true; best=dist; best_s=cum+t*seg; best_tan=atan2(dx,dy)*TUNNEL_MAP_DEG;
        }
        cum+=seg;
    }
    if (!have) return false;
    if (out_s) *out_s=best_s;
    if (out_dist) *out_dist=best;
    if (out_tangent_deg) *out_tangent_deg=best_tan;
    return true;
}

// A unique entry match, or false (no/ambiguous match; callers keep the DR).
inline bool tunnel_map_match(const TunnelMap* m,double lat,double lon,double course_deg,
                             double speed_kmh,double age_s,TunnelMatch* out) {
    if (!m||!m->chains||!m->points||m->nchains==0) return false;
    if (!std::isfinite(lat)||!std::isfinite(lon)||!std::isfinite(course_deg)) return false;
    if (!(speed_kmh>=TUNNEL_MAP_MIN_KMH)||!std::isfinite(age_s)||age_s<0.0||age_s>TUNNEL_MAP_MAX_AGE_S)
        return false;
    const double mLat=tunnel_map_m_per_deg_lat(lat), mLon=tunnel_map_m_per_deg_lon(lat);
    if (!(mLon>1.0)) return false;
    // The gates pad the bounding box by the lateral gate and by the 60 m
    // before-the-first-point extension (a fix lost just before the portal is
    // still a valid entry even though it lies outside the chain's own bbox).
    const double padM=TUNNEL_MAP_LATERAL_M-TUNNEL_MAP_S_MIN_M;
    const double latPad=padM/mLat, lonPad=padM/mLon;
    bool found=false;
    TunnelMatch best=TunnelMatch();
    for (uint32_t i=0;i<m->nchains;++i) {
        const TunnelMapChain& c=m->chains[i];
        if (c.count<2||c.length_m<1.0f) continue;
        if (lat<c.lat_min-latPad||lat>c.lat_max+latPad||
            lon<c.lon_min-lonPad||lon>c.lon_max+lonPad) continue;
        double s,dist,tan;
        if (!tunnel_map_project(m,c,lat,lon,mLat,mLon,&s,&dist,&tan)) continue;
        const double dh=tunnel_map_ang_deg(course_deg-tan);
        const double sMax=c.length_m-TUNNEL_MAP_END_MARGIN_M;
        const double sHi=TUNNEL_MAP_S_MAX_M<sMax?TUNNEL_MAP_S_MAX_M:sMax;
        if (!(dist<=TUNNEL_MAP_LATERAL_M)) continue;
        if (!(s>=TUNNEL_MAP_S_MIN_M&&s<=sHi)) continue;
        if (fabs(dh)>TUNNEL_MAP_HEADING_DEG) continue;
        if (found) return false;   // ambiguous: two chains pass the gates
        found=true;
        best.chain_index=i; best.s_m=s; best.lateral_m=dist; best.dh_deg=dh;
    }
    if (!found) return false;
    if (out) *out=best;
    return true;
}

// Chain tangent (deg) at along-distance s; straight extrapolation outside the
// chain. mLat/mLon come from the entry origin (metric frame of the match).
inline double tunnel_map_tangent_deg(const TunnelMap* m,uint32_t chain_index,double s,
                                     double mLat,double mLon) {
    if (!m||!m->chains||!m->points||chain_index>=m->nchains) return 0.0;
    const TunnelMapChain& c=m->chains[chain_index];
    if (c.count<2) return 0.0;
    const uint32_t n=c.count;
    double cum=0.0,tan=0.0;
    for (uint32_t i=0;i+1<n;++i) {
        const TunnelMapPoint& a=m->points[c.first+i];
        const TunnelMapPoint& b=m->points[c.first+i+1];
        const double dx=(b.lon-a.lon)*mLon, dy=(b.lat-a.lat)*mLat;
        const double seg=sqrt(dx*dx+dy*dy);
        if (seg<1e-9) continue;
        tan=atan2(dx,dy)*TUNNEL_MAP_DEG;
        if (s<=cum+seg||i+2==n) return tan;
        cum+=seg;
    }
    return tan;
}

// Chain point at along-distance s; straight extrapolation outside the chain.
inline bool tunnel_map_point(const TunnelMap* m,uint32_t chain_index,double s,
                             double mLat,double mLon,double* out_lat,double* out_lon) {
    if (!m||!m->chains||!m->points||chain_index>=m->nchains) return false;
    const TunnelMapChain& c=m->chains[chain_index];
    if (c.count<2) return false;
    const uint32_t n=c.count;
    double cum=0.0;
    for (uint32_t i=0;i+1<n;++i) {
        const TunnelMapPoint& a=m->points[c.first+i];
        const TunnelMapPoint& b=m->points[c.first+i+1];
        const double dx=(b.lon-a.lon)*mLon, dy=(b.lat-a.lat)*mLat;
        const double seg=sqrt(dx*dx+dy*dy);
        if (seg<1e-9) continue;
        if (s<=cum+seg||i+2==n) {
            const double t=(s-cum)/seg;
            if (out_lat) *out_lat=a.lat+t*(b.lat-a.lat);
            if (out_lon) *out_lon=a.lon+t*(b.lon-a.lon);
            return true;
        }
        cum+=seg;
    }
    return false;
}

inline void tunnel_map_free(TunnelMap* m) {
    if (!m) return;
    free(m->allocation);
    free(m);
}

// Load the on-device database. Returns 0 on any error (callers keep the DR).
inline TunnelMap* tunnel_map_load(const char* path) {
    if (!path) return 0;
    FILE* f=fopen(path,"rb");
    if (!f) return 0;
    unsigned char head[24];
    if (fread(head,1,sizeof head,f)!=sizeof head) { fclose(f); return 0; }
    if (memcmp(head,TUNNEL_MAP_MAGIC,8)!=0) { fclose(f); return 0; }
    uint32_t v,nc,np,pad;
    memcpy(&v,head+8,4); memcpy(&nc,head+12,4); memcpy(&np,head+16,4); memcpy(&pad,head+20,4);
    if (v!=TUNNEL_MAP_VERSION||nc==0||np==0||pad!=0) { fclose(f); return 0; }
    // A corrupt count must fail before any large allocation; the real database
    // has 19290 chains / 47401 points, so these caps are far above any release.
    if (nc>0x400000u||np>0x1000000u) { fclose(f); return 0; }
    const size_t chainBytes=(size_t)nc*sizeof(TunnelMapChain);
    const size_t pointBytes=(size_t)np*sizeof(TunnelMapPoint);
    if (chainBytes>((size_t)-1)-24u-pointBytes) { fclose(f); return 0; }
    const size_t want=24u+chainBytes+pointBytes;
    if (fseek(f,0,SEEK_END)!=0) { fclose(f); return 0; }
    const long end=ftell(f);
    if (end<0||(unsigned long)end!=(unsigned long)want) { fclose(f); return 0; }
    if (fseek(f,24,SEEK_SET)!=0) { fclose(f); return 0; }
    TunnelMap* m=(TunnelMap*)calloc(1,sizeof(TunnelMap));
    if (!m) { fclose(f); return 0; }
    m->allocation=malloc(chainBytes+pointBytes);
    if (!m->allocation) { fclose(f); free(m); return 0; }
    m->chains=(TunnelMapChain*)m->allocation;
    m->points=(TunnelMapPoint*)((unsigned char*)m->allocation+chainBytes);
    m->nchains=nc; m->npoints=np;
    if (fread(m->chains,1,chainBytes,f)!=chainBytes||
        fread(m->points,1,pointBytes,f)!=pointBytes) {
        fclose(f); tunnel_map_free(m); return 0;
    }
    fclose(f);
    for (uint32_t i=0;i<nc;++i) {
        const TunnelMapChain& c=m->chains[i];
        if (c.count<2||c.count>np||c.first>np-c.count) { tunnel_map_free(m); return 0; }
    }
    return m;
}

} }
#endif
