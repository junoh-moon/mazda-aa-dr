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
mx5::runtime::Config config = {0, 8388608, 3, 1000, false};
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
  if (!journal_ok.load(std::memory_order_acquire)) { memset(out, 0, sizeof *out); return false; }
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
bool journal_writer_push(JournalWriter*,const char*,size_t,bool evidence);
bool journal_writer_ok(JournalWriter*);
void journal_writer_request_flush(JournalWriter*);
bool journal_writer_flush_wait(JournalWriter*,uint64_t timeout_ns);
JournalWriter* start_journal_writer(const char* root,size_t diagnostic_bytes,size_t evidence_bytes);

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
  bool failed;
  JournalWriter* writer;
  explicit Journal(const char *directory = ROOT)
      : root(directory), f(0), written(0), failed(false), writer(0) {}
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
  // Ring sizes: 384 KiB of diagnostic rows (about 10 s of the full profile's
  // peak rate) and 128 KiB that only evidence rows may use. false: no
  // writer (allocation/thread failure); rows stay synchronous.
  bool start_writer(size_t diagnostic_bytes=393216,size_t evidence_bytes=131072) {
    if (writer || f || failed) return false;
    writer=start_journal_writer(root,diagnostic_bytes,evidence_bytes);
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
    if (writer) {
      poll();
      if (failed) return;
      const size_t n = strlen(s);
      // An evidence row that cannot be queued is a journal failure: fail
      // closed (mutation disabled) instead of dropping it.
      if (!journal_writer_push(writer, s, n, mx5::runtime::journal_evidence_row(s, n))) fail();
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
  unsigned char* storage;      // diagnostic ring | evidence ring | row buffer
  mx5::runtime::JournalRing ring;
  pthread_t thread;
  std::atomic<unsigned> ok, stopping, durable, closed_ok;
  std::atomic<uint64_t> flush_target, flushed, written;
  std::atomic<uint64_t> inject_stall_ns;   // tests only: one stall before the next row
  char* row_buffer;                         // writer thread only
  JournalWriter(const char* r,unsigned char* buffer,size_t diagnostic,size_t evidence)
      : root(r),storage(buffer),ring(buffer,diagnostic,buffer+diagnostic,evidence),thread(),
        ok(1),stopping(0),durable(0),closed_ok(0),flush_target(0),flushed(0),written(0),
        inject_stall_ns(0),row_buffer(0) {}
};
const size_t JOURNAL_ROW_BUFFER=mx5::runtime::JournalRing::MAX_ROW+1;
void journal_dropped(Journal& file,uint64_t first,uint64_t next,
                     const mx5::runtime::JournalRing::Stats& stats) {
  char line[400];
  const int n=snprintf(line,sizeof line,
      "{\"kind\":\"journal_dropped\",\"schema\":1,\"mono_ns\":%llu,\"class\":\"diagnostic\","
      "\"rows\":%llu,\"first_seq\":%llu,\"last_seq\":%llu,\"dropped_total\":%llu,"
      "\"dropped_bytes_total\":%llu,\"reason\":\"writer_backlog\"}",
      (unsigned long long)clock_ns(0),(unsigned long long)(next-first),(unsigned long long)first,
      (unsigned long long)(next-1),(unsigned long long)stats.dropped_rows,
      (unsigned long long)stats.dropped_bytes);
  if(n>0 && size_t(n)<sizeof line)file.line(line);else file.fail();
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
    uint64_t expected=0;
    for(;;) {
      // Read stop BEFORE draining: every row pushed before stop() is written.
      const bool stop=w.stopping.load(std::memory_order_acquire)!=0;
      unsigned batch=0;size_t n;uint64_t seq;bool evidence;
      while(batch<64 && w.ring.pop(buffer,JOURNAL_ROW_BUFFER,&n,&seq,&evidence)) {
        ++batch;
        const uint64_t stall=w.inject_stall_ns.exchange(0,std::memory_order_acq_rel);
        if(stall) { struct timespec t={time_t(stall/1000000000ULL),long(stall%1000000000ULL)};nanosleep(&t,0); }
        if(seq!=expected && !file.failed)journal_dropped(file,expected,seq,w.ring.stats());
        expected=seq+1;
        if(!file.failed)file.line(buffer);
        w.written.store(expected,std::memory_order_release);
      }
      if(file.failed)w.ok.store(0,std::memory_order_release);
      const uint64_t target=w.flush_target.load(std::memory_order_acquire);
      if(target>w.flushed.load(std::memory_order_acquire) && expected>=target) {
        file.flush();
        if(file.failed)w.ok.store(0,std::memory_order_release);
        w.flushed.store(expected,std::memory_order_release);
      }
      if(stop && !batch) {
        const mx5::runtime::JournalRing::Stats stats=w.ring.stats();
        if(expected<stats.next_seq && !file.failed)journal_dropped(file,expected,stats.next_seq,stats);
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
      if(!batch) { const struct timespec pause={0,5000000};nanosleep(&pause,0); }
    }
  }
  uselocale(LC_GLOBAL_LOCALE);
  if(numeric)freelocale(numeric);
  return 0;
}
JournalWriter* start_journal_writer(const char* root,size_t diagnostic_bytes,size_t evidence_bytes) {
  unsigned char* buffer=new(std::nothrow) unsigned char[diagnostic_bytes+evidence_bytes+JOURNAL_ROW_BUFFER];
  if(!buffer)return 0;
  JournalWriter* w=new(std::nothrow) JournalWriter(root,buffer,diagnostic_bytes,evidence_bytes);
  if(!w) { delete[] buffer;return 0; }
  w->row_buffer=reinterpret_cast<char*>(buffer+diagnostic_bytes+evidence_bytes);
  // Joinable, explicit stack (the stock 128 KiB default is not relied on).
  if(!mx5::runtime::create_thread(&w->thread,journal_writer_main,w,256u<<10,false)) {
    delete w;delete[] buffer;return 0;
  }
  return w;
}
void stop_journal_writer(JournalWriter* w,bool durable,bool* ok) {
  w->durable.store(durable?1:0,std::memory_order_release);
  w->stopping.store(1,std::memory_order_release);
  pthread_join(w->thread,0);
  *ok=w->closed_ok.load(std::memory_order_acquire)!=0;
  unsigned char* buffer=w->storage;
  delete w;delete[] buffer;
}
bool journal_writer_push(JournalWriter* w,const char* s,size_t n,bool evidence) {
  const mx5::runtime::JournalRing::Result r=w->ring.push(s,n,evidence);
  return r==mx5::runtime::JournalRing::PUSHED || r==mx5::runtime::JournalRing::PUSHED_AFTER_DROP;
}
bool journal_writer_ok(JournalWriter* w) { return w->ok.load(std::memory_order_acquire)!=0; }
void journal_writer_request_flush(JournalWriter* w) {
  const uint64_t next=w->ring.stats().next_seq;
  if(next>w->flush_target.load(std::memory_order_acquire))w->flush_target.store(next,std::memory_order_release);
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
      "\"events\":%llu,\"max_late_ms\":%llu,\"fresh_limit_ms\":%llu,\"late_limit_ms\":%llu,"
      "\"late_accepted_total\":%llu}",
      (unsigned long long)m.burst_checked_ns,(unsigned long long)m.burst_epoch,
      (unsigned long long)m.burst_first_seq,(unsigned long long)m.burst_last_seq,
      (unsigned long long)m.burst_events,(unsigned long long)(m.burst_max_ns/1000000ULL),
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
  char line[1600],source_status[420]="";
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
      "\"motion_late\":{\"accepted\":%llu,\"bursts\":%llu,\"max_late_ms\":%llu}%s}",
      (unsigned long long)now,(unsigned long long)queue.dropped(),hook_installed?"true":"false",
      unsigned(A::mode()),__sync_fetch_and_add(&audit_fault,0),capture?"true":"false",
      computation?"true":"false",h.prepared?"true":"false",h.abi_fault?"true":"false",
      A::R::result_name(h.result),(unsigned long long)h.ledger.loss_epoch,h.ledger.requests,
      h.ledger.workers,h.ledger.loss_reasons,h.ledger.exhausted?"true":"false",
      s.prepared?"true":"false",s.contexts,unsigned(A::SESSION_CONTEXT_CAPACITY),s.faults,
      b.prepared?"true":"false",b.contexts,unsigned(A::BUS_CONTEXT_CAPACITY),b.faults,
      (unsigned long long)motion_late.accepted,(unsigned long long)motion_late.bursts,
      (unsigned long long)(motion_late.max_ns/1000000ULL),source_status);
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
      if(format_observation(line,sizeof line,o))j.line(line);else j.fail();
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
  const bool journal_thread=j.start_writer();
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
           "\"session_hooks\":\"%s\",\"journal_writer\":\"%s\","
           "\"beta\":{\"mode\":\"%s\",\"enabled\":%s,\"reason\":\"%s\",\"session_fence\":\"%s\"},"
           "\"install_diag\":{\"stage\":%u,\"slot_offset\":%llu,\"expected_offset\":%llu,"
           "\"observed_offset\":%llu,\"owner\":\"%s\",\"symbol\":\"%s\"}}",
           (long)getpid(), (unsigned long long)clock_ns(0), boot_id, config.mode,
           boot_result,
           !hook_installed ? "none"
               : install_report.sessions_declined ? "declined_third_party_interposer" : "observing",
           journal_thread ? "thread" : "inline",
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
    const char* blocked=!hook_installed?"hook_not_installed":
        (j.failed || !durable || !boot_durable)?"journal_failed":
        __sync_fetch_and_add(&audit_fault,0)?"audit_fault":
        !capture?"motion_capture_unavailable":
        !shadow?"model_unavailable":
        !beta_core?"beta_core_unavailable":0;
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
      j.line(line);
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
      beta.tick(j,now,navigation,model_session.available() && model_bus.available(),
                navigation.status().last_received_ns,model_bus.epoch(),fault);
    }
    if (now - last_flush >= 1000000000ULL) {
      last_flush = now;
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
