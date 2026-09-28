// Normalized SYNTHETIC integration only. No CMU, no live profile qualification.
#include "runtime/core_bridge.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
using namespace mx5;
static uint64_t now_ns=1100000000;
static unsigned sends;
static adapter::VehicleData* borrowed_wrapper;
static void* expected_session;
static bool expect_original;
static uint8_t sent[48];
static adapter::Observation last_observation;
static int32_t fake_oem(void* session,adapter::VehicleData* data) {
    assert(session==expected_session);
    assert((data==borrowed_wrapper)==expect_original);
    ++sends;
    assert(data && data->payload && data->length==48);
    std::memcpy(sent,data->payload,48);
    return -731;
}
static uint64_t clock_fn(void*) { return now_ns; }
static bool provenance(void*,const adapter::PositionInput*,adapter::Provenance* p,void*) {
    p->source_epoch=11;p->session_epoch=12;
    p->exact_request=p->verified_lds=p->legacy_receiver=true; // fixture only
    return true;
}
static void observe(const adapter::Observation* p,void*) { last_observation=*p; }
static void put32(uint8_t* p,uint32_t n) { for(unsigned i=0;i<4;++i)p[i]=uint8_t(n>>(8*i)); }
static uint32_t get32(const uint8_t* p) {
    return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);
}
static mx5_dr_context context(uint64_t generation) { mx5_dr_context c={11,12,generation}; return c; }
static mx5_dr_evidence evidence(unsigned id) {
    mx5_dr_evidence e=mx5_dr_evidence();e.source_id=id;e.source_epoch=1;e.producer_seq=1;
    e.measured_ns=e.received_ns=1000000000;e.lease_until_ns=1250000000;
    e.quality=MX5_DR_VALID;e.freshness=MX5_DR_PRODUCER_TIME;return e;
}
static mx5_dr_snapshot prepare(mx5_dr_core& core) {
    const uint32_t g=adapter::generation();assert(g>=2);
    mx5_dr_config config=mx5_dr_default_config();
    assert(mx5_dr_init(&core,&config,context(g-1))==MX5_DR_OK);
    mx5_dr_anchor a=mx5_dr_anchor();a.context=context(g-1);a.anchor_id=1;a.position_seq=1;
    a.measured_ns=1000000000;a.utc_ns=1700000000000000000ULL;
    a.latitude_deg=37.0;a.longitude_deg=127.0;a.body_heading_rad=0;
    a.position_error_m=1;a.heading_error_rad=0.01;
    a.validated=a.heading_valid=a.calibration_verified=1;a.quality=MX5_DR_VALID;
    assert(mx5_dr_seed(&core,&a)==MX5_DR_OK);
    assert(mx5_dr_control(&core,MX5_DR_GAP,context(g),2)==MX5_DR_OK);
    mx5_dr_interval i=mx5_dr_interval();i.context=context(g);i.interval_seq=1;
    i.start_ns=1000000000;i.end_ns=i.received_ns=1100000000;
    i.speed=evidence(1);i.yaw=evidence(2);i.reverse=evidence(3);
    i.speed_mps=20;i.raw_yaw=2047;i.yaw_count=1;
    assert(mx5_dr_step(&core,&i)==MX5_DR_OK);
    mx5_dr_snapshot s=mx5_dr_snapshot();assert(mx5_dr_get_snapshot(&core,now_ns,context(g),&s)==MX5_DR_OK);
    return s;
}
static runtime::CoreBridgeQualification qualify(const mx5_dr_snapshot& s) {
    runtime::CoreBridgeQualification q=runtime::CoreBridgeQualification();q.expected_context=s.context;q.now_mono_ns=now_ns;
    q.max_snapshot_age_ns=150000000;q.limits_verified_until_mono_ns=now_ns;
    q.duration_max_s=60;q.distance_max_m=1500;q.error_max_m=100;
    q.profile_verified=q.input_quality_verified=true; // fixture claims, never a live default
    return q;
}
static void reject(const mx5_dr_snapshot& s,const runtime::CoreBridgeQualification& q,
                   runtime::CoreBridgeResult reason) {
    adapter::DrSnapshot out=adapter::DrSnapshot();out.ready=true;
    assert(runtime::map_core_snapshot(s,q,&out)==reason);
    assert(!out.ready && !out.profile_verified && !out.input_quality_verified && !out.limits_ok);
    assert(out.source_epoch==0 && out.derived_utc_ns==0);
}
static void bridge_contracts(const mx5_dr_snapshot& valid) {
    using namespace runtime;
    mx5_dr_snapshot s=valid; CoreBridgeQualification q=qualify(s); adapter::DrSnapshot out=adapter::DrSnapshot();
    assert(map_core_snapshot(s,q,&out)==CORE_BRIDGE_OK);
    assert(out.frontier_mono_ns==s.frontier_ns && out.derived_utc_ns==s.derived_utc_ns);
    assert(out.valid_until_mono_ns==now_ns); // no invented future error qualification
    assert(out.source_epoch==11 && out.session_epoch==12 && out.ready);
    assert(map_core_snapshot(s,q,0)==CORE_BRIDGE_NO_OUTPUT);
    q=CoreBridgeQualification();reject(s,q,CORE_BRIDGE_UNQUALIFIED);
    q=qualify(s);q.profile_verified=false;reject(s,q,CORE_BRIDGE_UNQUALIFIED);
    q=qualify(s);q.input_quality_verified=false;reject(s,q,CORE_BRIDGE_UNQUALIFIED);
    q=qualify(s);s.valid=0;reject(s,q,CORE_BRIDGE_UNQUALIFIED);
    s=valid;s.state=MX5_DR_READY;reject(s,q,CORE_BRIDGE_UNQUALIFIED);
    s=valid;s.reason=MX5_DR_E_LIMIT;reject(s,q,CORE_BRIDGE_UNQUALIFIED);
    for(unsigned j=0;j<3;++j) {
        s=valid;uint64_t* field=j==0?&s.context.source_epoch:j==1?&s.context.session_epoch:&s.context.generation;
        *field=0;q=qualify(s);reject(s,q,CORE_BRIDGE_CONTEXT);
        *field=uint64_t(std::numeric_limits<uint32_t>::max())+1;q=qualify(s);reject(s,q,CORE_BRIDGE_OVERFLOW);
    }
    s=valid;q=qualify(s);q.expected_context.generation++;reject(s,q,CORE_BRIDGE_CONTEXT);
    s=valid;q=qualify(s);s.sensor_lease_until_ns=now_ns-1;reject(s,q,CORE_BRIDGE_TIME);
    s=valid;q=qualify(s);q.now_mono_ns=s.frontier_ns-1;reject(s,q,CORE_BRIDGE_TIME);
    s=valid;q=qualify(s);q.limits_verified_until_mono_ns=now_ns-1;reject(s,q,CORE_BRIDGE_TIME);
    s=valid;q=qualify(s);s.error_budget_m=100.01;reject(s,q,CORE_BRIDGE_LIMIT);
    s=valid;q=qualify(s);s.distance_m=1500.01;reject(s,q,CORE_BRIDGE_LIMIT);
    s=valid;q=qualify(s);s.latitude_deg=std::numeric_limits<double>::quiet_NaN();reject(s,q,CORE_BRIDGE_NUMERIC);
    s=valid;q=qualify(s);s.has_bearing=0;reject(s,q,CORE_BRIDGE_BEARING);
    s=valid;q=qualify(s);s.stopped=1;s.has_bearing=0;s.speed_mps=0;
    s.travel_bearing_rad=std::numeric_limits<double>::quiet_NaN();
    assert(map_core_snapshot(s,q,&out)==CORE_BRIDGE_OK && out.stopped);
    uint8_t encoded[48];assert(adapter::encode_location(out,encoded));assert(encoded[40]==0 && get32(encoded+36)==0);
    s=valid;q=qualify(s);s.travel_bearing_rad=3.14159265358979323846;
    assert(map_core_snapshot(s,q,&out)==CORE_BRIDGE_OK);
    assert(std::fabs(out.travel_bearing_deg-180.0)<1e-12);
    // A longer independently qualified horizon is still capped by source lease/age.
    s=valid;q=qualify(s);q.limits_verified_until_mono_ns=now_ns+1000000000;
    assert(map_core_snapshot(s,q,&out)==CORE_BRIDGE_OK && out.valid_until_mono_ns==1250000000);
}
static void send(adapter::VehicleData& data,bool original) {
    borrowed_wrapper=&data;expect_original=original;const unsigned before=sends;
    assert(adapter::send_vehicle_data(expected_session,&data)==-731);
    assert(sends==before+1);
}
int main() {
    adapter::Options options=adapter::Options();options.clock=clock_fn;options.provenance=provenance;
    options.sink=observe;options.max_snapshot_age_ns=150000000;
    options.allow_assist=true; // fake OEM test only; runtime configuration remains disabled
    assert(adapter::configure(fake_oem,options));assert(adapter::set_mode(adapter::ASSIST));
    uint32_t session_handle=0x1234;expected_session=&session_handle;
    uint8_t original[48],position[72]={};for(unsigned j=0;j<48;++j)original[j]=uint8_t(j+1);
    adapter::VehicleData data={1,original,48};
    adapter::position_enter(0,position); // establishes exact mode0 context and generation
    mx5_dr_core core;mx5_dr_snapshot s=prepare(core);bridge_contracts(s);
    adapter::DrSnapshot mapped=adapter::DrSnapshot();
    assert(runtime::map_core_snapshot(s,qualify(s),&mapped)==runtime::CORE_BRIDGE_OK);
    assert(adapter::publish_snapshot(mapped));
    send(data,false);adapter::position_leave();
    assert(last_observation.choice==adapter::DR_REPLACEMENT);
    assert(get32(sent+36)==20000 && sent[32]==1 && sent[16]==0 && sent[24]==0);
    assert(int32_t(get32(sent+8))>370000000 && int32_t(get32(sent+12))==1270000000);
    for(unsigned j=0;j<48;++j)assert(original[j]==uint8_t(j+1)); // borrowed input untouched
    // Even 1 ns beyond the explicitly qualified lease fails open exactly once.
    now_ns++;adapter::position_enter(0,position);send(data,true);adapter::position_leave();
    assert(last_observation.reason==adapter::EXPIRED);assert(!std::memcmp(sent,original,48));
    // GPS return immediately invalidates both core authority and adapter generation.
    put32(position,1);adapter::position_enter(0,position);
    assert(mx5_dr_control(&core,MX5_DR_GPS_RETURN,context(adapter::generation()),3)==MX5_DR_OK);
    assert(mx5_dr_get_snapshot(&core,now_ns,context(adapter::generation()),&s)==MX5_DR_E_NO_SEED);
    reject(s,qualify(s),runtime::CORE_BRIDGE_UNQUALIFIED);
    send(data,true);adapter::position_leave();assert(last_observation.reason==adapter::NOT_UNKNOWN);
    put32(position,0);adapter::position_enter(0,position);
    assert(mx5_dr_control(&core,MX5_DR_GAP,context(adapter::generation()),4)==MX5_DR_E_NO_SEED);
    send(data,true);adapter::position_leave();assert(last_observation.reason==adapter::EPOCH_MISMATCH);
    assert(!std::memcmp(sent,original,48));
    for(unsigned native=1;native<=3;++native) {
        put32(position,native);adapter::position_enter(0,position);send(data,true);adapter::position_leave();
        assert(last_observation.reason==adapter::NOT_UNKNOWN && !std::memcmp(sent,original,48));
    }
    // Other sensor data is never rewritten, including within a mode0 call.
    put32(position,0);data.type=3;adapter::position_enter(0,position);send(data,true);adapter::position_leave();
    assert(last_observation.choice==adapter::ORIGINAL);
    std::printf("pipeline tests: core -> qualified bridge -> fake OEM, %u exactly-once sends passed (synthetic only)\n",sends);
    return 0;
}
