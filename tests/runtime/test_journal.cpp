// Includes the private logger so failure paths can be tested without exposing
// configuration switches in the shipped runtime. No firmware is loaded.
#include "../../src/runtime/runtime.cpp"
#include <cassert>
#include <string>
#include <fstream>
#include <iterator>
#include <vector>
#include <new>

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
    assert(every_input.late>0&&every_input.holdout_late>0);
    if(wheel==20)assert(minimum_gate.late>0&&minimum_gate.holdout_late>0);
    assert(fixed.late==0&&fixed.holdout_late==0&&fixed.resets==0);
    assert(fixed.drains==old.drains&&fixed.drains.size()==31);
  }
  puts("worker cadence: receipt-only 150ms yaw, 10/20ms wheels; negative schedules fail, deadline preserves MODEL/holdout");
}
static void arm_test_mode() {
  audit_fault = 0;
  // Test-only reset, with no concurrent queue users.
  queue.~ObservationQueue();new(&queue) ObservationQueue;
  assert(A::set_mode(A::SCRUB_STALE));
}
struct FakeReceiver {
  unsigned calls,limit;
  bool gap;
  N::MotionCursor cursor;
  FakeReceiver(unsigned n,bool missing=false):calls(0),limit(n),gap(missing) {}
  N::ReceiveResult receive(uint64_t now,N::RawEvent* out,N::ReceiveDiagnostic* d) {
    if(calls==limit)return N::CHANNEL_EMPTY;
    N::RawEvent e=N::RawEvent();e.kind=N::WHEELS;e.epoch=1;
    ++calls;e.receive_seq=calls+(gap && calls>1?1:0);e.received_ns=now;
    for(unsigned i=0;i<4;++i)e.raw[i]=10000;
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
  t.reply.sender=t.reply.error_name=text;
  return event;
}
static void route_capture_tail(const char* root,const std::string& logs) {
  arm_test_mode();
  const A::Observation event=long_route_event();
  char expected[mx5::runtime::OBSERVATION_JSON_CAPACITY];
  assert(format_observation(expected,sizeof expected,event) && strlen(expected)>2200);
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
  assert(format_observation(expected,sizeof expected,event) && strlen(expected)>2200);
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
  t.issue.route.destination=R::copy_text("com.jci.lds.data");
  t.issue.route.path=R::copy_text("/com/jci/lds/data");
  t.issue.route.interface_name=R::copy_text("com.jci.lds.data");
  t.issue.route.member=R::copy_text("GetPosition");
  char line[mx5::runtime::OBSERVATION_JSON_CAPACITY];
  assert(format_observation(line,sizeof line,o));if(emit)puts(line);
  o.kind=A::Observation::SEND;o.type=1;o.length=48;o.has_payload=true;
  assert(format_observation(line,sizeof line,o));if(emit)puts(line);
  // Failed observation must not serialize a stale input Trace as associated.
  o.request_result=R::FULL;
  assert(format_observation(line,sizeof line,o));if(emit)puts(line);
  o.request_result=R::OK;
  t.reply.sender=R::copy_text("quote\"\\\n\001\377");
  assert(format_observation(line,sizeof line,o));if(emit)puts(line);
  // Worst bounded names and integers still fit the actual worker buffer.
  memset(t.reply.sender.bytes,1,sizeof t.reply.sender.bytes);t.reply.sender.bytes[63]=0;
  t.reply.sender.complete=false;t.reply.error_name=t.reply.sender;
  t.issue.route.destination=t.issue.route.path=t.issue.route.interface_name=t.issue.route.member=t.reply.sender;
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
  assert(format_observation(line,sizeof line,o));if(emit)puts(line);
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
}
int main(int argc,char** argv) {
  const bool emit_requests=argc==2 && !strcmp(argv[1],"--emit-requests");
  request_journal(emit_requests);
  if(emit_requests)return 0;
  if(argc==2 && !strcmp(argv[1],"--emit-rejected")) {
    N::ReceiveDiagnostic d=N::ReceiveDiagnostic();d.reason=N::RECEIVE_STALE;
    d.authenticated_decoded=d.credentials_present=true;d.checked_ns=1250000001;
    d.sender_pid=42;d.rejected.kind=N::REVERSE;d.rejected.epoch=9;
    d.rejected.receive_seq=3;d.rejected.received_ns=1000000000;d.rejected.reverse=1;
    char line[1200];assert(format_motion_rejected(line,sizeof line,d));puts(line);return 0;
  }
  cadence_tests();
  A::Options opt = A::Options();
  assert(A::configure(unused_next, opt));
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
  route_capture_tail(tmp,logs);
  stop_tests(tmp,logs);
  route_general_worker(tmp,logs);
  for(unsigned i=0;i<3;++i)unlink((logs+"/trace."+char('0'+i)+".jsonl").c_str());
  rmdir(logs.c_str());
  rmdir(tmp);
  puts("Journal tests: bounded rotation, audit failure, continued raw capture, "
       "separate rejected evidence, bounded receive turns and durable requested stop passed");
}
