#ifndef MX5_RUNTIME_LDS_SIDEBAND_H
#define MX5_RUNTIME_LDS_SIDEBAND_H

#include "adapter/adapter.h"
#include "sensors/lds_lineage.h"
#include <atomic>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

namespace mx5 { namespace runtime { namespace lds_sideband {

// Observation transport only. Neither credentials nor field assignment clocks
// establish producer measurement time, receiver quality or ASSIST provenance.
enum { RECORD_SIZE=640, JSON_CAPACITY=4096, DRAIN_LIMIT=16 };
// Exact NA 74.00.324A normal/WCP services use cmu, whose shipped UID is zero.
// This is a local account boundary; it does not authenticate the LDS executable.
static const uid_t LDS_UID=0;
static const char CHANNEL_NAME[]="mx5dr.lds.v1";
typedef request_trace::Text Text;
typedef sensors::lds_lineage::FieldOrigin FieldOrigin;
enum Flags {
    SNAPSHOT_KNOWN=1, REQUEST_KNOWN=2, REPLY_KNOWN=4, RAW_SEND_SUCCEEDED=8,
    CHAIN_CONFLICT=16, PATH_RETURNED=32, RAW_SEND_CALLED=64,
    LOSS_COUNTER_SATURATED=128
};
struct WireIdentity {
    Text server_guid,client_unique,server_unique,destination;
    uint32_t request_serial,response_serial,reply_serial;
};
struct Lineage {
    uint64_t lifetime,write_sequence;
    FieldOrigin fields[sensors::lds_lineage::FIELD_COUNT];
};
struct Record {
    // Sender supplies the first three fields, unrelated to cache/bus lifetimes.
    uint64_t source_instance,sequence,dropped_before,observed_ns;
    uint32_t flags;
    int32_t path_result,send_result,reply_type;
    WireIdentity wire;
    Lineage field_lineage;
    adapter::PositionInput position;
};
void copy_text(Text*,const char* borrowed);
// Explicit little-endian fields; no native padding, pointers or Snapshot owner.
// Encoding preserves IEEE double bits, including nonfinite diagnostic inputs.
bool encode(const Record&,unsigned char out[RECORD_SIZE]);
bool decode(const unsigned char*,size_t,Record*);

enum ReceiveResult { EMPTY, RECORD, REJECTED };
enum ReceiveFault { RECEIVE_OK, SYSCALL_FAILED, TRUNCATED, NO_CREDENTIALS,
                    WRONG_CREDENTIALS, BAD_RECORD };
const char* receive_fault_name(ReceiveFault);
struct Diagnostic {
    ReceiveFault fault;
    pid_t sender_pid;
    uid_t sender_uid;
    int syscall_errno;
    uint64_t received_ns;
};
struct Datagram {
    const unsigned char* bytes;
    size_t size;
    bool truncated,credentials_present;
    pid_t sender_pid;
    uid_t sender_uid;
};
// No age, contiguous-sequence, motion-epoch or prediction-generation gate.
ReceiveResult inspect(const Datagram&,uid_t expected_uid,Record*,Diagnostic*);
class Receiver {
public:
    Receiver();
    ~Receiver();
    bool open_channel(const char* name=CHANNEL_NAME,uid_t expected_uid=LDS_UID);
    ReceiveResult receive(Record*,Diagnostic*);
    void close_channel();
    bool active() const { return fd_>=0; }
private:
    int fd_;
    uid_t expected_uid_;
    Receiver(const Receiver&)=delete;
    Receiver& operator=(const Receiver&)=delete;
};
class Sender {
public:
    Sender();
    ~Sender();
    // Initialization/shutdown require external quiescence. instance must be a
    // nonzero observed process-instance identity, not producer measurement time.
    bool open_channel(const char* name=CHANNEL_NAME,uint64_t instance=0);
    // Call only after original cache unlock and completed original Path/send.
    // Bounded try-lock, no allocation/retry/blocking socket operation; errno is
    // preserved on success and failure. Failure never changes OEM forwarding.
    bool try_send(const Record&);
private:
    friend struct SenderTestAccess;
    int fd_;
    char name_[96];
    uint64_t instance_,sequence_;
    std::atomic<unsigned> busy_;
    std::atomic<uint32_t> dropped_;
    std::atomic<bool> saturated_;
    void loss();
    Sender(const Sender&)=delete;
    Sender& operator=(const Sender&)=delete;
};
// AA worker only; standalone row uses the existing bounded journal. It never
// edits, delays or supplies a missing field to the raw POSITION observation.
bool format_record(char*,size_t,const Record&,const Diagnostic&);
bool format_status(char*,size_t,const char* status,const Diagnostic&,
                   uint64_t count=0);

} } }
#endif
