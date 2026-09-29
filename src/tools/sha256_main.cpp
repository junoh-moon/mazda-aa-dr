// Small static ARM helper for CMUs without a sha256sum applet.
#include "../runtime/sha256.h"
#include <stdio.h>
int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "Usage: mx5dr-sha256 FILE\n");
    return 2;
  }
  char digest[65];
  if (!mx5_sha256_file(argv[1], digest)) {
    fprintf(stderr, "mx5dr-sha256: cannot read %s\n", argv[1]);
    return 1;
  }
  return printf("%s  %s\n", digest, argv[1]) < 0 || fflush(stdout) ? 1 : 0;
}
