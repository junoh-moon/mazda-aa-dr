// Authored callback ABI test; does not load or call any OEM library.
#define main session_probe_unused_main
#include "oem_session_probe.cpp"
#undef main
#include <assert.h>

#ifdef NDEBUG
#error Session callback regression requires assertions
#endif

namespace {
const Callbacks* retained[2];
Callbacks expected;
unsigned submissions, destructions, deliveries;
int handles[2];
const int32_t create_results[2]={0x1234567,-1234567};
const int32_t destroy_results[2]={-456789,0x2345678};
struct FullInfo { int32_t state, detail; char tail[512]; } expected_info;
void* expected_payload;
const char* expected_xml;
void original_callback(void* user,void* info) {
    assert(user==0 && info==expected_payload && errno==EDOM);
    if(info)assert(!memcmp(info,&expected_info,sizeof expected_info));
    ++deliveries;
    errno=ERANGE;
}
int32_t original_create(const char* xml,void* user,const Callbacks* table,void** storage) {
    assert(submissions<2 && xml==expected_xml && user==0 && storage && errno==EINTR);
    retained[submissions]=table; // Deliberately retain, do not copy the table.
    for(unsigned i=0;i<19;++i) {
        if(i!=1)assert(table->entry[i]==expected.entry[i]);
    }
    const int32_t result=create_results[submissions];
    *storage=&handles[submissions++];
    errno=ENOSPC;
    return result;
}
int32_t original_destroy(void** storage) {
    assert(storage && *storage==&handles[destructions] && errno==EINTR);
    const int32_t result=destroy_results[destructions];
    ++destructions;*storage=0;errno=ENOSPC;
    return result;
}
void deliver(unsigned generation,void* payload) {
    for(unsigned i=0;i<19;++i) {
        if(i!=1)assert(retained[generation]->entry[i]==expected.entry[i]);
    }
    expected_payload=payload;
    errno=EDOM;
    reinterpret_cast<StatusCallback>(retained[generation]->entry[1])(0,payload);
    assert(errno==ERANGE);
    if(payload)assert(!memcmp(payload,&expected_info,sizeof expected_info));
}
}

int main() {
    alarm(15);
    create_original=original_create;destroy_original=original_destroy;
    original_status=reinterpret_cast<uintptr_t>(original_callback);
    for(unsigned i=0;i<19;++i)expected.entry[i]=0x12340000u+i;
    expected.entry[1]=original_status;
    expected_xml="authored callback fixture";
    void* handle=0;
    FullInfo info={99,-1,{0}};
    for(unsigned i=0;i<sizeof info.tail;++i)info.tail[i]=static_cast<char>(i*37u);
    memcpy(&expected_info,&info,sizeof info);
    for(unsigned cycle=0;cycle<2;++cycle) {
        Callbacks borrowed=expected;
        errno=EINTR;
        assert(create(expected_xml,0,&borrowed,&handle)==create_results[cycle] && errno==ENOSPC);
        assert(!memcmp(&borrowed,&expected,sizeof borrowed));
        memset(&borrowed,0,sizeof borrowed);
        deliver(cycle,&info);
        errno=EINTR;
        assert(destroy(&handle)==destroy_results[cycle] && !handle && errno==ENOSPC);
    }
    assert(retained[0]!=retained[1] && retained[0]->entry[1]!=retained[1]->entry[1]);
    // Delayed old-generation and NULL callbacks preserve their own context.
    deliver(0,&info);deliver(1,0);
    assert(submissions==2 && destructions==2 && deliveries==4 && next_event.load()==4);
    puts("PASS retained callback tables, distinct generations, exact returns, payload and errno, including NULL");
    return 0;
}
