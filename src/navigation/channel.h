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
class MotionCursor {
public:
    MotionCursor():pid_(0),epoch_(0),sequence_(0) {}
    bool accept(pid_t pid,uint64_t epoch,uint64_t sequence);
private:
    pid_t pid_;
    uint64_t epoch_,sequence_;
};
class MotionReceiver {
public:
    MotionReceiver();
    ~MotionReceiver();
    bool open_channel(const char* name="mx5dr.motion.v1");
    ReceiveResult receive(uint64_t now_ns, RawEvent*);
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
