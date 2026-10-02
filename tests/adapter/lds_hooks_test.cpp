#include "adapter/lds_hooks.h"
#include "adapter/bus_hooks.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <stddef.h>
#include <atomic>

namespace A=mx5::adapter;
namespace S=mx5::runtime::lds_sideband;
namespace L=mx5::sensors::lds_lineage;
namespace B=mx5::runtime::bus_trace;
namespace R=mx5::runtime::request_trace;
namespace T=mx5::sensors::nmea_course_token;
extern "C" {
uint32_t lds_test_parse(char*,void*,uint32_t); extern char lds_test_parse_return;
int lds_test_checkpoint(int,fd_set*,fd_set*,fd_set*,timeval*); extern char lds_test_checkpoint_return;
void lds_test_dispatch(A::LdsCallback,void*); extern char lds_test_dispatch_return;
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
void lds_test_message_lock(void*); extern char lds_test_message_lock_return;
int32_t lds_test_native_lock(void*); extern char lds_test_native_lock_return;
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
bool course_test;
bool repeat_rmc,repeat_copy;
unsigned parser_calls,checkpoint_calls;
std::atomic<unsigned> driver_closes(0);
bool close_block;
pthread_barrier_t close_entered,close_release;
uint32_t parser_result,parser_heading,parser_word;
char* parser_input;
void* parser_workspace;
int parser_entry_errno;
void (*parser_action)();
void (*rmc_action)();
void (*gga_action)();
void (*update_action)();
uint32_t parse_sentence(char* input,void* workspace,uint32_t word) {
    ++parser_calls;parser_entry_errno=errno;
    assert(input==parser_input && workspace==parser_workspace && word==parser_word);
    const uint32_t result=parser_result,heading=parser_heading;
    void (*action)()=parser_action;parser_action=0;
    if(action)action();
    if(!result || result==6)*static_cast<uint32_t*>(workspace)=heading;
    errno=EILSEQ;return result;
}
int checkpoint(int n,fd_set* r,fd_set* w,fd_set* e,timeval* t) {
    ++checkpoint_calls;assert(n==0 && !r && !w && !e && t);
    assert(t->tv_sec==3 && t->tv_usec==7);errno=EINTR;return -1;
}
void barrier(pthread_barrier_t* b) {
    const int r=pthread_barrier_wait(b);assert(!r || r==PTHREAD_BARRIER_SERIAL_THREAD);
}
void driver_close() {
    const unsigned ordinal=driver_closes.fetch_add(1);
    if(close_block && !ordinal) { barrier(&close_entered);barrier(&close_release); }
    errno=ECHILD;
}
unsigned endpoint_matches;
A::EndpointMatch endpoint_before=A::ENDPOINT_MATCH,endpoint_after=A::ENDPOINT_MATCH;
Cache retained_read;
uint64_t tick=100;
S::Record records[32]; unsigned record_count;
A::LdsGeneric dispatch;
void* dispatch_descriptor;
pthread_mutex_t send_mutex=PTHREAD_MUTEX_INITIALIZER,other_mutex=PTHREAD_MUTEX_INITIALIZER;
struct Connection { uintptr_t padding;void* mutex; };
Connection connection={0,&send_mutex},other_connection={0,&other_mutex};
int method,reply,object;
uint32_t current_generation=1,initialized_generation=1;
bool locked_test,send_held,skip_native,native_wrong_pc,message_wrong_pc,native_failure;
bool native_twice,message_twice,generation_change,mutex_change,throw_send,extra_send;
bool bad_reply,bad_destination,bad_endpoint,clock_missing,bad_type,nested_send;
unsigned native_calls,message_locks,locked_count,invalidations;
A::LdsLockedLoss last_loss;
A::LdsLockedSend locked_values[8];
int32_t native_mutex_lock(void* value) {
    ++native_calls;assert(value==connection.mutex);
    if(native_failure) { errno=ENOTTY;return EBUSY; }
    if(!send_held) { assert(!pthread_mutex_lock(static_cast<pthread_mutex_t*>(value)));send_held=true; }
    errno=ENOTTY;return 0;
}
void message_lock(void* value);
void published(const A::LdsLockedSend& value,void*) {
    assert(send_held && !held && locked_count<8);
    assert(pthread_mutex_trylock(&send_mutex)==EBUSY);
    locked_values[locked_count++]=value;errno=ENFILE;
}
void invalidated(A::LdsLockedLoss loss,void*) { ++invalidations;last_loss=loss;errno=ENFILE; }
struct Message { int type; uint32_t serial,reply_serial;const char *sender,*destination,*path,*interface_name,*member; };
Message request={1,7,0,":1.2",":1.1","/com/jci/lds/data","com.jci.lds.data","GetPosition"};
Message nested_request={1,8,0,":1.3",":1.1","/com/jci/lds/data","com.jci.lds.data","GetPosition"};
Message response={2,0,7,":1.1",":1.2",0,0,0};
void message_lock(void* value) { assert(value==&response);++message_locks;errno=ECHRNG; }
uint64_t clock_ns(void*) { errno=ERANGE;return clock_missing?0:++tick; }
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
    lds_test_lock_update(&mutex,"fixture",2);
    if(update_action) { void (*action)()=update_action;update_action=0;action(); }
    lds_test_copy_update(&cache,s,48);
    if(repeat_copy)lds_test_copy_update(&cache,s,48);
    lds_test_unlock_update(&mutex);
    errno=ENODATA;return 100;
}
void rmc(void* workspace) {
    ++callback_calls;Cache p=Cache();
    Cache* destination=(throw_rmc||cancel_rmc)?&retained_read:&p;
    if(!missing_read)assert(lds_test_read_rmc(destination)==100);
    if(throw_rmc)throw 17;
    if(cancel_rmc) { assert(!pthread_cancel(pthread_self()));pthread_testcancel();assert(false); }
    if(nested_gsa)callbacks[2](0);
    if(rmc_action) { void (*action)()=rmc_action;rmc_action=0;action(); }
    p.mode=1;p.utc=123;p.latitude=12.5f;p.longitude=34.5f;p.heading=67;p.velocity=89;
    if(course_test)p.heading=float(*static_cast<uint32_t*>(workspace));
    if(wrong_read_buffer) { Cache different=p;assert(lds_test_update_rmc(&different)==100); }
    else assert(lds_test_update_rmc(&p)==100);
    if(repeat_rmc) { assert(lds_test_read_rmc(&p)==100);assert(lds_test_update_rmc(&p)==100); }
    errno=E2BIG;
}
void gga(void*) {
    ++callback_calls;Cache p;assert(lds_test_read_gga(&p)==100);
    if(gga_action) { void (*action)()=gga_action;gga_action=0;action(); }
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
    ++sends;response.serial=(!locked_test && send_failure)?0:response.reply_serial+12;
    if(serial)*serial=response.serial;
    if(locked_test) {
        const bool threaded=current_generation==initialized_generation && connection.mutex &&
            uintptr_t(connection.mutex)!=0xabcdef;
        if(threaded && !skip_native) {
            const int32_t result=native_wrong_pc?mx5_lds_native_mutex_lock(connection.mutex):lds_test_native_lock(connection.mutex);
            assert(result==(native_failure?EBUSY:0) && errno==ENOTTY);
            if(native_twice)assert(lds_test_native_lock(connection.mutex)==0);
        }
        if(generation_change)++current_generation;
        void* saved_mutex=connection.mutex;
        if(mutex_change)connection.mutex=&other_mutex;
        if(bad_reply)response.reply_serial+=1;
        if(bad_destination)response.destination=":1.999";
        if(bad_type)response.type=3;
        if(nested_send) { nested_send=false;assert(mx5_lds_send(raw,message,0)==1); }
        if(message_wrong_pc)mx5_lds_message_lock(message);else lds_test_message_lock(message);
        assert(errno==ECHRNG);
        if(message_twice)lds_test_message_lock(message);
        connection.mutex=saved_mutex;
        if(send_held) { send_held=false;assert(!pthread_mutex_unlock(static_cast<pthread_mutex_t*>(saved_mutex))); }
        if(throw_send)throw 23;
    }
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
    if(extra_send)assert(mx5_lds_send(raw,response_message,0)==1);
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
    b.message_lock=message_lock;b.native_mutex_lock=native_mutex_lock;
    b.input=A::LdsInputBindings{parse_sentence,checkpoint,driver_close,
        uintptr_t(&lds_test_parse_return),uintptr_t(&lds_test_checkpoint_return),uintptr_t(&lds_test_dispatch_return)};
    b.send_sites=A::LdsSendSites{uintptr_t(&lds_test_message_lock_return),uintptr_t(&lds_test_native_lock_return),
        &current_generation,&initialized_generation,offsetof(Connection,mutex),0xabcdef};
    if(locked_test) { b.publish_locked=published;b.invalidate_locked=invalidated; }
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
            if(bad_endpoint)e->server_guid.complete=false;
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
    invalid=bindings();invalid.message_lock=0;assert(!A::prepare_lds_hooks(invalid));
    invalid=bindings();invalid.native_mutex_lock=0;assert(!A::prepare_lds_hooks(invalid));
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
void locked_case(const char* scenario) {
    locked_test=true;endpoint_available=true;
    if(!strcmp(scenario,"locked_pair")) {
        A::LdsBindings b=bindings();b.invalidate_locked=0;
        assert(!A::prepare_lds_hooks(b));return;
    }
    if(!strcmp(scenario,"locked_inactive")) {
        assert(A::prepare_lds_hooks(bindings()));mx5_lds_initialize();
        assert(mx5_lds_set_callback(&object,generic,&descriptor)==13);query();
        assert(native_calls==1 && message_locks==1 && sends==1 && !locked_count && !record_count && !invalidations);
        mx5_lds_clear();return;
    }
    initialize_observer();register_three();callbacks[0](0);
    const bool unknown=!strcmp(scenario,"locked_unknown");
    if(unknown) { mx5_lds_clear();mx5_lds_initialize(); }
    if(!strcmp(scenario,"locked_unthreaded")) { initialized_generation=0;connection.mutex=reinterpret_cast<void*>(0xabcdef); }
    if(!strcmp(scenario,"locked_null"))connection.mutex=0;
    if(!strcmp(scenario,"locked_generation"))generation_change=true;
    if(!strcmp(scenario,"locked_mutex"))mutex_change=true;
    if(!strcmp(scenario,"locked_native_pc"))native_wrong_pc=true;
    if(!strcmp(scenario,"locked_message_pc"))message_wrong_pc=true;
    if(!strcmp(scenario,"locked_native_fail"))native_failure=true;
    if(!strcmp(scenario,"locked_no_native"))skip_native=true;
    if(!strcmp(scenario,"locked_native_twice"))native_twice=true;
    if(!strcmp(scenario,"locked_message_twice"))message_twice=true;
    if(!strcmp(scenario,"locked_reply"))bad_reply=true;
    if(!strcmp(scenario,"locked_destination"))bad_destination=true;
    if(!strcmp(scenario,"locked_endpoint"))bad_endpoint=true;
    if(!strcmp(scenario,"locked_failed_send"))send_failure=true;
    if(!strcmp(scenario,"locked_unwind"))throw_send=true;
    if(!strcmp(scenario,"locked_extra_send"))extra_send=true;
    if(!strcmp(scenario,"locked_clock"))clock_missing=true;
    if(!strcmp(scenario,"locked_type"))bad_type=true;
    if(!strcmp(scenario,"locked_zero_request"))request.serial=0;
    if(!strcmp(scenario,"locked_endpoint_changed"))endpoint_after=A::ENDPOINT_MISMATCH;
    if(!strcmp(scenario,"locked_snapshot"))write_after_snapshot=true;
    if(!strcmp(scenario,"locked_nested_send"))nested_send=true;
    const bool nested=!strcmp(scenario,"locked_nested_send");
    if(throw_send) { try {query();assert(false);}catch(int e){assert(e==23);} }
    else query();
    const bool positive=!strcmp(scenario,"locked") || unknown || message_twice || send_failure || throw_send || extra_send ||
        clock_missing || !strcmp(scenario,"locked_snapshot");
    assert(locked_count==unsigned(positive));
    if(positive) {
        const A::LdsLockedSend& p=locked_values[0];
        assert(p.stage==mx5::runtime::lds_association::LOCKED_FOR_SEND && p.reply_type==2 &&
            (clock_missing?p.observed_ns==0:p.observed_ns>0));
        assert(p.wire.request_serial==7 && p.wire.reply_serial==7 && p.wire.response_serial==19);
        assert(!strcmp(p.wire.server_guid.bytes,"authored-server-address-guid") && !strcmp(p.wire.client_unique.bytes,":1.2"));
        assert(!strcmp(p.wire.server_unique.bytes,":1.1") && !strcmp(p.wire.destination.bytes,":1.2"));
        assert(p.field_lineage.write_sequence==(unknown?0u:1u));
        assert(p.position.utc_seconds==(unknown?0u:123u));
        if(!strcmp(scenario,"locked_snapshot"))assert(p.position.latitude_deg==12.5 && cache.latitude==15.5f);
        assert(invalidations==unsigned(message_twice||send_failure||throw_send||extra_send));
        if(send_failure)assert(last_loss==A::LOCKED_SEND_FAILED);
    }
    assert(sends==(extra_send||nested?2u:1u));
    assert(message_locks==(message_twice||extra_send||nested?2u:1u));
    if(!throw_send)assert(record_count==1 && (records[0].flags&S::RAW_SEND_CALLED));
    mx5_lds_clear();
}
void parse_input(char* input,uint32_t* workspace,uint32_t result=0,uint32_t heading=0,bool wrong_caller=false) {
    parser_input=input;parser_workspace=workspace;parser_word=777;
    parser_result=result;parser_heading=heading;
    const unsigned before=parser_calls;errno=EBUSY;
    const uint32_t returned=wrong_caller?mx5_lds_parse_sentence(input,workspace,parser_word):
        lds_test_parse(input,workspace,parser_word);
    assert(returned==result);
    assert(parser_calls==before+1 && parser_entry_errno==EBUSY && errno==EILSEQ);
}
void checkpoint_input() {
    timeval timeout={3,7};const unsigned before=checkpoint_calls;
    assert(lds_test_checkpoint(0,0,0,0,&timeout)==-1);
    assert(checkpoint_calls==before+1 && errno==EINTR && timeout.tv_sec==3 && timeout.tv_usec==7);
}
void course_values() {
    course_test=locked_test=endpoint_available=true;initialize_observer();
    assert(mx5_lds_driver_open(0x1234)==104 && errno==ENOSPC);register_three();
    char empty[]="$GPRMC,120000,A,3500,N,13500,E,10,,011026,,,A*00";
    char zero[]="$GPRMC,120001,A,3500,N,13500,E,10,0,011026,,,A*00";
    uint32_t workspace=37;
    parse_input(empty,&workspace);lds_test_dispatch(callbacks[0],&workspace);query();
    assert(record_count==1 && locked_count==1 && cache.heading==0);
    assert(records[0].field_lineage.write_sequence==1 && records[0].field_lineage.fields[L::HEADING].write_sequence==1);
    assert(records[0].field_lineage.heading_presence==T::EMPTY);
    assert(locked_values[0].field_lineage.heading_presence==T::EMPTY);
    checkpoint_input();
    parse_input(zero,&workspace);lds_test_dispatch(callbacks[0],&workspace);query();
    assert(cache.heading==0 && records[1].field_lineage.write_sequence==2);
    assert(records[1].field_lineage.heading_presence==T::PRESENT);
    assert(locked_values[1].field_lineage.heading_presence==T::PRESENT);
    // A rejected parse really ran, but no callback/cache write follows it.
    parse_input(empty,&workspace,14);checkpoint_input();query();
    assert(records[2].field_lineage.write_sequence==2 && records[2].field_lineage.heading_presence==T::PRESENT);
    mx5_lds_driver_close();assert(errno==ECHILD && driver_closes==1);
    mx5_lds_clear();
}
void course_bindings() {
    // Each newly published data slot must have a forwardable immutable target
    // and each observation must have its exact supported call site.
    for(unsigned i=0;i<6;++i) {
        A::LdsBindings b=bindings();
        if(i==0)b.input.parse_sentence=0;
        if(i==1)b.input.select=0;
        if(i==2)b.input.driver_close=0;
        if(i==3)b.input.parse_return=0;
        if(i==4)b.input.select_return=0;
        if(i==5)b.input.dispatch_return=0;
        errno=E2BIG;assert(!A::prepare_lds_hooks(b) && errno==E2BIG);
    }
    assert(A::prepare_lds_hooks(bindings()));
}
void course_nested_callback() {
    course_test=true;initialize_observer();register_three();nested_gsa=true;
    char frame[]="$GPRMC,120000,A,3500,N,13500,E,10,,011026,,,A*00";
    uint32_t workspace=42;
    parse_input(frame,&workspace);lds_test_dispatch(callbacks[0],&workspace);query();
    assert(callback_calls==2 && cache.heading==0);
    assert(records[0].field_lineage.write_sequence==2);
    assert(records[0].field_lineage.fields[L::HEADING].write_sequence==2);
    assert(records[0].field_lineage.heading_presence==T::UNKNOWN);
    nested_gsa=false;checkpoint_input();
    parse_input(frame,&workspace);lds_test_dispatch(callbacks[0],&workspace);query();
    assert(records[1].field_lineage.heading_presence==T::EMPTY);
    mx5_lds_clear();
}
void* close_thread(void*) { mx5_lds_driver_close();assert(errno==ECHILD);return 0; }
void course_boundary_overlap() {
    course_test=true;initialize_observer();register_three();
    close_block=true;
    assert(!pthread_barrier_init(&close_entered,0,2));
    assert(!pthread_barrier_init(&close_release,0,2));
    pthread_t thread;assert(!pthread_create(&thread,0,close_thread,0));
    barrier(&close_entered); // First original Close is still executing.
    mx5_lds_driver_close();mx5_lds_driver_close();
    char frame[]="$GPRMC,120000,A,3500,N,13500,E,10,,011026,,,A*00";
    uint32_t workspace=42;
    parse_input(frame,&workspace);lds_test_dispatch(callbacks[0],&workspace);query();
    assert(callback_calls==1 && records[0].field_lineage.write_sequence==1);
    assert(records[0].field_lineage.heading_presence==T::UNKNOWN);
    barrier(&close_release);assert(!pthread_join(thread,0));
    close_block=false;assert(driver_closes==3);
    // A later isolated boundary and checkpoint can establish a new observation
    // epoch. The overlapping callbacks themselves remain ordinary forwards.
    mx5_lds_driver_close();checkpoint_input();
    parse_input(frame,&workspace);lds_test_dispatch(callbacks[0],&workspace);query();
    assert(records[1].field_lineage.heading_presence==T::EMPTY);
    assert(!pthread_barrier_destroy(&close_entered));
    assert(!pthread_barrier_destroy(&close_release));mx5_lds_clear();
}
void presence(T::Presence expected) {
    query();assert(record_count && records[record_count-1].field_lineage.heading_presence==expected);
}
void course_identity() {
    course_test=true;initialize_observer();register_three();
    char empty[]="$GPRMC,120000,A,3500,N,13500,E,10,,011026,,,A*00";
    char zero[]="$GPRMC,120001,A,3500,N,13500,E,10,0,011026,,,A*00";
    uint32_t a=37,b=41;
    parse_input(empty,&a);callbacks[0](&a);presence(T::UNKNOWN); // Wrong dispatch PC.
    checkpoint_input();
    parse_input(empty,&a,0,0,true);lds_test_dispatch(callbacks[0],&a);presence(T::UNKNOWN);
    checkpoint_input();
    parse_input(empty,&a);lds_test_dispatch(callbacks[0],&b);presence(T::UNKNOWN);
    assert(cache.heading==41);checkpoint_input();
    parse_input(empty,&a);lds_test_dispatch(callbacks[2],&a); // Wrong route consumes uncertainty.
    lds_test_dispatch(callbacks[0],&a);presence(T::UNKNOWN);checkpoint_input();
    parse_input(empty,&a);lds_test_dispatch(callbacks[0],&a);presence(T::EMPTY);
    lds_test_dispatch(callbacks[0],&a);presence(T::UNKNOWN); // One claim only.
    parse_input(zero,&a);lds_test_dispatch(callbacks[0],&a);presence(T::PRESENT); // Same address, new call.
    parse_input(empty,&a);parse_input(zero,&a); // Unconsumed input must not become latest-wins.
    lds_test_dispatch(callbacks[0],&a);presence(T::UNKNOWN);
    timeval timeout={3,7};assert(mx5_lds_select(0,0,0,0,&timeout)==-1); // Wrong checkpoint PC.
    parse_input(empty,&a);lds_test_dispatch(callbacks[0],&a);presence(T::UNKNOWN);
    checkpoint_input();parse_input(empty,&a);lds_test_dispatch(callbacks[0],&a);presence(T::EMPTY);
    parse_input(empty,&a);checkpoint_input();lds_test_dispatch(callbacks[0],&a);presence(T::UNKNOWN);
    mx5_lds_clear();
}
char action_frame[]="$GPRMC,120002,A,3500,N,13500,E,10,0,011026,,,A*00";
uint32_t action_workspace;
void inner_parse() {
    parser_input=action_frame;parser_workspace=&action_workspace;parser_result=0;parser_heading=0;parser_word=99;
    assert(lds_test_parse(action_frame,&action_workspace,99)==0 && errno==EILSEQ);
    lds_test_dispatch(callbacks[0],&action_workspace);
}
void parse_throw() { throw 31; }
void parse_cancel() { assert(!pthread_cancel(pthread_self()));pthread_testcancel();assert(false); }
void cancelled_parse_cleanup(void*) {
    lds_test_dispatch(callbacks[0],&action_workspace);presence(T::UNKNOWN);cancel_cleaned=true;
}
void* cancelled_parse(void*) {
    pthread_cleanup_push(cancelled_parse_cleanup,0);
    parse_input(action_frame,&action_workspace);
    pthread_cleanup_pop(0);return 0;
}
void course_parser_control(const char* name) {
    course_test=true;initialize_observer();register_three();
    char frame[]="$GPRMC,120000,A,3500,N,13500,E,10,,011026,,,A*00";
    uint32_t workspace=37;
    if(!strcmp(name,"course_cancel")) {
        parser_action=parse_cancel;pthread_t thread;void* result=0;
        assert(!pthread_create(&thread,0,cancelled_parse,0));
        assert(!pthread_join(thread,&result) && result==PTHREAD_CANCELED && cancel_cleaned);
    } else if(!strcmp(name,"course_unwind")) {
        parser_action=parse_throw;
        try { parse_input(frame,&workspace);assert(false); } catch(int value) { assert(value==31); }
        lds_test_dispatch(callbacks[0],&workspace);presence(T::UNKNOWN);
    } else {
        parser_action=inner_parse;
        parser_input=frame;parser_workspace=&workspace;parser_word=777;parser_result=0;parser_heading=0;
        const unsigned before=parser_calls;assert(lds_test_parse(frame,&workspace,777)==0 && errno==EILSEQ);
        assert(parser_calls==before+2);
        lds_test_dispatch(callbacks[0],&workspace);presence(T::UNKNOWN);
        assert(callback_calls==2);
    }
    checkpoint_input();parse_input(frame,&workspace);lds_test_dispatch(callbacks[0],&workspace);presence(T::EMPTY);
    mx5_lds_clear();
}
void course_lifecycle(const char* name) {
    course_test=true;initialize_observer();register_three();
    char frame[]="$GPRMC,120000,A,3500,N,13500,E,10,,011026,,,A*00";
    uint32_t workspace=37;const A::LdsCallback retained=callbacks[0];
    parse_input(frame,&workspace);
    if(!strcmp(name,"course_open")) { assert(mx5_lds_driver_open(0x1234)==104 && errno==ENOSPC);register_three(); }
    else if(!strcmp(name,"course_reset")) { mx5_lds_clear();mx5_lds_initialize(); }
    else if(!strcmp(name,"course_write_boundary"))update_action=mx5_lds_driver_close;
    else mx5_lds_driver_close();
    lds_test_dispatch(retained,&workspace);presence(T::UNKNOWN);
    checkpoint_input();parse_input(frame,&workspace);lds_test_dispatch(retained,&workspace);presence(T::EMPTY);
    mx5_lds_clear();
}
void* independent_parse(void*) {
    parse_input(action_frame,&action_workspace);lds_test_dispatch(callbacks[0],&action_workspace);return 0;
}
void newer_heading_write() {
    pthread_t thread;assert(!pthread_create(&thread,0,independent_parse,0));assert(!pthread_join(thread,0));
    presence(T::PRESENT);
}
void course_saved_read() {
    course_test=true;initialize_observer();register_three();
    char frame[]="$GPRMC,120000,A,3500,N,13500,E,10,,011026,,,A*00";
    uint32_t workspace=37;
    parse_input(frame,&workspace);lds_test_dispatch(callbacks[0],&workspace);presence(T::EMPTY);
    gga_action=newer_heading_write;callbacks[4](0);presence(T::EMPTY);
    assert(record_count==3 && records[1].field_lineage.write_sequence==2 && records[2].field_lineage.write_sequence==3);
    assert(records[2].field_lineage.fields[L::HEADING].write_sequence==1 && cache.heading==0);
    mx5_lds_clear();
}
void course_routes() {
    course_test=true;initialize_observer();register_three();
    assert(mx5_lds_register(1,rmc)==100 && mx5_lds_register(6,rmc)==100);
    char frame[]="$GNRMC,120000,A,3500,N,13500,E,10,,011026,,,A*00";
    uint32_t workspace=37;
    parse_input(frame,&workspace,6);lds_test_dispatch(callbacks[6],&workspace);presence(T::EMPTY);
    // The numeric result is a route, not generic success. An authored mismatch
    // between the lexical RMC frame and another original sentence ID cannot
    // turn that other route into evidence about an RMC course token.
    parse_input(frame,&workspace,1);lds_test_dispatch(callbacks[1],&workspace);presence(T::UNKNOWN);
    assert(records[1].field_lineage.fields[L::HEADING].write_sequence==2);
    mx5_lds_clear();
}
void course_callback_parse() {
    course_test=true;initialize_observer();register_three();
    char frame[]="$GPRMC,120000,A,3500,N,13500,E,10,,011026,,,A*00";
    uint32_t workspace=37;
    parse_input(frame,&workspace);rmc_action=inner_parse;
    lds_test_dispatch(callbacks[0],&workspace);presence(T::UNKNOWN);
    assert(callback_calls==2 && records[0].field_lineage.write_sequence==2);
    checkpoint_input();parse_input(frame,&workspace);lds_test_dispatch(callbacks[0],&workspace);presence(T::EMPTY);
    // An input epoch change during the original parser also invalidates the
    // earlier lexical observation, even when the parser returns normally.
    parser_action=mx5_lds_driver_close;parse_input(frame,&workspace);
    lds_test_dispatch(callbacks[0],&workspace);presence(T::UNKNOWN);
    mx5_lds_clear();
}
void course_inactive() {
    course_test=true;assert(A::prepare_lds_hooks(bindings()));mx5_lds_initialize();
    assert(mx5_lds_set_callback(&object,generic,&descriptor)==13);
    register_three();assert(callbacks[0]==rmc);
    char frame[]="$GPRMC,120000,A,3500,N,13500,E,10,,011026,,,A*00";
    uint32_t workspace=37;
    parse_input(frame,&workspace);lds_test_dispatch(callbacks[0],&workspace);checkpoint_input();
    query();assert(!record_count && sends==1 && cache.heading==0);
    mx5_lds_driver_close();assert(driver_closes==1 && errno==ECHILD);
    mx5_lds_clear();assert(errno==EFBIG);
}
void course_one_commit(bool repeated_copy) {
    course_test=true;initialize_observer();register_three();
    repeat_copy=repeated_copy;repeat_rmc=!repeated_copy;
    char frame[]="$GPRMC,120000,A,3500,N,13500,E,10,,011026,,,A*00";
    uint32_t workspace=37;
    parse_input(frame,&workspace);lds_test_dispatch(callbacks[0],&workspace);presence(T::UNKNOWN);
    assert(callback_calls==1 && records[0].field_lineage.write_sequence==2 && cache.heading==0);
    repeat_copy=repeat_rmc=false;
    parse_input(frame,&workspace);lds_test_dispatch(callbacks[0],&workspace);presence(T::EMPTY);
    mx5_lds_clear();
}
int main(int argc,char** argv) {
    const char* test=argc==2?argv[1]:"chain";
    if(!strcmp(test,"--emit")) { emit_hook_record();return 0; }
    if(!strcmp(test,"course_values"))course_values();
    else if(!strcmp(test,"course_bindings"))course_bindings();
    else if(!strcmp(test,"course_nested_callback"))course_nested_callback();
    else if(!strcmp(test,"course_boundary_overlap"))course_boundary_overlap();
    else if(!strcmp(test,"course_identity"))course_identity();
    else if(!strcmp(test,"course_parse_nested") || !strcmp(test,"course_unwind") || !strcmp(test,"course_cancel"))course_parser_control(test);
    else if(!strcmp(test,"course_open") || !strcmp(test,"course_close") || !strcmp(test,"course_reset") || !strcmp(test,"course_write_boundary"))course_lifecycle(test);
    else if(!strcmp(test,"course_saved_read"))course_saved_read();
    else if(!strcmp(test,"course_routes"))course_routes();
    else if(!strcmp(test,"course_callback_parse"))course_callback_parse();
    else if(!strcmp(test,"course_inactive"))course_inactive();
    else if(!strcmp(test,"course_one_commit"))course_one_commit(false);
    else if(!strcmp(test,"course_copy_twice"))course_one_commit(true);
    else if(!strncmp(test,"locked",6))locked_case(test);
    else if(!strcmp(test,"chain"))chain();
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
