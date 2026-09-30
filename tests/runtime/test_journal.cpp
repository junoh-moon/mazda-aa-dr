// Includes the private logger so failure paths can be tested without exposing
// configuration switches in the shipped runtime. No firmware is loaded.
#include "../../src/runtime/runtime.cpp"
#include <cassert>
#include <string>
#include <fstream>
#include <iterator>

static int32_t unused_next(void *, A::VehicleData *) { return 0; }
static void arm_test_mode() {
  audit_fault = 0;
  capture_stopped = 0;
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
  for(unsigned mode=0;mode<3;++mode) {
    arm_test_mode();
    N::Pipeline navigation;N::GpsHoldout holdout;
    assert(navigation.init_model(N::research_model_profile(),mx5_dr_default_config(),context));
    assert(holdout.init_model(N::research_model_profile(),mx5_dr_default_config(),context));
    if(mode==1)audit_fault=1;
    {
      Journal j(root);mx5::runtime::MotionBatch batch;FakeReceiver receiver(3,true);
      drain_motion(j,batch,receiver,navigation,holdout,mode!=2);j.flush();
      assert(!j.failed && batch.empty() && receiver.calls==3);
      assert(navigation.status().events==(mode==0?2:0));
      assert(navigation.context().generation==(mode==0?2:1));
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
    arm_test_mode();assert(worker_at(root)==0 && capture_stopped==1);
    std::ifstream after((logs+"/trace.0.jsonl").c_str());
    const std::string retained((std::istreambuf_iterator<char>(after)),std::istreambuf_iterator<char>());
    assert(saved==retained && access(ack.c_str(),F_OK)!=0);
  }
  arm_test_mode();qhead=qtail=qsize=0;dropped=0;
  A::Observation event=A::Observation();sink(&event,0);
  freeze_capture();sink(&event,0);
  assert(qsize==1 && dropped==0 && A::mode()==A::OBSERVE);
  assert(!pthread_mutex_lock(&queue_mu));sink(&event,0);pthread_mutex_unlock(&queue_mu);
  assert(dropped==0 && qsize==1);
  assert(pop(&event) && !pop(&event));
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
static void request_journal(bool emit) {
  namespace R=mx5::runtime::request_trace;
  A::Observation o=A::Observation();o.kind=A::Observation::POSITION;o.call_sequence=17;
  o.mono_ns=103;o.request_result=R::OK;
  R::Trace& t=o.request_trace;
  t.request.id=1;t.request.epoch=3;t.worker.id=2;t.worker.epoch=3;
  t.issue.observed_ns=101;t.reply.observed_ns=102;t.reply.type_known=true;t.reply.type=2;
  t.reply.sender=R::copy_text(":1.42");t.reply.error_name=R::copy_text("org.freedesktop.DBus.Error.ServiceUnknown");
  char line[2200];
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
  t.request.id=t.request.epoch=t.worker.id=t.worker.epoch=UINT64_MAX;
  t.issue.observed_ns=t.reply.observed_ns=UINT64_MAX;
  t.issue.bus_lifetime=t.issue.session_lifetime=t.issue.session_event=UINT64_MAX;
  t.issue.known=7;t.issue.session_state=INT32_MIN;
  t.reply.wire_serial_known=true;t.reply.wire_serial=UINT32_MAX;
  assert(format_observation(line,sizeof line,o));if(emit)puts(line);
  // Exact-size success, one byte short failure, and adjacent bytes untouched.
  char request[1800];assert(mx5::runtime::format_request_trace(request,sizeof request,R::OK,t));
  const size_t required=strlen(request)+1;
  char bounds[1802];memset(bounds,0x5a,sizeof bounds);
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
  qhead = qtail = qsize = 0;
  dropped = 0;
  A::Observation event = A::Observation();
  for (unsigned i = 0; i < 257; ++i)
    sink(&event, 0);
  assert(qsize == 256 && dropped == 1 && A::mode() == A::OBSERVE);
  A::Observation read = A::Observation();
  for (unsigned i = 0; i < 256; ++i)
    assert(pop(&read));
  assert(!pop(&read));
  arm_test_mode();
  assert(!pthread_mutex_lock(&queue_mu));
  sink(&event, 0);
  assert(!pthread_mutex_unlock(&queue_mu));
  assert(dropped == 2 && A::mode() == A::OBSERVE);
  receive_turn_tests(tmp,logs);
  stop_tests(tmp,logs);
  for(unsigned i=0;i<3;++i)unlink((logs+"/trace."+char('0'+i)+".jsonl").c_str());
  rmdir(logs.c_str());
  rmdir(tmp);
  puts("Journal tests: bounded rotation, audit failure, continued raw capture, "
       "separate rejected evidence, bounded receive turns and durable requested stop passed");
}
