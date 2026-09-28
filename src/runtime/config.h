#ifndef MX5_RUNTIME_CONFIG_H
#define MX5_RUNTIME_CONFIG_H
#include <stddef.h>
namespace mx5 {
namespace runtime {
struct Config {
  unsigned mode;
  size_t max_log_bytes;
  unsigned max_log_files, sample_ms;
  bool valid;
};
Config read_config(const char *path);
} // namespace runtime
} // namespace mx5
#endif
