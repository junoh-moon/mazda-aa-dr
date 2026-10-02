#ifndef MX5_ADAPTER_LDS_HOOKS_H
#define MX5_ADAPTER_LDS_HOOKS_H
#include "runtime/lds_sideband.h"
#include "runtime/lds_association.h"
#include <stdint.h>
#include <sys/select.h>

namespace mx5 { namespace adapter {
typedef void (*LdsCallback)(void*);
typedef int32_t (*LdsGeneric)(void*,void*,void*,void*);
typedef void (*LdsService)(void*,int32_t*,uint64_t*,double*,double*,int32_t*,
                           double*,double*,double*,double*);
// Exact four-word ARM descriptor; the original table is never rewritten.
struct LdsDescriptor { const char* name; LdsService service; LdsGeneric generic; uintptr_t reserved; };
struct LdsRoute {
    // Immutable verified handler for this registration ID, not a registration
    // instance or physical measurement owner. RMC mode can inherit GGA/GSA
    // dependencies; assigned_mask identifies observed assignments only.
    LdsCallback callback;
    uint32_t assigned_mask;
    uintptr_t read_caller,update_caller;
};
struct LdsCacheSites {
    uintptr_t read_lock,read_copy,read_unlock;
    uintptr_t update_lock,update_copy,update_unlock;
    uintptr_t service_lock,service_unlock;
};
struct LdsMessageApi {
    int32_t (*type)(void*);
    uint32_t (*serial)(void*),(*reply_serial)(void*);
    const char* (*sender)(void*);
    const char* (*destination)(void*);
    const char* (*path)(void*);
    const char* (*interface_name)(void*);
    const char* (*member)(void*);
};
struct LdsSendSites {
    uintptr_t message_lock_return,native_lock_return;
    const uint32_t *current_generation,*initialized_generation;
    uintptr_t connection_mutex_offset,uninitialized_mutex;
};
// An observation at the original message-lock return, never send completion
// or physical sensor qualification. Every value is owned; no OEM pointer escapes.
struct LdsLockedSend {
    runtime::lds_association::Stage stage;
    int32_t reply_type;
    uint64_t observed_ns;
    runtime::lds_sideband::WireIdentity wire;
    runtime::lds_sideband::Lineage field_lineage;
    PositionInput position;
};
enum LdsLockedLoss : uint32_t { LOCKED_CHAIN_CONFLICT=1,LOCKED_SEND_FAILED=2 };
typedef void (*LdsPublishLocked)(const LdsLockedSend&,void*);
typedef void (*LdsInvalidateLocked)(LdsLockedLoss,void*);
struct LdsInputBindings {
    // Parse's third word is opaque, not a readable length. Only the verified
    // original reader call site supplies the assembled terminated sentence.
    // A subsequent verified reader select discards pending ownership; it is
    // not evidence of callback execution or cache assignment.
    uint32_t (*parse_sentence)(char*,void*,uint32_t);
    int (*select)(int,fd_set*,fd_set*,fd_set*,timeval*);
    // Original Close has no defined result; normal return is only a boundary.
    void (*driver_close)();
    uintptr_t parse_return,select_return,dispatch_return;
};
struct LdsBindings {
    void (*initialize)(); void (*clear)();
    // The original Open callback word is forwarded unchanged, never invoked
    // or dereferenced here. Open's return cannot identify a callback-table clear.
    int32_t (*driver_open)(uintptr_t);
    int32_t (*registration)(uint32_t,LdsCallback);
    int32_t (*read)(void*); int32_t (*update)(const void*);
    void (*lock)(void*,const char*,unsigned); void (*unlock)(void*);
    void* (*copy)(void*,const void*,unsigned);
    int32_t (*set_callback)(void*,LdsGeneric,void*);
    LdsGeneric generic; LdsService service;
    int32_t (*path)(void*,void*,void*);
    void* (*method_build)(void*); void* (*reply_create)(void*,void*);
    void* (*reply_message)(void*,void*);
    int32_t (*send)(void*,void*,uint32_t*);
    LdsMessageApi raw;
    const LdsDescriptor* descriptor;
    void *current_cache,*current_mutex;
    LdsRoute routes[12]; LdsCacheSites sites;
    // Bounded, nonthrowing observer clock; may run under the already-held
    // original cache mutex. Never producer measurement time.
    uint64_t (*clock)(void*);
    // Bounded, nonblocking, nonthrowing consumer, after normal Path return.
    // No original mutex or borrowed OEM pointer is retained in this value.
    void (*emit)(const runtime::lds_sideband::Record&,void*);
    void* user;
    void (*message_lock)(void*);
    int32_t (*native_mutex_lock)(void*);
    LdsSendSites send_sites;
    // Optional pair, bounded/nonthrowing memory operations only. These may
    // execute under the original connection mutex; no I/O or waiting here.
    LdsPublishLocked publish_locked;
    LdsInvalidateLocked invalidate_locked;
    LdsInputBindings input;
};
// Installer-owned cold preparation is immutable even after transaction rollback.
// Partial-publication wrappers always forward; only activate enables metadata.
// Retain all target modules until process exit. An observed normal Initialize
// starts a cache observation lifetime; late activation never fabricates one.
bool prepare_lds_hooks(const LdsBindings&);
bool activate_lds_hooks();
} }
extern "C" void mx5_lds_initialize();
extern "C" void mx5_lds_clear();
extern "C" int32_t mx5_lds_driver_open(uintptr_t);
extern "C" void mx5_lds_driver_close();
extern "C" uint32_t mx5_lds_parse_sentence(char*,void*,uint32_t);
extern "C" int mx5_lds_select(int,fd_set*,fd_set*,fd_set*,timeval*);
extern "C" int32_t mx5_lds_register(uint32_t,mx5::adapter::LdsCallback);
extern "C" int32_t mx5_lds_read(void*);
extern "C" int32_t mx5_lds_update(const void*);
extern "C" void mx5_lds_mutex_lock(void*,const char*,unsigned);
extern "C" void mx5_lds_mutex_unlock(void*);
extern "C" void* mx5_lds_mem_copy(void*,const void*,unsigned);
extern "C" int32_t mx5_lds_set_callback(void*,mx5::adapter::LdsGeneric,void*);
extern "C" int32_t mx5_lds_path(void*,void*,void*);
extern "C" void* mx5_lds_method_build(void*);
extern "C" void* mx5_lds_reply_create(void*,void*);
extern "C" void* mx5_lds_reply_message(void*,void*);
extern "C" int32_t mx5_lds_send(void*,void*,uint32_t*);
extern "C" void mx5_lds_message_lock(void*);
extern "C" int32_t mx5_lds_native_mutex_lock(void*);
#endif
