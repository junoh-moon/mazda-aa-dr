#include "adapter/adapter.h"
#include <cassert>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
using namespace mx5::adapter;

static unsigned calls, events;
static uint8_t sent[48];
static uint64_t clock_ns = 1000000000;
static void* expected_session;
static VehicleData* original_wrapper;
static bool expect_original, publish_on_position;
static Observation last_event;
static int next_expected_errno = 17;
static DrSnapshot fixture() {
    DrSnapshot s = DrSnapshot();
    s.source_epoch = 11; s.session_epoch = 12;
    s.prediction_generation = generation();
    s.frontier_mono_ns = clock_ns - 1000000;
    s.valid_until_mono_ns = clock_ns + 100000000;
    s.derived_utc_ns = 1234000000000ULL;
    s.latitude_deg = -37.12345675; s.longitude_deg = 127.1;
    s.speed_mps = 12.3455; s.travel_bearing_deg = 359.9999996;
    s.ready = s.profile_verified = s.input_quality_verified = s.limits_ok = true;
    return s;
}
static int32_t fake_next(void* session, VehicleData* data) {
    assert(session == expected_session); assert(errno == next_expected_errno);
    ++calls;
    if (expect_original) assert(data == original_wrapper);
    else assert(data != original_wrapper);
    if (data && data->payload && data->length == 48) std::memcpy(sent, data->payload, 48);
    errno = EDOM; return -123;
}
static void sink(const Observation* e, void*) {
    ++events; last_event = *e;
    if (e->kind == Observation::POSITION && publish_on_position && e->original_mode == 0)
        assert(publish_snapshot(fixture()));
    errno = EBUSY; // The shim must hide this side effect from OEM code.
}
static uint64_t clock_fn(void*) { errno = EAGAIN; return clock_ns; }
static bool provenance(void*, const PositionInput*, Provenance* out, void*) {
    out->source_epoch = 11; out->session_epoch = 12;
    out->exact_request = out->verified_lds = out->legacy_receiver = true; return true;
}
static void put32(uint8_t* p, uint32_t v) {
    for (unsigned i=0;i<4;++i) p[i] = uint8_t(v >> (8*i));
}
static uint32_t get32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1])<<8 | uint32_t(p[2])<<16 | uint32_t(p[3])<<24;
}
static void run_send(VehicleData& data, bool original) {
    original_wrapper=&data; expect_original=original; errno=17;
    const unsigned before=calls;
    assert(send_vehicle_data(expected_session,&data)==-123);
    assert(calls==before+1); assert(errno==EDOM);
}
int main(int argc,char** argv) {
    assert(argc==2);
    Options o = Options();o.sink=sink;o.clock=clock_fn;o.provenance=provenance;
    o.allow_assist=true;o.max_snapshot_age_ns=150000000;
    assert(configure(fake_next,o));
    assert(!configure(fake_next,o));
    static uint32_t session_handle=0xabcdef; expected_session=&session_handle;
    uint8_t payload[48]; for(unsigned i=0;i<48;++i)payload[i]=uint8_t(i+1);
    VehicleData data={1,payload,48}; uint8_t position[72]={};
    const char* test=argv[1];
    if (!std::strcmp(test,"observe")) {
        position_enter(0,position); run_send(data,true); position_leave();
        assert(!std::memcmp(sent,payload,48)); assert(last_event.choice==ORIGINAL);
        data.type=3;run_send(data,true);
    } else if (!std::strcmp(test,"scrub")) {
        assert(set_mode(SCRUB_STALE));position_enter(0,position);
        run_send(data,false);position_leave();
        assert(sent[32]==0 && sent[40]==0 && get32(sent+36)==0 && get32(sent+44)==0);
        for(unsigned i=0;i<48;++i)
            if(i!=32 && i!=40 && !(i>=36&&i<40) && !(i>=44&&i<48)) assert(sent[i]==payload[i]);
        assert(payload[32]==33 && payload[40]==41);assert(last_event.choice==SCRUBBED);
    } else if (!std::strcmp(test,"native")) {
        assert(set_mode(SCRUB_STALE));
        for(unsigned m=1;m<=3;++m) {put32(position,m);position_enter(0,position);run_send(data,true);position_leave();assert(!std::memcmp(sent,payload,48));}
        run_send(data,true);assert(last_event.reason==NO_CONTEXT);
    } else if (!std::strcmp(test,"malformed")) {
        assert(set_mode(SCRUB_STALE));position_enter(0,position);
        data.length=47;run_send(data,true);data.length=48;run_send(data,true);
        assert(last_event.reason==EXTRA_LOCATION);position_leave();
        position_enter(0,position);run_send(data,true);position_leave(); // fault is latched
    } else if (!std::strcmp(test,"nested")) {
        assert(set_mode(SCRUB_STALE));position_enter(0,position);position_enter(0,position);
        run_send(data,true);assert(last_event.reason==NESTED_CALL);position_leave();
        run_send(data,false);position_leave();
    } else if (!std::strcmp(test,"assist")) {
        assert(set_mode(ASSIST));publish_on_position=true;position_enter(0,position);
        run_send(data,false);position_leave();
        assert(last_event.choice==DR_REPLACEMENT);assert(sent[16]==0 && sent[24]==0 && sent[32]==1);
        assert(get32(sent+36)==12346);assert(get32(sent+44)==0);
    } else if (!std::strcmp(test,"epoch")) {
        assert(set_mode(ASSIST));publish_on_position=true;position_enter(0,position);
        invalidate();run_send(data,true);assert(last_event.reason==EPOCH_MISMATCH);position_leave();
    } else if (!std::strcmp(test,"reacquire")) {
        assert(set_mode(ASSIST));publish_on_position=true;position_enter(0,position);
        run_send(data,false);position_leave();publish_on_position=false;
        put32(position,1);position_enter(0,position);run_send(data,true);position_leave();
        put32(position,0);position_enter(0,position);run_send(data,true);position_leave();
        assert(last_event.reason==EPOCH_MISMATCH);
    } else if (!std::strcmp(test,"expiry")) {
        assert(set_mode(ASSIST));publish_on_position=true;position_enter(0,position);
        clock_ns+=200000000;run_send(data,true);position_leave();assert(last_event.reason==EXPIRED);
    } else if (!std::strcmp(test,"encoder")) {
        DrSnapshot s=fixture();uint8_t b[48];assert(encode_location(s,b));
        s.stopped=true;s.travel_bearing_deg=std::numeric_limits<double>::quiet_NaN();
        assert(encode_location(s,b));assert(b[40]==0 && get32(b+36)==0);
        s.latitude_deg=91;assert(!encode_location(s,b));s=fixture();
        s.longitude_deg=std::numeric_limits<double>::infinity();assert(!encode_location(s,b));
        s=fixture();s.speed_mps=-1;assert(!encode_location(s,b));
        s=fixture();s.speed_mps=1e20;assert(!encode_location(s,b));
        s=fixture();s.longitude_deg=180;assert(encode_location(s,b));assert(int32_t(get32(b+12))==-1800000000);
    } else if (!std::strcmp(test,"backend")) {
        InstallOptions io = InstallOptions();
#if defined(__arm__) && !defined(__ARM_PCS_VFP) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        assert(install_v74(io)==INVALID_INSTALL_ARGUMENT);
#else
        assert(install_v74(io)==UNSUPPORTED_ARCH);
#endif
    } else return 2;
    std::printf("PASS %s\n",test);return 0;
}
