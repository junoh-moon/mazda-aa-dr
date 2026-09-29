#ifndef MX5_RUNTIME_MOTION_BATCH_H
#define MX5_RUNTIME_MOTION_BATCH_H
#include "navigation/pipeline.h"
#include <stdio.h>
#include <string.h>

namespace mx5 { namespace runtime {

// Lossless journal representation, NOT a sensor/transport format. Only the
// worker owns this object. No heap allocation, sampling, or inferred clocks.
// Rows: sensor, receive_seq, received_ns, source_mono_ms, raw[0..3], count, reverse.
// Every line is independently decodable after file rotation.
class MotionBatch {
public:
  enum { MAX_EVENTS = 32, BUFFER_BYTES = 6144 };
  MotionBatch() : size_(0), count_(0), epoch_(0) { data_[0] = 0; }
  bool empty() const { return count_ == 0; }
  void clear() { size_ = count_ = 0; epoch_ = 0; data_[0] = 0; }
  // false means flush this batch and retry the SAME event, never discard it.
  bool append(const navigation::RawEvent &e) {
    if (count_ == MAX_EVENTS || (count_ && epoch_ != e.epoch))
      return false;
    char row[256];
    const int n = snprintf(row, sizeof row,
        "%s[%u,%llu,%llu,%lld,%u,%u,%u,%u,%u,%d]",
        count_ ? "," : "", unsigned(e.kind),
        (unsigned long long)e.receive_seq, (unsigned long long)e.received_ns,
        (long long)e.source_mono_ms, unsigned(e.raw[0]), unsigned(e.raw[1]),
        unsigned(e.raw[2]), unsigned(e.raw[3]), unsigned(e.count), e.reverse);
    if (n < 0 || size_t(n) >= sizeof row)
      return false;
    if (!count_) {
      const int h = snprintf(data_, sizeof data_,
          "{\"kind\":\"motion_batch\",\"schema\":1,\"epoch\":%llu,"
          "\"producer_time_status\":\"unknown\",\"events\":[",
          (unsigned long long)e.epoch);
      if (h < 0 || size_t(h) >= sizeof data_)
        return false;
      size_ = size_t(h);
      epoch_ = e.epoch;
    }
    if (size_ + size_t(n) + 3 > sizeof data_)
      return false;
    memcpy(data_ + size_, row, size_t(n));
    size_ += size_t(n);
    ++count_;
    return true;
  }
  const char *line() {
    if (!count_)
      return 0;
    memcpy(data_ + size_, "]}", 3);
    return data_;
  }
private:
  char data_[BUFFER_BYTES];
  size_t size_;
  unsigned count_;
  uint64_t epoch_;
};
} }
#endif
