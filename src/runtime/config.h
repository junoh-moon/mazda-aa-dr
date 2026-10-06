#ifndef MX5_RUNTIME_CONFIG_H
#define MX5_RUNTIME_CONFIG_H
#include <stddef.h>
namespace mx5 {
namespace runtime {
// mode: 0 OFF, 1 OBSERVE, 2 SCRUB, 4 SHADOW, 5 BETA (SHADOW capture plus the
// opt-in MODEL-domain BETA replacement). 3 (qualified ASSIST) is never parsed.
struct Config {
  unsigned mode;
  size_t max_log_bytes;
  unsigned max_log_files, sample_ms;
  bool valid;
  // log_profile=full (0, default) or persistent (1): the quiet always-on
  // journal profile (validation/PERSISTENT_LOGGING_2026-10-06.md).
  unsigned log_profile;
};
enum { LOG_PROFILE_FULL = 0, LOG_PROFILE_PERSISTENT = 1 };
// Require exactly one explicit mode. Missing/invalid configuration is invalid OFF;
// numeric defaults apply only to otherwise valid configuration.
Config read_config(const char *path);
// Fail closed unless valid enabled configuration and a definitely absent marker.
bool startup_enabled(const char *config_path, const char *disable_path, Config *out);
} // namespace runtime
} // namespace mx5
#endif
