#define MX5_COLLECTOR_TESTING
#define main collector_program_main
#include "../runtime/storage_fixture.h"
#define statvfs(path,info) fixture_statvfs(path,info)
#include "../../src/collector/collector.cpp"
#undef statvfs
#undef main
#include <cassert>
#include <string>

int main(int argc,char** argv) {
  if(argc==3 && !strcmp(argv[1],"--real-storage")) {
    config.max_log_bytes=1048576;config.max_log_files=2;
    check_real_storage<Journal>(argv[2],"collector");return 0;
  }
  if(argc==3 && !strcmp(argv[1],"--storage")) {
    config.max_log_bytes=65536;config.max_log_files=2;
    check_storage<Journal>("collector",argv[2]);return 0;
  }
  // Collector coordinates: 3 decimals (validation/V1_STABLE_PRIVACY_2026-10-18.md).
  {
    struct { double in; const char *want; } cases[] = {
        {12.3456789, "12.346"}, {-45.6784999, "-45.678"}, {0.0, "0.000"}, {90.0, "90.000"},
        {-180.0, "-180.000"}, {179.99951, "180.000"}, {0.00049, "0.000"}, {0.00051, "0.001"},
        {360.0, "360.000"}, {360.0001, "null"}, {-1e300, "null"}, {NAN, "null"}, {INFINITY, "null"}};
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
      char out[48];
      json_coordinate(cases[i].in, out);
      if (strcmp(out, cases[i].want)) fprintf(stderr, "coordinate %.10g -> %s (want %s)\n", cases[i].in, out, cases[i].want);
      assert(!strcmp(out, cases[i].want));
    }
    char full[48];
    json_number(12.3456789, full);
    assert(!strcmp(full, "12.345678899999999"));   // other numbers keep full precision
  }
  char tmp[] = "/tmp/mx5dr-collector-journal-XXXXXX";
  assert(mkdtemp(tmp));
  std::string identity_path = std::string(tmp) + "/boot-id";
  const char *identity = "12345678-1234-1234-1234-123456789abc";
  FILE *identity_file = fopen(identity_path.c_str(), "w");
  assert(identity_file && fputs(identity, identity_file) >= 0 && !fclose(identity_file));
  char boot_id[37];
  mx5::runtime::read_boot_id(boot_id, identity_path.c_str());
  assert(!strcmp(identity, boot_id));
  identity_file = fopen(identity_path.c_str(), "w");
  assert(identity_file && fputs("malformed", identity_file) >= 0 && !fclose(identity_file));
  mx5::runtime::read_boot_id(boot_id, identity_path.c_str());
  assert(!strcmp("unknown", boot_id));
  assert(!unlink(identity_path.c_str()));
  mx5::runtime::read_boot_id(boot_id, identity_path.c_str());
  assert(!strcmp("unknown", boot_id));
  std::string logs = std::string(tmp) + "/logs";
  assert(mkdir(logs.c_str(), 0700) == 0);
  config.max_log_bytes = 512;
  config.max_log_files = 2;
  {
    Journal journal(tmp);
    for (unsigned i = 0; i < 100; ++i)
      journal.line("{\"kind\":\"fixture\"}");
    journal.flush();
    assert(!journal.failed);
  }
  for (unsigned i = 0; i < 2; ++i) {
    std::string path = logs + "/collector." + char('0' + i) + ".jsonl";
    struct stat st;
    assert(!stat(path.c_str(), &st) && st.st_size <= 512);
    FILE *file = fopen(path.c_str(), "r");
    assert(file);
    char line[512];
    assert(fgets(line, sizeof line, file));
    assert(strstr(line, "\"stream\":\"collector\""));
    assert(strstr(line, "\"producer_mono_ns\":null"));
    fclose(file);
    assert(!unlink(path.c_str()));
  }
  {
    Journal journal(tmp);
    journal.f = fopen("/dev/full", "w");
    assert(journal.f);
    journal.line("{}");
    journal.flush();
    assert(journal.failed);
  }
  {
    Journal journal(tmp);
    journal.line(("{\"large\":\"" + std::string(1000, 'a') + "\"}").c_str());
    assert(journal.failed);
  }
  assert(!rmdir(logs.c_str()));
  assert(!rmdir(tmp));
  puts("Collector journal: bounded independent rotation, unknown producer time, write failures and sanitized child environment passed");
}
