#include "adapter/adapter.h"
#include <cassert>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <limits>
#include <pthread.h>
using namespace mx5::adapter;

// Test-only interposition: the adapter's hook path uses trylock, so a forced
// EBUSY exercises its LOCK_BUSY branch without racing a worker thread.
static bool force_lock_busy;
extern "C" int pthread_mutex_trylock(pthread_mutex_t* m) {
    if (force_lock_busy) return EBUSY;
    typedef int (*Real)(pthread_mutex_t*);
    static Real real = 0;
    if (!real) real = reinterpret_cast<Real>(dlsym(RTLD_NEXT, "pthread_mutex_trylock"));
    assert(real);
    return real(m);
}

static unsigned calls, events;
static uint8_t sent[48];
static uint64_t clock_ns = 1000000000;
static void* expected_session;
static VehicleData* original_wrapper;
static bool expect_original, publish_on_position;
static Observation last_event;
static int next_expected_errno = 17;
namespace R = mx5::runtime::request_trace;
static const void* expected_position;
static R::Result trace_result=R::OK;
static unsigned trace_reads;
static unsigned session_reads;
// BETA-case knobs. Defaults leave every pre-existing case unchanged.
static bool beta_fixture, speed_fixture;
static void (*mutate_snapshot)(DrSnapshot&);
static Provenance::Domain provenance_domain = Provenance::Domain::NONE;
static mx5::runtime::session_trace::Result beta_session_result = mx5::runtime::session_trace::OBSERVED;
static int32_t next_result = -123;
static unsigned hold_set_events, hold_cleared_events;
static void beta_session_reader(const void* storage,mx5::runtime::session_trace::Snapshot* out,void*) {
    assert(storage==expected_session);++session_reads;errno=EIO;
    *out=mx5::runtime::session_trace::Snapshot();out->result=beta_session_result;
}
static void beta_event(void* user,const char* what) {
    assert(user==&hold_set_events);errno=ENOSPC;
    if(!std::strcmp(what,"hold_set"))++hold_set_events;
    else if(!std::strcmp(what,"hold_cleared"))++hold_cleared_events;
    else assert(!"unexpected beta event");
}
static unsigned storage_calls;
static bool storage_revokes;
static void send_storage(void* user,const void* storage) {
    assert(user==&hold_set_events && storage==expected_session);
    ++storage_calls;errno=ENOTTY;
    if(storage_revokes)invalidate();
}
static void session_reader(const void* storage,mx5::runtime::session_trace::Snapshot* out,void*) {
    assert(storage==expected_session);++session_reads;errno=EIO;
    const mx5::runtime::session_trace::Snapshot s={mx5::runtime::session_trace::OBSERVED,77,9,-2,true,100};*out=s;
}
static R::Result request_reader(const void* position, R::Trace* out, void*) {
    assert(position==expected_position);++trace_reads;
    *out=R::Trace();out->request.id=43;out->worker.id=51;
    out->reply.type=2;out->reply.type_known=true;
    errno=EBUSY;return trace_result;
}
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
    if (beta_fixture) {
        s.profile_verified = s.input_quality_verified = s.limits_ok = false;
        s.travel_bearing_deg = 123.456789; s.accuracy_m = 24.0001; s.beta = true;
    }
    if (speed_fixture) {
        // A speed-only candidate: no position, bearing or accuracy claim.
        s.latitude_deg = s.longitude_deg = s.travel_bearing_deg = 0;
        s.accuracy_m = 0; s.derived_utc_ns = 0; s.speed_only = true;
    }
    if (mutate_snapshot) mutate_snapshot(s);
    return s;
}
static int32_t fake_next(void* session, VehicleData* data) {
    assert(session == expected_session); assert(errno == next_expected_errno);
    ++calls;
    if (expect_original) assert(data == original_wrapper);
    else assert(data != original_wrapper);
    if (data && data->payload && data->length == 48) std::memcpy(sent, data->payload, 48);
    errno = EDOM; return next_result;
}
static void sink(const Observation* e, void*) {
    ++events; last_event = *e;
    if (e->kind == Observation::POSITION && publish_on_position &&
        (e->original_mode == 0 || (speed_fixture && e->position_class == POSITION_NO_FIX_STALE)))
        assert(publish_snapshot(fixture()));
    errno = EBUSY; // The shim must hide this side effect from OEM code.
}
static uint64_t clock_fn(void*) { errno = EAGAIN; return clock_ns; }
static bool provenance(void*, const PositionContext&, Provenance* out, void*) {
    out->source_epoch = 11; out->session_epoch = 12;
    out->exact_request = out->verified_lds = out->legacy_receiver = true;
    out->domain = provenance_domain; return true;
}
static bool beta_provenance(void*, const PositionContext&, Provenance* out, void*) {
    *out = Provenance(); out->source_epoch = 11; out->session_epoch = 12;
    out->domain = provenance_domain; return true;
}
static void put32(uint8_t* p, uint32_t v) {
    for (unsigned i=0;i<4;++i) p[i] = uint8_t(v >> (8*i));
}
static void putd(uint8_t* p, double v) {
    uint64_t b;std::memcpy(&b,&v,8);put32(p,uint32_t(b));put32(p+4,uint32_t(b>>32));
}
static uint32_t get32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1])<<8 | uint32_t(p[2])<<16 | uint32_t(p[3])<<24;
}
static void run_send(VehicleData& data, bool original) {
    original_wrapper=&data; expect_original=original; errno=17;
    const unsigned before=calls;
    assert(send_vehicle_data(expected_session,&data)==next_result);
    assert(calls==before+1); assert(errno==EDOM);
}
// One BETA send with original mode 0; checks the recorded reason.
static uint8_t beta_payload[48];
static void beta_send(void (*mutate)(DrSnapshot&), Reason expected) {
    mutate_snapshot=mutate;
    uint8_t position[72]={};VehicleData data={1,beta_payload,48};
    position_enter(0,position);
    run_send(data,expected!=PASS);
    position_leave();mutate_snapshot=0;
    if (last_event.reason!=expected) {
        std::fprintf(stderr,"reason %d expected %d\n",int(last_event.reason),int(expected));assert(false);
    }
    if (expected==PASS) assert(last_event.choice==BETA_REPLACEMENT);
    else {assert(last_event.choice==ORIGINAL);assert(!std::memcmp(sent,beta_payload,48));}
    for(unsigned i=0;i<48;++i)assert(beta_payload[i]==uint8_t(0xa0+i)); // OEM memory untouched
}
static void m_not_ready(DrSnapshot& s){s.ready=false;}
static void m_not_beta(DrSnapshot& s){s.beta=false;s.profile_verified=s.input_quality_verified=s.limits_ok=true;}
static void m_source_epoch(DrSnapshot& s){s.source_epoch=99;}
static void m_session_epoch(DrSnapshot& s){s.session_epoch=99;}
static void m_future(DrSnapshot& s){s.frontier_mono_ns=clock_ns+1;}
static void m_lease_over(DrSnapshot& s){s.valid_until_mono_ns=clock_ns-1;}
static void m_too_old(DrSnapshot& s){s.frontier_mono_ns=clock_ns-150000001;}
static void m_age_edge(DrSnapshot& s){s.frontier_mono_ns=clock_ns-150000000;s.valid_until_mono_ns=clock_ns;}
static void m_acc_399(DrSnapshot& s){s.accuracy_m=39.9;}
static void m_acc_400(DrSnapshot& s){s.accuracy_m=40.0;}
static void m_acc_401(DrSnapshot& s){s.accuracy_m=40.1;}
static void m_acc_nan(DrSnapshot& s){s.accuracy_m=std::numeric_limits<double>::quiet_NaN();}
static void m_acc_zero(DrSnapshot& s){s.accuracy_m=0;}
static void m_acc_neg(DrSnapshot& s){s.accuracy_m=-1;}
static void m_lat(DrSnapshot& s){s.latitude_deg=90.5;}
static void m_lon(DrSnapshot& s){s.longitude_deg=180.5;}
static void m_lon_nan(DrSnapshot& s){s.longitude_deg=std::numeric_limits<double>::quiet_NaN();}
static void m_speed_neg(DrSnapshot& s){s.speed_mps=-0.1;}
static void m_speed_absurd(DrSnapshot& s){s.speed_mps=100.5;}
static void m_speed_inf(DrSnapshot& s){s.speed_mps=std::numeric_limits<double>::infinity();}
static void m_bearing_360(DrSnapshot& s){s.travel_bearing_deg=360.0;}
static void m_bearing_neg(DrSnapshot& s){s.travel_bearing_deg=-0.5;}
static void m_bearing_nan(DrSnapshot& s){s.travel_bearing_deg=std::numeric_limits<double>::quiet_NaN();}
static void m_stopped(DrSnapshot& s){s.stopped=true;s.travel_bearing_deg=std::numeric_limits<double>::quiet_NaN();}
static void m_beta_qualified(DrSnapshot& s){s.beta=true;s.accuracy_m=10;}
static void m_speed_only(DrSnapshot& s){s.speed_only=true;}
static void m_qualified_speed_only(DrSnapshot& s){s.beta=false;s.speed_only=true;}
static void m_stopped_speed(DrSnapshot& s){s.stopped=true;}
static void m_not_speed_only(DrSnapshot& s){s.speed_only=false;s.latitude_deg=-37.1;s.longitude_deg=127.1;
    s.travel_bearing_deg=10;s.accuracy_m=20;}
// NO_FIX overlay fixture: the stored LOCATION (8.8 m accuracy, 4 km/h) built
// from the stored POSITION (mode 1, utc 0) with the same coordinates.
static uint8_t overlay_payload[48];
static const double STORED_LAT=37.5001234, STORED_LON=127.0412345;
static void make_overlay_payload(uint32_t speed_e3) {
    for(unsigned i=0;i<48;++i)overlay_payload[i]=uint8_t(0x30+i);
    put32(overlay_payload,0);put32(overlay_payload+4,0);
    put32(overlay_payload+8,uint32_t(int32_t(::round(STORED_LAT*1e7))));
    put32(overlay_payload+12,uint32_t(int32_t(::round(STORED_LON*1e7))));
    overlay_payload[16]=1;put32(overlay_payload+20,8800);
    overlay_payload[32]=1;put32(overlay_payload+36,speed_e3);
    overlay_payload[40]=1;put32(overlay_payload+44,335000000);
}
static Observation last_position;
static void overlay_send(void (*mutate)(DrSnapshot&), Reason expected, double lat=STORED_LAT,
                         int32_t mode=1, uint64_t utc=0) {
    mutate_snapshot=mutate;
    uint8_t position[72]={};put32(position,uint32_t(mode));
    put32(position+8,uint32_t(utc));put32(position+12,uint32_t(utc>>32));
    putd(position+16,lat);putd(position+24,STORED_LON);putd(position+40,335.0);putd(position+48,4.0);
    putd(position+56,4.4);putd(position+64,9.7);
    uint8_t before[48];std::memcpy(before,overlay_payload,48);
    VehicleData data={1,overlay_payload,48};
    position_enter(0,position);last_position=last_event;
    run_send(data,expected!=PASS);
    position_leave();mutate_snapshot=0;
    if (last_event.reason!=expected) {
        std::fprintf(stderr,"overlay reason %d expected %d\n",int(last_event.reason),int(expected));assert(false);
    }
    assert(!std::memcmp(overlay_payload,before,48)); // OEM memory untouched
    if (expected==PASS) {
        assert(last_event.choice==BETA_SPEED_OVERLAY);
        // Only hasSpeed (32) and speed (36..39) may differ.
        for(unsigned i=0;i<48;++i)
            if(i!=32 && !(i>=36 && i<40))assert(sent[i]==overlay_payload[i]);
        assert(sent[32]==1);
        assert(!std::memcmp(last_event.original,overlay_payload,48) && !std::memcmp(last_event.outgoing,sent,48));
    } else { assert(last_event.choice==ORIGINAL); assert(!std::memcmp(sent,overlay_payload,48)); }
}
static void check_beta_bytes(uint32_t accuracy_e3) {
    for(unsigned i=0;i<8;++i)assert(sent[i]==beta_payload[i]);            // timestamp original
    assert(int32_t(get32(sent+8))==int32_t(::round(-37.12345675*1e7)) && int32_t(get32(sent+12))==1271000000);
    assert(sent[16]==1);
    for(unsigned i=17;i<20;++i)assert(sent[i]==beta_payload[i]);           // padding original
    assert(get32(sent+20)==accuracy_e3);
    for(unsigned i=24;i<32;++i)assert(sent[i]==beta_payload[i]);           // altitude original
    assert(sent[32]==1);
    for(unsigned i=33;i<36;++i)assert(sent[i]==beta_payload[i]);
    assert(get32(sent+36)==12346);
    assert(sent[40]==1);
    for(unsigned i=41;i<44;++i)assert(sent[i]==beta_payload[i]);
    assert(get32(sent+44)==123456789);
}
static int beta_main(const char* test) {
    Options o = Options();o.sink=sink;o.clock=clock_fn;o.provenance=beta_provenance;
    o.max_snapshot_age_ns=150000000;o.allow_beta=true;o.user=&hold_set_events;
    o.beta_event=beta_event;o.session_reader=beta_session_reader;
    if(!std::strcmp(test,"beta_send_storage"))o.send_storage=send_storage;
    // The libpatch case: the installer declined session observation. Only
    // beta_undeclined leaves it false (installer observed sessions).
    o.sessions_declined=std::strcmp(test,"beta_undeclined")!=0;
    for(unsigned i=0;i<48;++i)beta_payload[i]=uint8_t(0xa0+i);
    next_result=0; // OEM success unless a case injects a failure
    static uint32_t session_handle=0xabcdef; expected_session=&session_handle;
    if (!std::strcmp(test,"beta_disallowed")) {
        o.allow_beta=false;assert(configure(fake_next,o));
        assert(!set_mode(BETA));assert(!set_mode(ASSIST));assert(mode()==OBSERVE);
        std::printf("PASS %s\n",test);return 0;
    }
    { Options bad=o;bad.clock=0;assert(!configure(fake_next,bad));
      bad=o;bad.provenance=0;assert(!configure(fake_next,bad));
      bad=o;bad.max_snapshot_age_ns=0;assert(!configure(fake_next,bad));
      // BETA_DECISIONS 3.6: allow_beta requires the hold event hook.
      bad=o;bad.beta_event=0;assert(!configure(fake_next,bad)); }
    assert(configure(fake_next,o));
    assert(!set_mode(ASSIST)); // BETA opt-in never opens the qualified gate.
    assert(set_mode(BETA)&&mode()==BETA);
    beta_fixture=true;publish_on_position=true;
    provenance_domain=Provenance::Domain::BETA;
    if (!std::strcmp(test,"beta_replace")) {
        beta_send(0,PASS);check_beta_bytes(24001);
        assert(last_event.has_payload && !std::memcmp(last_event.original,beta_payload,48));
        assert(!std::memcmp(last_event.outgoing,sent,48));
        assert(!hold_set_events && !hold_cleared_events && !beta_held());
        // UNOBSERVED (session observation declined) is accepted as well.
        beta_session_result=mx5::runtime::session_trace::UNOBSERVED;beta_send(0,PASS);
        beta_send(m_age_edge,PASS);
        beta_send(m_stopped,PASS);
        assert(sent[32]==1 && get32(sent+36)==0 && sent[40]==0 && get32(sent+44)==0 && sent[16]==1);
        for(unsigned i=0;i<8;++i)assert(sent[i]==beta_payload[i]);
        for(unsigned i=24;i<32;++i)assert(sent[i]==beta_payload[i]);
        // The qualified encoder is unchanged: it still builds a fresh buffer
        // without accuracy for QUALIFIED snapshots.
        DrSnapshot q=fixture();q.beta=false;uint8_t b[48];assert(encode_location(q,b));
        assert(b[16]==0 && get32(b+20)==0 && b[24]==0);
    } else if (!std::strcmp(test,"beta_accuracy")) {
        beta_send(m_acc_399,PASS);
        assert(get32(sent+20)==uint32_t(std::ceil(39.9*1000.0)) && get32(sent+20)>=39900);
        beta_send(m_acc_400,PASS);check_beta_bytes(40000);
        beta_send(m_acc_401,NOT_READY);
        beta_send(m_acc_nan,NOT_READY);
        beta_send(m_acc_zero,BAD_ENCODING);
        beta_send(m_acc_neg,BAD_ENCODING);
    } else if (!std::strcmp(test,"beta_branches")) {
        namespace S=mx5::runtime::session_trace;
        provenance_domain=Provenance::Domain::NONE;beta_send(0,BAD_PROVENANCE);
        provenance_domain=Provenance::Domain::QUALIFIED;beta_send(0,BAD_PROVENANCE);
        provenance_domain=Provenance::Domain::BETA;
        const S::Result rejected[]={S::NONE,S::TRANSITION,S::AMBIGUOUS,S::FAULT};
        for(unsigned i=0;i<4;++i){beta_session_result=rejected[i];beta_send(0,EPOCH_MISMATCH);}
        beta_session_result=S::OBSERVED;
        force_lock_busy=true;beta_send(0,LOCK_BUSY);force_lock_busy=false;
        beta_send(m_source_epoch,EPOCH_MISMATCH);beta_send(m_session_epoch,EPOCH_MISMATCH);
        { uint8_t position[72]={};VehicleData data={1,beta_payload,48};
          position_enter(0,position);invalidate();run_send(data,true);position_leave();
          assert(last_event.reason==EPOCH_MISMATCH && last_event.choice==ORIGINAL); }
        beta_send(m_not_ready,NOT_READY);beta_send(m_not_beta,NOT_READY);
        beta_send(m_future,EXPIRED);beta_send(m_lease_over,EXPIRED);beta_send(m_too_old,EXPIRED);
        beta_send(m_lat,BAD_ENCODING);beta_send(m_lon,BAD_ENCODING);beta_send(m_lon_nan,BAD_ENCODING);
        beta_send(m_speed_neg,BAD_ENCODING);beta_send(m_speed_absurd,BAD_ENCODING);
        beta_send(m_speed_inf,BAD_ENCODING);
        beta_send(m_bearing_360,BAD_ENCODING);beta_send(m_bearing_neg,BAD_ENCODING);
        beta_send(m_bearing_nan,BAD_ENCODING);
        beta_send(0,PASS);
        // Native FIX (modes 1/2 with an increasing utc) and native DR (3)
        // always pass the original. (Mode 1/2 with utc 0 is the NO_FIX class
        // since BETA_DECISIONS 1; see beta_overlay.)
        for(int32_t m=1;m<=3;++m) {
            uint8_t position[72]={};put32(position,uint32_t(m));put32(position+8,1700000000u+uint32_t(m));
            VehicleData data={1,beta_payload,48};
            position_enter(0,position);run_send(data,true);position_leave();
            assert(last_event.reason==NOT_UNKNOWN && !std::memcmp(sent,beta_payload,48));
            assert(last_event.position_class==(m==3?POSITION_NATIVE_DR:POSITION_FIX));
        }
        // NO_FIX with only a DR candidate: the overlay never takes it.
        { uint8_t position[72]={};put32(position,1);VehicleData data={1,beta_payload,48};
          position_enter(0,position);run_send(data,true);position_leave();
          assert(last_event.position_class==POSITION_NO_FIX_STALE && last_event.choice==ORIGINAL);
          assert(last_event.reason==OVERLAY_MISMATCH || last_event.reason==EPOCH_MISMATCH ||
                 last_event.reason==NOT_READY); }
        // Back to mode 0: the mode change revoked the old generation; the sink
        // republishes for the new one, so replacement resumes.
        beta_send(0,PASS);
        // No call context at all.
        { VehicleData data={1,beta_payload,48};run_send(data,true);assert(last_event.reason==NO_CONTEXT); }
        // A sticky fault (extra LOCATION) disables BETA.
        { uint8_t position[72]={};VehicleData data={1,beta_payload,48};
          position_enter(0,position);run_send(data,false);run_send(data,true);
          assert(last_event.reason==EXTRA_LOCATION);position_leave(); }
        publish_on_position=false; // publication is refused once faulted
        beta_send(0,DISABLED);assert(faulted());
    } else if (!std::strcmp(test,"beta_overlay")) {
        // BETA_DECISIONS 1-2: NO_FIX (mode 1/2, utc 0) gets only the wheel
        // speed; the stored position, bearing and 8.8 m accuracy stay original.
        speed_fixture=true;make_overlay_payload(1111);
        overlay_send(0,PASS);
        assert(last_position.position_class==POSITION_NO_FIX_STALE && last_event.position_class==POSITION_NO_FIX_STALE);
        assert(get32(sent+36)==12346 && get32(sent+20)==8800 && sent[16]==1);
        overlay_send(m_stopped_speed,PASS);assert(get32(sent+36)==0 && sent[32]==1);
        // Mode 2 is the same class.
        overlay_send(0,PASS,STORED_LAT,2);
        // The LOCATION is not the one built from this POSITION.
        overlay_send(0,OVERLAY_MISMATCH,STORED_LAT+2e-7);
        overlay_send(0,PASS,STORED_LAT+0.6e-7); // e7 rounding: one unit tolerated
        // Nothing to correct: original speed 0 and the wheels stand.
        make_overlay_payload(0);
        overlay_send(m_stopped_speed,OVERLAY_NOT_NEEDED);
        overlay_send(0,PASS);assert(get32(sent+36)==12346);
        make_overlay_payload(1111);
        // Candidate checks.
        overlay_send(m_not_speed_only,NOT_READY);
        overlay_send(m_not_ready,NOT_READY);
        overlay_send(m_qualified_speed_only,NOT_READY);
        overlay_send(m_source_epoch,EPOCH_MISMATCH);overlay_send(m_session_epoch,EPOCH_MISMATCH);
        overlay_send(m_future,EXPIRED);overlay_send(m_lease_over,EXPIRED);overlay_send(m_too_old,EXPIRED);
        overlay_send(m_speed_neg,BAD_ENCODING);overlay_send(m_speed_absurd,BAD_ENCODING);
        overlay_send(m_speed_inf,BAD_ENCODING);
        force_lock_busy=true;overlay_send(0,LOCK_BUSY);force_lock_busy=false;
        provenance_domain=Provenance::Domain::QUALIFIED;overlay_send(0,BAD_PROVENANCE);
        provenance_domain=Provenance::Domain::BETA;
        beta_session_result=mx5::runtime::session_trace::TRANSITION;overlay_send(0,EPOCH_MISMATCH);
        beta_session_result=mx5::runtime::session_trace::UNOBSERVED;overlay_send(0,PASS); // declined install
        beta_session_result=mx5::runtime::session_trace::OBSERVED;
        // A speed-only candidate never feeds the DR replacement (class LOST).
        speed_fixture=true;publish_on_position=false;
        { uint8_t position[72]={};VehicleData data={1,beta_payload,48};
          position_enter(0,position);
          assert(publish_snapshot(fixture()));
          run_send(data,true);position_leave();
          assert(last_event.position_class==POSITION_LOST && last_event.reason==NOT_READY); }
        publish_on_position=true;
        // NO_FIX -> FIX revokes: a candidate published for NO_FIX is gone.
        overlay_send(0,PASS);
        { const uint32_t g=generation();
          overlay_send(0,NOT_UNKNOWN,STORED_LAT,1,1700000100);
          assert(last_position.position_class==POSITION_FIX && generation()>g); }
        // FIX keeps its class within the same utc second; a stall > 3 s and a
        // backwards utc are UTC_STALL (original).
        clock_ns+=1000000000;overlay_send(0,NOT_UNKNOWN,STORED_LAT,1,1700000100);
        assert(last_position.position_class==POSITION_FIX);
        clock_ns+=2500000000ULL;overlay_send(0,NOT_UNKNOWN,STORED_LAT,1,1700000100);
        assert(last_position.position_class==POSITION_UTC_STALL);
        overlay_send(0,NOT_UNKNOWN,STORED_LAT,1,1700000101);assert(last_position.position_class==POSITION_FIX);
        overlay_send(0,NOT_UNKNOWN,STORED_LAT,1,1700000050);assert(last_position.position_class==POSITION_UTC_STALL);
        overlay_send(0,NOT_UNKNOWN,STORED_LAT,1,1700000051);assert(last_position.position_class==POSITION_FIX);
        // Native DR and undecodable modes pass the original.
        overlay_send(0,NOT_UNKNOWN,STORED_LAT,3);assert(last_position.position_class==POSITION_NATIVE_DR);
        overlay_send(0,NOT_UNKNOWN,STORED_LAT,7);assert(last_position.position_class==POSITION_UNDECODED);
        // Back to NO_FIX: the new generation's candidate overlays again.
        overlay_send(0,PASS);
        // Hold: a failed overlay send revokes and holds; HELD until an
        // ORIGINAL LOCATION returns 0.
        { const uint32_t g=generation();
          next_result=-3;overlay_send(0,PASS);
          assert(beta_held() && hold_set_events==1 && generation()>g);
          next_result=0;overlay_send(0,HELD);
          assert(!beta_held() && hold_cleared_events==1);
          overlay_send(0,PASS); }
        // Encoder unit checks.
        { DrSnapshot o=DrSnapshot();o.speed_mps=13.0;uint8_t out[48];
          assert(encode_speed_overlay(o,overlay_payload,out) && get32(out+36)==13000);
          for(unsigned i=0;i<48;++i)if(i!=32&&!(i>=36&&i<40))assert(out[i]==overlay_payload[i]);
          o.speed_mps=100.0;assert(encode_speed_overlay(o,overlay_payload,out));
          o.speed_mps=100.001;assert(!encode_speed_overlay(o,overlay_payload,out));
          o.speed_mps=-0.001;assert(!encode_speed_overlay(o,overlay_payload,out));
          o.speed_mps=std::numeric_limits<double>::quiet_NaN();assert(!encode_speed_overlay(o,overlay_payload,out));
          o.speed_mps=50;o.stopped=true;assert(encode_speed_overlay(o,overlay_payload,out) && get32(out+36)==0); }
    } else if (!std::strcmp(test,"beta_undeclined")) {
        // Sessions were not declined by the installer: an UNOBSERVED send
        // session means the fence is missing, so the original passes.
        beta_send(0,PASS);
        beta_session_result=mx5::runtime::session_trace::UNOBSERVED;beta_send(0,EPOCH_MISMATCH);
        beta_session_result=mx5::runtime::session_trace::OBSERVED;beta_send(0,PASS);
    } else if (!std::strcmp(test,"beta_send_storage")) {
        // BETA_DECISIONS 3.7: the storage fence is its own hook; the session
        // reader stays whatever the installer accepted.
        assert(!storage_calls);
        beta_send(0,PASS);assert(storage_calls==1 && session_reads==1);
        storage_revokes=true;beta_send(0,EPOCH_MISMATCH);assert(storage_calls==2);
        storage_revokes=false;
        { VehicleData other={3,beta_payload,48};original_wrapper=&other;expect_original=true;errno=17;
          assert(send_vehicle_data(expected_session,&other)==0 && errno==EDOM); }
        assert(storage_calls==3); // every send, LOCATION or not
        beta_send(0,PASS);assert(storage_calls==4);
    } else if (!std::strcmp(test,"beta_hold")) {
        const uint32_t before_fail=generation();
        next_result=-5;beta_send(0,PASS);                 // replaced send failed
        assert(beta_held() && hold_set_events==1 && !hold_cleared_events);
        // The adapter itself revoked the failed candidate's generation.
        assert(generation()>before_fail);
        { DrSnapshot stale=fixture();stale.prediction_generation=before_fail;
          assert(!publish_snapshot(stale)); }
        beta_send(0,HELD);assert(hold_set_events==1);      // original fails too: still held
        next_result=0;
        { VehicleData other={3,beta_payload,48};original_wrapper=&other;expect_original=true;errno=17;
          assert(send_vehicle_data(expected_session,&other)==0 && errno==EDOM); }
        assert(beta_held());                               // non-LOCATION success does not clear
        { uint8_t position[72]={};put32(position,1);VehicleData data={1,beta_payload,48};
          position_enter(0,position);run_send(data,true);position_leave(); }
        assert(!beta_held() && hold_cleared_events==1);    // ORIGINAL LOCATION returned 0
        beta_send(0,PASS);assert(!beta_held());            // replaced success keeps it clear
        next_result=7;beta_send(0,PASS);assert(beta_held() && hold_set_events==2);
        next_result=0;beta_send(0,HELD);assert(!beta_held() && hold_cleared_events==2);
        beta_send(0,PASS);
    } else return 2;
    std::printf("PASS %s\n",test);return 0;
}
int main(int argc,char** argv) {
    assert(argc==2);
    if (!std::strncmp(argv[1],"beta_",5)) return beta_main(argv[1]);
    Options o = Options();o.sink=sink;o.clock=clock_fn;o.provenance=provenance;
    o.allow_assist=true;o.max_snapshot_age_ns=150000000;
    if (!std::strcmp(argv[1],"request")) {
        o.request_reader=request_reader;o.provenance=0;o.allow_assist=false;o.session_reader=session_reader;
    }
    // Decision G is opt-in (SHADOW/BETA product options); small_payload_off
    // keeps the default.
    if (!std::strcmp(argv[1],"small_payload")) o.journal_gear_payload=true;
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
        for(unsigned m=1;m<=3;++m) {put32(position,m);position_enter(0,position);run_send(data,true);position_leave();assert(!std::memcmp(sent,payload,48));
            assert(last_event.reason==NOT_UNKNOWN);} // NO_FIX overlay is BETA only
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
    } else if (!std::strcmp(test,"assist_beta")) {
        // BETA is never reachable through the qualified ASSIST gate.
        assert(!set_mode(BETA));assert(set_mode(ASSIST));publish_on_position=true;
        provenance_domain=Provenance::Domain::BETA;position_enter(0,position);
        run_send(data,true);position_leave();assert(last_event.reason==BAD_PROVENANCE);
        provenance_domain=Provenance::Domain::QUALIFIED;mutate_snapshot=m_beta_qualified;position_enter(0,position);
        run_send(data,true);position_leave();assert(last_event.reason==NOT_READY);
        mutate_snapshot=m_speed_only;position_enter(0,position);
        run_send(data,true);position_leave();assert(last_event.reason==NOT_READY);
        mutate_snapshot=0;position_enter(0,position);run_send(data,false);position_leave();
        assert(last_event.choice==DR_REPLACEMENT && sent[16]==0);
        next_result=-1;position_enter(0,position);run_send(data,false);position_leave();
        assert(!beta_held()); // the BETA hold is not a qualified-path mechanism
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
    } else if (!std::strcmp(test,"request")) {
        expected_position=position;errno=17;
        assert(!set_mode(ASSIST));
        position_enter(0,position);assert(errno==17 && trace_reads==1);
        assert(last_event.request_result==R::OK && last_event.request_trace.request.id==43);
        assert(!last_event.provenance.exact_request);
        run_send(data,true);position_leave();
        assert(last_event.request_trace.request.id==43 && last_event.request_trace.worker.id==51);
        assert(last_event.request_trace.reply.type==2 && last_event.choice==ORIGINAL);
        assert(session_reads==1 && last_event.send_session.lifetime==77);
        assert(last_event.send_session.state_known && last_event.send_session.state==-2);
        assert(!last_event.request_trace.issue.session_lifetime);
        run_send(data,true);
        assert(last_event.request_result==R::NOT_FOUND && !last_event.request_trace.request.id);
        trace_result=R::STALE;
        position_enter(0,position);run_send(data,true);position_leave();
        assert(last_event.request_result==R::STALE && !last_event.request_trace.request.id);
        assert(!std::memcmp(sent,payload,48) && trace_reads==2);
    } else if (!std::strcmp(test,"small_payload")) {
        // Decision G: only the AA GEAR shape (type 8, exactly 4 bytes) is
        // copied for the journal; the OEM call, result and errno are unchanged.
        uint8_t gear[4]={0x01,0x02,0x03,0x04};VehicleData g={8,gear,4};
        run_send(g,true);assert(last_event.small_length==4 && !std::memcmp(last_event.small_payload,gear,4));
        assert(!last_event.has_payload && last_event.choice==ORIGINAL);
        next_result=-3;run_send(g,true);next_result=0;      // non-zero OEM result passes through
        assert(last_event.small_length==4 && last_event.result==-3);
        uint8_t big[17]={};VehicleData b={3,big,17};
        run_send(b,true);assert(!last_event.small_length);
        uint8_t sixteen[16];for(unsigned i=0;i<16;++i)sixteen[i]=uint8_t(0xf0+i);
        VehicleData x={3,sixteen,16};run_send(x,true);assert(!last_event.small_length);   // other types: none
        VehicleData y={3,sixteen,4};run_send(y,true);assert(!last_event.small_length);
        VehicleData z={8,sixteen,3};run_send(z,true);assert(!last_event.small_length);    // type 8, not 4 bytes
        VehicleData w={8,sixteen,5};run_send(w,true);assert(!last_event.small_length);
        VehicleData none={8,0,4};run_send(none,true);assert(!last_event.small_length);
        position_enter(0,position);run_send(data,true);position_leave();
        assert(!last_event.small_length && last_event.has_payload);  // LOCATION keeps its own copy
        // Mode OFF emits nothing and copies nothing.
        assert(set_mode(OFF));const unsigned before=events;
        run_send(g,true);assert(events==before);
        assert(set_mode(OBSERVE));
    } else if (!std::strcmp(test,"small_payload_off")) {
        // Without the SHADOW/BETA opt-in (OBSERVE/SCRUB configurations) the
        // send path copies nothing, and the OEM contract is unchanged.
        uint8_t gear[4]={0x01,0x02,0x03,0x04};VehicleData g={8,gear,4};
        run_send(g,true);assert(!last_event.small_length && last_event.type==8 && last_event.length==4);
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
