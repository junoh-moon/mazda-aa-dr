#ifndef MX5_RUNTIME_WORKER_TICK_H
#define MX5_RUNTIME_WORKER_TICK_H
#include <stdint.h>

namespace mx5 { namespace runtime {
// Input readiness may wake the worker between MODEL calculations. Always wait
// only until the existing 50 ms calculation deadline, so input cadence cannot
// move it to e.g. 0, 60, 120 ms for a stream arriving every 20 ms.
class WorkerTick {
public:
    WorkerTick():last_(0),next_(0) {}
    bool due(uint64_t now) {
        if(next_ && now>=last_ && now<next_)return false;
        last_=now;
        next_=UINT64_MAX-now<50000000ULL?UINT64_MAX:now+50000000ULL;
        return true;
    }
    unsigned wait_ms(uint64_t now) const {
        if(!next_ || now<last_ || now>=next_)return 0;
        const uint64_t remaining=next_-now;
        return unsigned((remaining+999999ULL)/1000000ULL);
    }
private:
    uint64_t last_,next_;
};
} }
#endif
