// Includes the private logger so failure paths can be tested without exposing
// configuration switches in the shipped runtime. No firmware is loaded.
#include "../../src/runtime/runtime.cpp"
#include <cassert>
#include <string>
#include <fstream>
#include <iterator>

static int32_t unused_next(void *, A::VehicleData *) { return 0; }
static void arm_test_mode() {
  audit_fault = 0;
  assert(A::set_mode(A::SCRUB_STALE));
}
int main() {
  A::Options opt = A::Options();
  assert(A::configure(unused_next, opt));
  config.max_log_bytes = 64;
  config.max_log_files = 3;
  char tmp[] = "/tmp/mx5dr-journal-XXXXXX";
  assert(mkdtemp(tmp));
  std::string logs = std::string(tmp) + "/logs";
  arm_test_mode();
  {
    Journal missing(tmp);
    missing.line("{}");
    assert(missing.failed && A::mode() == A::OBSERVE);
  }
  assert(mkdir(logs.c_str(), 0700) == 0);
  arm_test_mode();
  {
    Journal j(tmp);
    for (int i = 0; i < 20; ++i)
      j.line("{\"kind\":\"fixture\"}");
    j.flush();
    assert(!j.failed && A::mode() == A::SCRUB_STALE);
  }
  for (int i = 0; i < 3; ++i) {
    struct stat st;
    std::string name = logs + "/trace." + char('0' + i) + ".jsonl";
    assert(stat(name.c_str(), &st) == 0 && st.st_size <= 64);
    unlink(name.c_str());
  }
  // Exercise the actual worker helpers, not a test-only batching wrapper.
  config.max_log_bytes = 65536;
  {
    Journal j(tmp);
    mx5::runtime::MotionBatch batch;
    N::RawEvent raw = N::RawEvent();
    raw.kind = N::YAW; raw.epoch = 1; raw.received_ns = 10;
    for (unsigned i = 1; i <= 33; ++i) {
      raw.receive_seq = i;
      journal_motion(j, batch, raw);
    }
    raw.epoch = 2; raw.receive_seq = 1;
    journal_motion(j, batch, raw); // Flushes the old epoch before adding new.
    flush_motion(j, batch);
    j.line("{\"kind\":\"shadow_input_reset\"}");
    raw.receive_seq = 3;
    journal_motion(j, batch, raw);
    flush_motion(j, batch); // Empty channel/turn end before health or sleep.
    flush_motion(j, batch); // Must not duplicate a previous batch.
    j.flush();
    assert(!j.failed && batch.empty());
  }
  {
    std::ifstream f((logs + "/trace.0.jsonl").c_str());
    std::string line;
    unsigned n = 0;
    while (std::getline(f, line)) {
      if (n == 3) assert(line.find("shadow_input_reset") != std::string::npos);
      else assert(line.find("motion_batch") != std::string::npos);
      if (n < 2) assert(line.find("\"epoch\":1") != std::string::npos);
      if (n == 2 || n == 4) assert(line.find("\"epoch\":2") != std::string::npos);
      ++n;
    }
    assert(n == 5);
  }
  unlink((logs + "/trace.0.jsonl").c_str());
  {
    Journal full(tmp);
    full.f = fopen("/dev/full", "w");
    assert(full.f);
    mx5::runtime::MotionBatch batch;
    N::RawEvent raw = N::RawEvent(); raw.kind = N::REVERSE;
    raw.epoch = raw.receive_seq = raw.received_ns = 1;
    journal_motion(full, batch, raw); flush_motion(full, batch); full.flush();
    assert(full.failed && batch.empty() && A::mode() == A::OBSERVE);
  }
  arm_test_mode();
  config.max_log_bytes = 64;
  {
    Journal full(tmp);
    full.f = fopen("/dev/full", "w");
    assert(full.f);
    full.line("{}");
    full.flush();
    assert(full.failed && A::mode() == A::OBSERVE);
  }
  arm_test_mode();
  {
    Journal rotation(tmp);
    rotation.f = fopen("/dev/full", "w");
    assert(rotation.f);
    assert(fputs("buffered", rotation.f) >= 0);
    rotation.rotate();
    assert(rotation.failed && A::mode() == A::OBSERVE);
  }
  arm_test_mode();
  {
    Journal overlong(tmp);
    overlong.line(std::string(80, 'x').c_str());
    assert(overlong.failed && A::mode() == A::OBSERVE);
  }
  arm_test_mode();
  qhead = qtail = qsize = 0;
  dropped = 0;
  A::Observation event = A::Observation();
  for (unsigned i = 0; i < 257; ++i)
    sink(&event, 0);
  assert(qsize == 256 && dropped == 1 && A::mode() == A::OBSERVE);
  A::Observation read = A::Observation();
  for (unsigned i = 0; i < 256; ++i)
    assert(pop(&read));
  assert(!pop(&read));
  arm_test_mode();
  assert(!pthread_mutex_lock(&queue_mu));
  sink(&event, 0);
  assert(!pthread_mutex_unlock(&queue_mu));
  assert(dropped == 2 && A::mode() == A::OBSERVE);
  rmdir(logs.c_str());
  rmdir(tmp);
  puts("Journal tests: bounded rotation, open/flush/rotation/size failures and "
       "queue loss disable mutation");
}
