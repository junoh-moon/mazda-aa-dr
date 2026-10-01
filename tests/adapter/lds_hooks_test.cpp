#include "adapter/lds_hooks.h"
#include "adapter/bus_hooks.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

namespace A=mx5::adapter;
namespace S=mx5::runtime::lds_sideband;
namespace L=mx5::sensors::lds_lineage;
namespace B=mx5::runtime::bus_trace;
namespace R=mx5::runtime::request_trace;
extern "C" {
int lds_test_read_rmc(void*); extern char lds_test_read_rmc_return;
int lds_test_read_gga(void*); extern char lds_test_read_gga_return;
int lds_test_read_gsa(void*); extern char lds_test_read_gsa_return;
int lds_test_update_rmc(const void*); extern char lds_test_update_rmc_return;
int lds_test_update_gga(const void*); extern char lds_test_update_gga_return;
int lds_test_update_gsa(const void*); extern char lds_test_update_gsa_return;
void lds_test_lock_read(void*,const char*,unsigned); extern char lds_test_lock_read_return;
void lds_test_lock_update(void*,const char*,unsigned); extern char lds_test_lock_update_return;
void lds_test_lock_service(void*,const char*,unsigned); extern char lds_test_lock_service_return;
void lds_test_unlock_read(void*); extern char lds_test_unlock_read_return;
void lds_test_unlock_update(void*); extern char lds_test_unlock_update_return;
void lds_test_unlock_service(void*); extern char lds_test_unlock_service_return;
void* lds_test_copy_read(void*,const void*,unsigned); extern char lds_test_copy_read_return;
void* lds_test_copy_update(void*,const void*,unsigned); extern char lds_test_copy_update_return;
}
namespace {
struct Cache {
    int32_t mode,padding; uint64_t utc;
    float latitude,longitude; int32_t altitude;
    float heading,velocity,horizontal,vertical; uint32_t tail;
};
static_assert(sizeof(Cache)==48,"Authored cache follows verified original copy width");
Cache cache;
pthread_mutex_t mutex;
bool held;
A::LdsCallback callbacks[12];
unsigned callback_calls,registrations,id6_effect,initializes,clears,sends;
unsigned other_calls;
bool inline_callback;
int registration_entry_errno;
bool nested_gsa,wrong_read_buffer,missing_read,throw_rmc,cancel_rmc,write_after_snapshot;
bool cancel_cleaned;
bool send_other_connection,send_failure,build_failure,nested_path,throw_path;
bool initialize_query,initialize_throw;
bool endpoint_available;
unsigned endpoint_matches;
A::EndpointMatch endpoint_before=A::ENDPOINT_MATCH,endpoint_after=A::ENDPOINT_MATCH;
Cache retained_read;
uint64_t tick=100;
S::Record records[32]; unsigned record_count;
A::LdsGeneric dispatch;
void* dispatch_descriptor;
int connection,other_connection,method,reply,object;
struct Message { int type; uint32_t serial,reply_serial;const char *sender,*destination,*path,*interface_name,*member; };
Message request={1,7,0,":1.2",":1.1","/com/jci/lds/data","com.jci.lds.data","GetPosition"};
Message nested_request={1,8,0,":1.3",":1.1","/com/jci/lds/data","com.jci.lds.data","GetPosition"};
Message response={2,0,7,":1.1",":1.2",0,0,0};
uint64_t clock_ns(void*) { errno=ERANGE;return ++tick; }
void emit(const S::Record& value,void*) {
    assert(!held);assert(record_count<32);records[record_count++]=value;errno=EDOM;
}
void initialize() {
    assert(!pthread_mutex_init(&mutex,0));memset(&cache,0,sizeof cache);++initializes;
    if(initialize_query)assert(mx5_lds_path(&connection,&request,&connection)==0);
    if(initialize_throw)throw 21;
    errno=ENOTTY;
}
void clear() { assert(!pthread_mutex_destroy(&mutex));++clears;errno=EFBIG; }
int32_t driver_open(uintptr_t word) { assert(word==0x1234);memset(callbacks,0,sizeof callbacks);errno=ENOSPC;return 104; }
int32_t registration(uint32_t id,A::LdsCallback callback) {
    registration_entry_errno=errno;
    ++registrations;if(id==6)++id6_effect;
    if(id>=12 || !callback || callbacks[id]) { errno=ENOSYS;return 104; }
    callbacks[id]=callback;
    if(inline_callback)callback(0);
    errno=EILSEQ;return 100;
}
void lock(void* m,const char*,unsigned) { assert(m==&mutex);assert(!pthread_mutex_lock(&mutex));assert(!held);held=true;errno=ENOTSUP; }
void unlock(void* m) { assert(m==&mutex && held);held=false;assert(!pthread_mutex_unlock(&mutex));errno=ENOMSG; }
void* copy(void* d,const void* s,unsigned n) { return memcpy(d,s,n); }
int32_t read_current(void* d) {
    if(!d)return 120;
    lds_test_lock_read(&mutex,"fixture",1);lds_test_copy_read(d,&cache,48);lds_test_unlock_read(&mutex);
    errno=EOVERFLOW;return 100;
}
int32_t update_current(const void* s) {
    if(!s)return 120;
    lds_test_lock_update(&mutex,"fixture",2);lds_test_copy_update(&cache,s,48);lds_test_unlock_update(&mutex);
    errno=ENODATA;return 100;
}
void rmc(void*) {
    ++callback_calls;Cache p=Cache();
    Cache* destination=(throw_rmc||cancel_rmc)?&retained_read:&p;
    if(!missing_read)assert(lds_test_read_rmc(destination)==100);
    if(throw_rmc)throw 17;
    if(cancel_rmc) { assert(!pthread_cancel(pthread_self()));pthread_testcancel();assert(false); }
    if(nested_gsa)callbacks[2](0);
    p.mode=1;p.utc=123;p.latitude=12.5f;p.longitude=34.5f;p.heading=67;p.velocity=89;
    if(wrong_read_buffer) { Cache different=p;assert(lds_test_update_rmc(&different)==100); }
    else assert(lds_test_update_rmc(&p)==100);
    errno=E2BIG;
}
void gga(void*) {
    ++callback_calls;Cache p;assert(lds_test_read_gga(&p)==100);
    p.mode=2;p.latitude=15.5f;p.longitude=37.5f;p.altitude=-42;
    assert(lds_test_update_gga(&p)==100);errno=E2BIG;
}
void gsa(void*) {
    ++callback_calls;Cache p;assert(lds_test_read_gsa(&p)==100);
    p.mode=3;p.horizontal=1.2f;p.vertical=2.3f;
    assert(lds_test_update_gsa(&p)==100);errno=E2BIG;
}
void other(void* value) { assert(value==&object);++other_calls;errno=EAGAIN; }
void service(void*,int32_t* mode,uint64_t* utc,double* lat,double* lon,int32_t* alt,
             double* heading,double* velocity,double* horizontal,double* vertical) {
    lds_test_lock_service(&mutex,"fixture",3);
    *mode=cache.mode;*utc=cache.utc;*lat=cache.latitude;*lon=cache.longitude;*alt=cache.altitude;
    *heading=cache.heading;*velocity=cache.velocity;*horizontal=cache.horizontal;*vertical=cache.vertical;
    lds_test_unlock_service(&mutex);errno=EPIPE;
}
int32_t generic(void* c,void* m,void* r,void* d) {
    assert(c==&connection && m==&method && r==&reply);
    const A::LdsDescriptor* descriptor=static_cast<const A::LdsDescriptor*>(d);
    A::PositionInput p=A::PositionInput();
    descriptor->service(c,&p.mode,&p.utc_seconds,&p.latitude_deg,&p.longitude_deg,&p.altitude_m,
        &p.heading_deg,&p.velocity_kmh,&p.horizontal,&p.vertical);
    if(nested_path) {
        nested_path=false;
        assert(mx5_lds_path(&connection,&nested_request,&connection)==0);
    }
    if(write_after_snapshot) { write_after_snapshot=false;callbacks[4](0); }
    assert(p.utc_seconds==cache.utc);errno=EIO;return 0;
}
A::LdsDescriptor descriptor={"GetPosition",service,generic,0};
int32_t set_callback(void* o,A::LdsGeneric callback,void* d) { assert(o==&object);dispatch=callback;dispatch_descriptor=d;errno=ESPIPE;return 13; }
bool request_pointer(void* p) { return p==&request || p==&nested_request; }
void* method_build(void* raw) { assert(request_pointer(raw));errno=ECONNREFUSED;return build_failure?0:&method; }
void* reply_create(void* m,void* raw) { assert(m==&method && request_pointer(raw));errno=ENOBUFS;return &reply; }
void* reply_message(void* r,void* raw) {
    assert(r==&reply && request_pointer(raw));
    response.reply_serial=static_cast<Message*>(raw)->serial;
    response.destination=static_cast<Message*>(raw)->sender;
    errno=EHOSTUNREACH;return &response;
}
int32_t send(void* raw,void* message,uint32_t* serial) {
    assert(raw==(send_other_connection?&other_connection:&connection) && message==&response);
    ++sends;response.serial=send_failure?0:response.reply_serial+12;
    if(serial)*serial=response.serial;
    errno=ETIMEDOUT;return send_failure?0:1;
}
int32_t path(void* raw,void* message,void* c) {
    assert(raw==&connection && request_pointer(message) && c==&connection);
    void* m=mx5_lds_method_build(message);
    if(!m) { errno=EACCES;return 17; }
    void* r=mx5_lds_reply_create(m,message);
    assert(dispatch(c,m,r,dispatch_descriptor)==0);
    if(throw_path)throw 19;
    void* response_message=mx5_lds_reply_message(r,message);
    assert(mx5_lds_send(send_other_connection?&other_connection:raw,response_message,0)==(send_failure?0:1));
    errno=EACCES;return 0;
}
int32_t message_type(void* p) { return static_cast<Message*>(p)->type; }
uint32_t message_serial(void* p) { return static_cast<Message*>(p)->serial; }
uint32_t message_reply_serial(void* p) { return static_cast<Message*>(p)->reply_serial; }
const char* message_sender(void* p) { return static_cast<Message*>(p)->sender; }
const char* message_destination(void* p) { return static_cast<Message*>(p)->destination; }
const char* message_path(void* p) { return static_cast<Message*>(p)->path; }
const char* message_interface(void* p) { return static_cast<Message*>(p)->interface_name; }
const char* message_member(void* p) { return static_cast<Message*>(p)->member; }
A::LdsBindings bindings() {
    A::LdsBindings b=A::LdsBindings();
    b.initialize=initialize;b.clear=clear;b.driver_open=driver_open;b.registration=registration;
    b.read=read_current;b.update=update_current;b.lock=lock;b.unlock=unlock;b.copy=copy;
    b.set_callback=set_callback;b.generic=generic;b.service=service;b.path=path;
    b.method_build=method_build;b.reply_create=reply_create;b.reply_message=reply_message;b.send=send;
    b.descriptor=&descriptor;b.current_cache=&cache;b.current_mutex=&mutex;b.clock=clock_ns;b.emit=emit;
    b.raw.type=message_type;b.raw.serial=message_serial;b.raw.reply_serial=message_reply_serial;
    b.raw.sender=message_sender;b.raw.destination=message_destination;b.raw.path=message_path;
    b.raw.interface_name=message_interface;b.raw.member=message_member;
    const A::LdsRoute r={rmc,0x06f,uintptr_t(&lds_test_read_rmc_return),uintptr_t(&lds_test_update_rmc_return)};
    const A::LdsRoute a={gga,0x01d,uintptr_t(&lds_test_read_gga_return),uintptr_t(&lds_test_update_gga_return)};
    const A::LdsRoute s={gsa,0x181,uintptr_t(&lds_test_read_gsa_return),uintptr_t(&lds_test_update_gsa_return)};
    for(unsigned i=0;i<12;++i)if(i<10)b.routes[i]=(i==4 || i==8)?a:((i==2 || i==3 || i==5 || i==9)?s:r);
    b.sites.read_lock=uintptr_t(&lds_test_lock_read_return);b.sites.read_copy=uintptr_t(&lds_test_copy_read_return);b.sites.read_unlock=uintptr_t(&lds_test_unlock_read_return);
    b.sites.update_lock=uintptr_t(&lds_test_lock_update_return);b.sites.update_copy=uintptr_t(&lds_test_copy_update_return);b.sites.update_unlock=uintptr_t(&lds_test_unlock_update_return);
    b.sites.service_lock=uintptr_t(&lds_test_lock_service_return);b.sites.service_unlock=uintptr_t(&lds_test_unlock_service_return);
    return b;
}
void query() { assert(mx5_lds_path(&connection,&request,&connection)==0);assert(errno==EACCES); }
}
// This focused fixture supplies only the already-tested bus reader boundary.
// Original-runtime integration links actual hidden bus_hooks, not these stubs.
namespace mx5 { namespace adapter {
B::Snapshot read_bus_endpoint(const void* object,R::Endpoint* e,uintptr_t* key) {
    assert(object==&connection);
    if(e)*e=R::Endpoint();
    if(key)*key=0;
    B::Snapshot out=B::Snapshot();out.result=B::UNOBSERVED;
    if(endpoint_available) {
        out.result=B::CONNECTED;out.object=3;out.lifetime=11;
        if(e) {
            S::copy_text(&e->server_guid,"authored-server-address-guid");
            S::copy_text(&e->unique_name,":1.1");
        }
        if(key)*key=uintptr_t(&connection);
    }
    errno=EAGAIN;return out;
}
EndpointMatch bus_endpoint_matches(const void* object,const B::Snapshot&,uintptr_t key) {
    assert(object==&connection && key==uintptr_t(&connection));
    errno=ERANGE;
    if(!endpoint_available)return ENDPOINT_UNAVAILABLE;
    return endpoint_matches++%2?endpoint_after:endpoint_before;
}
} }
void initialize_observer() {
    assert(A::prepare_lds_hooks(bindings()));assert(A::activate_lds_hooks());
    mx5_lds_initialize();assert(initializes==1 && errno==ENOTTY);
    assert(mx5_lds_set_callback(&object,generic,&descriptor)==13 && errno==ESPIPE);
}
void chain() {
    initialize_observer();
    assert(mx5_lds_register(0,rmc)==100 && errno==EILSEQ);
    callbacks[0](0);assert(callback_calls==1 && errno==E2BIG);
    query();assert(sends==1);
    assert(record_count==1); // RED: forwarding alone loses actual response lineage.
    assert((records[0].flags&S::SNAPSHOT_KNOWN)!=0);
    assert(records[0].field_lineage.lifetime && records[0].field_lineage.write_sequence==1);
    assert(records[0].field_lineage.fields[L::UTC].write_sequence==1);
    assert(records[0].field_lineage.fields[L::ALTITUDE].write_sequence==0);
    assert(records[0].position.utc_seconds==123 && records[0].position.latitude_deg==12.5);
    assert(records[0].wire.request_serial==7 && records[0].wire.response_serial==19 && records[0].wire.reply_serial==7);
    mx5_lds_clear();assert(clears==1 && errno==EFBIG);
}
void preparation() {
    errno=E2BIG;assert(!A::activate_lds_hooks() && errno==E2BIG);
    A::LdsBindings invalid=bindings();invalid.registration=0;
    assert(!A::prepare_lds_hooks(invalid));
    assert(A::prepare_lds_hooks(bindings()));
    A::LdsBindings replacement=bindings();replacement.routes[0].callback=other;
    assert(!A::prepare_lds_hooks(replacement));
    assert(A::activate_lds_hooks());
    assert(!A::activate_lds_hooks());
}
void inactive() {
    assert(A::prepare_lds_hooks(bindings()));
    mx5_lds_initialize();
    assert(mx5_lds_set_callback(&object,generic,&descriptor)==13);
    errno=EBUSY;
    assert(mx5_lds_register(0,rmc)==100 && errno==EILSEQ);
    assert(registration_entry_errno==EBUSY && callbacks[0]==rmc);
    callbacks[0](0);query();
    assert(callback_calls==1 && sends==1 && record_count==0);
    mx5_lds_clear();assert(errno==EFBIG);
}
void registration_contract(bool inline_case) {
    initialize_observer();inline_callback=inline_case;
    errno=EBUSY;
    assert(mx5_lds_register(0,rmc)==100 && errno==EILSEQ);
    assert(registration_entry_errno==EBUSY && registrations==1);
    assert(callbacks[0] && callbacks[0]!=rmc); // RED: real stored callback needs the immutable observer route.
    if(inline_case)assert(callback_calls==1);
    else { callbacks[0](0);assert(callback_calls==1 && errno==E2BIG); }
    inline_callback=false;
    const A::LdsCallback first=callbacks[0];
    assert(mx5_lds_register(0,other)==104 && errno==ENOSYS && callbacks[0]==first);
    assert(mx5_lds_register(6,rmc)==100 && errno==EILSEQ);
    assert(callbacks[6] && callbacks[6]!=rmc && callbacks[6]!=first);
    assert(mx5_lds_register(6,rmc)==104 && errno==ENOSYS && id6_effect==2);
    assert(mx5_lds_register(12,rmc)==104 && errno==ENOSYS);
    assert(mx5_lds_register(10,0)==104 && errno==ENOSYS);
    assert(mx5_lds_register(10,other)==100 && callbacks[10]==other);
    callbacks[10](&object);assert(other_calls==1 && errno==EAGAIN);
    assert(registrations==7 && record_count==0);
    query();assert(record_count==1 && (records[0].flags&S::SNAPSHOT_KNOWN));
    assert(records[0].field_lineage.write_sequence==1);
    mx5_lds_clear();
}
void retained_callback() {
    initialize_observer();
    assert(mx5_lds_register(0,rmc)==100);
    A::LdsCallback retained=callbacks[0];
    assert(retained!=rmc);
    // This original Open clears its table and fails. 104 must not rebind or
    // destroy a callable that the original caller already obtained.
    assert(mx5_lds_driver_open(0x1234)==104 && errno==ENOSPC);
    assert(!callbacks[0]);
    assert(mx5_lds_register(0,other)==100 && callbacks[0]==other);
    retained(0);assert(callback_calls==1 && !other_calls && errno==E2BIG);
    callbacks[0](&object);assert(other_calls==1 && callback_calls==1);
    query();assert(record_count==1 && records[0].field_lineage.write_sequence==1);
    mx5_lds_clear();
}
void register_three() {
    assert(mx5_lds_register(0,rmc)==100);
    assert(mx5_lds_register(2,gsa)==100);
    assert(mx5_lds_register(4,gga)==100);
}
void assert_unknown(const S::Record& record) {
    for(unsigned i=0;i<L::FIELD_COUNT;++i)assert(!record.field_lineage.fields[i].write_sequence);
}
void read_copy_race() {
    initialize_observer();register_three();nested_gsa=true;
    callbacks[0](0);query();
    assert(callback_calls==2 && record_count==1);
    const S::Record& r=records[0];
    assert(r.field_lineage.write_sequence==2 && r.position.horizontal==0);
    assert(r.field_lineage.fields[L::UTC].write_sequence==2);
    assert(r.field_lineage.fields[L::HORIZONTAL].write_sequence==0);
    mx5_lds_clear();
}
void snapshot_and_equal_assignment() {
    initialize_observer();register_three();callbacks[0](0);
    write_after_snapshot=true;query();
    assert(record_count==1 && records[0].field_lineage.write_sequence==1);
    assert(records[0].position.latitude_deg==12.5 && records[0].position.altitude_m==0);
    query();assert(record_count==2 && records[1].field_lineage.write_sequence==2);
    assert(records[1].position.latitude_deg==15.5 && records[1].position.altitude_m==-42);
    query();assert(record_count==3 && records[2].field_lineage.write_sequence==2);
    callbacks[4](0);query();
    assert(record_count==4 && records[3].field_lineage.write_sequence==3);
    assert(records[3].position.latitude_deg==records[2].position.latitude_deg);
    assert(records[3].field_lineage.fields[L::LATITUDE].write_sequence==3);
    assert(records[0].field_lineage.fields[L::UTC].write_sequence==1);
    mx5_lds_clear();
}
void unavailable_read(bool wrong_pointer) {
    initialize_observer();register_three();
    wrong_read_buffer=wrong_pointer;missing_read=!wrong_pointer;
    callbacks[0](0);query();
    assert(record_count==1 && records[0].position.utc_seconds==123);
    assert(records[0].field_lineage.write_sequence==1);assert_unknown(records[0]);
    mx5_lds_clear();
}
void cache_lifetime() {
    initialize_observer();register_three();callbacks[0](0);query();
    assert(record_count==1 && records[0].field_lineage.lifetime);
    const uint64_t old=records[0].field_lineage.lifetime;
    const A::LdsCallback retained=callbacks[0];
    mx5_lds_clear();mx5_lds_initialize();query();
    assert(record_count==2 && records[1].field_lineage.lifetime>old);
    assert(records[1].field_lineage.write_sequence==0);assert_unknown(records[1]);
    retained(0);query();
    assert(record_count==3 && records[2].field_lineage.lifetime==records[1].field_lineage.lifetime);
    assert(records[2].field_lineage.write_sequence==1);
    mx5_lds_clear();
}
void late_activation() {
    assert(A::prepare_lds_hooks(bindings()));mx5_lds_initialize();
    assert(A::activate_lds_hooks());
    assert(mx5_lds_set_callback(&object,generic,&descriptor)==13);
    register_three();callbacks[0](0);query();
    assert(record_count==1 && records[0].position.utc_seconds==123);
    assert(records[0].field_lineage.lifetime==0);assert_unknown(records[0]);
    mx5_lds_clear();
}
void unwind_callback() {
    initialize_observer();register_three();throw_rmc=true;
    try { callbacks[0](0);assert(false); } catch(int value) { assert(value==17); }
    throw_rmc=false;
    // Exact verified update PC + same buffer after unwind must not reuse the
    // departed callback's read. No original write or query is suppressed.
    assert(lds_test_update_rmc(&retained_read)==100);query();
    assert(record_count==1 && records[0].field_lineage.write_sequence==1);
    assert_unknown(records[0]);
    callbacks[0](0);query();assert(record_count==2 && records[1].field_lineage.write_sequence==2);
    assert(records[1].field_lineage.fields[L::UTC].write_sequence==2);
    mx5_lds_clear();
}
void cancellation_cleanup(void*) {
    assert(lds_test_update_rmc(&retained_read)==100);query();cancel_cleaned=true;
}
void* cancelled_callback(void*) {
    pthread_cleanup_push(cancellation_cleanup,0);
    callbacks[0](0);
    pthread_cleanup_pop(0);
    return 0;
}
void cancellation() {
    initialize_observer();register_three();cancel_rmc=true;
    pthread_t thread;assert(!pthread_create(&thread,0,cancelled_callback,0));
    void* result=0;assert(!pthread_join(thread,&result)&&result==PTHREAD_CANCELED);
    assert(cancel_cleaned && record_count==1 && records[0].field_lineage.write_sequence==1);
    assert_unknown(records[0]);mx5_lds_clear();
}
void chain_mismatch() {
    initialize_observer();register_three();callbacks[0](0);
    send_other_connection=true;query();
    assert(sends==1 && record_count==1);
    const unsigned observed=S::SNAPSHOT_KNOWN|S::REQUEST_KNOWN|S::REPLY_KNOWN|
        S::RAW_SEND_CALLED|S::RAW_SEND_SUCCEEDED|S::PATH_RETURNED|S::CHAIN_CONFLICT;
    assert(records[0].flags==observed); // RED: a real contradiction must not erase the raw send.
    assert(records[0].wire.response_serial==19 && records[0].wire.reply_serial==7);
    assert(records[0].position.utc_seconds==123);mx5_lds_clear();
}
void endpoint(bool post_lost) {
    initialize_observer();register_three();callbacks[0](0);endpoint_available=true;
    if(post_lost)endpoint_after=A::ENDPOINT_MISMATCH;
    query();assert(record_count==1 && !(records[0].flags&S::CHAIN_CONFLICT));
    assert(records[0].wire.server_guid.known==!post_lost);
    assert(records[0].wire.server_unique.known==!post_lost);
    assert(records[0].wire.response_serial==19 && records[0].wire.client_unique.known);
    if(!post_lost) {
        assert(!strcmp(records[0].wire.server_guid.bytes,"authored-server-address-guid"));
        assert(!strcmp(records[0].wire.server_unique.bytes,":1.1"));
    }
    endpoint_available=false;query();
    assert(record_count==2 && !records[1].wire.server_guid.known);
    assert(records[0].wire.server_guid.known==!post_lost);mx5_lds_clear();
}
void nested_paths() {
    initialize_observer();register_three();callbacks[0](0);nested_path=true;
    query();assert(record_count==2 && sends==2);
    assert(records[0].wire.request_serial==8 && records[0].wire.reply_serial==8);
    assert(records[0].wire.response_serial==20 && !strcmp(records[0].wire.client_unique.bytes,":1.3"));
    assert(records[1].wire.request_serial==7 && records[1].wire.reply_serial==7);
    assert(records[1].wire.response_serial==19 && !strcmp(records[1].wire.client_unique.bytes,":1.2"));
    assert(!(records[0].flags&S::CHAIN_CONFLICT) && !(records[1].flags&S::CHAIN_CONFLICT));
    query();assert(record_count==3 && records[2].wire.request_serial==7);
    assert(records[0].wire.request_serial==8);mx5_lds_clear();
}
void failed_chain(bool failed_build) {
    initialize_observer();register_three();callbacks[0](0);
    send_failure=!failed_build;build_failure=failed_build;
    assert(mx5_lds_path(&connection,&request,&connection)==(failed_build?17:0));
    assert(errno==EACCES && record_count==1);
    assert((records[0].flags&S::PATH_RETURNED) && !(records[0].flags&S::RAW_SEND_SUCCEEDED));
    if(failed_build)assert(records[0].flags==(S::REQUEST_KNOWN|S::PATH_RETURNED) && sends==0);
    else assert((records[0].flags&S::RAW_SEND_CALLED) && (records[0].flags&S::REPLY_KNOWN) && sends==1);
    send_failure=build_failure=false;query();
    assert(record_count==2 && (records[1].flags&S::RAW_SEND_SUCCEEDED));mx5_lds_clear();
}
void path_unwind() {
    initialize_observer();register_three();callbacks[0](0);throw_path=true;
    try { query();assert(false); } catch(int value) { assert(value==19); }
    assert(record_count==0 && sends==0);
    throw_path=false;query();assert(record_count==1 && sends==1);
    assert((records[0].flags&S::SNAPSHOT_KNOWN) && !(records[0].flags&S::CHAIN_CONFLICT));mx5_lds_clear();
}
void all_ids() {
    initialize_observer();const A::LdsBindings b=bindings();
    for(unsigned i=0;i<10;++i) {
        assert(mx5_lds_register(i,b.routes[i].callback)==100);
        assert(callbacks[i]!=b.routes[i].callback);
        for(unsigned j=0;j<i;++j)assert(callbacks[i]!=callbacks[j]);
        callbacks[i](0);
    }
    for(unsigned i=10;i<12;++i) {
        assert(mx5_lds_register(i,other)==100 && callbacks[i]==other);
        callbacks[i](&object);
    }
    query();assert(callback_calls==10 && other_calls==2 && registrations==12 && id6_effect==1);
    assert(record_count==1 && records[0].field_lineage.write_sequence==10);mx5_lds_clear();
}
void lifecycle_unknown(bool unwind) {
    assert(A::prepare_lds_hooks(bindings()));assert(A::activate_lds_hooks());
    assert(mx5_lds_set_callback(&object,generic,&descriptor)==13);
    initialize_query=!unwind;initialize_throw=unwind;
    if(unwind) {
        try { mx5_lds_initialize();assert(false); } catch(int value) { assert(value==21); }
    } else mx5_lds_initialize();
    initialize_query=initialize_throw=false;query();
    assert(record_count==(unwind?1u:2u));
    assert(records[record_count-1].field_lineage.lifetime==0);
    mx5_lds_clear();mx5_lds_initialize();query();
    assert(records[record_count-1].field_lineage.lifetime>0);
    assert(records[record_count-1].field_lineage.write_sequence==0);mx5_lds_clear();
}
void emit_hook_record() {
    initialize_observer();register_three();callbacks[0](0);endpoint_available=true;
    query();assert(record_count==1);
    // Only transport-assigned instance/sequence and receive diagnostics are
    // authored here. All lineage, wire fields, flags and position scalars came
    // through the actual product wrappers above, including original Path=0.
    S::Record record=records[0];record.source_instance=41;record.sequence=1;
    S::Diagnostic diagnostic=S::Diagnostic();
    diagnostic.sender_pid=222;diagnostic.sender_uid=0;diagnostic.received_ns=1000;
    char line[S::JSON_CAPACITY];assert(S::format_record(line,sizeof line,record,diagnostic));
    puts(line);mx5_lds_clear();
}
int main(int argc,char** argv) {
    const char* test=argc==2?argv[1]:"chain";
    if(!strcmp(test,"--emit")) { emit_hook_record();return 0; }
    if(!strcmp(test,"chain"))chain();
    else if(!strcmp(test,"prepare"))preparation();
    else if(!strcmp(test,"inactive"))inactive();
    else if(!strcmp(test,"register"))registration_contract(false);
    else if(!strcmp(test,"inline"))registration_contract(true);
    else if(!strcmp(test,"retained"))retained_callback();
    else if(!strcmp(test,"read_copy"))read_copy_race();
    else if(!strcmp(test,"snapshot"))snapshot_and_equal_assignment();
    else if(!strcmp(test,"missing_read"))unavailable_read(false);
    else if(!strcmp(test,"wrong_pointer"))unavailable_read(true);
    else if(!strcmp(test,"lifetime"))cache_lifetime();
    else if(!strcmp(test,"late"))late_activation();
    else if(!strcmp(test,"unwind"))unwind_callback();
    else if(!strcmp(test,"cancel"))cancellation();
    else if(!strcmp(test,"chain_mismatch"))chain_mismatch();
    else if(!strcmp(test,"endpoint"))endpoint(false);
    else if(!strcmp(test,"endpoint_post"))endpoint(true);
    else if(!strcmp(test,"nested_path"))nested_paths();
    else if(!strcmp(test,"failed_send"))failed_chain(false);
    else if(!strcmp(test,"failed_build"))failed_chain(true);
    else if(!strcmp(test,"path_unwind"))path_unwind();
    else if(!strcmp(test,"all_ids"))all_ids();
    else if(!strcmp(test,"initialize_overlap"))lifecycle_unknown(false);
    else if(!strcmp(test,"initialize_unwind"))lifecycle_unknown(true);
    else return 2;
    printf("LDS hooks: %s PASS\n",test);
}
