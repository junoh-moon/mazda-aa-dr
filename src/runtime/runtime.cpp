#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../adapter/adapter.h"
#include "../adapter/request_hooks.h"
#include "config.h"
#include "loader.h"
#include "boot_id.h"
#include "sha256.h"
#include "motion_batch.h"
#include "shadow_log.h"
#include "request_log.h"
#include "worker_tick.h"
#include "journal_queue.h"
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
typedef mx5::runtime::JournalQueue<A::Observation,256> ObservationQueue;
ObservationQueue queue;
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
  if (queue.push(*o) == ObservationQueue::FULL) disable_mutation();
}
void freeze_capture() {
  queue.close();
  A::set_mode(A::OBSERVE);
}
bool pop(A::Observation *out) { return queue.pop(out); }

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
bool format_observation(char* line,size_t capacity,const A::Observation& o) {
  char request[1800];
  if(!mx5::runtime::format_request_trace(request,sizeof request,o.request_result,o.request_trace))return false;
  int n;
  if(o.kind==A::Observation::POSITION) {
    char lat[48],lon[48],h[48],v[48];
    json_number(o.position.latitude_deg,lat);json_number(o.position.longitude_deg,lon);
    json_number(o.position.heading_deg,h);json_number(o.position.velocity_kmh,v);
    n=snprintf(line,capacity,
      "{\"kind\":\"position\",\"call\":%u,\"generation\":%u,\"mono_ns\":%llu,"
      "\"mode\":%d,\"utc_s\":%llu,\"lat\":%s,\"lon\":%s,\"heading\":%s,\"kmh\":%s,\"request\":%s}",
      o.call_sequence,o.prediction_generation,(unsigned long long)o.mono_ns,o.original_mode,
      (unsigned long long)o.position.utc_seconds,lat,lon,h,v,request);
  } else {
    char a[97]="",b[97]="";
    if(o.has_payload) { hex48(o.original,a);hex48(o.outgoing,b); }
    n=snprintf(line,capacity,
      "{\"kind\":\"send\",\"call\":%u,\"generation\":%u,\"mono_ns\":%llu,\"mode\":%d,"
      "\"type\":%u,\"length\":%u,\"choice\":%u,\"reason\":%u,\"result\":%d,"
      "\"original_hex\":\"%s\",\"outgoing_hex\":\"%s\",\"request\":%s}",
      o.call_sequence,o.prediction_generation,(unsigned long long)o.mono_ns,o.original_mode,
      o.type,o.length,unsigned(o.choice),unsigned(o.reason),o.result,a,b,request);
  }
  return n>0 && size_t(n)<capacity;
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

bool format_motion_rejected(char* line,size_t capacity,const N::ReceiveDiagnostic& d) {
  const N::RawEvent& r=d.rejected;
  const int n=snprintf(line,capacity,
      "{\"kind\":\"motion_rejected\",\"schema\":1,\"domain\":\"model\","
      "\"assist_ready\":false,\"producer_time_status\":\"unknown\","
      "\"authenticated_decoded\":true,\"reason\":\"%s\",\"checked_ns\":%llu,"
      "\"sender_pid\":%ld,\"sender_uid\":%lu,\"sensor\":%u,\"epoch\":%llu,"
      "\"receive_seq\":%llu,\"received_ns\":%llu,\"source_mono_ms\":%lld,"
      "\"raw\":[%u,%u,%u,%u],\"count\":%u,\"reverse\":%d}",
      N::receive_fault_name(d.reason),(unsigned long long)d.checked_ns,
      (long)d.sender_pid,(unsigned long)d.sender_uid,unsigned(r.kind),
      (unsigned long long)r.epoch,(unsigned long long)r.receive_seq,
      (unsigned long long)r.received_ns,(long long)r.source_mono_ms,
      unsigned(r.raw[0]),unsigned(r.raw[1]),unsigned(r.raw[2]),unsigned(r.raw[3]),
      unsigned(r.count),r.reverse);
  return d.authenticated_decoded && n>0 && size_t(n)<capacity;
}

// One bounded worker receive turn. Capture survives model/AA audit failure;
// rejected input is separate evidence and can never enter either estimator.
template<class Receiver>
void drain_motion(Journal& j,mx5::runtime::MotionBatch& batch,Receiver& motion,
                  N::Pipeline& navigation,N::GpsHoldout& holdout,bool compute) {
  for(unsigned i=0;i<256 && !j.failed;++i) {
    N::RawEvent raw=N::RawEvent();
    N::ReceiveDiagnostic d=N::ReceiveDiagnostic();
    const N::ReceiveResult received=motion.receive(clock_ns(0),&raw,&d);
    if(received==N::CHANNEL_EMPTY)break;
    const bool enabled=compute && !__sync_fetch_and_add(&audit_fault,0);
    if(received==N::CHANNEL_FAULT) {
      flush_motion(j,batch);
      char line[1200];
      if(d.authenticated_decoded) {
        if(format_motion_rejected(line,sizeof line,d))j.line(line);
        else j.fail();
      }
      if(enabled) {
        mx5_dr_context c=navigation.context();
        if(c.source_epoch==UINT64_MAX || c.generation==UINT64_MAX) {
          disable_mutation();break;
        }
        ++c.source_epoch;++c.generation;navigation.reset(c);
        holdout.reset(c,N::HOLDOUT_SOURCE_FAULT);
      }
      snprintf(line,sizeof line,
          "{\"kind\":\"shadow_input_reset\",\"mono_ns\":%llu,\"reason\":\"%s\","
          "\"credentials_present\":%s,\"sender_pid\":%ld,\"sender_uid\":%lu,"
          "\"syscall_errno\":%d,\"computation_active\":%s,\"assist_ready\":false}",
          (unsigned long long)d.checked_ns,N::receive_fault_name(d.reason),
          d.credentials_present?"true":"false",(long)d.sender_pid,(unsigned long)d.sender_uid,
          d.syscall_errno,enabled?"true":"false");
      j.line(line);
    } else {
      if(enabled) { navigation.enqueue_raw(raw);holdout.enqueue_raw(raw); }
      journal_motion(j,batch,raw);
    }
  }
  // No batch crosses the worker sleep, including a capped or failed turn.
  flush_motion(j,batch);
}

void journal_health(Journal& j,uint64_t now,bool capture,bool computation) {
  const A::RequestHookHealth h=A::request_hook_health();
  char line[1000];
  const int n=snprintf(line,sizeof line,
      "{\"kind\":\"health\",\"mono_ns\":%llu,\"dropped\":%llu,\"hook_installed\":%s,"
      "\"runtime_mode\":%u,\"audit_fault\":%u,\"capture_active\":%s,"
      "\"computation_active\":%s,\"assist_ready\":false,\"request_observer\":{"
      "\"prepared\":%s,\"abi_fault\":%s,\"result\":\"%s\",\"loss_epoch\":%llu,"
      "\"requests\":%u,\"workers\":%u,\"loss_reasons\":%u,\"exhausted\":%s}}",
      (unsigned long long)now,(unsigned long long)queue.dropped(),hook_installed?"true":"false",
      unsigned(A::mode()),__sync_fetch_and_add(&audit_fault,0),capture?"true":"false",
      computation?"true":"false",h.prepared?"true":"false",h.abi_fault?"true":"false",
      A::R::result_name(h.result),(unsigned long long)h.ledger.loss_epoch,h.ledger.requests,
      h.ledger.workers,h.ledger.loss_reasons,h.ledger.exhausted?"true":"false");
  if(n<=0 || size_t(n)>=sizeof line)j.fail();else j.line(line);
}
bool stop_requested(const char* root) {
  char path[256];snprintf(path,sizeof path,"%s/logs/capture.stop",root);
  struct stat st;
  return lstat(path,&st)==0 && S_ISDIR(st.st_mode);
}
// A close prevents new reservations but a preempted producer may still own
// an unpublished one. Only this worker waits, with a finite retry budget.
// At most 256 accepted observations remain after close. A stuck producer
// leaves the capture incomplete; absence of a ready head is not completion.
bool drain_capture_tail(Journal& j) {
  if(!queue.closed()) { j.fail();return false; }
  for(unsigned attempt=0;attempt<100;++attempt) {
    A::Observation o;
    for(unsigned n=0;n<256 && pop(&o);++n) {
      char line[2200];
      if(format_observation(line,sizeof line,o))j.line(line);else j.fail();
    }
    if(queue.drained())return true;
    const struct timespec pause={0,1000000};
    nanosleep(&pause,0);
  }
  j.line("{\"kind\":\"capture_incomplete\",\"reason\":\"observation_pending\",\"assist_ready\":false}");
  j.fail();j.flush();
  return false;
}
// Only after input is frozen and the bounded final drain has completed.
// No acknowledgement can precede durable terminal records.
bool finish_capture(Journal& j,const char* boot_id,uint64_t cutoff,uint64_t now) {
  if(!cutoff || now<cutoff || !queue.drained()) { j.fail();return false; }
  // A producer may have returned its failed reservation before the sink's
  // disable_mutation call. The closed+drained acquire covers its sticky loss.
  if(queue.lost())disable_mutation();
  char line[400];
  snprintf(line,sizeof line,
      "{\"kind\":\"capture_end\",\"schema\":1,\"mono_ns\":%llu,\"boot_id\":\"%s\","
      "\"domain\":\"model\",\"assist_ready\":false,\"reason\":\"requested\","
      "\"cutoff_ns\":%llu,\"bounded_final_drain\":true}",
      (unsigned long long)now,boot_id,(unsigned long long)cutoff);
  j.line(line);
  journal_health(j,now,false,false);
  j.flush();
  if(j.failed || !j.f || fsync(fileno(j.f))) { j.fail();return false; }
  const bool close_failed=fclose(j.f)!=0;j.f=0;
  if(close_failed) { j.fail();return false; }
  if(!strcmp(boot_id,"unknown"))return false;
  char temporary[256],done[256],directory[256];
  snprintf(directory,sizeof directory,"%s/logs",j.root);
  snprintf(temporary,sizeof temporary,"%s/logs/capture.done.%ld.tmp",j.root,(long)getpid());
  snprintf(done,sizeof done,"%s/logs/capture.done",j.root);
  int dirfd=open(directory,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
  if(dirfd<0)return false;
  int fd=open(temporary,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
  if(fd<0) { close(dirfd);return false; }
  const int n=snprintf(line,sizeof line,"%s\n",boot_id);
  bool ok=write(fd,line,size_t(n))==n && fsync(fd)==0;
  if(close(fd))ok=false;
  if(ok)ok=rename(temporary,done)==0;
  if(ok)ok=fsync(dirfd)==0;
  if(!ok) { unlink(temporary);unlink(done);fsync(dirfd); }
  close(dirfd);
  return ok;
}

void *worker_at(const char* root) {
  // An explicit stop survives same-boot service restarts. Do not rotate or
  // append even a boot record after an acknowledged capture was closed.
  if(stop_requested(root)) { freeze_capture();return 0; }
  locale_t numeric_locale = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
  if (!numeric_locale) {
    disable_mutation();
    return 0;
  }
  uselocale(numeric_locale);
  Journal j(root);
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
  const bool capture=config.mode==4 && motion.open_channel();
  bool shadow=capture && hook_installed &&
      navigation.init_model(model,mx5_dr_default_config(),nav_context,true,true) &&
      holdout.init_model(model,mx5_dr_default_config(),nav_context);
  if(config.mode==4) {
    snprintf(line,sizeof line,
        "{\"kind\":\"shadow_boot\",\"active\":%s,\"capture_active\":%s,\"domain\":\"model\","
        "\"source\":\"existing_vbs_vim_callback\",\"assist_ready\":false,"
        "\"motion_log_format\":\"motion_batch_v1\",\"motion_sampling\":false,"
        "\"stationary_bias_model\":true,\"gps_holdout_model\":true,"
        "\"gps_anchor_gate_model\":true,\"wheel_scale_model\":true,"
        "\"yaw_zero\":%.9g,\"yaw_rad_per_count\":%.9g,"
        "\"wheel_kmh_per_count\":%.9g,\"wheel_zero_kmh\":%.9g,"
        "\"reverse_forward\":%d,\"reverse_reverse\":%d,\"reorder_ns\":%llu}",
        shadow?"true":"false",capture?"true":"false",model.yaw_zero,model.yaw_rad_per_count,
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
  uint64_t last_stop_check=0;
  mx5::runtime::WorkerTick model_tick;
  for (;;) {
    const uint64_t cutoff=clock_ns(0);
    bool stopping=false;
    if(cutoff>=last_stop_check && cutoff-last_stop_check>=1000000000ULL) {
      last_stop_check=cutoff;
      stopping=stop_requested(root);
      if(stopping) {
        freeze_capture();
        if(shadow) {
          navigation.reset(navigation.context());
          holdout.reset(navigation.context(),N::HOLDOUT_CAPTURE_STOP);
          journal_holdout(j,holdout,cutoff);
        }
        shadow=false;
      }
    }
    A::Observation o;
    unsigned drained = 0;
    while (drained++ < 256 && pop(&o)) {
      if (o.kind == A::Observation::POSITION) {
        if(shadow && !__sync_fetch_and_add(&audit_fault,0)) {
          navigation.enqueue_position(o);
          holdout.enqueue_position(o);
        }
      }
      if(!format_observation(line,sizeof line,o)) { j.fail();continue; }
      j.line(line);
    }
    uint64_t now = clock_ns(0);
    if(shadow && (j.failed || __sync_fetch_and_add(&audit_fault,0))) {
          navigation.reset(navigation.context());
          holdout.reset(navigation.context(),N::HOLDOUT_AUDIT_RESET);
          journal_holdout(j,holdout,now);
          j.line("{\"kind\":\"shadow_disabled\",\"reason\":\"audit_fault\",\"assist_ready\":false}");
          shadow=false; // Permanent for this worker, even if a fault flag changes.
    }
    if(capture && !j.failed)drain_motion(j,motion_batch,motion,navigation,holdout,shadow);
    if(stopping) {
      if(drain_capture_tail(j))finish_capture(j,boot_id,cutoff,clock_ns(0));
      return 0; // Even failed finalization cannot reopen this capture.
    }
    now=clock_ns(0);
    if(shadow && !j.failed && !__sync_fetch_and_add(&audit_fault,0) && model_tick.due(now)) {
        if(now>navigation.reorder_ns())navigation.drain(now-navigation.reorder_ns());
        if(now>navigation.reorder_ns())holdout.drain(now-navigation.reorder_ns());
        journal_holdout(j,holdout,now);
        if(now>=last_calibration_log && now-last_calibration_log>=1000000000ULL) {
          last_calibration_log=now;
          if(mx5::runtime::format_shadow_calibration(line,sizeof line,now,navigation.calibration(),
              navigation.wheel_calibration(),N::anchor_gate_name(navigation.anchor_gate())))
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
              "\"yaw_zero\":%.17g,\"calibration_version\":%llu,\"wheel_scale\":%.17g,"
              "\"wheel_scale_version\":%llu,\"preview_encoded\":%s,\"location_preview_hex\":\"%s\"}",
              (unsigned long long)now,d.snapshot.model_valid?"true":"false",unsigned(d.snapshot.state),
              mx5_dr_result_name(d.result),N::pipeline_result_name(d.status.result),d.status.uncertainties,
              (unsigned long long)d.status.events,(unsigned long long)d.status.intervals,
              (unsigned long long)d.status.resets,(unsigned long long)d.status.rejected,
              (unsigned long long)d.snapshot.frontier_ns,lat,lon,heading,speed,error,
              d.snapshot.stopped?"true":"false",navigation.calibration().active_zero,
              (unsigned long long)navigation.calibration().calibration_version,
              navigation.wheel_calibration().active_scale,
              (unsigned long long)navigation.wheel_calibration().calibration_version,encoded?"true":"false",preview);
          j.line(line);
        }
    }
    if (now - last_flush >= 1000000000ULL) {
      last_flush = now;
      journal_health(j,now,capture&&!j.failed,shadow);
      j.flush();
    }
    // The stock unconnected Unix-datagram queue is small. Wake on motion
    // arrival instead of accumulating bursts across an unconditional sleep.
    // Keep MODEL computation on its 50 ms deadline even during frequent input.
    // Merely gating it by elapsed time would shift the tick with input cadence.
    // A failed wait falls back to the bounded sleep, avoiding an error spin.
    const unsigned wait_ms=shadow?model_tick.wait_ms(clock_ns(0)):50;
    if(capture && !j.failed && motion.wait_for_input(wait_ms)>=0)continue;
    struct timespec pause = {0, 50000000};
    nanosleep(&pause, 0);
  }
  return 0;
}
void* worker(void*) { return worker_at(ROOT); }

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
    io.observe_requests = true;
    io.blm_handle = h;
    io.runtime.request_reader = A::read_request_trace;
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
