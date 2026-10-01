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
  char preload[] = "LD_PRELOAD=/fixture.so", audit[] = "LD_AUDIT=/audit.so";
  char library[] = "LD_LIBRARY_PATH=/jci/lib", data[] = "JCI_FIXTURE=data";
  char *input[] = {preload, library, data, audit, 0}, *output[3];
  assert(child_environment(input, output, 3));
  assert(output[0] == library && output[1] == data && !output[2]);
  assert(!child_environment(input, output, 2));
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
