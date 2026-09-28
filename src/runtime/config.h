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
// Require exactly one explicit mode. Missing/invalid configuration is invalid OFF;
// numeric defaults apply only to otherwise valid configuration.
Config read_config(const char *path);
// Fail closed unless valid enabled configuration and a definitely absent marker.
bool startup_enabled(const char *config_path, const char *disable_path, Config *out);
} // namespace runtime
} // namespace mx5
#endif
