#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../adapter/adapter.h"
#include "config.h"
#include "loader.h"
#include "boot_id.h"
#include "sha256.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace {
namespace A = mx5::adapter;
const char *const ROOT = "/data_persist/mx5-aa-dr";
A::Observation queue[256];
unsigned qhead = 0, qtail = 0, qsize = 0;
pthread_mutex_t queue_mu = PTHREAD_MUTEX_INITIALIZER;
volatile uint32_t dropped = 0;
volatile uint32_t audit_fault = 0;
mx5::runtime::Config config = {0, 8388608, 3, 1000, false};
const char *boot_result = "not_attempted";
bool hook_installed = false;
void disable_mutation() {
  if (__sync_bool_compare_and_swap(&audit_fault, 0, 1))
    A::set_mode(A::OBSERVE);
}
uint64_t clock_ns(void *) {
  struct timespec t;
  if (clock_gettime(CLOCK_MONOTONIC, &t))
    return 0;
  return uint64_t(t.tv_sec) * 1000000000ULL + t.tv_nsec;
}
void sink(const A::Observation *o, void *) {
  if (pthread_mutex_trylock(&queue_mu)) {
    __sync_fetch_and_add(&dropped, 1);
    disable_mutation();
    return;
  }
  if (qsize == 256) {
    __sync_fetch_and_add(&dropped, 1);
    disable_mutation();
  } else {
    queue[qtail] = *o;
    qtail = (qtail + 1) % 256;
    ++qsize;
  }
  pthread_mutex_unlock(&queue_mu);
}
bool pop(A::Observation *out) {
  pthread_mutex_lock(&queue_mu);
  bool ok = qsize != 0;
  if (ok) {
    *out = queue[qhead];
    qhead = (qhead + 1) % 256;
    --qsize;
  }
  pthread_mutex_unlock(&queue_mu);
  return ok;
}

// No live provenance or sensor freshness is fabricated from polling. SCRUB
// uses only the original request mode; custom DR remains a separate gate.
bool provenance(void *, const A::PositionInput *, A::Provenance *out, void *) {
  memset(out, 0, sizeof *out);
  return false;
}
void hex48(const uint8_t *p, char *out) {
  const char *h = "0123456789abcdef";
  for (unsigned i = 0; i < 48; ++i) {
    out[2 * i] = h[p[i] >> 4];
    out[2 * i + 1] = h[p[i] & 15];
  }
  out[96] = 0;
}
void json_number(double x, char out[48]) {
  if (isfinite(x))
    snprintf(out, 48, "%.17g", x);
  else
    strcpy(out, "null");
}

struct Journal {
  const char *root;
  FILE *f;
  size_t written;
  bool failed;
  explicit Journal(const char *directory = ROOT)
      : root(directory), f(0), written(0), failed(false) {}
  ~Journal() {
    if (f)
      fclose(f);
  }
  void fail() {
    failed = true;
    disable_mutation();
  }
  void rotate() {
    if (f) {
      bool bad = fflush(f) != 0;
      if (fclose(f))
        bad = true;
      f = 0;
      if (bad) {
        fail();
        return;
      }
    }
    char a[256], b[256];
    for (unsigned i = config.max_log_files; i > 1; --i) {
      snprintf(a, sizeof a, "%s/logs/trace.%u.jsonl", root, i - 2);
      snprintf(b, sizeof b, "%s/logs/trace.%u.jsonl", root, i - 1);
      if (rename(a, b) && errno != ENOENT) {
        fail();
        return;
      }
    }
    snprintf(a, sizeof a, "%s/logs/trace.0.jsonl", root);
    int fd =
        open(a, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
      fail();
      return;
    }
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) {
      close(fd);
      fail();
      return;
    }
    f = fdopen(fd, "w");
    if (!f) {
      close(fd);
      fail();
      return;
    }
    written = 0;
  }
  void line(const char *s) {
    if (failed)
      return;
    size_t n = strlen(s);
    if (n + 1 > config.max_log_bytes) {
      fail();
      return;
    }
    if (!f || written + n + 1 > config.max_log_bytes)
      rotate();
    if (!f || failed)
      return;
    if (fwrite(s, 1, n, f) != n || fputc('\n', f) == EOF) {
      fail();
      return;
    }
    written += n + 1;
  }
  void flush() {
    if (f && fflush(f))
      fail();
  }
};

void *worker(void *) {
  locale_t numeric_locale = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
  if (!numeric_locale) {
    disable_mutation();
    return 0;
  }
  uselocale(numeric_locale);
  Journal j;
  char line[2200];
  char boot_id[37];
  mx5::runtime::read_boot_id(boot_id);
  snprintf(line, sizeof line,
           "{\"kind\":\"boot\",\"schema\":1,\"pid\":%ld,\"mono_ns\":%llu,"
           "\"boot_id\":\"%s\","
           "\"mode\":%u,\"install\":\"%s\",\"assist_ready\":false,\"assist_"
           "block\":\"sensor_timing_quality_calibration_unverified\",\"wire_"
           "timestamp_modified\":false}",
           (long)getpid(), (unsigned long long)clock_ns(0), boot_id, config.mode,
           boot_result);
  j.line(line);
  j.flush();
  if (hook_installed && config.mode == 2 && !j.failed &&
      !__sync_fetch_and_add(&audit_fault, 0)) {
    A::set_mode(A::SCRUB_STALE);
    if (__sync_fetch_and_add(&audit_fault, 0))
      A::set_mode(A::OBSERVE);
  }
  uint64_t last_flush = 0;
  for (;;) {
    A::Observation o;
    unsigned drained = 0;
    while (drained++ < 256 && pop(&o)) {
      if (o.kind == A::Observation::POSITION) {
        char lat[48], lon[48], h[48], v[48];
        json_number(o.position.latitude_deg, lat);
        json_number(o.position.longitude_deg, lon);
        json_number(o.position.heading_deg, h);
        json_number(o.position.velocity_kmh, v);
        snprintf(line, sizeof line,
                 "{\"kind\":\"position\",\"call\":%u,\"generation\":%u,\"mono_"
                 "ns\":%llu,\"mode\":%d,\"utc_s\":%llu,\"lat\":%s,\"lon\":%s,"
                 "\"heading\":%s,\"kmh\":%s}",
                 o.call_sequence, o.prediction_generation,
                 (unsigned long long)o.mono_ns, o.original_mode,
                 (unsigned long long)o.position.utc_seconds, lat, lon, h, v);
      } else {
        char a[97] = "", b[97] = "";
        if (o.has_payload) {
          hex48(o.original, a);
          hex48(o.outgoing, b);
        }
        snprintf(
            line, sizeof line,
            "{\"kind\":\"send\",\"call\":%u,\"generation\":%u,\"mono_ns\":%llu,"
            "\"mode\":%d,\"type\":%u,\"length\":%u,\"choice\":%u,\"reason\":%u,"
            "\"result\":%d,\"original_hex\":\"%s\",\"outgoing_hex\":\"%s\"}",
            o.call_sequence, o.prediction_generation,
            (unsigned long long)o.mono_ns, o.original_mode, o.type, o.length,
            unsigned(o.choice), unsigned(o.reason), o.result, a, b);
      }
      j.line(line);
    }
    uint64_t now = clock_ns(0);
    if (now - last_flush >= 1000000000ULL) {
      last_flush = now;
      snprintf(line, sizeof line,
               "{\"kind\":\"health\",\"mono_ns\":%llu,\"dropped\":%u,\"hook_"
               "installed\":%s,\"runtime_mode\":%u,\"audit_fault\":%u,\"assist_"
               "ready\":false}",
               (unsigned long long)now, __sync_fetch_and_add(&dropped, 0),
               hook_installed ? "true" : "false", unsigned(A::mode()),
               __sync_fetch_and_add(&audit_fault, 0));
      j.line(line);
      j.flush();
    }
    struct timespec pause = {0, 50000000};
    nanosleep(&pause, 0);
  }
  return 0;
}

void bootstrap(void *h) {
  void *entry = dlsym(h, "GetServiceInterfaces");
  void *send = dlsym(h, "aap_send_vehicle_data");
  Dl_info bi, ai;
  memset(&bi, 0, sizeof bi);
  memset(&ai, 0, sizeof ai);
  if (!entry || !send || !dladdr(entry, &bi) || !dladdr(send, &ai)) {
    boot_result = "module_lookup_failed";
  } else {
    A::InstallOptions io = A::InstallOptions();
    io.blm_load_bias = reinterpret_cast<uintptr_t>(bi.dli_fbase);
    io.interface_load_bias = reinterpret_cast<uintptr_t>(ai.dli_fbase);
    io.blm_path = bi.dli_fname;
    io.interface_path = ai.dli_fname;
    io.verify_file_hash = mx5_verify_file_sha256;
    io.verified_cold_start = true;
    io.begin_patch = mx5::runtime::loader_begin_patch;
    io.end_patch = mx5::runtime::loader_end_patch;
    io.runtime.sink = sink;
    io.runtime.clock = clock_ns;
    io.runtime.provenance = provenance;
    io.runtime.max_snapshot_age_ns = 500000000ULL;
    io.runtime.allow_assist = false;
    A::InstallResult result = A::install_v74(io);
    boot_result = A::install_result_name(result);
    hook_installed = result == A::INSTALL_OK;
    if (result == A::RESTORE_FAILED_FATAL) {
      const char fatal[] = "restore_failed_fatal\n";
      int fd =
          open("/data_persist/mx5-aa-dr/logs/disable-next-start",
               O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
      if (fd >= 0) {
        ssize_t n = write(fd, fatal, sizeof fatal - 1);
        (void)n;
        fsync(fd);
        close(fd);
      }
      _exit(126);
    }
    if (hook_installed)
      A::set_mode(A::OBSERVE);
  }
  pthread_t thread;
  if (pthread_create(&thread, 0, worker, 0) == 0)
    pthread_detach(thread);
  else {
    A::set_mode(A::OBSERVE);
    A::invalidate();
  }
}
} // namespace

namespace mx5 { namespace runtime {
bool loader_enabled() {
  char cfg[256];
  snprintf(cfg, sizeof cfg, "%s/mx5dr.conf", ROOT);
  return startup_enabled(cfg, "/data_persist/mx5-aa-dr/logs/disable-next-start", &config);
}
void loader_bootstrap(void* h) { bootstrap(h); }
} }
