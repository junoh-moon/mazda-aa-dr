// Includes the private logger so failure paths can be tested without exposing
// configuration switches in the shipped runtime. No firmware is loaded.
#include "../../src/runtime/runtime.cpp"
#include <cassert>
#include <string>

static int32_t unused_next(void *, A::VehicleData *) { return 0; }
static void arm_test_mode() {
  audit_fault = 0;
  assert(A::set_mode(A::SCRUB_STALE));
}
int main() {
  char a[] = "LD_PRELOAD=/fixture.so", b[] = "LD_LIBRARY_PATH=/jci/lib",
       c[] = "JCI_FIXTURE=data", d[] = "LD_AUDIT=/audit.so";
  char *input[] = {a, b, c, d, 0}, *output[3];
  assert(child_environment(input, output, 3));
  assert(output[0] == b && output[1] == c && !output[2]);
  assert(!child_environment(input, output, 2));
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
