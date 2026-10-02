#ifndef MX5_RUNTIME_LDS_ASSOCIATION_CHANNEL_H
#define MX5_RUNTIME_LDS_ASSOCIATION_CHANNEL_H
#include "lds_association.h"
#include <atomic>
#include <sys/types.h>

namespace mx5 { namespace adapter { struct LdsLockedSend;struct PositionContext; } }
namespace mx5 { namespace runtime { namespace lds_association {
static const char CHANNEL_NAME[]="mx5dr.lds.assoc.v1";
// This channel establishes an observation association only. Credentials identify
// a local account, never the program, physical sensor, receiver or freshness.
class Publisher {
public:
    Publisher();
    ~Publisher(); // owner quiescent; production retains the owner until exit
    bool prepare(uint64_t source_instance,const char* directory="/tmp");
    bool publish(const adapter::LdsLockedSend&); // bounded memory only
    void invalidate();
    void disable_after_fork(); // child-local only; never writes parent's map
    bool offer(const char* channel,uint64_t now_ns); // post-Path only
private:
    friend struct AssociationTestAccess;
    void* mapping_;
    int readonly_fd_,socket_;
    uint64_t instance_,created_ns_,last_offer_ns_,floor_ns_,latest_ns_;
    uint32_t sequence_,count_,seen_loss_;
    std::atomic<uint32_t> busy_,offer_busy_,disabled_;
    Publisher(const Publisher&)=delete;
    Publisher& operator=(const Publisher&)=delete;
};
class Registry {
public:
    Registry();
    ~Registry(); // quiescent owner; callbacks must not outlive this object
    bool open_channel(const char* channel,uid_t expected_uid);
    void drain(uint64_t now_ns); // worker only; at most two control datagrams
    bool read(const adapter::PositionContext&,Owned*); // bounded memory only
    void retire(); // immediate local admission revocation, no wait/unmap
    void close_channel(); // worker only; active borrowers are never unmapped
    void disable_after_fork(); // no close/unmap/allocation/lock in child handler
private:
    friend struct AssociationTestAccess;
    struct View {
        std::atomic<uint32_t> gate;
        void* mapping;
        uint64_t device,inode,instance,floor_ns;
        uint32_t revision,admission_epoch;
        pid_t pid;
        uid_t uid;
        View();
    };
    View views_[2];
    std::atomic<uint32_t> active_,disabled_,admission_epoch_;
    int socket_;
    uid_t expected_uid_;
    uint32_t revision_,drained_epoch_;
    uint64_t floor_ns_,last_drain_ns_,last_created_ns_,last_device_,last_inode_,last_instance_;
    pid_t last_pid_;
    void reclaim();
    void close_views(); // worker only; never called by asynchronous retire
    Registry(const Registry&)=delete;
    Registry& operator=(const Registry&)=delete;
};
} } }
#endif
