// G10 soak (validation/V1_C_GATES_2026-10-18.md): ONE long-lived process
// running the REAL BETA runtime worker thread, journal writer thread, motion
// socket and adapter POSITION/SEND logic with the product runtime Options
// (config mode 5, persistent log profile, session observation declined as
// with the vehicle's libpatch), driven against the wall clock by the authored
// OEM endpoint and request reader of tests/runtime/test_worker_beta.cpp
// (reused by inclusion, its main renamed). Synthetic receipt-time streams
// only: not vehicle, phone or DHU evidence.
//
//   soak-worker SECONDS [MAX_LOG_BYTES] [PARENT_DIR]
//
// Drive: 36 km/h straight, wheels every 20 ms, yaw every 100 ms, a reverse
// 1 -> 0 at start. Repeated tunnel cycles: 12 s of 2 Hz GPS fixes (an anchor
// needs 10 s), then a GPS loss of 5..29 s (5 Hz mode-0 POSITIONs, which BETA
// replaces), then the GPS returns. The journal uses a small trace ring
// (MAX_LOG_BYTES x 3 files, default 256 KiB) so rotation runs all the time.
// Every 20 ms the harness reads the journal lag guard flag (journal_current):
// a 1 -> 0 change is a guard trip (oldest unwritten row older than 1.5 s).
// Every second: heap in use (mallinfo), process RSS (/proc/self/statm: under
// qemu-user this is the emulator's), trace ring files and sizes.
// Pass: no guard trip, every tunnel after the first anchor got replaced sends,
// never more than 3 trace files and none above MAX_LOG_BYTES, heap in use at
// the end at most 1 MiB above the sample after the first 25 % of the run
// (bounded memory), no assertion failure (abort = crash = fail).
#define main mx5dr_test_worker_beta_main
#include "../runtime/test_worker_beta.cpp"
#undef main
#include <malloc.h>

namespace {
struct Sample { uint64_t t_ms; size_t heap,rss_kb; };
size_t heap_in_use() {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    const struct mallinfo m=mallinfo();
#pragma GCC diagnostic pop
    return size_t(unsigned(m.uordblks))+size_t(unsigned(m.hblkhd));
}
size_t rss_kb() {
    FILE* f=fopen("/proc/self/statm","r");size_t pages=0,res=0;
    if(f) { if(fscanf(f,"%zu %zu",&pages,&res)!=2)res=0;fclose(f); }
    return res*size_t(sysconf(_SC_PAGESIZE))/1024;
}
}

int main(int argc,char** argv) {
    setvbuf(stdout,0,_IOLBF,0);
    const unsigned seconds=argc>1?unsigned(atoi(argv[1])):600;
    const size_t max_log=argc>2?size_t(atoll(argv[2])):262144;
    const char* parent=argc>3?argv[3]:"/tmp";
    assert(seconds>=60 && max_log>=65536);
    char root[512];snprintf(root,sizeof root,"%s/mx5dr-soak-XXXXXX",parent);assert(mkdtemp(root));
    const std::string logs=std::string(root)+"/logs";
    assert(!mkdir(logs.c_str(),0700));
    config.mode=5;config.valid=true;config.max_log_bytes=max_log;config.max_log_files=3;
    config.log_profile=mx5::runtime::LOG_PROFILE_PERSISTENT;
    hook_installed=true;boot_result="ok";
    install_report.sessions_declined=true;install_report.declined_stage=3;
    A::Options options=product_options();
    options.request_reader=authored_request;options.sessions_declined=true;
    assert(A::configure(next_send,options));
    assert(A::set_mode(A::OBSERVE));
    bus_fixture::prepare();
    snprintf(motion_channel,sizeof motion_channel,"mx5dr.soak.%ld",(long)getpid());
    pthread_t worker_thread;assert(!pthread_create(&worker_thread,0,run,root));
    const char* ready[]={"\"kind\":\"shadow_boot\",\"active\":true",
        "\"kind\":\"beta_state\",\"mono_ns\"","\"to\":\"ARMED\",\"reason\":\"enabled\""};
    assert(wait_rows(logs,ready,3));
    assert(A::mode()==A::BETA);
    N::MotionSender sender;assert(sender.open_channel(motion_channel));uint64_t sequence=0;

    const unsigned FIX_MS=12000;
    unsigned tunnel=0,cycle_start=0,lost_ms=5000;
    unsigned tunnels_done=0,tunnels_replaced=0,replaced_total=0,replaced_this=0,unreplaced_after_anchor=0;
    unsigned guard_trips=0,max_files=0,size_violations=0,rotations=0;
    size_t max_size=0,prev_size0=0;
    int prev_current=journal_current.load()!=0;
    // Reserved up front: the harness's own sample log must not grow the heap it measures.
    std::vector<Sample> samples;samples.reserve(seconds+2);
    const uint64_t start=clock_ns(0);
    const unsigned end_ms=seconds*1000;
    for(unsigned ms=0;ms<=end_ms;ms+=20) {
        until(start+uint64_t(ms)*1000000ULL);
        const uint64_t now=clock_ns(0);
        for(unsigned kind=1;kind<=3;++kind) {
            if(kind==N::YAW && ms%100)continue;
            if(kind==N::REVERSE && ms!=0 && ms!=100)continue;
            N::RawEvent r=N::RawEvent();r.kind=static_cast<N::SensorKind>(kind);
            r.epoch=1;r.receive_seq=++sequence;r.received_ns=now;r.count=1;r.reverse=ms==0?1:0;
            for(unsigned i=0;i<4;++i)r.raw[i]=kind==N::WHEELS?uint16_t(lround(kmh*100+10000)):2048;
            if(!sender.send_event(r)) { fprintf(stderr,"motion send failed at %u errno=%d\n",ms,errno);abort(); }
        }
        const unsigned c=ms-cycle_start;
        if(c<FIX_MS) {
            if(c%500==0) { const bool replaced=oem_call(ms,1);assert(!replaced); }
        } else if(c<FIX_MS+lost_ms) {
            if(c%200==0 && oem_call(ms,0)) { ++replaced_this;++replaced_total; }
        } else {
            // GPS back: close this tunnel, start the next cycle (5..29 s losses).
            ++tunnels_done;
            if(replaced_this)++tunnels_replaced;else if(tunnel>0)++unreplaced_after_anchor;
            ++tunnel;replaced_this=0;cycle_start=ms;lost_ms=5000+(tunnel*7000)%25000;
            const bool replaced=oem_call(ms,1);assert(!replaced);
        }
        const int current=journal_current.load()!=0;
        if(prev_current && !current) {
            ++guard_trips;
            char load[64]="-";FILE* lf=fopen("/proc/loadavg","r");
            if(lf) { if(!fgets(load,sizeof load,lf))strcpy(load,"-");fclose(lf); }
            load[strcspn(load,"\n")]=0;
            printf("GUARD_TRIP t_ms=%u tunnel=%u loadavg=%s\n",ms,tunnel,load);
        }
        prev_current=current;
        if(ms%1000==0) {
            std::vector<Call>().swap(calls);   // the fixture's call log is the harness's, not the product's
            unsigned files=0;
            for(unsigned i=0;i<9;++i) {
                struct stat st;
                const std::string p=logs+"/trace."+char('0'+i)+".jsonl";
                if(stat(p.c_str(),&st))continue;
                ++files;
                if(size_t(st.st_size)>max_size)max_size=size_t(st.st_size);
                if(size_t(st.st_size)>max_log)++size_violations;
                if(i==0) { if(size_t(st.st_size)<prev_size0)++rotations;prev_size0=size_t(st.st_size); }
            }
            if(files>max_files)max_files=files;
            Sample s={ms,heap_in_use(),rss_kb()};samples.push_back(s);
            if(ms%60000==0)printf("SOAK t=%us heap=%zu rss_kb=%zu tunnels=%u replaced=%u guard_trips=%u files=%u rotations=%u\n",
                                  ms/1000,s.heap,s.rss_kb,tunnels_done,replaced_total,guard_trips,files,rotations);
        }
    }
    assert(!mkdir((logs+"/capture.stop").c_str(),0700));
    assert(!pthread_join(worker_thread,0));
    // Best effort: guard rows still in the retained ring.
    unsigned lag_rows=0;
    for(unsigned i=0;i<3;++i) {
        std::ifstream f((logs+"/trace."+char('0'+i)+".jsonl").c_str());std::string line;
        while(std::getline(f,line))
            if(line.find("{\"kind\":\"beta_journal_lag\"")==0 && line.find("\"event\":\"lagging\"")!=std::string::npos)++lag_rows;
    }
    const Sample first=samples.front(),last=samples.back();
    const Sample warm=samples[samples.size()/4];
    size_t heap_max=0,rss_max=0;
    for(size_t i=0;i<samples.size();++i) { heap_max=std::max(heap_max,samples[i].heap);rss_max=std::max(rss_max,samples[i].rss_kb); }
    const bool heap_ok=last.heap<=warm.heap+1048576;
    const bool ok=!guard_trips && !lag_rows && !unreplaced_after_anchor && tunnels_done>0 &&
                  max_files<=3 && !size_violations && heap_ok;
    printf("{\"soak_seconds\":%u,\"max_log_bytes\":%zu,\"tunnels\":%u,\"tunnels_replaced\":%u,"
           "\"unreplaced_after_first_anchor\":%u,\"replaced_sends\":%u,\"guard_trips\":%u,\"lag_rows_retained\":%u,"
           "\"max_trace_files\":%u,\"max_trace_bytes\":%zu,\"size_violations\":%u,\"rotations\":%u,"
           "\"heap_first\":%zu,\"heap_warm\":%zu,\"heap_end\":%zu,\"heap_max\":%zu,"
           "\"rss_kb_first\":%zu,\"rss_kb_warm\":%zu,\"rss_kb_end\":%zu,\"rss_kb_max\":%zu,\"result\":\"%s\"}\n",
           seconds,max_log,tunnels_done,tunnels_replaced,unreplaced_after_anchor,replaced_total,guard_trips,lag_rows,
           max_files,max_size,size_violations,rotations,first.heap,warm.heap,last.heap,heap_max,
           first.rss_kb,warm.rss_kb,last.rss_kb,rss_max,ok?"PASS":"FAIL");
    // Clean up the scratch tree (files the worker owns).
    DIR* d=opendir(logs.c_str());
    if(d) {
        struct dirent* e;
        while((e=readdir(d))) {
            if(!strcmp(e->d_name,".") || !strcmp(e->d_name,".."))continue;
            const std::string p=logs+"/"+e->d_name;
            if(rmdir(p.c_str()))unlink(p.c_str());
        }
        closedir(d);
    }
    rmdir(logs.c_str());rmdir(root);
    return ok?0:1;
}
