// Includes the private logger so failure paths can be tested without exposing
// configuration switches in the shipped runtime. No firmware is loaded.
#include "storage_fixture.h"
// Test-only journal writer hooks (stall injection) compiled into this TU only.
#define MX5DR_JOURNAL_TEST_HOOKS 1
#define statvfs(path,info) fixture_statvfs(path,info)
#include "../../src/runtime/runtime.cpp"
#undef statvfs
#include <algorithm>
#include <cassert>
#include <string>
#include <fstream>
#include <sstream>
#include <iterator>
#include <vector>
#include <new>
#include <limits>

static int32_t unused_next(void *, A::VehicleData *) { return 0; }
struct CadenceResult { unsigned late,holdout_late;uint64_t resets;std::vector<unsigned> drains; };
static CadenceResult cadence_case(unsigned mode,unsigned wheel_period,unsigned reverse_period) {
  N::Pipeline navigation;N::GpsHoldout holdout;const mx5_dr_context context={1,1,1};
  assert(navigation.init_model(N::research_model_profile(),mx5_dr_default_config(),context,true,true));
  assert(holdout.init_model(N::research_model_profile(),mx5_dr_default_config(),context));
  mx5::runtime::WorkerTick tick;CadenceResult result=CadenceResult();
  std::vector<N::RawEvent> pending;uint64_t sequence=0;
  unsigned wake_deadline=0,last_model=0;
  for(unsigned ms=0;ms<=1500;++ms) {
    const uint64_t now=1000000000ULL+uint64_t(ms)*1000000ULL;
    for(unsigned sensor=1;sensor<=3;++sensor) {
      const unsigned period=sensor==N::WHEELS?wheel_period:sensor==N::YAW?150:reverse_period;
      if(ms%period)continue;
      N::RawEvent event=N::RawEvent();event.kind=static_cast<N::SensorKind>(sensor);
      event.epoch=1;event.receive_seq=++sequence;event.received_ns=now;
      if(sensor==N::WHEELS)for(unsigned j=0;j<4;++j)event.raw[j]=13600;
      if(sensor==N::YAW){event.raw[0]=2047;event.count=1;}
      pending.push_back(event);
    }
    const bool woke=mode==0?ms%50==0:(!pending.empty()||ms>=wake_deadline);
    if(!woke)continue;
    for(size_t j=0;j<pending.size();++j) {
      const N::PipelineResult a=navigation.enqueue_raw(pending[j]),b=holdout.enqueue_raw(pending[j]);
      if(a==N::PIPELINE_LATE)++result.late;
      else assert(a==N::PIPELINE_OK||a==N::PIPELINE_WAITING);
      if(b==N::PIPELINE_LATE)++result.holdout_late;
      else assert(b==N::PIPELINE_OK||b==N::PIPELINE_WAITING);
    }
    pending.clear();
    const bool due=mode==3?tick.due(now):(mode<2||result.drains.empty()||ms-last_model>=50);
    if(due) {
      last_model=ms;result.drains.push_back(ms);
      navigation.drain(now-navigation.reorder_ns());holdout.drain(now-navigation.reorder_ns());
    }
    wake_deadline=ms+(mode==3?tick.wait_ms(now):50);
    N::HoldoutResult ignored;while(holdout.pop(&ignored)){}
  }
  result.resets=navigation.status().resets;return result;
}
static void cadence_tests() {
  for(unsigned wheel=10;wheel<=20;wheel+=10) {
    const CadenceResult old=cadence_case(0,wheel,5*wheel);
    const CadenceResult every_input=cadence_case(1,wheel,5*wheel);
    const CadenceResult minimum_gate=cadence_case(2,wheel,5*wheel);
    const CadenceResult fixed=cadence_case(3,wheel,5*wheel);
    assert(old.late==0&&old.holdout_late==0&&old.resets==0);
    // Warmup now retains open yaw windows even with these alternate wake
    // schedules. Keep the cadence regression independent of that old fault:
    // input-driven draining runs too often, and a minimum-only gate drifts.
    assert(every_input.late==0&&every_input.holdout_late==0&&every_input.resets==0);
    assert(minimum_gate.late==0&&minimum_gate.holdout_late==0&&minimum_gate.resets==0);
    assert(every_input.drains!=old.drains&&every_input.drains.size()>old.drains.size());
    if(wheel==20)
      assert(minimum_gate.drains!=old.drains&&minimum_gate.drains.size()<old.drains.size());
    assert(fixed.late==0&&fixed.holdout_late==0&&fixed.resets==0);
    assert(fixed.drains==old.drains&&fixed.drains.size()==31);
  }
  puts("worker cadence: 150ms yaw, 10/20ms wheels; warmup survives all schedules, deadline preserves 31 drains");
}
static void arm_test_mode() {
  audit_fault = 0;
  // Test-only reset, with no concurrent queue users.
  queue.~ObservationQueue();new(&queue) ObservationQueue;
  assert(A::set_mode(A::SCRUB_STALE));
  journal_ok.store(1);journal_current.store(1);
}
struct FakeReceiver {
  unsigned calls,limit;
  bool gap,bad_model;
  N::MotionCursor cursor;
  FakeReceiver(unsigned n,bool missing=false,bool invalid_model=false):
    calls(0),limit(n),gap(missing),bad_model(invalid_model) {}
  N::ReceiveResult receive(N::RawEvent* out,N::ReceiveDiagnostic* d) {
    if(calls==limit)return N::CHANNEL_EMPTY;
    const uint64_t now=clock_ns(0);
    N::RawEvent e=N::RawEvent();e.kind=N::WHEELS;e.epoch=1;
    ++calls;e.receive_seq=calls+(gap && calls>1?1:0);e.received_ns=now;
    for(unsigned i=0;i<4;++i)e.raw[i]=bad_model&&calls==1?50000:10000;
    unsigned char bytes[N::MOTION_RECORD_SIZE];assert(N::encode_motion(e,bytes));
    const N::MotionDatagram packet={bytes,sizeof bytes,false,true,42,0};
    return N::inspect_motion_datagram(packet,0,now,cursor,out,d);
  }
};
static void receive_turn_tests(const char* root,const std::string& logs) {
  config.max_log_bytes=65536;
  const mx5_dr_context context={1,1,1};
  for(unsigned mode=0;mode<4;++mode) {
    arm_test_mode();
    N::Pipeline navigation;N::GpsHoldout holdout;
    assert(navigation.init_model(N::research_model_profile(),mx5_dr_default_config(),context));
    assert(holdout.init_model(N::research_model_profile(),mx5_dr_default_config(),context));
    if(mode==1)audit_fault=1;
    // Each case reads its own rows: a small trace.0 would otherwise be
    // appended to by the next Journal (log retention, 2026-10-10).
    unlink((logs+"/trace.0.jsonl").c_str());
    {
      Journal j(root);mx5::runtime::MotionBatch batch;FakeReceiver receiver(3,true);
      drain_motion(j,batch,receiver,navigation,holdout,mode!=2,mode==3?UINT64_MAX:0);j.flush();
      assert(!j.failed && batch.empty() && receiver.calls==3);
      assert(navigation.status().events==(mode==0?2:0));
      assert(navigation.context().generation==(mode==0||mode==3?2:1));
    }
    std::ifstream f((logs+"/trace.0.jsonl").c_str());std::string line;
    unsigned batches=0,rejected=0,resets=0;
    while(std::getline(f,line)) {
      if(line.find("motion_batch")!=std::string::npos)++batches;
      if(line.find("motion_rejected")!=std::string::npos) {
        ++rejected;
        assert(line.find("\"reason\":\"sequence_discontinuity\"")!=std::string::npos);
        assert(line.find("\"receive_seq\":3")!=std::string::npos);
        assert(line.find("\"authenticated_decoded\":true")!=std::string::npos);
      }
      if(line.find("shadow_input_reset")!=std::string::npos)++resets;
    }
    assert(batches==2 && rejected==1 && resets==1);
  }
  arm_test_mode();
  N::Pipeline navigation;N::GpsHoldout holdout;
  Journal j(root);mx5::runtime::MotionBatch batch;FakeReceiver receiver(300);
  drain_motion(j,batch,receiver,navigation,holdout,false);
  assert(receiver.calls==256 && batch.empty() && !j.failed);
}
// Synthetic receive clock: record k (seq k+1) was received by the producer
// at base+recv_ms[k] and read by this worker at base+read_ms[k], as around a
// worker stall. Only the channel check sees this clock.
struct LateReceiver {
  unsigned calls;
  std::vector<unsigned> recv_ms,read_ms;
  uint64_t base;
  N::MotionCursor cursor;
  LateReceiver(const std::vector<unsigned>& r,const std::vector<unsigned>& c):
    calls(0),recv_ms(r),read_ms(c),base(clock_ns(0)) {}
  N::ReceiveResult receive(N::RawEvent* out,N::ReceiveDiagnostic* d) {
    if(calls==recv_ms.size())return N::CHANNEL_EMPTY;
    N::RawEvent e=N::RawEvent();e.kind=N::WHEELS;e.epoch=1;
    e.received_ns=base+recv_ms[calls]*1000000ULL;
    const uint64_t now=base+read_ms[calls]*1000000ULL;
    ++calls;e.receive_seq=calls;
    for(unsigned i=0;i<4;++i)e.raw[i]=10000;
    unsigned char bytes[N::MOTION_RECORD_SIZE];assert(N::encode_motion(e,bytes));
    const N::MotionDatagram packet={bytes,sizeof bytes,false,true,42,0};
    return N::inspect_motion_datagram(packet,0,now,cursor,out,d);
  }
};
// 2026-10-06 late-arrival tolerance through the real drain_motion: a worker
// stall of <= 2 s no longer resets the MODEL pipeline; > 2 s still does, but
// the following fresh record is no longer a sequence_discontinuity as well.
static void late_turn_tests(const char* root,const std::string& logs) {
  config.max_log_bytes=65536;
  const mx5_dr_context context={1,1,1};
  const uint64_t MS=1000000ULL;
  for(unsigned scenario=0;scenario<2;++scenario) {
    arm_test_mode();motion_late=MotionLate();
    N::Pipeline navigation;N::GpsHoldout holdout;
    assert(navigation.init_model(N::research_model_profile(),mx5_dr_default_config(),context));
    assert(holdout.init_model(N::research_model_profile(),mx5_dr_default_config(),context));
    // seq 1 read on time; seq 2..6 received every 20 ms, then read together
    // after the stall (scenario 0: ages 1500..1420 ms; scenario 1: ages
    // 2040, 2020, 2000, 1980, 1960 ms); seq 7..9 fresh after the stall.
    const unsigned stall_end=scenario==0?1520:2060;
    std::vector<unsigned> recv,read;recv.push_back(0);read.push_back(0);
    for(unsigned k=1;k<=5;++k) { recv.push_back(20*k);read.push_back(stall_end); }
    for(unsigned k=0;k<3;++k) { recv.push_back(stall_end+20*k);read.push_back(stall_end+20*k); }
    unlink((logs+"/trace.0.jsonl").c_str()); // own rows only (small-boot reuse)
    {
      Journal j(root);mx5::runtime::MotionBatch batch;LateReceiver receiver(recv,read);
      drain_motion(j,batch,receiver,navigation,holdout,true);j.flush();
      assert(!j.failed && batch.empty() && receiver.calls==9);
      if(scenario==0) {
        // All 9 accepted, no reset, no rejection.
        assert(navigation.status().events==9 && navigation.status().resets==0);
        assert(navigation.context().generation==1);
        assert(motion_late.accepted==5 && motion_late.bursts==1 && motion_late.max_ns==1500*MS);
      } else {
        // 2040 and 2020 ms: stale (two resets); 2000 (inclusive), 1980 and
        // 1960 ms: late again, because the stale records advanced the cursor.
        assert(navigation.context().generation==3);
        assert(motion_late.accepted==3 && motion_late.bursts==1 && motion_late.max_ns==2000*MS);
      }
    }
    const std::string saved=storage_read(logs+"/trace.0.jsonl");
    std::istringstream rows(saved);std::string line;
    unsigned late_rows=0,stale=0,discontinuity=0,resets=0;
    size_t last_batch=0,late_at=0,at=0;
    while(std::getline(rows,line)) {
      ++at;
      if(line.find("\"kind\":\"motion_batch\"")!=std::string::npos)last_batch=at;
      if(line.find("\"kind\":\"motion_late_accepted\"")!=std::string::npos) {
        ++late_rows;late_at=at;
        assert(line.find(scenario==0?"\"first_seq\":2,\"last_seq\":6,\"events\":5":
                                     "\"first_seq\":4,\"last_seq\":6,\"events\":3")!=std::string::npos);
        assert(line.find("\"fresh_limit_ms\":250,\"late_limit_ms\":2000")!=std::string::npos);
      }
      if(line.find("\"reason\":\"stale\"")!=std::string::npos &&
         line.find("motion_rejected")!=std::string::npos)++stale;
      if(line.find("sequence_discontinuity")!=std::string::npos)++discontinuity;
      if(line.find("shadow_input_reset")!=std::string::npos)++resets;
    }
    assert(late_rows==1 && late_at>0 && last_batch>0 && discontinuity==0);
    assert(stale==(scenario==0?0u:2u) && resets==stale);
  }
  motion_late=MotionLate();
  puts("Late arrival: <= 2 s backlog accepted without reset and journaled; > 2 s stays stale "
       "without a following sequence discontinuity");
}
// ---- Journal writer thread (2026-10-06) ----
static std::vector<std::string> trace_rows(const std::string& logs) {
  std::vector<std::string> rows;
  for(int i=2;i>=0;--i) {
    std::ifstream f((logs+"/trace."+char('0'+i)+".jsonl").c_str());std::string line;
    while(std::getline(f,line))rows.push_back(line);
  }
  return rows;
}
static void clear_traces(const std::string& logs) {
  for(unsigned i=0;i<3;++i)unlink((logs+"/trace."+char('0'+i)+".jsonl").c_str());
}
static uint64_t row_number(const std::string& row,const char* key) {
  const std::string k=std::string("\"")+key+"\":";
  const size_t at=row.find(k);
  return at==std::string::npos?UINT64_MAX:strtoull(row.c_str()+at+k.size(),0,10);
}
struct AgeReceiver {
  N::MotionReceiver* receiver;
  uint64_t max_age;unsigned events,late,faults;
  N::ReceiveResult receive(N::RawEvent* out,N::ReceiveDiagnostic* d) {
    const N::ReceiveResult r=receiver->receive(out,d);
    if(r==N::CHANNEL_EVENT) { ++events;if(d->age_ns>max_age)max_age=d->age_ns;if(d->late)++late; }
    else if(r==N::CHANNEL_FAULT)++faults;
    return r;
  }
};
struct SenderJob { char name[80];unsigned count; };
static void* motion_sender(void* argument) {
  SenderJob* job=static_cast<SenderJob*>(argument);
  N::MotionSender sender;assert(sender.open_channel(job->name));
  for(unsigned i=1;i<=job->count;++i) {
    N::RawEvent e=N::RawEvent();e.kind=N::WHEELS;e.epoch=7;e.receive_seq=i;e.received_ns=clock_ns(0);
    for(unsigned k=0;k<4;++k)e.raw[k]=13600;
    assert(sender.send_event(e));
    usleep(20000);
  }
  return 0;
}
// A writer stalled for 1 s (injected sleep before its next row) must not
// delay the worker's receive turns or make queued motion stale.
static void writer_stall_isolation(const char* root,const std::string& logs) {
  arm_test_mode();clear_traces(logs);config.max_log_bytes=8388608;motion_late=MotionLate();
  SenderJob job;snprintf(job.name,sizeof job.name,"mx5dr.writer.%ld",(long)getpid());job.count=80;
  N::MotionReceiver receiver;
  if(!receiver.open_channel(job.name)) {
    puts("Writer stall isolation: SKIP (motion socket unavailable on this host)");return;
  }
  N::Pipeline navigation;N::GpsHoldout holdout;const mx5_dr_context context={1,1,1};
  assert(navigation.init_model(N::research_model_profile(),mx5_dr_default_config(),context));
  assert(holdout.init_model(N::research_model_profile(),mx5_dr_default_config(),context));
  AgeReceiver ages={&receiver,0,0,0,0};
  uint64_t max_turn=0,writer_progress_mid=UINT64_MAX,stall_begin=0;
  unsigned turns=0;
  {
    Journal j(root);assert(j.start_writer());
    j.line("{\"kind\":\"fixture\",\"n\":0}");
    assert(j.flush_wait());
    j.writer->inject_stall_ns.store(1000000000ULL);
    pthread_t thread;assert(pthread_create(&thread,0,motion_sender,&job)==0);
    mx5::runtime::MotionBatch batch;
    const uint64_t begin=clock_ns(0);stall_begin=begin;
    uint64_t last_flush=begin;
    while(clock_ns(0)-begin<1800000000ULL) {
      const uint64_t turn=clock_ns(0);
      drain_motion(j,batch,ages,navigation,holdout,true);
      char row[96];snprintf(row,sizeof row,"{\"kind\":\"fixture\",\"n\":%u}",++turns);
      j.line(row);
      if(turn-last_flush>=1000000000ULL) { last_flush=turn;j.flush(); }
      const uint64_t spent=clock_ns(0)-turn;
      if(spent>max_turn)max_turn=spent;
      if(writer_progress_mid==UINT64_MAX && clock_ns(0)-begin>=500000000ULL)
        writer_progress_mid=j.writer->written.load();
      receiver.wait_for_input(50);
    }
    assert(pthread_join(thread,0)==0);
    drain_motion(j,batch,ages,navigation,holdout,true);
    assert(!j.failed);
  } // joins the writer: everything queued is written
  const uint64_t elapsed=clock_ns(0)-stall_begin;
  printf("Writer stall isolation: injected writer stall 1000 ms; worker turns=%u max_turn_ms=%.2f "
         "motion events=%u max_receive_age_ms=%.2f late=%u rejected=%u writer_rows_at_500ms=%llu elapsed_ms=%llu\n",
         turns,max_turn/1e6,ages.events,ages.max_age/1e6,ages.late,ages.faults,
         (unsigned long long)writer_progress_mid,(unsigned long long)(elapsed/1000000ULL));
  // The writer really was stalled (it wrote at most the row it held)...
  assert(writer_progress_mid<=2);
  // ...but reception and the worker turns were not: no stale, no late, and
  // no turn anywhere near the stall (bound generous for a shared host).
  assert(ages.events==job.count && ages.faults==0 && ages.late==0);
  assert(ages.max_age<200000000ULL && max_turn<200000000ULL);
  const std::vector<std::string> rows=trace_rows(logs);
  unsigned motion=0,fixtures=0;uint64_t last_n=0;
  for(size_t i=0;i<rows.size();++i) {
    assert(rows[i].find("journal_dropped")==std::string::npos);
    if(rows[i].find("\"kind\":\"fixture\"")!=std::string::npos) {
      const uint64_t n=row_number(rows[i],"n");assert(!fixtures || n==last_n+1);last_n=n;++fixtures;
    }
    if(rows[i].find("\"kind\":\"motion_batch\"")!=std::string::npos) {
      size_t at=0;while((at=rows[i].find("[1,",at))!=std::string::npos) { ++motion;at+=3; }
    }
  }
  assert(fixtures==turns+1 && motion==job.count);
  clear_traces(logs);
}
// Overflow: the oldest diagnostic rows are dropped with one counter row at
// the place of the loss; evidence rows and the order of every kept row stay.
static void writer_overflow(const char* root,const std::string& logs) {
  arm_test_mode();clear_traces(logs);config.max_log_bytes=65536;
  {
    Journal j(root);assert(j.start_writer(2048,1024));
    j.line("{\"kind\":\"fixture\",\"n\":0}");assert(j.flush_wait());
    j.writer->inject_stall_ns.store(300000000ULL);
    j.line("{\"kind\":\"fixture\",\"n\":1}");      // held by the stalled writer
    usleep(20000);
    char row[200];
    for(unsigned n=2;n<62;++n) {
      if(n%20==0)snprintf(row,sizeof row,"{\"kind\":\"beta_hold\",\"n\":%u,\"domain\":\"beta\"}",n);
      else snprintf(row,sizeof row,"{\"kind\":\"fixture\",\"n\":%u,\"pad\":\"%040u\"}",n,n);
      j.line(row);
    }
    assert(!j.failed && journal_ok.load()==1 && A::mode()==A::SCRUB_STALE);
  }
  const std::vector<std::string> rows=trace_rows(logs);
  uint64_t last=0;unsigned evidence=0,dropped_rows=0,kept=0,gap_rows=0;bool first=true;
  for(size_t i=0;i<rows.size();++i) {
    if(rows[i].find("journal_dropped")!=std::string::npos) {
      ++gap_rows;dropped_rows+=unsigned(row_number(rows[i],"rows"));
      // The total at this loss, not the ring's final total.
      assert(row_number(rows[i],"dropped_total")==dropped_rows);
      assert(rows[i].find("\"reason\":\"writer_backlog\"")!=std::string::npos);
      continue;
    }
    const uint64_t n=row_number(rows[i],"n");
    assert(first || n>last);first=false;last=n;++kept;   // push order kept
    if(rows[i].find("beta_hold")!=std::string::npos)++evidence;
  }
  printf("Writer overflow: kept=%u dropped=%u counter_rows=%u evidence=%u\n",kept,dropped_rows,gap_rows,evidence);
  assert(evidence==3 && gap_rows>=1 && dropped_rows>0 && kept+dropped_rows==62);
  clear_traces(logs);
}
// An evidence row that cannot be queued, or a failing writer, fails closed:
// journal_ok drops (read by the OEM provenance callback) and mutation stops.
static void writer_fail_closed(const char* root,const std::string& logs) {
  arm_test_mode();clear_traces(logs);config.max_log_bytes=65536;
  // BETA provenance as the adapter's POSITION hook reads it.
  A::PositionInput input=A::PositionInput();
  mx5::runtime::request_trace::Trace trace=mx5::runtime::request_trace::Trace();
  const A::PositionContext context={input,mx5::runtime::request_trace::Result(),trace,1,1,0};
  beta_shared.active.store(1);beta_shared.source_epoch.store(1);beta_shared.storage_epoch.store(1);
  A::Provenance out;
  assert(provenance(0,context,&out,0) && out.domain==A::Provenance::Domain::BETA);
  {
    Journal j(root);assert(j.start_writer(2048,512));
    j.line("{\"kind\":\"fixture\",\"n\":0}");assert(j.flush_wait());
    j.writer->inject_stall_ns.store(300000000ULL);
    j.line("{\"kind\":\"fixture\",\"n\":1}");usleep(20000);
    char row[300];unsigned n=2;
    while(!j.failed && n<100) {
      snprintf(row,sizeof row,"{\"kind\":\"beta_state\",\"n\":%u,\"pad\":\"%0100u\"}",n,n);++n;
      j.line(row);
    }
    assert(j.failed && n<100 && journal_ok.load()==0 && A::mode()==A::OBSERVE);
    assert(!provenance(0,context,&out,0) && out.domain!=A::Provenance::Domain::BETA);
  }
  arm_test_mode();
  // The writer's own file failure (no logs directory) reaches the worker.
  {
    char missing[]="/tmp/mx5dr-writer-missing-XXXXXX";assert(mkdtemp(missing));
    Journal j(missing);assert(j.start_writer());
    j.line("{\"kind\":\"fixture\"}");
    assert(!j.flush_wait() && j.failed && journal_ok.load()==0 && A::mode()==A::OBSERVE);
    assert(!provenance(0,context,&out,0));
    assert(!rmdir(missing));
  }
  beta_shared.active.store(0);beta_shared.source_epoch.store(0);beta_shared.storage_epoch.store(0);
  arm_test_mode();clear_traces(logs);
  puts("Writer fail-closed: evidence overflow and writer file failure disable mutation and BETA provenance");
}
// The writer sleeps on its condition variable (no 5 ms polling); the health
// row reports the backlog it sees while the writer is stalled.
static void writer_idle_and_health(const char* root,const std::string& logs) {
  arm_test_mode();clear_traces(logs);config.max_log_bytes=65536;
  {
    Journal j(root);assert(j.start_writer());
    j.line("{\"kind\":\"fixture\",\"n\":0}");assert(j.flush_wait());
    const uint64_t before=j.writer->loops.load();
    usleep(1000000);
    const uint64_t idle=j.writer->loops.load()-before;
    j.writer->inject_stall_ns.store(600000000ULL);
    j.line("{\"kind\":\"fixture\",\"n\":1}");
    for(unsigned n=2;n<12;++n) { char row[64];snprintf(row,sizeof row,"{\"kind\":\"fixture\",\"n\":%u}",n);j.line(row); }
    usleep(300000);
    journal_health(j,clock_ns(0),true,false);
    const JournalLag lag=journal_writer_lag(j.writer,clock_ns(0));
    printf("Writer idle: %llu wakeups in 1 s; stalled lag %llu ms, %llu unwritten rows\n",
           (unsigned long long)idle,(unsigned long long)(lag.oldest_ns/1000000ULL),
           (unsigned long long)lag.unwritten_rows);
    assert(idle<=3);
    assert(lag.thread && lag.unwritten_rows>=11 && lag.oldest_ns>=250000000ULL);
  }
  const std::vector<std::string> rows=trace_rows(logs);
  bool health=false;
  for(size_t i=0;i<rows.size();++i)
    if(rows[i].find("{\"kind\":\"health\"")==0) {
      health=true;
      assert(rows[i].find("\"journal\":{\"writer\":\"thread\",\"unwritten_rows\":")!=std::string::npos);
      assert(row_number(rows[i],"unwritten_rows")>=11 && row_number(rows[i],"oldest_unwritten_ms")>=250);
    }
  assert(health);
  clear_traces(logs);
  puts("Writer idle wakeups and health backlog fields passed");
}
// Storage stall with a healthy worker (F3-A): BETA provenance is withheld
// within the 1.5 s lag bound, a beta_journal_lag row records it, and both
// recover (hysteresis 0.5 s) once the writer catches up. A boot flush that
// times out keeps BETA/SCRUB off with journal_not_durable, not a failure.
static void journal_lag_bound(const char* root,const std::string& logs) {
  arm_test_mode();clear_traces(logs);config.max_log_bytes=65536;
  A::PositionInput input=A::PositionInput();
  mx5::runtime::request_trace::Trace trace=mx5::runtime::request_trace::Trace();
  const A::PositionContext context={input,mx5::runtime::request_trace::Result(),trace,1,1,0};
  beta_shared.active.store(1);beta_shared.source_epoch.store(1);beta_shared.storage_epoch.store(1);
  A::Provenance out;
  uint64_t lowered_after=0,raised_after=0,stall_end=0,caught_up=0;
  {
    Journal j(root);assert(j.start_writer());
    j.line("{\"kind\":\"fixture\",\"n\":0}");assert(j.flush_wait());
    // A boot-time flush that does not complete in time is not a failure.
    j.writer->inject_stall_ns.store(2500000000ULL);
    j.line("{\"kind\":\"fixture\",\"n\":1}");
    const uint64_t begin=clock_ns(0);
    assert(!j.flush_wait(200000000ULL) && !j.failed && journal_ok.load()==1);
    assert(!strcmp(beta_block_reason(true,j.failed,false,false,true,true,true),"journal_not_durable"));
    assert(!strcmp(beta_block_reason(true,true,false,false,true,true,true),"journal_failed"));
    assert(beta_block_reason(true,false,true,false,true,true,true)==0);
    const uint64_t generation=A::generation();
    // Worker turns every 20 ms while the writer is stalled for 2.5 s; a
    // flush request every 1 s as the worker loop does (rows in the stdio
    // buffer count as lag since 2026-10-07).
    unsigned n=2;uint64_t last_flush=begin;
    while(clock_ns(0)-begin<8000000000ULL && !(raised_after && clock_ns(0)-begin>raised_after+200000000ULL)) {
      const uint64_t now=clock_ns(0);
      char row[64];snprintf(row,sizeof row,"{\"kind\":\"fixture\",\"n\":%u}",n++);j.line(row);
      if(now-last_flush>=1000000000ULL) { last_flush=now;j.flush(); }
      // When the writer has caught up (oldest unwritten row < 0.5 s). Under
      // emulation the backlog after the stall takes longer to write.
      if(lowered_after && !caught_up && journal_writer_lag(j.writer,now).oldest_ns<JOURNAL_LAG_CLEAR_NS)
        caught_up=now-begin;
      journal_lag_guard(j,now);
      const bool beta=provenance(0,context,&out,0);
      if(!lowered_after && !journal_current.load()) { lowered_after=now-begin;assert(!beta && A::generation()!=generation); }
      if(lowered_after && !raised_after && journal_current.load()) { raised_after=now-begin;assert(beta); }
      if(!stall_end && j.writer->written.load()>2)stall_end=now-begin;
      usleep(20000);
    }
    assert(!j.failed);
  }
  printf("Journal lag bound: writer stalled 2500 ms; provenance withheld after %llu ms, "
         "writer resumed at %llu ms, caught up at %llu ms, restored after %llu ms\n",
         (unsigned long long)(lowered_after/1000000ULL),(unsigned long long)(stall_end/1000000ULL),
         (unsigned long long)(caught_up/1000000ULL),(unsigned long long)(raised_after/1000000ULL));
  fflush(stdout);
  assert(lowered_after>=1400000000ULL && lowered_after<=1700000000ULL);
  // Restored only after the stall ended, in the turn the backlog was caught up.
  assert(raised_after && raised_after>=stall_end && caught_up && raised_after>=caught_up &&
         raised_after-caught_up<=60000000ULL);
  const std::vector<std::string> rows=trace_rows(logs);
  unsigned lagging=0,current=0;
  for(size_t i=0;i<rows.size();++i) {
    if(rows[i].find("{\"kind\":\"beta_journal_lag\"")!=0)continue;
    if(rows[i].find("\"event\":\"lagging\"")!=std::string::npos) { ++lagging;assert(!current); }
    if(rows[i].find("\"event\":\"current\"")!=std::string::npos)++current;
  }
  assert(lagging==1 && current==1);
  beta_shared.active.store(0);beta_shared.source_epoch.store(0);beta_shared.storage_epoch.store(0);
  arm_test_mode();clear_traces(logs);
  puts("Journal lag bound: provenance withheld within 1.5 s, restored after catch-up; boot timeout is journal_not_durable");
}

// 2026-10-07 re-review follow-ups of the writer thread.
// (1) Rows handed to stdio but not yet fflush'ed are part of the lag.
// (2) A flush or stop request that lands between the writer's own check and
//     its wait() is not slept through (injected gap before wait()).
static void writer_lag_and_wakeups(const char* root,const std::string& logs) {
  arm_test_mode();clear_traces(logs);config.max_log_bytes=65536;
  uint64_t unflushed_ms=0,flushed_ms=0,flush_latency=0,stop_latency=0;
  {
    Journal j(root);assert(j.start_writer());
    // Request-driven flushes only here (the age-triggered flush would hide
    // both effects; it has its own test below).
    j.writer->flush_max_age_ns.store(0);
    j.line("{\"kind\":\"fixture\",\"n\":0}");assert(j.flush_wait());
    for(unsigned n=1;n<=5;++n) { char row[64];snprintf(row,sizeof row,"{\"kind\":\"fixture\",\"n\":%u}",n);j.line(row); }
    const uint64_t begin=clock_ns(0);
    while(j.writer->written.load()<6) { assert(clock_ns(0)-begin<2000000000ULL);usleep(1000); }
    usleep(300000);   // written to stdio, no flush requested
    JournalLag lag=journal_writer_lag(j.writer,clock_ns(0));
    unflushed_ms=lag.oldest_ns/1000000ULL;
    assert(lag.unwritten_rows==0 && lag.oldest_ns>=250000000ULL);
    assert(j.flush_wait());
    lag=journal_writer_lag(j.writer,clock_ns(0));
    flushed_ms=lag.oldest_ns/1000000ULL;
    assert(lag.oldest_ns<50000000ULL);
    // Flush request inside the gap between the flush check and wait().
    j.writer->inject_wait_gap_ns.store(300000000ULL);
    j.line("{\"kind\":\"fixture\",\"n\":6}");
    uint64_t t0=clock_ns(0);
    while(j.writer->written.load()<7) { assert(clock_ns(0)-t0<2000000000ULL);usleep(1000); }
    usleep(100000);   // the writer sits in the injected gap now
    assert(j.writer->inject_wait_gap_ns.load()==0);
    t0=clock_ns(0);
    j.flush();
    while(j.writer->flushed.load()<7) { assert(clock_ns(0)-t0<3000000000ULL);usleep(1000); }
    flush_latency=clock_ns(0)-t0;
    // Stop inside the same gap.
    j.writer->inject_wait_gap_ns.store(300000000ULL);
    j.line("{\"kind\":\"fixture\",\"n\":7}");
    t0=clock_ns(0);
    while(j.writer->written.load()<8) { assert(clock_ns(0)-t0<2000000000ULL);usleep(1000); }
    usleep(100000);
    t0=clock_ns(0);
    bool ok=false;stop_journal_writer(j.writer,true,&ok);j.writer=0;
    stop_latency=clock_ns(0)-t0;
    assert(ok);
  }
  printf("Writer lag/wakeups: unflushed rows lag %llu ms (after flush %llu ms); flush request in the wait gap "
         "served after %.1f ms, stop after %.1f ms (gap remainder about 200 ms; a lost wake-up costs 1000 ms)\n",
         (unsigned long long)unflushed_ms,(unsigned long long)flushed_ms,flush_latency/1e6,stop_latency/1e6);
  assert(flush_latency<700000000ULL && stop_latency<700000000ULL);
  const std::vector<std::string> rows=trace_rows(logs);
  assert(rows.size()==8);
  for(unsigned n=0;n<8;++n)assert(row_number(rows[n],"n")==n);
  clear_traces(logs);
  puts("Writer: stdio-buffered rows count as lag; flush/stop requests in the wait gap are not lost");
}

// Age-triggered fflush (2026-10-07 follow-up): rows in the stdio buffer are
// flushed once the oldest is JOURNAL_FLUSH_MAX_AGE_NS old, so the steady-state
// lag stays near 250 ms. A single 400 ms fflush must not withdraw BETA; a
// 1.6 s writer stall must (within the unchanged 1.5 s bound).
static void writer_age_flush(const char* root,const std::string& logs) {
  arm_test_mode();clear_traces(logs);config.max_log_bytes=8388608;
  A::PositionInput input=A::PositionInput();
  mx5::runtime::request_trace::Trace trace=mx5::runtime::request_trace::Trace();
  const A::PositionContext context={input,mx5::runtime::request_trace::Result(),trace,1,1,0};
  beta_shared.active.store(1);beta_shared.source_epoch.store(1);beta_shared.storage_epoch.store(1);
  A::Provenance out;
  uint64_t first_flush_ms=0,steady_max=0,slow_max=0,lowered_after=0,raised_after=0;
  unsigned steady_samples=0,lowered_in_steady=0;
  {
    Journal j(root);assert(j.start_writer());
    j.line("{\"kind\":\"fixture\",\"n\":0}");assert(j.flush_wait());
    // (a) No request at all: the writer flushes by itself after ~250 ms.
    const uint64_t flushes=j.writer->flush_count.load();
    uint64_t t0=clock_ns(0);
    j.line("{\"kind\":\"fixture\",\"n\":1}");
    const uint64_t row=j.writer->ring.stats().next_seq;
    while(j.writer->flush_count.load()==flushes) { assert(clock_ns(0)-t0<2000000000ULL);usleep(1000); }
    first_flush_ms=(clock_ns(0)-t0)/1000000ULL;
    assert(first_flush_ms>=240 && first_flush_ms<600);
    // flush_count counts the writer's fflush when it starts (before fflush
    // and ring.flushed()); a running fflush is lag by design (busy_since).
    // Check the bound after that same self-flush has completed, which the
    // writer publishes in `flushed` (2026-10-10: under QEMU the 1 ms poll
    // could sample inside the fflush and read the 250 ms row age).
    while(j.writer->flushed.load()<row) { assert(clock_ns(0)-t0<2000000000ULL);usleep(1000); }
    assert(j.writer->flush_count.load()==flushes+1);   // no request: the same self-flush
    assert(journal_writer_lag(j.writer,clock_ns(0)).oldest_ns<50000000ULL);
    // (b) Steady state: a row every 20 ms, guard every turn, worker flush
    // request every 1 s; (c) one 400 ms fflush in the middle.
    unsigned n=2;uint64_t last_flush=clock_ns(0);const uint64_t begin=last_flush;bool slow_armed=false;
    while(clock_ns(0)-begin<6000000000ULL) {
      const uint64_t now=clock_ns(0);
      char row[64];snprintf(row,sizeof row,"{\"kind\":\"fixture\",\"n\":%u}",n++);j.line(row);
      if(now-last_flush>=1000000000ULL) { last_flush=now;j.flush(); }
      const uint64_t lag=journal_writer_lag(j.writer,now).oldest_ns;
      journal_lag_guard(j,now);
      if(!journal_current.load())++lowered_in_steady;
      assert(provenance(0,context,&out,0));
      if(now-begin<3000000000ULL) { if(lag>steady_max)steady_max=lag;++steady_samples; }
      else {
        if(!slow_armed) { slow_armed=true;j.writer->inject_flush_stall_ns.store(400000000ULL); }
        if(lag>slow_max)slow_max=lag;
      }
      usleep(20000);
    }
    assert(j.writer->inject_flush_stall_ns.load()==0);   // the slow fflush happened
    // (d) A 1.6 s writer stall: withheld within the 1.5 s bound, restored after.
    j.writer->inject_stall_ns.store(1600000000ULL);
    t0=clock_ns(0);
    while(clock_ns(0)-t0<5000000000ULL && !(raised_after && clock_ns(0)-t0>raised_after+100000000ULL)) {
      const uint64_t now=clock_ns(0);
      char row[64];snprintf(row,sizeof row,"{\"kind\":\"fixture\",\"n\":%u}",n++);j.line(row);
      if(now-last_flush>=1000000000ULL) { last_flush=now;j.flush(); }
      journal_lag_guard(j,now);
      if(!lowered_after && !journal_current.load())lowered_after=now-t0;
      if(lowered_after && !raised_after && journal_current.load())raised_after=now-t0;
      usleep(20000);
    }
    assert(!j.failed);
  }
  printf("Writer age flush: first self-flush after %llu ms; steady lag max %.1f ms over %u turns; "
         "with one 400 ms fflush max %.1f ms (journal_current never lowered: %u); 1.6 s stall lowered after "
         "%llu ms, restored after %llu ms\n",
         (unsigned long long)first_flush_ms,steady_max/1e6,steady_samples,slow_max/1e6,lowered_in_steady,
         (unsigned long long)(lowered_after/1000000ULL),(unsigned long long)(raised_after/1000000ULL));
  fflush(stdout);
  assert(steady_max<450000000ULL && lowered_in_steady==0);
  assert(slow_max>=400000000ULL && slow_max<JOURNAL_LAG_LIMIT_NS);
  // Measured from the stall's first row; rows already waiting in the stdio
  // buffer (at most ~250 ms old) count too, so it can trip up to ~0.25 s
  // earlier than 1.5 s after that row, never later than the bound.
  assert(lowered_after>=1200000000ULL && lowered_after<=1700000000ULL && raised_after>lowered_after);
  beta_shared.active.store(0);beta_shared.source_epoch.store(0);beta_shared.storage_epoch.store(0);
  arm_test_mode();clear_traces(logs);
  puts("Writer age flush: steady lag near 250 ms; a 400 ms fflush keeps BETA; a 1.6 s stall withdraws it");
}
// (3) While the lag guard is engaged (journal_current 0) FIX-class POSITION
// rows are diagnostic: a long stall drops old ones instead of filling the
// evidence ring and failing the journal. LOST rows stay evidence.
static void writer_lagging_fix_rows(const char* root,const std::string& logs) {
  A::Observation fix=A::Observation();fix.kind=A::Observation::POSITION;fix.position_class=A::POSITION_FIX;
  A::Observation lost=fix;lost.position_class=A::POSITION_LOST;
  char row[300];
  for(unsigned c=0;c<3;++c) {
    // c 0: FIX while lagging (diagnostic); 1: FIX while current (evidence);
    // 2: LOST while lagging (evidence).
    arm_test_mode();clear_traces(logs);config.max_log_bytes=65536;
    journal_current.store(c==1?1:0);
    unsigned n=2;bool failed=false;
    {
      Journal j(root);assert(j.start_writer(2048,512));
      j.line("{\"kind\":\"fixture\",\"n\":0}");assert(j.flush_wait());
      j.writer->inject_stall_ns.store(300000000ULL);
      j.line("{\"kind\":\"fixture\",\"n\":1}");usleep(20000);
      while(!j.failed && n<100) {
        snprintf(row,sizeof row,"{\"kind\":\"position\",\"n\":%u,\"pad\":\"%0100u\"}",n,n);++n;
        j.observation_line(row,c==2?lost:fix);
      }
      failed=j.failed;
    }
    journal_current.store(1);
    const std::vector<std::string> rows=trace_rows(logs);
    unsigned dropped=0;
    for(size_t i=0;i<rows.size();++i)if(rows[i].find("journal_dropped")!=std::string::npos)++dropped;
    printf("Writer lagging FIX rows: case %u rows=%u failed=%d dropped_counters=%u\n",c,n-2,int(failed),dropped);
    if(c==0) assert(!failed && n==100 && dropped>=1 && journal_ok.load()==1);
    else assert(failed && n<100 && journal_ok.load()==0 && A::mode()==A::OBSERVE);
    clear_traces(logs);
  }
  arm_test_mode();
  puts("Writer: FIX POSITION rows are diagnostic while the lag guard is engaged; LOST and current FIX stay evidence");
}
// Every queued row is written on worker exit; a requested stop closes the
// file durably before its acknowledgement, in order.
static void writer_shutdown(const char* root,const std::string& logs) {
  arm_test_mode();clear_traces(logs);config.max_log_bytes=65536;
  {
    Journal j(root);assert(j.start_writer());
    char row[64];
    for(unsigned n=0;n<1000;++n) { snprintf(row,sizeof row,"{\"kind\":\"fixture\",\"n\":%u}",n);j.line(row); }
  }
  std::vector<std::string> rows=trace_rows(logs);
  assert(rows.size()==1000);
  for(unsigned n=0;n<1000;++n)assert(row_number(rows[n],"n")==n);
  clear_traces(logs);
  const char* boot="12345678-1234-1234-1234-123456789abc";
  arm_test_mode();freeze_capture();assert(queue.drained());
  {
    Journal j(root);assert(j.start_writer());
    j.line("{\"kind\":\"fixture\",\"mono_ns\":9}");
    assert(finish_capture(j,boot,10,11) && !j.writer);
  }
  rows=trace_rows(logs);
  assert(rows.size()==3 && rows[0].find("fixture")!=std::string::npos &&
         rows[1].find("capture_end")!=std::string::npos && rows[2].find("\"kind\":\"health\"")!=std::string::npos);
  std::ifstream a((logs+"/capture.done").c_str());std::string value;std::getline(a,value);assert(value==boot);
  unlink((logs+"/capture.done").c_str());clear_traces(logs);
  puts("Writer shutdown: exit drains every queued row; capture stop fsyncs and closes before its acknowledgement");
}
// Pipeline::CAPACITY (128 queued events) within one receive turn: the 129th
// undrained event is PIPELINE_OVERFLOW, a MODEL reset with its own row; raw
// capture keeps every record and later events enter the reset pipeline.
// (On the CMU the motion socket queue holds only 10 datagrams, so a worker
// stall loses datagrams long before a backlog could reach this bound.)
static void pipeline_capacity_overflow(const char* root,const std::string& logs) {
  arm_test_mode();config.max_log_bytes=65536;
  unlink((logs+"/trace.0.jsonl").c_str()); // own rows only (small-boot reuse)
  N::Pipeline navigation;N::GpsHoldout holdout;const mx5_dr_context context={1,1,1};
  assert(navigation.init_model(N::research_model_profile(),mx5_dr_default_config(),context));
  assert(holdout.init_model(N::research_model_profile(),mx5_dr_default_config(),context));
  {
    Journal j(root);mx5::runtime::MotionBatch batch;FakeReceiver receiver(140);
    drain_motion(j,batch,receiver,navigation,holdout,true);j.flush();
    assert(!j.failed && receiver.calls==140 && !audit_fault);
    assert(navigation.status().resets==1 && navigation.status().events==128+11);
  }
  const std::string saved=storage_read(logs+"/trace.0.jsonl");
  assert(saved.find("\"reason\":\"OVERFLOW\",\"operation\":\"raw\"")!=std::string::npos);
  assert(saved.find("\"receive_seq\":129,")!=std::string::npos);
  size_t events=0,at=0;
  while((at=saved.find("[1,",at))!=std::string::npos) { ++events;at+=3; }
  assert(events==140);
  puts("Pipeline capacity: the 129th undrained event resets MODEL with an OVERFLOW row; raw capture keeps all 140");
}
static void pipeline_fault_capture(const char* root,const std::string& logs) {
  arm_test_mode();config.max_log_bytes=65536;
  unlink((logs+"/trace.0.jsonl").c_str()); // own rows only (small-boot reuse)
  N::Pipeline navigation;N::GpsHoldout holdout;const mx5_dr_context context={1,1,1};
  assert(navigation.init_model(N::research_model_profile(),mx5_dr_default_config(),context));
  assert(holdout.init_model(N::research_model_profile(),mx5_dr_default_config(),context));
  {
    Journal j(root);mx5::runtime::MotionBatch batch;FakeReceiver receiver(3,false,true);
    drain_motion(j,batch,receiver,navigation,holdout,true);j.flush();
    // A later good input overwrites the current result. Keep the earlier
    // actual reset reason and its exact raw input, without disabling capture.
    assert(navigation.status().resets==1 && navigation.status().result==N::PIPELINE_OK);
    assert(!j.failed && batch.empty() && receiver.calls==3 && !audit_fault);
  }
  const std::string saved=storage_read(logs+"/trace.0.jsonl");
  assert(saved.find("\"kind\":\"shadow_pipeline_reset\"")!=std::string::npos);
  assert(saved.find("\"reason\":\"BAD_INPUT\"")!=std::string::npos);
  assert(saved.find("\"operation\":\"raw\"")!=std::string::npos);
  assert(saved.find("\"receive_seq\":1")!=std::string::npos);
  assert(saved.find(",50000,50000,50000,50000,")!=std::string::npos);
  assert(saved.find("\"kind\":\"motion_batch\"")!=std::string::npos);
  assert(saved.find(",50000,50000,50000,50000,")<
         saved.find("\"kind\":\"shadow_pipeline_reset\""));
  puts("MODEL reset: reason and raw input survive subsequent successful input");
}
static void stop_tests(const char* root,const std::string& logs) {
  const char* boot="12345678-1234-1234-1234-123456789abc";
  const std::string request=logs+"/capture.stop",ack=logs+"/capture.done";
  assert(!stop_requested(root));
  assert(mkdir(request.c_str(),0700)==0 && stop_requested(root));
  {
    std::ifstream before((logs+"/trace.0.jsonl").c_str());
    const std::string saved((std::istreambuf_iterator<char>(before)),std::istreambuf_iterator<char>());
    arm_test_mode();assert(worker_at(root)==0 && queue.closed());
    std::ifstream after((logs+"/trace.0.jsonl").c_str());
    const std::string retained((std::istreambuf_iterator<char>(after)),std::istreambuf_iterator<char>());
    assert(saved==retained && access(ack.c_str(),F_OK)!=0);
  }
  arm_test_mode();
  A::Observation event=A::Observation();sink(&event,0);
  freeze_capture();sink(&event,0);
  assert(!queue.drained() && !queue.dropped() && A::mode()==A::OBSERVE);
  sink(&event,0);
  assert(!queue.dropped() && !queue.drained());
  assert(pop(&event) && !pop(&event));
  assert(queue.drained());
  unlink((logs+"/trace.0.jsonl").c_str()); // own rows only (small-boot reuse)
  {
    Journal j(root);j.line("{\"kind\":\"fixture\",\"mono_ns\":9}");
    assert(finish_capture(j,boot,10,11));
    std::ifstream a(ack.c_str());std::string value;std::getline(a,value);assert(value==boot);
    std::ifstream f((logs+"/trace.0.jsonl").c_str());std::string line;
    assert(std::getline(f,line) && line.find("fixture")!=std::string::npos);
    assert(std::getline(f,line) && line.find("capture_end")!=std::string::npos);
    assert(std::getline(f,line) && line.find("health")!=std::string::npos);
    assert(line.find("\"capture_active\":false")!=std::string::npos);
    assert(!std::getline(f,line));
  }
  unlink(ack.c_str());
  {
    Journal j(root);assert(!finish_capture(j,"unknown",10,11));
    assert(access(ack.c_str(),F_OK)!=0);
  }
  {
    Journal j(root);j.f=fopen("/dev/null","w");assert(j.f);
    assert(!finish_capture(j,boot,10,11)); // fflush succeeds; fsync fails.
    assert(j.failed && access(ack.c_str(),F_OK)!=0);
  }
  assert(rmdir(request.c_str())==0);
}
static A::Observation long_route_event() {
  A::Observation event=A::Observation();event.kind=A::Observation::SEND;
  event.request_result=A::R::OK;
  A::R::Text text=A::R::Text();text.known=true;
  memset(text.bytes,1,sizeof text.bytes-1);
  A::R::Trace& t=event.request_trace;
  t.issue.route.destination=t.issue.route.path=t.issue.route.interface_name=t.issue.route.member=text;
  t.issue.endpoint.server_guid=t.issue.endpoint.unique_name=text;
  t.reply.sender=t.reply.error_name=text;
  t.issue.wire.serial=1;t.issue.wire.known=true;
  t.reply.wire.serial=2;t.reply.wire.reply_serial=1;t.reply.wire.type=3;t.reply.wire.known=true;
  t.reply.wire.sender=t.reply.wire.error_name=text;
  t.request.id=t.request.epoch=t.worker.id=t.worker.epoch=UINT64_MAX;
  t.issue.observed_ns=t.reply.observed_ns=t.issue.wire.observed_ns=t.reply.wire.observed_ns=UINT64_MAX;
  t.issue.connection={mx5::runtime::bus_trace::CONNECTED,UINT32_MAX,UINT64_MAX};
  t.reply.connection=t.issue.connection;t.issue.bus_lifetime=UINT64_MAX;t.issue.known=A::R::ISSUE_BUS_LIFETIME;
  event.call_sequence=event.prediction_generation=UINT32_MAX;event.mono_ns=UINT64_MAX;
  return event;
}
static void heading_presence_journal() {
  namespace T=mx5::sensors::nmea_course_token;
  const char* labels[]={"unknown","empty","present","unknown"};
  for(unsigned kind=0;kind<2;++kind)for(unsigned value=0;value<4;++value) {
    A::Observation o=long_route_event();
    o.kind=kind?A::Observation::SEND:A::Observation::POSITION;
    o.position.heading_deg=0;
    LA::Owned& a=o.lds_association;a.result=LA::MATCHED_LOCKED_FOR_SEND;
    a.stage=LA::LOCKED_FOR_SEND;a.cache_lifetime=a.write_sequence=UINT64_MAX;
    a.source_instance=a.record_sequence=a.locked_observed_ns=a.map_loss_epoch=UINT64_MAX;
    a.request_id=a.request_epoch=a.worker_id=a.worker_epoch=UINT64_MAX;
    a.call_sequence=a.prediction_generation=a.view_revision=a.layout_version=UINT32_MAX;
    for(unsigned i=0;i<9;++i)a.fields[i]={UINT64_MAX,UINT64_MAX};
    a.heading_presence=T::Presence(value);
    char output[mx5::runtime::OBSERVATION_JSON_CAPACITY];
    assert(format_observation(output,sizeof output,o));
    const std::string expected=std::string("\"heading_presence\":\"")+labels[value]+"\"";
    assert(strstr(output,expected.c_str()));
    // An invalid supplemental enum is unknown; it must not erase the raw row.
    if(!kind)assert(strstr(output,"\"heading\":0"));
    const size_t n=strlen(output);
    char exact[mx5::runtime::OBSERVATION_JSON_CAPACITY+2];memset(exact,0x5a,sizeof exact);
    assert(format_observation(exact+1,n+1,o));assert(exact[0]==0x5a&&exact[n+2]==0x5a);
    memset(exact,0x5a,sizeof exact);
    assert(!format_observation(exact+1,n,o));assert(exact[0]==0x5a&&exact[n+1]==0x5a);
  }
  puts("heading presence journal: POSITION/SEND 8 cases, exact/N-1/canary PASS");
}
static void rmc_status_journal() {
  namespace T=mx5::sensors::nmea_course_token;
  const char* labels[]={"unknown","empty","a","v","other","unknown"};
  for(unsigned kind=0;kind<2;++kind)for(unsigned value=0;value<7;++value) {
    A::Observation o=long_route_event();o.kind=kind?A::Observation::SEND:A::Observation::POSITION;
    LA::Owned& a=o.lds_association;a.result=LA::MATCHED_LOCKED_FOR_SEND;
    a.stage=LA::LOCKED_FOR_SEND;a.cache_lifetime=a.write_sequence=UINT64_MAX;
    a.source_instance=a.record_sequence=a.locked_observed_ns=a.map_loss_epoch=UINT64_MAX;
    a.request_id=a.request_epoch=a.worker_id=a.worker_epoch=UINT64_MAX;
    a.call_sequence=a.prediction_generation=a.view_revision=a.layout_version=UINT32_MAX;
    for(unsigned i=0;i<9;++i)a.fields[i]={UINT64_MAX,UINT64_MAX};
    a.heading_presence=T::PRESENT;a.heading_rmc_status=T::RmcStatus(value);
    if(value==6) {a.heading_rmc_status=T::RMC_A;a.fields[5]={0,0};}
    char output[mx5::runtime::OBSERVATION_JSON_CAPACITY];
    assert(format_observation(output,sizeof output,o));
    const std::string expected=std::string("\"heading_rmc_status\":\"")+(value==6?"unknown":labels[value])+"\"";
    assert(strstr(output,expected.c_str()));
    const size_t n=strlen(output);
    char exact[mx5::runtime::OBSERVATION_JSON_CAPACITY+2];memset(exact,0x5a,sizeof exact);
    assert(format_observation(exact+1,n+1,o));assert(exact[0]==0x5a&&exact[n+2]==0x5a);
    memset(exact,0x5a,sizeof exact);
    assert(!format_observation(exact+1,n,o));assert(exact[0]==0x5a&&exact[n+1]==0x5a);
  }
  puts("RMC status journal: POSITION/SEND 14 cases, exact/N-1/canary PASS");
}
static void route_capture_tail(const char* root,const std::string& logs) {
  arm_test_mode();
  const A::Observation event=long_route_event();
  char expected[mx5::runtime::OBSERVATION_JSON_CAPACITY];
  assert(format_observation(expected,sizeof expected,event) && strlen(expected)>5120);
  sink(&event,0);freeze_capture();
  unlink((logs+"/trace.0.jsonl").c_str()); // own rows only (small-boot reuse)
  {
    Journal j(root);assert(drain_capture_tail(j));j.flush();
    assert(!j.failed && queue.drained());
  }
  std::ifstream f((logs+"/trace.0.jsonl").c_str());std::string line;
  assert(std::getline(f,line) && line==expected);
  assert(!std::getline(f,line));
}
static void* route_worker(void* root) { return worker_at(static_cast<const char*>(root)); }
static void route_general_worker(const char* root,const std::string& logs) {
  assert(!unlink((logs+"/trace.0.jsonl").c_str()) || errno==ENOENT);
  arm_test_mode();config.mode=1;config.max_log_bytes=65536;
  const A::Observation event=long_route_event();
  char expected[mx5::runtime::OBSERVATION_JSON_CAPACITY];
  assert(format_observation(expected,sizeof expected,event) && strlen(expected)>5120);
  sink(&event,0);
  pthread_t thread;assert(!pthread_create(&thread,0,route_worker,const_cast<char*>(root)));
  bool found=false;
  for(unsigned attempt=0;attempt<150&&!found;++attempt) {
    usleep(20000);
    std::ifstream f((logs+"/trace.0.jsonl").c_str());std::string line;
    while(std::getline(f,line))if(line==expected)found=true;
  }
  // Require a persisted full row before requesting stop, so the final-tail
  // buffer cannot conceal a regression in the ordinary worker's buffer.
  assert(found && !__sync_fetch_and_add(&audit_fault,0));
  assert(!mkdir((logs+"/capture.stop").c_str(),0700));
  assert(!pthread_join(thread,0) && queue.drained() && !audit_fault);
  assert(access((logs+"/capture.done").c_str(),F_OK)==0);
  assert(!unlink((logs+"/capture.done").c_str()));
  assert(!rmdir((logs+"/capture.stop").c_str()));
  puts("Long route: ordinary worker and capture tail retain the entire row");
}
// log_profile=persistent through the real worker and writer thread: the
// boot row names the profile, the first POSITION (a class transition) opens
// a raw period behind a raw_window marker, and the capture stop writes the
// final digest before capture_end and its acknowledgement.
// RAW window burst at an event (2026-10-08, first persistent BETA drive):
// about 1100 window rows (400 KiB) written at one GPS loss/return while the
// vehicle writer managed about 350-630 rows/s; the oldest unwritten row
// passed the 1.5 s guard and BETA provenance was withdrawn exactly when BETA
// should engage. Replayed here with the product Journal, writer thread and
// PersistentLog, the writer slowed to 350 rows/s (a sleep before every row),
// worker turns every 20 ms (pump, lag guard, 1 s flush) and evidence rows
// flowing (LOST POSITION, replaced SEND and beta_summary every second, a raw
// batch every 100 ms). paced 0 is the beta.3/beta.4 behaviour (the whole
// window at the event); paced 150 is the product now.
struct BurstResult { uint64_t max_lag_ns; unsigned lowered,evidence_rows,window_rows,unwritten_max; };
static BurstResult window_burst(const char* root,const std::string& logs,unsigned paced,uint64_t stall_ns=0,
                                uint64_t* lowered_after=0,unsigned writer_rows_per_s=350,
                                uint64_t stall_at_ns=9000000000ULL,bool sparse=false,
                                uint64_t* live_emit_max_ns=0) {
  arm_test_mode();clear_traces(logs);config.max_log_bytes=8388608;
  journal_current.store(1);
  BurstResult r=BurstResult();
  static unsigned char storage[mx5::runtime::PersistentLog::WINDOW_BYTES+mx5::runtime::PersistentLog::ROW_BYTES];
  mx5::runtime::PersistentLog quiet;quiet.init(storage,N::research_model_profile());quiet.set_paced(paced);
  {
    Journal j(root);assert(j.start_writer(524288,262144,131072));
    j.filter=&quiet;
    j.line("{\"kind\":\"boot\",\"schema\":1}");assert(j.flush_wait());
    // 1100 raw batches of about 370 bytes: a full window, as on the drive.
    std::string pad(320,'p');
    for(unsigned i=0;i<1100;++i) {
      char head[64];snprintf(head,sizeof head,"{\"kind\":\"motion_batch\",\"n\":%u,\"p\":\"",i);
      j.line((head+pad+"\"}").c_str());
    }
    assert(j.flush_wait());
    j.writer->inject_row_delay_ns.store(1000000000ULL/writer_rows_per_s);
    // The event: GPS lost while BETA is armed.
    j.line("{\"kind\":\"beta_state\",\"mono_ns\":1,\"domain\":\"beta\",\"assist_ready\":false,"
           "\"from\":\"ARMED\",\"to\":\"GPS_LOST\",\"reason\":\"gps_lost\"}");
    A::Observation lost=A::Observation();lost.kind=A::Observation::POSITION;
    lost.position_class=A::POSITION_LOST;lost.original_mode=0;
    A::Observation replaced=A::Observation();replaced.kind=A::Observation::SEND;replaced.type=1;
    replaced.choice=A::BETA_REPLACEMENT;
    const uint64_t begin=clock_ns(0);uint64_t last_flush=begin,last_second=0;unsigned call=0,batch=0;
    bool stalled=false;
    while(clock_ns(0)-begin<12000000000ULL) {
      const uint64_t now=clock_ns(0);
      if(!sparse && now-begin>=uint64_t(batch)*100000000ULL) {
        // A current raw row during the drain (review M1): queued in the
        // journal ring at once, untagged, timed by the lag.
        char b[64];snprintf(b,sizeof b,"{\"kind\":\"motion_batch\",\"live\":%u}",batch++);
        const uint64_t before_seq=j.writer->ring.stats().next_seq;
        j.line(b);
        const mx5::runtime::JournalRing::Stats after_push=j.writer->ring.stats();
        assert(after_push.next_seq>before_seq);
        if(live_emit_max_ns) { const uint64_t d=clock_ns(0)-now;if(d>*live_emit_max_ns)*live_emit_max_ns=d; }
      }
      if(!last_second || now-last_second>=1000000000ULL) {
        last_second=now;++call;char row[200];
        snprintf(row,sizeof row,"{\"kind\":\"position\",\"call\":%u,\"mode\":0,\"class\":3}",call);
        lost.call_sequence=call;j.observation_line(row,lost);
        snprintf(row,sizeof row,"{\"kind\":\"send\",\"call\":%u,\"type\":1,\"choice\":3,\"reason\":0}",call);
        replaced.call_sequence=call;j.observation_line(row,replaced);
        snprintf(row,sizeof row,"{\"kind\":\"beta_summary\",\"mono_ns\":%llu,\"domain\":\"beta\","
                 "\"assist_ready\":false,\"state\":\"ENGAGED\",\"n\":%u}",(unsigned long long)now,call);
        j.line(row);
        r.evidence_rows+=3;
      }
      // A real writer stall in the middle (once), after the window drained.
      if(stall_ns && !stalled && now-begin>=stall_at_ns) { stalled=true;j.writer->inject_stall_ns.store(stall_ns); }
      j.pump(now);
      if(now-last_flush>=1000000000ULL) { last_flush=now;j.flush(); }
      const JournalLag lag=journal_writer_lag(j.writer,now);
      if(lag.oldest_ns>r.max_lag_ns && !(stall_ns && stalled))r.max_lag_ns=lag.oldest_ns;
      if(lag.unwritten_rows>r.unwritten_max)r.unwritten_max=unsigned(lag.unwritten_rows);
      const bool before=journal_current.load()!=0;
      journal_lag_guard(j,now);
      if(before && !journal_current.load()) {
        ++r.lowered;
        if(lowered_after && stalled && !*lowered_after)*lowered_after=now-begin-stall_at_ns;
      }
      usleep(20000);
    }
    // The rest of the window (a writer slower than the drain rate, e.g.
    // under emulation) without further traffic.
    for(unsigned turn=0;turn<6000 && quiet.draining();++turn) { j.pump(clock_ns(0));usleep(20000); }
    assert(!quiet.draining());
    j.writer->inject_row_delay_ns.store(0);
    assert(j.flush_wait() && !j.failed);
    j.filter=0;
  }
  journal_current.store(1);
  const std::vector<std::string> rows=trace_rows(logs);
  unsigned tagged=0,positions=0;
  for(size_t i=0;i<rows.size();++i) {
    if(rows[i].find("{\"kind\":\"motion_batch\",\"raw_window\":true,\"n\":")==0)++tagged;
    if(rows[i].find("\"live\":")!=std::string::npos)assert(rows[i].find("raw_window")==std::string::npos);
    if(rows[i].find("{\"kind\":\"position\",\"call\":")==0)++positions;
  }
  r.window_rows=tagged;
  assert(positions*3==r.evidence_rows);   // every evidence row reached the file
  arm_test_mode();clear_traces(logs);
  return r;
}
static void window_burst_lag(const char* root,const std::string& logs) {
  const BurstResult before=window_burst(root,logs,0);
  printf("Window burst, whole window at the event (beta.4): max lag %.0f ms, %u unwritten rows at most, "
         "journal_current lowered %u times, %u evidence rows, %u window rows\n",
         before.max_lag_ns/1e6,before.unwritten_max,before.lowered,before.evidence_rows,before.window_rows);
  fflush(stdout);
  uint64_t live_emit_max=0;
  const BurstResult after=window_burst(root,logs,mx5::runtime::PersistentLog::DRAIN_ROWS_PER_S,0,0,350,
                                       9000000000ULL,false,&live_emit_max);
  printf("Window burst: current raw rows during the drain queued in the journal ring within %.3f ms\n",
         live_emit_max/1e6);
  printf("Window burst, paced %u rows/s (BULK, untimed): max lag %.0f ms, %u unwritten rows at most, "
         "journal_current lowered %u times, %u evidence rows, %u window rows\n",
         mx5::runtime::PersistentLog::DRAIN_ROWS_PER_S,after.max_lag_ns/1e6,after.unwritten_max,after.lowered,
         after.evidence_rows,after.window_rows);
  fflush(stdout);
  // The defect reproduces with the whole-window burst (the guard is right
  // to trip on it: prompt rows really waited behind 1100 rows)...
  assert(before.lowered>=1 && before.max_lag_ns>JOURNAL_LAG_LIMIT_NS && before.window_rows==1100);
  // ...and the paced drain keeps every prompt row prompt (host: within the
  // 250 ms age flush; at most one pump of window rows ahead of any row).
  assert(!after.lowered && after.max_lag_ns<1000000000ULL && after.window_rows==1100);
  assert(after.evidence_rows>=33);
  // A writer slower than the drain rate (90 rows/s, as under QEMU): the
  // drain follows the writer (no new window rows while earlier ones wait).
  const BurstResult slow=window_burst(root,logs,mx5::runtime::PersistentLog::DRAIN_ROWS_PER_S,0,0,90);
  printf("Window burst, paced, writer at 90 rows/s: max lag %.0f ms, %u unwritten rows at most, "
         "journal_current lowered %u times, %u window rows\n",
         slow.max_lag_ns/1e6,slow.unwritten_max,slow.lowered,slow.window_rows);
  fflush(stdout);
  assert(!slow.lowered && slow.max_lag_ns<1000000000ULL && slow.window_rows==1100);
  // A real writer stall (1.6 s) DURING the drain (review L1), with only
  // 1 Hz prompt rows: the writer is blocked on a BULK row, timed from its
  // pop, so the guard drops within the bound of the stall start, not 1.5 s
  // after the next prompt row.
  uint64_t during_after=0;
  const BurstResult during=window_burst(root,logs,mx5::runtime::PersistentLog::DRAIN_ROWS_PER_S,1600000000ULL,
                                        &during_after,350,1050000000ULL,true);
  printf("Window burst, 1.6 s writer stall during the drain (1 Hz prompt rows): journal_current lowered %u times, "
         "%llu ms after the stall began\n",during.lowered,(unsigned long long)(during_after/1000000ULL));
  fflush(stdout);
  assert(during.lowered==1 && during_after>=1000000000ULL && during_after<=1700000000ULL);
  // A real writer stall (1.6 s) during the same traffic still trips it.
  uint64_t lowered_after=0;
  const BurstResult stall=window_burst(root,logs,mx5::runtime::PersistentLog::DRAIN_ROWS_PER_S,1600000000ULL,
                                       &lowered_after);
  printf("Window burst then a 1.6 s writer stall: journal_current lowered %u times, %llu ms after the stall began\n",
         stall.lowered,(unsigned long long)(lowered_after/1000000ULL));
  fflush(stdout);
  assert(stall.lowered==1 && lowered_after>=1000000000ULL && lowered_after<=1700000000ULL);
  puts("Window burst: paced drain keeps journal_current while evidence flows; a 1.6 s stall still trips the guard");
}
static void persistent_worker(const char* root,const std::string& logs) {
  clear_traces(logs);
  arm_test_mode();config.mode=1;config.max_log_bytes=65536;
  config.log_profile=mx5::runtime::LOG_PROFILE_PERSISTENT;
  A::Observation event=long_route_event();event.kind=A::Observation::POSITION;
  const A::Observation other=long_route_event();   // an ORIGINAL non-LOCATION SEND
  char expected[mx5::runtime::OBSERVATION_JSON_CAPACITY],digested[mx5::runtime::OBSERVATION_JSON_CAPACITY];
  assert(format_observation(expected,sizeof expected,event));
  assert(format_observation(digested,sizeof digested,other));
  sink(&event,0);sink(&other,0);
  pthread_t thread;assert(!pthread_create(&thread,0,route_worker,const_cast<char*>(root)));
  bool found=false;
  for(unsigned attempt=0;attempt<150&&!found;++attempt) {
    usleep(20000);
    std::ifstream f((logs+"/trace.0.jsonl").c_str());std::string line;
    while(std::getline(f,line))if(line==expected)found=true;
  }
  assert(found);
  assert(!mkdir((logs+"/capture.stop").c_str(),0700));
  assert(!pthread_join(thread,0) && queue.drained() && !audit_fault);
  config.log_profile=mx5::runtime::LOG_PROFILE_FULL;
  const std::vector<std::string> rows=trace_rows(logs);
  size_t boot=rows.size(),marker=rows.size(),position=rows.size(),digest=rows.size(),end=rows.size();
  for(size_t i=0;i<rows.size();++i) {
    if(rows[i].find("{\"kind\":\"boot\"")==0)boot=i;
    if(rows[i].find("{\"kind\":\"raw_window\"")==0 && marker==rows.size())marker=i;
    if(rows[i]==expected)position=i;
    assert(rows[i]!=digested);                     // counted in the digest only
    if(rows[i].find("{\"kind\":\"log_digest\"")==0 && rows[i].find("\"digest\":\"final\"")!=std::string::npos)digest=i;
    if(rows[i].find("{\"kind\":\"capture_end\"")==0)end=i;
  }
  assert(boot<rows.size() && rows[boot].find("\"log_profile\":\"persistent\",\"raw_window\":\"available\"")!=std::string::npos);
  assert(rows[boot].find("\"journal_writer\":\"thread\"")!=std::string::npos);
  assert(boot<marker && marker<position && position<digest && digest<end && end<rows.size());
  assert(rows[marker].find("\"trigger\":\"position_class\"")!=std::string::npos);
  assert(rows[digest].find("\"sends\":1,")!=std::string::npos &&
         rows[digest].find("\"send_original\":1")!=std::string::npos);
  assert(access((logs+"/capture.done").c_str(),F_OK)==0);
  assert(!unlink((logs+"/capture.done").c_str()));
  assert(!rmdir((logs+"/capture.stop").c_str()));
  clear_traces(logs);
  puts("Persistent profile worker: boot names the profile; raw window, final digest and stop acknowledgement in order");
}
static void context_loss_worker(const char* root,const std::string& logs) {
  assert(!unlink((logs+"/trace.0.jsonl").c_str()) || errno==ENOENT);
  arm_test_mode();config.mode=1;config.max_log_bytes=65536;
  A::Observation event=A::Observation();event.kind=A::Observation::POSITION;
  event.reason=A::CONTEXT_UNAVAILABLE;event.request_result=A::R::NOT_FOUND;
  event.call_sequence=19;event.prediction_generation=A::generation();
  event.mono_ns=clock_ns(0);event.original_mode=event.position.mode=0;
  sink(&event,0);
  pthread_t thread;assert(!pthread_create(&thread,0,route_worker,const_cast<char*>(root)));
  bool found=false;
  for(unsigned attempt=0;attempt<150&&!found;++attempt) {
    usleep(20000);
    const std::string journal=storage_read(logs+"/trace.0.jsonl");
    found=journal.find("\"reason\":13")!=std::string::npos &&
          journal.find("\"reason\":\"adapter_context_unavailable\"")!=std::string::npos;
  }
  assert(found && audit_fault && A::mode()==A::OBSERVE);
  assert(!mkdir((logs+"/capture.stop").c_str(),0700));
  assert(!pthread_join(thread,0) && queue.drained());
  assert(!unlink((logs+"/capture.done").c_str()));
  assert(!rmdir((logs+"/capture.stop").c_str()));
  puts("Context loss: raw POSITION and failure marker survive, mutation disabled");
}
static void adapter_fault_worker(const char* root,const std::string& logs) {
  assert(!unlink((logs+"/trace.0.jsonl").c_str()) || errno==ENOENT);
  arm_test_mode();config.mode=1;config.max_log_bytes=65536;
  A::position_leave(); // No active scope: sticky fault with no observation row.
  assert(A::faulted());
  pthread_t thread;assert(!pthread_create(&thread,0,route_worker,const_cast<char*>(root)));
  bool found=false;
  for(unsigned attempt=0;attempt<150&&!found;++attempt) {
    usleep(20000);
    found=storage_read(logs+"/trace.0.jsonl").find(
        "\"reason\":\"adapter_fault\"")!=std::string::npos;
  }
  assert(found && audit_fault && A::mode()==A::OBSERVE);
  assert(!mkdir((logs+"/capture.stop").c_str(),0700));
  assert(!pthread_join(thread,0) && queue.drained());
  const std::string final=storage_read(logs+"/trace.0.jsonl");
  assert(final.rfind("\"reason\":\"adapter_fault\"") <
         final.find("\"kind\":\"capture_end\""));
  assert(!unlink((logs+"/capture.done").c_str()));
  assert(!rmdir((logs+"/capture.stop").c_str()));
  puts("Adapter fault without a row: worker records incomplete capture");
}
// Authored snapshots reproduce the independently observed partial-cache shape:
// GSA changes quality alone; GGA changes altitude/coordinates before RMC time
// and motion. This exercises the real formatter, not an OEM parser replacement.
static void position_journal() {
  A::Observation o=A::Observation();o.kind=A::Observation::POSITION;
  o.request_result=A::R::NOT_FOUND;
  o.call_sequence=17;o.prediction_generation=4;o.mono_ns=103;
  o.original_mode=o.position.mode=1;o.position.utc_seconds=1790856000;
  o.position.latitude_deg=35;o.position.longitude_deg=135;
  o.position.heading_deg=20;o.position.velocity_kmh=18;
  for(unsigned stage=0;stage<8;++stage) {
    if(stage==1) {o.position.horizontal=1;o.position.vertical=1.5;}
    if(stage==2)o.position.altitude_m=12;
    if(stage==3) {
      o.position.latitude_deg=36;o.position.longitude_deg=136;o.position.altitude_m=24;
    }
    if(stage==4) {
      ++o.position.utc_seconds;o.position.heading_deg=40;o.position.velocity_kmh=37;
    }
    if(stage==5) {
      o.position.altitude_m=INT32_MIN;
      o.position.horizontal=std::numeric_limits<double>::quiet_NaN();
      o.position.vertical=std::numeric_limits<double>::infinity();
    }
    if(stage==6) {
      o.position.altitude_m=INT32_MAX;
      o.position.horizontal=-std::numeric_limits<double>::infinity();o.position.vertical=0;
    }
    if(stage==7) {
      o.position.altitude_m=-12;o.position.horizontal=-0.25;o.position.vertical=0.5;
    }
    char line[mx5::runtime::OBSERVATION_JSON_CAPACITY];
    assert(format_observation(line,sizeof line,o));puts(line);
    ++o.call_sequence;++o.mono_ns;
  }
}
static void request_journal(bool emit) {
  namespace R=mx5::runtime::request_trace;
  A::Observation o=A::Observation();o.kind=A::Observation::POSITION;o.call_sequence=17;
  o.mono_ns=103;o.request_result=R::OK;
  R::Trace& t=o.request_trace;
  t.request.id=1;t.request.epoch=3;t.worker.id=2;t.worker.epoch=3;
  t.issue.observed_ns=101;t.reply.observed_ns=102;t.reply.type_known=true;t.reply.type=2;
  const A::S::Snapshot session={A::S::OBSERVED,8,2,-7,true,12};
  t.issue.session_context=session;o.send_session=session;
  t.reply.sender=R::copy_text(":1.42");t.reply.error_name=R::copy_text("org.freedesktop.DBus.Error.ServiceUnknown");
  t.issue.wire.observed_ns=101;t.issue.wire.serial=23;t.issue.wire.known=true;
  t.reply.wire.observed_ns=102;t.reply.wire.serial=41;t.reply.wire.reply_serial=23;
  t.reply.wire.type=3;t.reply.wire.known=true;
  t.reply.wire.sender=t.reply.sender;t.reply.wire.error_name=t.reply.error_name;
  t.issue.route.destination=R::copy_text("com.jci.lds.data");
  t.issue.route.path=R::copy_text("/com/jci/lds/data");
  t.issue.route.interface_name=R::copy_text("com.jci.lds.data");
  t.issue.route.member=R::copy_text("GetPosition");
  const A::Observation baseline=o;
  char line[mx5::runtime::OBSERVATION_JSON_CAPACITY];
  assert(format_observation(line,sizeof line,o));if(emit)puts(line);
  o.kind=A::Observation::SEND;o.type=1;o.length=48;o.has_payload=true;
  assert(format_observation(line,sizeof line,o));if(emit)puts(line);
  // Failed observation must not serialize a stale input Trace as associated.
  o.request_result=R::FULL;
  assert(format_observation(line,sizeof line,o));if(emit)puts(line);
  o.request_result=R::OK;
  t.reply.sender=R::copy_text("quote\"\\\n\001\377");
  t.reply.wire.sender=t.reply.sender;
  assert(format_observation(line,sizeof line,o));if(emit)puts(line);
  // Worst bounded names and integers still fit the actual worker buffer.
  memset(t.reply.sender.bytes,1,sizeof t.reply.sender.bytes);
  t.reply.sender.complete=false;t.reply.error_name=t.reply.sender;
  t.issue.route.destination=t.issue.route.path=t.issue.route.interface_name=t.issue.route.member=t.reply.sender;
  t.issue.endpoint.server_guid=t.issue.endpoint.unique_name=t.reply.sender;
  t.reply.wire.sender=t.reply.wire.error_name=t.reply.sender;
  t.issue.wire.observed_ns=t.reply.wire.observed_ns=UINT64_MAX;
  t.issue.wire.serial=t.reply.wire.serial=t.reply.wire.reply_serial=UINT32_MAX;
  t.request.id=t.request.epoch=t.worker.id=t.worker.epoch=UINT64_MAX;
  t.issue.observed_ns=t.reply.observed_ns=UINT64_MAX;
  t.issue.bus_lifetime=t.issue.session_lifetime=t.issue.session_event=UINT64_MAX;
  t.issue.connection={mx5::runtime::bus_trace::CONNECTED,UINT32_MAX,UINT64_MAX};
  t.reply.connection=t.issue.connection;
  t.issue.known=7;t.issue.session_state=INT32_MIN;
  t.issue.session_context.lifetime=t.issue.session_context.event=UINT32_MAX;
  t.issue.session_context.revision=UINT64_MAX;
  t.issue.session_context.state=INT32_MIN;o.send_session=t.issue.session_context;
  t.reply.wire_serial_known=true;t.reply.wire_serial=UINT32_MAX;
  o.call_sequence=o.prediction_generation=o.type=o.length=UINT32_MAX;
  o.mono_ns=UINT64_MAX;o.original_mode=o.result=INT32_MIN;
  char measured_request[16384];
  assert(mx5::runtime::format_request_trace(measured_request,sizeof measured_request,R::OK,t));
  if(!emit)fprintf(stderr,"maximum escaped request JSON: %zu bytes; capacity %u\n",strlen(measured_request)+1,
      unsigned(mx5::runtime::REQUEST_JSON_CAPACITY));
  assert(format_observation(line,sizeof line,o));if(emit)puts(line);
  if(!emit)fprintf(stderr,"maximum escaped observation JSON: %zu bytes; capacity %u\n",strlen(line)+1,
      unsigned(mx5::runtime::OBSERVATION_JSON_CAPACITY));
  // Full nine-field POSITION rows must fit beside the same worst-case request
  // metadata. A legitimate long request must not trigger capture failure.
  A::Observation full=o;full.kind=A::Observation::POSITION;
  full.original_mode=full.position.mode=INT32_MIN;
  full.call_sequence=full.prediction_generation=UINT32_MAX;full.mono_ns=UINT64_MAX;
  full.position.utc_seconds=UINT64_MAX;
  full.position.altitude_m=INT32_MIN;
  full.position.latitude_deg=full.position.heading_deg=full.position.horizontal=
      std::numeric_limits<double>::max();
  full.position.longitude_deg=full.position.velocity_kmh=full.position.vertical=
      -std::numeric_limits<double>::max();
  assert(format_observation(line,sizeof line,full));
  const size_t position_required=strlen(line)+1;
  if(!emit)fprintf(stderr,"maximum escaped position JSON: %zu bytes; capacity %u\n",position_required,
      unsigned(mx5::runtime::OBSERVATION_JSON_CAPACITY));
  char position_bounds[mx5::runtime::OBSERVATION_JSON_CAPACITY+2];
  memset(position_bounds,0x5a,sizeof position_bounds);
  assert(format_observation(position_bounds+1,position_required,full));
  assert(!strcmp(position_bounds+1,line));
  assert(position_bounds[0]==0x5a && position_bounds[position_required+1]==0x5a);
  memset(position_bounds,0x5a,sizeof position_bounds);
  assert(!format_observation(position_bounds+1,position_required-1,full));
  assert(position_bounds[position_required-1]==0 && position_bounds[0]==0x5a &&
      position_bounds[position_required]==0x5a);
  A::Observation identified=full;
  identified.request_trace.issue.endpoint.server_guid=R::copy_text("0123456789abcdef0123456789abcdef");
  identified.request_trace.issue.endpoint.unique_name=R::copy_text(":1.7");
  identified.request_trace.issue.wire.endpoint_matched=true;
  assert(format_observation(line,sizeof line,identified));
  assert(strstr(line,"\"server_guid\":{\"value\":\"0123456789abcdef0123456789abcdef\",\"complete\":true}"));
  assert(strstr(line,"\"unique_name\":{\"value\":\":1.7\",\"complete\":true}"));
  assert(strstr(line,"\"endpoint_matched\":true"));
  identified.request_trace.issue.wire.conflict=true;
  assert(format_observation(line,sizeof line,identified));
  assert(strstr(line,"\"endpoint_matched\":false"));
  // Exact-size success, one byte short failure, and adjacent bytes untouched.
  char request[mx5::runtime::REQUEST_JSON_CAPACITY];assert(mx5::runtime::format_request_trace(request,sizeof request,R::OK,t));
  const size_t required=strlen(request)+1;
  char bounds[mx5::runtime::REQUEST_JSON_CAPACITY+2];memset(bounds,0x5a,sizeof bounds);
  assert(mx5::runtime::format_request_trace(bounds+1,required,R::OK,t));
  assert(bounds[0]==0x5a && bounds[required+1]==0x5a);
  memset(bounds,0x5a,sizeof bounds);
  assert(!mx5::runtime::format_request_trace(bounds+1,required-1,R::OK,t));
  assert(!bounds[1] && bounds[0]==0x5a && bounds[required]==0x5a);
  assert(!mx5::runtime::format_request_trace(0,0,R::OK,t));
  // A locally generated timeout still has a copied raw header; zero response
  // serial/unknown sender must not erase its reply_serial, type or error name.
  A::Observation local=baseline;
  R::Reply& local_reply=local.request_trace.reply;
  local_reply.type_known=false;local_reply.wire_serial_known=false;
  local_reply.sender=local_reply.error_name=R::Text();
  local.request_trace.issue.wire.observed_ns=0;local_reply.wire.observed_ns=0;
  local_reply.wire.serial=0;local_reply.wire.sender=R::Text();
  local_reply.wire.error_name=R::copy_text("org.freedesktop.DBus.Error.NoReply");
  assert(format_observation(line,sizeof line,local));if(emit)puts(line);
}
// Authored producer controls for the Python result auditor. Pipeline rows use
// actual status counters; direct core rows have a different kind and must not
// be mistaken for complete runtime worker diagnostics.
namespace model_result_cases {
static uint64_t time_at(unsigned ms) { return 1000000000ULL+uint64_t(ms)*1000000ULL; }
static N::RawEvent raw(N::SensorKind kind,unsigned ms,unsigned seq,unsigned value=13600) {
  N::RawEvent r=N::RawEvent();r.kind=kind;r.epoch=1;r.receive_seq=seq;
  r.received_ns=time_at(ms);r.source_mono_ms=int64_t(r.received_ns/1000000);r.count=1;
  for(unsigned i=0;i<4;++i)r.raw[i]=uint16_t(kind==N::YAW?2047:value);
  return r;
}
static A::Observation position(unsigned ms,int mode,unsigned seq) {
  A::Observation o=A::Observation();o.kind=A::Observation::POSITION;
  o.call_sequence=seq;o.mono_ns=time_at(ms);o.original_mode=mode;o.position.mode=mode;
  o.position.utc_seconds=1700000000;o.position.latitude_deg=35;o.position.longitude_deg=135;
  o.position.velocity_kmh=36;return o;
}
static void init(N::Pipeline& p) {
  const mx5_dr_context c={1,1,1};
  assert(p.init_model(N::research_model_profile(),mx5_dr_default_config(),c));
}
static void feed(N::Pipeline& p,unsigned ms,unsigned seq,unsigned wheels=13600) {
  assert(p.enqueue_raw(raw(N::WHEELS,ms,seq,wheels))==N::PIPELINE_OK);
  const N::PipelineResult r=p.enqueue_raw(raw(N::YAW,ms,seq));
  assert(r==N::PIPELINE_OK||r==N::PIPELINE_WAITING);
}
static void seeded(N::Pipeline& p,unsigned wheels=13600) {
  init(p);
  for(unsigned ms=0;ms<=200;ms+=100) {
    const unsigned seq=ms/100+1;
    assert(p.enqueue_raw(raw(N::REVERSE,ms,seq))==N::PIPELINE_OK);feed(p,ms,seq,wheels);
    if(ms<200)assert(p.enqueue_position(position(ms,1,seq))==N::PIPELINE_OK);
    if(ms==100)assert(p.enqueue_position(position(110,0,3))==N::PIPELINE_OK);
  }
  feed(p,300,4,wheels);assert(p.drain(time_at(200))==N::PIPELINE_OK);
}
static void emit(const char* label,uint64_t now,const N::Diagnostic& d,bool core_only=false) {
  char lat[48],lon[48],heading[48],speed[48],error[48],preview[97]="";
  json_number(d.snapshot.latitude_deg,lat);json_number(d.snapshot.longitude_deg,lon);
  json_number(d.snapshot.body_heading_rad,heading);json_number(d.snapshot.speed_mps,speed);
  json_number(d.snapshot.error_budget_m,error);uint8_t bytes[48];
  const bool encoded=mx5::runtime::encode_model_location_preview(d.snapshot,bytes);
  if(encoded)hex48(bytes,preview);
  printf("{\"case\":\"%s\",\"kind\":\"%s\",\"mono_ns\":%llu,\"domain\":\"model\","
      "\"assist_ready\":false,\"model_valid\":%s,\"state\":%u,\"result\":\"%s\",\"pipeline\":\"%s\","
      "\"uncertainties\":%u,\"events\":%llu,\"intervals\":%llu,\"resets\":%llu,\"rejected\":%llu,"
      "\"frontier_ns\":%llu,\"lat\":%s,\"lon\":%s,\"heading_rad\":%s,\"speed_mps\":%s,"
      "\"error_model_m\":%s,\"stopped\":%s,\"preview_encoded\":%s,\"location_preview_hex\":\"%s\"}\n",
      label,core_only?"test_core_snapshot":"shadow",(unsigned long long)now,
      d.snapshot.model_valid?"true":"false",unsigned(d.snapshot.state),mx5_dr_result_name(d.result),
      N::pipeline_result_name(d.status.result),d.status.uncertainties,(unsigned long long)d.status.events,
      (unsigned long long)d.status.intervals,(unsigned long long)d.status.resets,(unsigned long long)d.status.rejected,
      (unsigned long long)d.snapshot.frontier_ns,lat,lon,heading,speed,error,d.snapshot.stopped?"true":"false",
      encoded?"true":"false",preview);
}
static mx5_dr_core core(double lon,double heading,double error=0,mx5_dr_config cfg=mx5_dr_default_config()) {
  mx5_dr_core c;mx5_dr_context context={1,1,1};
  assert(mx5_dr_init_model(&c,&cfg,context)==MX5_DR_OK);
  mx5_dr_anchor a=mx5_dr_anchor();a.context=context;a.anchor_id=a.position_seq=1;a.measured_ns=time_at(0);
  a.utc_ns=1700000000000000000ULL;a.longitude_deg=lon;a.body_heading_rad=heading;
  a.position_error_m=error;a.quality=MX5_DR_MODEL;assert(mx5_dr_seed(&c,&a)==MX5_DR_OK);
  context.generation=2;assert(mx5_dr_control(&c,MX5_DR_GAP,context,2)==MX5_DR_OK);return c;
}
static void step(mx5_dr_core& c,uint64_t dt,double speed,double yaw) {
  mx5_dr_interval i=mx5_dr_interval();i.context=c.estimate.context;i.interval_seq=1;i.start_ns=c.estimate.frontier_ns;
  i.end_ns=i.start_ns+dt;i.received_ns=i.end_ns;i.speed_mps=speed;i.yaw_rad_s=yaw;i.raw_yaw=2047;i.yaw_count=1;
  mx5_dr_evidence* inputs[]={&i.speed,&i.yaw,&i.reverse};
  for(unsigned n=0;n<3;++n) {
    inputs[n]->source_id=n+1;inputs[n]->source_epoch=1;inputs[n]->producer_seq=1;
    inputs[n]->measured_ns=inputs[n]->received_ns=i.start_ns;inputs[n]->lease_until_ns=i.start_ns+250000000;
    inputs[n]->quality=MX5_DR_MODEL;inputs[n]->freshness=MX5_DR_MODEL_TIME;
  }
  assert(mx5_dr_step(&c,&i)==MX5_DR_OK);
}
static void emit_core(const char* label,const mx5_dr_core& c,uint64_t now) {
  N::Diagnostic d=N::Diagnostic();
  d.result=mx5_dr_get_model_snapshot(&c,now,c.estimate.context,&d.snapshot);emit(label,now,d,true);
}
static int run() {
  N::Pipeline p;init(p);emit("unseeded",time_at(0),p.diagnostic(time_at(0)));
  seeded(p);emit("active_valid",time_at(300),p.diagnostic(time_at(300)));
  emit("active_stale",time_at(500),p.diagnostic(time_at(500)));
  emit("active_time_error",time_at(199),p.diagnostic(time_at(199)));
  assert(p.enqueue_raw(raw(N::REVERSE,350,4))==N::PIPELINE_OK);
  // The reverse callback waits behind the open yaw window. The prior closed
  // interval remains valid; no future event is committed to force WAITING.
  assert(p.drain(time_at(350))==N::PIPELINE_OK);
  assert(p.diagnostic(time_at(350)).snapshot.model_valid);
  emit("active_valid_pending",time_at(350),p.diagnostic(time_at(350)));
  seeded(p);assert(p.enqueue_position(position(310,1,4))==N::PIPELINE_OK);
  emit("active_queued_gps",time_at(310),p.diagnostic(time_at(310)));
  seeded(p);assert(p.enqueue_position(position(310,3,4))==N::PIPELINE_OK);
  emit("active_queued_native",time_at(310),p.diagnostic(time_at(310)));
  p.drain(time_at(310));
  assert(p.enqueue_raw(raw(N::YAW,400,5))==N::PIPELINE_OK);
  assert(p.drain(time_at(400))==N::PIPELINE_OK);
  emit("native",time_at(400),p.diagnostic(time_at(400)));
  seeded(p,10000);emit("active_near_zero_not_stopped",time_at(300),p.diagnostic(time_at(300)));
  for(unsigned ms=400;ms<=2100;ms+=100) {
    assert(p.enqueue_raw(raw(N::REVERSE,ms,ms/100+1))==N::PIPELINE_OK);feed(p,ms,ms/100+1,10000);
    assert(p.drain(time_at(ms-100))==N::PIPELINE_OK);
  }
  emit("active_stopped",time_at(2100),p.diagnostic(time_at(2100)));
  mx5_dr_core c=core(0,0);step(c,100000000,0,0);emit_core("zero_not_stopped",c,c.estimate.frontier_ns);
  c=core(0,0);step(c,1,1,-1e-7);emit_core("heading_2pi",c,c.estimate.frontier_ns);
  c=core(-180,3*3.14159265358979323846/2);step(c,1,3,0);emit_core("longitude_180",c,c.estimate.frontier_ns);
  c=core(0,0,99);step(c,100000000,10,0);emit_core("error_limit",c,c.estimate.frontier_ns+150000000);
  mx5_dr_config cfg=mx5_dr_default_config();cfg.duration_max_s=.2;
  c=core(0,0,0,cfg);step(c,100000000,1,0);emit_core("duration_limit",c,c.estimate.frontier_ns+150000000);
  // Keep the 250 ms sensor lease live across both queries, so only query age
  // distinguishes the inclusive 150 ms boundary from one nanosecond beyond it.
  c=core(0,0);step(c,50000000,1,0);emit_core("exact_age_limit",c,c.estimate.frontier_ns+150000000);
  emit_core("over_age_limit",c,c.estimate.frontier_ns+150000001);
  cfg=mx5_dr_default_config();cfg.physical_speed_max_mps=200;
  c=core(0,0,0,cfg);step(c,100000000,101,0);emit_core("custom_speed",c,c.estimate.frontier_ns);
  return 0;
}
}
// ---- Log retention: boots append (validation/LOG_RETENTION_2026-10-10.md) ----
// A boot appends to an existing trace.0.jsonl until it is full; only a full
// file rotates, so the ring is the newest rows whatever the number of boots.
static bool retention_exists(const std::string& path) { struct stat st;return lstat(path.c_str(),&st)==0; }
static void retention_put(const std::string& path,const std::string& bytes,mode_t mode=0600) {
  unlink(path.c_str());
  const int fd=open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,mode);assert(fd>=0);
  assert(fchmod(fd,mode)==0);
  size_t done=0;
  while(done<bytes.size()) { const ssize_t w=write(fd,bytes.data()+done,bytes.size()-done);assert(w>0);done+=size_t(w); }
  assert(close(fd)==0);
}
// n bytes of complete rows (the last row padded to land exactly on n).
static std::string retention_rows(size_t n,char tag) {
  std::string out;const std::string head=std::string("{\"kind\":\"old_")+tag+"\",\"pad\":\"";
  while(out.size()<n) {
    const size_t left=n-out.size();
    if(left<head.size()+3) { out.append(left-1,' ');out+='\n';break; }
    const size_t pad=std::min<size_t>(left-head.size()-3,200);
    out+=head;out.append(pad,'x');out+="\"}\n";
  }
  assert(out.size()==n);return out;
}
static size_t retention_lines(const std::string& s) { return size_t(std::count(s.begin(),s.end(),'\n')); }
static void retention_boot(const char* root,const char* row,size_t* written=0) {
  Journal j(root);j.line(row);j.flush();assert(!j.failed);
  if(written)*written=j.written;
}
static void retention_tests() {
  const size_t saved_bytes=config.max_log_bytes;const unsigned saved_files=config.max_log_files;
  config.max_log_bytes=16777216;config.max_log_files=3;
  char root[]="/tmp/mx5dr-retention-XXXXXX";assert(mkdtemp(root));
  const std::string logs=std::string(root)+"/logs";assert(!mkdir(logs.c_str(),0700));
  const std::string t0=logs+"/trace.0.jsonl",t1=logs+"/trace.1.jsonl",t2=logs+"/trace.2.jsonl";
  arm_test_mode();
  // 1. First boot creates trace.0 (0600) and nothing else.
  size_t w=0;
  retention_boot(root,"{\"kind\":\"boot\",\"n\":1}",&w);
  assert(storage_read(t0)=="{\"kind\":\"boot\",\"n\":1}\n" && !retention_exists(t1));
  { struct stat st;assert(!stat(t0.c_str(),&st) && (st.st_mode&0777)==0600); }
  // 2. A short boot appends: no shift, written continues from the file size.
  retention_boot(root,"{\"kind\":\"boot\",\"n\":2}",&w);
  assert(storage_read(t0)=="{\"kind\":\"boot\",\"n\":1}\n{\"kind\":\"boot\",\"n\":2}\n");
  assert(w==storage_read(t0).size() && !retention_exists(t1));
  // 3. Many short boots after a large drive file (2 MiB, well above any
  //    fixed small-file threshold) keep appending: no trace.1 appears and the
  //    drive's rows stay first in trace.0.
  const std::string drive=retention_rows(2u*1024*1024,'d');
  retention_put(t0,drive);
  const std::string chunk=std::string("{\"kind\":\"short\",\"pad\":\"")+std::string(100000,'s')+"\"}";
  for(unsigned i=3;i<11;++i) {
    char row[64];snprintf(row,sizeof row,"{\"kind\":\"boot\",\"n\":%u}",i);
    Journal j(root);j.line(row);j.line(chunk.c_str());j.flush();assert(!j.failed);
  }
  {
    const std::string now=storage_read(t0);
    assert(!retention_exists(t1) && now.compare(0,drive.size(),drive)==0);
    assert(retention_lines(now)==retention_lines(drive)+16);
  }
  // 4. A boot that finds a file within 100 bytes of the cap rotates normally
  //    on its first write; a file at the cap is rotated too.
  config.max_log_bytes=1048576;
  const std::string full=retention_rows(1048576-100,'f');
  retention_put(t0,full);unlink(t1.c_str());
  const std::string row200=std::string("{\"kind\":\"boot\",\"pad\":\"")+std::string(170,'p')+"\"}";
  retention_boot(root,row200.c_str(),&w);
  assert(storage_read(t1)==full && storage_read(t0)==row200+"\n" && w==row200.size()+1);
  const std::string atcap=retention_rows(1048576,'c');
  retention_put(t0,atcap);unlink(t1.c_str());
  retention_boot(root,"{\"kind\":\"boot\",\"n\":20}");
  assert(storage_read(t1)==atcap && storage_read(t0)=="{\"kind\":\"boot\",\"n\":20}\n");
  // A full file is not even opened for append: a cut last row gets no
  // separator and moves to trace.1 byte for byte.
  const std::string cutfull=atcap.substr(0,atcap.size()-1)+"x";
  retention_put(t0,cutfull);unlink(t1.c_str());
  retention_boot(root,"{\"kind\":\"boot\",\"n\":22}");
  assert(storage_read(t1)==cutfull && storage_read(t0)=="{\"kind\":\"boot\",\"n\":22}\n");
  // 4b. 100 short boots stay in trace.0 until it fills; then the ring
  //     rotates once and trace.1 starts with those 100 boots in order.
  config.max_log_bytes=65536;
  unlink(t0.c_str());unlink(t1.c_str());unlink(t2.c_str());
  std::string hundred;
  for(unsigned i=0;i<100;++i) {
    char row[64];snprintf(row,sizeof row,"{\"kind\":\"boot\",\"n\":%u,\"pad\":\"xxxxxxxxxxxxxxxx\"}",i);
    retention_boot(root,row);hundred+=std::string(row)+"\n";
  }
  assert(storage_read(t0)==hundred && !retention_exists(t1));
  const std::string big900=std::string("{\"kind\":\"boot\",\"pad\":\"")+std::string(900,'b')+"\"}";
  unsigned boots=0;
  while(!retention_exists(t1)) { retention_boot(root,big900.c_str());++boots; assert(boots<200); }
  assert(boots>=30 && storage_read(t1).compare(0,hundred.size(),hundred)==0 &&
         storage_read(t1).size()<=65536 && storage_read(t0)==big900+"\n");
  config.max_log_bytes=16777216;
  unlink(t1.c_str());
  // 5. A previous boot cut mid-row gets a newline separator first: rows
  //    never concatenate, and the separator is counted by the cap.
  retention_put(t0,"{\"kind\":\"boot\",\"n\":10}\n{\"kind\":\"cut");
  retention_boot(root,"{\"kind\":\"boot\",\"n\":11}",&w);
  assert(storage_read(t0)=="{\"kind\":\"boot\",\"n\":10}\n{\"kind\":\"cut\n{\"kind\":\"boot\",\"n\":11}\n");
  assert(w==storage_read(t0).size() && !retention_exists(t1));
  // An empty existing trace.0 is reused without a separator.
  retention_put(t0,"");
  retention_boot(root,"{\"kind\":\"boot\",\"n\":12}",&w);
  assert(storage_read(t0)=="{\"kind\":\"boot\",\"n\":12}\n" && w==storage_read(t0).size());
  // 6. The cap counts the reused bytes: a reused file that fills rotates
  //    normally, and a same-boot rotation never reuses.
  config.max_log_bytes=65536;
  const std::string nearly=retention_rows(65000,'n');
  retention_put(t0,nearly);unlink(t1.c_str());unlink(t2.c_str());
  {
    Journal j(root);
    const std::string big=std::string("{\"kind\":\"fill\",\"pad\":\"")+std::string(900,'f')+"\"}";
    j.line(big.c_str());j.flush();assert(!j.failed);
    assert(storage_read(t1)==nearly && storage_read(t0)==big+"\n" && j.written==big.size()+1);
    while(!retention_exists(t2)) { j.line(big.c_str());assert(!j.failed); }
    j.flush();assert(!j.failed);
    assert(storage_read(t2)==nearly);
    std::string first=storage_read(t1);
    assert(first.size()<=65536 && first.size()+big.size()+1>65536 && retention_lines(first)==first.size()/(big.size()+1));
    assert(storage_read(t0)==big+"\n");
  }
  config.max_log_bytes=16777216;
  // 7. Never follows a symlink: a linked trace.0 is rotated as before and
  //    its target is untouched.
  unlink(t1.c_str());unlink(t2.c_str());
  const std::string target=std::string(root)+"/target";
  retention_put(target,"{\"kind\":\"target\"}\n");
  unlink(t0.c_str());assert(!symlink(target.c_str(),t0.c_str()));
  retention_boot(root,"{\"kind\":\"boot\",\"n\":13}");
  assert(storage_read(target)=="{\"kind\":\"target\"}\n");
  { struct stat st;assert(!lstat(t1.c_str(),&st) && S_ISLNK(st.st_mode)); }
  assert(storage_read(t0)=="{\"kind\":\"boot\",\"n\":13}\n");
  // 8. A file this writer would not have created (group/other access, or a
  //    second hard link) is rotated, not appended to.
  unlink(t1.c_str());
  retention_put(t0,"{\"kind\":\"loose\"}\n",0644);
  retention_boot(root,"{\"kind\":\"boot\",\"n\":14}");
  assert(storage_read(t1)=="{\"kind\":\"loose\"}\n" && storage_read(t0)=="{\"kind\":\"boot\",\"n\":14}\n");
  unlink(t1.c_str());
  const std::string other=std::string(root)+"/other";
  retention_put(t0,"{\"kind\":\"linked\"}\n");assert(!link(t0.c_str(),other.c_str()));
  retention_boot(root,"{\"kind\":\"boot\",\"n\":15}");
  assert(storage_read(other)=="{\"kind\":\"linked\"}\n" && storage_read(t0)=="{\"kind\":\"boot\",\"n\":15}\n");
  unlink(other.c_str());unlink(target.c_str());
  // 8b. A FIFO at trace.0 is neither waited on nor appended to: rotated.
  unlink(t0.c_str());unlink(t1.c_str());assert(!mkfifo(t0.c_str(),0600));
  retention_boot(root,"{\"kind\":\"boot\",\"n\":21}");
  { struct stat st;assert(!lstat(t1.c_str(),&st) && S_ISFIFO(st.st_mode)); }
  assert(storage_read(t0)=="{\"kind\":\"boot\",\"n\":21}\n");
  // 9. The writer thread's backend follows the same rule.
  unlink(t1.c_str());
  retention_put(t0,"{\"kind\":\"boot\",\"n\":16}\n{\"kind\":\"cut");
  {
    Journal j(root);assert(j.start_writer());
    j.line("{\"kind\":\"boot\",\"n\":17}");
    assert(j.close_durable());assert(!j.failed);
  }
  assert(storage_read(t0)=="{\"kind\":\"boot\",\"n\":16}\n{\"kind\":\"cut\n{\"kind\":\"boot\",\"n\":17}\n");
  assert(!retention_exists(t1));
  // 10. Only the first open of a boot may reuse: a write after this
  //     object closed its file (capture stop) rotates as before.
  unlink(t0.c_str());
  {
    Journal j(root);j.line("{\"kind\":\"boot\",\"n\":18}");assert(j.close_durable());
    j.line("{\"kind\":\"late\"}");j.flush();assert(!j.failed);
  }
  assert(storage_read(t1)=="{\"kind\":\"boot\",\"n\":18}\n" && storage_read(t0)=="{\"kind\":\"late\"}\n");
  unlink(t0.c_str());unlink(t1.c_str());unlink(t2.c_str());
  assert(!rmdir(logs.c_str()) && !rmdir(root));
  config.max_log_bytes=saved_bytes;config.max_log_files=saved_files;
  puts("Log retention: boots append to trace.0 until it is full (separator after a cut row, cap counts reused bytes); "
       "only a full file rotates, so short boots never push a drive out of the ring");
}
int main(int argc,char** argv) {
  if(argc==2 && !strcmp(argv[1],"--rmc-status")) {rmc_status_journal();return 0;}
  if(argc==2 && !strcmp(argv[1],"--heading-presence")) {heading_presence_journal();return 0;}
  if(argc==2 && !strcmp(argv[1],"--emit-positions")) {position_journal();return 0;}
  if(argc==2 && !strcmp(argv[1],"--retention")) {retention_tests();return 0;}
  if(argc==3 && !strcmp(argv[1],"--real-storage")) {
    config.max_log_bytes=8388608;config.max_log_files=3;
    arm_test_mode();check_real_storage<Journal>(argv[2],"trace");return 0;
  }
  if(argc==3 && !strcmp(argv[1],"--storage")) {
    config.max_log_bytes=65536;config.max_log_files=3;
    arm_test_mode();check_storage<Journal>("trace",argv[2]);return 0;
  }
  if(argc==2 && !strcmp(argv[1],"--emit-model-results"))return model_result_cases::run();
  if(argc==2 && !strcmp(argv[1],"--emit-rejected")) {
    N::ReceiveDiagnostic d=N::ReceiveDiagnostic();d.reason=N::RECEIVE_STALE;
    d.authenticated_decoded=d.credentials_present=true;d.checked_ns=1250000001;
    d.sender_pid=42;d.rejected.kind=N::REVERSE;d.rejected.epoch=9;
    d.rejected.receive_seq=3;d.rejected.received_ns=1000000000;d.rejected.reverse=1;
    char line[1200];assert(format_motion_rejected(line,sizeof line,d));puts(line);return 0;
  }
  const bool emit_requests=argc==2 && !strcmp(argv[1],"--emit-requests");
  request_journal(emit_requests);
  if(emit_requests)return 0;
  heading_presence_journal();
  rmc_status_journal();
  A::Options opt = A::Options();
  assert(A::configure(unused_next, opt));
  if(argc==2 && !strcmp(argv[1],"--late-adapter-fault")) {
    char fault_root[]="/tmp/mx5dr-late-fault-XXXXXX";
    assert(mkdtemp(fault_root));
    const std::string fault_logs=std::string(fault_root)+"/logs";
    assert(!mkdir(fault_logs.c_str(),0700));
    arm_test_mode();config.max_log_bytes=65536;
    freeze_capture();assert(queue.drained());
    A::position_leave(); // Fault after the final queue has closed.
    assert(A::faulted());
    const char* boot="12345678-1234-1234-1234-123456789abc";
    { Journal journal(fault_root);assert(finish_capture(journal,boot,100,101)); }
    const std::string final=storage_read(fault_logs+"/trace.0.jsonl");
    assert(final.find("\"reason\":\"adapter_fault\"") <
           final.find("\"kind\":\"capture_end\""));
    assert(final.find("\"audit_fault\":1")!=std::string::npos);
    assert(!unlink((fault_logs+"/capture.done").c_str()));
    assert(!unlink((fault_logs+"/trace.0.jsonl").c_str()));
    assert(!rmdir(fault_logs.c_str())&&!rmdir(fault_root));
    puts("Late adapter fault: incomplete marker precedes durable stop acknowledgement");
    return 0;
  }
  if(argc==2 && !strcmp(argv[1],"--adapter-fault")) {
    char fault_root[]="/tmp/mx5dr-adapter-fault-XXXXXX";
    assert(mkdtemp(fault_root));
    const std::string fault_logs=std::string(fault_root)+"/logs";
    assert(!mkdir(fault_logs.c_str(),0700));
    context_loss_worker(fault_root,fault_logs);
    adapter_fault_worker(fault_root,fault_logs);
    for(unsigned i=0;i<3;++i)
      unlink((fault_logs+"/trace."+char('0'+i)+".jsonl").c_str());
    assert(!rmdir(fault_logs.c_str())&&!rmdir(fault_root));
    return 0;
  }
  if(argc==2 && !strcmp(argv[1],"--writer")) {
    // Journal writer thread (about 3 s of deliberate stalls): run separately
    // so the default run stays a quick fixture for other suites.
    char root[]="/tmp/mx5dr-writer-XXXXXX";assert(mkdtemp(root));
    const std::string logs=std::string(root)+"/logs";assert(!mkdir(logs.c_str(),0700));
    writer_stall_isolation(root,logs);
    writer_overflow(root,logs);
    writer_fail_closed(root,logs);
    writer_shutdown(root,logs);
    writer_idle_and_health(root,logs);
    journal_lag_bound(root,logs);
    writer_lag_and_wakeups(root,logs);
    writer_age_flush(root,logs);
    writer_lagging_fix_rows(root,logs);
    window_burst_lag(root,logs);
    persistent_worker(root,logs);
    clear_traces(logs);assert(!rmdir(logs.c_str())&&!rmdir(root));
    return 0;
  }
  cadence_tests();
  retention_tests();
  config.max_log_bytes = 64;
  config.max_log_files = 3;
  char tmp[] = "/tmp/mx5dr-journal-XXXXXX";
  assert(mkdtemp(tmp));
  std::string logs = std::string(tmp) + "/logs";
  arm_test_mode();
  {
    Journal missing(tmp);
    missing.line("{}");
    assert(missing.failed && A::mode() == A::OBSERVE);
  }
  assert(mkdir(logs.c_str(), 0700) == 0);
  arm_test_mode();
  {
    Journal j(tmp);
    for (int i = 0; i < 20; ++i)
      j.line("{\"kind\":\"fixture\"}");
    j.flush();
    assert(!j.failed && A::mode() == A::SCRUB_STALE);
  }
  for (int i = 0; i < 3; ++i) {
    struct stat st;
    std::string name = logs + "/trace." + char('0' + i) + ".jsonl";
    assert(stat(name.c_str(), &st) == 0 && st.st_size <= 64);
    unlink(name.c_str());
  }
  // Exercise the actual worker helpers, not a test-only batching wrapper.
  config.max_log_bytes = 65536;
  {
    Journal j(tmp);
    mx5::runtime::MotionBatch batch;
    N::RawEvent raw = N::RawEvent();
    raw.kind = N::YAW; raw.epoch = 1; raw.received_ns = 10;
    for (unsigned i = 1; i <= 33; ++i) {
      raw.receive_seq = i;
      journal_motion(j, batch, raw);
    }
    raw.epoch = 2; raw.receive_seq = 1;
    journal_motion(j, batch, raw); // Flushes the old epoch before adding new.
    flush_motion(j, batch);
    j.line("{\"kind\":\"shadow_input_reset\"}");
    raw.receive_seq = 3;
    journal_motion(j, batch, raw);
    flush_motion(j, batch); // Empty channel/turn end before health or sleep.
    flush_motion(j, batch); // Must not duplicate a previous batch.
    j.flush();
    assert(!j.failed && batch.empty());
  }
  {
    std::ifstream f((logs + "/trace.0.jsonl").c_str());
    std::string line;
    unsigned n = 0;
    while (std::getline(f, line)) {
      if (n == 3) assert(line.find("shadow_input_reset") != std::string::npos);
      else assert(line.find("motion_batch") != std::string::npos);
      if (n < 2) assert(line.find("\"epoch\":1") != std::string::npos);
      if (n == 2 || n == 4) assert(line.find("\"epoch\":2") != std::string::npos);
      ++n;
    }
    assert(n == 5);
  }
  unlink((logs + "/trace.0.jsonl").c_str());
  {
    Journal full(tmp);
    full.f = fopen("/dev/full", "w");
    assert(full.f);
    mx5::runtime::MotionBatch batch;
    N::RawEvent raw = N::RawEvent(); raw.kind = N::REVERSE;
    raw.epoch = raw.receive_seq = raw.received_ns = 1;
    journal_motion(full, batch, raw); flush_motion(full, batch); full.flush();
    assert(full.failed && batch.empty() && A::mode() == A::OBSERVE);
  }
  arm_test_mode();
  config.max_log_bytes = 64;
  {
    Journal full(tmp);
    full.f = fopen("/dev/full", "w");
    assert(full.f);
    full.line("{}");
    full.flush();
    assert(full.failed && A::mode() == A::OBSERVE);
  }
  arm_test_mode();
  {
    Journal rotation(tmp);
    rotation.f = fopen("/dev/full", "w");
    assert(rotation.f);
    assert(fputs("buffered", rotation.f) >= 0);
    rotation.rotate();
    assert(rotation.failed && A::mode() == A::OBSERVE);
  }
  arm_test_mode();
  {
    Journal overlong(tmp);
    overlong.line(std::string(80, 'x').c_str());
    assert(overlong.failed && A::mode() == A::OBSERVE);
  }
  arm_test_mode();
  A::Observation event = A::Observation();
  for (unsigned i = 0; i < 257; ++i) {
    event.call_sequence=i;
    sink(&event, 0);
  }
  assert(queue.dropped() == 1 && queue.lost() && A::mode() == A::OBSERVE);
  A::Observation read = A::Observation();
  for (unsigned i = 0; i < 256; ++i) {
    assert(pop(&read) && read.call_sequence==i);
  }
  assert(!pop(&read));
  arm_test_mode();
  sink(&event, 0);
  assert(!queue.dropped() && A::mode() == A::SCRUB_STALE);
  assert(pop(&read));
  receive_turn_tests(tmp,logs);
  late_turn_tests(tmp,logs);
  pipeline_capacity_overflow(tmp,logs);
  pipeline_fault_capture(tmp,logs);
  route_capture_tail(tmp,logs);
  stop_tests(tmp,logs);
  route_general_worker(tmp,logs);
  for(unsigned i=0;i<3;++i)unlink((logs+"/trace."+char('0'+i)+".jsonl").c_str());
  rmdir(logs.c_str());
  rmdir(tmp);
  puts("Journal tests: bounded rotation, audit failure, continued raw capture, "
       "separate rejected evidence, bounded receive turns and durable requested stop passed");
}
