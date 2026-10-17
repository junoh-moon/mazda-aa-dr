#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../runtime/config.h"
#include "../runtime/boot_id.h"
#include "../runtime/storage.h"
#include "phone_imu_rx.h"
#include <dbus/dbus.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <sys/file.h>
#include <pwd.h>
#include <grp.h>
#ifndef MX5_COLLECTOR_TESTING
#include <linux/capability.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif
namespace {
const char *const ROOT = "/data_persist/mx5-aa-dr";
const char *bus_address = "unix:path=/tmp/dbus_service_socket";
// The phone IMU receiver's BCA queries and signals: the stock BCA registers
// /com/jci/bca and sends ConnectionStatusResp only on the HMI bus
// (validation/BCA_QUERY_CONNECT_STATUS_RE_2026-10-17.md).
const char *bca_bus_address = "unix:path=/tmp/dbus_hmi_socket";
mx5::runtime::Config config = {0, 8388608, 3, 1000, false, mx5::runtime::LOG_PROFILE_FULL};
volatile sig_atomic_t stop_requested = 0;
void stop_signal(int) { stop_requested = 1; }
uint64_t clock_ns(void *) {
  struct timespec t;
  if (clock_gettime(CLOCK_MONOTONIC, &t)) return 0;
  return uint64_t(t.tv_sec) * 1000000000ULL + t.tv_nsec;
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
    char a[512], b[512];
    for (unsigned i = config.max_log_files; i > 1; --i) {
      snprintf(a, sizeof a, "%s/logs/collector.%u.jsonl", root, i - 2);
      snprintf(b, sizeof b, "%s/logs/collector.%u.jsonl", root, i - 1);
      struct stat existing;
      if (!lstat(a, &existing) && !S_ISREG(existing.st_mode)) { fail(); return; }
      if (rename(a, b) && errno != ENOENT) {
        fail();
        return;
      }
    }
    snprintf(a, sizeof a, "%s/logs/collector.0.jsonl", root);
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
  void line(const char *event) {
    char record[4608];
    if (event[0] != '{') { fail(); return; }
    int length = snprintf(record, sizeof record,
             "{\"stream\":\"collector\",\"collector_pid\":%ld,"
             "\"observed_at_mono_ns\":%llu,\"producer_mono_ns\":null,"
             "\"producer_time_status\":\"unknown\",%s",
             (long)getpid(), (unsigned long long)clock_ns(0), event + 1);
    if (length < 0 || size_t(length) >= sizeof record) { fail(); return; }
    const char *s = record;
    if (failed)
      return;
    size_t n = strlen(s);
    if (n + 1 > config.max_log_bytes) {
      fail();
      return;
    }
    const mx5::runtime::StorageSpace space=mx5::runtime::storage_space(root,n+1);
    if(space.reason) {
      mx5::runtime::record_storage_stop(root,"collector",space);
      fail();return;
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

// The stock SMDB (shared memory database behind jciblmVdt/VDM) is deliberately
// NOT read by this collector. libjcismdb guards it with a named POSIX semaphore
// that a killed reader never releases, and an earlier collector that spawned
// the stock smdb-read tool and SIGKILLed it on timeout is the leading suspect
// for a vehicle reset (jciblmVdt stopped answering SM heartbeats, SM stopped
// the watchdog). The same wheel/yaw/gear values reach the AA runtime trace
// through the VBS tap. This collector never forks, execs, signals or waits on
// another process. See validation/COLLECTOR_SMDB_RESET_2026-10-04.md.
DBusConnection *connect_bus() {
  DBusError e;
  dbus_error_init(&e);
  DBusConnection *c =
      dbus_connection_open_private(bus_address, &e);
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
  char line[4096];
  uint64_t begin = clock_ns(0);
  snprintf(line, sizeof line,
           "{\"kind\":\"poll\",\"seq\":%llu,\"begin_ns\":%llu,\"end_ns\":%llu,"
           "\"speed_raw\":\"smdb_disabled\",\"yaw_raw\":\"smdb_disabled\","
           "\"gear_raw\":\"smdb_disabled\","
           "\"freshness\":\"unproven_poll\",\"quality\":\"unknown\",\"assist_"
           "ready\":false}",
           (unsigned long long)serial, (unsigned long long)begin,
           (unsigned long long)clock_ns(0));
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

// Phone IMU channel (validation/PHONE_IMU_CHANNEL_2026-10-14.md): the
// receiver runs only when menu 1 generated the Bluetooth configuration
// (bt/BdsConfiguration.xml), neither bt-off nor logs/bt-fail exists, and the
// mode records sensors (SHADOW or BETA). It stays inert until the stock bds
// is seen with our -configFilePath. A missing marker check fails closed.
bool absent_path(const char *path) {
  struct stat st;
  return lstat(path, &st) != 0 && errno == ENOENT;
}
bool phone_imu_wanted(const char *root) {
  char xml[512], off[512], fail[512];
  snprintf(xml, sizeof xml, "%s/bt/BdsConfiguration.xml", root);
  snprintf(off, sizeof off, "%s/bt-off", root);
  snprintf(fail, sizeof fail, "%s/logs/bt-fail", root);
  struct stat st;
  return !lstat(xml, &st) && S_ISREG(st.st_mode) && absent_path(off) && absent_path(fail);
}
#ifndef MX5_COLLECTOR_TESTING
// Per-thread capability sets (capset with pid 0 changes the calling thread).
bool set_caps(unsigned permitted, unsigned effective) {
  struct __user_cap_header_struct h;
  struct __user_cap_data_struct d[2];
  memset(&h, 0, sizeof h);
  memset(d, 0, sizeof d);
  h.version = _LINUX_CAPABILITY_VERSION_3;
  d[0].permitted = permitted;
  d[0].effective = effective;
  return syscall(SYS_capset, &h, d) == 0;
}
#endif

} // namespace

int main(int argc, char **argv) {
  const char *root = ROOT;
  unsigned session_seconds = 28800;
  const char *pimu_proc = "/proc", *pimu_ready = "/tmp/bds_ready_flag", *pimu_pts = "/dev/pts/";
  long pimu_owner = -1;
  unsigned long pimu_ready_ms = 60000, pimu_vanish_ms = 30000;
#ifdef MX5_COLLECTOR_TESTING
  unsigned max_samples = 0;
#endif
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--session-seconds") && i + 1 < argc) {
      char *end = 0;
      unsigned long value = strtoul(argv[++i], &end, 10);
      if (!*argv[i] || *end || value < 1 || value > 86400) return 64;
      session_seconds = unsigned(value);
    }
#ifdef MX5_COLLECTOR_TESTING
    else if (!strcmp(argv[i], "--root") && i + 1 < argc) root = argv[++i];
    else if (!strcmp(argv[i], "--bus-address") && i + 1 < argc) bus_address = argv[++i];
    else if (!strcmp(argv[i], "--samples") && i + 1 < argc) max_samples = unsigned(atoi(argv[++i]));
    else if (!strcmp(argv[i], "--bca-bus-address") && i + 1 < argc) bca_bus_address = argv[++i];
    else if (!strcmp(argv[i], "--pimu-proc-root") && i + 1 < argc) pimu_proc = argv[++i];
    else if (!strcmp(argv[i], "--pimu-ready-flag") && i + 1 < argc) pimu_ready = argv[++i];
    else if (!strcmp(argv[i], "--pimu-ready-timeout-ms") && i + 1 < argc) pimu_ready_ms = strtoul(argv[++i], 0, 10);
    else if (!strcmp(argv[i], "--pimu-pts-dir") && i + 1 < argc) pimu_pts = argv[++i];
    else if (!strcmp(argv[i], "--pimu-vanish-ms") && i + 1 < argc) pimu_vanish_ms = strtoul(argv[++i], 0, 10);
    else if (!strcmp(argv[i], "--pimu-pty-owner") && i + 1 < argc) pimu_owner = atol(argv[++i]);
#endif
    else { fprintf(stderr, "Usage: mx5dr-collector [--session-seconds 1..86400]\n"); return 64; }
  }
  // A verified root-owned external startup path may launch us once. Drop to
  // the existing unprivileged account before config/DBus/log access. In the
  // shipped passwd update cmu is UID 0, and service is the bus account.
  // The phone IMU receiver must open the bds PTY slave (uid 0, devpts
  // mode=600): only CAP_DAC_OVERRIDE is kept in the permitted set of the
  // receiver thread and made effective only around that open().
  bool pimu = phone_imu_wanted(root), pimu_cap = false;
#ifndef MX5_COLLECTOR_TESTING
  if (geteuid() == 0) {
    struct passwd *account = getpwnam("cmu");
    if (!account) return 77;
    if (account->pw_uid == 0) account = getpwnam("service");
    if (pimu && prctl(PR_SET_KEEPCAPS, 1, 0, 0, 0)) pimu = false;
    if (!account || account->pw_uid == 0 || setgroups(0, 0) ||
        setgid(account->pw_gid) || setuid(account->pw_uid)) return 77;
    if (pimu) {
      pimu_cap = set_caps(1u << CAP_DAC_OVERRIDE, 0);
      prctl(PR_SET_KEEPCAPS, 0, 0, 0, 0);
      if (!pimu_cap) set_caps(0, 0);
    }
  }
#endif
  // Initialization is private to this process, before any DBus operation.
  if (!dbus_threads_init_default() || !setlocale(LC_NUMERIC, "C")) return 70;
  char cfg[512], lock_path[512], stop_path[512], pid_path[512], logs_path[512];
  if (strlen(root) > 400) return 64;
  snprintf(cfg, sizeof cfg, "%s/mx5dr.conf", root);
  snprintf(lock_path, sizeof lock_path, "%s/logs/collector.lock", root);
  snprintf(stop_path, sizeof stop_path, "%s/logs/collector.stop", root);
  snprintf(pid_path, sizeof pid_path, "%s/logs/collector.pid", root);
  snprintf(logs_path, sizeof logs_path, "%s/logs", root);
  config = mx5::runtime::read_config(cfg);
  if (!config.valid || config.mode == 0) return 78;
  // Separate disk budget: at most 2 MiB total for this low-rate stream.
  if (config.max_log_bytes > 4194304) config.max_log_bytes = 4194304;
  if (config.max_log_files > 2) config.max_log_files = 2;
  // The quiet always-on profile keeps only a small collector ring (2 MiB).
  if (config.log_profile == mx5::runtime::LOG_PROFILE_PERSISTENT && config.max_log_bytes > 1048576)
    config.max_log_bytes = 1048576;
  // Kernel lock survives no process death: stale PID files never authorize kill.
  struct stat st;
  // The installer owns the parent and assigns logs to the selected account.
  // Do not leave root-owned singleton/log files that block its next invocation.
  if (lstat(logs_path, &st) || !S_ISDIR(st.st_mode) || st.st_uid != geteuid()) return 73;
  int lock_fd = open(lock_path, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (lock_fd < 0 || fstat(lock_fd, &st) || !S_ISREG(st.st_mode)) return 73;
  if (flock(lock_fd, LOCK_EX | LOCK_NB)) { close(lock_fd); return 73; }
  if (remove(stop_path) && errno != ENOENT) return 73;
  int pid_fd = open(pid_path, O_WRONLY | O_CREAT | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (pid_fd < 0 || fstat(pid_fd, &st) || !S_ISREG(st.st_mode) || ftruncate(pid_fd, 0)) return 73;
  char pid[32];
  int pid_length = snprintf(pid, sizeof pid, "%ld\n", (long)getpid());
  bool pid_ok = write(pid_fd, pid, pid_length) == pid_length;
  close(pid_fd);
  if (!pid_ok) return 73;
  struct sigaction action;
  memset(&action, 0, sizeof action);
  action.sa_handler = stop_signal;
  sigemptyset(&action.sa_mask);
  sigaction(SIGTERM, &action, 0);
  sigaction(SIGINT, &action, 0);
  Journal journal(root);
  char line[512];
  char boot_id[37];
  mx5::runtime::read_boot_id(boot_id);
  if (config.mode != 4 && config.mode != 5) pimu = false;
  mx5::pimu::PimuJournal pimu_journal(root);
  mx5::pimu::RxOptions pimu_options = {root, bca_bus_address, pimu_proc, pimu_ready, mx5::pimu::BT_XML, boot_id,
                                       uint64_t(pimu_ready_ms) * 1000000ULL, pimu_cap, geteuid(), pimu_pts,
                                       uint64_t(pimu_vanish_ms) * 1000000ULL};
#ifndef MX5_COLLECTOR_TESTING
  pimu_options.pty_owner = 0;
  (void)pimu_owner;
#else
  if (pimu_owner >= 0) pimu_options.pty_owner = uid_t(pimu_owner);
#endif
  mx5::pimu::Receiver *receiver = pimu ? new mx5::pimu::Receiver(pimu_options) : 0;
  if (receiver && !receiver->start()) { delete receiver; receiver = 0; }
#ifndef MX5_COLLECTOR_TESTING
  // The main thread keeps no capability; the receiver thread has its own copy.
  if (pimu_cap) set_caps(0, 0);
#endif
  snprintf(line, sizeof line,
           "{\"kind\":\"collector_boot\",\"schema\":1,\"sample_ms\":%u,"
           "\"boot_id\":\"%s\","
           "\"session_seconds\":%u,\"max_log_bytes\":%u,\"max_log_files\":%u}",
           config.sample_ms, boot_id, session_seconds, unsigned(config.max_log_bytes), config.max_log_files);
  journal.line(line);
  journal.flush();
  DBusConnection *connection = 0;
  uint64_t serial = 0, started = clock_ns(0);
  const char *reason = "session_limit";
  while (!journal.failed && !stop_requested &&
         clock_ns(0) - started < uint64_t(session_seconds) * 1000000000ULL) {
    if (access(stop_path, F_OK) == 0) { reason = "stop_marker"; break; }
    mx5::runtime::Config current = mx5::runtime::read_config(cfg);
    if (!current.valid || current.mode == 0) { reason = "config_disabled"; break; }
    if (connection && !dbus_connection_get_is_connected(connection)) {
      dbus_connection_close(connection); dbus_connection_unref(connection); connection = 0;
    }
    if (!connection) connection = connect_bus();
    sensor_log(journal, connection, serial++);
    journal.flush();
#ifdef MX5_COLLECTOR_TESTING
    if (max_samples && serial >= max_samples) { reason = "test_sample_limit"; break; }
#endif
    // No shared memory, IPC, signal, or restart link to the AA process.
    unsigned waited = 0;
    while (!stop_requested && waited < config.sample_ms) {
      if (receiver) receiver->drain(pimu_journal);
      if (access(stop_path, F_OK) == 0) break;
      struct timespec pause = {0, 50000000};
      nanosleep(&pause, 0);
      waited += 50;
    }
  }
  if (receiver) {
    receiver->stop();
    receiver->drain(pimu_journal);
    delete receiver;
  }
  if (stop_requested) reason = "signal";
  snprintf(line, sizeof line, "{\"kind\":\"collector_stop\",\"reason\":\"%s\",\"samples\":%llu}",
           reason, (unsigned long long)serial);
  journal.line(line); journal.flush();
  if (connection) { dbus_connection_close(connection); dbus_connection_unref(connection); }
  unlink(pid_path);
  close(lock_fd);
  return journal.failed ? 74 : 0;
}
