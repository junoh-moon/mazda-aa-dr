#ifndef MX5_DR_BOOT_ID_H
#define MX5_DR_BOOT_ID_H
#include <stdio.h>
#include <string.h>

namespace mx5 { namespace runtime {
// Kernel boot identity correlates observers on one CMU boot, never a producer
// measurement or exact LDS request. Unknown is explicit if procfs is missing.
inline void read_boot_id(char out[37], const char *path = "/proc/sys/kernel/random/boot_id") {
  strcpy(out, "unknown");
  FILE *file = fopen(path, "r");
  if (!file) return;
  char value[64];
  bool ok = fgets(value, sizeof value, file) != 0;
  fclose(file);
  if (!ok) return;
  size_t length = strcspn(value, "\r\n");
  if (length != 36) return;
  for (size_t i = 0; i < length; ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (value[i] != '-') return;
    } else if (!((value[i] >= '0' && value[i] <= '9') ||
                 (value[i] >= 'a' && value[i] <= 'f') ||
                 (value[i] >= 'A' && value[i] <= 'F'))) return;
  }
  memcpy(out, value, 36);
  out[36] = 0;
}
} }
#endif
