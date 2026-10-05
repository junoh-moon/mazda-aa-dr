#include "config.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
namespace mx5 {
namespace runtime {
static char *trim(char *p) {
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
    ++p;
  char *e = p + strlen(p);
  while (e > p &&
         (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
    *--e = 0;
  return p;
}
Config read_config(const char *path) {
  Config c = {0, 8388608, 3, 1000, true};
  FILE *f = fopen(path, "r");
  if (!f) {
    c.valid = false;
    c.mode = 0;
    return c;
  }
  char line[256];
  unsigned seen = 0;
  while (fgets(line, sizeof line, f)) {
    if (!strchr(line, '\n') && !feof(f)) {
      c.valid = false;
      break;
    }
    char *comment = strchr(line, '#');
    if (comment)
      *comment = 0;
    char *k = trim(line);
    if (!*k)
      continue;
    char *eq = strchr(k, '=');
    if (!eq) {
      c.valid = false;
      break;
    }
    *eq = 0;
    char *v = trim(eq + 1);
    k = trim(k);
    unsigned bit = 0;
    if (!strcmp(k, "mode")) {
      bit = 1;
      if (!strcmp(v, "OFF"))
        c.mode = 0;
      else if (!strcmp(v, "OBSERVE"))
        c.mode = 1;
      else if (!strcmp(v, "SCRUB"))
        c.mode = 2;
      else if (!strcmp(v, "SHADOW"))
        c.mode = 4;
      // Explicit opt-in: SHADOW capture plus the MODEL-domain BETA
      // replacement (validation/ASSIST_BETA_DESIGN_2026-10-05.md decision 9).
      // The qualified ASSIST token (internal 3) stays unsupported.
      else if (!strcmp(v, "BETA"))
        c.mode = 5;
      else {
        c.valid = false;
        break;
      }
    } else {
      char *end = 0;
      errno = 0;
      unsigned long n = strtoul(v, &end, 10);
      if (errno || !*v || *end || *v == '-' || *v == '+') {
        c.valid = false;
        break;
      }
      if (!strcmp(k, "max_log_bytes")) {
        bit = 2;
        if (n < 65536 || n > 41943040)
          c.valid = false;
        else
          c.max_log_bytes = n;
      } else if (!strcmp(k, "max_log_files")) {
        bit = 4;
        if (n < 1 || n > 3)
          c.valid = false;
        else
          c.max_log_files = n;
      } else if (!strcmp(k, "sample_ms")) {
        bit = 8;
        if (n < 500 || n > 5000)
          c.valid = false;
        else
          c.sample_ms = n;
      } else
        c.valid = false;
    }
    if (!c.valid || (seen & bit)) {
      c.valid = false;
      break;
    }
    seen |= bit;
  }
  // An existing file is not an opt-in: require one explicit, valid mode.
  if (ferror(f) || !(seen & 1))
    c.valid = false;
  fclose(f);
  if (!c.valid)
    c.mode = 0;
  return c;
}
bool startup_enabled(const char *config_path, const char *disable_path, Config *out) {
  *out = read_config(config_path);
  if (!out->valid || out->mode == 0) return false;
  struct stat marker;
  if (lstat(disable_path, &marker) == 0) return false;
  return errno == ENOENT;
}
} // namespace runtime
} // namespace mx5
