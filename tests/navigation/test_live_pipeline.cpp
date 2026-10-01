// Synthetic end-to-end execution of production parser/codec/pipeline/adapter.
// No OEM binary, hardware, Unix socket or physical qualification is used.
#include "sensors/vim_source.h"
#include "navigation/channel.h"
#include "runtime/core_bridge.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
using namespace mx5;
namespace N=mx5::navigation;
namespace A=mx5::adapter;
static unsigned checks,sends;
static uint64_t clock_value;
static A::Observation last_position,last_send;
static A::VehicleData* original_wrapper;
static unsigned char outgoing[48];
static bool expect_original;
static uint32_t session=0x12345678;
#define CHECK(x) do { ++checks; if(!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::exit(1); } } while(0)
static uint64_t T(unsigned ms) { return 1000000000ULL+uint64_t(ms)*1000000ULL; }
static void put(unsigned char* p,uint64_t value,unsigned n) {
    for(unsigned i=0;i<n;++i)p[i]=static_cast<unsigned char>(value>>(8*i));
}
static uint32_t get32(const unsigned char* p) {
    return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);
}
static uint64_t clock_fn(void*) { return clock_value; }
static bool provenance(void*,const A::PositionInput*,A::Provenance* p,void*) {
    // Explicit synthetic provenance, never a runtime default.
    p->source_epoch=11;p->session_epoch=12;
    p->exact_request=p->verified_lds=p->legacy_receiver=true;return true;
}
static void observe(const A::Observation* o,void*) {
    if(o->kind==A::Observation::POSITION)last_position=*o;else last_send=*o;
}
static int32_t fake_oem(void* s,A::VehicleData* data) {
    CHECK(s==&session);CHECK((data==original_wrapper)==expect_original);
    CHECK(data&&data->payload&&data->length==48);++sends;
    std::memcpy(outgoing,data->payload,48);errno=EINPROGRESS;return -713;
}
static void position_bytes(unsigned char raw[72],int mode) {
    std::memset(raw,0,72);put(raw,uint32_t(mode),4);put(raw+8,1700000000,8);
    const double lat=35,lon=135,heading=0,speed=36;
    std::memcpy(raw+16,&lat,8);std::memcpy(raw+24,&lon,8);
    std::memcpy(raw+40,&heading,8);std::memcpy(raw+48,&speed,8);
}
static void gps_event(N::Pipeline& p,unsigned ms,int mode) {
    unsigned char raw[72];position_bytes(raw,mode);clock_value=T(ms);
    A::position_enter(0,raw);A::position_leave();
    CHECK(last_position.kind==A::Observation::POSITION);
    CHECK(last_position.position.mode==mode);
    CHECK(p.enqueue_position(last_position)==N::PIPELINE_OK);
}
static void transmit(bool original) {
    unsigned char payload[48],saved[48],position[72];
    for(unsigned i=0;i<48;++i)payload[i]=static_cast<unsigned char>(i+17);
    std::memcpy(saved,payload,48);position_bytes(position,0);
    A::VehicleData data={1,payload,48};original_wrapper=&data;expect_original=original;
    const unsigned before=sends;A::position_enter(0,position);
    errno=EDOM;CHECK(A::send_vehicle_data(&session,&data)==-713);
    CHECK(errno==EINPROGRESS);A::position_leave();CHECK(sends==before+1);
    CHECK(!std::memcmp(payload,saved,48));
    if(original)CHECK(!std::memcmp(outgoing,saved,48));
}
static void parsed_record(N::Pipeline& p,N::MotionCursor& cursor,uint64_t& seq,
                          unsigned ms,uint32_t id,unsigned value) {
    unsigned char payload[16];std::memset(payload,0x5a,sizeof payload);
    // Original VIM IPC data starts at SPI+3; byte0 is not an event envelope.
    unsigned length=0;
    if(id==0x100) {length=9;for(unsigned j=0;j<4;++j)put(payload+1+2*j,value,2);}
    else if(id==0x116) {length=4;put(payload+1,value,2);payload[3]=2;}
    else {length=2;payload[1]=static_cast<unsigned char>(value);}
    sensors::VimMessage message={id,length,payload};
    N::RawEvent raw=N::RawEvent();
    CHECK(sensors::decode_vim_message(message,99,++seq,T(ms),&raw)==sensors::VIM_DECODED);
    CHECK(raw.source_mono_ms==0);CHECK(raw.received_ns==T(ms));
    if(id==0x116) {CHECK(raw.raw[0]==value);CHECK(raw.count==2);}
    unsigned char wire[N::MOTION_RECORD_SIZE];CHECK(N::encode_motion(raw,wire));
    N::RawEvent copied=N::RawEvent();CHECK(N::decode_motion(wire,sizeof wire,&copied));
    CHECK(cursor.accept(4242,copied.epoch,copied.receive_seq));
    CHECK(copied.received_ns==raw.received_ns&&copied.source_mono_ms==0);
    N::PipelineResult r=p.enqueue_raw(copied);
    CHECK(r==N::PIPELINE_OK||r==N::PIPELINE_WAITING);
    // Copy ownership: mutating the borrowed input after parsing changes nothing.
    std::memset(payload,0xff,sizeof payload);
}
static runtime::CoreBridgeQualification qualify(const mx5_dr_snapshot& s) {
    runtime::CoreBridgeQualification q=runtime::CoreBridgeQualification();
    q.expected_context=s.context;q.now_mono_ns=clock_value;
    q.max_snapshot_age_ns=150000000;q.limits_verified_until_mono_ns=clock_value;
    q.duration_max_s=60;q.distance_max_m=1500;q.error_max_m=100;
    q.profile_verified=q.input_quality_verified=true;return q;
}
static void raw_shadow() {
    CHECK(A::set_mode(A::OBSERVE));
    N::Pipeline p;mx5_dr_context c={1,1,1};
    CHECK(p.init_model(N::research_model_profile(),mx5_dr_default_config(),c));
    N::MotionCursor cursor;uint64_t seq=0;
    for(unsigned ms=0;ms<=2300;ms+=50) {
        clock_value=T(ms);
        if(ms%100==0) {
            parsed_record(p,cursor,seq,ms,0x100,ms>=600?10000:13600);
            parsed_record(p,cursor,seq,ms,0x118,ms>=400?1:0);
            parsed_record(p,cursor,seq,ms,0x116,4094);
        }
        if(ms==0||ms==100)gps_event(p,ms,1);
        if(ms==150)gps_event(p,ms,0);
        CHECK(p.drain(clock_value-p.reorder_ns())==N::PIPELINE_OK);
        if(ms==300||ms==600||ms==2300) {
            N::Diagnostic d=p.diagnostic(clock_value);
            CHECK(d.result==MX5_DR_OK&&d.snapshot.model_valid&&!d.snapshot.valid);
            CHECK(d.snapshot.domain==MX5_DR_MODEL_DOMAIN);
            unsigned char preview[48];CHECK(runtime::encode_model_location_preview(d.snapshot,preview));
            CHECK(preview[32]==1);
            if(ms==300) {CHECK(get32(preview+36)==10000);CHECK(get32(preview+44)==0);}
            if(ms==600) {CHECK(preview[40]==1);CHECK(get32(preview+44)==180000000);}
            if(ms==2300) {CHECK(d.snapshot.stopped);CHECK(get32(preview+36)==0);CHECK(preview[40]==0);}
            A::DrSnapshot mapped=A::DrSnapshot();
            CHECK(p.qualified_snapshot(clock_value,qualify(d.snapshot),&mapped)==runtime::CORE_BRIDGE_UNQUALIFIED);
            CHECK(!mapped.ready);CHECK(!A::publish_snapshot(mapped));
            // Even forged physical-valid flags cannot cross the domain barrier.
            d.snapshot.valid=1;d.snapshot.model_valid=0;
            CHECK(runtime::map_core_snapshot(d.snapshot,qualify(d.snapshot),&mapped)==runtime::CORE_BRIDGE_UNQUALIFIED);
            CHECK(!mapped.ready);
            transmit(true);CHECK(last_send.choice==A::ORIGINAL);
        }
    }
    // Codec cannot smuggle extra bytes through the reserved wire region.
    N::RawEvent r=N::RawEvent();r.kind=N::REVERSE;r.epoch=1;r.receive_seq=1;r.received_ns=T(0);
    unsigned char bytes[N::MOTION_RECORD_SIZE];CHECK(N::encode_motion(r,bytes));bytes[63]=1;
    CHECK(!N::decode_motion(bytes,sizeof bytes,&r));
}
static mx5_dr_evidence evidence(unsigned id,unsigned seq,unsigned ms) {
    mx5_dr_evidence e=mx5_dr_evidence();e.source_id=id;e.source_epoch=1;e.producer_seq=seq;
    e.measured_ns=e.received_ns=T(ms);e.lease_until_ns=T(ms+250);
    e.quality=MX5_DR_VALID;e.freshness=MX5_DR_PRODUCER_TIME;return e;
}
static uint64_t revoke_candidate(void*) { return A::invalidate(); }
static void qualified_case(bool stopped) {
    // This side deliberately starts from qualified synthetic contracts. Parsed
    // raw data above is never upgraded into this evidence or this pipeline.
    clock_value=T(3000);CHECK(A::set_mode(A::ASSIST));
    unsigned char position[72];position_bytes(position,1);
    A::position_enter(0,position);A::position_leave();
    const A::Observation gps=last_position;
    const uint32_t generation=A::generation();CHECK(generation>1);
    N::Pipeline p;mx5_dr_context context={11,12,generation};
    mx5_dr_config config=mx5_dr_default_config();
    if(stopped)config.stop_hold_s=0.1; // explicit fast stationary fixture
    CHECK(p.init_qualified(config,context));
    CHECK(p.bind_qualified_revoker(revoke_candidate,0));
    mx5_dr_anchor a=mx5_dr_anchor();a.context=context;a.anchor_id=a.position_seq=1;
    a.measured_ns=T(3000);a.utc_ns=1700000000000000000ULL;a.latitude_deg=35;a.longitude_deg=135;
    a.position_error_m=1;a.heading_error_rad=0.01;
    a.validated=a.heading_valid=a.calibration_verified=1;a.quality=MX5_DR_VALID;
    CHECK(p.enqueue_anchor(a,T(3000),gps.call_sequence)==N::PIPELINE_OK);
    CHECK(p.enqueue_position(gps)==N::PIPELINE_OK);
    CHECK(p.enqueue_speed(evidence(1,1,3000),stopped?0:10)==N::PIPELINE_OK);
    CHECK(p.enqueue_reverse(evidence(3,1,3000),1)==N::PIPELINE_OK);
    CHECK(p.enqueue_yaw(evidence(2,1,3100),0,2047,1,T(3000),T(3100))==N::PIPELINE_OK);
    position_bytes(position,0);A::position_enter(0,position);A::position_leave();
    A::Observation gap=last_position;
    CHECK(p.enqueue_position(gap)==N::PIPELINE_OK);CHECK(p.drain(T(3100))==N::PIPELINE_OK);
    clock_value=T(3100);N::Diagnostic d=p.diagnostic(clock_value);
    CHECK(d.result==MX5_DR_OK&&d.snapshot.valid&&!d.snapshot.model_valid);
    CHECK(d.snapshot.context.generation==A::generation());
    A::DrSnapshot mapped=A::DrSnapshot();
    CHECK(p.qualified_snapshot(clock_value,qualify(d.snapshot),&mapped)==runtime::CORE_BRIDGE_OK);
    CHECK(A::publish_snapshot(mapped));transmit(false);CHECK(last_send.choice==A::DR_REPLACEMENT);
    CHECK(outgoing[32]==1);CHECK(get32(outgoing+36)==(stopped?0u:10000u));
    CHECK(outgoing[40]==(stopped?0:1));CHECK(get32(outgoing+44)==(stopped?0u:180000000u));
    if(!stopped)CHECK(int32_t(get32(outgoing+8))<350000000);
    ++clock_value;transmit(true);CHECK(last_send.reason==A::EXPIRED);
}
int main() {
    A::Options options=A::Options();options.clock=clock_fn;options.provenance=provenance;
    options.sink=observe;options.max_snapshot_age_ns=150000000;
    options.allow_assist=true; // synthetic fake OEM test; live runtime stays false
    CHECK(A::configure(fake_oem,options));raw_shadow();
    qualified_case(false);qualified_case(true);
    std::printf("raw parser -> motion codec -> navigation -> preview/adapter: %u checks, %u exactly-once sends (synthetic)\n",checks,sends);
    return 0;
}
