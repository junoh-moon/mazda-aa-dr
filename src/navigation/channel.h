#ifndef MX5_NAVIGATION_CHANNEL_H
#define MX5_NAVIGATION_CHANNEL_H
#include "pipeline.h"
#include <sys/types.h>

namespace mx5 { namespace navigation {
// Local diagnostic input only. This protocol grants no physical qualification.
// The one definition of the product's motion channel name (tap and worker).
static const char MOTION_CHANNEL_NAME[]="mx5dr.motion.v1";
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
    // Receipt age at the consumer check (checked_ns - producer received_ns)
    // for authenticated decoded records with a usable clock; 0 otherwise.
    // late: accepted although older than MOTION_FRESH_NS (see below).
    uint64_t age_ns;
    bool late;
};
// A record older than this at the consumer check is stale unless it is a
// late arrival of the same source (inspect_motion_datagram).
static const uint64_t MOTION_FRESH_NS=250000000ULL;
struct MotionDatagram {
    const unsigned char* bytes;
    size_t size;
    bool truncated, credentials_present;
    pid_t sender_pid;
    uid_t sender_uid;
};
class MotionCursor {
public:
    MotionCursor():pid_(0),epoch_(0),sequence_(0),received_ns_(0) {}
    bool accept(pid_t pid,uint64_t epoch,uint64_t sequence);
    // received_ns (producer receipt) is only remembered for the late-arrival
    // monotonicity check; it is never rewritten or used as event time here.
    ReceiveFault check(pid_t pid,uint64_t epoch,uint64_t sequence,uint64_t received_ns=0);
    // Late arrival: the next contiguous record of the current producer pid and
    // epoch whose receipt time does not go backwards.
    bool late_admissible(pid_t pid,uint64_t epoch,uint64_t sequence,uint64_t received_ns) const;
    // A stale record of the current source still consumes its sequence number,
    // so the next fresh record is not also rejected as a discontinuity. A
    // replay, rewind or another source leaves the cursor unchanged.
    void advance_stale(pid_t pid,uint64_t epoch,uint64_t sequence,uint64_t received_ns);
private:
    pid_t pid_;
    uint64_t epoch_,sequence_,received_ns_;
};
// Shared bounded validation boundary; rejected authenticated records are
// diagnostic evidence only and are never returned through accepted_out.
ReceiveResult inspect_motion_datagram(const MotionDatagram&, uid_t expected_uid,
    uint64_t now_ns, MotionCursor&, RawEvent* accepted_out, ReceiveDiagnostic*);
class MotionReceiver {
public:
    MotionReceiver();
    ~MotionReceiver();
    bool open_channel(const char* name=MOTION_CHANNEL_NAME);
    // Bounded readiness wait only: 1=input, 0=timeout/interrupted, -1=error.
    // Does not consume records, alter their receipt times, or advance the cursor.
    int wait_for_input(unsigned timeout_ms) const;
    // Sample the consumer check clock after recvmsg. A packet may arrive
    // after the caller's earlier clock read; that is not a future timestamp.
    ReceiveResult receive(RawEvent*, ReceiveDiagnostic* = 0);
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
    bool open_channel(const char* name=MOTION_CHANNEL_NAME);
    bool send_event(const RawEvent&);
private:
    int fd_;
    char name_[96];
    MotionSender(const MotionSender&);
    MotionSender& operator=(const MotionSender&);
};

// ---- VIM side channel (validation/VIM_CHANNEL_CAPTURE_2026-10-10.md) ----
// LOGGING ONLY. Raw payload copies of the VIM 0x116 (whole payload, incl.
// the longitudinal acceleration, brake pressure and Qf bytes the motion path
// ignores), 0x169 and 0x15B messages, batched by the tap. A separate socket
// with its own batch counter: never a RawEvent, never a receive_seq, never
// read by the Pipeline, the BETA controller or the adapter. Lossy by design:
// the tap sends nonblocking and counts what it could not send; the worker
// drains it only for diagnostic journal rows.
static const size_t CHAN_RECORDS=8;
static const size_t CHAN_PAYLOAD=16;          // VIMC application bytes (28-12)
static const size_t CHAN_RECORD_SIZE=24;      // id u16, length u8, 0, dt_ms u32, payload[16]
static const size_t CHAN_HEADER_SIZE=24;      // "MDC1", version u16, count u16, epoch u64, batch u32, lost u32
static const size_t CHAN_DATAGRAM_MAX=CHAN_HEADER_SIZE+CHAN_RECORDS*CHAN_RECORD_SIZE;
// length CHAN_LENGTH_INVALID: the callback message had no data or more than
// CHAN_PAYLOAD bytes (nothing copied).
static const uint8_t CHAN_LENGTH_INVALID=255;
struct ChanRecord {
    uint16_t id;
    uint8_t length;
    uint32_t dt_ms;                           // tap receipt time - epoch (ms)
    unsigned char data[CHAN_PAYLOAD];
};
struct ChanBatch {
    uint64_t epoch;                           // the tap's motion epoch (CLOCK_MONOTONIC ns)
    uint32_t batch;                           // tap batch counter (1, 2, ...)
    uint32_t lost;                            // cumulative records the tap dropped
    unsigned count;
    ChanRecord records[CHAN_RECORDS];
};
// Bytes written (CHAN_HEADER_SIZE + count*CHAN_RECORD_SIZE) or 0 when invalid.
size_t encode_chan_batch(const ChanBatch&,unsigned char out[CHAN_DATAGRAM_MAX]);
bool decode_chan_batch(const unsigned char*,size_t,ChanBatch*);
// The side channel's name: the motion channel's name + ".ch".
bool chan_channel_name(const char* motion,char out[96]);
// Owner switch (2026-10-10 review): while this file exists (any type,
// any content) the tap opens no side-channel socket and the worker binds
// none; read once at start. Absent, or not checkable for another reason:
// on. Deliberately not a mx5dr.conf key: the guard binds that file's hash,
// so editing it declines the whole product until a reinstall.
static const char CHAN_OFF_MARKER[]="/data_persist/mx5-aa-dr/vimchan-off";
// false only when `marker` exists (lstat succeeds); every error is "on".
bool chan_switch_on(const char* marker);
class ChanSender {
public:
    ChanSender();
    ~ChanSender();
    bool open_channel(const char* name);
    // One nonblocking datagram; no retry.
    bool send(const unsigned char*,size_t);
private:
    int fd_;
    char name_[96];
    ChanSender(const ChanSender&);
    ChanSender& operator=(const ChanSender&);
};
class ChanReceiver {
public:
    ChanReceiver();
    ~ChanReceiver();
    bool open_channel(const char* name);
    // Nonblocking: CHANNEL_EMPTY, CHANNEL_EVENT (*out decoded from an
    // authenticated sender of this uid) or CHANNEL_FAULT (rejected datagram).
    ReceiveResult receive(ChanBatch* out);
    bool active() const { return fd_>=0; }
private:
    int fd_;
    ChanReceiver(const ChanReceiver&);
    ChanReceiver& operator=(const ChanReceiver&);
};
} }
#endif
