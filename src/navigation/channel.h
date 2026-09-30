#ifndef MX5_NAVIGATION_CHANNEL_H
#define MX5_NAVIGATION_CHANNEL_H
#include "pipeline.h"
#include <sys/types.h>

namespace mx5 { namespace navigation {
// Local diagnostic input only. This protocol grants no physical qualification.
// Fixed byte encoding, no compiler ABI structs, no pointer/heap ownership.
static const size_t MOTION_RECORD_SIZE=64;
bool encode_motion(const RawEvent&, unsigned char out[MOTION_RECORD_SIZE]);
bool decode_motion(const unsigned char*, size_t, RawEvent*);
enum ReceiveResult { CHANNEL_EMPTY, CHANNEL_EVENT, CHANNEL_FAULT };
enum ReceiveFault {
    RECEIVE_OK, RECEIVE_SYSCALL, RECEIVE_TRUNCATED, RECEIVE_CREDENTIALS_MISSING,
    RECEIVE_CREDENTIALS_MISMATCH, RECEIVE_DECODE, RECEIVE_CLOCK_UNAVAILABLE,
    RECEIVE_FUTURE, RECEIVE_STALE, RECEIVE_SOURCE_CHANGED, RECEIVE_SEQUENCE
};
const char* receive_fault_name(ReceiveFault);
struct ReceiveDiagnostic {
    ReceiveFault reason;
    uint64_t checked_ns;
    bool credentials_present, authenticated_decoded;
    pid_t sender_pid;
    uid_t sender_uid;
    int syscall_errno;
    RawEvent rejected;
};
struct MotionDatagram {
    const unsigned char* bytes;
    size_t size;
    bool truncated, credentials_present;
    pid_t sender_pid;
    uid_t sender_uid;
};
class MotionCursor {
public:
    MotionCursor():pid_(0),epoch_(0),sequence_(0) {}
    bool accept(pid_t pid,uint64_t epoch,uint64_t sequence);
    ReceiveFault check(pid_t pid,uint64_t epoch,uint64_t sequence);
private:
    pid_t pid_;
    uint64_t epoch_,sequence_;
};
// Shared bounded validation boundary; rejected authenticated records are
// diagnostic evidence only and are never returned through accepted_out.
ReceiveResult inspect_motion_datagram(const MotionDatagram&, uid_t expected_uid,
    uint64_t now_ns, MotionCursor&, RawEvent* accepted_out, ReceiveDiagnostic*);
class MotionReceiver {
public:
    MotionReceiver();
    ~MotionReceiver();
    bool open_channel(const char* name="mx5dr.motion.v1");
    // Bounded readiness wait only: 1=input, 0=timeout/interrupted, -1=error.
    // Does not consume records, alter their receipt times, or advance the cursor.
    int wait_for_input(unsigned timeout_ms) const;
    ReceiveResult receive(uint64_t now_ns, RawEvent*, ReceiveDiagnostic* = 0);
    bool active() const { return fd_>=0; }
private:
    int fd_;
    MotionCursor cursor_;
    MotionReceiver(const MotionReceiver&);
    MotionReceiver& operator=(const MotionReceiver&);
};
// Collector-side, nonblocking: a rejected datagram is not retried as new data.
class MotionSender {
public:
    MotionSender();
    ~MotionSender();
    bool open_channel(const char* name="mx5dr.motion.v1");
    bool send_event(const RawEvent&);
private:
    int fd_;
    char name_[96];
    MotionSender(const MotionSender&);
    MotionSender& operator=(const MotionSender&);
};
} }
#endif
