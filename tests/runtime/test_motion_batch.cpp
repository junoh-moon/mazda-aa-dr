#include "runtime/motion_batch.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
using mx5::runtime::MotionBatch;
using mx5::navigation::RawEvent;

static RawEvent fixture(unsigned i) {
  RawEvent e = RawEvent();
  e.kind = static_cast<mx5::navigation::SensorKind>(1 + i % 3);
  e.epoch = 42;
  e.receive_seq = 1 + i;
  e.received_ns = 1000000000000ULL + uint64_t(i) * 6666667;
  e.source_mono_ms = i % 7 == 0 ? -1 : 0;
  for (unsigned j = 0; j < 4; ++j) e.raw[j] = uint16_t(i * 13 + j);
  e.count = uint16_t(i % 256);
  e.reverse = i % 5;
  return e;
}

int main(int argc, char **argv) {
  MotionBatch b;
  assert(b.empty() && !b.line());
  RawEvent max = fixture(0);
  max.epoch = max.receive_seq = max.received_ns = UINT64_MAX;
  max.source_mono_ms = INT64_MIN;
  for (unsigned j = 0; j < 4; ++j) max.raw[j] = UINT16_MAX;
  max.count = UINT16_MAX; max.reverse = UINT16_MAX;
  for (unsigned i = 0; i < MotionBatch::MAX_EVENTS; ++i) assert(b.append(max));
  assert(!b.append(max));
  assert(strlen(b.line()) + 1 < MotionBatch::BUFFER_BYTES);
  if (argc == 2 && !strcmp(argv[1], "--max")) { puts(b.line()); return 0; }
  b.clear();
  assert(b.append(max));
  char before[MotionBatch::BUFFER_BYTES]; strcpy(before, b.line());
  RawEvent next = fixture(0);
  assert(!b.append(next)); // Epoch change leaves previous line unchanged.
  assert(!strcmp(before, b.line()));
  b.clear();
  if (argc >= 3 && !strcmp(argv[1], "--emit")) {
    const unsigned n = unsigned(strtoul(argv[2], 0, 10));
    const unsigned group = argc == 4 ? unsigned(strtoul(argv[3], 0, 10)) : 32;
    assert(group && group <= MotionBatch::MAX_EVENTS);
    for (unsigned i = 0; i < n; ++i) {
      assert(b.append(fixture(i)));
      if ((i + 1) % group == 0) { puts(b.line()); b.clear(); }
    }
    if (!b.empty()) puts(b.line());
    return 0;
  }
  puts("Motion batch: empty, full, epoch split, maximum-width fields and bound passed");
}
