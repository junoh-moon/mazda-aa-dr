#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../adapter/adapter.h"
#include "../adapter/request_hooks.h"
#include "../adapter/session_hooks.h"
#include "config.h"
#include "loader.h"
#include "boot_id.h"
#include "storage.h"
#include "sha256.h"
#include "motion_batch.h"
#include "shadow_log.h"
#include "request_log.h"
#include "../adapter/bus_hooks.h"
#include "worker_tick.h"
#include "worker.h"
#include "worker_thread.h"
#include "assist_worker.h"
#include "journal_queue.h"
#include "journal_ring.h"
#include "log_profile.h"
#include "model_session.h"
#include "beta_controller.h"
#include "motion_gap.h"
#include "model_bus.h"
#include "lds_sideband.h"
#include "lds_request_source.h"
#include "lds_source_bus.h"
#include "lds_association_channel.h"
#include "navigation/channel.h"
#include <atomic>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <math.h>
#include <new>
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
// 1 while every journal row has been accepted for writing. Any journal failure
// (worker or writer thread, file error, or an evidence row that cannot be
// queued) clears it before disabling mutation. The OEM POSITION thread reads
// it lock-free in provenance(), so a BETA claim stops even before the worker
// observes the failure. Constant-initialized; never set back to 1 in a boot.
std::atomic<unsigned> journal_ok(1);
static_assert(ATOMIC_INT_LOCK_FREE==2,"journal_ok is read by OEM threads");
// 1 while the journal writer keeps up. A storage stall no longer blocks the
// worker (the writer thread does the I/O), so without this a long eMMC stall
// would keep BETA replacing while its evidence rows sit only in RAM. The
// worker lowers it when the oldest unflushed row is older than
// JOURNAL_LAG_LIMIT_NS and raises it again below JOURNAL_LAG_CLEAR_NS
// (hysteresis, not sticky). provenance() reads it lock-free with journal_ok.
std::atomic<unsigned> journal_current(1);
const uint64_t JOURNAL_LAG_LIMIT_NS=1500000000ULL;
const uint64_t JOURNAL_LAG_CLEAR_NS=500000000ULL;
// The writer fflushes on its own once the oldest row handed to stdio since
// the last fflush is this old (2026-10-07 follow-up). Rows in the stdio
// buffer count as journal lag, so without it the steady-state lag rose to the
// worker's 1 s flush period, leaving about 0.4 s of margin to
// JOURNAL_LAG_LIMIT_NS. At most about 4 fflush calls per second; fsync policy
// unchanged (capture stop only).
const uint64_t JOURNAL_FLUSH_MAX_AGE_NS=250000000ULL;
mx5::runtime::Config config = {0, 8388608, 3, 1000, false, mx5::runtime::LOG_PROFILE_FULL};
const char *boot_result = "not_attempted";
bool hook_installed = false;
mx5::adapter::InstallReport install_report = mx5::adapter::InstallReport();
// BETA (config mode 5) state shared with OEM POSITION/SEND threads. Static,
// zero-initialized, lock-free atomics only; see beta_controller.h.
mx5::runtime::BetaShared beta_shared;
namespace LA=mx5::runtime::lds_association;
// Construct off the callback on the worker; retain until process exit. A later
// DSO constructor/destructor must not reset or free state an OEM callback uses.
alignas(LA::Registry) unsigned char association_storage[sizeof(LA::Registry)];
std::atomic<LA::Registry*> association_owner(0);
std::atomic<unsigned> association_started(0);
void association_child() {
  LA::Registry* owner=association_owner.load(std::memory_order_acquire);
  if(owner)owner->disable_after_fork();
}
LA::Registry* prepare_association_owner() {
  unsigned expected=0;
  if(!association_started.compare_exchange_strong(expected,1))return 0;
  LA::Registry* owner=new(association_storage)LA::Registry();
  if(pthread_atfork(0,0,association_child))owner->disable_after_fork();
  association_owner.store(owner,std::memory_order_release);
  return owner;
}
void retire_association() {
  LA::Registry* owner=association_owner.load(std::memory_order_acquire);
  if(owner)owner->retire();
}
bool read_inline_association(const A::PositionContext& context,
                             mx5::runtime::lds_association::Owned* out,void*) {
  *out=LA::Owned();
  LA::Registry* owner=association_owner.load(std::memory_order_acquire);
  if(!owner || queue.closed() || __sync_fetch_and_add(&audit_fault,0))return false;
  const bool matched=owner->read(context,out);
  if(queue.closed() || __sync_fetch_and_add(&audit_fault,0)) {
    *out=LA::Owned();return false;
  }
  return matched;
}
void disable_mutation() {
  retire_association();
  if (__sync_bool_compare_and_swap(&audit_fault, 0, 1))
    A::set_mode(A::OBSERVE);
}
#ifdef MX5DR_SIMULATED_CLOCK
// Host measurement seam only (tests/runtime/log_rate.cpp): a simulated drive
// clock for rows and journal-profile decisions. Never defined in a product
// or ARM build.
uint64_t simulated_clock_ns = 0;
uint64_t clock_ns(void *) { return simulated_clock_ns; }
#else
uint64_t clock_ns(void *) {
  struct timespec t;
  if (clock_gettime(CLOCK_MONOTONIC, &t))
    return 0;
  return uint64_t(t.tv_sec) * 1000000000ULL + t.tv_nsec;
}
#endif
void sink(const A::Observation *o, void *) {
  if (queue.push(*o) == ObservationQueue::FULL) disable_mutation();
}
void freeze_capture() {
  queue.close();
  retire_association();
  A::set_mode(A::OBSERVE);
}
bool pop(A::Observation *out) { return queue.pop(out); }

// No live provenance or sensor freshness is fabricated from polling. SCRUB
// uses only the original request mode; custom DR remains a separate gate.
// BETA (MODEL domain) provenance never sets a qualified flag or
// Domain::QUALIFIED; see beta_provenance() for what each field means.
bool provenance(void *, const A::PositionContext& context, A::Provenance *out, void *) {
  // A failed journal can no longer record the evidence of a replacement.
  // A journal that is not writing its rows promptly withholds BETA too.
  if (!journal_ok.load(std::memory_order_acquire) ||
      !journal_current.load(std::memory_order_acquire)) { memset(out, 0, sizeof *out); return false; }
  if (mx5::runtime::beta_provenance(beta_shared, out)) return true;
  memset(out, 0, sizeof *out);
  if(!context.lds_association ||
     context.lds_association->result!=LA::MATCHED_LOCKED_FOR_SEND)return false;
  // TODO: qualify provider, receiver and physical sensor time/quality through
  // their independent verified source. An exact observed wire association
  // alone cannot supply epochs or physical qualification for live ASSIST.
  return false;
}
// OEM SEND thread, BETA only: the design S3 storage fence, called by the
// adapter right after the product session reader (which the v74 installer
// requires unchanged, BETA_DECISIONS 3.7). Lock-free; no allocation, I/O or
// dereference of storage.
void observe_send_storage_beta(void*, const void* storage) {
  mx5::runtime::beta_observe_storage(beta_shared, storage);
}
// OEM SEND thread, BETA only: counts hold transitions for the worker journal.
void beta_event(void*, const char* what) { mx5::runtime::beta_count_event(beta_shared, what); }
// Product runtime options. Startup-only: configure() copies them before any
// OEM producer runs. BETA additions are present only for config mode 5.
A::Options product_options() {
  A::Options o = A::Options();
  o.sink = sink;
  o.clock = clock_ns;
  o.provenance = provenance;
  o.association_reader = read_inline_association;
  o.max_snapshot_age_ns = 500000000ULL;
  o.allow_assist = false; // The qualified ASSIST gate stays closed.
  o.request_reader = A::read_request_trace;
  const bool beta = config.valid && config.mode == 5;
  // Always the stock reader: install_v74 refuses any other session reader.
  o.session_reader = A::read_send_session;
  o.allow_beta = beta;
  o.beta_event = beta ? beta_event : 0;
  o.send_storage = beta ? observe_send_storage_beta : 0;
  // Decision G: the AA GEAR payload is journaled only by SHADOW (4) / BETA (5).
  o.journal_gear_payload = config.valid && (config.mode == 4 || config.mode == 5);
  return o;
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
bool format_association(char* out,size_t capacity,const LA::Owned& o) {
  const char* result=0;
  switch(o.result) {
    case LA::UNAVAILABLE:result="unavailable";break;
    case LA::MATCHED_LOCKED_FOR_SEND:result="matched_locked_for_send";break;
    case LA::CONFLICT:result="conflict";break;
    case LA::PAYLOAD_MISMATCH:result="payload_mismatch";break;
    default:return false;
  }
  mx5::runtime::request_log_detail::Json j(out,capacity);
  j.add("{\"result\":\"");j.add(result);j.add("\"");
  if(o.result==LA::MATCHED_LOCKED_FOR_SEND) {
    j.add(",\"stage\":\"locked_for_send\"");
    j.number("call",o.call_sequence);j.number("generation",o.prediction_generation);
    j.number("revision",o.view_revision);j.number("layout",o.layout_version);
    j.number("source_instance",o.source_instance);j.number("record_sequence",o.record_sequence);
    j.number("locked_observed_ns",o.locked_observed_ns);j.number("map_loss_epoch",o.map_loss_epoch);
    j.number("cache_lifetime",o.cache_lifetime);j.number("write_sequence",o.write_sequence);
    char pair[128];
    snprintf(pair,sizeof pair,",\"request\":[%llu,%llu],\"worker\":[%llu,%llu]",
        (unsigned long long)o.request_id,(unsigned long long)o.request_epoch,
        (unsigned long long)o.worker_id,(unsigned long long)o.worker_epoch);
    j.add(pair);j.add(",\"fields\":[");
    for(unsigned i=0;i<9;++i) {
      snprintf(pair,sizeof pair,"%s[%llu,%llu]",i?",":"",
          (unsigned long long)o.fields[i].write_sequence,(unsigned long long)o.fields[i].observed_ns);
      j.add(pair);
    }
    j.add("]");
    // Preserve raw POSITION/SEND even if supplemental metadata is unavailable.
    // Neither a zero heading nor a new origin sequence implies token presence.
    const auto state=o.heading_presence;
    const bool origin=o.fields[mx5::sensors::lds_lineage::HEADING].write_sequence!=0;
    const char* presence=origin&&state==mx5::sensors::nmea_course_token::EMPTY?"empty":
        origin&&state==mx5::sensors::nmea_course_token::PRESENT?"present":"unknown";
    j.add(",\"heading_presence\":\"");j.add(presence);j.add("\"");
    const auto status=o.heading_rmc_status;
    const char* labels[]={"unknown","empty","a","v","other"};
    const char* rmc=origin&&status<=mx5::sensors::nmea_course_token::RMC_OTHER?labels[status]:"unknown";
    j.add(",\"heading_rmc_status\":\"");j.add(rmc);j.add("\"");
  }
  j.add("}");return j.ok();
}
bool format_observation(char* line,size_t capacity,const A::Observation& o) {
  char request[mx5::runtime::REQUEST_JSON_CAPACITY];
  if(!mx5::runtime::format_request_trace(request,sizeof request,o.request_result,o.request_trace))return false;
  char association[1536];
  if(!format_association(association,sizeof association,o.lds_association))return false;
  int n;
  if(o.kind==A::Observation::POSITION) {
    char lat[48],lon[48],h[48],v[48],horizontal[48],vertical[48];
    json_number(o.position.latitude_deg,lat);json_number(o.position.longitude_deg,lon);
    json_number(o.position.heading_deg,h);json_number(o.position.velocity_kmh,v);
    json_number(o.position.horizontal,horizontal);json_number(o.position.vertical,vertical);
    n=snprintf(line,capacity,
      "{\"kind\":\"position\",\"call\":%u,\"generation\":%u,\"mono_ns\":%llu,"
      "\"mode\":%d,\"utc_s\":%llu,\"lat\":%s,\"lon\":%s,\"heading\":%s,\"kmh\":%s,"
      "\"altitude_m\":%d,\"horizontal\":%s,\"vertical\":%s,\"reason\":%u,\"request\":%s,\"lds_association\":%s,"
      "\"class\":%u}",
      o.call_sequence,o.prediction_generation,(unsigned long long)o.mono_ns,o.original_mode,
      (unsigned long long)o.position.utc_seconds,lat,lon,h,v,o.position.altitude_m,horizontal,vertical,
      unsigned(o.reason),request,association,unsigned(o.position_class));
  } else {
    char a[97]="",b[97]="";
    char session[200];
    if(!mx5::runtime::format_session_trace(session,sizeof session,o.send_session,true))return false;
    if(o.has_payload) { hex48(o.original,a);hex48(o.outgoing,b); }
    // Decision G: the AA GEAR payload (type 8, 4 bytes; SHADOW/BETA only).
    char small[64]="";
    if(o.small_length && o.small_length<=sizeof o.small_payload) {
      static const char digits[]="0123456789abcdef";
      char hex[33];
      for(unsigned i=0;i<o.small_length;++i) {
        hex[2*i]=digits[o.small_payload[i]>>4];hex[2*i+1]=digits[o.small_payload[i]&15];
      }
      hex[2*o.small_length]=0;
      snprintf(small,sizeof small,",\"payload_hex\":\"%s\"",hex);
    }
    n=snprintf(line,capacity,
      "{\"kind\":\"send\",\"call\":%u,\"generation\":%u,\"mono_ns\":%llu,\"mode\":%d,"
      "\"type\":%u,\"length\":%u,\"choice\":%u,\"reason\":%u,\"result\":%d,"
      "\"original_hex\":\"%s\",\"outgoing_hex\":\"%s\",\"request\":%s,\"send_session\":%s,\"lds_association\":%s,"
      "\"class\":%u%s}",
      o.call_sequence,o.prediction_generation,(unsigned long long)o.mono_ns,o.original_mode,
      o.type,o.length,unsigned(o.choice),unsigned(o.reason),o.result,a,b,request,session,association,
      unsigned(o.position_class),small);
  }
  return n>0 && size_t(n)<capacity;
}

struct JournalWriter;
void stop_journal_writer(JournalWriter*,bool durable,bool* ok);
bool journal_writer_push(JournalWriter*,const char*,size_t,mx5::runtime::JournalRing::Class);
bool journal_writer_ok(JournalWriter*);
void journal_writer_request_flush(JournalWriter*);
bool journal_writer_flush_wait(JournalWriter*,uint64_t timeout_ns);
JournalWriter* start_journal_writer(const char* root,size_t diagnostic_bytes,size_t evidence_bytes,
                                    size_t bulk_bytes);
bool journal_writer_bulk_ready(JournalWriter*);
struct JournalLag { bool thread; uint64_t unwritten_rows,oldest_ns,dropped_rows; size_t high_water; };
JournalLag journal_writer_lag(JournalWriter*,uint64_t now);

// The worker's journal. By default (and in unit tests) rows are written
// synchronously by the calling thread. The runtime worker calls
// start_writer(): rows are then queued in a preallocated JournalRing and a
// dedicated writer thread does every fwrite/fflush/statvfs/rotation, so a
// storage stall cannot block motion reception (2026-10-06). The writer
// thread itself uses a synchronous Journal as its file backend.
struct Journal {
  const char *root;
  FILE *f;
  size_t written;
  uint64_t total_bytes;   // bytes written by this object (all files)
  bool failed;
  JournalWriter* writer;
  // log_profile=persistent: the quiet profile's filter (log_profile.h) in
  // front of the writer. Worker side only; the writer's own backend has none.
  mx5::runtime::PersistentLog* filter;
  explicit Journal(const char *directory = ROOT)
      : root(directory), f(0), written(0), total_bytes(0), failed(false), writer(0), filter(0) {}
  ~Journal() {
    if (writer) { bool ignored; stop_journal_writer(writer,false,&ignored); writer=0; }
    if (f)
      fclose(f);
  }
  void fail() {
    failed = true;
    journal_ok.store(0, std::memory_order_release);
    disable_mutation();
  }
  // Ring sizes: 512 KiB of diagnostic rows (about 16 s of the full profile's
  // peak rate; holds a whole 400 KiB persistent RAW window flush) and 256 KiB
  // that only evidence rows may use (a persistent RAW
  // window holds up to about 60 POSITION rows of 1.5 KiB while BETA is live).
  // bulk_bytes (persistent profile, 2026-10-08): the paced RAW window rows,
  // not timed by the journal lag; 0 queues them as diagnostic rows.
  // false: no writer (allocation/thread failure); rows stay synchronous.
  bool start_writer(size_t diagnostic_bytes=524288,size_t evidence_bytes=262144,size_t bulk_bytes=0) {
    if (writer || f || failed) return false;
    writer=start_journal_writer(root,diagnostic_bytes,evidence_bytes,bulk_bytes);
    return writer!=0;
  }
  // Worker side: adopt a failure the writer thread reported.
  void poll() {
    if (writer && !failed && !journal_writer_ok(writer)) fail();
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
    if (filter) { sync_filter(); filter->row(s, clock_ns(0), emit_to, this); return; }
    emit(s);
  }
  // A formatted POSITION/SEND observation row (the profile needs its values).
  // While the lag guard holds journal_current at 0 no POSITION can select a
  // replacement (provenance() refuses), so a FIX-class POSITION row is
  // queued as a diagnostic row: a long storage stall then drops old FIX rows
  // instead of filling the evidence ring and failing the journal. LOST and
  // NO_FIX rows stay evidence. Restored when the guard recovers (2026-10-07).
  void observation_line(const char *s, const A::Observation& o) {
    if (failed)
      return;
    if (filter) { sync_filter(); filter->observation(s, o, clock_ns(0), emit_to, this); return; }
    emit(s, o.kind == A::Observation::POSITION && o.position_class == A::POSITION_FIX &&
                !journal_current.load(std::memory_order_acquire) ?
            mx5::runtime::PersistentLog::ROW_RAW : mx5::runtime::PersistentLog::ROW_KEEP);
  }
  // Every accepted motion event (digest statistics of the quiet profile).
  void note_motion(const N::RawEvent& e) { if (filter) filter->motion(e); }
  void tick(uint64_t now) { if (filter && !failed) { sync_filter(); filter->tick(now, emit_to, this); } }
  // Every worker turn: the quiet profile's paced RAW window drain. It
  // waits while rows that must be durable promptly are still queued.
  void pump(uint64_t now) {
    if (filter && !failed && filter->draining()) { sync_filter(); filter->pump(now, emit_to, this, bulk_ready, this); }
  }
  static bool bulk_ready(void* journal) {
    Journal& j = *static_cast<Journal*>(journal);
    return !j.writer || journal_writer_bulk_ready(j.writer);
  }
  void sync_filter() { filter->set_journal_current(journal_current.load(std::memory_order_acquire) != 0); }
  static void emit_to(void* journal, const char* s, unsigned row_class) {
    static_cast<Journal*>(journal)->emit(s, row_class);
  }
  // A row that is written (profile decisions already made). row_class:
  // PersistentLog::ROW_KEEP (by kind), ROW_RAW (RAW context, diagnostic) or
  // ROW_BULK (paced RAW window, not timed by the journal lag).
  void emit(const char *s, unsigned row_class = mx5::runtime::PersistentLog::ROW_KEEP) {
    if (failed)
      return;
    if (writer) {
      poll();
      if (failed) return;
      const size_t n = strlen(s);
      typedef mx5::runtime::JournalRing R;
      const R::Class c = row_class == mx5::runtime::PersistentLog::ROW_BULK ? R::BULK :
          row_class == mx5::runtime::PersistentLog::ROW_KEEP && mx5::runtime::journal_evidence_row(s, n) ?
          R::EVIDENCE : R::DIAGNOSTIC;
      // An evidence row that cannot be queued is a journal failure: fail
      // closed (mutation disabled) instead of dropping it.
      if (!journal_writer_push(writer, s, n, c)) fail();
      return;
    }
    write_line(s);
  }
  void write_line(const char *s) {
    size_t n = strlen(s);
    if (n + 1 > config.max_log_bytes) {
      fail();
      return;
    }
    const mx5::runtime::StorageSpace space=mx5::runtime::storage_space(root,n+1);
    if(space.reason) {
      mx5::runtime::record_storage_stop(root,"trace",space);
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
    total_bytes += n + 1;
  }
  // Periodic (1 s) flush. Asynchronous with a writer: it only requests one.
  void flush() {
    if (writer) { journal_writer_request_flush(writer); poll(); return; }
    if (f && fflush(f))
      fail();
  }
  // Every row queued so far has reached the kernel (fflush), or false. Used
  // where the next step requires earlier rows to be durable-in-order (the
  // boot row before BETA is armed). A timeout is not a failure by itself.
  bool flush_wait(uint64_t timeout_ns = 10000000000ULL) {
    if (writer) {
      const bool ok = journal_writer_flush_wait(writer, timeout_ns);
      poll();
      return ok && !failed;
    }
    flush();
    return !failed;
  }
  // Capture stop: every queued row written, flushed, fsynced and the file
  // closed. With a writer this stops and joins the writer thread.
  bool close_durable() {
    if (writer) {
      bool ok=false;
      stop_journal_writer(writer, true, &ok);
      writer = 0;
      if (!ok) { fail(); return false; }
      return !failed;
    }
    flush();
    if (failed || !f || fsync(fileno(f))) { fail(); return false; }
    const bool close_failed = fclose(f) != 0; f = 0;
    if (close_failed) { fail(); return false; }
    return true;
  }
};

// The writer thread's state. Allocated once per worker (off the hot path);
// the ring storage is preallocated with it. Shared fields are atomics; the
// ring has its own short lock. OEM threads never touch any of it.
struct JournalWriter {
  const char* root;
  unsigned char* storage;      // diagnostic ring | evidence ring | bulk ring | row buffer
  mx5::runtime::JournalRing ring;
  pthread_t thread;
  std::atomic<unsigned> ok, stopping, durable, closed_ok;
  std::atomic<uint64_t> flush_target, flushed, written;
  // Start of the writer's fflush (0: not flushing). The rows themselves
  // (queued, popped-and-held, written-not-flushed) are timed by the ring
  // under its mutex; together they give the journal lag.
  std::atomic<uint64_t> busy_since;
  std::atomic<uint64_t> inject_stall_ns;   // tests only: one stall before the next row
  std::atomic<uint64_t> inject_wait_gap_ns; // tests only: one stall between the flush check and wait()
  std::atomic<uint64_t> inject_flush_stall_ns; // tests only: one slow fflush
  std::atomic<uint64_t> inject_row_delay_ns;   // tests only: a slow writer (sleep before every row)
  // Age-triggered fflush bound (JOURNAL_FLUSH_MAX_AGE_NS; 0: only on a
  // worker request, the earlier behaviour; tests and measurements only).
  std::atomic<uint64_t> flush_max_age_ns;
  std::atomic<uint64_t> flush_count;         // fflush calls by the writer (measurement)
  std::atomic<uint64_t> loops;             // tests only: writer loop iterations
  char* row_buffer;                         // writer thread only
  JournalWriter(const char* r,unsigned char* buffer,size_t diagnostic,size_t evidence,size_t bulk)
      : root(r),storage(buffer),ring(buffer,diagnostic,buffer+diagnostic,evidence,
                                     bulk?buffer+diagnostic+evidence:0,bulk),thread(),
        ok(1),stopping(0),durable(0),closed_ok(0),flush_target(0),flushed(0),written(0),
        busy_since(0),inject_stall_ns(0),inject_wait_gap_ns(0),inject_flush_stall_ns(0),inject_row_delay_ns(0),
        flush_max_age_ns(JOURNAL_FLUSH_MAX_AGE_NS),flush_count(0),loops(0),row_buffer(0) {}
};
const size_t JOURNAL_ROW_BUFFER=mx5::runtime::JournalRing::MAX_ROW+1;
// dropped_total: rows lost up to and including this gap (the writer's own
// count of sequence gaps), not the ring's later total at write time.
void journal_dropped(Journal& file,uint64_t first,uint64_t next,uint64_t dropped_total) {
  char line[400];
  const int n=snprintf(line,sizeof line,
      "{\"kind\":\"journal_dropped\",\"schema\":1,\"mono_ns\":%llu,\"class\":\"diagnostic\","
      "\"rows\":%llu,\"first_seq\":%llu,\"last_seq\":%llu,\"dropped_total\":%llu,"
      "\"reason\":\"writer_backlog\"}",
      (unsigned long long)clock_ns(0),(unsigned long long)(next-first),(unsigned long long)first,
      (unsigned long long)(next-1),(unsigned long long)dropped_total);
  if(n>0 && size_t(n)<sizeof line)file.line(line);else file.fail();
}
// Writer's wait predicate, evaluated under the ring mutex: a stop or a flush
// it can complete now. Both are stored before the requester's notify(), so
// a request made after the loop's own check is not slept through.
bool journal_writer_pending(void* argument) {
  JournalWriter& w=*static_cast<JournalWriter*>(argument);
  if(w.stopping.load(std::memory_order_acquire))return true;
  const uint64_t target=w.flush_target.load(std::memory_order_acquire);
  return target>w.flushed.load(std::memory_order_acquire) && w.written.load(std::memory_order_acquire)>=target;
}
void* journal_writer_main(void* argument) {
  JournalWriter& w=*static_cast<JournalWriter*>(argument);
  // The stdio stream belongs to this thread only. No numeric row is
  // formatted here except the counter row; keep the C locale anyway.
  locale_t numeric=newlocale(LC_NUMERIC_MASK,"C",(locale_t)0);
  if(numeric)uselocale(numeric);
  char* buffer=w.row_buffer;
  {
    Journal file(w.root);
    uint64_t expected=0,dropped_seen=0;
    for(;;) {
      // Read stop BEFORE draining: every row pushed before stop() is written.
      const bool stop=w.stopping.load(std::memory_order_acquire)!=0;
#ifdef MX5DR_JOURNAL_TEST_HOOKS
      w.loops.fetch_add(1,std::memory_order_relaxed);
#endif
      unsigned batch=0;size_t n;uint64_t seq,push_ns;bool evidence;
      while(batch<64 && w.ring.pop(buffer,JOURNAL_ROW_BUFFER,&n,&seq,&evidence,&push_ns)) {
        ++batch;
        // The popped row is already timed by the ring (held), from inside
        // pop()'s critical section: no window in which the lag reads 0.
#ifdef MX5DR_JOURNAL_TEST_HOOKS
        const uint64_t stall=w.inject_stall_ns.exchange(0,std::memory_order_acq_rel);
        if(stall) { struct timespec t={time_t(stall/1000000000ULL),long(stall%1000000000ULL)};nanosleep(&t,0); }
        const uint64_t delay=w.inject_row_delay_ns.load(std::memory_order_acquire);
        if(delay) { struct timespec t={time_t(delay/1000000000ULL),long(delay%1000000000ULL)};nanosleep(&t,0); }
#endif
        if(seq!=expected) {
          dropped_seen+=seq-expected;
          if(!file.failed)journal_dropped(file,expected,seq,dropped_seen);
        }
        expected=seq+1;
        if(!file.failed)file.line(buffer);
        w.written.store(expected,std::memory_order_release);
        // Handed to stdio, not yet fflush'ed: still counted in the lag
        // (stdio may also have written it earlier; the lag then over-, never
        // under-states).
        w.ring.row_written();
        // End the batch when a prompt row in the stdio buffer is due for
        // its age flush (2026-10-08): with a slow writer 64 rows can take
        // longer than the whole lag limit (QEMU: about 22 ms per row).
        const uint64_t age_limit=w.flush_max_age_ns.load(std::memory_order_acquire);
        if(age_limit) {
          const uint64_t oldest=w.ring.stats().unflushed_push_ns;
          const uint64_t at=oldest?clock_ns(0):0;
          if(oldest && at>oldest && at-oldest>=age_limit)break;
        }
      }
      if(file.failed)w.ok.store(0,std::memory_order_release);
      const uint64_t target=w.flush_target.load(std::memory_order_acquire);
      // Age-triggered flush: the oldest row in the stdio buffer reached the
      // bound. Otherwise the time until it does bounds the wait below.
      const uint64_t max_age=w.flush_max_age_ns.load(std::memory_order_acquire);
      const uint64_t unflushed=max_age?w.ring.stats().unflushed_push_ns:0;
      const uint64_t now=unflushed?clock_ns(0):0;
      const uint64_t age=unflushed && now>unflushed?now-unflushed:0;
      const bool aged=unflushed && age>=max_age;
      if((target>w.flushed.load(std::memory_order_acquire) && expected>=target) || aged) {
        w.busy_since.store(clock_ns(0),std::memory_order_release);
#ifdef MX5DR_JOURNAL_TEST_HOOKS
        const uint64_t slow=w.inject_flush_stall_ns.exchange(0,std::memory_order_acq_rel);
        if(slow) { struct timespec t={time_t(slow/1000000000ULL),long(slow%1000000000ULL)};nanosleep(&t,0); }
#endif
        w.flush_count.fetch_add(1,std::memory_order_relaxed);
        file.flush();
        if(!file.failed)w.ring.flushed();
        w.busy_since.store(0,std::memory_order_release);
        if(file.failed)w.ok.store(0,std::memory_order_release);
        w.flushed.store(expected,std::memory_order_release);
      }
      if(stop && !batch) {
        const mx5::runtime::JournalRing::Stats stats=w.ring.stats();
        if(expected<stats.next_seq) {
          dropped_seen+=stats.next_seq-expected;
          if(!file.failed)journal_dropped(file,expected,stats.next_seq,dropped_seen);
        }
        file.flush();
        bool ok=!file.failed;
        if(w.durable.load(std::memory_order_acquire)) {
          if(!ok || !file.f || fsync(fileno(file.f)))ok=false;
          if(file.f) { if(fclose(file.f))ok=false;file.f=0; }
        }
        if(!ok)w.ok.store(0,std::memory_order_release);
        w.closed_ok.store(ok?1:0,std::memory_order_release);
        break;
      }
      // Sleep until a push, a flush/stop request, the age-triggered flush is
      // due, or at most 1 s (no polling).
      if(!batch) {
        uint64_t timeout=1000000000ULL;
        if(unflushed && !aged) { timeout=max_age-age;if(timeout<1000000ULL)timeout=1000000ULL; }
#ifdef MX5DR_JOURNAL_TEST_HOOKS
        const uint64_t gap=w.inject_wait_gap_ns.exchange(0,std::memory_order_acq_rel);
        if(gap) { struct timespec t={time_t(gap/1000000000ULL),long(gap%1000000000ULL)};nanosleep(&t,0); }
#endif
        w.ring.wait(timeout,journal_writer_pending,&w);
      }
    }
  }
  uselocale(LC_GLOBAL_LOCALE);
  if(numeric)freelocale(numeric);
  return 0;
}
JournalWriter* start_journal_writer(const char* root,size_t diagnostic_bytes,size_t evidence_bytes,
                                    size_t bulk_bytes) {
  const size_t rings=diagnostic_bytes+evidence_bytes+bulk_bytes;
  unsigned char* buffer=new(std::nothrow) unsigned char[rings+JOURNAL_ROW_BUFFER];
  if(!buffer)return 0;
  JournalWriter* w=new(std::nothrow) JournalWriter(root,buffer,diagnostic_bytes,evidence_bytes,bulk_bytes);
  if(!w) { delete[] buffer;return 0; }
  w->row_buffer=reinterpret_cast<char*>(buffer+rings);
  // Joinable, explicit stack (the stock 128 KiB default is not relied on).
  if(!mx5::runtime::create_thread(&w->thread,journal_writer_main,w,256u<<10,false)) {
    delete w;delete[] buffer;return 0;
  }
  return w;
}
void stop_journal_writer(JournalWriter* w,bool durable,bool* ok) {
  w->durable.store(durable?1:0,std::memory_order_release);
  w->stopping.store(1,std::memory_order_release);
  w->ring.notify();
  pthread_join(w->thread,0);
  *ok=w->closed_ok.load(std::memory_order_acquire)!=0;
  unsigned char* buffer=w->storage;
  delete w;delete[] buffer;
}
bool journal_writer_push(JournalWriter* w,const char* s,size_t n,mx5::runtime::JournalRing::Class c) {
  const mx5::runtime::JournalRing::Result r=w->ring.push_class(s,n,c,clock_ns(0));
  return r==mx5::runtime::JournalRing::PUSHED || r==mx5::runtime::JournalRing::PUSHED_AFTER_DROP;
}
bool journal_writer_ok(JournalWriter* w) { return w->ok.load(std::memory_order_acquire)!=0; }
// The paced RAW window drain may add its next rows: no prompt (evidence or
// diagnostic) row waits and the previous bulk rows were all taken by the
// writer. The drain then follows the writer's own pace (at most the
// PersistentLog rate), so a later prompt row waits behind at most one
// pump's rows (16), even on a writer slower than that rate (2026-10-08:
// under QEMU about 90 rows/s).
bool journal_writer_bulk_ready(JournalWriter* w) {
  const mx5::runtime::JournalRing::Stats s=w->ring.stats();
  return !s.oldest_push_ns && !s.bulk_used;
}
void journal_writer_request_flush(JournalWriter* w) {
  const uint64_t next=w->ring.stats().next_seq;
  if(next>w->flush_target.load(std::memory_order_acquire)) {
    w->flush_target.store(next,std::memory_order_release);
    w->ring.notify();
  }
}
// Worker side, lock-free except the ring's short lock: how far the writer
// is behind. oldest_ns: age of the oldest row that has not reached the
// kernel: queued, popped and being written, or handed to stdio but not yet
// fflush'ed (2026-10-07; before, rows in the stdio buffer were not counted
// and the lag read up to about 1 s young), or the start of a running fflush.
// Only rows that must be durable promptly are timed (2026-10-08): BULK rows
// (the paced persistent RAW window) are not; a prompt row queued behind
// them is timed from its own push, so a writer stall still shows.
JournalLag journal_writer_lag(JournalWriter* w,uint64_t now) {
  JournalLag lag=JournalLag();
  if(!w)return lag;
  lag.thread=true;
  const mx5::runtime::JournalRing::Stats stats=w->ring.stats();
  const uint64_t written=w->written.load(std::memory_order_acquire);
  lag.unwritten_rows=stats.next_seq>written?stats.next_seq-written:0;
  lag.dropped_rows=stats.dropped_rows;lag.high_water=stats.high_water;
  uint64_t oldest=0;
  const uint64_t times[4]={stats.oldest_push_ns,stats.held_push_ns,stats.unflushed_push_ns,
                           w->busy_since.load(std::memory_order_acquire)};
  for(unsigned i=0;i<4;++i)if(times[i] && (!oldest || times[i]<oldest))oldest=times[i];
  lag.oldest_ns=oldest && now>oldest?now-oldest:0;
  return lag;
}
bool journal_writer_flush_wait(JournalWriter* w,uint64_t timeout_ns) {
  const uint64_t next=w->ring.stats().next_seq;
  journal_writer_request_flush(w);
  const uint64_t begin=clock_ns(0);
  while(w->flushed.load(std::memory_order_acquire)<next) {
    if(!journal_writer_ok(w))return false;
    if(clock_ns(0)-begin>timeout_ns)return false;
    const struct timespec pause={0,1000000};nanosleep(&pause,0);
  }
  return journal_writer_ok(w);
}

// Worker, every turn (BETA only): the journal lag guard described at
// journal_current. Lowering also revokes the adapter generation so a stored
// candidate cannot be selected by a POSITION that read provenance earlier.
// The row is BETA evidence (never dropped by the ring); while the writer is
// stalled it waits in RAM like every other row.
void journal_lag_guard(Journal& j,uint64_t now) {
  if(!j.writer || j.failed)return;
  const JournalLag lag=journal_writer_lag(j.writer,now);
  const bool current=journal_current.load(std::memory_order_acquire)!=0;
  const char* event=0;
  if(current && lag.oldest_ns>JOURNAL_LAG_LIMIT_NS) {
    journal_current.store(0,std::memory_order_release);
    A::invalidate();
    event="lagging";
  } else if(!current && lag.oldest_ns<JOURNAL_LAG_CLEAR_NS) {
    journal_current.store(1,std::memory_order_release);
    event="current";
  }
  if(!event)return;
  char line[400];
  const int n=snprintf(line,sizeof line,
      "{\"kind\":\"beta_journal_lag\",\"mono_ns\":%llu,\"domain\":\"beta\",\"assist_ready\":false,"
      "\"event\":\"%s\",\"lag_ms\":%llu,\"unwritten_rows\":%llu,\"limit_ms\":%llu,\"clear_ms\":%llu}",
      (unsigned long long)now,event,(unsigned long long)(lag.oldest_ns/1000000ULL),
      (unsigned long long)lag.unwritten_rows,(unsigned long long)(JOURNAL_LAG_LIMIT_NS/1000000ULL),
      (unsigned long long)(JOURNAL_LAG_CLEAR_NS/1000000ULL));
  if(n>0 && size_t(n)<sizeof line)j.line(line);else j.fail();
}
// Why BETA stays disabled at startup (0: it may arm). A boot row that did
// not reach the file in time is "journal_not_durable", distinct from a
// failed journal.
const char* beta_block_reason(bool hook,bool failed,bool durable,bool audit,bool capture,
                              bool shadow,bool core) {
  return !hook?"hook_not_installed":failed?"journal_failed":!durable?"journal_not_durable":
      audit?"audit_fault":!capture?"motion_capture_unavailable":!shadow?"model_unavailable":
      !core?"beta_core_unavailable":0;
}
void flush_motion(Journal &j, mx5::runtime::MotionBatch &batch) {
  if (!batch.empty()) {
    j.line(batch.line());
    batch.clear();
  }
}
void journal_motion(Journal &j, mx5::runtime::MotionBatch &batch,
                    const N::RawEvent &raw) {
  j.note_motion(raw);
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

void journal_model_motion_excluded(Journal& j,const N::RawEvent& raw,
                                  uint64_t since,const char* reason) {
  char line[600];
  const int n=snprintf(line,sizeof line,
      "{\"kind\":\"shadow_motion_excluded\",\"mono_ns\":%llu,\"domain\":\"model\","
      "\"assist_ready\":false,\"reason\":\"%s\",\"raw_since_ns\":%llu,"
      "\"sensor\":%u,\"epoch\":%llu,\"receive_seq\":%llu,"
      "\"received_ns\":%llu,\"source_mono_ms\":%lld}",
      (unsigned long long)clock_ns(0),reason,(unsigned long long)since,unsigned(raw.kind),
      (unsigned long long)raw.epoch,(unsigned long long)raw.receive_seq,
      (unsigned long long)raw.received_ns,(long long)raw.source_mono_ms);
  if(n>0 && size_t(n)<sizeof line)j.line(line);else j.fail();
}

void journal_pipeline_reset(Journal& j,const N::Pipeline& navigation,uint64_t before,
                            const char* operation,uint64_t input_ns,
                            uint64_t receive_seq=0,unsigned sensor=0,unsigned call=0) {
  const N::Status& status=navigation.status();
  if(status.resets==before)return;
  // The next accepted input can overwrite status.result in the same receive
  // turn. Record the primary MODEL fault while its cause is still available.
  char line[600];
  const int n=snprintf(line,sizeof line,
      "{\"kind\":\"shadow_pipeline_reset\",\"mono_ns\":%llu,\"domain\":\"model\","
      "\"assist_ready\":false,\"reason\":\"%s\",\"operation\":\"%s\","
      "\"input_ns\":%llu,\"receive_seq\":%llu,\"sensor\":%u,\"call\":%u,\"resets\":%llu}",
      (unsigned long long)clock_ns(0),N::pipeline_result_name(status.result),operation,
      (unsigned long long)input_ns,(unsigned long long)receive_seq,sensor,call,
      (unsigned long long)status.resets);
  if(n>0 && size_t(n)<sizeof line)j.line(line);else j.fail();
}

// Task E (2026-10-05): see runtime/motion_gap.h. Worker thread only.
mx5::runtime::MotionGapTracker motion_gap;
void journal_latch_gap(Journal& j,const char* event,N::ReceiveFault reason,uint64_t missing,
                       uint64_t span_ns,const N::Pipeline& navigation) {
  char line[400];
  const int n=snprintf(line,sizeof line,
      "{\"kind\":\"beta_reverse_latch\",\"mono_ns\":%llu,\"domain\":\"model\",\"event\":\"%s\","
      "\"reason\":\"%s\",\"missing_events\":%llu,\"gap_ms\":%llu,\"latched\":%s,\"value\":%d,"
      "\"keep_limit_events\":%llu,\"keep_limit_ms\":%llu}",
      (unsigned long long)clock_ns(0),event,N::receive_fault_name(reason),(unsigned long long)missing,
      (unsigned long long)(span_ns/1000000ULL),navigation.reverse_latched()?"true":"false",
      navigation.latch_value(),(unsigned long long)mx5::runtime::MotionGapTracker::KEEP_EVENTS,
      (unsigned long long)(mx5::runtime::MotionGapTracker::KEEP_NS/1000000ULL));
  if(n>0 && size_t(n)<sizeof line)j.line(line);else j.fail();
}
// An accepted motion event closes an open gap (clearing the latch if it grew
// beyond the keep limit after all) and advances the high-water mark.
void motion_gap_accept(Journal& j,const N::RawEvent& raw,N::Pipeline& navigation,bool enabled) {
  const N::ReceiveFault reason=motion_gap.reason;
  uint64_t missing=0,span=0;
  const mx5::runtime::MotionGapTracker::Close closed=motion_gap.accept(raw,&missing,&span);
  if(!enabled || closed==mx5::runtime::MotionGapTracker::NO_GAP)return;
  if(closed==mx5::runtime::MotionGapTracker::TOO_LARGE) {
    if(navigation.reverse_latched())journal_latch_gap(j,"cleared_after_gap",reason,missing,span,navigation);
    navigation.exclude_reverse(N::LATCH_CLEAR_INPUT_GAP);
  } else if(navigation.reverse_latched())
    journal_latch_gap(j,"kept_across_gap",reason,missing,span,navigation);
}
// Late arrivals (channel.cpp inspect_motion_datagram): accepted records that
// waited in the socket queue for more than 250 ms while this worker was not
// receiving. One motion_late_accepted row per contiguous burst plus totals
// in health. Diagnostics only; received_ns is never rewritten. Worker only.
struct MotionLate {
  uint64_t accepted,bursts,max_ns;            // totals since start
  uint64_t burst_events,burst_max_ns,burst_first_seq,burst_last_seq,burst_epoch,burst_checked_ns;
};
MotionLate motion_late={0,0,0,0,0,0,0,0,0};
void close_late_burst(Journal& j) {
  MotionLate& m=motion_late;
  if(!m.burst_events)return;
  char line[500];
  const int n=snprintf(line,sizeof line,
      "{\"kind\":\"motion_late_accepted\",\"schema\":1,\"mono_ns\":%llu,\"domain\":\"model\","
      "\"assist_ready\":false,\"epoch\":%llu,\"first_seq\":%llu,\"last_seq\":%llu,"
      "\"events\":%llu,\"max_late_ms\":%llu,\"max_late_ns\":%llu,\"fresh_limit_ms\":%llu,"
      "\"late_limit_ms\":%llu,\"late_accepted_total\":%llu}",
      (unsigned long long)m.burst_checked_ns,(unsigned long long)m.burst_epoch,
      (unsigned long long)m.burst_first_seq,(unsigned long long)m.burst_last_seq,
      (unsigned long long)m.burst_events,(unsigned long long)(m.burst_max_ns/1000000ULL),
      (unsigned long long)m.burst_max_ns,
      (unsigned long long)(N::MOTION_FRESH_NS/1000000ULL),
      (unsigned long long)(mx5::runtime::MotionGapTracker::KEEP_NS/1000000ULL),
      (unsigned long long)m.accepted);
  m.burst_events=m.burst_max_ns=m.burst_first_seq=m.burst_last_seq=m.burst_epoch=m.burst_checked_ns=0;
  if(n>0 && size_t(n)<sizeof line)j.line(line);else j.fail();
}
void note_late(const N::RawEvent& raw,const N::ReceiveDiagnostic& d) {
  MotionLate& m=motion_late;
  if(m.accepted!=UINT64_MAX)++m.accepted;
  if(d.age_ns>m.max_ns)m.max_ns=d.age_ns;
  if(!m.burst_events) {
    if(m.bursts!=UINT64_MAX)++m.bursts;
    m.burst_first_seq=raw.receive_seq;m.burst_epoch=raw.epoch;
  }
  ++m.burst_events;m.burst_last_seq=raw.receive_seq;m.burst_checked_ns=d.checked_ns;
  if(d.age_ns>m.burst_max_ns)m.burst_max_ns=d.age_ns;
}
// One bounded worker receive turn. Capture survives model/AA audit failure;
// rejected input is separate evidence and can never enter either estimator.
//
// A late-accepted record (d.late) enters the estimators exactly like an
// on-time one, at its PRODUCER receipt time: Pipeline orders and integrates
// events by that time, never by this worker's receive time. The late arrival
// therefore adds no extra age to the position error budget: the estimate at
// the event's time equals the one had it arrived on time (tests/navigation/
// test_beta.cpp late_arrival_output_stays_bounded). What the delay can
// affect is how old the newest estimate is when it is published; that stays
// bounded by the pipeline's sample-age/sensor-timeout faults, the BETA 300 ms
// receipt-silence check and the publication lease, all measured against now.
template<class Receiver>
void drain_motion(Journal& j,mx5::runtime::MotionBatch& batch,Receiver& motion,
                  N::Pipeline& navigation,N::GpsHoldout& holdout,bool compute,
                  uint64_t model_since_ns=0,bool bus_boundary=false) {
  for(unsigned i=0;i<256 && !j.failed;++i) {
    N::RawEvent raw=N::RawEvent();
    N::ReceiveDiagnostic d=N::ReceiveDiagnostic();
    const N::ReceiveResult received=motion.receive(&raw,&d);
    if(received==N::CHANNEL_EMPTY)break;
    const bool enabled=compute && !__sync_fetch_and_add(&audit_fault,0);
    if(received==N::CHANNEL_EVENT && d.late)note_late(raw,d);
    else if(motion_late.burst_events) {
      // The burst row follows the batch that holds its events.
      flush_motion(j,batch);close_late_burst(j);
    }
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
        ++c.source_epoch;++c.generation;
        uint64_t missing=0,span=0;
        if(motion_gap.reject(d.authenticated_decoded,d.reason,d.rejected,&missing,&span))
          navigation.reset_keep_reverse(c);
        else {
          if(navigation.reverse_latched())
            journal_latch_gap(j,"cleared_by_rejection",d.reason,missing,span,navigation);
          navigation.reset(c);
        }
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
      // The existing MODEL pipeline uses a positive transport timestamp as
      // event time. A later receipt cannot make a pre-boundary event new.
      // Leave negative/overflow/future timestamps on the Pipeline fault path.
      const bool old_transport=raw.source_mono_ms>0 &&
          uint64_t(raw.source_mono_ms)<=UINT64_MAX/1000000ULL &&
          uint64_t(raw.source_mono_ms)*1000000ULL<model_since_ns;
      if(enabled && (raw.received_ns<model_since_ns || old_transport)) {
        // BETA_DECISIONS 3.4: an excluded REVERSE change ends the latch.
        if(raw.kind==N::REVERSE)navigation.exclude_reverse();
        motion_gap_accept(j,raw,navigation,enabled);
        journal_motion(j,batch,raw);flush_motion(j,batch);
        journal_model_motion_excluded(j,raw,model_since_ns,
            raw.received_ns<model_since_ns?
                (bus_boundary?"receipt_before_bus":"receipt_before_session"):
                (bus_boundary?"transport_before_bus":"transport_before_session"));
        continue;
      }
      const uint64_t resets=navigation.status().resets;
      motion_gap_accept(j,raw,navigation,enabled);
      if(enabled) {navigation.enqueue_raw(raw);holdout.enqueue_raw(raw);}
      else if(raw.kind==N::REVERSE)navigation.exclude_reverse(); // change not computed
      journal_motion(j,batch,raw);
      if(enabled && navigation.status().resets!=resets) {
        // Write the offending raw input before its diagnostic. These are
        // separate buffered records, not an atomic durable pair.
        flush_motion(j,batch);
        journal_pipeline_reset(j,navigation,resets,"raw",raw.received_ns,
                               raw.receive_seq,unsigned(raw.kind));
      }
    }
  }
  // No batch crosses the worker sleep, including a capped or failed turn.
  flush_motion(j,batch);
  close_late_burst(j);
}

void journal_health(Journal& j,uint64_t now,bool capture,bool computation,
                    const mx5::runtime::LdsRequestSource* source=0) {
  const A::RequestHookHealth h=A::request_hook_health();
  const A::SessionHealth s=A::session_hook_health();
  const A::BusHealth b=A::bus_hook_health();
  const JournalLag lag=journal_writer_lag(j.writer,now);
  char line[1800],source_status[420]="";
  if(source) {
    const mx5::runtime::LdsRequestSource::Status& status=source->status();
    const int n=snprintf(source_status,sizeof source_status,
        ",\"lds_request_source\":{\"association_only\":true,\"entries\":%u,"
        "\"positions_total\":%llu,\"records_total\":%llu,\"matches_total\":%llu,"
        "\"conflicts_total\":%llu,\"retirements_total\":%llu,\"rejected_total\":%llu}",
        status.entries,(unsigned long long)status.positions,(unsigned long long)status.records,
        (unsigned long long)status.matches,(unsigned long long)status.conflicts,
        (unsigned long long)status.retirements,(unsigned long long)status.rejected);
    if(n<=0 || size_t(n)>=sizeof source_status) { j.fail();return; }
  }
  const int n=snprintf(line,sizeof line,
      "{\"kind\":\"health\",\"mono_ns\":%llu,\"dropped\":%llu,\"hook_installed\":%s,"
      "\"runtime_mode\":%u,\"audit_fault\":%u,\"capture_active\":%s,"
      "\"computation_active\":%s,\"assist_ready\":false,\"request_observer\":{"
      "\"prepared\":%s,\"abi_fault\":%s,\"result\":\"%s\",\"loss_epoch\":%llu,"
      "\"requests\":%u,\"workers\":%u,\"loss_reasons\":%u,\"exhausted\":%s},"
      "\"session_observer\":{\"prepared\":%s,\"contexts\":%u,\"capacity\":%u,\"faults\":%u},"
      "\"bus_observer\":{\"prepared\":%s,\"contexts\":%u,\"capacity\":%u,\"faults\":%u},"
      "\"motion_late\":{\"accepted\":%llu,\"bursts\":%llu,\"max_late_ms\":%llu},"
      "\"journal\":{\"writer\":\"%s\",\"unwritten_rows\":%llu,\"oldest_unwritten_ms\":%llu,"
      "\"high_water_bytes\":%llu,\"dropped_rows\":%llu}%s}",
      (unsigned long long)now,(unsigned long long)queue.dropped(),hook_installed?"true":"false",
      unsigned(A::mode()),__sync_fetch_and_add(&audit_fault,0),capture?"true":"false",
      computation?"true":"false",h.prepared?"true":"false",h.abi_fault?"true":"false",
      A::R::result_name(h.result),(unsigned long long)h.ledger.loss_epoch,h.ledger.requests,
      h.ledger.workers,h.ledger.loss_reasons,h.ledger.exhausted?"true":"false",
      s.prepared?"true":"false",s.contexts,unsigned(A::SESSION_CONTEXT_CAPACITY),s.faults,
      b.prepared?"true":"false",b.contexts,unsigned(A::BUS_CONTEXT_CAPACITY),b.faults,
      (unsigned long long)motion_late.accepted,(unsigned long long)motion_late.bursts,
      (unsigned long long)(motion_late.max_ns/1000000ULL),
      lag.thread?"thread":"inline",(unsigned long long)lag.unwritten_rows,
      (unsigned long long)(lag.oldest_ns/1000000ULL),(unsigned long long)lag.high_water,
      (unsigned long long)lag.dropped_rows,source_status);
  if(n<=0 || size_t(n)>=sizeof line)j.fail();else j.line(line);
}
// Fail closed: anything except a definitely absent marker counts as present.
bool disable_marker_present(const char* root) {
  char path[256];snprintf(path,sizeof path,"%s/logs/disable-next-start",root);
  struct stat marker;
  if(lstat(path,&marker)==0)return true;
  return errno!=ENOENT;
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
      char line[mx5::runtime::OBSERVATION_JSON_CAPACITY];
      if(format_observation(line,sizeof line,o))j.observation_line(line,o);else j.fail();
      if(o.kind==A::Observation::POSITION && o.reason==A::CONTEXT_UNAVAILABLE) {
        j.line("{\"kind\":\"capture_incomplete\",\"reason\":\"adapter_context_unavailable\",\"assist_ready\":false}");
        disable_mutation();
      }
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
bool finish_capture(Journal& j,const char* boot_id,uint64_t cutoff,uint64_t now,
                    const mx5::runtime::LdsRequestSource* source=0) {
  if(!cutoff || now<cutoff || !queue.drained()) { j.fail();return false; }
  // A producer may have returned its failed reservation before the sink's
  // disable_mutation call. The closed+drained acquire covers its sticky loss.
  if(queue.lost())disable_mutation();
  // A callback can fault after queue.close, when its sink can no longer record
  // a row. Preserve this final observed failure before the durable stop ack.
  // This snapshot cannot certify that no OEM callback will run after the ack.
  if(A::faulted()) {
    j.line("{\"kind\":\"capture_incomplete\",\"reason\":\"adapter_fault\",\"assist_ready\":false}");
    disable_mutation();
  }
  char line[400];
  snprintf(line,sizeof line,
      "{\"kind\":\"capture_end\",\"schema\":1,\"mono_ns\":%llu,\"boot_id\":\"%s\","
      "\"domain\":\"model\",\"assist_ready\":false,\"reason\":\"requested\","
      "\"cutoff_ns\":%llu,\"bounded_final_drain\":true}",
      (unsigned long long)now,boot_id,(unsigned long long)cutoff);
  j.line(line);
  journal_health(j,now,false,false,source);
  if(!j.close_durable())return false;
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

void sync_model_session(Journal& j,mx5::runtime::ModelSession& session,
                       N::Pipeline& navigation,N::GpsHoldout& holdout) {
  const A::S::Snapshot current=A::read_issue_session();
  const uint64_t now=clock_ns(0);
  const mx5::runtime::ModelSession::Update update=session.update(current,now);
  if(update==mx5::runtime::ModelSession::SAME)return;
  const bool reset=update==mx5::runtime::ModelSession::CHANGED;
  if(reset) {
    mx5_dr_context c=navigation.context();
    if(c.session_epoch==UINT64_MAX || c.generation==UINT64_MAX) {
      disable_mutation();return;
    }
    ++c.session_epoch;++c.generation;
    navigation.reset(c);holdout.reset(c,N::HOLDOUT_SESSION_RESET);
    journal_holdout(j,holdout,now);
  }
  char observed[200],line[700];
  if(!mx5::runtime::format_session_trace(observed,sizeof observed,current,false)) { j.fail();return; }
  const int n=snprintf(line,sizeof line,
      "{\"kind\":\"shadow_session\",\"mono_ns\":%llu,\"domain\":\"model\","
      "\"assist_ready\":false,\"reset\":%s,\"input_available\":%s,"
      "\"model_session_epoch\":%llu,\"raw_since_ns\":%llu,\"session\":%s}",
      (unsigned long long)now,reset?"true":"false",session.available()?"true":"false",
      (unsigned long long)navigation.context().session_epoch,
      (unsigned long long)session.since_ns(),observed);
  if(n>0 && size_t(n)<sizeof line)j.line(line);else j.fail();
}
void sync_model_bus(Journal& j,mx5::runtime::ModelBus& bus,
                    N::Pipeline& navigation,N::GpsHoldout& holdout) {
  const mx5::runtime::bus_trace::Boundary current=A::read_position_bus();
  const uint64_t now=clock_ns(0);
  const mx5::runtime::ModelBus::Update update=bus.update(current,now);
  if(update==mx5::runtime::ModelBus::SAME)return;
  const bool reset=update==mx5::runtime::ModelBus::CHANGED;
  if(reset) {
    mx5_dr_context c=navigation.context();
    if(bus.exhausted() || c.source_epoch==UINT64_MAX || c.generation==UINT64_MAX) {
      disable_mutation();return;
    }
    ++c.source_epoch;++c.generation;
    navigation.reset(c);holdout.reset(c,N::HOLDOUT_BUS_RESET);
    journal_holdout(j,holdout,now);
  }
  char observed[128],line[600];
  if(!mx5::runtime::format_bus_trace(observed,sizeof observed,current.connection)) { j.fail();return; }
  const int n=snprintf(line,sizeof line,
      "{\"kind\":\"shadow_bus\",\"mono_ns\":%llu,\"domain\":\"model\","
      "\"assist_ready\":false,\"reset\":%s,\"input_available\":%s,"
      "\"model_bus_epoch\":%llu,\"bus_revision\":%llu,\"raw_since_ns\":%llu,\"connection\":%s}",
      (unsigned long long)now,reset?"true":"false",bus.available()?"true":"false",
      (unsigned long long)bus.epoch(),(unsigned long long)current.revision,
      (unsigned long long)bus.since_ns(),observed);
  if(n>0 && size_t(n)<sizeof line)j.line(line);else j.fail();
}
void sync_model_boundaries(Journal& j,mx5::runtime::ModelSession& session,
                           mx5::runtime::ModelBus& bus,N::Pipeline& nav,N::GpsHoldout& hold) {
  sync_model_session(j,session,nav,hold);sync_model_bus(j,bus,nav,hold);
}
void rejected_model_position(Journal& j,const A::Observation& o,const char* reason,
                             const mx5::runtime::ModelSession& session,
                             const mx5::runtime::ModelBus& bus) {
  char line[500];
  const int n=snprintf(line,sizeof line,
      "{\"kind\":\"shadow_position_rejected\",\"mono_ns\":%llu,\"domain\":\"model\","
      "\"assist_ready\":false,\"call\":%u,\"generation\":%u,\"reason\":\"%s\","
      "\"session_revision\":%llu,\"model_bus_epoch\":%llu,\"bus_revision\":%llu}",
      (unsigned long long)clock_ns(0),o.call_sequence,o.prediction_generation,reason,
      (unsigned long long)session.current().revision,(unsigned long long)bus.epoch(),
      (unsigned long long)bus.current().revision);
  if(n>0 && size_t(n)<sizeof line)j.line(line);else j.fail();
}

// One MODEL diagnostic row (100 ms cadence in the worker).
void journal_shadow(Journal& j,uint64_t now,const N::Pipeline& navigation,
                    const mx5::runtime::ModelSession& model_session,
                    const mx5::runtime::ModelBus& model_bus,uint64_t drain_calls) {
  char line[2048];
  const N::Diagnostic d=navigation.diagnostic(now);
  char lat[48],lon[48],heading[48],speed[48],error[48],preview[97]="";
  json_number(d.snapshot.latitude_deg,lat);json_number(d.snapshot.longitude_deg,lon);
  json_number(d.snapshot.body_heading_rad,heading);json_number(d.snapshot.speed_mps,speed);
  json_number(d.snapshot.error_budget_m,error);
  uint8_t bytes[48];
  const bool encoded=mx5::runtime::encode_model_location_preview(d.snapshot,bytes);
  if(encoded)hex48(bytes,preview);
  const int formatted=snprintf(line,sizeof line,
      "{\"kind\":\"shadow\",\"mono_ns\":%llu,\"domain\":\"model\","
      "\"model_session_epoch\":%llu,\"session_revision\":%llu,"
      "\"model_bus_epoch\":%llu,\"bus_revision\":%llu,"
      "\"model_valid\":%s,\"assist_ready\":false,\"state\":%u,"
      "\"result\":\"%s\",\"pipeline\":\"%s\",\"uncertainties\":%u,"
      "\"events\":%llu,\"intervals\":%llu,\"resets\":%llu,\"rejected\":%llu,"
      "\"drain_calls_total\":%llu,"
      "\"frontier_ns\":%llu,\"lat\":%s,\"lon\":%s,\"heading_rad\":%s,"
      "\"speed_mps\":%s,\"error_model_m\":%s,\"stopped\":%s,"
      "\"yaw_zero\":%.17g,\"calibration_version\":%llu,\"wheel_scale\":%.17g,"
      "\"wheel_scale_version\":%llu,\"preview_encoded\":%s,\"location_preview_hex\":\"%s\"}",
      (unsigned long long)now,(unsigned long long)navigation.context().session_epoch,
      (unsigned long long)model_session.current().revision,
      (unsigned long long)model_bus.epoch(),(unsigned long long)model_bus.current().revision,
      d.snapshot.model_valid?"true":"false",unsigned(d.snapshot.state),
      mx5_dr_result_name(d.result),N::pipeline_result_name(d.status.result),d.status.uncertainties,
      (unsigned long long)d.status.events,(unsigned long long)d.status.intervals,
      (unsigned long long)d.status.resets,(unsigned long long)d.status.rejected,
      (unsigned long long)drain_calls,
      (unsigned long long)d.snapshot.frontier_ns,lat,lon,heading,speed,error,
      d.snapshot.stopped?"true":"false",navigation.calibration().active_zero,
      (unsigned long long)navigation.calibration().calibration_version,
      navigation.wheel_calibration().active_scale,
      (unsigned long long)navigation.wheel_calibration().calibration_version,encoded?"true":"false",preview);
  if(formatted>0 && size_t(formatted)<sizeof line)j.line(line);else j.fail();
}

void journal_assist(Journal& j,const mx5::runtime::AssistStatus& s,uint64_t now) {
  const char* const states[]={"waiting_source","waiting_begin","waiting_input","published",
      "source_fault","input_fault","clock_fault","context_changed","backlog","stopped"};
  if(unsigned(s.state)>=sizeof states/sizeof states[0]) { j.fail();return; }
  char line[640];
  const int n=snprintf(line,sizeof line,
      "{\"kind\":\"assist_worker\",\"mono_ns\":%llu,\"state\":\"%s\","
      "\"candidate_ready\":%s,\"ticks\":%llu,\"inputs\":%llu,\"begins\":%llu,"
      "\"publications\":%llu,\"withdrawals\":%llu,\"ignored\":%llu,"
      "\"unpaired_positions\":%llu,"
      "\"pipeline\":\"%s\",\"bridge_result\":%u,"
      "\"last_frontier_ns\":%llu,\"last_valid_until_ns\":%llu}",
      (unsigned long long)now,states[unsigned(s.state)],s.state==mx5::runtime::ASSIST_PUBLISHED?"true":"false",
      (unsigned long long)s.ticks,(unsigned long long)s.inputs,(unsigned long long)s.begins,
      (unsigned long long)s.published,(unsigned long long)s.withdrawn,(unsigned long long)s.ignored,
      (unsigned long long)s.unpaired_positions,
      N::pipeline_result_name(s.pipeline_result),unsigned(s.bridge_result),
      (unsigned long long)s.last_publication.frontier_mono_ns,
      (unsigned long long)s.last_publication.valid_until_mono_ns);
  if(n>0 && size_t(n)<sizeof line)j.line(line);else j.fail();
}

} // namespace

namespace mx5 { namespace runtime {
// Kept separate so the same bounded drain can be exercised with a perpetually
// readable authored receiver as well as the real credentialed socket.
template<class Receiver> static unsigned drain_lds(Journal& journal,Receiver& receiver,
                                                  LdsRequestSource* source=0) {
  namespace L=lds_sideband;
  char line[L::JSON_CAPACITY];unsigned drained=0;
  while(drained<L::DRAIN_LIMIT&&!journal.failed) {
    L::Record record;L::Diagnostic diagnostic;
    const L::ReceiveResult result=receiver.receive(&record,&diagnostic);
    if(result==L::EMPTY)break;
    ++drained;
    bool formatted=false;
    if(result==L::RECORD)formatted=L::format_record(line,sizeof line,record,diagnostic);
    if(!formatted) {
      if(result==L::RECORD)diagnostic.fault=L::BAD_RECORD;
      formatted=L::format_status(line,sizeof line,"rejected",diagnostic);
    }
    if(formatted)journal.line(line);
    if(source && result==L::RECORD && formatted && !journal.failed)
      source->sideband(record,diagnostic,clock_ns(0));
    // Do not spin on a broken descriptor or let metadata acquisition faults
    // suppress the independently queued OEM POSITION/SEND observations.
    if(diagnostic.fault==L::SYSCALL_FAILED) {
      if(source)source->reset(clock_ns(0));
      receiver.close_channel();break;
    }
  }
  if(drained==L::DRAIN_LIMIT) {
    L::Diagnostic diagnostic=L::Diagnostic();diagnostic.received_ns=clock_ns(0);
    if(L::format_status(line,sizeof line,"drain_limit",diagnostic,drained))journal.line(line);
  }
  return drained;
}
void* run_worker_association(const char* root,const char* motion_channel,const char* lds_channel,
                       uid_t lds_uid,AssistWorker* assist,LdsRequestSource* supplied_source,
                       const char* association_channel) {
  LA::Registry* associations=prepare_association_owner();
  LdsRequestSource local_source;
  LdsRequestSource& source=supplied_source?*supplied_source:local_source;
  // Stop on every exit, including startup failures before the main loop.
  struct StopAssist {
    AssistWorker* worker;
    LdsRequestSource* source;
    LA::Registry* associations;
    ~StopAssist() {
      if(associations) { associations->retire();associations->close_channel(); }
      if(worker)worker->stop();
      source->reset(clock_ns(0));
    }
  } stop_assist={assist,&source,associations};
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
  // Journal file I/O on its own thread; without one (allocation or thread
  // failure) rows stay synchronous as before, and the boot row says so.
  // The persistent profile adds a 128 KiB BULK ring for its paced RAW
  // window rows (2026-10-08).
  const bool journal_thread=config.log_profile==mx5::runtime::LOG_PROFILE_PERSISTENT?
      j.start_writer(524288,262144,131072):j.start_writer();
  // log_profile=persistent (validation/PERSISTENT_LOGGING_2026-10-06.md):
  // digest rows and an in-memory RAW window instead of every raw row. The
  // window storage is allocated once here; without it the profile still
  // applies but raw rows are only counted.
  struct QuietStorage {
    unsigned char* bytes;
    ~QuietStorage() { delete[] bytes; }
  } quiet_storage={0};
  mx5::runtime::PersistentLog quiet;
  const bool quiet_profile=config.log_profile==mx5::runtime::LOG_PROFILE_PERSISTENT;
  if(quiet_profile) {
    quiet_storage.bytes=new(std::nothrow) unsigned char[mx5::runtime::PersistentLog::WINDOW_BYTES+
                                                       mx5::runtime::PersistentLog::ROW_BYTES];
    quiet.init(quiet_storage.bytes,N::research_model_profile());
    j.filter=&quiet;
  }
  mx5::runtime::MotionBatch motion_batch;
  char line[mx5::runtime::OBSERVATION_JSON_CAPACITY];
  char boot_id[37];
  mx5::runtime::read_boot_id(boot_id);
  // BETA (config mode 5) at boot: "enabled" is the install-time adapter
  // opt-in (allow_beta with an installed hook). The worker's own decision
  // after this row is durable is the first beta_state row.
  const bool beta_requested=config.mode==5;
  const bool beta_allowed=beta_requested && hook_installed;
  const bool beta_declined=beta_requested && hook_installed && install_report.sessions_declined;
  const char* beta_boot_reason=!beta_requested?"not_requested":
      !hook_installed?"hook_not_installed":"adapter_opt_in";
  snprintf(line, sizeof line,
           "{\"kind\":\"boot\",\"schema\":1,\"pid\":%ld,\"mono_ns\":%llu,"
           "\"boot_id\":\"%s\","
           "\"mode\":%u,\"install\":\"%s\",\"assist_ready\":false,\"assist_"
           "block\":\"sensor_timing_quality_calibration_unverified\",\"wire_"
           "timestamp_modified\":false,"
           "\"session_hooks\":\"%s\",\"journal_writer\":\"%s\",\"log_profile\":\"%s\",\"raw_window\":\"%s\","
           "\"beta\":{\"mode\":\"%s\",\"enabled\":%s,\"reason\":\"%s\",\"session_fence\":\"%s\"},"
           "\"install_diag\":{\"stage\":%u,\"slot_offset\":%llu,\"expected_offset\":%llu,"
           "\"observed_offset\":%llu,\"owner\":\"%s\",\"symbol\":\"%s\"}}",
           (long)getpid(), (unsigned long long)clock_ns(0), boot_id, config.mode,
           boot_result,
           !hook_installed ? "none"
               : install_report.sessions_declined ? "declined_third_party_interposer" : "observing",
           journal_thread ? "thread" : "inline",
           quiet_profile ? "persistent" : "full",
           !quiet_profile ? "none" : quiet.window_available() ? "available" : "unavailable",
           beta_requested ? "BETA" : "off", beta_allowed ? "true" : "false", beta_boot_reason,
           !beta_requested ? "none" : beta_declined ? "declined_send_storage_counter"
                                                    : "observed_session_and_send_storage_counter",
           install_report.declined_stage,
           (unsigned long long)install_report.slot_offset,
           (unsigned long long)install_report.slot_expected_offset,
           (unsigned long long)install_report.observed_offset,
           install_report.owner, install_report.symbol);
  j.line(line);
  // Later decisions (SCRUB/BETA) require this row to have reached the file.
  const bool boot_durable=j.flush_wait();
  // Optional observation transport failure must not suppress raw capture.
  if(associations && !j.failed && !__sync_fetch_and_add(&audit_fault,0))
    associations->open_channel(association_channel,lds_uid);
  lds_sideband::Receiver lds;
  if(!j.failed) {
    const bool opened=lds.open_channel(lds_channel,lds_uid);
    const int open_errno=opened?0:errno;
    lds_sideband::Diagnostic diagnostic=lds_sideband::Diagnostic();
    diagnostic.received_ns=clock_ns(0);
    if(!opened) { diagnostic.fault=lds_sideband::SYSCALL_FAILED;diagnostic.syscall_errno=open_errno; }
    if(lds_sideband::format_status(line,sizeof line,opened?"opened":"unavailable",diagnostic))j.line(line);
  }
  N::Pipeline navigation;
  N::GpsHoldout holdout;
  N::MotionReceiver motion;
  mx5::runtime::ModelSession model_session;
  // Design decision 6: only a KNOWN decline (third-party session shim)
  // replaces the session fence with the send-time storage counter.
  model_session.accept_declined(beta_declined);
  mx5::runtime::ModelBus model_bus;
  ModelSession source_session;
  LdsSourceBus source_bus;
  const N::ModelProfile model=N::research_model_profile();
  mx5_dr_context nav_context={1,1,1}; // local diagnostic identity, not LDS provenance
  // BETA (5) keeps SHADOW capture and adds the MODEL reverse latch and the
  // separate BETA core (decisions 7 and 9). SHADOW (4) is unchanged.
  const bool capture=(config.mode==4 || beta_requested) && motion.open_channel(motion_channel);
  bool shadow=capture && hook_installed &&
      navigation.init_model(model,mx5_dr_default_config(),nav_context,true,true,beta_requested) &&
      holdout.init_model(model,mx5_dr_default_config(),nav_context);
  const bool beta_core=shadow && beta_requested && navigation.enable_beta(mx5::runtime::beta_profile());
  mx5::runtime::BetaController beta(beta_shared);
  if(config.mode==4 || beta_requested) {
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
  if (hook_installed && (config.mode == 2 || beta_requested) && !boot_durable && !j.failed) {
    // The boot row did not reach the file within the bound: SCRUB/BETA stay
    // off for this worker, with this reason (not a journal failure).
    j.line("{\"kind\":\"journal_not_durable\",\"stage\":\"boot\",\"assist_ready\":false}");
  }
  if (hook_installed && config.mode == 2 && boot_durable && !j.failed &&
      !__sync_fetch_and_add(&audit_fault, 0)) {
    A::set_mode(A::SCRUB_STALE);
    if (__sync_fetch_and_add(&audit_fault, 0))
      A::set_mode(A::OBSERVE);
  }
  if (beta_requested) {
    // Same pattern as SCRUB: only after the durable boot row, with an
    // installed hook and no audit fault. Every outcome is journaled.
    const bool durable=j.flush_wait();
    const char* blocked=beta_block_reason(hook_installed,j.failed,durable && boot_durable,
        __sync_fetch_and_add(&audit_fault,0)!=0,capture,shadow,beta_core);
    beta.enable(j,clock_ns(0),blocked);
    if (__sync_fetch_and_add(&audit_fault, 0))
      beta.fault(j,clock_ns(0),"audit_fault");
    j.flush();
  }
  uint64_t last_flush = 0;
  uint64_t last_shadow_log=0;
  uint64_t last_calibration_log=0;
  uint64_t last_stop_check=0;
  uint64_t last_assist_log=0;
  uint64_t last_assist_unpaired=0;
  AssistState last_assist_state=ASSIST_WAITING_SOURCE;
  uint64_t drain_calls=0;
  bool source_disabled=false;
  bool adapter_fault_reported=false;
  mx5::runtime::WorkerTick model_tick;
  for (;;) {
    j.poll();   // adopt a writer-thread failure before any decision below
    if(beta_requested)journal_lag_guard(j,clock_ns(0));
    const uint64_t cutoff=clock_ns(0);
    source.advance(cutoff);
    // The first LDS submission discovers its bus before issuing the request.
    // A later worker poll must not treat that discovery as a lost lifetime.
    // Subsequent observed changes still retire the window and reject old input.
    const ModelSession::Update session_update=source_session.update(A::read_issue_session(),cutoff);
    const bool bus_changed=source_bus.update(A::read_position_bus());
    if(session_update==ModelSession::CHANGED || bus_changed) {
      source.reset(cutoff);
      if(associations)associations->retire();
    }
    bool stopping=false;
    if(cutoff>=last_stop_check && cutoff-last_stop_check>=1000000000ULL) {
      last_stop_check=cutoff;
      stopping=stop_requested(root);
      if(stopping) {
        if(assist)assist->stop();
        source.reset(cutoff);source_disabled=true;
        freeze_capture();
        // capture.stop takes precedence: OBSERVE is already set; drop any
        // stored BETA candidate and record why.
        beta.disable(j,cutoff,"capture_stop");
        if(shadow) {
          navigation.reset(navigation.context());
          holdout.reset(navigation.context(),N::HOLDOUT_CAPTURE_STOP);
          journal_holdout(j,holdout,cutoff);
        }
        shadow=false;
      } else if(beta.live() && disable_marker_present(root)) {
        // A same-boot disable-next-start request also ends BETA now; raw
        // capture continues until capture.stop.
        beta.disable(j,cutoff,"disable_next_start");
      }
    }
    if(shadow)sync_model_boundaries(j,model_session,model_bus,navigation,holdout);
    if(associations && !stopping && !j.failed && !__sync_fetch_and_add(&audit_fault,0))
      associations->drain(cutoff);
    A::Observation o;
    unsigned drained = 0;
    while (drained++ < 256 && pop(&o)) {
      if(!format_observation(line,sizeof line,o)) { j.fail();continue; }
      j.observation_line(line,o);
      if(o.kind==A::Observation::SEND)beta.send(o);
      if(o.kind==A::Observation::POSITION && o.reason==A::CONTEXT_UNAVAILABLE) {
        // A pool miss keeps the raw input but cannot establish a usable
        // POSITION context. Revoke mutation and all later MODEL/source input.
        j.line("{\"kind\":\"capture_incomplete\",\"reason\":\"adapter_context_unavailable\",\"assist_ready\":false}");
        adapter_fault_reported=true;
        disable_mutation();
        continue;
      }
      if (o.kind == A::Observation::POSITION) {
        if(!source_disabled && !j.failed && !__sync_fetch_and_add(&audit_fault,0)) {
          // A lifecycle may change while this turn drains 256 queued rows.
          // Recheck before each source admission, as SHADOW does below.
          const uint64_t position_cutoff=clock_ns(0);
          const ModelSession::Update update=
              source_session.update(A::read_issue_session(),position_cutoff);
          const bool bus_update=source_bus.update(A::read_position_bus());
          if(update==ModelSession::CHANGED || bus_update) {
            source.reset(position_cutoff);
            // This inner poll consumes the transition. The next outer poll
            // may already see the new baseline, so retire the map here too.
            if(associations)associations->retire();
          }
          source.position(o,position_cutoff);
        }
        if(shadow && !__sync_fetch_and_add(&audit_fault,0)) {
          sync_model_boundaries(j,model_session,model_bus,navigation,holdout);
          const char* reason=model_session.reject(o);
          if(!reason)reason=model_bus.reject(o);
          if(reason)rejected_model_position(j,o,reason,model_session,model_bus);
          else {
            const uint64_t resets=navigation.status().resets;
            navigation.enqueue_position(o);
            journal_pipeline_reset(j,navigation,resets,"position",o.mono_ns,0,0,o.call_sequence);
            holdout.enqueue_position(o);
          }
        }
        beta.position(j,o,clock_ns(0));
      }
    }
    if(!adapter_fault_reported && A::faulted()) {
      j.line("{\"kind\":\"capture_incomplete\",\"reason\":\"adapter_fault\",\"assist_ready\":false}");
      adapter_fault_reported=true;
      disable_mutation();
    }
    // Resolve owned records on this worker without waiting for the other half
    // or modifying the original POSITION/SEND. This does not qualify time,
    // sensor freshness, receiver identity or the already completed callback.
    uint64_t now = clock_ns(0);
    if(!source_disabled && (j.failed || __sync_fetch_and_add(&audit_fault,0))) {
      source.reset(now);source_disabled=true;
    }
    if(shadow && (j.failed || __sync_fetch_and_add(&audit_fault,0))) {
          navigation.reset(navigation.context());
          holdout.reset(navigation.context(),N::HOLDOUT_AUDIT_RESET);
          journal_holdout(j,holdout,now);
          j.line("{\"kind\":\"shadow_disabled\",\"reason\":\"audit_fault\",\"assist_ready\":false}");
          shadow=false; // Permanent for this worker, even if a fault flag changes.
    }
    if(lds.active()&&!j.failed)drain_lds(j,lds,source_disabled?0:&source);
    if(shadow)sync_model_boundaries(j,model_session,model_bus,navigation,holdout);
    const bool bus_boundary=model_bus.since_ns()>model_session.since_ns();
    if(capture && !j.failed)drain_motion(j,motion_batch,motion,navigation,holdout,
        shadow && model_session.available() && model_bus.available(),
        bus_boundary?model_bus.since_ns():model_session.since_ns(),bus_boundary);
    if(stopping) {
      lds.close_channel();
      lds_sideband::Diagnostic diagnostic=lds_sideband::Diagnostic();
      diagnostic.received_ns=clock_ns(0);
      if(lds_sideband::format_status(line,sizeof line,"closed",diagnostic))j.line(line);
      if(drain_capture_tail(j)) {
        if(assist)journal_assist(j,assist->status(),clock_ns(0));
        finish_capture(j,boot_id,cutoff,clock_ns(0),&source);
      }
      return 0; // Even failed finalization cannot reopen this capture.
    }
    now=clock_ns(0);
    bool model_ticked=false;
    if(shadow && !j.failed && !__sync_fetch_and_add(&audit_fault,0) && model_tick.due(now)) {
        model_ticked=true;
        sync_model_boundaries(j,model_session,model_bus,navigation,holdout);
        if(now>navigation.reorder_ns()) {
          const uint64_t resets=navigation.status().resets,watermark=now-navigation.reorder_ns();
          navigation.drain(watermark);
          if(drain_calls!=UINT64_MAX)++drain_calls;
          journal_pipeline_reset(j,navigation,resets,"drain",watermark);
        }
        if(now>navigation.reorder_ns())holdout.drain(now-navigation.reorder_ns());
        // A lifecycle can complete while this worker computes. A coherent
        // recheck clears queued predictions before its next diagnostic snapshot.
        sync_model_boundaries(j,model_session,model_bus,navigation,holdout);
        now=clock_ns(0);
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
          journal_shadow(j,now,navigation,model_session,model_bus,drain_calls);
        }
    }
    if(beta.live() && (model_ticked || !shadow)) {
      // BETA publication on the MODEL tick cadence, after the drain above.
      // Faults are sticky and return the adapter to OBSERVE.
      const char* fault=j.failed?"journal_failed":
          __sync_fetch_and_add(&audit_fault,0)?"audit_fault":
          A::faulted()?"adapter_fault":
          A::mode()!=A::BETA?"adapter_mode_changed":
          !shadow?"model_disabled":0;
      now=clock_ns(0);
      // A POSITION or SEND gap above 3 s (reconnect) ends the session
      // evidence: the BETA anchor state must be qualified again.
      if(!fault && beta.cadence_fence(j,now))navigation.fence_beta();
      beta.tick(j,now,navigation,model_session.available() && model_bus.available(),
                navigation.status().last_received_ns,model_bus.epoch(),fault);
    }
    j.pump(clock_ns(0));
    if (now - last_flush >= 1000000000ULL) {
      last_flush = now;
      j.tick(now);
      journal_health(j,now,capture&&!j.failed,shadow && model_session.available() && model_bus.available(),&source);
      j.flush();
    }
    // The live qualified source remains unimplemented. A future verified
    // backend uses this same worker, after raw capture and journal checks.
    // The controller never enables adapter mutation or supplies provenance.
    if(assist) {
      if(config.mode==3 && hook_installed && !j.failed &&
         !__sync_fetch_and_add(&audit_fault,0))assist->tick(clock_ns,0);
      else assist->stop();
      const AssistStatus& status=assist->status();
      now=clock_ns(0);
      if(!last_assist_log || status.state!=last_assist_state ||
         status.unpaired_positions!=last_assist_unpaired ||
         (now>=last_assist_log && now-last_assist_log>=1000000000ULL)) {
        last_assist_log=now;last_assist_state=status.state;
        last_assist_unpaired=status.unpaired_positions;
        journal_assist(j,status,now);
      }
      if(j.failed || __sync_fetch_and_add(&audit_fault,0))assist->stop();
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
void* run_worker(const char* root,const char* motion_channel,AssistWorker* assist) {
  return run_worker_channels(root,motion_channel,lds_sideband::CHANNEL_NAME,lds_sideband::LDS_UID,assist);
}
void* run_worker_channels(const char* root,const char* motion_channel,const char* lds_channel,
                         uid_t lds_uid,AssistWorker* assist) {
  return run_worker_inputs(root,motion_channel,lds_channel,lds_uid,assist,0);
}
void* run_worker_inputs(const char* root,const char* motion_channel,const char* lds_channel,
                       uid_t lds_uid,AssistWorker* assist,LdsRequestSource* source) {
  return run_worker_association(root,motion_channel,lds_channel,lds_uid,assist,source,
                               lds_association::CHANNEL_NAME);
}
} }

namespace {
void* worker_at(const char* root,const char* motion_channel="mx5dr.motion.v1") {
  // TODO: connect the physically verified sensor and per-request provider
  // backend. Never substitute the MODEL source or observed receipt clock.
  return mx5::runtime::run_worker(root,motion_channel,0);
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
    io.runtime = product_options();
    io.observe_requests = true;
    io.blm_handle = h;
    io.report = &install_report;
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
  if (!mx5::runtime::create_thread(&thread, worker, 0,
                                   mx5::runtime::WORKER_STACK_BYTES, true)) {
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
