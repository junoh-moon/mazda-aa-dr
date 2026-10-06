// Includes the private logger so failure paths can be tested without exposing
// configuration switches in the shipped runtime. No firmware is loaded.
#include "storage_fixture.h"
#define statvfs(path,info) fixture_statvfs(path,info)
#include "../../src/runtime/runtime.cpp"
#undef statvfs
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
  journal_ok.store(1);
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
static void pipeline_fault_capture(const char* root,const std::string& logs) {
  arm_test_mode();config.max_log_bytes=65536;
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
int main(int argc,char** argv) {
  if(argc==2 && !strcmp(argv[1],"--rmc-status")) {rmc_status_journal();return 0;}
  if(argc==2 && !strcmp(argv[1],"--heading-presence")) {heading_presence_journal();return 0;}
  if(argc==2 && !strcmp(argv[1],"--emit-positions")) {position_journal();return 0;}
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
    persistent_worker(root,logs);
    clear_traces(logs);assert(!rmdir(logs.c_str())&&!rmdir(root));
    return 0;
  }
  cadence_tests();
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
