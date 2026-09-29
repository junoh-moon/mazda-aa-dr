#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../adapter/adapter.h"
#include "config.h"
#include "loader.h"
#include "boot_id.h"
#include "sha256.h"
#include "motion_batch.h"
#include "shadow_log.h"
#include "navigation/channel.h"
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
namespace N = mx5::navigation;
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
  if (std::isfinite(x))
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

void flush_motion(Journal &j, mx5::runtime::MotionBatch &batch) {
  if (!batch.empty()) {
    j.line(batch.line());
    batch.clear();
  }
}
void journal_motion(Journal &j, mx5::runtime::MotionBatch &batch,
                    const N::RawEvent &raw) {
  if (!batch.append(raw)) {
    flush_motion(j, batch);
    if (!batch.append(raw))
      j.fail(); // An unrepresentable record is an audit fault, not sampling.
  }
}

void journal_holdout(Journal &j,N::GpsHoldout &holdout,uint64_t now) {
  N::HoldoutResult result;
  char line[2200];
  while(holdout.pop(&result)) {
    if(mx5::runtime::format_shadow_holdout(line,sizeof line,now,result))j.line(line);
    else j.fail();
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
  mx5::runtime::MotionBatch motion_batch;
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
  N::Pipeline navigation;
  N::GpsHoldout holdout;
  N::MotionReceiver motion;
  const N::ModelProfile model=N::research_model_profile();
  mx5_dr_context nav_context={1,1,1}; // local diagnostic identity, not LDS provenance
  bool shadow=config.mode==4 && hook_installed &&
      navigation.init_model(model,mx5_dr_default_config(),nav_context,true) &&
      holdout.init_model(model,mx5_dr_default_config(),nav_context) &&
      motion.open_channel();
  if(config.mode==4) {
    snprintf(line,sizeof line,
        "{\"kind\":\"shadow_boot\",\"active\":%s,\"domain\":\"model\","
        "\"source\":\"existing_vbs_vim_callback\",\"assist_ready\":false,"
        "\"motion_log_format\":\"motion_batch_v1\",\"motion_sampling\":false,"
        "\"stationary_bias_model\":true,\"gps_holdout_model\":true,"
        "\"yaw_zero\":%.9g,\"yaw_rad_per_count\":%.9g,"
        "\"wheel_kmh_per_count\":%.9g,\"wheel_zero_kmh\":%.9g,"
        "\"reverse_forward\":%d,\"reverse_reverse\":%d,\"reorder_ns\":%llu}",
        shadow?"true":"false",model.yaw_zero,model.yaw_rad_per_count,
        model.wheel_kmh_per_count,model.wheel_zero_kmh,
        model.reverse_forward_value,model.reverse_reverse_value,
        (unsigned long long)model.reorder_ns);
    j.line(line);j.flush();
  }
  if (hook_installed && config.mode == 2 && !j.failed &&
      !__sync_fetch_and_add(&audit_fault, 0)) {
    A::set_mode(A::SCRUB_STALE);
    if (__sync_fetch_and_add(&audit_fault, 0))
      A::set_mode(A::OBSERVE);
  }
  uint64_t last_flush = 0;
  uint64_t last_shadow_log=0;
  uint64_t last_calibration_log=0;
  bool shadow_audit_reported=false;
  for (;;) {
    A::Observation o;
    unsigned drained = 0;
    while (drained++ < 256 && pop(&o)) {
      if (o.kind == A::Observation::POSITION) {
        if(shadow && !__sync_fetch_and_add(&audit_fault,0)) {
          navigation.enqueue_position(o);
          holdout.enqueue_position(o);
        }
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
    if(shadow) {
      if(j.failed || __sync_fetch_and_add(&audit_fault,0)) {
        if(!shadow_audit_reported) {
          navigation.reset(navigation.context());
          holdout.reset(navigation.context(),N::HOLDOUT_AUDIT_RESET);
          journal_holdout(j,holdout,now);
          j.line("{\"kind\":\"shadow_disabled\",\"reason\":\"audit_fault\",\"assist_ready\":false}");
          shadow_audit_reported=true;
        }
      } else {
        for(unsigned i=0;i<256;++i) {
          N::RawEvent raw=N::RawEvent();
          const N::ReceiveResult received=motion.receive(clock_ns(0),&raw);
          if(received==N::CHANNEL_EMPTY)break;
          if(received==N::CHANNEL_FAULT) {
            flush_motion(j,motion_batch);
            mx5_dr_context c=navigation.context();
            if(c.source_epoch==UINT64_MAX || c.generation==UINT64_MAX) {
              disable_mutation();break;
            }
            ++c.source_epoch;++c.generation;navigation.reset(c);
            holdout.reset(c,N::HOLDOUT_SOURCE_FAULT);
            j.line("{\"kind\":\"shadow_input_reset\",\"reason\":\"channel_discontinuity\",\"assist_ready\":false}");
          } else {
            navigation.enqueue_raw(raw);
            holdout.enqueue_raw(raw);
            journal_motion(j,motion_batch,raw);
          }
        }
        // Empty socket, 256-event turn limit and faults all leave no batch
        // pending across the worker's sleep or the next POSITION observation.
        flush_motion(j,motion_batch);
        now=clock_ns(0);
        if(now>navigation.reorder_ns())navigation.drain(now-navigation.reorder_ns());
        if(now>navigation.reorder_ns())holdout.drain(now-navigation.reorder_ns());
        journal_holdout(j,holdout,now);
        if(now>=last_calibration_log && now-last_calibration_log>=1000000000ULL) {
          last_calibration_log=now;
          if(mx5::runtime::format_shadow_calibration(line,sizeof line,now,navigation.calibration()))
            j.line(line);
          else j.fail();
        }
        if(now>=last_shadow_log && now-last_shadow_log>=100000000ULL) {
          last_shadow_log=now;
          const N::Diagnostic d=navigation.diagnostic(now);
          char lat[48],lon[48],heading[48],speed[48],error[48],preview[97]="";
          json_number(d.snapshot.latitude_deg,lat);json_number(d.snapshot.longitude_deg,lon);
          json_number(d.snapshot.body_heading_rad,heading);json_number(d.snapshot.speed_mps,speed);
          json_number(d.snapshot.error_budget_m,error);
          uint8_t bytes[48];
          const bool encoded=mx5::runtime::encode_model_location_preview(d.snapshot,bytes);
          if(encoded)hex48(bytes,preview);
          snprintf(line,sizeof line,
              "{\"kind\":\"shadow\",\"mono_ns\":%llu,\"domain\":\"model\","
              "\"model_valid\":%s,\"assist_ready\":false,\"state\":%u,"
              "\"result\":\"%s\",\"pipeline\":\"%s\",\"uncertainties\":%u,"
              "\"events\":%llu,\"intervals\":%llu,\"resets\":%llu,\"rejected\":%llu,"
              "\"frontier_ns\":%llu,\"lat\":%s,\"lon\":%s,\"heading_rad\":%s,"
              "\"speed_mps\":%s,\"error_model_m\":%s,\"stopped\":%s,"
              "\"yaw_zero\":%.17g,\"calibration_version\":%llu,\"preview_encoded\":%s,\"location_preview_hex\":\"%s\"}",
              (unsigned long long)now,d.snapshot.model_valid?"true":"false",unsigned(d.snapshot.state),
              mx5_dr_result_name(d.result),N::pipeline_result_name(d.status.result),d.status.uncertainties,
              (unsigned long long)d.status.events,(unsigned long long)d.status.intervals,
              (unsigned long long)d.status.resets,(unsigned long long)d.status.rejected,
              (unsigned long long)d.snapshot.frontier_ns,lat,lon,heading,speed,error,
              d.snapshot.stopped?"true":"false",navigation.calibration().active_zero,
              (unsigned long long)navigation.calibration().calibration_version,encoded?"true":"false",preview);
          j.line(line);
        }
      }
    }
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
