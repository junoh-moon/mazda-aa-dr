#include "../../src/runtime/config.h"
#include "../../src/runtime/sha256.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

static std::string path;
static unsigned checks;
static void require(bool v) {
  ++checks;
  assert(v);
}
static mx5::runtime::Config parse(const std::string &value) {
  FILE *f = fopen(path.c_str(), "wb");
  assert(f);
  assert(fwrite(value.data(), 1, value.size(), f) == value.size());
  assert(fclose(f) == 0);
  return mx5::runtime::read_config(path.c_str());
}
static void digest(const std::string &input, const char *expected) {
  char out[65];
  mx5_sha256_bytes(input.data(), input.size(), out);
  require(strcmp(out, expected) == 0);
}
int main() {
  char temp[] = "/tmp/mx5dr-runtime-XXXXXX";
  int fd = mkstemp(temp);
  assert(fd >= 0);
  close(fd);
  path = temp;
  digest("",
         "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  digest("abc",
         "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  digest("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
         "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  digest(std::string(1000000, 'a'),
         "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
  parse("abc");
  require(mx5_verify_file_sha256(
      path.c_str(),
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  require(!mx5_verify_file_sha256(path.c_str(), "aa"));
  require(!mx5_verify_file_sha256(
      path.c_str(),
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
  auto c = parse("# empty config uses conservative defaults\n");
  require(c.valid && c.mode == 1 && c.max_log_bytes == 8388608 &&
          c.max_log_files == 3 && c.sample_ms == 1000);
  c = parse("mode = SCRUB # "
            "explicit\nmax_log_bytes=65536\nmax_log_files=1\nsample_ms=500\n");
  require(c.valid && c.mode == 2 && c.max_log_bytes == 65536 &&
          c.max_log_files == 1 && c.sample_ms == 500);
  c = parse("mode=SHADOW\nsample_ms=5000\n");
  require(c.valid && c.mode == 4);
  c = parse("mode=OFF\n");
  require(c.valid && c.mode == 0);
  const char *bad[] = {"mode=ASSIST\n",
                       "mode=OBSERVE\nmode=SCRUB\n",
                       "sample_ms=499\n",
                       "sample_ms=5001\n",
                       "max_log_bytes=8388609\n",
                       "max_log_files=0\n",
                       "max_log_files=4\n",
                       "max_log_bytes=-1\n",
                       "max_log_bytes=+65536\n",
                       "sample_ms=1000x\n",
                       "sample_ms=\n",
                       "future_key=1\n",
                       "mode OBSERVE\n",
                       "sample_ms=9999999999999999999999999999999999999999\n"};
  for (const auto *input : bad) {
    c = parse(input);
    require(!c.valid && c.mode == 0);
  }
  c = parse(std::string(260, 'x') + "\n");
  require(!c.valid && c.mode == 0);
  unlink(path.c_str());
  c = mx5::runtime::read_config(path.c_str());
  require(!c.valid && c.mode == 0);
  printf("Runtime config/SHA: %u checks passed\n", checks);
}
