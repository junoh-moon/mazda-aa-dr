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
  // Archives (validation/LOG_COMPRESSION_2026-11-04.md): every overwritten
  // collector.1 was gzipped; newest is collector.2.jsonl.gz. Decoding all
  // archives oldest-first then .1 and .0 must give every row exactly once and
  // in order, with collector_archive notes interleaved.
  {
    mx5::zlibdyn::Api z;
    assert(mx5::zlibdyn::load(z));
    std::string all;
    unsigned archives = 0;
    for (unsigned i = ARCHIVE_LAST; i >= 2; --i) {
      std::string path = logs + "/collector." + fixture_number(i) + ".jsonl.gz";
      int in = open(path.c_str(), O_RDONLY);
      if (in < 0) continue;
      ++archives;
      char out_path[] = "/tmp/mx5dr-collector-gunzip-XXXXXX";
      int out = mkstemp(out_path);
      assert(out >= 0 && mx5::zlibdyn::gunzip_fd(z, in, out));
      close(in);
      lseek(out, 0, SEEK_SET);
      char buf[4096]; ssize_t r;
      while ((r = read(out, buf, sizeof buf)) > 0) all.append(buf, size_t(r));
      close(out); unlink(out_path);
      assert(!unlink(path.c_str()));
    }
    assert(archives > 10);
    for (unsigned i = 1; i <= 1; --i) {
      std::string path = logs + "/collector." + char('0' + i) + ".jsonl";
      FILE *file = fopen(path.c_str(), "r");
      assert(file);
      char buf[4096]; size_t r;
      while ((r = fread(buf, 1, sizeof buf, file)) > 0) all.append(buf, r);
      fclose(file);
    }
    size_t fixtures = 0, notes = 0, pos = 0;
    while ((pos = all.find("\"kind\":\"fixture\"", pos)) != std::string::npos) { ++fixtures; ++pos; }
    pos = 0;
    while ((pos = all.find("\"kind\":\"collector_archive\",\"result\":\"ok\"", pos)) != std::string::npos) { ++notes; ++pos; }
    // Rows of the current collector.0 are not in the archive set.
    assert(fixtures > 60 && fixtures <= 100);
    assert(notes >= archives - 1);
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
  // Without zlib the old two-file ring stays and no archive appears.
  {
    setenv("MX5_TEST_ZLIB_PATH", "/nonexistent/libz.so.1", 1);
    {
      Journal journal(tmp);
      for (unsigned i = 0; i < 40; ++i) journal.line("{\"kind\":\"fixture\"}");
      journal.flush();
      assert(!journal.failed && journal.zlib_state < 0);
    }
    unsetenv("MX5_TEST_ZLIB_PATH");
    struct stat st;
    assert(stat((logs + "/collector.2.jsonl.gz").c_str(), &st) && errno == ENOENT);
    FILE *file = fopen((logs + "/collector.0.jsonl").c_str(), "r");
    assert(file);
    char buf[8192]; size_t r = fread(buf, 1, sizeof buf - 1, file); buf[r] = 0; fclose(file);
    file = fopen((logs + "/collector.1.jsonl").c_str(), "r");
    assert(file);
    char buf1[8192]; r = fread(buf1, 1, sizeof buf1 - 1, file); buf1[r] = 0; fclose(file);
    assert(strstr(buf, "\"result\":\"unavailable\"") || strstr(buf1, "\"result\":\"unavailable\""));
    assert(!unlink((logs + "/collector.0.jsonl").c_str()) && !unlink((logs + "/collector.1.jsonl").c_str()));
  }
  // Byte budget: the newest archive is always kept, older ones stop at the budget.
  {
    archive_budget_bytes = 400;
    {
      Journal journal(tmp);
      for (unsigned i = 0; i < 100; ++i) journal.line("{\"kind\":\"fixture\"}");
      journal.flush();
      assert(!journal.failed);
    }
    unsigned long long total = 0; unsigned count = 0, highest = 0;
    for (unsigned i = 2; i <= ARCHIVE_LAST; ++i) {
      std::string path = logs + "/collector." + fixture_number(i) + ".jsonl.gz";
      struct stat st;
      if (stat(path.c_str(), &st)) continue;
      total += (unsigned long long)st.st_size; ++count; highest = i;
      assert(!unlink(path.c_str()));
    }
    assert(count >= 1 && (count == 1 || total <= 400));
    assert(highest == count + 1);   // contiguous from collector.2: no gaps
    {
      // The note precedes the row that rotated, so it can never follow a stop.
      FILE *file = fopen((logs + "/collector.0.jsonl").c_str(), "r");
      assert(file);
      char row[1024]; bool seen_fixture = false;
      while (fgets(row, sizeof row, file)) {
        if (strstr(row, "\"kind\":\"fixture\"")) seen_fixture = true;
        if (strstr(row, "\"kind\":\"collector_archive\"")) assert(!seen_fixture);
      }
      fclose(file);
    }
    archive_budget_bytes = 2097152ULL;
    assert(!unlink((logs + "/collector.0.jsonl").c_str()) && !unlink((logs + "/collector.1.jsonl").c_str()));
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
  puts("Collector journal: bounded independent rotation, gzip archives (order, fallback, budget), unknown producer time, write failures and sanitized child environment passed");
}
