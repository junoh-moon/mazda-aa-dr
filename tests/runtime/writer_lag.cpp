// Journal writer lag and write-rate harness (2026-10-07 follow-up,
// validation/WORKER_STALL_STALE_2026-10-06.md). Measurement tool, not a pass/
// fail test and not part of `make test` (it runs in real time).
//
// Replays a journal produced by build/log_rate (a synthetic drive with the
// full or persistent profile, simulated time) into the product Journal with
// its writer thread, in REAL time: each row is pushed when its simulated
// mono_ns is due (rows without one, and older RAW-window rows, at the latest
// time seen). A worker-like loop turns every 10 ms, requests a flush every
// 1 s, runs journal_lag_guard and samples journal_writer_lag. Reports the lag
// distribution, guard withdrawals, fflush calls and the process's write
// syscalls (/proc/self/io syscw/wchar). Host storage and scheduling: not the
// CMU's eMMC. The persistent profile's raw flags are not replayed (rows are
// classified by kind only); that changes no byte or timing.
#include "../../src/runtime/runtime.cpp"
#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

namespace {
struct Io { unsigned long long syscw,wchar; };
Io read_io() {
  Io io={0,0};
  std::ifstream f("/proc/self/io");std::string k;unsigned long long v;
  while(f>>k>>v) { if(k=="syscw:")io.syscw=v;else if(k=="wchar:")io.wchar=v; }
  return io;
}
uint64_t row_time(const std::string& row) {
  const size_t at=row.find("\"mono_ns\":");
  if(at==std::string::npos)return 0;
  return strtoull(row.c_str()+at+10,0,10);
}
}

int main(int argc,char** argv) {
  std::string trace,out;long max_age_ms=250;unsigned seconds=0;
  for(int i=1;i+1<argc;i+=2) {
    const std::string k=argv[i],v=argv[i+1];
    if(k=="--trace")trace=v;else if(k=="--out")out=v;
    else if(k=="--flush-max-age-ms")max_age_ms=atol(v.c_str());
    else if(k=="--seconds")seconds=unsigned(atoi(v.c_str()));
    else { fprintf(stderr,"unknown option %s\n",k.c_str());return 64; }
  }
  if(trace.empty() || out.empty() || max_age_ms<0) {
    fprintf(stderr,"usage: writer_lag --trace FILE --out DIR [--flush-max-age-ms 250 (0: request only)] [--seconds N]\n");
    return 64;
  }
  std::vector<std::string> rows;std::vector<uint64_t> times;
  {
    std::ifstream f(trace.c_str());std::string line;uint64_t latest=0,base=0;
    while(std::getline(f,line)) {
      if(line.empty())continue;
      const uint64_t t=row_time(line);
      if(t && !base)base=t;
      if(t>latest)latest=t;
      rows.push_back(line);times.push_back(latest>base?latest-base:0);
    }
  }
  if(rows.empty())return 66;
  const uint64_t span=seconds?uint64_t(seconds)*1000000000ULL:times.back()+1;
  const std::string logs=out+"/logs";
  if(mkdir(out.c_str(),0700) && errno!=EEXIST)return 73;
  if(mkdir(logs.c_str(),0700) && errno!=EEXIST)return 73;
  for(unsigned i=0;i<3;++i)unlink((logs+"/trace."+char('0'+i)+".jsonl").c_str());
  config.max_log_files=3;config.max_log_bytes=41943040;
  std::vector<uint64_t> lags;lags.reserve(size_t(span/10000000ULL)+16);
  unsigned lowered=0;size_t next=0;uint64_t pushed_bytes=0,flushes=0;
  Io before=Io(),after=Io();uint64_t elapsed=0;
  {
    Journal j(out.c_str());
    if(!j.start_writer())return 70;
    j.writer->flush_max_age_ns.store(uint64_t(max_age_ms)*1000000ULL);
    before=read_io();
    const uint64_t start=clock_ns(0);uint64_t last_flush=start;
    for(;;) {
      const uint64_t now=clock_ns(0),t=now-start;
      if(t>=span)break;
      while(next<rows.size() && times[next]<=t) { j.line(rows[next].c_str());pushed_bytes+=rows[next].size()+1;++next; }
      if(now-last_flush>=1000000000ULL) { last_flush=now;j.flush(); }
      lags.push_back(journal_writer_lag(j.writer,now).oldest_ns);
      const bool was=journal_current.load()!=0;
      journal_lag_guard(j,now);
      if(was && !journal_current.load())++lowered;
      if(j.failed)return 71;
      usleep(10000);
    }
    elapsed=clock_ns(0)-start;
    after=read_io();
    flushes=j.writer->flush_count.load();
  }
  std::vector<uint64_t> sorted(lags);std::sort(sorted.begin(),sorted.end());
  const double sec=elapsed/1e9;
  const unsigned long long writes=after.syscw-before.syscw,bytes=after.wchar-before.wchar;
  printf("{\"flush_max_age_ms\":%ld,\"seconds\":%.1f,\"rows\":%zu,\"row_bytes_per_s\":%.0f,"
         "\"lag_ms\":{\"samples\":%zu,\"p50\":%.1f,\"p99\":%.1f,\"p999\":%.1f,\"max\":%.1f},"
         "\"guard_lowered\":%u,\"fflush_per_s\":%.2f,\"write_syscalls_per_s\":%.2f,\"bytes_per_write\":%.0f}\n",
         max_age_ms,sec,next,pushed_bytes/sec,sorted.size(),
         sorted[sorted.size()/2]/1e6,sorted[size_t(sorted.size()*0.99)]/1e6,
         sorted[size_t(sorted.size()*0.999)]/1e6,sorted.back()/1e6,
         lowered,flushes/sec,writes/sec,writes?double(bytes)/double(writes):0.0);
  return 0;
}
