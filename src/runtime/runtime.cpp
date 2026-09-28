#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../adapter/adapter.h"
#include "config.h"
#include "sha256.h"
#include <dbus/dbus.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

namespace {
namespace A = mx5::adapter;
const char *const ROOT = "/data_persist/mx5-aa-dr";
const char *const BLM = "/jci/aapa/blmjciaapa.so";
typedef void *(*Dlopen)(const char *, int);
Dlopen next_dlopen = 0;
pthread_mutex_t boot_mu = PTHREAD_MUTEX_INITIALIZER;
bool boot_attempted = false;
__thread bool inside_dlopen = false;
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
void json_text(const char *in, char *out, size_t n) {
  size_t j = 0;
  for (size_t i = 0; in[i] && j + 7 < n; ++i) {
    unsigned char c = in[i];
    if (c == '"' || c == '\\') {
      out[j++] = '\\';
      out[j++] = c;
    } else if (c >= 32 && c < 127)
      out[j++] = c;
    else {
      snprintf(out + j, n - j, "\\u%04x", c);
      j += 6;
    }
  }
  out[j] = 0;
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

bool child_environment(char *const *source, char **out, size_t capacity) {
  size_t n = 0;
  if (!capacity)
    return false;
  for (size_t i = 0; source && source[i]; ++i) {
    if (!strncmp(source[i], "LD_PRELOAD=", 11) ||
        !strncmp(source[i], "LD_AUDIT=", 9))
      continue;
    if (n + 1 >= capacity)
      return false;
    out[n++] = source[i];
  }
  out[n] = 0;
  return true;
}

// Spawn only fixed, read-only SMDB commands, with LD_PRELOAD removed. Preserve
// the service's library/search/data environment rather than guessing it. The
// child has a 250 ms wall-time budget and bounded output. It never touches the
// GPS tty.
void raw_field(const char *name, char *out, size_t cap) {
  out[0] = 0;
  const char *exe = access("/jci/bin/smdb-read", X_OK) == 0
                        ? "/jci/bin/smdb-read"
                        : "/jci/smdb/smdb-read";
  if (access(exe, X_OK)) {
    snprintf(out, cap, "unavailable");
    return;
  }
  int p[2];
  if (pipe(p)) {
    snprintf(out, cap, "pipe_error");
    return;
  }
  fcntl(p[0], F_SETFD, FD_CLOEXEC);
  fcntl(p[1], F_SETFD, FD_CLOEXEC);
  fcntl(p[0], F_SETFL, O_NONBLOCK);
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_adddup2(&fa, p[1], 1);
  posix_spawn_file_actions_adddup2(&fa, p[1], 2);
  posix_spawn_file_actions_addclose(&fa, p[0]);
  posix_spawn_file_actions_addclose(&fa, p[1]);
  char *args[] = {const_cast<char *>(exe),
                  const_cast<char *>("-n"),
                  const_cast<char *>("vdm_vdt_current_data"),
                  const_cast<char *>("-e"),
                  const_cast<char *>(name),
                  0};
  char *env[256];
  if (!child_environment(environ, env, sizeof env / sizeof env[0])) {
    posix_spawn_file_actions_destroy(&fa);
    close(p[0]);
    close(p[1]);
    snprintf(out, cap, "environment_limit");
    return;
  }
  pid_t pid;
  int result = posix_spawn(&pid, exe, &fa, 0, args, env);
  posix_spawn_file_actions_destroy(&fa);
  close(p[1]);
  if (result) {
    close(p[0]);
    snprintf(out, cap, "spawn_error:%d", result);
    return;
  }
  size_t used = 0;
  uint64_t start = clock_ns(0);
  int status = 0;
  bool done = false;
  while (clock_ns(0) - start < 250000000ULL) {
    ssize_t n = read(p[0], out + used, cap - used - 1);
    if (n > 0)
      used += n;
    pid_t w = waitpid(pid, &status, WNOHANG);
    if (w == pid) {
      done = true;
      break;
    }
    if (w < 0 && errno != EINTR) {
      done = errno == ECHILD;
      break;
    }
    struct timespec t = {0, 10000000};
    nanosleep(&t, 0);
  }
  if (!done) {
    kill(pid, SIGKILL);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    snprintf(out + used, cap - used, " [timeout]");
  } else {
    ssize_t n = read(p[0], out + used, cap - used - 1);
    if (n > 0)
      used += n;
    out[used] = 0;
  }
  close(p[0]);
}

DBusConnection *connect_bus() {
  DBusError e;
  dbus_error_init(&e);
  DBusConnection *c =
      dbus_connection_open_private("unix:path=/tmp/dbus_service_socket", &e);
  if (c && !dbus_bus_register(c, &e)) {
    dbus_connection_close(c);
    dbus_connection_unref(c);
    c = 0;
  }
  if (c)
    dbus_connection_set_exit_on_disconnect(c, 0);
  dbus_error_free(&e);
  return c;
}
DBusMessage *call(DBusConnection *c, const char *dest, const char *path,
                  const char *iface, const char *method, const char *arg = 0) {
  if (!c)
    return 0;
  DBusMessage *m = dbus_message_new_method_call(dest, path, iface, method);
  if (!m)
    return 0;
  dbus_message_set_auto_start(m, 0);
  if (arg &&
      !dbus_message_append_args(m, DBUS_TYPE_STRING, &arg, DBUS_TYPE_INVALID)) {
    dbus_message_unref(m);
    return 0;
  }
  DBusError e;
  dbus_error_init(&e);
  DBusMessage *r = dbus_connection_send_with_reply_and_block(c, m, 350, &e);
  dbus_message_unref(m);
  dbus_error_free(&e);
  return r;
}
void owner_log(Journal &j, DBusConnection *c) {
  DBusMessage *r =
      call(c, "org.freedesktop.DBus", "/org/freedesktop/DBus",
           "org.freedesktop.DBus", "GetNameOwner", "com.jci.lds.data");
  if (!r)
    return;
  const char *owner = 0;
  if (dbus_message_get_args(r, 0, DBUS_TYPE_STRING, &owner,
                            DBUS_TYPE_INVALID)) {
    dbus_uint32_t pid = 0;
    DBusMessage *p =
        call(c, "org.freedesktop.DBus", "/org/freedesktop/DBus",
             "org.freedesktop.DBus", "GetConnectionUnixProcessID", owner);
    if (p) {
      if (!dbus_message_get_args(p, 0, DBUS_TYPE_UINT32, &pid,
                                 DBUS_TYPE_INVALID))
        pid = 0;
      dbus_message_unref(p);
    }
    char proc[64], comm[128] = "", eo[256], ec[800], line[1400];
    if (pid) {
      snprintf(proc, sizeof proc, "/proc/%u/comm", pid);
      FILE *f = fopen(proc, "r");
      if (f) {
        if (!fgets(comm, sizeof comm, f))
          comm[0] = 0;
        fclose(f);
        comm[strcspn(comm, "\r\n")] = 0;
      }
    }
    json_text(owner, eo, sizeof eo);
    json_text(comm, ec, sizeof ec);
    snprintf(line, sizeof line,
             "{\"kind\":\"owner_poll\",\"receipt_ns\":%llu,\"owner\":\"%s\","
             "\"pid\":%u,\"comm\":\"%s\",\"request_provenance\":false}",
             (unsigned long long)clock_ns(0), eo, pid, ec);
    j.line(line);
  }
  dbus_message_unref(r);
}
void sensor_log(Journal &j, DBusConnection *c, uint64_t serial) {
  char line[4096], speed[160], yaw[160], reverse[160], es[1024], ey[1024],
      er[1024];
  uint64_t begin = clock_ns(0);
  raw_field("VehicleSpeed", speed, sizeof speed);
  raw_field("YawRate", yaw, sizeof yaw);
  raw_field("TransmChangeLeverPosition", reverse, sizeof reverse);
  json_text(speed, es, sizeof es);
  json_text(yaw, ey, sizeof ey);
  json_text(reverse, er, sizeof er);
  snprintf(line, sizeof line,
           "{\"kind\":\"poll\",\"seq\":%llu,\"begin_ns\":%llu,\"end_ns\":%llu,"
           "\"speed_raw\":\"%s\",\"yaw_raw\":\"%s\",\"gear_raw\":\"%s\","
           "\"freshness\":\"unproven_poll\",\"quality\":\"unknown\",\"assist_"
           "ready\":false}",
           (unsigned long long)serial, (unsigned long long)begin,
           (unsigned long long)clock_ns(0), es, ey, er);
  j.line(line);
  DBusMessage *r = call(c, "com.jci.lds.data", "/com/jci/lds/data",
                        "com.jci.lds.data", "GetPosition");
  if (r) {
    dbus_int32_t mode = 0, alt = 0;
    dbus_uint64_t utc = 0;
    double lat = 0, lon = 0, head = 0, vel = 0, h = 0, v = 0;
    if (dbus_message_get_args(r, 0, DBUS_TYPE_INT32, &mode, DBUS_TYPE_UINT64,
                              &utc, DBUS_TYPE_DOUBLE, &lat, DBUS_TYPE_DOUBLE,
                              &lon, DBUS_TYPE_INT32, &alt, DBUS_TYPE_DOUBLE,
                              &head, DBUS_TYPE_DOUBLE, &vel, DBUS_TYPE_DOUBLE,
                              &h, DBUS_TYPE_DOUBLE, &v, DBUS_TYPE_INVALID)) {
      char slat[48], slon[48], sh[48], sv[48];
      json_number(lat, slat);
      json_number(lon, slon);
      json_number(head, sh);
      json_number(vel, sv);
      snprintf(line, sizeof line,
               "{\"kind\":\"position_poll\",\"receipt_ns\":%llu,\"mode\":%d,"
               "\"utc_s\":%llu,\"lat\":%s,\"lon\":%s,\"heading\":%s,\"kmh\":%s,"
               "\"request_provenance\":false}",
               (unsigned long long)clock_ns(0), mode, (unsigned long long)utc,
               slat, slon, sh, sv);
      j.line(line);
    } else
      j.line("{\"kind\":\"position_poll_error\",\"reason\":\"signature\"}");
    dbus_message_unref(r);
  } else
    j.line("{\"kind\":\"position_poll_error\",\"reason\":\"unavailable\"}");
  if (serial % 5 == 0) {
    owner_log(j, c);
    r = call(c, "com.jci.lds.control", "/com/jci/lds/control",
             "com.jci.lds.control", "GetSelectedGPS_sync");
    if (r) {
      dbus_int32_t selected = -1;
      if (dbus_message_get_args(r, 0, DBUS_TYPE_INT32, &selected,
                                DBUS_TYPE_INVALID)) {
        snprintf(
            line, sizeof line,
            "{\"kind\":\"receiver_poll\",\"receipt_ns\":%llu,\"receiver\":%d}",
            (unsigned long long)clock_ns(0), selected);
        j.line(line);
      }
      dbus_message_unref(r);
    }
  }
}

void *worker(void *) {
  locale_t numeric_locale = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
  if (!numeric_locale) {
    disable_mutation();
    return 0;
  }
  uselocale(numeric_locale);
  Journal j;
  char line[2200];
  snprintf(line, sizeof line,
           "{\"kind\":\"boot\",\"schema\":1,\"pid\":%ld,\"mono_ns\":%llu,"
           "\"mode\":%u,\"install\":\"%s\",\"assist_ready\":false,\"assist_"
           "block\":\"sensor_timing_quality_calibration_unverified\",\"wire_"
           "timestamp_modified\":false}",
           (long)getpid(), (unsigned long long)clock_ns(0), config.mode,
           boot_result);
  j.line(line);
  j.flush();
  if (hook_installed && config.mode == 2 && !j.failed &&
      !__sync_fetch_and_add(&audit_fault, 0)) {
    A::set_mode(A::SCRUB_STALE);
    if (__sync_fetch_and_add(&audit_fault, 0))
      A::set_mode(A::OBSERVE);
  }
  dbus_threads_init_default();
  DBusConnection *c = connect_bus();
  uint64_t last_poll = 0, last_flush = 0, serial = 0;
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
    if (now - last_poll >= uint64_t(config.sample_ms) * 1000000ULL) {
      last_poll = now;
      if (c && !dbus_connection_get_is_connected(c)) {
        dbus_connection_close(c);
        dbus_connection_unref(c);
        c = 0;
      }
      if (!c)
        c = connect_bus();
      sensor_log(j, c, serial++);
    }
    now = clock_ns(0);
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
  char cfg[256];
  snprintf(cfg, sizeof cfg, "%s/mx5dr.conf", ROOT);
  config = mx5::runtime::read_config(cfg);
  if (!config.valid || config.mode == 0)
    return;
  if (access("/data_persist/mx5-aa-dr/logs/disable-next-start", F_OK) == 0)
    return;
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

extern "C" __attribute__((visibility("default"))) void *dlopen(const char *path,
                                                               int flags) {
  int incoming_errno = errno;
  // The resolver does not interpose dlsym. No untrusted symbol-name guessing.
  Dlopen real = __atomic_load_n(&next_dlopen, __ATOMIC_ACQUIRE);
  if (!real) {
    real = reinterpret_cast<Dlopen>(dlsym(RTLD_NEXT, "dlopen"));
    if (real)
      __atomic_store_n(&next_dlopen, real, __ATOMIC_RELEASE);
  }
  if (!real) {
    errno = ENOSYS;
    return 0;
  }
  if (inside_dlopen || !path || strcmp(path, BLM) || (flags & RTLD_NOLOAD)) {
    errno = incoming_errno;
    return real(path, flags);
  }
  inside_dlopen = true;
  pthread_mutex_lock(&boot_mu);
  bool first = !boot_attempted;
  void *existing = 0;
  if (first)
    existing = real(path, RTLD_NOW | RTLD_NOLOAD);
  // Resolve this first target's GOT before replacing its verified send slot.
  int load_flags =
      (first && !existing) ? ((flags & ~RTLD_LAZY) | RTLD_NOW) : flags;
  errno = incoming_errno;
  void *h = real(path, load_flags);
  int original_errno = errno;
  if (first && h) {
    boot_attempted = true;
    if (!existing)
      bootstrap(h); /* already initialized: never patch live code */
  }
  if (existing)
    dlclose(existing);
  pthread_mutex_unlock(&boot_mu);
  inside_dlopen = false;
  errno = original_errno;
  return h;
}
