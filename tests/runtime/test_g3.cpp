// G3 record-only heading carry-over host tests (validation/G3_RECORD_ONLY_2026-10-11.md)
// and the G3 bearing send state machine (validation/G3_SEND_2026-10-13.md):
// store codec, atomic file I/O and its failure paths, the guard chain token,
// the off switch, the restore / continuity / course state machine, the store
// policy, journal rows and the pipeline's G3Motion observation. Synthetic
// inputs only; not vehicle, phone or DHU evidence.
//   test_g3                 every unit check
//   test_g3 --two-boot DIR  prints a synthetic two-boot JSONL (real tracker
//                           rows, real store file in DIR) for the analyzer test
#include "runtime/g3_heading.h"
#include "runtime/g3_send.h"
#include "navigation/pipeline.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

using namespace mx5;
using namespace mx5::runtime;
static unsigned checks;
#define CHECK(x) do { ++checks; if(!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::exit(1); } } while(0)

struct Rows {
    std::vector<std::string> lines;
    bool failed;
    Rows():failed(false) {}
    void line(const char* s) { lines.push_back(s); }
    void fail() { failed=true; }
    size_t count(const char* needle) const {
        size_t n=0;
        for(size_t i=0;i<lines.size();++i)if(lines[i].find(needle)!=std::string::npos)++n;
        return n;
    }
    std::string last(const char* needle) const {
        for(size_t i=lines.size();i>0;--i)if(lines[i-1].find(needle)!=std::string::npos)return lines[i-1];
        return std::string();
    }
};
static bool has(const std::string& s,const char* needle) { return s.find(needle)!=std::string::npos; }

static std::string temp_dir() {
    const char* base=getenv("TMPDIR");
    std::string t=std::string(base&&*base?base:"/tmp")+"/mx5dr-g3-XXXXXX";
    std::vector<char> v(t.begin(),t.end());v.push_back(0);
    CHECK(mkdtemp(&v[0]));
    return std::string(&v[0]);
}
static void write_file(const std::string& path,const std::string& data) {
    FILE* f=fopen(path.c_str(),"wb");CHECK(f);
    CHECK(fwrite(data.data(),1,data.size(),f)==data.size());
    CHECK(!fclose(f));
}
static std::string read_file(const std::string& path) {
    std::string out;FILE* f=fopen(path.c_str(),"rb");
    if(!f)return out;
    char b[512];size_t n;
    while((n=fread(b,1,sizeof b,f))>0)out.append(b,n);
    fclose(f);return out;
}
static const char BOOT_A[]="aaaaaaaa-1111-2222-3333-444444444444";
static const char BOOT_B[]="bbbbbbbb-1111-2222-3333-444444444444";
static const uint64_t RT=1760000000ULL;   // 2025-10-09, a plausible wall time

static G3Record good_record() {
    G3Record r=g3_record();
    r.seq=7;strcpy(r.boot_id,BOOT_A);r.state='S';r.flags=G3_FLAG_HEADING|G3_FLAG_POSITION;
    r.heading_cdeg=19700;r.unc_cdeg=450;r.lat_e7=355000000;r.lon_e7=1395000000;r.pos_err_dm=120;
    r.utc_s=RT;r.rt_s=RT;r.mono_ms=2230000;strcpy(r.chain,"none");
    return r;
}
static std::string encode(const G3Record& r) {
    char b[G3_RECORD_MAX];const size_t n=g3_encode(r,b,sizeof b);CHECK(n>0);
    return std::string(b,n);
}
// A line with a correct CRC over arbitrary content (for out-of-range fields).
static std::string with_crc(const std::string& body) {
    char crc[16];snprintf(crc,sizeof crc," %08lx\n",(unsigned long)g3_crc32(body.data(),body.size()));
    return body+crc;
}

static void codec() {
    // Known CRC32 check value.
    CHECK(g3_crc32("123456789",9)==0xCBF43926u);
    const G3Record r=good_record();
    const std::string line=encode(r);
    CHECK(line.size()<G3_RECORD_MAX && line[line.size()-1]=='\n');
    G3Record d;
    CHECK(g3_decode(line.data(),line.size(),&d)==G3_LOAD_OK);
    CHECK(d.version==1 && d.seq==7 && !strcmp(d.boot_id,BOOT_A) && d.state=='S' && d.flags==3);
    CHECK(d.heading_cdeg==19700 && d.unc_cdeg==450 && d.lat_e7==355000000 && d.lon_e7==1395000000);
    CHECK(d.pos_err_dm==120 && d.utc_s==RT && d.rt_s==RT && d.mono_ms==2230000 && !strcmp(d.chain,"none"));
    // Extremes and the other chain forms.
    G3Record e=good_record();e.seq=0xFFFFFFFFu;e.heading_cdeg=35999;e.unc_cdeg=18000;e.lat_e7=-900000000;
    e.lon_e7=-1800000000;strcpy(e.chain,"0123abcd");strcpy(e.boot_id,"unknown");e.state='M';e.flags=0;
    const std::string el=encode(e);
    CHECK(g3_decode(el.data(),el.size(),&d)==G3_LOAD_OK && d.seq==0xFFFFFFFFu && !strcmp(d.chain,"0123abcd") &&
          d.state=='M' && d.lon_e7==-1800000000 && !strcmp(d.boot_id,"unknown"));
    // Corruptions.
    CHECK(g3_decode("",0,&d)==G3_LOAD_EMPTY);
    std::string bad=line;bad[30]=bad[30]=='1'?'2':'1';
    CHECK(g3_decode(bad.data(),bad.size(),&d)==G3_LOAD_CHECKSUM);
    std::string crcbad=line;crcbad[crcbad.size()-2]=crcbad[crcbad.size()-2]=='0'?'1':'0';
    CHECK(g3_decode(crcbad.data(),crcbad.size(),&d)==G3_LOAD_CHECKSUM);
    CHECK(g3_decode(line.data(),line.size()-1,&d)==G3_LOAD_TRUNCATED);   // no newline
    CHECK(g3_decode(line.data(),20,&d)==G3_LOAD_TRUNCATED);              // cut mid-record
    CHECK(g3_decode("mx5",3,&d)==G3_LOAD_TRUNCATED);
    CHECK(g3_decode("xyz",3,&d)==G3_LOAD_MAGIC);
    std::string magic=line;magic[0]='M';
    CHECK(g3_decode(magic.data(),magic.size(),&d)==G3_LOAD_MAGIC);
    // Versions are reported before the checksum (the layout may differ).
    std::string old=line;old[6]='0';
    CHECK(g3_decode(old.data(),old.size(),&d)==G3_LOAD_VERSION_OLD);
    const std::string future=with_crc("mx5g3 2 1 x S 1 0 0 0 0 0 0 0 0 none");
    CHECK(g3_decode(future.data(),future.size(),&d)==G3_LOAD_VERSION_FUTURE);
    const std::string future_long=with_crc("mx5g3 12 future layout");
    CHECK(g3_decode(future_long.data(),future_long.size(),&d)==G3_LOAD_VERSION_FUTURE);
    // Correct CRC, invalid fields.
    const char* const fields[]={
        "mx5g3 1 7 aaaaaaaa-1111-2222-3333-444444444444 S 3 36000 450 0 0 0 0 0 0 none",   // heading
        "mx5g3 1 7 aaaaaaaa-1111-2222-3333-444444444444 S 3 100 18001 0 0 0 0 0 0 none",   // unc
        "mx5g3 1 7 aaaaaaaa-1111-2222-3333-444444444444 X 3 100 1 0 0 0 0 0 0 none",       // state
        "mx5g3 1 7 aaaaaaaa-1111-2222-3333-444444444444 S 8 100 1 0 0 0 0 0 0 none",       // flags
        "mx5g3 1 7 aaaaaaaa-1111-2222-3333-444444444444 S 3 100 1 0 0 0 0 0 0 ABCDEF12",   // chain case
        "mx5g3 1 7 not-a-boot-id S 3 100 1 0 0 0 0 0 0 none",                               // boot id
        "mx5g3 1 07 aaaaaaaa-1111-2222-3333-444444444444 S 3 100 1 0 0 0 0 0 0 none",      // leading zero
        "mx5g3 1 7 aaaaaaaa-1111-2222-3333-444444444444 S 3 -5 1 0 0 0 0 0 0 none",        // negative heading
        "mx5g3 1 7 aaaaaaaa-1111-2222-3333-444444444444 S 3 100 1 900000001 0 0 0 0 0 none",
        "mx5g3 1 7 aaaaaaaa-1111-2222-3333-444444444444 S 3 100 1 0 0 0 0 0 0 none extra"};
    for(size_t i=0;i<sizeof fields/sizeof fields[0];++i) {
        const std::string l=with_crc(fields[i]);
        CHECK(g3_decode(l.data(),l.size(),&d)==G3_LOAD_FORMAT);
    }
    const std::string fewer=with_crc("mx5g3 1 7 aaaaaaaa-1111-2222-3333-444444444444 S 3 100 1 0 0 0");
    CHECK(g3_decode(fewer.data(),fewer.size(),&d)==G3_LOAD_TRUNCATED);
    std::string twice=line+line;
    CHECK(g3_decode(twice.data(),twice.size(),&d)==G3_LOAD_FORMAT);
    std::string spaced=line;spaced.insert(spaced.size()-1," ");
    CHECK(g3_decode(spaced.data(),spaced.size(),&d)!=G3_LOAD_OK);
    std::string nul=line;nul[10]='\0';
    CHECK(g3_decode(nul.data(),nul.size(),&d)!=G3_LOAD_OK);
    // The encoder refuses a buffer that is too small.
    char tiny[40];CHECK(g3_encode(r,tiny,sizeof tiny)==0);
}

static void file_io() {
    const std::string dir=temp_dir();
    G3Record d;
    CHECK(g3_load(dir.c_str(),&d)==G3_LOAD_MISSING);
    const std::string line=encode(good_record());
    G3IoResult w=g3_write_atomic(dir.c_str(),line.data(),line.size());
    CHECK(w.stage==G3_IO_OK && !w.error);
    CHECK(g3_load(dir.c_str(),&d)==G3_LOAD_OK && d.heading_cdeg==19700);
    CHECK(read_file(dir+"/"+G3_STORE_NAME)==line);
    CHECK(access((dir+"/"+G3_STORE_TEMP).c_str(),F_OK)!=0);   // no temporary left
    // Overwrite with a newer record; a stale temporary from a cut write is replaced.
    write_file(dir+"/"+G3_STORE_TEMP,"partial");
    G3Record r2=good_record();r2.seq=8;r2.heading_cdeg=100;
    const std::string line2=encode(r2);
    w=g3_write_atomic(dir.c_str(),line2.data(),line2.size());
    CHECK(w.stage==G3_IO_OK);
    CHECK(g3_load(dir.c_str(),&d)==G3_LOAD_OK && d.seq==8 && d.heading_cdeg==100);
    // A symlinked temporary is unlinked, never followed.
    const std::string victim=dir+"/victim";write_file(victim,"keep");
    CHECK(!symlink(victim.c_str(),(dir+"/"+G3_STORE_TEMP).c_str()));
    w=g3_write_atomic(dir.c_str(),line.data(),line.size());
    CHECK(w.stage==G3_IO_OK && read_file(victim)=="keep");
    // Truncated / corrupt / too large / empty files on disk.
    const std::string path=dir+"/"+G3_STORE_NAME;
    write_file(path,line.substr(0,line.size()/2));CHECK(g3_load(dir.c_str(),&d)==G3_LOAD_TRUNCATED);
    std::string flipped=line;flipped[12]=flipped[12]=='1'?'2':'1';
    write_file(path,flipped);CHECK(g3_load(dir.c_str(),&d)==G3_LOAD_CHECKSUM);
    write_file(path,std::string(G3_RECORD_MAX+1,'x'));CHECK(g3_load(dir.c_str(),&d)==G3_LOAD_TOO_LARGE);
    write_file(path,"");CHECK(g3_load(dir.c_str(),&d)==G3_LOAD_EMPTY);
    // Not a regular file: a symlink (never followed) and a directory.
    CHECK(!unlink(path.c_str()));
    write_file(dir+"/real",line);CHECK(!symlink((dir+"/real").c_str(),path.c_str()));
    CHECK(g3_load(dir.c_str(),&d)==G3_LOAD_NOT_REGULAR);
    CHECK(!unlink(path.c_str()));CHECK(!mkdir(path.c_str(),0700));
    CHECK(g3_load(dir.c_str(),&d)==G3_LOAD_NOT_REGULAR);
    // Atomic rename failure (target is a non-empty directory): the temporary
    // is removed and the result names the stage.
    write_file(path+"/inside","x");
    w=g3_write_atomic(dir.c_str(),line.data(),line.size());
    CHECK(w.stage==G3_IO_RENAME && w.error!=0);
    CHECK(access((dir+"/"+G3_STORE_TEMP).c_str(),F_OK)!=0);
    CHECK(!unlink((path+"/inside").c_str()));CHECK(!rmdir(path.c_str()));
    // Path and argument errors.
    w=g3_write_atomic(0,line.data(),line.size());CHECK(w.stage==G3_IO_PATH);
    w=g3_write_atomic(dir.c_str(),line.data(),0);CHECK(w.stage==G3_IO_PATH);
    w=g3_write_atomic((dir+"/missing-subdir").c_str(),line.data(),line.size());
    CHECK(w.stage==G3_IO_OPEN && w.error==ENOENT);
    // Read-only directory: the write fails at open, the previous record stays.
    w=g3_write_atomic(dir.c_str(),line.data(),line.size());CHECK(w.stage==G3_IO_OK);
    if(geteuid()!=0) {
        CHECK(!chmod(dir.c_str(),0500));
        w=g3_write_atomic(dir.c_str(),line2.data(),line2.size());
        CHECK(w.stage==G3_IO_OPEN && (w.error==EACCES || w.error==EROFS || w.error==EPERM));
        CHECK(g3_load(dir.c_str(),&d)==G3_LOAD_OK && d.seq==7);
        CHECK(!chmod(dir.c_str(),0700));
        CHECK(!chmod(path.c_str(),0));
        CHECK(g3_load(dir.c_str(),&d)==G3_LOAD_UNREADABLE);
        CHECK(!chmod(path.c_str(),0600));
    } else std::fprintf(stderr,"test_g3: running as root, read-only directory checks skipped\n");
}

static void chain_and_switch() {
    const std::string root=temp_dir();
    char t[9];
    g3_chain_token(root.c_str(),t);CHECK(!strcmp(t,"none"));        // no guard directory
    CHECK(!mkdir((root+"/guard").c_str(),0700));
    g3_chain_token(root.c_str(),t);CHECK(!strcmp(t,"none"));
    const std::string p=root+"/"+G3_GUARD_DECISION;
    write_file(p,"mx5dr-last-decision-v1\nboot_id=ABCDEF01-2222-3333-4444-555555555555\nreason=tripped:x\n");
    g3_chain_token(root.c_str(),t);CHECK(!strcmp(t,"abcdef01"));
    write_file(p,"mx5dr-last-decision-v1\nboot_id=none\nreason=x\n");
    g3_chain_token(root.c_str(),t);CHECK(!strcmp(t,"unread"));
    write_file(p,"garbage");
    g3_chain_token(root.c_str(),t);CHECK(!strcmp(t,"unread"));
    if(geteuid()!=0) {
        write_file(p,"mx5dr-last-decision-v1\nboot_id=abcdef01-2222-3333-4444-555555555555\nreason=x\n");
        CHECK(!chmod(p.c_str(),0));
        g3_chain_token(root.c_str(),t);CHECK(!strcmp(t,"unread"));
        CHECK(!chmod(p.c_str(),0600));
    }
    CHECK(g3_switch_on(root.c_str()));
    write_file(root+"/"+G3_OFF_NAME,"");
    CHECK(!g3_switch_on(root.c_str()));
    CHECK(!unlink((root+"/"+G3_OFF_NAME).c_str()));
    CHECK(!mkdir((root+"/"+G3_OFF_NAME).c_str(),0700));           // any type switches off
    CHECK(!g3_switch_on(root.c_str()));
    CHECK(!g3_switch_on(0));
}

// ---- tracker ----------------------------------------------------------------
static const uint64_t S=1000000000ULL;
static G3Sample sample(uint64_t now,double distance=0,double turn_deg=0,double moving_s=0,
                       bool moving=false,int reverse=0) {
    G3Sample s;memset(&s,0,sizeof s);
    s.now=now;s.rt_s=RT+3*3600+now/S;
    s.heading_valid=true;s.heading_deg=200;s.unc_deg=5;
    s.position_valid=true;s.lat=35.5;s.lon=139.5;s.pos_err_m=12;
    s.motion.distance_m=distance;s.motion.turn_rad=turn_deg*G3_PI/180;s.motion.abs_turn_rad=fabs(s.motion.turn_rad);
    s.motion.moving_s=moving_s;s.motion.speed_mps=moving?5:0;
    s.motion.last_speed_ns=now;s.motion.last_moving_ns=moving?now:(now>3*S?now-3*S:0);
    s.motion.zero_since_ns=moving?0:(now>3*S?now-3*S+1:1);
    s.wheels_fresh=true;s.wheels_moving=moving;
    s.motion.first_move_ns=distance>0?1:0;s.motion.first_move_reverse=reverse;
    s.reverse=reverse;s.yaw_error_rad_s=0.002;s.rotation_per_rad=0.10;
    return s;
}
static adapter::Observation fix(uint64_t mono,double lat,double lon,uint64_t utc) {
    adapter::Observation o=adapter::Observation();o.kind=adapter::Observation::POSITION;o.mono_ns=mono;
    o.position_class=adapter::POSITION_FIX;o.original_mode=1;o.position.mode=1;o.position.utc_seconds=utc;
    o.position.latitude_deg=lat;o.position.longitude_deg=lon;o.position.velocity_kmh=0;o.position.horizontal=1;
    return o;
}
static adapter::Observation location(uint64_t mono,bool has_bearing,double bearing,double speed) {
    adapter::Observation o=adapter::Observation();o.kind=adapter::Observation::SEND;o.type=1;o.length=48;
    o.has_payload=true;o.mono_ns=mono;
    const uint32_t b=uint32_t(lround(bearing*1e6)),v=uint32_t(lround(speed*1e3));
    o.original[40]=has_bearing?1:0;o.original[32]=1;
    for(unsigned i=0;i<4;++i) { o.original[44+i]=uint8_t(b>>(8*i));o.original[36+i]=uint8_t(v>>(8*i)); }
    return o;
}
static G3Tracker started(const G3Record& r,G3Load load=G3_LOAD_OK,const char* chain="none",
                         uint64_t rt=RT+3*3600,const char* boot=BOOT_B) {
    G3Tracker t;t.start(0,true,"marker",boot,chain,rt,load,r);return t;
}

// Start-up movement check (validation/NNG_G2_G3_LESSONS_2026-10-12.md,
// validation/YAW_SATURATION_FIX_2026-10-12.md): a carried heading is rejected
// (moving_at_boot) when the boot may have missed rotation before its first
// observed motion; the yaw rail allowance grows the carried uncertainty.
static void moving_at_boot() {
    const G3Record r=good_record();
    const unsigned flags[]={navigation::G3_BOOT_FIRST_MOVING,navigation::G3_BOOT_GAP_INTO_MOTION,
                            navigation::G3_BOOT_LONG_GAP};
    for(unsigned k=0;k<3;++k) {
        G3Tracker t=started(r);CHECK(t.state()==G3_CARRIED);
        Rows j;G3Sample s=sample(2*S,3,0,1,true);s.motion.boot_motion=flags[k];s.motion.boot_gap_ns=400000000ULL;
        t.tick(j,s,0,0);
        CHECK(t.state()==G3_REJECTED && !strcmp(t.reason(),"moving_at_boot") &&
              j.count("\"event\":\"checked\",\"reason\":\"moving_at_boot\"")==1);
        char want[48];snprintf(want,sizeof want,"\"boot_motion\":%u,\"gap_ms\":400",flags[k]);
        CHECK(has(j.last("\"event\":\"checked\""),want));
        // Once rejected it stays rejected; a course only records the error.
        G3Course c={3*S,197.0,20.0,1};t.tick(j,sample(3*S,10,0,2,true),&c,0);
        CHECK(t.state()==G3_REJECTED && !strcmp(t.reason(),"moving_at_boot") &&
              j.count("\"event\":\"checked\",\"reason\":\"moving_at_boot\"")==1);
    }
    // CARRIED_UNVERIFIED is rejected too; nothing carried: no row.
    { G3Tracker t=started(r,G3_LOAD_OK,"unread");CHECK(t.state()==G3_CARRIED_UNVERIFIED);
      Rows j;G3Sample s=sample(2*S);s.motion.boot_motion=navigation::G3_BOOT_LONG_GAP;t.tick(j,s,0,0);
      CHECK(t.state()==G3_REJECTED && !strcmp(t.reason(),"moving_at_boot")); }
    { G3Tracker t=started(r,G3_LOAD_MISSING);Rows j;G3Sample s=sample(2*S);s.motion.boot_motion=1;
      t.tick(j,s,0,0);CHECK(t.state()==G3_UNKNOWN && !j.count("moving_at_boot")); }
    // A clean start keeps the carried heading (and its course confirmation).
    { G3Tracker t=started(r);Rows j;t.tick(j,sample(2*S,3,0,1,true),0,0);CHECK(t.state()==G3_CARRIED);
      G3Course c={3*S,197.0,20.0,1};t.tick(j,sample(3*S,10,0,2,true),&c,0);CHECK(t.state()==G3_CONFIRMED); }
    // Yaw rail allowance: 4.5 deg carried + 25.4 deg stays within 30 deg,
    // + 25.6 deg is rejected as unc_grew.
    { G3Tracker t=started(r);Rows j;G3Sample s=sample(2*S);s.motion.rail_excess_rad=25.4*G3_PI/180;
      t.tick(j,s,0,0);CHECK(t.state()==G3_CARRIED);
      s.motion.rail_excess_rad=25.6*G3_PI/180;t.tick(j,s,0,0);
      CHECK(t.state()==G3_REJECTED && !strcmp(t.reason(),"unc_grew")); }
}
static void restore_rules() {
    G3Record r=good_record();
    { G3Tracker t;t.start(0,false,"marker",BOOT_B,"none",RT,G3_LOAD_OK,r);
      CHECK(t.state()==G3_OFF && !strcmp(t.reason(),"marker") && !t.active() && !t.store_on());
      Rows j;t.journal_start(j,1);CHECK(j.count("\"event\":\"off\"")==1); }
    { G3Tracker t=started(r,G3_LOAD_MISSING);CHECK(t.state()==G3_UNKNOWN && !strcmp(t.reason(),"missing"));
      Rows j;t.journal_start(j,1);CHECK(j.count("\"event\":\"missing\"")==1 && t.store_on()); }
    { G3Tracker t=started(r,G3_LOAD_CHECKSUM);CHECK(t.state()==G3_UNKNOWN && !strcmp(t.reason(),"checksum"));
      Rows j;t.journal_start(j,1);CHECK(j.count("\"event\":\"rejected\",\"reason\":\"checksum\"")==1); }
    { G3Tracker t=started(r,G3_LOAD_VERSION_FUTURE);CHECK(!strcmp(t.reason(),"version_future")); }
    { G3Record m=r;m.state='M';G3Tracker t=started(m);CHECK(t.state()==G3_UNKNOWN && !strcmp(t.reason(),"moving_at_end")); }
    { G3Record m=r;m.flags=G3_FLAG_POSITION;G3Tracker t=started(m);CHECK(!strcmp(t.reason(),"no_heading")); }
    { G3Record m=r;m.unc_cdeg=3001;G3Tracker t=started(m);CHECK(t.state()==G3_UNKNOWN && !strcmp(t.reason(),"unc")); }
    { G3Record m=r;m.unc_cdeg=3000;G3Tracker t=started(m);CHECK(t.state()==G3_CARRIED); }
    { G3Tracker t=started(r);CHECK(t.state()==G3_CARRIED && !strcmp(t.reason(),"chain_ok"));
      Rows j;t.journal_start(j,1);
      const std::string l=j.last("g3_store");
      CHECK(has(l,"\"event\":\"loaded\"") && has(l,"\"h\":1970") && has(l,"\"unc\":45") &&
            has(l,"\"age_s\":10800") && has(l,"\"age_src\":\"rt\"") && has(l,"\"chain\":\"ok\"") &&
            has(l,"\"boot\":\"aaaaaaaa\"") && has(l,"\"seq\":7") && has(l,"\"g3\":\"CARRIED\""));
      char f[96];CHECK(t.boot_fields(f,sizeof f)>0 && has(f,",\"g3\":{\"state\":\"CARRIED\",\"reason\":\"chain_ok\"}")); }
    // Boot chain: a guard decline between the two boots breaks continuity.
    { G3Tracker t=started(r,G3_LOAD_OK,"12345678");CHECK(t.state()==G3_UNKNOWN && !strcmp(t.reason(),"chain_broken")); }
    { G3Record m=r;strcpy(m.chain,"12345678");G3Tracker t=started(m,G3_LOAD_OK,"12345678");CHECK(t.state()==G3_CARRIED); }
    { G3Tracker t=started(r,G3_LOAD_OK,"unread");CHECK(t.state()==G3_CARRIED_UNVERIFIED && !strcmp(t.reason(),"chain_unknown")); }
    { G3Record m=r;strcpy(m.chain,"unread");G3Tracker t=started(m);CHECK(t.state()==G3_CARRIED_UNVERIFIED); }
    { G3Tracker t=started(r,G3_LOAD_OK,"none",RT+3600,BOOT_A);
      CHECK(t.state()==G3_CARRIED_UNVERIFIED && !strcmp(t.reason(),"same_boot")); }
    // Age: stale (> 7 days), unknown wall clock, clock behind the store.
    { G3Tracker t=started(r,G3_LOAD_OK,"none",RT+G3_AGE_MAX_S+1);CHECK(t.state()==G3_UNKNOWN && !strcmp(t.reason(),"age")); }
    { G3Tracker t=started(r,G3_LOAD_OK,"none",RT+G3_AGE_MAX_S);CHECK(t.state()==G3_CARRIED); }
    { G3Tracker t=started(r,G3_LOAD_OK,"none",100);CHECK(t.state()==G3_CARRIED_UNVERIFIED && !strcmp(t.reason(),"age_unknown")); }
    { G3Tracker t=started(r,G3_LOAD_OK,"none",RT-10);CHECK(t.state()==G3_CARRIED_UNVERIFIED); }
}

static void first_fix_checks() {
    const G3Record r=good_record();
    const navigation::G3Motion still=navigation::G3Motion();
    // Unverified (no wall clock) -> the first fix nearby with a GPS age: CARRIED.
    { G3Tracker t=started(r,G3_LOAD_OK,"none",0);CHECK(t.state()==G3_CARRIED_UNVERIFIED);
      Rows j;t.position(j,fix(5*S,35.50005,139.5,RT+7200),still);
      CHECK(t.state()==G3_CARRIED && !strcmp(t.reason(),"position_ok"));
      const std::string l=j.last("\"event\":\"checked\"");
      CHECK(has(l,"\"pos\":\"ok\"") && has(l,"\"age_src\":\"gps\"") && has(l,"\"age_s\":7200") && has(l,"\"limit_m\":62.0"));
      // Only the first fix is checked.
      t.position(j,fix(6*S,36.0,139.5,RT+7201),still);CHECK(t.state()==G3_CARRIED && j.count("checked")==1); }
    // Moved while off: farther than 50 m + the stored budget.
    { G3Tracker t=started(r);Rows j;t.position(j,fix(5*S,35.505,139.5,RT+7200),still);
      CHECK(t.state()==G3_REJECTED && !strcmp(t.reason(),"moved")); }
    // ... unless the car drove that far in this boot before the first fix.
    { G3Tracker t=started(r);Rows j;navigation::G3Motion m=still;m.distance_m=600;
      t.position(j,fix(5*S,35.505,139.5,RT+7200),m);CHECK(t.state()==G3_CARRIED); }
    // GPS age beyond the limit, or before the store.
    { G3Tracker t=started(r,G3_LOAD_OK,"none",0);Rows j;t.position(j,fix(5*S,35.5,139.5,RT+G3_AGE_MAX_S+1),still);
      CHECK(t.state()==G3_REJECTED && !strcmp(t.reason(),"age")); }
    { G3Tracker t=started(r);Rows j;t.position(j,fix(5*S,35.5,139.5,RT-100),still);
      CHECK(t.state()==G3_REJECTED && !strcmp(t.reason(),"age_negative")); }
    // Only FIX-class mode 1/2 positions count (the stale no-fix record does not).
    { G3Tracker t=started(r,G3_LOAD_OK,"none",0);Rows j;adapter::Observation o=fix(5*S,35.5,139.5,0);
      o.position_class=adapter::POSITION_NO_FIX_STALE;t.position(j,o,still);
      CHECK(t.state()==G3_CARRIED_UNVERIFIED && j.lines.empty()); }
    // No stored position: age only.
    { G3Record m=r;m.flags=G3_FLAG_HEADING;G3Tracker t=started(m,G3_LOAD_OK,"none",0);Rows j;
      t.position(j,fix(5*S,10,10,RT+60),still);
      CHECK(t.state()==G3_CARRIED_UNVERIFIED && has(j.last("checked"),"\"pos\":\"no_position\"")); }
}

static void course_and_session() {
    const G3Record r=good_record();     // carried 197.0 deg, unc 4.5
    // Forward departure turning +37 deg before the first M course (234 deg).
    { G3Tracker t=started(r);Rows j;
      t.send(location(1*S,true,0,0));
      t.tick(j,sample(1*S),0,0);
      std::string l=j.last("\"event\":\"start\"");
      CHECK(has(l,"\"kind\":\"g3_first\"") && has(l,"\"carried\":[1970,45]") && has(l,"\"body\":[1970,45]") &&
            has(l,"\"stock\":[1,0,0]") && has(l,"\"d_sp\":1630") && has(l,"\"t_s\":0.0") && has(l,"\"m_s\":0.0"));
      for(unsigned k=2;k<=20;++k)t.send(location(k*S,true,k<12?90+k:0,0.3));
      t.tick(j,sample(25*S,30,37,8,true),0,0);
      CHECK(j.count("\"event\":\"t30\"")==0);
      t.tick(j,sample(31*S,40,37,10,true),0,0);
      l=j.last("\"event\":\"t30\"");
      CHECK(has(l,"\"stock_n\":20") && has(l,"\"stock_zero\":10") && has(l,"\"m_s\":40.0") && has(l,"\"body\":[2340,"));
      G3Course c={44*S,234.0,12.0,1};
      t.tick(j,sample(44*S,45,37,12,true),&c,0);
      CHECK(t.state()==G3_CONFIRMED && !strcmp(t.reason(),"course_ok") && !t.wants_course());
      l=j.last("\"event\":\"course\"");
      CHECK(has(l,"\"course\":[2340,120,1]") && has(l,"\"err_c\":-370") && has(l,"\"err_p\":0") &&
            has(l,"\"err_s\":1260") && has(l,"\"g3\":\"CONFIRMED\"") && has(l,"\"t_s\":43.0"));
      // A second course is ignored.
      t.tick(j,sample(45*S,46,37,12,true),&c,0);CHECK(j.count("\"event\":\"course\"")==1); }
    // A wrong carried heading is rejected by the course; the error is kept.
    { G3Tracker t=started(r);Rows j;G3Course c={10*S,17.0,20.0,1};
      t.tick(j,sample(10*S,20,0,4,true),&c,0);
      CHECK(t.state()==G3_REJECTED && !strcmp(t.reason(),"course_error"));
      CHECK(has(j.last("\"event\":\"course\""),"\"err_p\":-1800")); }
    // Reverse start: the carried heading is the BODY; travel = body + 180.
    { G3Tracker t=started(r);Rows j;t.send(location(1*S,true,0,0));
      t.tick(j,sample(2*S,3,0,2,true,1),0,0);
      const std::string l=j.last("\"event\":\"start\"");
      CHECK(has(l,"\"rev\":[1,1]") && has(l,"\"body\":[1970,") && has(l,"\"travel\":170")); }
    // Uncertainty growth beyond the limit before any course.
    { G3Tracker t=started(r);Rows j;G3Sample s=sample(5*S,200,10,60,true);s.motion.abs_turn_rad=4.0;
      t.tick(j,s,0,0);CHECK(t.state()==G3_REJECTED && !strcmp(t.reason(),"unc_grew") && j.count("unc_grew")==1);
      G3Course c={6*S,207.0,20.0,1};t.tick(j,sample(6*S,201,10,61,true),&c,0);
      CHECK(t.state()==G3_REJECTED && has(j.last("\"event\":\"course\""),"\"err_p\":0")); }
    // Nothing carried: the course row still records the stock error.
    { G3Tracker t=started(r,G3_LOAD_MISSING);Rows j;t.send(location(1*S,true,90,0.4));
      G3Course c={3*S,180.0,15.0,1};t.tick(j,sample(3*S,10,0,2,true),&c,0);
      const std::string l=j.last("\"event\":\"course\"");
      CHECK(t.state()==G3_UNKNOWN && has(l,"\"carried\":null") && has(l,"\"err_p\":null") && has(l,"\"err_s\":-900")); }
    // No session: t_s null; no rows for OFF.
    { G3Tracker t=started(r);Rows j;G3Course c={3*S,197.0,15.0,1};t.tick(j,sample(3*S,10,0,2,true),&c,0);
      CHECK(has(j.last("\"event\":\"course\""),"\"t_s\":null")); }
    { G3Tracker t;t.start(0,false,"marker",BOOT_B,"none",RT,G3_LOAD_OK,r);Rows j;
      t.send(location(1*S,true,0,0));t.tick(j,sample(2*S),0,0);CHECK(j.lines.empty()); }
}

struct Store {
    G3Tracker t;Rows j;std::string dir;
    std::vector<std::string> written;
    // Hands every request to the real file writer (or a forced failure).
    void pump(uint64_t now,int force_stage=-1) {
        char b[G3_RECORD_MAX];size_t n;uint32_t seq;
        if(!t.take_request(b,sizeof b,&n,&seq))return;
        uint32_t f;CHECK(t.in_flight(&f) && f==seq);
        G3IoResult r={G3_IO_OK,0,0,G3_STEP_NONE,now,now};
        if(force_stage>=0) { r.stage=force_stage;r.error=EIO;r.step=G3_STEP_RENAME; }
        else r=g3_write_atomic(dir.c_str(),b,n);
        if(r.stage==G3_IO_OK)written.push_back(std::string(b,n));
        t.store_result(j,now,seq,r);
    }
    void tick(const G3Sample& s,const char* block=0,int force_stage=-1) { t.tick(j,s,0,block);pump(s.now,force_stage); }
};
// Parked at `now` since the stop at `moving_ns` (all wheels exactly zero since
// then), `distance` m driven.
static G3Sample park(uint64_t now,double distance,uint64_t moving_ns,double heading=200) {
    G3Sample s=sample(now,distance,20,10,false);
    s.motion.last_moving_ns=moving_ns;s.motion.zero_since_ns=moving_ns;
    s.motion.first_move_ns=moving_ns?1:0;s.heading_deg=heading;
    return s;
}
static G3Sample drive(uint64_t now,double distance,double heading=200) {
    G3Sample s=sample(now,distance,20,10,true);s.heading_deg=heading;return s;
}
static bool same(const char* a,const char* b) { return a && b && !strcmp(a,b); }
static void io_rule() {
    CHECK(!g3_io_block_reason(true,false,true,true,false,0));
    CHECK(!g3_io_block_reason(true,false,true,true,false,G3_WRITER_IDLE_LAG_NS));
    CHECK(same(g3_io_block_reason(false,false,true,true,false,0),"no_store_thread"));
    CHECK(same(g3_io_block_reason(true,true,true,true,false,0),"beta_replacing"));
    CHECK(same(g3_io_block_reason(true,false,false,true,false,0),"journal_lagging"));
    CHECK(same(g3_io_block_reason(true,false,true,false,false,0),"no_writer"));
    CHECK(same(g3_io_block_reason(true,false,true,true,true,0),"journal_draining"));
    CHECK(same(g3_io_block_reason(true,false,true,true,false,G3_WRITER_IDLE_LAG_NS+1),"writer_busy"));
}
static void store_policy() {
    Store st;st.dir=temp_dir();
    st.t.start(0,true,"marker",BOOT_B,"none",RT,G3_LOAD_MISSING,g3_record());
    // Parked, never moved this boot: nothing is written, however long.
    for(unsigned k=1;k<=40;++k)st.tick(park(k*S,0,0));
    CHECK(st.written.empty() && st.t.disk_state()==0);
    // Driving: never a write while moving (no MOVING record any more).
    for(unsigned k=41;k<=60;++k)st.tick(drive(k*S,100+k));
    CHECK(st.written.empty());
    // Stopped at 60 s: nothing before 15 s parked ...
    st.tick(park(70*S,160,60*S));st.tick(park(74999*S/1000,160,60*S));
    CHECK(st.written.empty());
    // ... nothing while the idle rule blocks (BETA replacing, journal lagging) ...
    st.tick(park(75*S,160,60*S),"beta_replacing");
    st.tick(park(76*S,160,60*S),"journal_lagging");
    st.tick(park(77*S,160,60*S),"journal_draining");
    CHECK(st.written.empty());
    // ... then one parked record when idle.
    st.tick(park(78*S,160,60*S));
    CHECK(st.written.size()==1 && st.t.disk_state()=='S');
    G3Record d;CHECK(g3_load(st.dir.c_str(),&d)==G3_LOAD_OK);
    CHECK(d.state=='S' && d.flags==3 && d.heading_cdeg==20000 && d.unc_cdeg==500 && d.seq==1 &&
          !strcmp(d.boot_id,BOOT_B) && d.lat_e7==355000000 && d.pos_err_dm==120 && d.rt_s==RT+3*3600+78 &&
          d.mono_ms==78000 && !strcmp(d.chain,"none") && d.utc_s==0);
    CHECK(has(st.j.last("\"event\":\"written\""),"\"st\":\"S\",\"hv\":1,\"h\":2000,\"unc\":50"));
    CHECK(has(st.j.last("\"event\":\"written\""),"\"wait_s\":3.0") &&
          has(st.j.last("\"event\":\"written\""),"\"sync\":0"));   // 15-60 s: no fsync
    // The same stop passes 60 s: written once more, with the fsync.
    for(unsigned k=79;k<=119;++k)st.tick(park(k*S,160,60*S));
    CHECK(st.written.size()==1);
    st.tick(park(120*S,160,60*S));
    CHECK(st.written.size()==2 && has(st.j.last("\"event\":\"written\""),"\"seq\":2,") &&
          has(st.j.last("\"event\":\"written\""),"\"sync\":1"));
    // Then nothing more for this stop, even with a different heading.
    for(unsigned k=121;k<=200;++k)st.tick(park(k*S,160,60*S,k>150?205:200));
    CHECK(st.written.size()==2);
    // M1: a short parking adjustment (0.5 m) that turned the car 3 deg is stored.
    st.tick(drive(201*S,160.5,203));
    for(unsigned k=202;k<=220;++k)st.tick(park(k*S,160.5,201*S,203));
    CHECK(st.written.size()==3 && g3_load(st.dir.c_str(),&d)==G3_LOAD_OK && d.heading_cdeg==20300 && d.seq==3);
    // ... one that turned it 0.5 deg is not.
    st.tick(drive(221*S,161,203.5));
    for(unsigned k=222;k<=240;++k)st.tick(park(k*S,161,221*S,203.5));
    CHECK(st.written.size()==3);
    // A stop that needed a write but never became idle: one g3_since
    // "unstored" row with the last reason when the car moves on.
    for(unsigned k=241;k<=250;++k)st.tick(drive(k*S,300));
    for(unsigned k=251;k<=280;++k)st.tick(park(k*S,300,250*S),"beta_replacing");
    CHECK(st.written.size()==3 && st.j.count("\"event\":\"unstored\"")==0);
    st.tick(drive(281*S,301));
    { const std::string l=st.j.last("\"event\":\"unstored\"");
      CHECK(has(l,"{\"kind\":\"g3_since\"") && has(l,"\"reason\":\"beta_replacing\"") &&
            has(l,"\"m\":140.5") && has(l,"\"rot\":0") && has(l,"\"age_s\":"));
      // The start of motion after a stored park (the M1 park at 202-220 s)
      // left one "left" row.
      CHECK(st.j.count("\"event\":\"left\"")>=2); }
    // Heading not valid at a parked stop after moving and the yaw chain
    // broken since the last valid BETA heading: STOPPED without heading.
    G3Sample nohead=park(300*S,320,282*S);nohead.heading_valid=false;nohead.position_valid=false;
    nohead.motion.breaks=1;
    st.tick(nohead);
    CHECK(g3_load(st.dir.c_str(),&d)==G3_LOAD_OK && d.state=='S' && d.flags==0);
    // Failures: retried within the same stop after the rate limit; three in a
    // row disable the store for the boot.
    const size_t before=st.written.size();
    st.tick(drive(301*S,400));
    st.tick(park(320*S,400,301*S),0,G3_IO_OPEN);
    CHECK(has(st.j.last("\"event\":\"failed\""),"\"stage\":\"open\",\"errno\":5") && st.t.store_on());
    st.tick(park(322*S,400,301*S),0,G3_IO_OPEN);          // rate limit: no attempt
    CHECK(st.j.count("\"event\":\"failed\"")==1);
    st.tick(park(326*S,400,301*S),0,G3_IO_RENAME);
    st.tick(park(332*S,400,301*S),0,G3_IO_FSYNC);
    CHECK(!st.t.store_on() && has(st.j.last("\"event\":\"failed\""),"\"disabled\":true"));
    st.tick(drive(333*S,500));st.tick(park(350*S,500,333*S));st.tick(drive(351*S,510));
    CHECK(st.written.size()==before && has(st.j.last("\"event\":\"unstored\""),"\"reason\":\"store_off\""));
    CHECK(!st.j.failed);   // never j.fail()
    // A cancelled write (the idle rule failed during it) replaces nothing,
    // is not a failure and the same stop retries; seq continues from the
    // restored record. A stopped write ends writes for the boot.
    { Store s2;s2.dir=temp_dir();s2.t.start(0,true,"m",BOOT_B,"none",RT,G3_LOAD_OK,good_record());
      CHECK(s2.t.disk_state()=='S');
      s2.tick(drive(1*S,5));s2.tick(park(17*S,5,1*S),0,G3_IO_CANCELLED);
      const std::string c=s2.j.last("\"event\":\"cancelled\"");
      CHECK(has(c,"\"reason\":\"cancelled\",\"at\":\"rename\"") && has(c,"\"seq\":8") && has(c,"\"start_ns\":"));
      CHECK(s2.written.empty() && s2.t.store_on());
      s2.tick(park(23*S,5,1*S),0,G3_IO_STALE);
      CHECK(has(s2.j.last("\"event\":\"cancelled\""),"\"reason\":\"stale\"") && s2.t.store_on());
      s2.tick(park(29*S,5,1*S));
      CHECK(s2.written.size()==1 && has(s2.j.last("\"event\":\"written\""),"\"seq\":10"));
      s2.tick(drive(30*S,20));s2.tick(park(50*S,20,31*S),0,G3_IO_STOPPED);
      CHECK(!s2.t.store_on() && has(s2.j.last("\"event\":\"cancelled\""),"\"disabled\":true")); }
    // The restored heading counts for M1: parked again after < 2 m with the
    // same heading as the stored record (197.0) is not rewritten.
    { Store s4;s4.dir=temp_dir();s4.t.start(0,true,"m",BOOT_B,"none",RT,G3_LOAD_OK,good_record());
      s4.tick(drive(1*S,1,197.0));for(unsigned k=2;k<=20;++k)s4.tick(park(k*S,1,1*S,197.0));
      CHECK(s4.written.empty()); }
    // A refused mailbox post leaves nothing in flight; the stop retries.
    { Store s5;s5.dir=temp_dir();s5.t.start(0,true,"m",BOOT_B,"none",RT,G3_LOAD_MISSING,g3_record());
      s5.t.tick(s5.j,drive(1*S,10),0,0);s5.t.tick(s5.j,park(17*S,10,1*S),0,0);
      char b[G3_RECORD_MAX];size_t n;uint32_t q,f;
      CHECK(s5.t.take_request(b,sizeof b,&n,&q) && s5.t.in_flight(&f));
      s5.t.post_failed();CHECK(!s5.t.in_flight(&f));
      s5.t.tick(s5.j,park(18*S,10,1*S),0,0);CHECK(!s5.t.take_request(b,sizeof b,&n,&q));   // rate limit
      s5.t.tick(s5.j,park(23*S,10,1*S),0,0);CHECK(s5.t.take_request(b,sizeof b,&n,&q)); }
    // No store thread: the store is off for the boot, with one row.
    { G3Tracker t;t.start(0,true,"m",BOOT_B,"none",RT,G3_LOAD_MISSING,g3_record());Rows j;
      t.store_unavailable(j,1,"no_store_thread");t.store_unavailable(j,2,"no_store_thread");
      CHECK(!t.store_on() && j.count("\"event\":\"unavailable\",\"reason\":\"no_store_thread\"")==1);
      t.tick(j,drive(5*S,10),0,0);t.tick(j,park(25*S,10,5*S),0,0);
      char b[G3_RECORD_MAX];size_t n;uint32_t q;CHECK(!t.take_request(b,sizeof b,&n,&q)); }
    // A stale result (unknown sequence) is ignored.
    { Store s3;s3.dir=temp_dir();s3.t.start(0,true,"m",BOOT_B,"none",RT,G3_LOAD_OK,good_record());
      const G3IoResult ok={G3_IO_OK,0,0,G3_STEP_NONE,0,0};
      s3.t.store_result(s3.j,1,99,ok);CHECK(s3.j.lines.empty()); }
}

// G3_FIRST_DRIVE_2026-10-11: the stored heading is the BETA body heading
// carried by the G3Motion yaw integral while BETA is unseeded (cadence fence)
// or its budget grows faster than the carried one; a yaw chain break or a boot
// that was never seeded stores no heading.
static G3Sample moved(uint64_t now,double distance,double turn_deg,double aturn_deg,double moving_s,bool moving,
                      bool beta,double h=100,double unc=4,uint32_t breaks=0) {
    G3Sample s=sample(now,distance,turn_deg,moving_s,moving);
    s.motion.abs_turn_rad=aturn_deg*G3_PI/180;s.motion.breaks=breaks;
    s.heading_valid=beta;s.heading_deg=h;s.unc_deg=unc;
    return s;
}
static G3Sample parked(G3Sample s,uint64_t stop_ns) {
    s.motion.last_moving_ns=stop_ns;s.motion.zero_since_ns=stop_ns;s.motion.first_move_ns=1;return s;
}
static void carried_reference() {
    // Never seeded this boot: parked records carry no heading.
    { Store st;st.dir=temp_dir();st.t.start(0,true,"m",BOOT_B,"none",RT,G3_LOAD_MISSING,g3_record());
      st.tick(moved(1*S,50,40,40,10,true,false));
      st.tick(parked(moved(20*S,60,40,40,12,false,false),2*S));
      CHECK(st.written.size()==1 && has(st.j.last("\"event\":\"written\""),"\"hv\":0,\"h\":0,\"unc\":0") &&
            has(st.j.last("\"event\":\"written\""),"\"ref_s\":null,\"beta\":null")); }
    Store st;st.dir=temp_dir();st.t.start(0,true,"m",BOOT_B,"none",RT,G3_LOAD_MISSING,g3_record());
    // BETA valid at 100.0 deg +- 4 at turn 0; then unseeded (fence) while the
    // car turns +30 deg (|30| deg, 20 s moving): stored 130.0 +- (4 + (0.002*20
    // + 0.10*pi/6) rad) = 9.29 deg.
    st.tick(moved(1*S,10,0,0,0,true,true,100,4));
    st.tick(moved(2*S,40,30,30,20,true,false));
    st.tick(parked(moved(20*S,60,30,30,20,false,false),3*S));
    G3Record d;
    CHECK(st.written.size()==1 && g3_load(st.dir.c_str(),&d)==G3_LOAD_OK && d.flags==3 && d.heading_cdeg==13000 &&
          d.unc_cdeg==929);
    { const std::string w=st.j.last("\"event\":\"written\"");
      CHECK(has(w,"\"hv\":1,\"h\":1300,\"unc\":93") && has(w,"\"ref_s\":19.0,\"beta\":null")); }
    // BETA valid again with a larger (standstill-inflated) budget that agrees:
    // the carried value is kept and BETA's own value is journaled.
    st.tick(moved(21*S,70,40,40,25,true,true,141,40));
    st.tick(parked(moved(40*S,80,40,40,25,false,true,141,45),22*S));
    CHECK(st.written.size()==2 && g3_load(st.dir.c_str(),&d)==G3_LOAD_OK && d.heading_cdeg==14000 &&
          d.unc_cdeg>929 && d.unc_cdeg<1100);
    CHECK(has(st.j.last("\"event\":\"written\""),"\"beta\":[1410,450]"));
    // BETA disagrees beyond both bounds: BETA is adopted.
    st.tick(moved(41*S,90,40,40,25,true,true,200,20));
    st.tick(parked(moved(60*S,100,40,40,25,false,true,200,25),42*S));
    CHECK(st.written.size()==3 && g3_load(st.dir.c_str(),&d)==G3_LOAD_OK && d.heading_cdeg==20000 &&
          d.unc_cdeg==2000);
    // A smaller BETA budget is adopted (and later standstill growth is not).
    st.tick(moved(61*S,110,40,40,25,true,true,203,3));
    st.tick(parked(moved(80*S,120,40,40,25,false,true,203,30),62*S));
    CHECK(st.written.size()==4 && g3_load(st.dir.c_str(),&d)==G3_LOAD_OK && d.heading_cdeg==20300 &&
          d.unc_cdeg==300);
    // A yaw chain break while BETA is unseeded: no heading until BETA again.
    st.tick(moved(81*S,130,50,50,30,true,false,0,0,1));
    st.tick(parked(moved(100*S,140,50,50,30,false,false,0,0,1),82*S));
    CHECK(st.written.size()==5 && g3_load(st.dir.c_str(),&d)==G3_LOAD_OK && d.flags==2);
    st.tick(moved(101*S,150,50,50,30,true,true,10,6,1));
    st.tick(moved(102*S,160,60,60,31,true,false,0,0,1));
    st.tick(parked(moved(120*S,170,60,60,31,false,false,0,0,1),103*S));
    CHECK(st.written.size()==6 && g3_load(st.dir.c_str(),&d)==G3_LOAD_OK && d.flags==3 && d.heading_cdeg==2000);
    CHECK(!st.j.failed);
}

// fsync only for long stops (third review): the real writer skips the fsync
// step (and its hook) for sync=false; the tracker writes at 15 s without and
// at 60 s with fsync, or once with fsync when the stop was blocked until 60 s.
// Rule F v3 (validation/REVERSE_GATE_ASSUMED_FORWARD_2026-10-11.md): a BETA
// seed made under the assumed forward state was discarded (G3Motion::discards
// and a yaw-chain break). A record written in this boot with a heading built
// before the discard is rewritten at the next parked tick without it; a
// pending request with such a heading is dropped and requested again.
static G3Sample discarded(G3Sample s,uint32_t n) { s.motion.discards=n;s.motion.breaks+=n;return s; }
static void rule_f_discard() {
    { Store st;st.dir=temp_dir();st.t.start(0,true,"m",BOOT_B,"none",RT,G3_LOAD_MISSING,g3_record());
      // BETA (seeded under the assumption) says 100 deg; parked: written with it.
      st.tick(moved(1*S,50,0,0,10,true,true,100,4));
      st.tick(parked(moved(20*S,60,0,0,12,false,true,100,4),2*S));
      G3Record d;
      CHECK(st.written.size()==1 && g3_load(st.dir.c_str(),&d)==G3_LOAD_OK && d.flags==3 && d.heading_cdeg==10000);
      // The real reverse 1 arrives while parked: discard. The same stop
      // rewrites the record without a heading (after the 5 s rate limit).
      st.tick(discarded(parked(moved(21*S,60,0,0,12,false,false),2*S),1));
      st.tick(discarded(parked(moved(26*S,60,0,0,12,false,false),2*S),1));
      CHECK(st.written.size()==2 && g3_load(st.dir.c_str(),&d)==G3_LOAD_OK && d.flags==2);
      // Nothing more for this stop.
      st.tick(discarded(parked(moved(40*S,60,0,0,12,false,false),2*S),1));
      CHECK(st.written.size()==2); }
    { G3Tracker t;t.start(0,true,"m",BOOT_B,"none",RT,G3_LOAD_MISSING,g3_record());Rows j;
      t.tick(j,moved(1*S,50,0,0,10,true,true,100,4),0,0);
      t.tick(j,parked(moved(20*S,60,0,0,12,false,true,100,4),2*S),0,0);   // request pending, not handed over
      t.tick(j,discarded(parked(moved(21*S,60,0,0,12,false,false),2*S),1),0,0);
      char b[G3_RECORD_MAX];size_t n;uint32_t q;
      CHECK(!t.take_request(b,sizeof b,&n,&q));                        // dropped
      t.tick(j,discarded(parked(moved(26*S,60,0,0,12,false,false),2*S),1),0,0);
      CHECK(t.take_request(b,sizeof b,&n,&q));
      G3IoResult ok={G3_IO_OK,0,0,G3_STEP_NONE,0,0};t.store_result(j,26*S,q,ok);
      CHECK(has(j.last("\"event\":\"written\""),"\"hv\":0")); }
}
static unsigned hook_calls;
static void count_hook() { ++hook_calls; }
static void fsync_policy() {
    const std::string dir=temp_dir();const std::string line=encode(good_record());
    hook_calls=0;
    G3IoResult w=g3_write_atomic(dir.c_str(),line.data(),line.size(),0,0,count_hook,false);
    CHECK(w.stage==G3_IO_OK && hook_calls==0);
    w=g3_write_atomic(dir.c_str(),line.data(),line.size(),0,0,count_hook,true);
    CHECK(w.stage==G3_IO_OK && hook_calls==1);
    // Tracker: request flags.
    { G3Tracker t;t.start(0,true,"m",BOOT_B,"none",RT,G3_LOAD_MISSING,g3_record());Rows j;
      t.tick(j,drive(1*S,50),0,0);
      t.tick(j,park(17*S,50,1*S),0,0);
      char b[G3_RECORD_MAX];size_t n;uint32_t q;bool sync=true;
      CHECK(t.take_request(b,sizeof b,&n,&q,&sync) && !sync);
      G3IoResult ok={G3_IO_OK,0,0,G3_STEP_NONE,0,0};t.store_result(j,17*S,q,ok);
      t.tick(j,park(60*S,50,1*S),0,0);CHECK(!t.take_request(b,sizeof b,&n,&q,&sync));   // 59 s
      t.tick(j,park(61*S,50,1*S),0,0);CHECK(t.take_request(b,sizeof b,&n,&q,&sync) && sync);
      // A cancelled fsync write retries in the same stop.
      G3IoResult c={G3_IO_CANCELLED,0,0,G3_STEP_RENAME,0,0};t.note_cancel("wheels_stale");t.store_result(j,61*S,q,c);
      CHECK(has(j.last("\"event\":\"cancelled\""),"\"why\":\"wheels_stale\"") &&
            has(j.last("\"event\":\"cancelled\""),"\"sync\":1"));
      t.tick(j,park(67*S,50,1*S),0,0);CHECK(t.take_request(b,sizeof b,&n,&q,&sync) && sync);
      t.store_result(j,67*S,q,ok);
      t.tick(j,park(200*S,50,1*S),0,0);CHECK(!t.take_request(b,sizeof b,&n,&q,&sync)); }
    // Blocked from 15 s to 60 s: one write, with the fsync.
    { G3Tracker t;t.start(0,true,"m",BOOT_B,"none",RT,G3_LOAD_MISSING,g3_record());Rows j;
      t.tick(j,drive(1*S,50),0,0);
      for(unsigned k=16;k<=61;++k)t.tick(j,park(k*S,50,1*S),0,"beta_replacing");
      char b[G3_RECORD_MAX];size_t n;uint32_t q;bool sync=false;
      CHECK(!t.take_request(b,sizeof b,&n,&q,&sync));
      t.tick(j,park(62*S,50,1*S),0,0);CHECK(t.take_request(b,sizeof b,&n,&q,&sync) && sync); }
}
// Park detection (review v2, Codex MEDIUM): exact zero, fresh stream.
static void park_rules() {
    // A crawl (a wheel above exactly zero) never parks, however long.
    { Store st;st.dir=temp_dir();st.t.start(0,true,"m",BOOT_B,"none",RT,G3_LOAD_MISSING,g3_record());
      st.tick(drive(1*S,30));
      for(unsigned k=2;k<=60;++k) { G3Sample c=drive(k*S,30+0.02*k);c.motion.speed_mps=0.02;st.tick(c); }
      CHECK(st.written.empty() && st.j.count("g3_since")==0); }
    // A stale wheel stream blocks parking even after 15 s of zero samples.
    { Store st;st.dir=temp_dir();st.t.start(0,true,"m",BOOT_B,"none",RT,G3_LOAD_MISSING,g3_record());
      st.tick(drive(1*S,30));
      G3Sample stale=park(30*S,30,2*S);stale.wheels_fresh=false;st.tick(stale);
      CHECK(st.written.empty());
      st.tick(park(31*S,30,2*S));CHECK(st.written.size()==1); }
}
static unsigned hook_ms;
static G3Mailbox* hook_mailbox;
static int hook_action;   // 0 nothing, 1 cancel during the fsync step, 2 stop during it
static void slow_fsync() {
    struct timespec t={time_t(hook_ms/1000),long(hook_ms%1000)*1000000L};nanosleep(&t,0);
    if(hook_action==1)hook_mailbox->cancel_gen.fetch_add(1);
    if(hook_action==2)hook_mailbox->stop.store(1);
}
static int gate_step;
static int gate_result;
static int test_gate(void*,int step) { return step==gate_step?gate_result:G3_IO_OK; }
static void gated_write() {
    // Every gated step: a refusal removes the temporary file and never
    // replaces the previous record.
    const int steps[]={G3_STEP_OPEN,G3_STEP_FSYNC,G3_STEP_RENAME};
    for(unsigned i=0;i<3;++i)for(unsigned k=0;k<2;++k) {
        const std::string dir=temp_dir();
        const std::string old=encode(good_record());
        CHECK(g3_write_atomic(dir.c_str(),old.data(),old.size()).stage==G3_IO_OK);
        G3Record r2=good_record();r2.seq=9;const std::string line=encode(r2);
        gate_step=steps[i];gate_result=k?G3_IO_STOPPED:G3_IO_CANCELLED;
        const G3IoResult w=g3_write_atomic(dir.c_str(),line.data(),line.size(),test_gate,0);
        CHECK(w.stage==gate_result && w.step==steps[i] && w.end_ns>=w.start_ns);
        CHECK(access((dir+"/"+G3_STORE_TEMP).c_str(),F_OK)!=0);
        G3Record d;CHECK(g3_load(dir.c_str(),&d)==G3_LOAD_OK && d.seq==7);
    }
    // No directory fsync: exactly the file write (checked by the result).
    const std::string dir=temp_dir();const std::string line=encode(good_record());
    gate_step=-1;CHECK(g3_write_atomic(dir.c_str(),line.data(),line.size(),test_gate,0).stage==G3_IO_OK);
}
// The dedicated store thread: an fsync step blocked for 3 s never delays the
// worker side; a cancel or stop during it prevents the rename; a request
// picked up late is dropped; a result of another worker epoch is ignored.
static uint64_t mono() { return g3_clock_ns(CLOCK_MONOTONIC); }
static bool wait_result(G3Mailbox& m,uint32_t seq,uint32_t epoch,G3IoResult* r,uint64_t* worst) {
    const uint64_t t0=mono();
    while(mono()-t0<10*S) {
        const uint64_t a=mono();
        const bool done=g3_mailbox_result(m,seq,epoch,r);
        const uint64_t b=mono();if(worst && b-a>*worst)*worst=b-a;
        if(done)return true;
        struct timespec t={0,10000000};nanosleep(&t,0);
    }
    return false;
}
// Rule F v4: a taint end (G3Motion::discards) while a write with a heading
// built before it is in flight. The worker adds discard_cancel() to its
// cancel reasons (runtime.cpp g3_pump); a taint end that arrives after the
// temporary write (here inside the fsync step) prevents the rename: the
// previous record (none) stays, and the stop requests again without heading.
static G3Tracker* inflight_tracker;
static Rows* inflight_rows;
static G3Mailbox* inflight_mailbox;
static void discard_during_fsync() {
    const G3Sample s=discarded(parked(moved(21*S,60,0,0,12,false,false),2*S),1);
    inflight_tracker->tick(*inflight_rows,s,0,0);
    if(const char* why=inflight_tracker->discard_cancel(s)) {
        inflight_tracker->note_cancel(why);inflight_mailbox->cancel_gen.fetch_add(1);
    }
}
static void rule_f_discard_in_flight() {
    static G3Mailbox m;
    const std::string dir=temp_dir();
    CHECK(g3_mailbox_init(m,dir.c_str(),discard_during_fsync));
    G3Tracker t;t.start(0,true,"m",BOOT_B,"none",RT,G3_LOAD_MISSING,g3_record());Rows j;
    inflight_tracker=&t;inflight_rows=&j;inflight_mailbox=&m;
    t.tick(j,moved(1*S,50,0,0,10,true,true,100,4),0,0);
    t.tick(j,parked(moved(20*S,60,0,0,12,false,true,100,4),2*S),0,0);
    char b[G3_RECORD_MAX];size_t n;uint32_t q;bool sync=false;
    CHECK(t.take_request(b,sizeof b,&n,&q,&sync));
    const uint64_t now=mono();
    CHECK(g3_mailbox_post(m,b,n,q,1,now,true));
    CHECK(g3_mailbox_run_once(m,now));                 // temp write, fsync (taint end), rename gated
    G3IoResult r;CHECK(g3_mailbox_result(m,q,1,&r));
    CHECK(r.stage==G3_IO_CANCELLED && r.step==G3_STEP_RENAME);
    t.store_result(j,21*S,q,r);
    CHECK(has(j.last("\"event\":\"cancelled\""),"\"why\":\"rule_f_discard\""));
    G3Record d;CHECK(g3_load(dir.c_str(),&d)==G3_LOAD_MISSING);
    t.tick(j,discarded(parked(moved(26*S,60,0,0,12,false,false),2*S),1),0,0);
    CHECK(t.take_request(b,sizeof b,&n,&q));
    G3IoResult ok={G3_IO_OK,0,0,G3_STEP_NONE,0,0};t.store_result(j,26*S,q,ok);
    CHECK(has(j.last("\"event\":\"written\""),"\"hv\":0"));
}
static void store_thread() {
    static G3Mailbox m;
    const std::string dir=temp_dir();
    CHECK(g3_mailbox_init(m,dir.c_str(),slow_fsync));m.poll_ns=20000000ULL;
    hook_mailbox=&m;hook_ms=3000;hook_action=0;
    pthread_t th;CHECK(!pthread_create(&th,0,g3_store_main,&m));
    const std::string line=encode(good_record());
    G3IoResult r;uint64_t worst=0;
    CHECK(!g3_mailbox_post(m,line.data(),line.size(),0,1,mono()));        // seq 0 is never valid
    uint64_t t0=mono();
    CHECK(g3_mailbox_post(m,line.data(),line.size(),7,1,t0));
    CHECK(mono()-t0<50000000ULL && !g3_mailbox_idle(m));
    CHECK(!g3_mailbox_post(m,line.data(),line.size(),8,1,mono()));        // one slot only
    CHECK(wait_result(m,7,1,&r,&worst));
    CHECK(r.stage==G3_IO_OK && r.latency_ns>=2900000000ULL && mono()-t0>=2900000000ULL);
    CHECK(worst<5000000ULL);   // polling never blocks on the slow write
    for(unsigned k=0;k<100 && !g3_mailbox_idle(m);++k) { struct timespec t={0,1000000};nanosleep(&t,0); }
    CHECK(g3_mailbox_idle(m) && m.runs.load()==1);
    G3Record d;CHECK(g3_load(dir.c_str(),&d)==G3_LOAD_OK && d.seq==7);
    // ABA: the same seq from another worker epoch is not this tracker's result.
    CHECK(!g3_mailbox_result(m,7,2,&r));
    // A cancel during the blocked fsync: no rename (the old record stays).
    G3Record r8=good_record();r8.seq=8;const std::string l8=encode(r8);
    hook_action=1;hook_ms=500;
    CHECK(g3_mailbox_post(m,l8.data(),l8.size(),8,1,mono()));
    CHECK(wait_result(m,8,1,&r,0) && r.stage==G3_IO_CANCELLED && r.step==G3_STEP_RENAME);
    CHECK(g3_load(dir.c_str(),&d)==G3_LOAD_OK && d.seq==7 && access((dir+"/"+G3_STORE_TEMP).c_str(),F_OK)!=0);
    // A request picked up later than the stale limit: dropped without I/O.
    hook_action=0;
    for(unsigned k=0;k<100 && !g3_mailbox_idle(m);++k) { struct timespec t={0,1000000};nanosleep(&t,0); }
    CHECK(g3_mailbox_post(m,l8.data(),l8.size(),9,1,mono()-G3_STORE_STALE_NS-1));
    CHECK(wait_result(m,9,1,&r,0) && r.stage==G3_IO_STALE && r.step==G3_STEP_NONE);
    CHECK(g3_load(dir.c_str(),&d)==G3_LOAD_OK && d.seq==7);
    // A stop during the blocked fsync: no rename after the stop, and the
    // thread ends without being waited for longer than its current step.
    for(unsigned k=0;k<100 && !g3_mailbox_idle(m);++k) { struct timespec t={0,1000000};nanosleep(&t,0); }
    hook_action=2;hook_ms=500;
    CHECK(g3_mailbox_post(m,l8.data(),l8.size(),10,1,mono()));
    CHECK(wait_result(m,10,1,&r,0) && r.stage==G3_IO_STOPPED && r.step==G3_STEP_RENAME);
    CHECK(g3_load(dir.c_str(),&d)==G3_LOAD_OK && d.seq==7 && access((dir+"/"+G3_STORE_TEMP).c_str(),F_OK)!=0);
    CHECK(!g3_mailbox_post(m,l8.data(),l8.size(),11,1,mono()));           // stopped: no new work
    CHECK(!pthread_join(th,0));
}

// End to end through real files: boot A writes at its parked stop, boot B restores.
static void two_boot_roundtrip() {
    const std::string dir=temp_dir();
    Store a;a.dir=dir;a.t.start(0,true,"m",BOOT_A,"none",RT,G3_LOAD_MISSING,g3_record());
    Rows ja;adapter::Observation f=fix(2*S,35.5,139.5,RT+2);a.t.position(ja,f,navigation::G3Motion());
    a.tick(drive(10*S,500,90));
    G3Sample stop=park(30*S,520,11*S,197.3);stop.unc_deg=4.4;
    a.tick(stop);
    G3Record d;CHECK(g3_load(dir.c_str(),&d)==G3_LOAD_OK && d.heading_cdeg==19730 && d.utc_s==RT+30);
    // Next boot, 3 h later, chain unchanged.
    G3Tracker b;b.start(0,true,"m",BOOT_B,"none",RT+30+3*3600,g3_load(dir.c_str(),&d),d);
    CHECK(b.state()==G3_CARRIED && !strcmp(b.reason(),"chain_ok"));
    // Stale (8 days) and a guard decline in between.
    G3Tracker c;c.start(0,true,"m",BOOT_B,"none",RT+8*24*3600,G3_LOAD_OK,d);CHECK(!strcmp(c.reason(),"age"));
    G3Tracker e;e.start(0,true,"m",BOOT_B,"0badc0de",RT+3600,G3_LOAD_OK,d);CHECK(!strcmp(e.reason(),"chain_broken"));
}

// ---- pipeline observation (G3Motion) --------------------------------------
static uint64_t T(unsigned ms) { return 1000000000ULL+uint64_t(ms)*1000000ULL; }
static navigation::RawEvent raw(navigation::SensorKind k,unsigned ms,uint64_t seq) {
    navigation::RawEvent r=navigation::RawEvent();r.kind=k;r.epoch=1;r.receive_seq=seq;r.received_ns=T(ms);
    r.count=1;return r;
}
static void pipeline_motion() {
    navigation::Pipeline p;mx5_dr_context x={1,1,1};
    CHECK(p.init_model(navigation::research_model_profile(),mx5_dr_default_config(),x,false,false,true));
    // Without BETA nothing is observed.
    CHECK(p.g3_motion().distance_m==0 && p.g3_motion().first_move_reverse==-1);
    CHECK(p.enable_beta(beta_profile_tunnel()));
    const uint32_t breaks0=p.g3_motion().breaks;
    uint64_t seq=0;
    // Reverse 1 at boot (latched), 3 s standstill with a large yaw offset
    // (must not integrate), then 5 s at 18 km/h turning at -0.1 rad/s raw...
    const double rate=0.1;   // body rad/s, clockwise positive
    const unsigned yaw_turn=unsigned(lround(2048-rate/0.000658615));
    for(unsigned ms=0;ms<=8000;ms+=100) {
        if(ms==0) { navigation::RawEvent r=raw(navigation::REVERSE,ms,++seq);r.reverse=1;CHECK(p.enqueue_raw(r)==navigation::PIPELINE_OK); }
        const bool moving=ms>=3000;
        navigation::RawEvent w=raw(navigation::WHEELS,ms,++seq);
        for(unsigned i=0;i<4;++i)w.raw[i]=uint16_t(moving?lround(18*100+10000):10000);
        CHECK(p.enqueue_raw(w)==navigation::PIPELINE_OK);
        navigation::RawEvent y=raw(navigation::YAW,ms,++seq);y.raw[0]=uint16_t(moving?yaw_turn:2100);
        const navigation::PipelineResult yr=p.enqueue_raw(y);
        CHECK(yr==navigation::PIPELINE_OK||yr==navigation::PIPELINE_WAITING);
        if(ms>=100)p.drain(T(ms)-100000000ULL);
        if(ms==2900) {
            const navigation::G3Motion& m=p.g3_motion();
            CHECK(m.distance_m==0 && m.turn_rad==0 && m.moving_s==0 && !m.first_move_ns && m.last_speed_ns);
            CHECK(m.zero_since_ns==T(0));   // one gap-free exact-zero run since boot
        }
    }
    const navigation::G3Motion& m=p.g3_motion();
    // Drained through about 7.9 s: about 5 s of motion at 5 m/s, 0.1 rad/s.
    CHECK(m.first_move_ns==T(3000) && m.first_move_reverse==1);
    CHECK(m.distance_m>23 && m.distance_m<25.5);
    // The fixed BETA zero (2048) and the profile scale give the quantized rate.
    const double applied=(double(yaw_turn)-2048.0)*-0.000658615;
    CHECK(fabs(applied-rate)<0.001);
    CHECK(fabs(m.turn_rad-applied*m.moving_s)<1e-6 && m.moving_s>4.5 && m.moving_s<5.0+1e-9);
    CHECK(fabs(m.abs_turn_rad-m.turn_rad)<1e-9 && fabs(m.speed_mps-5.0)<1e-9 && m.last_moving_ns==m.last_speed_ns);
    CHECK(!m.zero_since_ns);
    CHECK(m.breaks==breaks0);   // contiguous 100 ms windows: no break
    // A reset does not clear the boot-lifetime observation but breaks the
    // yaw chain (G3_FIRST_DRIVE_2026-10-11).
    const uint32_t breaks=m.breaks;
    const double before=m.distance_m;p.reset(x);CHECK(p.g3_motion().distance_m==before);
    CHECK(p.g3_motion().breaks==breaks+1);
    // g3_sample on an unseeded BETA core: motion only, no heading.
    const G3Sample s=g3_sample(p,T(9000),RT);
    CHECK(!s.heading_valid && !s.position_valid && s.motion.distance_m==before && s.yaw_error_rad_s==0.002 &&
          s.rotation_per_rad==0.10);
    uint64_t seen=0;G3Course c;CHECK(!g3_find_course(p,&seen,&c));
}
// The pipeline's start-up movement observation (G3Motion::boot_motion).
// wheel(ms): km/h, or a negative value for no wheel event at that ms.
static navigation::G3Motion boot_motion_run(double (*wheel)(unsigned),unsigned end_ms) {
    navigation::Pipeline p;mx5_dr_context x={1,1,1};
    CHECK(p.init_model(navigation::research_model_profile(),mx5_dr_default_config(),x,false,false,true));
    CHECK(p.enable_beta(beta_profile_tunnel()));
    uint64_t seq=0;
    for(unsigned ms=0;ms<=end_ms;ms+=100) {
        const double kmh=wheel(ms);
        if(kmh>=0) {
            navigation::RawEvent w=raw(navigation::WHEELS,ms,++seq);
            for(unsigned i=0;i<4;++i)w.raw[i]=uint16_t(lround(kmh*100+10000));
            p.enqueue_raw(w);
        }
        navigation::RawEvent y=raw(navigation::YAW,ms,++seq);y.raw[0]=2048;
        p.enqueue_raw(y);
        if(ms>=100)p.drain(T(ms)-100000000ULL);
    }
    p.drain(T(end_ms+1000));
    return p.g3_motion();
}
static double moving_first(unsigned) { return 18; }
static double gap_into_motion(unsigned ms) { return ms<1000?0:ms<1400?-1:18; }    // 0.5 s gap, then moving
static double short_gap(unsigned ms) { return ms<1000?0:ms<2500?-1:ms<3000?0:18; } // 1.6 s gap while parked
static double long_gap(unsigned ms) { return ms<1000?0:ms<3100?-1:ms<3500?0:18; }  // 2.2 s gap while parked
static double late_gap(unsigned ms) { return ms<1000?0:ms<1500?18:ms<4000?-1:18; } // gap after the first motion
static void pipeline_boot_motion() {
    navigation::G3Motion m=boot_motion_run(moving_first,1000);
    CHECK(m.boot_motion==navigation::G3_BOOT_FIRST_MOVING && m.first_move_ns==T(0));
    m=boot_motion_run(gap_into_motion,2000);
    CHECK(m.boot_motion==navigation::G3_BOOT_GAP_INTO_MOTION && m.boot_gap_ns==500000000ULL && m.first_move_ns==T(1400));
    m=boot_motion_run(short_gap,4000);
    CHECK(m.boot_motion==0 && m.boot_gap_ns==1600000000ULL && m.first_move_ns==T(3000));
    m=boot_motion_run(long_gap,4000);
    CHECK(m.boot_motion==navigation::G3_BOOT_LONG_GAP && m.boot_gap_ns==2200000000ULL);
    m=boot_motion_run(late_gap,5000);
    CHECK(m.boot_motion==0 && m.first_move_ns==T(1000));
}
// Exact-zero stop runs in the pipeline: a crawl at 0.02 m/s is never a stop;
// a 20 s wheel gap followed by one zero sample restarts the run there.
// Review gap (2026-10-11): the G3 yaw-chain break conditions reachable from
// raw input. A yaw silence longer than sample_age_max (250 ms, the core's
// upper bound) is a MISSING_SENSOR fault: reset_state counts one break and,
// while moving, the first window after it (a hole > G3_YAW_HOLE_NS) a
// second one. A hole while standing still is no break. A window longer than
// G3_YAW_WINDOW_MAX_NS (1 s) or a non-finite rate cannot reach g3_yaw from
// the raw path (the 250 ms limit faults first; rates are finite by
// construction): those checks are defensive only.
static uint32_t yaw_gap_breaks(bool moving) {
    navigation::Pipeline p;mx5_dr_context x={1,1,1};
    CHECK(p.init_model(navigation::research_model_profile(),mx5_dr_default_config(),x,false,false,true));
    CHECK(p.enable_beta(beta_profile_tunnel()));
    uint64_t seq=0;
    uint32_t before=0;
    for(unsigned ms=0;ms<=4000;ms+=100) {
        navigation::RawEvent w=raw(navigation::WHEELS,ms,++seq);
        for(unsigned i=0;i<4;++i)w.raw[i]=uint16_t(moving?lround(18*100+10000):10000);
        p.enqueue_raw(w);
        if(ms<2000||ms>=2500) {                      // yaw silent 2.0-2.5 s
            navigation::RawEvent y=raw(navigation::YAW,ms,++seq);y.raw[0]=2048;
            p.enqueue_raw(y);
        }
        if(ms>=100)p.drain(T(ms)-100000000ULL);
        if(ms==1900)before=p.g3_motion().breaks;
    }
    return p.g3_motion().breaks-before;
}
static void yaw_chain_breaks() {
    CHECK(yaw_gap_breaks(true)==2);    // the fault's reset + the hole after it
    CHECK(yaw_gap_breaks(false)==1);   // stationary: the reset only
}
static void pipeline_park_detection() {
    navigation::Pipeline p;mx5_dr_context x={1,1,1};
    CHECK(p.init_model(navigation::research_model_profile(),mx5_dr_default_config(),x,false,false,true));
    CHECK(p.enable_beta(beta_profile_tunnel()));
    uint64_t seq=0;
    const unsigned crawl=unsigned(lround(0.02*3.6*100+10000));   // 0.072 km/h raw
    for(unsigned ms=0;ms<=26000;ms+=100) {
        const bool wheel=ms<=23000 && !(ms>23000-20000 && ms<23000);   // the gap: 3.0-23.0 s
        if(wheel) {
            navigation::RawEvent w=raw(navigation::WHEELS,ms,++seq);
            for(unsigned i=0;i<4;++i)w.raw[i]=uint16_t(ms<1000?crawl:10000);
            CHECK(p.enqueue_raw(w)==navigation::PIPELINE_OK);
        }
        navigation::RawEvent y=raw(navigation::YAW,ms,++seq);y.raw[0]=2048;
        const navigation::PipelineResult yr=p.enqueue_raw(y);
        CHECK(yr==navigation::PIPELINE_OK||yr==navigation::PIPELINE_WAITING||yr==navigation::PIPELINE_MISSING_SENSOR||
              yr==navigation::PIPELINE_BAD_INPUT);
        if(ms>=100)p.drain(T(ms)-100000000ULL);
        const navigation::G3Motion& m=p.g3_motion();
        if(ms==900)CHECK(!m.zero_since_ns && m.last_moving_ns);       // crawling: never a stop
        if(ms==2900)CHECK(m.zero_since_ns==T(1000));                  // exact zero from 1.0 s
        if(ms==23200) {
            // The gap ended the run; the single zero sample after it starts a new one.
            CHECK(m.zero_since_ns==T(23000));
            const G3Sample s=g3_sample(p,T(23200),RT);
            CHECK(s.wheels_fresh && !s.wheels_moving);
        }
        if(ms==26000) {
            // No wheel sample since 23.0 s: the stream is stale.
            const G3Sample s=g3_sample(p,T(26000),RT);
            CHECK(!s.wheels_fresh);
        }
    }
}

// ---- synthetic two-boot JSONL for the analyzer test -------------------------
// ---- G3 bearing send (g3_send.h) -------------------------------------------
static uint32_t next_call=100;
static adapter::Observation fixv(uint64_t mono,double kmh,double course) {
    adapter::Observation o=fix(mono,35.5,139.5,RT+mono/S);
    o.position.velocity_kmh=kmh;o.position.heading_deg=course;o.call_sequence=++next_call;return o;
}
static G3SendView view(uint64_t now) {
    G3SendView v;v.now=now;v.journal_ok=v.journal_current=v.beta_live=v.beta_armed=v.adapter_ok=true;
    v.source_epoch=11;v.session_epoch=12;v.cadence_fences=0;return v;
}
static adapter::Observation overlaid(uint64_t mono,int32_t result) {
    adapter::Observation o=location(mono,true,0,0);o.choice=adapter::BETA_BEARING_OVERLAY;o.result=result;
    o.position_class=adapter::POSITION_FIX;return o;
}
// A tracker restored CARRIED/chain_ok (197.0 +- 4.5 deg) with a session
// start at 1 s and a G3Send opened at that tick.
struct SendRig {
    G3Tracker t;G3Send g;Rows j;
    explicit SendRig(const G3Record& r=good_record(),bool enabled=true) {
        adapter::set_bearing_overlay(false);   // process-global gate: each case starts closed
        t=started(r);g.start(enabled);
        t.send(location(1*S,true,0,0));
        tick(1*S);
    }
    void tick(uint64_t now,const G3SendView* v=0,const G3Sample* s=0) {
        const G3Sample x=s?*s:sample(now);
        t.tick(j,x,0,0);g.tick(j,t,x,v?*v:view(now));
    }
    void pos(const adapter::Observation& o,double wheel) { g.position(j,o,wheel,o.mono_ns+1000); }
    std::string end() const { return j.last("\"event\":\"end\""); }
};
static bool send_started(const Rows& j) {
    for(size_t i=0;i<j.lines.size();++i)
        if(has(j.lines[i],"{\"kind\":\"g3_send\"") && has(j.lines[i],"\"event\":\"start\""))return true;
    return false;
}
static void g3_send_rules() {
    adapter::set_bearing_overlay(false);
    // Opened at the session start: one start row, the OEM gate open.
    { SendRig r;CHECK(r.g.active() && adapter::bearing_overlay());
      const std::string l=r.j.last("\"kind\":\"g3_send\"");
      CHECK(has(l,"\"event\":\"start\"") && has(l,"\"g3\":\"CARRIED\"") && has(l,"\"h\":1970") &&
            has(l,"\"carried\":[1970,45]") && has(l,"\"tol\":100") && has(l,"\"age_s\":10800") && has(l,"\"session\":12"));
      CHECK(r.j.count("\"kind\":\"g3_send\"")==1 && r.g.heading()==197.0);
      // Trust gate: 4 consecutive gated fixes agreeing within 10 deg.
      r.pos(fixv(2*S,18,203),5.0);r.pos(fixv(3*S,18,190),5.0);r.pos(fixv(4*S,18,199),5.0);CHECK(r.g.active());
      // A fix below 3.6 km/h, a wheel ratio of 0.25 or a wheel below 1 m/s resets the run.
      r.pos(fixv(5*S,3.5,197),0.97);CHECK(r.g.active());
      r.pos(fixv(6*S,18,197),5.0);r.pos(fixv(7*S,18,197),4.0);r.pos(fixv(8*S,3.6,197),0.99);
      r.pos(fixv(9*S,18,197),5.0);r.pos(fixv(10*S,18,197),5.0);
      r.pos(fixv(11*S,18,207.0),5.0);CHECK(r.g.active());   // 3 in a row (10.0 deg agrees)
      r.g.send(r.j,overlaid(11*S,0),11*S);r.g.send(r.j,location(11*S,true,0,0),11*S);
      adapter::Observation stock=location(11*S,true,0,0);stock.position_class=adapter::POSITION_FIX;
      r.g.send(r.j,stock,11*S);CHECK(r.g.overlaid()==1);
      const adapter::Observation last=fixv(12*S,18,196);r.pos(last,5.0);
      CHECK(!r.g.active() && r.g.phase()==G3_SEND_ENDED && !adapter::bearing_overlay() && !strcmp(r.g.end_reason(),"trusted"));
      char call[48];snprintf(call,sizeof call,"\"by\":\"fix\",\"call\":%lu,",(unsigned long)last.call_sequence);
      // The end row carries the worker time and the deciding call.
      CHECK(has(r.end(),"\"reason\":\"trusted\",\"why\":\"stock_course\"") && has(r.end(),call) &&
            has(r.end(),"\"mono_ns\":12000001000,") && has(r.end(),"\"sent\":1,\"stock\":1") &&
            has(r.end(),"\"course\":[1960,180],\"d\":10"));
      const size_t rows=r.j.lines.size();
      r.tick(20*S);r.pos(fixv(21*S,18,290),5.0);r.g.send(r.j,overlaid(21*S,0),21*S);
      CHECK(r.g.phase()==G3_SEND_ENDED && !adapter::bearing_overlay() && r.j.lines.size()==rows); }
    // Wheel ratio boundary: |18/3.6 / 4.0 - 1| = 0.25 is not gated (it resets
    // the run), 0.09 is.
    { SendRig r;
      for(unsigned k=0;k<3;++k)r.pos(fixv((2+k)*S,18,197),5.0);
      r.pos(fixv(5*S,18,197),4.0);CHECK(r.g.active());
      for(unsigned k=0;k<3;++k)r.pos(fixv((6+k)*S,18,197),5.0);
      CHECK(r.g.active());
      r.pos(fixv(9*S,18,197),5.0/1.09);CHECK(!r.g.active() && !strcmp(r.g.end_reason(),"trusted")); }
    // A larger honest uncertainty widens the agreement (25 deg): 220 agrees.
    { G3Record m=good_record();m.unc_cdeg=2500;SendRig r(m);CHECK(has(r.j.last("start"),"\"tol\":250"));
      for(unsigned k=0;k<4;++k)r.pos(fixv((2+k)*S,18,221.5),5.0);
      CHECK(!strcmp(r.g.end_reason(),"trusted")); }
    // Conflict: 8 consecutive gated fixes disagreeing -> the stock course wins.
    { SendRig r;
      for(unsigned k=0;k<7;++k)r.pos(fixv((2+k)*S,18,290),5.0);
      CHECK(r.g.active() && !r.j.count("g3_send_conflict"));
      r.pos(fixv(9*S,18,197),5.0);      // one agreeing fix resets the conflict run
      for(unsigned k=0;k<7;++k)r.pos(fixv((10+k)*S,18,17),5.0);
      CHECK(r.g.active());
      r.pos(fixv(17*S,18,17),5.0);
      CHECK(!r.g.active() && !adapter::bearing_overlay() && !strcmp(r.g.end_reason(),"conflict"));
      const std::string c=r.j.last("g3_send_conflict");
      CHECK(has(c,"\"h\":1970") && has(c,"\"course\":[170,180]") && has(c,"\"d\":1800") && has(c,"\"fixes\":8"));
      CHECK(has(r.end(),"\"reason\":\"conflict\"") && has(r.end(),"\"by\":\"fix\"")); }
    // Stationary cap: 900 s from the open while no stock fix reached 3.6 km/h.
    { SendRig r;r.pos(fixv(2*S,3.5,197),0);r.tick(800*S);CHECK(r.g.active() && r.j.count("\"event\":\"count\"")>=1);
      r.tick(900*S);CHECK(r.g.active());
      r.tick(901*S);CHECK(!r.g.active() && !adapter::bearing_overlay() &&
                          has(r.end(),"\"reason\":\"cap\",\"why\":\"stationary_900s\",\"by\":\"tick\"")); }
    // Motion cap: 60 s from the first stock fix at >= 3.6 km/h (here without
    // a gated fix: the wheels report nothing), even after a long stand.
    { SendRig r;r.tick(700*S);CHECK(r.g.active());
      r.pos(fixv(700*S,3.6,197),0);       // motion starts at 700 s + 1 us
      r.tick(760*S);CHECK(r.g.active());
      r.tick(760*S+2000);CHECK(!r.g.active() && has(r.end(),"\"reason\":\"cap\",\"why\":\"motion_60s\"")); }
    // Faults: fail closed to the stock LOCATION at once.
    for(unsigned k=0;k<8;++k) {
        SendRig r;G3SendView v=view(2*S);G3Sample s=sample(2*S);
        const char* why[]={"journal_failed","writer_lag","beta_not_live","adapter","clock","yaw_break","model","wheel_stale"};
        if(k==0)v.journal_ok=false;else if(k==1)v.journal_current=false;else if(k==2)v.beta_live=false;
        else if(k==3)v.adapter_ok=false;else if(k==4)v.now=S/2;
        else if(k==5) { s=sample(2*S,3,0,1,true);s.motion.breaks=1; }   // a break while moving
        else if(k==7) { s.wheels_fresh=false; }                         // a dead wheel stream is not "parked"
        if(k==6)r.g.fail(r.j,2*S,"model_disabled");else r.tick(2*S,&v,&s);
        CHECK(!r.g.active() && !adapter::bearing_overlay() && !strcmp(r.g.end_reason(),"fault"));
        char want[64];snprintf(want,sizeof want,"\"why\":\"%s",why[k]);CHECK(has(r.end(),want));
    }
    // Stale wheels and a break together: wheel_stale (a dead stream never counts as parked).
    { SendRig r;G3Sample s=sample(2*S);s.wheels_fresh=false;s.motion.breaks=1;r.tick(2*S,0,&s);
      CHECK(has(r.end(),"\"why\":\"wheel_stale\"") && has(r.end(),"\"pbreaks\":0")); }
    // A break while parked (fresh wheels, no moving wheel at this and the
    // previous tick, no wheel distance) cannot hide a rotation: tolerated.
    { SendRig r;G3Sample s=sample(2*S);s.motion.breaks=2;r.tick(2*S,0,&s);
      CHECK(r.g.active() && adapter::bearing_overlay());
      s=sample(3*S,0.5,0,0.1,true);s.motion.breaks=3;r.tick(3*S,0,&s);
      CHECK(!r.g.active() && has(r.end(),"\"why\":\"yaw_break\"") && has(r.end(),"\"pbreaks\":1")); }
    { SendRig r;G3Sample s=sample(2*S,0.02);s.motion.breaks=1;r.tick(2*S,0,&s);
      CHECK(!r.g.active() && has(r.end(),"\"why\":\"yaw_break\"")); }
    // Session boundary: a new send session, bus or cadence fence ends it (never re-published).
    for(unsigned k=0;k<3;++k) {
        SendRig r;G3SendView v=view(2*S);
        if(k==0)v.session_epoch=13;else if(k==1)v.source_epoch=12;else v.cadence_fences=1;
        r.tick(2*S,&v);
        const char* why[]={"send_session","source","cadence_fence"};
        char want[64];snprintf(want,sizeof want,"\"reason\":\"session\",\"why\":\"%s\"",why[k]);
        CHECK(!r.g.active() && !adapter::bearing_overlay() && has(r.end(),want));
        r.tick(3*S,&v);CHECK(r.g.phase()==G3_SEND_ENDED && r.j.count("\"event\":\"start\",\"g3\":\"CARRIED\",\"t_s\"")==1);
    }
    // Reverse latch: a change or loss during the window ends it (no 180 deg jump).
    for(int latch=-1;latch<=1;latch+=2) {
        SendRig r;G3Sample s=sample(2*S,0,0,0,false,latch);r.tick(2*S,0,&s);
        CHECK(!r.g.active() && has(r.end(),latch==1?"\"reason\":\"latch\",\"why\":\"reverse\"":"\"reason\":\"latch\",\"why\":\"unknown\""));
        CHECK(r.g.heading()==197.0);
    }
    // A failed overlaid send ends the window.
    { SendRig r;r.g.send(r.j,overlaid(2*S,-3),2*S);
      CHECK(!r.g.active() && has(r.end(),"\"reason\":\"fault\",\"why\":\"send_result\"")); }
    // The tracker leaves CARRIED: an M course (CONFIRMED) or a rejection.
    { SendRig r;G3Course c={5*S,198.0,20.0,1};r.t.tick(r.j,sample(5*S,10,0,2,true),&c,0);
      r.g.tick(r.j,r.t,sample(5*S,10,0,2,true),view(5*S));
      CHECK(!r.g.active() && has(r.end(),"\"reason\":\"trusted\",\"why\":\"m_course\"")); }
    { SendRig r;G3Course c={5*S,17.0,20.0,1};r.t.tick(r.j,sample(5*S,10,0,2,true),&c,0);
      r.g.tick(r.j,r.t,sample(5*S,10,0,2,true),view(5*S));
      CHECK(!r.g.active() && has(r.end(),"\"reason\":\"rejected\",\"why\":\"course_error\"")); }
    // The carried heading follows the yaw integral.
    { SendRig r;r.tick(3*S,0,0);G3Sample s=sample(4*S,8,30,4,true);r.tick(4*S,0,&s);
      CHECK(r.g.active() && fabs(r.g.heading()-227.0)<1e-9); }
    // Only the genuinely restored CARRIED (chain_ok) opens; CARRIED_UNVERIFIED
    // (age_unknown, chain_unknown, same_boot) never does, also not when the
    // first FIX (position_ok) raises it to CARRIED before or after the tick.
    const char* chains[]={"none","unread","none"};
    const uint64_t rts[]={0,RT+3*3600,RT+3*3600};
    const char* boots[]={BOOT_B,BOOT_B,BOOT_A};
    const char* reasons[]={"age_unknown","chain_unknown","same_boot"};
    for(unsigned k=0;k<3;++k)for(unsigned order=0;order<2;++order) {
        adapter::set_bearing_overlay(false);
        G3Tracker t=started(good_record(),G3_LOAD_OK,chains[k],rts[k],boots[k]);
        CHECK(t.state()==G3_CARRIED_UNVERIFIED && !strcmp(t.reason(),reasons[k]));
        G3Send g;g.start(true);Rows j;t.send(location(1*S,true,0,0));
        if(order==0)t.position(j,fix(1*S,35.50005,139.5,RT+7200),navigation::G3Motion());   // first FIX before the tick
        t.tick(j,sample(1*S),0,0);g.tick(j,t,sample(1*S),view(1*S));
        if(order==1) { t.position(j,fix(2*S,35.50005,139.5,RT+7200),navigation::G3Motion());
                       t.tick(j,sample(2*S),0,0);g.tick(j,t,sample(2*S),view(2*S)); }
        if(k!=1)CHECK(t.state()==G3_CARRIED && !strcmp(t.reason(),"position_ok"));
        CHECK(g.phase()==G3_SEND_ENDED && !adapter::bearing_overlay() && !send_started(j) && j.count("\"event\":\"none\"")==1);
    }
    // The genuine chain_ok restore opens whether the first FIX came before the tick or not.
    for(unsigned order=0;order<2;++order) {
        adapter::set_bearing_overlay(false);
        G3Tracker t=started(good_record());G3Send g;g.start(true);Rows j;t.send(location(1*S,true,0,0));
        if(order==0)t.position(j,fix(1*S,35.50005,139.5,RT+3*3600),navigation::G3Motion());
        t.tick(j,sample(1*S),0,0);g.tick(j,t,sample(1*S),view(1*S));
        CHECK(g.active() && adapter::bearing_overlay() && !strcmp(t.reason(),"chain_ok"));
    }
    // Waiting for a known forward latch while the car stands, at most 30 s;
    // given up when the car moves first.
    { adapter::set_bearing_overlay(false);
      G3Tracker t=started(good_record());G3Send g;g.start(true);Rows j;t.send(location(1*S,true,0,0));
      G3Sample s=sample(1*S,0,0,0,false,-1);t.tick(j,s,0,0);g.tick(j,t,s,view(1*S));
      CHECK(g.phase()==G3_SEND_IDLE && j.lines.size()>0 && !j.count("g3_send"));
      s=sample(5*S,0,0,0,false,1);t.tick(j,s,0,0);g.tick(j,t,s,view(5*S));CHECK(g.phase()==G3_SEND_IDLE);
      s=sample(6*S);t.tick(j,s,0,0);g.tick(j,t,s,view(6*S));CHECK(g.active() && has(j.last("start"),"\"rev\":0")); }
    { adapter::set_bearing_overlay(false);
      G3Tracker t=started(good_record());G3Send g;g.start(true);Rows j;t.send(location(1*S,true,0,0));
      G3Sample s=sample(1*S,0,0,0,false,-1);t.tick(j,s,0,0);g.tick(j,t,s,view(1*S));
      s=sample(31*S,0,0,0,false,-1);t.tick(j,s,0,0);g.tick(j,t,s,view(31*S));
      CHECK(g.phase()==G3_SEND_ENDED && has(j.last("g3_send"),"\"event\":\"none\",\"g3\":\"CARRIED\",\"reason\":\"latch\"")); }
    { adapter::set_bearing_overlay(false);
      G3Tracker t=started(good_record());G3Send g;g.start(true);Rows j;t.send(location(1*S,true,0,0));
      G3Sample s=sample(1*S,0,0,0,false,-1);t.tick(j,s,0,0);g.tick(j,t,s,view(1*S));
      s=sample(2*S,1,0,1,true,-1);t.tick(j,s,0,0);g.tick(j,t,s,view(2*S));
      CHECK(g.phase()==G3_SEND_ENDED && has(j.last("g3_send"),"\"reason\":\"latch\"")); }
    // Stale wheels or no observed session at the open: wait, then none.
    { adapter::set_bearing_overlay(false);
      G3Tracker t=started(good_record());G3Send g;g.start(true);Rows j;t.send(location(1*S,true,0,0));
      G3Sample s=sample(1*S);s.wheels_fresh=false;t.tick(j,s,0,0);g.tick(j,t,s,view(1*S));
      CHECK(g.phase()==G3_SEND_IDLE);
      G3SendView v=view(2*S);v.session_epoch=0;s=sample(2*S);t.tick(j,s,0,0);g.tick(j,t,s,v);CHECK(g.phase()==G3_SEND_IDLE);
      s=sample(40*S);s.wheels_fresh=false;t.tick(j,s,0,0);g.tick(j,t,s,view(40*S));
      CHECK(g.phase()==G3_SEND_ENDED && has(j.last("g3_send"),"\"reason\":\"wheel_stale\"")); }
    { G3Tracker t=started(good_record(),G3_LOAD_MISSING);G3Send g;g.start(true);Rows j;
      t.tick(j,sample(1*S),0,0);g.tick(j,t,sample(1*S),view(1*S));
      CHECK(g.phase()==G3_SEND_IDLE && j.lines.empty());          // no session yet
      t.send(location(2*S,true,0,0));t.tick(j,sample(2*S),0,0);g.tick(j,t,sample(2*S),view(2*S));
      CHECK(g.phase()==G3_SEND_ENDED && has(j.last("g3_send"),"\"g3\":\"UNKNOWN\",\"reason\":\"missing\"")); }
    // Off (g3-off marker or not BETA): no row, no gate, ever.
    { SendRig r(good_record(),false);
      r.pos(fixv(2*S,18,197),5.0);r.tick(2*S);r.g.send(r.j,overlaid(2*S,0),2*S);
      CHECK(r.g.phase()==G3_SEND_OFF && !adapter::bearing_overlay() && !r.j.count("g3_send")); }
}

// The window against the REAL adapter (configured once, at the end of this
// process): what reaches the OEM send. Covers the publish() generation check
// (a candidate needs a FIX POSITION of the current generation seen by the
// worker) and the empty-candidate overwrite after the end.
static uint64_t adapter_now=100*S;
static uint8_t oem_sent[48];
static adapter::Observation last_position_obs;
static uint64_t oem_clock(void*) { return adapter_now; }
static int32_t oem_next(void*,adapter::VehicleData* d) { if(d && d->payload && d->length==48)memcpy(oem_sent,d->payload,48);return 0; }
static void oem_sink(const adapter::Observation* o,void*) { if(o->kind==adapter::Observation::POSITION)last_position_obs=*o; }
static bool oem_provenance(void*,const adapter::PositionContext&,adapter::Provenance* out,void*) {
    *out=adapter::Provenance();out->source_epoch=11;out->session_epoch=12;
    out->domain=adapter::Provenance::Domain::BETA;return true;
}
static void oem_beta_event(void*,const char*) {}
static void oem_session(const void*,runtime::session_trace::Snapshot* out,void*) {
    *out=runtime::session_trace::Snapshot();out->result=runtime::session_trace::UNOBSERVED;
}
static void put32le(uint8_t* p,uint32_t v) { for(unsigned i=0;i<4;++i)p[i]=uint8_t(v>>(8*i)); }
// One FIX POSITION + its LOCATION (stock bearing 0); returns the outgoing bearing_e6 (or -1: original).
static long oem_fix(uint64_t utc) {
    uint8_t position[72]={};put32le(position,1);put32le(position+8,uint32_t(utc));
    const double lat=35.5,lon=139.5;uint64_t b;
    memcpy(&b,&lat,8);put32le(position+16,uint32_t(b));put32le(position+20,uint32_t(b>>32));
    memcpy(&b,&lon,8);put32le(position+24,uint32_t(b));put32le(position+28,uint32_t(b>>32));
    uint8_t original[48];for(unsigned i=0;i<48;++i)original[i]=uint8_t(0x40+i);
    put32le(original+8,355000000u);put32le(original+12,1395000000u);original[40]=1;put32le(original+44,0);
    adapter::VehicleData data={1,original,48};
    adapter::position_enter(0,position);adapter::send_vehicle_data(0,&data);adapter::position_leave();
    if(!memcmp(oem_sent,original,48))return -1;
    for(unsigned i=0;i<48;++i)if(i!=40 && !(i>=44 && i<48))CHECK(oem_sent[i]==original[i]);
    return long(oem_sent[44]|(oem_sent[45]<<8)|(oem_sent[46]<<16)|(uint32_t(oem_sent[47])<<24));
}
static void g3_send_adapter() {
    adapter::Options o=adapter::Options();
    o.sink=oem_sink;o.clock=oem_clock;o.provenance=oem_provenance;o.max_snapshot_age_ns=500000000ULL;
    o.allow_beta=true;o.beta_event=oem_beta_event;o.session_reader=oem_session;o.sessions_declined=true;
    CHECK(adapter::configure(oem_next,o) && adapter::set_mode(adapter::BETA));
    SendRig r;CHECK(r.g.active());
    uint64_t utc=1700000000;
    CHECK(oem_fix(++utc)==-1);                     // the worker has not seen a FIX POSITION yet
    r.g.position(r.j,last_position_obs,0,adapter_now);
    r.tick(adapter_now);CHECK(oem_fix(++utc)==197000000);   // published for this generation: overlaid
    // A generation change the worker has not followed with a FIX POSITION:
    // no publication with the new generation, the old candidate is unusable.
    adapter::invalidate();
    adapter_now+=50000000;r.tick(adapter_now);
    CHECK(oem_fix(++utc)==-1);
    r.g.position(r.j,last_position_obs,0,adapter_now);
    adapter_now+=50000000;r.tick(adapter_now);CHECK(oem_fix(++utc)==197000000);
    // End (stock trusted): the gate closes and the next tick overwrites the
    // candidate with an empty one; re-opening the gate by hand finds nothing.
    for(unsigned k=0;k<4;++k)r.pos(fixv(adapter_now+k,18,197),5.0);
    CHECK(!r.g.active() && !adapter::bearing_overlay());
    r.g.position(r.j,last_position_obs,0,adapter_now);
    CHECK(oem_fix(++utc)==-1);                     // gate closed: the beta.14 path
    adapter_now+=50000000;r.tick(adapter_now);
    adapter::set_bearing_overlay(true);
    CHECK(oem_fix(++utc)==-1);                     // the empty candidate: nothing to overlay
    adapter::set_bearing_overlay(false);
}

struct Out { void line(const char* s) { std::printf("%s\n",s); } void fail() {} };
static void boot_row(Out& o,uint64_t mono,const char* boot) {
    char b[600];
    snprintf(b,sizeof b,"{\"kind\":\"boot\",\"schema\":1,\"pid\":100,\"mono_ns\":%llu,\"boot_id\":\"%s\",\"mode\":5,"
             "\"install\":\"ok\",\"assist_ready\":false,\"assist_block\":\"sensor_timing_quality_calibration_unverified\","
             "\"wire_timestamp_modified\":false,\"log_profile\":\"persistent\"}",(unsigned long long)mono,boot);
    o.line(b);
}
static int two_boot(const char* dir) {
    Out o;
    // Boot A: nothing stored; one session; drive, stop, drive, final stop; power cut.
    {
        boot_row(o,1*S,BOOT_A);
        G3Tracker t;t.start(1*S,true,"m",BOOT_A,"none",RT,G3_LOAD_MISSING,g3_record());
        char f[96];t.boot_fields(f,sizeof f);
        std::printf("{\"kind\":\"shadow_boot\",\"active\":true,\"capture_active\":true,\"domain\":\"model\",\"source\":\"existing_vbs_vim_callback\",\"assist_ready\":false%s}\n",f);
        t.journal_start(o,1*S);
        t.send(location(5*S,true,0,0));
        navigation::G3Motion still=navigation::G3Motion();
        t.position(o,fix(6*S,35.5,139.5,RT+6),still);
        t.tick(o,sample(6*S),0,0);
        G3Course c={40*S,120.0,25.0,1};
        t.tick(o,sample(40*S,200,0,30,true),&c,0);
        // Drive, park 20 s (a parked record), drive on, final park (power cut).
        const uint64_t steps[][3]={{100,800,0},{120,820,100},{200,2000,0},{320,2400,300}};   // s, m, last moving s
        for(size_t i=0;i<sizeof steps/sizeof steps[0];++i) {
            G3Sample s=steps[i][2]?park(steps[i][0]*S,double(steps[i][1]),steps[i][2]*S,197.3):
                drive(steps[i][0]*S,double(steps[i][1]),197.3);
            s.unc_deg=4.4;
            t.tick(o,s,0,0);
            char b[G3_RECORD_MAX];size_t n;uint32_t seq;
            if(t.take_request(b,sizeof b,&n,&seq)) {
                const G3IoResult r=g3_write_atomic(dir,b,n);
                t.store_result(o,steps[i][0]*S,seq,r);
            }
        }
    }
    // Boot B, 3 h later: restore, first fix at the same place, a reverse
    // start out of the space, then forward with a +37 deg turn, first course.
    {
        boot_row(o,1*S,BOOT_B);
        G3Record d;const G3Load l=g3_load(dir,&d);
        G3Tracker t;t.start(1*S,true,"m",BOOT_B,"none",RT+300+6*3600,l,d);
        char f[96];t.boot_fields(f,sizeof f);
        std::printf("{\"kind\":\"shadow_boot\",\"active\":true,\"capture_active\":true,\"domain\":\"model\",\"source\":\"existing_vbs_vim_callback\",\"assist_ready\":false%s}\n",f);
        t.journal_start(o,1*S);
        t.position(o,fix(3*S,35.50003,139.5,RT+300+3*3600+3),navigation::G3Motion());
        t.send(location(4*S,true,95,0.4));
        t.tick(o,sample(4*S,0,0,0,false,-1),0,0);
        for(unsigned k=5;k<=34;++k)t.send(location(k*S,true,k<25?100:0,0.3));
        t.tick(o,sample(35*S,6,-3,6,true,1),0,0);
        G3Course c={50*S,234.0,14.0,1};
        G3Sample s=sample(50*S,40,37,15,true,0);s.motion.first_move_reverse=1;
        t.tick(o,s,&c,0);
    }
    return 0;
}


// ---- garage carry (validation/G3_GARAGE_EXIT_FIX_2026-10-17.md) ---------------
static G3Record depart_record(int32_t unc_cdeg=790) {
    G3Record r=good_record();r.flags=G3_FLAG_HEADING|G3_FLAG_POSITION|G3_FLAG_DEPART;r.unc_cdeg=unc_cdeg;
    r.heading_cdeg=16360;r.rt_s=0;r.utc_s=0;return r;
}
// prev: the previous product boot (g3_last_start_swap); depart_record() was
// written by BOOT_A, so the default is the record's own boot.
static G3Tracker started_garage(const G3Record& r,const char* chain="none",uint64_t rt=100,const char* boot=BOOT_B,
                                bool garage=true,G3Load load=G3_LOAD_OK,const char* prev=BOOT_A) {
    G3Tracker t;t.start(0,true,"marker",boot,chain,rt,load,r,garage,prev);return t;
}
static void garage_codec_restore() {
    // Flags 4..7 decode (old builds reject them as a format error: fail closed).
    const G3Record r=depart_record();
    const std::string l=encode(r);G3Record d;
    CHECK(g3_decode(l.data(),l.size(),&d)==G3_LOAD_OK && d.flags==7);
    // No wall clock (1970), chain ok: CARRIED/chain_depart with the switch on ...
    { G3Tracker t=started_garage(r);CHECK(t.state()==G3_CARRIED && !strcmp(t.reason(),"chain_depart") &&
                                           t.garage() && t.garage_carry() && t.unc_max()==G3_GARAGE_UNC_MAX_DEG);
      Rows j;t.journal_start(j,1);CHECK(has(j.last("g3_store"),"\"reason\":\"chain_depart\"") &&
                                        has(j.last("g3_store"),"\"age_s\":null"));
      char f[128];CHECK(t.boot_fields(f,sizeof f)>0 &&
                        has(f,",\"g3\":{\"state\":\"CARRIED\",\"reason\":\"chain_depart\",\"garage\":1}")); }
    // ... beta.16 behaviour with the switch off (and no "garage" field):
    // beta.16 decodes flags > 3 as a format error, so the marker rejects a
    // flag-4 record the same way (no 30 deg rules on a 2 % record, C2).
    { G3Tracker t=started_garage(r,"none",100,BOOT_B,false);
      CHECK(t.state()==G3_UNKNOWN && !strcmp(t.reason(),"format") && !t.garage() && !t.garage_carry() &&
            !t.have_carried() && t.disk_state()==0);
      Rows j;t.journal_start(j,1);CHECK(has(j.last("g3_store"),"\"event\":\"rejected\",\"reason\":\"format\"") &&
                                        !has(j.last("g3_store"),"\"prev\""));
      char f[128];CHECK(t.boot_fields(f,sizeof f)>0 && !has(f,"garage")); }
    { G3Record a=r;a.rt_s=RT;G3Tracker t=started_garage(a,"none",RT+60,BOOT_B,false);
      CHECK(t.state()==G3_UNKNOWN && !strcmp(t.reason(),"format")); }
    // Previous product boot (review P1): the record must come from the boot
    // that started just before this one; otherwise CARRIED_UNVERIFIED (no window).
    { G3Tracker t=started_garage(r,"none",100,BOOT_B,true,G3_LOAD_OK,BOOT_B);
      CHECK(t.state()==G3_CARRIED_UNVERIFIED && !strcmp(t.reason(),"not_prev_boot") && t.have_carried());
      Rows j;t.journal_start(j,1);CHECK(has(j.last("g3_store"),"\"prev\":\"bbbbbbbb\"")); }
    // The whole boot id is compared (third review, mutant M2 compared 8
    // characters): a token differing only after the 8th character, and one
    // differing within the first 8, are both another boot.
    { G3Tracker t=started_garage(r,"none",100,BOOT_B,true,G3_LOAD_OK,"aaaaaaaa-1111-2222-3333-444444444445");
      CHECK(t.state()==G3_CARRIED_UNVERIFIED && !strcmp(t.reason(),"not_prev_boot"));
      Rows j;t.journal_start(j,1);CHECK(has(j.last("g3_store"),"\"prev\":\"aaaaaaaa\"")); }
    { G3Tracker t=started_garage(r,"none",100,BOOT_B,true,G3_LOAD_OK,"aaaaaaab-1111-2222-3333-444444444444");
      CHECK(t.state()==G3_CARRIED_UNVERIFIED && !strcmp(t.reason(),"not_prev_boot")); }
    { G3Tracker t=started_garage(r,"none",100,BOOT_B,true,G3_LOAD_OK,"none");
      CHECK(t.state()==G3_CARRIED_UNVERIFIED && !strcmp(t.reason(),"not_prev_boot"));
      Rows j;t.journal_start(j,1);CHECK(has(j.last("g3_store"),"\"prev\":\"none\"")); }
    { G3Tracker t=started_garage(r,"none",100,BOOT_B,true,G3_LOAD_OK,"unread");
      CHECK(t.state()==G3_CARRIED_UNVERIFIED && !strcmp(t.reason(),"not_prev_boot")); }
    { G3Tracker t;t.start(0,true,"marker",BOOT_B,"none",100,G3_LOAD_OK,r,true);   // no token passed
      CHECK(t.state()==G3_CARRIED_UNVERIFIED && !strcmp(t.reason(),"not_prev_boot")); }
    { G3Record u=r;strcpy(u.boot_id,"unknown");G3Tracker t=started_garage(u,"none",100,BOOT_B,true,G3_LOAD_OK,"unknown");
      CHECK(t.state()==G3_CARRIED_UNVERIFIED && !strcmp(t.reason(),"not_prev_boot")); }
    // ... also when a wall clock proves the age (chain_ok is not a way around it).
    { G3Record a=r;a.rt_s=RT;G3Tracker t=started_garage(a,"none",RT+3600,BOOT_B,true,G3_LOAD_OK,BOOT_B);
      CHECK(t.state()==G3_CARRIED_UNVERIFIED && !strcmp(t.reason(),"not_prev_boot")); }
    // A record without flag 4 keeps beta.15 (no previous-boot condition).
    { G3Record o=r;o.flags=G3_FLAG_HEADING|G3_FLAG_POSITION;o.rt_s=RT;
      G3Tracker t=started_garage(o,"none",RT+60,BOOT_B,true,G3_LOAD_OK,"none");
      CHECK(t.state()==G3_CARRIED && !strcmp(t.reason(),"chain_ok") && !t.garage_carry()); }
    // A record without the departure flag (beta.16 writer) stays unverified.
    { G3Record o=r;o.flags=G3_FLAG_HEADING|G3_FLAG_POSITION;G3Tracker t=started_garage(o);
      CHECK(t.state()==G3_CARRIED_UNVERIFIED && !strcmp(t.reason(),"age_unknown") && !t.garage_carry()); }
    // Continuity still required: broken / unknown chain, same boot, stale age, M record.
    { G3Tracker t=started_garage(r,"12345678");CHECK(t.state()==G3_UNKNOWN && !strcmp(t.reason(),"chain_broken")); }
    { G3Tracker t=started_garage(r,"unread");CHECK(t.state()==G3_CARRIED_UNVERIFIED && !strcmp(t.reason(),"chain_unknown")); }
    { G3Tracker t=started_garage(r,"none",100,BOOT_A);CHECK(t.state()==G3_CARRIED_UNVERIFIED && !strcmp(t.reason(),"same_boot")); }
    { G3Record a=r;a.rt_s=RT;G3Tracker t=started_garage(a,"none",RT+G3_AGE_MAX_S+1);
      CHECK(t.state()==G3_UNKNOWN && !strcmp(t.reason(),"age")); }
    { G3Record a=r;a.rt_s=RT;G3Tracker t=started_garage(a,"none",RT+3600);
      CHECK(t.state()==G3_CARRIED && !strcmp(t.reason(),"chain_ok") && t.garage_carry()); }
    { G3Record m=r;m.state='M';m.flags=G3_FLAG_DEPART;G3Tracker t=started_garage(m);
      CHECK(t.state()==G3_UNKNOWN && !strcmp(t.reason(),"moving_at_end")); }
    // Limit 60 deg for departure-marked records, 30 deg otherwise.
    { G3Tracker t=started_garage(depart_record(6000));CHECK(t.state()==G3_CARRIED); }
    { G3Tracker t=started_garage(depart_record(6001));CHECK(t.state()==G3_UNKNOWN && !strcmp(t.reason(),"unc")); }
    { G3Record o=depart_record(3001);o.flags=3;o.rt_s=RT;G3Tracker t=started_garage(o,"none",RT+60);
      CHECK(t.state()==G3_UNKNOWN && !strcmp(t.reason(),"unc")); }
    { G3Tracker t=started_garage(depart_record(4500),"none",100,BOOT_B,false);
      CHECK(t.state()==G3_UNKNOWN && !strcmp(t.reason(),"format")); }
    // Propagation: rotation term 2 % (garage) instead of 10 %: 20 deg + 600 deg
    // of |rotation| = 32 deg stays CARRIED (limit 60) ...
    { G3Tracker t=started_garage(depart_record(2000));Rows j;
      G3Sample s=sample(2*S,30,0,0,true);s.motion.abs_turn_rad=600*G3_PI/180;s.motion.first_move_ns=1;
      t.tick(j,s,0,0);CHECK(t.state()==G3_CARRIED && fabs(t.propagated_unc()-32.0)<1e-6);
      // ... and 20 + 0.02 * 2101 deg = 62.02 deg is rejected (unc_grew).
      s.motion.abs_turn_rad=2101*G3_PI/180;t.tick(j,s,0,0);
      CHECK(t.state()==G3_REJECTED && !strcmp(t.reason(),"unc_grew")); }
    // The beta.16 coefficient with the switch off (a record without flag 4):
    // 20 + 60 deg.
    { G3Record o=depart_record(2000);o.flags=3;o.rt_s=RT;G3Tracker t=started_garage(o,"none",RT+60,BOOT_B,false);
      CHECK(t.state()==G3_CARRIED && !strcmp(t.reason(),"chain_ok"));
      Rows j;G3Sample s=sample(2*S,30,0,0,true);s.motion.abs_turn_rad=600*G3_PI/180;t.tick(j,s,0,0);
      CHECK(t.state()==G3_REJECTED && !strcmp(t.reason(),"unc_grew")); }
    // Review C2: with the switch ON, a carried record WITHOUT flag 4 keeps the
    // BETA terms (10 %, 0.002 rad/s) and the 30 deg limit: 20 + 0.1 * 99 deg
    // = 29.9 stays, 20 + 0.1 * 101 deg = 30.1 is rejected.
    { G3Record o=depart_record(2000);o.flags=3;o.rt_s=RT;G3Tracker t=started_garage(o,"none",RT+60);
      CHECK(t.state()==G3_CARRIED && !strcmp(t.reason(),"chain_ok") && !t.garage_carry() && t.unc_max()==G3_UNC_MAX_DEG);
      Rows j;G3Sample s=sample(2*S,30,0,0,true);s.motion.abs_turn_rad=99*G3_PI/180;t.tick(j,s,0,0);
      CHECK(t.state()==G3_CARRIED && fabs(t.propagated_unc()-29.9)<1e-6);
      s.motion.abs_turn_rad=101*G3_PI/180;t.tick(j,s,0,0);
      CHECK(t.state()==G3_REJECTED && !strcmp(t.reason(),"unc_grew")); }
    // Review P4: the time term of a carried flag-4 record is 0.0023 rad/s
    // (100 s moving: 13.178 deg), of a record without flag 4 BETA's 0.002.
    { G3Tracker t=started_garage(depart_record(1000));Rows j;
      G3Sample s=sample(2*S,30,0,100,true);s.motion.abs_turn_rad=0;t.tick(j,s,0,0);
      CHECK(t.state()==G3_CARRIED && fabs(t.propagated_unc()-(10.0+0.0023*100*180/G3_PI))<1e-6); }
    { G3Record o=depart_record(1000);o.flags=3;o.rt_s=RT;G3Tracker t=started_garage(o,"none",RT+60);Rows j;
      G3Sample s=sample(2*S,30,0,100,true);s.motion.abs_turn_rad=0;t.tick(j,s,0,0);
      CHECK(t.state()==G3_CARRIED && fabs(t.propagated_unc()-(10.0+0.002*100*180/G3_PI))<1e-6); }
}
// Review C2: the uncertainty a garage-on boot STORES grows with the garage
// chain terms (2 %, 0.0023 rad/s); with the switch off with BETA's (10 %,
// 0.002 rad/s). Reference 5 deg from BETA at 1 s, then BETA invalid while
// 500 deg of |rotation| and 100 s of motion pass; the record at the park.
static uint32_t stored_unc_cdeg(bool garage) {
    Store st;st.dir=temp_dir();st.t.start(0,true,"m",BOOT_B,"none",RT,G3_LOAD_MISSING,g3_record(),garage);
    G3Sample a=drive(1*S,10);a.motion.turn_rad=a.motion.abs_turn_rad=0;a.motion.moving_s=0;st.tick(a);
    G3Sample b=drive(2*S,60);b.heading_valid=false;b.motion.turn_rad=0;b.motion.abs_turn_rad=500*G3_PI/180;
    b.motion.moving_s=100;st.tick(b);
    for(unsigned k=3;k<=20;++k) {
        G3Sample p=park(k*S,60,2*S);p.heading_valid=false;p.motion.turn_rad=0;p.motion.abs_turn_rad=500*G3_PI/180;
        p.motion.moving_s=100;st.tick(p);
    }
    G3Record d;CHECK(st.written.size()==1 && g3_load(st.dir.c_str(),&d)==G3_LOAD_OK && (d.flags&G3_FLAG_HEADING));
    return uint32_t(d.unc_cdeg);
}
static void garage_stored_uncertainty() {
    const double on=5.0+0.0023*100*180/G3_PI+0.02*500,off=5.0+0.002*100*180/G3_PI+0.10*500;
    CHECK(fabs(stored_unc_cdeg(true)-on*100)<=1.0);
    CHECK(fabs(stored_unc_cdeg(false)-off*100)<=1.0);
}
static G3Sample replacing(G3Sample s) { s.beta_replacing=true;return s; }
static void garage_store() {
    G3Record d;
    // 5 s of exact standstill is enough; the record carries the departure flag.
    { Store st;st.dir=temp_dir();st.t.start(0,true,"m",BOOT_B,"none",RT,G3_LOAD_MISSING,g3_record(),true);
      st.tick(drive(1*S,50));st.tick(park(5999*S/1000,50,1*S));CHECK(st.written.empty());
      st.tick(park(6*S,50,1*S));
      CHECK(st.written.size()==1 && g3_load(st.dir.c_str(),&d)==G3_LOAD_OK && d.state=='S' && d.flags==7);
      CHECK(has(st.j.last("\"event\":\"written\""),"\"sync\":0")); }
    // The switch off: still 15 s and no departure flag.
    { Store st;st.dir=temp_dir();st.t.start(0,true,"m",BOOT_B,"none",RT,G3_LOAD_MISSING,g3_record(),false);
      st.tick(drive(1*S,50));for(unsigned k=2;k<=15;++k)st.tick(park(k*S,50,1*S));CHECK(st.written.empty());
      st.tick(park(16*S,50,1*S));CHECK(st.written.size()==1 && g3_load(st.dir.c_str(),&d)==G3_LOAD_OK && d.flags==3);
      // ... and no write while driving away (no departure mark).
      for(unsigned k=17;k<=30;++k)st.tick(drive(k*S,50+k));
      CHECK(st.written.size()==1);
      CHECK(g3_load(st.dir.c_str(),&d)==G3_LOAD_OK && d.state=='S'); }
    // BETA replacing (garage, GPS lost): the write runs without fsync; the
    // fsync write waits until BETA no longer replaces.
    { Store st;st.dir=temp_dir();st.t.start(0,true,"m",BOOT_B,"none",RT,G3_LOAD_MISSING,g3_record(),true);
      st.tick(replacing(drive(1*S,50)));
      for(unsigned k=2;k<=6;++k)st.tick(replacing(park(k*S,50,1*S)));
      CHECK(st.written.size()==1 && has(st.j.last("\"event\":\"written\""),"\"sync\":0"));
      for(unsigned k=7;k<=90;++k)st.tick(replacing(park(k*S,50,1*S)));
      CHECK(st.written.size()==1);                                   // 89 s parked, still replacing
      st.tick(park(91*S,50,1*S));
      CHECK(st.written.size()==2 && has(st.j.last("\"event\":\"written\""),"\"sync\":1")); }
    // A first write of a long stop while replacing is requested without fsync.
    { Store st;st.dir=temp_dir();st.t.start(0,true,"m",BOOT_B,"none",RT,G3_LOAD_MISSING,g3_record(),true);
      st.tick(replacing(drive(1*S,50)));
      for(unsigned k=2;k<=70;++k)st.tick(replacing(park(k*S,50,1*S)),k<65?"journal_lagging":0);
      CHECK(st.written.size()==1 && has(st.j.last("\"event\":\"written\""),"\"sync\":0")); }
    // wait_s counts from the 5 s garage threshold (review C3): blocked until 9 s.
    { Store st;st.dir=temp_dir();st.t.start(0,true,"m",BOOT_B,"none",RT,G3_LOAD_MISSING,g3_record(),true);
      st.tick(drive(1*S,50));
      for(unsigned k=2;k<=9;++k)st.tick(park(k*S,50,1*S),k<9?"journal_lagging":0);
      CHECK(st.written.size()==1 && has(st.j.last("\"event\":\"written\""),"\"wait_s\":3.0")); }
    // Review P3, while BETA replaces (tunnel jam): a second S request within
    // 30 s of the previous one waits ("replacing_rate"); the departure mark
    // between them is not spaced. Without replacing the next S comes at 5 s.
    { Store st;st.dir=temp_dir();st.t.start(0,true,"m",BOOT_B,"none",RT,G3_LOAD_MISSING,g3_record(),true);
      st.tick(replacing(drive(1*S,50)));
      for(unsigned k=2;k<=6;++k)st.tick(replacing(park(k*S,50,1*S)));
      CHECK(st.written.size()==1 && st.t.disk_state()=='S');                     // S requested at 6 s
      st.tick(replacing(drive(7*S,51)));st.tick(replacing(drive(11*S,60)));
      CHECK(st.written.size()==2 && st.t.disk_state()=='M');                     // mark at 11 s
      for(unsigned k=12;k<=35;++k)st.tick(replacing(park(k*S,60,11*S)));
      CHECK(st.written.size()==2 && st.t.disk_state()=='M');                     // 35 s < 6 + 30
      st.tick(replacing(park(36*S,60,11*S)));
      CHECK(st.written.size()==3 && st.t.disk_state()=='S'); }
    { Store st;st.dir=temp_dir();st.t.start(0,true,"m",BOOT_B,"none",RT,G3_LOAD_MISSING,g3_record(),true);
      st.tick(drive(1*S,50));for(unsigned k=2;k<=6;++k)st.tick(park(k*S,50,1*S));
      st.tick(drive(7*S,51));st.tick(drive(11*S,60));CHECK(st.written.size()==2);
      for(unsigned k=12;k<=16;++k)st.tick(park(k*S,60,11*S));
      CHECK(st.written.size()==3 && st.t.disk_state()=='S'); }
    // Review P3: the departure mark while BETA replaces needs a journal writer
    // lag <= 100 ms; without replacing the lag does not hold it (the runtime's
    // idle rule, 300 ms, comes through io_block).
    { Store st;st.dir=temp_dir();st.t.start(0,true,"m",BOOT_B,"none",RT,G3_LOAD_MISSING,g3_record(),true);
      st.tick(drive(1*S,50));for(unsigned k=2;k<=6;++k)st.tick(park(k*S,50,1*S));
      G3Sample a=replacing(drive(12*S,60));a.writer_lag_ns=G3_GARAGE_REPLACING_MARK_LAG_NS+1;st.tick(a);
      CHECK(st.written.size()==1 && st.t.disk_state()=='S');
      a=replacing(drive(13*S,61));a.writer_lag_ns=G3_GARAGE_REPLACING_MARK_LAG_NS;st.tick(a);
      CHECK(st.written.size()==2 && st.t.disk_state()=='M'); }
    { Store st;st.dir=temp_dir();st.t.start(0,true,"m",BOOT_B,"none",RT,G3_LOAD_MISSING,g3_record(),true);
      st.tick(drive(1*S,50));for(unsigned k=2;k<=6;++k)st.tick(park(k*S,50,1*S));
      G3Sample a=drive(12*S,60);a.writer_lag_ns=250000000ULL;st.tick(a);
      CHECK(st.written.size()==2 && st.t.disk_state()=='M'); }
    // Third review C1 (the probe of validation/G3_GARAGE_EXIT_FIX_2026-10-17.md
    // "third fix round"): while BETA replaces, the departure mark could not
    // land (7..13 s the idle rule, 14..22 s a writer lag of 150 ms), so the
    // disk keeps S1 (heading) although the car left. The final park's S2 is
    // not held by the 30 s spacing (disk S with heading) and replaces S1 at
    // 23 s; on the c75053c code S1 survived to the ignition off at 30 s.
    { Store st;st.dir=temp_dir();st.t.start(0,true,"m",BOOT_B,"none",RT,G3_LOAD_MISSING,g3_record(),true);
      st.tick(replacing(drive(1*S,50)));
      for(unsigned k=2;k<=6;++k)st.tick(replacing(park(k*S,50,1*S)));
      CHECK(st.written.size()==1 && st.t.disk_state()=='S');                     // S1 requested at 6 s
      for(unsigned k=7;k<=13;++k)st.tick(replacing(drive(k*S,50+2.0*(k-6))),"journal_draining");
      for(unsigned k=14;k<=17;++k) {
          G3Sample a=replacing(drive(k*S,50+2.0*(k-6)));a.writer_lag_ns=150000000ULL;st.tick(a);
      }
      CHECK(st.written.size()==1 && st.t.disk_state()=='S');                     // no mark landed
      for(unsigned k=18;k<=22;++k) {
          G3Sample a=replacing(park(k*S,72,18*S));a.writer_lag_ns=150000000ULL;st.tick(a);
      }
      CHECK(st.written.size()==1);
      st.tick(replacing(park(23*S,72,18*S)));
      CHECK(st.written.size()==2 && st.t.disk_state()=='S' && g3_load(st.dir.c_str(),&d)==G3_LOAD_OK &&
            d.state=='S' && d.seq==2 && d.mono_ms==23000 && d.flags==7);
      for(unsigned k=24;k<=30;++k)st.tick(replacing(park(k*S,72,18*S)));
      CHECK(st.written.size()==2 && g3_load(st.dir.c_str(),&d)==G3_LOAD_OK && d.seq==2); }
    // The same exception after the 60 s fsync rewrite (not replacing): it is
    // an S request too; GPS lost right after, the mark blocked by the idle
    // rule, the next garage park within 30 s still replaces the stale S.
    { Store st;st.dir=temp_dir();st.t.start(0,true,"m",BOOT_B,"none",RT,G3_LOAD_MISSING,g3_record(),true);
      st.tick(drive(1*S,50));
      for(unsigned k=2;k<=61;++k)st.tick(park(k*S,50,1*S));
      CHECK(st.written.size()==2 && has(st.j.last("\"event\":\"written\""),"\"sync\":1"));   // 6 s, 61 s
      for(unsigned k=62;k<=70;++k)st.tick(replacing(drive(k*S,50+3.0*(k-61))),"journal_draining");
      CHECK(st.written.size()==2 && st.t.disk_state()=='S');
      for(unsigned k=71;k<=75;++k)st.tick(replacing(park(k*S,77,71*S)),"journal_draining");
      st.tick(replacing(park(76*S,77,71*S)));                                     // 15 s after 61 s
      CHECK(st.written.size()==3 && g3_load(st.dir.c_str(),&d)==G3_LOAD_OK && d.state=='S' && d.mono_ms==76000); }
    // An S WITHOUT heading on disk keeps the 30 s spacing (it is never
    // carried: no_heading; no mark is requested for it).
    { Store st;st.dir=temp_dir();st.t.start(0,true,"m",BOOT_B,"none",RT,G3_LOAD_MISSING,g3_record(),true);
      st.tick(replacing(moved(1*S,50,0,0,10,true,false)));
      for(unsigned k=2;k<=6;++k)st.tick(replacing(parked(moved(k*S,50,0,0,10,false,false),1*S)));
      CHECK(st.written.size()==1 && g3_load(st.dir.c_str(),&d)==G3_LOAD_OK && d.flags==(G3_FLAG_POSITION|G3_FLAG_DEPART));
      for(unsigned k=7;k<=10;++k)st.tick(replacing(moved(k*S,50+3.0*(k-6),0,0,12,true,false)));
      CHECK(st.written.size()==1);                                                // no mark for hv 0
      for(unsigned k=11;k<=35;++k)st.tick(replacing(parked(moved(k*S,62,0,0,12,false,false),11*S)));
      CHECK(st.written.size()==1);                                                // 35 s < 6 + 30
      st.tick(replacing(parked(moved(36*S,62,0,0,12,false,false),11*S)));
      CHECK(st.written.size()==2); }
    // The rule F taint rewrite while BETA replaces is not spaced either
    // (review M3: with the disk-S exception this path is covered twice).
    { Store st;st.dir=temp_dir();st.t.start(0,true,"m",BOOT_B,"none",RT,G3_LOAD_MISSING,g3_record(),true);
      st.tick(replacing(moved(1*S,50,0,0,10,true,true,100,4)));
      for(unsigned k=2;k<=6;++k)st.tick(replacing(parked(moved(k*S,60,0,0,12,false,true,100,4),1*S)));
      CHECK(st.written.size()==1 && g3_load(st.dir.c_str(),&d)==G3_LOAD_OK && d.flags==7 && d.heading_cdeg==10000);
      st.tick(replacing(discarded(parked(moved(8*S,60,0,0,12,false,false),1*S),1)));
      st.tick(replacing(discarded(parked(moved(11*S,60,0,0,12,false,false),1*S),1)));
      CHECK(st.written.size()==2 && g3_load(st.dir.c_str(),&d)==G3_LOAD_OK &&
            d.flags==(G3_FLAG_POSITION|G3_FLAG_DEPART) && d.mono_ms==11000); }
    // Departure mark: after the S record, 0.4 m does not mark, 0.6 m does;
    // the mark has no heading and decodes as moving_at_end.
    { Store st;st.dir=temp_dir();st.t.start(0,true,"m",BOOT_B,"none",RT,G3_LOAD_MISSING,g3_record(),true);
      st.tick(drive(1*S,50));for(unsigned k=2;k<=6;++k)st.tick(park(k*S,50,1*S));CHECK(st.written.size()==1);
      st.tick(drive(12*S,50.4));CHECK(st.written.size()==1);
      st.tick(drive(13*S,50.6));
      CHECK(st.written.size()==2 && g3_load(st.dir.c_str(),&d)==G3_LOAD_OK && d.state=='M' && d.flags==G3_FLAG_DEPART &&
            d.heading_cdeg==0 && d.seq==2);
      CHECK(has(st.j.last("\"event\":\"written\""),"\"st\":\"M\",\"hv\":0") && st.t.disk_state()=='M');
      { G3Tracker n=started_garage(d);CHECK(n.state()==G3_UNKNOWN && !strcmp(n.reason(),"moving_at_end")); }
      // Only once; the next park writes S again (heading valid, disk has none).
      for(unsigned k=14;k<=30;++k)st.tick(drive(k*S,51+k));
      CHECK(st.written.size()==2);
      for(unsigned k=31;k<=36;++k)st.tick(park(k*S,52,30*S));
      CHECK(st.written.size()==3 && g3_load(st.dir.c_str(),&d)==G3_LOAD_OK && d.state=='S' && d.flags==7); }
    // A turn alone (2.5 deg, 0.1 m) marks the departure; 1.5 deg does not.
    { Store st;st.dir=temp_dir();st.t.start(0,true,"m",BOOT_B,"none",RT,G3_LOAD_MISSING,g3_record(),true);
      st.tick(drive(1*S,50));for(unsigned k=2;k<=6;++k)st.tick(park(k*S,50,1*S));
      G3Sample a=drive(12*S,50.1);a.motion.turn_rad=(20+1.5)*G3_PI/180;st.tick(a);CHECK(st.written.size()==1);
      G3Sample b=drive(13*S,50.1);b.motion.turn_rad=(20+2.5)*G3_PI/180;st.tick(b);
      CHECK(st.written.size()==2 && st.t.disk_state()=='M'); }
    // The mark waits for the rate limit and the idle rule; a cancelled mark is retried.
    { Store st;st.dir=temp_dir();st.t.start(0,true,"m",BOOT_B,"none",RT,G3_LOAD_MISSING,g3_record(),true);
      st.tick(drive(1*S,50));for(unsigned k=2;k<=6;++k)st.tick(park(k*S,50,1*S));
      st.tick(drive(8*S,60));CHECK(st.written.size()==1);             // 2 s after the write: rate limit
      st.tick(drive(11*S,61),"journal_lagging");CHECK(st.written.size()==1);
      st.tick(drive(12*S,62),0,G3_IO_CANCELLED);
      CHECK(st.written.size()==1 && has(st.j.last("\"event\":\"cancelled\""),"\"st\":\"M\"") && st.t.store_on());
      st.tick(drive(17*S,70));CHECK(st.written.size()==2 && st.t.disk_state()=='M'); }
    // A restored S record is marked when the car leaves (seq continues).
    { Store st;st.dir=temp_dir();st.t.start(0,true,"m",BOOT_B,"none",100,G3_LOAD_OK,depart_record(),true,BOOT_A);
      G3Sample a=drive(1*S,0.3);a.motion.turn_rad=a.motion.abs_turn_rad=0;st.tick(a);CHECK(st.written.empty());
      a=drive(2*S,1.0);a.motion.turn_rad=a.motion.abs_turn_rad=0;st.tick(a);
      CHECK(st.written.size()==1 && g3_load(st.dir.c_str(),&d)==G3_LOAD_OK && d.state=='M' && d.seq==8);
      CHECK(st.t.state()==G3_CARRIED); }                            // the carried heading in memory stays
    // No mark for an S record without heading, nor with the switch off.
    { Store st;st.dir=temp_dir();G3Record nh=depart_record();nh.flags=G3_FLAG_DEPART;
      st.t.start(0,true,"m",BOOT_B,"none",100,G3_LOAD_OK,nh,true,BOOT_A);
      for(unsigned k=1;k<=10;++k)st.tick(drive(k*S,10.0*k));
      CHECK(st.written.empty()); }
    { Store st;st.dir=temp_dir();st.t.start(0,true,"m",BOOT_B,"none",RT,G3_LOAD_OK,good_record(),false);
      for(unsigned k=1;k<=10;++k)st.tick(drive(k*S,10.0*k));
      CHECK(st.written.empty()); }
    // In-flight views for the worker's cancel rule.
    { G3Tracker t;t.start(0,true,"m",BOOT_B,"none",100,G3_LOAD_OK,depart_record(),true,BOOT_A);Rows j;
      t.tick(j,drive(1*S,2),0,0);char b[G3_RECORD_MAX];size_t n;uint32_t q;bool sync=true;
      CHECK(t.take_request(b,sizeof b,&n,&q,&sync) && !sync && t.in_flight_mark() && !t.in_flight_sync());
      // The worker's cancel rule: a moving wheel or a stale stream never
      // cancels a mark; the idle rule (io_cancel) still does.
      CHECK(!g3_cancel_reason(0,drive(2*S,3),t));
      G3Sample stale=drive(2*S,3);stale.wheels_fresh=false;CHECK(!g3_cancel_reason(0,stale,t));
      CHECK(same(g3_cancel_reason("journal_lagging",drive(2*S,3),t),"journal_lagging"));
      CHECK(g3_cancel_ignores_replacing(t));
      G3IoResult ok={G3_IO_OK,0,0,G3_STEP_NONE,0,0};t.store_result(j,3*S,q,ok);CHECK(!t.in_flight_mark()); }
    // A parked record in flight (no fsync): a moving wheel / stale stream cancel
    // it, BETA replacing does not; its fsync rewrite is cancelled by replacing.
    { G3Tracker t;t.start(0,true,"m",BOOT_B,"none",RT,G3_LOAD_MISSING,g3_record(),true);Rows j;
      t.tick(j,drive(1*S,50),0,0);t.tick(j,park(7*S,50,1*S),0,0);
      char b[G3_RECORD_MAX];size_t n;uint32_t q;bool sync=true;
      CHECK(t.take_request(b,sizeof b,&n,&q,&sync) && !sync && !t.in_flight_mark());
      CHECK(same(g3_cancel_reason(0,drive(8*S,51),t),"wheels_moving"));
      G3Sample stale=park(8*S,50,1*S);stale.wheels_fresh=false;CHECK(same(g3_cancel_reason(0,stale,t),"wheels_stale"));
      CHECK(!g3_cancel_reason(0,park(8*S,50,1*S),t) && g3_cancel_ignores_replacing(t));
      G3IoResult ok={G3_IO_OK,0,0,G3_STEP_NONE,0,0};t.store_result(j,8*S,q,ok);
      for(unsigned k=9;k<=61;++k)t.tick(j,park(k*S,50,1*S),0,0);
      CHECK(t.take_request(b,sizeof b,&n,&q,&sync) && sync && t.in_flight_sync() && !g3_cancel_ignores_replacing(t)); }
    { G3Tracker t;t.start(0,true,"m",BOOT_B,"none",RT,G3_LOAD_MISSING,g3_record(),false);
      CHECK(!g3_cancel_ignores_replacing(t)); }
}
static G3Sample garage_sample(uint64_t now,double distance,bool moving,int reverse,uint32_t breaks=0) {
    G3Sample s=sample(now,distance,0,moving?now/S:0,moving,reverse);s.motion.breaks=breaks;return s;
}
static void garage_send() {
    adapter::set_bearing_overlay(false);
    // chain_depart, a latch that is not valid and no lost 0x118 information
    // (review C1): waits ("latch_unknown") until the car has moved 20 m since
    // boot, then opens (beta.16 would wait for a known forward latch and give up).
    { G3Tracker t=started_garage(depart_record());G3Send g;g.start(true,true);Rows j;
      t.send(location(1*S,true,0,0));
      G3Sample s=garage_sample(1*S,0,false,-1);t.tick(j,s,0,0);g.tick(j,t,s,view(1*S));
      CHECK(!g.active() && g.phase()==G3_SEND_IDLE && !adapter::bearing_overlay() &&
            has(j.last("\"event\":\"wait\""),"\"reason\":\"latch_unknown\",\"t_s\":0.0,\"rev\":-1,\"rlost\":0"));
      s=garage_sample(2*S,19.9,true,-1);t.tick(j,s,0,0);g.tick(j,t,s,view(2*S));
      CHECK(!g.active() && j.count("\"event\":\"wait\"")==1);                // one row per reason change
      s=garage_sample(3*S,20.0,true,-1);t.tick(j,s,0,0);g.tick(j,t,s,view(3*S));
      CHECK(g.active() && adapter::bearing_overlay() && has(j.last("\"event\":\"start\""),"\"garage\":1,\"moved\":1,\"rlost\":0") &&
            has(j.last("\"event\":\"start\""),"\"rev\":-1"));
      // Not valid without lost information stays open; reverse ends it.
      const G3Sample m=garage_sample(5*S,40,true,-1);t.tick(j,m,0,0);g.tick(j,t,m,view(5*S));CHECK(g.active());
      const G3Sample r=garage_sample(6*S,41,true,1);t.tick(j,r,0,0);g.tick(j,t,r,view(6*S));
      CHECK(!g.active() && !adapter::bearing_overlay() && !strcmp(g.end_reason(),"latch") && !strcmp(g.end_why(),"reverse")); }
    // A valid forward latch opens at once, standing (moved 0).
    { G3Tracker t=started_garage(depart_record());G3Send g;g.start(true,true);Rows j;
      t.send(location(1*S,true,0,0));
      const G3Sample s=garage_sample(1*S,0,false,0);t.tick(j,s,0,0);g.tick(j,t,s,view(1*S));
      CHECK(g.active() && has(j.last("\"event\":\"start\""),"\"moved\":0") && !j.count("\"event\":\"wait\""));
      adapter::set_bearing_overlay(false); }
    // Review C1, lost information: the car was shifted into reverse before
    // the session (excluded REVERSE 1: reverse_unsure > 0, latch not valid):
    // never opens however far it drives; a later valid forward latch opens.
    { G3Tracker t=started_garage(depart_record());G3Send g;g.start(true,true);Rows j;
      t.send(location(1*S,true,0,0));
      for(unsigned k=1;k<=30;++k) {
          G3Sample s=garage_sample(k*S,10.0*k,k>1,-1);s.motion.reverse_unsure=1;t.tick(j,s,0,0);g.tick(j,t,s,view(k*S));
      }
      CHECK(g.phase()==G3_SEND_IDLE && !adapter::bearing_overlay() && j.count("\"event\":\"wait\"")==1 &&
            has(j.last("\"event\":\"wait\""),"\"reason\":\"latch_unsure\"") && has(j.last("\"event\":\"wait\""),"\"rlost\":1"));
      G3Sample s=garage_sample(31*S,310,true,0);s.motion.reverse_unsure=1;t.tick(j,s,0,0);g.tick(j,t,s,view(31*S));
      CHECK(g.active() && has(j.last("\"event\":\"start\""),"\"rlost\":1")); }
    // Review C1/P6: an open window whose latch is dropped (a pipeline reset
    // while standing: not valid, reverse_unsure > 0) ends with latch/unknown.
    { G3Tracker t=started_garage(depart_record());G3Send g;g.start(true,true);Rows j;
      t.send(location(1*S,true,0,0));
      G3Sample s=garage_sample(1*S,0,false,0);t.tick(j,s,0,0);g.tick(j,t,s,view(1*S));CHECK(g.active());
      s=garage_sample(2*S,0,false,-1,1);s.motion.reverse_unsure=1;t.tick(j,s,0,0);g.tick(j,t,s,view(2*S));
      CHECK(!g.active() && !adapter::bearing_overlay() && !strcmp(g.end_reason(),"latch") && !strcmp(g.end_why(),"unknown")); }
    // The wait rows are bounded per boot (reason flapping).
    { G3Tracker t=started_garage(depart_record());G3Send g;g.start(true,true);Rows j;
      t.send(location(1*S,true,0,0));
      for(unsigned k=1;k<=40;++k) { const G3Sample s=garage_sample(k*S,0,false,k%2?1:-1);t.tick(j,s,0,0);g.tick(j,t,s,view(k*S)); }
      CHECK(g.phase()==G3_SEND_IDLE && j.count("\"event\":\"wait\"")==G3_SEND_WAIT_ROWS_MAX); }
    // The switch off in G3Send: chain_depart never opens.
    { G3Tracker t=started_garage(depart_record());G3Send g;g.start(true,false);Rows j;
      t.send(location(1*S,true,0,0));const G3Sample s=garage_sample(1*S,0,false,0);
      t.tick(j,s,0,0);g.tick(j,t,s,view(1*S));
      CHECK(!g.active() && g.phase()==G3_SEND_ENDED && has(j.last("\"event\":\"none\""),"\"reason\":\"chain_depart\"")); }
    // Opens after the car moved since boot (the session started while driving).
    { G3Tracker t=started_garage(depart_record());G3Send g;g.start(true,true);Rows j;
      for(unsigned k=1;k<=4;++k) { const G3Sample s=garage_sample(k*S,5.0*k,true,-1);t.tick(j,s,0,0);g.tick(j,t,s,view(k*S)); }
      CHECK(!g.active() && g.phase()==G3_SEND_IDLE);               // no session yet
      t.send(location(5*S,true,0,0));
      const G3Sample s=garage_sample(5*S,25,true,-1);t.tick(j,s,0,0);g.tick(j,t,s,view(5*S));
      CHECK(g.active() && has(j.last("\"event\":\"start\""),"\"moved\":1")); }
    // A yaw chain break while moving (before the open) prevents the open.
    { G3Tracker t=started_garage(depart_record());G3Send g;g.start(true,true);Rows j;
      const G3Sample a=garage_sample(1*S,1,true,-1,0);t.tick(j,a,0,0);g.tick(j,t,a,view(1*S));
      const G3Sample b=garage_sample(2*S,6,true,-1,1);t.tick(j,b,0,0);g.tick(j,t,b,view(2*S));
      t.send(location(3*S,true,0,0));
      const G3Sample c=garage_sample(3*S,31,true,-1,1);t.tick(j,c,0,0);g.tick(j,t,c,view(3*S));
      CHECK(!g.active() && g.phase()==G3_SEND_ENDED && has(j.last("\"event\":\"none\""),"\"reason\":\"yaw_break\"")); }
    // Before the first moving wheel (stream not yet fresh) a break is tolerated;
    // after motion a break with a stale stream is not.
    { G3Tracker t=started_garage(depart_record());G3Send g;g.start(true,true);Rows j;
      G3Sample a=garage_sample(1*S,0,false,0,0);a.wheels_fresh=false;t.tick(j,a,0,0);g.tick(j,t,a,view(1*S));
      a=garage_sample(2*S,0,false,0,3);a.wheels_fresh=false;t.tick(j,a,0,0);g.tick(j,t,a,view(2*S));
      t.send(location(3*S,true,0,0));
      const G3Sample c=garage_sample(3*S,0,false,0,3);t.tick(j,c,0,0);g.tick(j,t,c,view(3*S));
      CHECK(g.active()); }
    { G3Tracker t=started_garage(depart_record());G3Send g;g.start(true,true);Rows j;
      G3Sample a=garage_sample(1*S,5,false,0,0);t.tick(j,a,0,0);g.tick(j,t,a,view(1*S));
      a=garage_sample(2*S,5,false,0,1);a.wheels_fresh=false;t.tick(j,a,0,0);g.tick(j,t,a,view(2*S));
      t.send(location(3*S,true,0,0));
      const G3Sample c=garage_sample(3*S,5,false,0,1);t.tick(j,c,0,0);g.tick(j,t,c,view(3*S));
      CHECK(!g.active() && has(j.last("\"event\":\"none\""),"\"reason\":\"yaw_break\"")); }
    // A break while standing is tolerated.
    { G3Tracker t=started_garage(depart_record());G3Send g;g.start(true,true);Rows j;
      const G3Sample a=garage_sample(1*S,0,false,0,0);t.tick(j,a,0,0);g.tick(j,t,a,view(1*S));
      const G3Sample b=garage_sample(2*S,0,false,0,1);t.tick(j,b,0,0);g.tick(j,t,b,view(2*S));
      t.send(location(3*S,true,0,0));
      const G3Sample c=garage_sample(3*S,0,false,0,1);t.tick(j,c,0,0);g.tick(j,t,c,view(3*S));
      CHECK(g.active()); }
    // Reverse at the session start: wait (no time limit), open once forward.
    { G3Tracker t=started_garage(depart_record());G3Send g;g.start(true,true);Rows j;
      t.send(location(1*S,true,0,0));
      for(unsigned k=1;k<=40;++k) { const G3Sample s=garage_sample(k*S,0.2*k,k>2,1);t.tick(j,s,0,0);g.tick(j,t,s,view(k*S)); }
      CHECK(g.phase()==G3_SEND_IDLE && j.count("\"kind\":\"g3_send\"")==1 &&
            has(j.last("\"kind\":\"g3_send\""),"\"event\":\"wait\",\"reason\":\"reverse\""));
      const G3Sample s=garage_sample(41*S,8.2,true,0);t.tick(j,s,0,0);g.tick(j,t,s,view(41*S));
      CHECK(g.active()); }
    // The propagated heading is what the window sends; a 60 deg garage limit
    // passed ends it (rejected/unc_grew).
    { G3Tracker t=started_garage(depart_record(5000));G3Send g;g.start(true,true);Rows j;
      t.send(location(1*S,true,0,0));
      G3Sample s=garage_sample(1*S,0,false,0);t.tick(j,s,0,0);g.tick(j,t,s,view(1*S));CHECK(g.active());
      s=garage_sample(2*S,30,true,-1);s.motion.turn_rad=45*G3_PI/180;s.motion.abs_turn_rad=45*G3_PI/180;
      t.tick(j,s,0,0);g.tick(j,t,s,view(2*S));
      CHECK(g.active() && fabs(g.heading()-208.6)<1e-6);
      s=garage_sample(3*S,60,true,-1);s.motion.turn_rad=45*G3_PI/180;s.motion.abs_turn_rad=600*G3_PI/180;
      t.tick(j,s,0,0);g.tick(j,t,s,view(3*S));
      CHECK(!g.active() && !strcmp(g.end_reason(),"rejected") && !strcmp(g.end_why(),"unc_grew")); }
    adapter::set_bearing_overlay(false);
}
// Review C1 with the real Pipeline: which REVERSE inputs count as lost
// information (G3Motion::reverse_unsure) and the two scenarios end to end.
static navigation::Pipeline* latch_pipeline(navigation::Pipeline& p) {
    mx5_dr_context x={1,1,1};
    CHECK(p.init_model(navigation::research_model_profile(),mx5_dr_default_config(),x,false,false,true));
    CHECK(p.enable_beta(beta_profile_tunnel()));
    return &p;
}
static void feed_still(navigation::Pipeline& p,unsigned from,unsigned to,uint64_t* seq,double kmh=0) {
    for(unsigned ms=from;ms<=to;ms+=100) {
        navigation::RawEvent w=raw(navigation::WHEELS,ms,++*seq);
        for(unsigned i=0;i<4;++i)w.raw[i]=uint16_t(lround(kmh*100+10000));
        p.enqueue_raw(w);
        navigation::RawEvent y=raw(navigation::YAW,ms,++*seq);y.raw[0]=2048;p.enqueue_raw(y);
        if(ms>=100)p.drain(T(ms)-100000000ULL);
    }
}
static void garage_latch_pipeline() {
    mx5_dr_context x={1,1,1};
    // Excluded forward value (an engine-start burst before the admission): no count.
    { navigation::Pipeline p;latch_pipeline(p);navigation::RawEvent r=raw(navigation::REVERSE,0,1);r.reverse=0;
      p.exclude_reverse_event(r);CHECK(p.g3_motion().reverse_unsure==0 && !p.reverse_latched()); }
    // Excluded reverse value: counted (the car may be in reverse).
    { navigation::Pipeline p;latch_pipeline(p);navigation::RawEvent r=raw(navigation::REVERSE,0,1);r.reverse=1;
      p.exclude_reverse_event(r);CHECK(p.g3_motion().reverse_unsure==1 && !p.reverse_latched()); }
    // A motion input gap that clears the latch: counted even with no latch.
    { navigation::Pipeline p;latch_pipeline(p);p.exclude_reverse(navigation::LATCH_CLEAR_INPUT_GAP);
      CHECK(p.g3_motion().reverse_unsure==1); }
    // A valid latch dropped by a reset while standing: counted.
    { navigation::Pipeline p;latch_pipeline(p);uint64_t seq=0;
      navigation::RawEvent r=raw(navigation::REVERSE,0,++seq);r.reverse=0;CHECK(p.enqueue_raw(r)==navigation::PIPELINE_OK);
      feed_still(p,0,1500,&seq);
      CHECK(p.reverse_latched() && p.latch_value()==0 && p.g3_motion().reverse_unsure==0);
      p.reset(x);CHECK(!p.reverse_latched() && p.g3_motion().reverse_unsure==1); }
    // A queued REVERSE message discarded by a reset before it was latched: counted.
    { navigation::Pipeline p;latch_pipeline(p);uint64_t seq=0;feed_still(p,0,1000,&seq);
      navigation::RawEvent r=raw(navigation::REVERSE,1050,++seq);r.reverse=1;CHECK(p.enqueue_raw(r)==navigation::PIPELINE_OK);
      CHECK(!p.reverse_latched() && p.g3_motion().reverse_unsure==0);
      p.reset(x);CHECK(!p.reverse_latched() && p.g3_motion().reverse_unsure==1); }
    // A rejected REVERSE message (epoch 0 is invalid input): counted.
    { navigation::Pipeline p;latch_pipeline(p);navigation::RawEvent r=raw(navigation::REVERSE,0,1);r.reverse=1;r.epoch=0;
      CHECK(p.enqueue_raw(r)!=navigation::PIPELINE_OK && p.g3_motion().reverse_unsure==1); }
    // Scenario 1 end to end: the car is shifted into reverse before the AA
    // session (the runtime excludes that message), then the session starts
    // and the car drives 100 m: the chain_depart window never opens.
    { navigation::Pipeline p;latch_pipeline(p);uint64_t seq=0;
      navigation::RawEvent r=raw(navigation::REVERSE,0,++seq);r.reverse=1;p.exclude_reverse_event(r);
      G3Tracker t=started_garage(depart_record());G3Send g;g.start(true,true);Rows j;
      t.send(location(T(500),true,0,0));
      feed_still(p,0,2000,&seq);
      feed_still(p,2100,22000,&seq,18);                    // 20 s at 18 km/h: about 100 m
      G3Sample s=g3_sample(p,T(21900),0);
      CHECK(s.reverse==-1 && s.motion.reverse_unsure==1 && s.motion.distance_m>90);
      t.tick(j,s,0,0);g.tick(j,t,s,view(T(21900)));
      CHECK(!g.active() && g.phase()==G3_SEND_IDLE && has(j.last("\"event\":\"wait\""),"\"reason\":\"latch_unsure\"")); }
    // ... and without that excluded message the same drive opens it.
    { navigation::Pipeline p;latch_pipeline(p);uint64_t seq=0;
      G3Tracker t=started_garage(depart_record());G3Send g;g.start(true,true);Rows j;
      t.send(location(T(500),true,0,0));
      feed_still(p,0,2000,&seq);feed_still(p,2100,22000,&seq,18);
      G3Sample s=g3_sample(p,T(21900),0);
      CHECK(s.reverse==-1 && s.motion.reverse_unsure==0);
      t.tick(j,s,0,0);g.tick(j,t,s,view(T(21900)));CHECK(g.active());
      // Scenario 2 end to end: the car stops, a pipeline reset (while
      // standing) drops nothing (no valid latch) and the window stays open;
      // a REVERSE message queued and discarded by a reset ends it.
      feed_still(p,22100,25000,&seq);
      s=g3_sample(p,T(24900),0);t.tick(j,s,0,0);g.tick(j,t,s,view(T(24900)));CHECK(g.active());
      navigation::RawEvent r=raw(navigation::REVERSE,25050,++seq);r.reverse=1;CHECK(p.enqueue_raw(r)==navigation::PIPELINE_OK);
      p.reset(x);
      s=g3_sample(p,T(25100),0);CHECK(s.reverse==-1 && s.motion.reverse_unsure==1);
      t.tick(j,s,0,0);g.tick(j,t,s,view(T(25100)));
      CHECK(!g.active() && !strcmp(g.end_reason(),"latch") && !strcmp(g.end_why(),"unknown")); }
    // Scenario 2b: a window opened with a valid forward latch; a reset while
    // standing drops it: the window ends (latch/unknown).
    { navigation::Pipeline p;latch_pipeline(p);uint64_t seq=0;
      navigation::RawEvent r=raw(navigation::REVERSE,0,++seq);r.reverse=0;CHECK(p.enqueue_raw(r)==navigation::PIPELINE_OK);
      G3Tracker t=started_garage(depart_record());G3Send g;g.start(true,true);Rows j;
      t.send(location(T(500),true,0,0));
      feed_still(p,0,2000,&seq);
      G3Sample s=g3_sample(p,T(1900),0);CHECK(s.reverse==0);
      t.tick(j,s,0,0);g.tick(j,t,s,view(T(1900)));CHECK(g.active());
      p.reset(x);
      s=g3_sample(p,T(2000),0);CHECK(s.reverse==-1 && s.motion.reverse_unsure==1);
      t.tick(j,s,0,0);g.tick(j,t,s,view(T(2000)));
      CHECK(!g.active() && !strcmp(g.end_reason(),"latch") && !strcmp(g.end_why(),"unknown")); }
    adapter::set_bearing_overlay(false);
}
// Review P1: the previous-product-boot token file.
static void last_start_token() {
    const std::string d=temp_dir();char prev[37];
    CHECK(g3_last_start_swap(d.c_str(),BOOT_A,prev) && !strcmp(prev,"none"));
    CHECK(read_file(d+"/"+G3_LAST_START_NAME)==std::string("mx5g3start 1 ")+BOOT_A+"\n");
    CHECK(g3_last_start_swap(d.c_str(),BOOT_B,prev) && !strcmp(prev,BOOT_A));
    CHECK(g3_last_start_swap(d.c_str(),BOOT_B,prev) && !strcmp(prev,BOOT_B));     // a second start of one boot
    CHECK(access((d+"/"+G3_LAST_START_TEMP).c_str(),F_OK)!=0);
    // "unknown" boot: reads, writes nothing.
    CHECK(!g3_last_start_swap(d.c_str(),"unknown",prev) && !strcmp(prev,BOOT_B));
    CHECK(read_file(d+"/"+G3_LAST_START_NAME)==std::string("mx5g3start 1 ")+BOOT_B+"\n");
    // Malformed / oversized / wrong boot token: "unread", then rewritten.
    write_file(d+"/"+G3_LAST_START_NAME,"mx5g3start 1 not-a-boot-id\n");
    CHECK(g3_last_start_swap(d.c_str(),BOOT_A,prev) && !strcmp(prev,"unread"));
    write_file(d+"/"+G3_LAST_START_NAME,"mx5g3start 1 zzzzzzzz-1111-2222-3333-444444444444\n");   // right length, not hex
    CHECK(g3_last_start_swap(d.c_str(),BOOT_A,prev) && !strcmp(prev,"unread"));
    write_file(d+"/"+G3_LAST_START_NAME,std::string("mx5g3start 2 ")+BOOT_A+"\n");
    CHECK(g3_last_start_swap(d.c_str(),BOOT_A,prev) && !strcmp(prev,"unread"));
    write_file(d+"/"+G3_LAST_START_NAME,std::string("mx5g3start 1 ")+BOOT_A+"\nextra\n");
    CHECK(g3_last_start_swap(d.c_str(),BOOT_A,prev) && !strcmp(prev,"unread"));
    write_file(d+"/"+G3_LAST_START_NAME,std::string(100,'x'));
    CHECK(g3_last_start_swap(d.c_str(),BOOT_A,prev) && !strcmp(prev,"unread"));
    CHECK(g3_last_start_swap(d.c_str(),BOOT_B,prev) && !strcmp(prev,BOOT_A));
    // A leftover temporary file is replaced; a symlink is never followed.
    write_file(d+"/"+G3_LAST_START_TEMP,"junk");
    CHECK(g3_last_start_swap(d.c_str(),BOOT_A,prev) && !strcmp(prev,BOOT_B) && access((d+"/"+G3_LAST_START_TEMP).c_str(),F_OK)!=0);
    unlink((d+"/"+G3_LAST_START_NAME).c_str());
    write_file(d+"/target",std::string("mx5g3start 1 ")+BOOT_B+"\n");
    CHECK(!symlink((d+"/target").c_str(),(d+"/"+G3_LAST_START_NAME).c_str()));
    CHECK(g3_last_start_swap(d.c_str(),BOOT_A,prev) && !strcmp(prev,"unread"));
    CHECK(read_file(d+"/target")==std::string("mx5g3start 1 ")+BOOT_B+"\n");   // rename replaced the link
    // No directory: nothing written, prev "unread" (not ENOENT on the file).
    CHECK(!g3_last_start_swap((d+"/missing").c_str(),BOOT_A,prev));
    CHECK(!g3_last_start_swap(0,BOOT_A,prev) && !strcmp(prev,"unread"));
}

// ---- lazy previous-product-boot token (fourth fix, 2026-10-18) -------------
// validation/G3_GARAGE_EXIT_FIX_2026-10-17.md "fourth fix": a garage-on boot
// reads g3-last-start at start and rewrites it only once it moved, was blind,
// wants its first S record or stops observing. Real files, the real tracker,
// the real raw-wheel watch; the store thread is replaced by a synchronous pump
// that routes each request to its target file (like the runtime's mailbox).
static const char BOOT_C[]="cccccccc-1111-2222-3333-444444444444";
static std::string token_of(const std::string& d) { return read_file(d+"/"+G3_LAST_START_NAME); }
static std::string token_line(const char* boot) { return std::string("mx5g3start 1 ")+boot+"\n"; }
static void token_codec() {
    const std::string d=temp_dir();char prev[37];
    // Read / line / write (the eager swap is the same read + write).
    g3_last_start_read(d.c_str(),prev);CHECK(!strcmp(prev,"none"));
    char line[64];CHECK(g3_last_start_line(BOOT_A,line,sizeof line)==50 && token_line(BOOT_A)==line);
    CHECK(!g3_last_start_line("unknown",line,sizeof line) && !g3_last_start_line(0,line,sizeof line) &&
          !g3_last_start_line(BOOT_A,line,40));
    CHECK(g3_last_start_write(d.c_str(),BOOT_A) && token_of(d)==token_line(BOOT_A));
    g3_last_start_read(d.c_str(),prev);CHECK(!strcmp(prev,BOOT_A));
    CHECK(!g3_last_start_write(d.c_str(),"unknown") && token_of(d)==token_line(BOOT_A));
    // The terminal path's own temporary name; no temporary file is left.
    CHECK(g3_last_start_write(d.c_str(),BOOT_B,G3_LAST_START_SYNC_TEMP) && token_of(d)==token_line(BOOT_B));
    CHECK(access((d+"/"+G3_LAST_START_SYNC_TEMP).c_str(),F_OK)!=0 && access((d+"/"+G3_LAST_START_TEMP).c_str(),F_OK)!=0);
    struct stat st;CHECK(!stat((d+"/"+G3_LAST_START_NAME).c_str(),&st) && (st.st_mode&0777)==0600);
    // Worker-boot file: first start, second start of the same boot, another boot.
    CHECK(g3_worker_boot_mark(d.c_str(),BOOT_A)==G3_WORKER_FIRST);
    CHECK(read_file(d+"/"+G3_WORKER_BOOT_NAME)==std::string("mx5g3worker 1 ")+BOOT_A+"\n");
    CHECK(g3_worker_boot_mark(d.c_str(),BOOT_A)==G3_WORKER_RESTART);
    CHECK(g3_worker_boot_mark(d.c_str(),BOOT_B)==G3_WORKER_FIRST && g3_worker_boot_mark(d.c_str(),BOOT_B)==G3_WORKER_RESTART);
    CHECK(access((d+"/"+G3_WORKER_BOOT_TEMP).c_str(),F_OK)!=0);
    // A malformed file is another boot; an unknown boot id or no directory: unmarked.
    write_file(d+"/"+G3_WORKER_BOOT_NAME,std::string("mx5g3worker 2 ")+BOOT_A+"\n");
    CHECK(g3_worker_boot_mark(d.c_str(),BOOT_A)==G3_WORKER_FIRST);
    write_file(d+"/"+G3_WORKER_BOOT_NAME,std::string("mx5g3worker 1 ")+BOOT_A);   // no newline
    CHECK(g3_worker_boot_mark(d.c_str(),BOOT_A)==G3_WORKER_FIRST);
    CHECK(g3_worker_boot_mark(d.c_str(),"unknown")==G3_WORKER_UNMARKED);
    CHECK(g3_worker_boot_mark((d+"/missing").c_str(),BOOT_A)==G3_WORKER_UNMARKED);
    CHECK(g3_worker_boot_mark(0,BOOT_A)==G3_WORKER_UNMARKED);
    CHECK(!strcmp(g3_worker_start_name(G3_WORKER_FIRST),"lazy") &&
          !strcmp(g3_worker_start_name(G3_WORKER_RESTART),"eager_restart") &&
          !strcmp(g3_worker_start_name(G3_WORKER_UNMARKED),"eager_unmarked"));
}
// Raw wheel counts of the research profile: kmh = raw * 0.01 - 100.
static void raw_wheels(double kmh,uint16_t raw[4]) { for(unsigned i=0;i<4;++i)raw[i]=uint16_t(lround(kmh*100+10000)); }
static void token_watch() {
    const uint64_t MS=1000000ULL;uint16_t raw[4];
    // Standing still for 10 min at 10 Hz: no trigger.
    { G3TokenWatch w;w.start(0,0.01,-100.0);raw_wheels(0,raw);
      bool any=false;for(uint64_t t=100*MS;t<=600*S;t+=100*MS) { w.wheel(t,raw);if(w.check(t))any=true; }
      CHECK(!any && w.distance_m==0); }
    // Rolling 1.9 m (below 2 m): no trigger; 2 m: moved.
    { G3TokenWatch w;w.start(0,0.01,-100.0);raw_wheels(0.36,raw);    // 0.1 m/s
      uint64_t t=0;for(unsigned k=1;k<=191;++k) { t=k*100*MS;w.wheel(t,raw); }
      CHECK(!w.check(t) && w.distance_m>1.89 && w.distance_m<1.91);
      for(unsigned k=192;k<=202;++k) { t=k*100*MS;w.wheel(t,raw); }
      CHECK(w.check(t) && same(w.check(t),"moved")); }
    // A receipt gap of 1.9 s keeps observing; 2.1 s that ends: blind_gap.
    { G3TokenWatch w;w.start(0,0.01,-100.0);raw_wheels(0,raw);
      w.wheel(1*S,raw);w.wheel(1*S+1900*MS,raw);CHECK(!w.check(4*S));
      w.wheel(5*S+1*MS,raw);CHECK(w.check(5*S+1*MS) && same(w.why,"blind_gap")); }   // 2.101 s
    // A gap that does not end (ignition off): silent only after 30 s.
    { G3TokenWatch w;w.start(0,0.01,-100.0);raw_wheels(0,raw);w.wheel(1*S,raw);
      CHECK(!w.check(1*S+G3_TOKEN_SILENT_NS));
      CHECK(w.check(1*S+G3_TOKEN_SILENT_NS+1) && same(w.why,"blind_silent")); }
    // No wheel event at all since the start: silent after 30 s.
    { G3TokenWatch w;w.start(5*S,0.01,-100.0);CHECK(!w.check(5*S+G3_TOKEN_SILENT_NS));
      CHECK(same(w.check(5*S+G3_TOKEN_SILENT_NS+1),"blind_silent")); }
    // A bad wheel value: conservative trigger; the first trigger is sticky.
    { G3TokenWatch w;w.start(0,0.01,-100.0);uint16_t bad[4]={10000,10000,40001,10000};w.wheel(1*S,bad);
      CHECK(same(w.check(1*S),"wheel_bad"));raw_wheels(100,raw);w.wheel(2*S,raw);w.wheel(3*S,raw);
      CHECK(same(w.check(3*S),"wheel_bad")); }
    { G3TokenWatch w;w.start(0,0.01,-100.0);uint16_t neg[4]={9000,10000,10000,10000};w.wheel(1*S,neg);   // -10 km/h
      CHECK(same(w.check(1*S),"wheel_bad")); }
    // An inactive watch (not lazy) never triggers.
    { G3TokenWatch w;raw_wheels(100,raw);w.wheel(1*S,raw);w.wheel(2*S,raw);CHECK(!w.check(100*S)); }
}
// One boot of a chain in directory dir: the runtime's start path (lazy unless
// garage is off or eager is asked for), then 100 ms steps; every request goes
// to its target file at once (or fails with force_stage, or per target with
// force_token / force_store). replacing / lag_ns: BETA replaces with that
// journal writer lag. The runtime's synchronous token attempt (review P3,
// token_sync_due, not while replacing) runs after each step; sync_fail makes
// it fail ('t' in landed when it lands).
struct ChainBoot {
    std::string dir;const char* id;G3Tracker t;G3TokenWatch w;Rows j;char prev[37];const char* swap;
    uint64_t now,zero_since,last_moving;double dist;bool gps,feed;const char* block;int force_stage;
    int force_token,force_store;bool replacing,sync_fail;uint64_t lag_ns;double heading;
    std::vector<char> landed;   // 'T' token, 't' synchronous token, 'S', 'M' in landing order
    ChainBoot(const std::string& d,const char* boot,bool garage=true,bool eager=false):
        dir(d),id(boot),swap(0),now(0),zero_since(1),last_moving(0),dist(0),gps(false),feed(true),block(0),force_stage(-1),
        force_token(-1),force_store(-1),replacing(false),sync_fail(false),lag_ns(0),heading(200) {
        if(garage && !eager) {
            g3_last_start_read(d.c_str(),prev);
            const G3WorkerStart ws=g3_worker_boot_mark(d.c_str(),boot);
            swap=g3_worker_start_name(ws);
            if(ws!=G3_WORKER_FIRST)g3_last_start_write(d.c_str(),boot);
            if(ws==G3_WORKER_RESTART)snprintf(prev,sizeof prev,"%s",boot);
        } else g3_last_start_swap(d.c_str(),boot,prev);
        G3Record rec=g3_record();const G3Load load=g3_load(d.c_str(),&rec);
        t.start(0,true,"m",boot,"none",100,load,rec,garage,prev,swap);t.journal_start(j,0);
        if(t.token_wanted())w.start(0,0.01,-100.0);
    }
    void pump() {
        char b[G3_RECORD_MAX];size_t n;uint32_t seq;bool sync=false;unsigned target=99;
        if(!t.take_request(b,sizeof b,&n,&seq,&sync,&target))return;
        G3IoResult r={G3_IO_OK,0,0,G3_STEP_NONE,now,now};
        const int forced=force_stage>=0?force_stage:target==G3_TARGET_TOKEN?force_token:force_store;
        if(forced>=0) { r.stage=forced;r.error=EIO;r.step=G3_STEP_RENAME; }
        else if(target==G3_TARGET_TOKEN)r=g3_write_atomic_at(dir.c_str(),G3_LAST_START_NAME,G3_LAST_START_TEMP,b,n,0,0,0,false);
        else { CHECK(target==G3_TARGET_STORE);r=g3_write_atomic(dir.c_str(),b,n,0,0,0,sync); }
        if(r.stage==G3_IO_OK)landed.push_back(target==G3_TARGET_TOKEN?'T':b[n>60?0:0]=='m'?
                                              (std::string(b,n).find(" S ")!=std::string::npos?'S':'M'):'?');
        t.store_result(j,now,seq,r);
    }
    // kmh for `seconds` (100 ms steps).
    void run(double kmh,double seconds) {
        for(unsigned k=0;k<unsigned(seconds*10+0.5);++k) {
            now+=100000000ULL;
            const bool moving=kmh>0;
            if(feed) { uint16_t raw[4];raw_wheels(kmh,raw);w.wheel(now,raw); }
            if(moving) { dist+=kmh/3.6*0.1;last_moving=now;zero_since=0; }
            else if(!zero_since)zero_since=now;
            G3Sample s;memset(&s,0,sizeof s);
            s.now=now;s.rt_s=100+now/S;s.heading_valid=gps;s.heading_deg=heading;s.unc_deg=5;
            s.position_valid=gps;s.lat=35.5;s.lon=139.5;s.pos_err_m=12;
            s.motion.distance_m=dist;s.motion.speed_mps=kmh/3.6;
            s.motion.last_speed_ns=feed?now:0;s.motion.last_moving_ns=last_moving;s.motion.zero_since_ns=zero_since;
            s.motion.first_move_ns=last_moving?1:0;s.wheels_fresh=feed;s.wheels_moving=moving;
            s.reverse=0;s.yaw_error_rad_s=0.002;s.rotation_per_rad=0.10;
            s.beta_replacing=replacing;s.writer_lag_ns=lag_ns;
            if(t.token_wanted())s.token_why=w.check(now);
            t.tick(j,s,0,block);pump();
            if(const char* due=t.token_sync_due(replacing)) {
                G3IoResult r={G3_IO_RENAME,EIO,0,G3_STEP_RENAME,now,now};
                if(!sync_fail)r=g3_last_start_write_io(dir.c_str(),id,G3_LAST_START_SYNC_TEMP);
                if(r.stage==G3_IO_OK)landed.push_back('t');
                t.token_sync(j,now,due,r);
            }
        }
    }
};
static std::string landed(const ChainBoot& b) { return std::string(b.landed.begin(),b.landed.end()); }
// Boot A: drives with GPS, parks 10 s: the token lands before its S record.
static void chain_boot_a(const std::string& d) {
    ChainBoot a(d,BOOT_A);a.gps=true;
    CHECK(a.t.token_lazy() && a.t.token_wanted() && !strcmp(a.prev,"none"));
    a.run(20,30);a.run(0,10);
    CHECK(landed(a)=="TS" && token_of(d)==token_line(BOOT_A));
    CHECK(has(a.j.last("\"event\":\"token\""),"\"reason\":\"moved\",\"result\":\"written\"") &&
          has(a.j.last("\"event\":\"token\""),"\"garage\":1,\"sync\":0"));
    CHECK(has(a.j.last("g3_store"),"\"event\":\"loaded\"")==false);
    G3Record r;CHECK(g3_load(d.c_str(),&r)==G3_LOAD_OK && r.state=='S' && r.flags==7 && !strcmp(r.boot_id,BOOT_A));
}
// Boot C starts on a copy of the directory (a later check of the same chain
// must not see C's own worker-boot mark).
static void chain_expect(const std::string& d,const char* reason,const char* prev8) {
    const std::string x=temp_dir();
    const char* names[]={G3_LAST_START_NAME,G3_STORE_NAME,G3_WORKER_BOOT_NAME};
    for(unsigned i=0;i<3;++i)if(access((d+"/"+names[i]).c_str(),F_OK)==0)write_file(x+"/"+names[i],read_file(d+"/"+names[i]));
    ChainBoot c(x,BOOT_C);
    CHECK(!strcmp(c.t.reason(),reason));
    char want[32];snprintf(want,sizeof want,"\"prev\":\"%s\",\"swap\":\"lazy\"",prev8);
    CHECK(has(c.j.last("g3_store"),want));
}
static void token_chain() {
    // The real scenario: A drives and parks, B only stands (ignition on, no
    // GPS, wheels exactly zero for 2 min), C starts: chain_depart (HEAD:
    // not_prev_boot, the defect).
    { const std::string d=temp_dir();chain_boot_a(d);
      ChainBoot b(d,BOOT_B);CHECK(!strcmp(b.prev,BOOT_A) && !strcmp(b.t.reason(),"chain_depart"));
      b.run(0,120);
      CHECK(b.landed.empty() && b.j.count("\"event\":\"token\"")==0 && token_of(d)==token_line(BOOT_A) &&
            b.t.token_wanted());
      CHECK(read_file(d+"/"+G3_WORKER_BOOT_NAME)==std::string("mx5g3worker 1 ")+BOOT_B+"\n");
      ChainBoot c(d,BOOT_C);
      CHECK(c.t.state()==G3_CARRIED && !strcmp(c.t.reason(),"chain_depart") && !strcmp(c.prev,BOOT_A)); }
    // HEAD's eager swap at B's start (the defect, kept for a marker boot):
    // C refuses A's record.
    { const std::string d=temp_dir();chain_boot_a(d);
      ChainBoot b(d,BOOT_B,true,true);b.run(0,120);CHECK(token_of(d)==token_line(BOOT_B));
      chain_expect(d,"not_prev_boot","bbbbbbbb"); }
    // B rolls 1 m (below 2 m): no token write; the departure mark (0.5 m)
    // lands, so C reads an M record (moving_at_end).
    { const std::string d=temp_dir();chain_boot_a(d);
      ChainBoot b(d,BOOT_B);b.run(0,20);b.run(1.8,2);b.run(0,30);
      CHECK(b.dist>0.9 && b.dist<1.1 && landed(b)=="M" && token_of(d)==token_line(BOOT_A) &&
            b.j.count("\"event\":\"token\"")==0);
      chain_expect(d,"moving_at_end","aaaaaaaa"); }
    // ... with the mark blocked the whole boot (documented residual): A's S
    // survives and C carries it (at most about 25 deg of turning fits in 2 m).
    { const std::string d=temp_dir();chain_boot_a(d);
      ChainBoot b(d,BOOT_B);b.block="journal_draining";b.run(0,20);b.run(1.8,2);b.run(0,30);
      CHECK(b.landed.empty() && token_of(d)==token_line(BOOT_A));
      chain_expect(d,"chain_depart","aaaaaaaa"); }
    // B moves 3 m then the ignition goes off. Order 1: the mark lands at
    // 0.5 m, the token at 2 m (5 s spacing): C reads B's M.
    { const std::string d=temp_dir();chain_boot_a(d);
      ChainBoot b(d,BOOT_B);b.run(0,20);b.run(1.8,6);b.run(0,4);
      CHECK(b.dist>2.9 && b.dist<3.1 && landed(b)=="MT" && token_of(d)==token_line(BOOT_B));
      CHECK(has(b.j.last("\"event\":\"token\""),"\"reason\":\"moved\""));
      chain_expect(d,"moving_at_end","bbbbbbbb");
      // A park of 5 s after it: B's own S record (no heading: no GPS) after the token.
      b.run(0,6);CHECK(landed(b)=="MTS");chain_expect(d,"no_heading","bbbbbbbb"); }
    // Order 2: the token lands first (the mark waits for the 5 s spacing and
    // the ignition goes off before it): A's S is still on disk but C refuses
    // it because B is the previous product boot.
    { const std::string d=temp_dir();chain_boot_a(d);
      ChainBoot b(d,BOOT_B);b.run(0,20);b.block="journal_draining";b.run(10.8,1);   // 3 m in 1 s, mark held
      b.block=0;b.run(0,1);
      CHECK(landed(b)=="T" && token_of(d)==token_line(BOOT_B));
      G3Record r;CHECK(g3_load(d.c_str(),&r)==G3_LOAD_OK && r.state=='S' && !strcmp(r.boot_id,BOOT_A));
      chain_expect(d,"not_prev_boot","bbbbbbbb");
      // ... and once the mark lands too (5 s later, rolling on), C reads B's M.
      b.run(3.6,6);CHECK(landed(b)=="TM");chain_expect(d,"moving_at_end","bbbbbbbb"); }
    // B drives and parks with a GPS heading: the token lands BEFORE B's S
    // record, and C carries B's record (the record's writer is the token).
    { const std::string d=temp_dir();chain_boot_a(d);
      ChainBoot b(d,BOOT_B);b.gps=true;b.run(0,5);b.run(20,20);b.run(0,15);
      CHECK(landed(b)=="MTS" && token_of(d)==token_line(BOOT_B));
      G3Record r;CHECK(g3_load(d.c_str(),&r)==G3_LOAD_OK && r.state=='S' && !strcmp(r.boot_id,BOOT_B));
      ChainBoot c(d,BOOT_C);CHECK(!strcmp(c.t.reason(),"chain_depart") && !strcmp(c.prev,BOOT_B)); }
    // The first S request is diverted to the token even without 2 m (turned:
    // a GPS heading at a park after < 2 m): token first, the S 5 s later.
    { const std::string d=temp_dir();
      ChainBoot b(d,BOOT_B);b.gps=true;b.run(0,12);
      CHECK(landed(b)=="TS" && has(b.j.last("\"event\":\"token\""),"\"reason\":\"record\""));
      G3Record r;CHECK(g3_load(d.c_str(),&r)==G3_LOAD_OK && !strcmp(r.boot_id,BOOT_B) && token_of(d)==token_line(BOOT_B)); }
    // Blind boots swap: no wheel event at all for 30 s ...
    { const std::string d=temp_dir();chain_boot_a(d);
      ChainBoot b(d,BOOT_B);b.feed=false;b.run(0,29.5);CHECK(b.landed.empty());
      b.run(0,1.5);CHECK(landed(b)=="T" && has(b.j.last("\"event\":\"token\""),"\"reason\":\"blind_silent\""));
      chain_expect(d,"not_prev_boot","bbbbbbbb"); }
    // ... or a wheel stream gap above 2 s that ends.
    { const std::string d=temp_dir();chain_boot_a(d);
      ChainBoot b(d,BOOT_B);b.run(0,10);b.feed=false;b.run(0,2.5);b.feed=true;b.run(0,1);
      CHECK(landed(b)=="T" && has(b.j.last("\"event\":\"token\""),"\"reason\":\"blind_gap\""));
      chain_expect(d,"not_prev_boot","bbbbbbbb"); }
    // A gap of 1.5 s does not count.
    { const std::string d=temp_dir();chain_boot_a(d);
      ChainBoot b(d,BOOT_B);b.run(0,10);b.feed=false;b.run(0,1.5);b.feed=true;b.run(0,10);
      CHECK(b.landed.empty() && token_of(d)==token_line(BOOT_A)); }
    // Corrupt or missing token: C refuses ("unread"/"none"); an idle lazy boot
    // leaves the file as it is; a boot that moves rewrites it.
    { const std::string d=temp_dir();chain_boot_a(d);
      write_file(d+"/"+G3_LAST_START_NAME,"garbage\n");
      ChainBoot b(d,BOOT_B);CHECK(!strcmp(b.prev,"unread") && !strcmp(b.t.reason(),"not_prev_boot"));
      b.run(0,60);CHECK(token_of(d)=="garbage\n");
      chain_expect(d,"not_prev_boot","unread");
      ChainBoot e(d,BOOT_B);   // a second start of B: eager, prev = itself
      CHECK(!strcmp(e.swap,"eager_restart") && !strcmp(e.prev,BOOT_B) && !e.t.token_lazy() &&
            token_of(d)==token_line(BOOT_B) && !strcmp(e.t.reason(),"not_prev_boot")); }
    { const std::string d=temp_dir();chain_boot_a(d);unlink((d+"/"+G3_LAST_START_NAME).c_str());
      ChainBoot b(d,BOOT_B);CHECK(!strcmp(b.prev,"none") && !strcmp(b.t.reason(),"not_prev_boot"));
      b.run(20,6);CHECK(landed(b)=="MT" && token_of(d)==token_line(BOOT_B)); }
    // A second worker start of the same boot after an idle first worker: the
    // gap between them was not observed: swap at once and refuse A's record.
    { const std::string d=temp_dir();chain_boot_a(d);
      { ChainBoot b(d,BOOT_B);b.run(0,10);CHECK(token_of(d)==token_line(BOOT_A)); }
      ChainBoot b2(d,BOOT_B);
      CHECK(!strcmp(b2.swap,"eager_restart") && token_of(d)==token_line(BOOT_B) && !strcmp(b2.t.reason(),"not_prev_boot"));
      CHECK(has(b2.j.last("g3_store"),"\"prev\":\"bbbbbbbb\",\"swap\":\"eager_restart\"")); }
    // The token already names this boot (the first worker swapped): nothing to do.
    { const std::string d=temp_dir();g3_last_start_write(d.c_str(),BOOT_B);
      ChainBoot b(d,BOOT_B);CHECK(b.t.token_lazy() && b.t.token_done() && !b.t.token_wanted()); }
}
static void token_gate() {
    // Retry when the gated write is blocked: the idle rule holds the request,
    // a cancelled write is requested again after the 5 s spacing, a failure
    // counts toward the store's three failures.
    { const std::string d=temp_dir();chain_boot_a(d);
      ChainBoot b(d,BOOT_B);b.block="journal_draining";b.run(0,1);b.run(20,20);
      CHECK(b.landed.empty() && token_of(d)==token_line(BOOT_A));
      b.block=0;b.force_stage=G3_IO_CANCELLED;b.run(20,0.1);
      CHECK(b.j.count("\"event\":\"token\"")==1 && has(b.j.last("\"event\":\"token\""),"\"result\":\"cancelled\""));
      b.force_stage=-1;b.run(20,4.8);CHECK(b.landed.empty());
      b.run(20,0.3);CHECK(landed(b)=="T" && token_of(d)==token_line(BOOT_B)); }
    // While BETA replaces: only with a journal writer lag <= 100 ms.
    { const std::string d=temp_dir();
      G3Tracker t;t.start(0,true,"m",BOOT_B,"none",100,G3_LOAD_MISSING,g3_record(),true,BOOT_A,"lazy");Rows j;
      G3Sample s=drive(1*S,10);s.token_why="moved";s.beta_replacing=true;s.writer_lag_ns=G3_GARAGE_REPLACING_MARK_LAG_NS+1;
      t.tick(j,s,0,0);char b[G3_RECORD_MAX];size_t n;uint32_t q;unsigned target;
      CHECK(!t.take_request(b,sizeof b,&n,&q,0,&target));
      s.now=2*S;s.writer_lag_ns=G3_GARAGE_REPLACING_MARK_LAG_NS;t.tick(j,s,0,0);
      // A caller without a target never receives the token line; the refused
      // request is dropped (review P4) with its spacing restored, so the
      // next tick raises a new one at once.
      CHECK(!t.take_request(b,sizeof b,&n,&q));
      CHECK(!t.take_request(b,sizeof b,&n,&q,0,&target) && !t.in_flight_token());
      s.now=2*S+100000000ULL;t.tick(j,s,0,0);
      CHECK(t.take_request(b,sizeof b,&n,&q,0,&target) && target==G3_TARGET_TOKEN && std::string(b,n)==token_line(BOOT_B) &&
            q==G3_TOKEN_SEQ_BASE+1 && t.in_flight_token() && t.in_flight_static());
      // Static content: a moving wheel or a stale stream does not cancel it;
      // the idle rule does.
      s.wheels_moving=true;CHECK(!g3_cancel_reason(0,s,t));
      s.wheels_fresh=false;CHECK(!g3_cancel_reason(0,s,t));
      CHECK(!strcmp(g3_cancel_reason("journal_lagging",s,t),"journal_lagging"));
      // The mailbox refused: requested again (not a failure).
      t.post_failed();CHECK(!t.in_flight_token() && t.token_wanted());
      s.now=8*S;s.wheels_moving=false;s.wheels_fresh=true;t.tick(j,s,0,0);
      CHECK(t.take_request(b,sizeof b,&n,&q,0,&target) && target==G3_TARGET_TOKEN);
      G3IoResult f={G3_IO_RENAME,EIO,0,G3_STEP_NONE,0,0};t.store_result(j,8*S,q,f);
      CHECK(has(j.last("\"event\":\"token\""),"\"result\":\"failed\",") && t.token_wanted() && t.store_on()); }
    // The mailbox routes the token line to g3-last-start (store thread path).
    { const std::string d=temp_dir();static G3Mailbox m;CHECK(g3_mailbox_init(m,d.c_str(),0));
      const std::string l=token_line(BOOT_A);
      CHECK(!g3_mailbox_post(m,l.data(),l.size(),G3_TOKEN_SEQ_BASE,1,0,false,7));
      CHECK(g3_mailbox_post(m,l.data(),l.size(),G3_TOKEN_SEQ_BASE,1,0,false,G3_TARGET_TOKEN));
      CHECK(g3_mailbox_run_once(m,0) && token_of(d)==l && access((d+"/"+G3_STORE_NAME).c_str(),F_OK)!=0);
      G3IoResult r;CHECK(g3_mailbox_result(m,G3_TOKEN_SEQ_BASE,1,&r) && r.stage==G3_IO_OK);
      const std::string rec=encode(depart_record());
      CHECK(g3_mailbox_post(m,rec.data(),rec.size(),5,1,0,false));
      CHECK(g3_mailbox_run_once(m,0) && read_file(d+"/"+G3_STORE_NAME)==rec && token_of(d)==l); }
    // Every store-thread write AND the synchronous token attempt failed
    // (documented residual): B drove 30 m, the store went off after three
    // record failures (the token failures have their own limit and did not
    // count), A's S and A's token stay, and C carries A's stale record.
    { const std::string d=temp_dir();chain_boot_a(d);
      ChainBoot b(d,BOOT_B);b.force_stage=G3_IO_WRITE;b.sync_fail=true;b.run(20,6);b.run(0,30);
      CHECK(b.landed.empty() && !b.t.store_on() && token_of(d)==token_line(BOOT_A) && !b.t.token_wanted() &&
            !b.t.token_done());
      CHECK(b.j.count("\"event\":\"token\"")==4 && b.j.count("\"result\":\"sync_failed\"")==1);
      chain_expect(d,"chain_depart","aaaaaaaa");
      // A later terminal attempt (capture stop) adds nothing: one per tracker.
      const size_t rows=b.j.count("\"event\":\"token\"");
      G3IoResult ok={G3_IO_OK,0,0,G3_STEP_NONE,0,0};b.t.token_sync(b.j,41*S,"capture_stop",ok);
      CHECK(b.j.count("\"event\":\"token\"")==rows); }
    // The terminal synchronous write (journal fault / capture stop) closes a
    // boot whose store thread never wrote the token.
    { const std::string d=temp_dir();chain_boot_a(d);
      ChainBoot b(d,BOOT_B);b.block="journal_draining";b.run(20,6);
      CHECK(b.landed.empty() && b.t.token_wanted() && !b.t.token_sync_due(false));
      G3IoResult ok={G3_IO_OK,0,0,G3_STEP_NONE,0,0};b.t.token_sync(b.j,40*S,"capture_stop",ok);
      CHECK(!b.t.token_wanted() && b.t.token_done() &&
            has(b.j.last("\"event\":\"token\""),"\"reason\":\"moved\",\"result\":\"sync\""));
      const size_t rows=b.j.count("\"event\":\"token\"");
      b.t.token_sync(b.j,41*S,"capture_stop",ok);CHECK(b.j.count("\"event\":\"token\"")==rows); }
    { G3Tracker t;t.start(0,true,"m",BOOT_B,"none",100,G3_LOAD_MISSING,g3_record(),true,BOOT_A,"lazy");Rows j;
      G3IoResult f={G3_IO_OPEN,EACCES,0,G3_STEP_NONE,0,0};t.token_sync(j,1,"journal_failed",f);
      CHECK(!t.token_wanted() && !t.token_done() &&
            has(j.last("\"event\":\"token\""),"\"reason\":\"journal_failed\",\"result\":\"sync_failed\"")); }
    // Marker semantics: with the garage switch off the tracker never writes the
    // token (the runtime swapped it at start, as before) and has no "swap".
    { G3Tracker t;t.start(0,true,"m",BOOT_B,"none",100,G3_LOAD_MISSING,g3_record(),false,BOOT_A,"lazy");
      CHECK(!t.token_lazy() && !t.token_wanted());Rows j;t.journal_start(j,0);
      CHECK(!has(j.last("g3_store"),"swap") && !has(j.last("g3_store"),"prev"));
      G3Sample s=drive(1*S,10);s.token_why="moved";t.tick(j,s,0,0);
      char b[G3_RECORD_MAX];size_t n;uint32_t q;unsigned target;CHECK(!t.take_request(b,sizeof b,&n,&q,0,&target)); }
    // Garage on but the caller swapped (swap 0): no lazy token, no "swap" field.
    { G3Tracker t;t.start(0,true,"m",BOOT_B,"none",100,G3_LOAD_MISSING,g3_record(),true,BOOT_A);
      CHECK(!t.token_lazy() && !t.token_wanted());Rows j;t.journal_start(j,0);
      CHECK(has(j.last("g3_store"),"\"prev\":\"aaaaaaaa\"}") && !has(j.last("g3_store"),"swap")); }
    // An unknown boot id has nothing to write.
    { G3Tracker t;t.start(0,true,"m","unknown","none",100,G3_LOAD_MISSING,g3_record(),true,BOOT_A,"lazy");
      CHECK(t.token_lazy() && !t.token_wanted()); }
}

// Fifth fix (validation/G3_GARAGE_EXIT_FIX_REVIEW4_FABLE_2026-10-18.md).
static void token_fifth() {
    // C3, the reviewer's probe: BETA replaces for the whole of B with a
    // journal writer lag of 150 ms (the 100-300 ms band of the third review:
    // the departure mark waits, the S lands). A drove and parked (S(A),
    // token A); B drives 16 m with a GPS heading and parks 40 s; C starts.
    // HEAD (fourth fix): nothing landed (mark, token and the diverted S all
    // under the 100 ms rule) and C carried A's stale heading. Now the token
    // diverted from the park S request takes the S gate: token, then B's S.
    { const std::string d=temp_dir();chain_boot_a(d);
      ChainBoot b(d,BOOT_B);b.gps=true;b.replacing=true;b.lag_ns=150000000ULL;b.heading=290;
      b.run(0,5);b.run(20,2.9);
      CHECK(b.dist>15.9 && b.dist<16.2 && b.landed.empty() && b.t.token_why() && same(b.t.token_why(),"moved"));
      // Order 1 on disk: the token landed at the park, B's S not yet (the
      // ignition goes off within the 5 s spacing): A's S, token B -> refused.
      b.run(0,5.5);
      CHECK(landed(b)=="T" && token_of(d)==token_line(BOOT_B));
      { G3Record r;CHECK(g3_load(d.c_str(),&r)==G3_LOAD_OK && r.state=='S' && !strcmp(r.boot_id,BOOT_A)); }
      chain_expect(d,"not_prev_boot","bbbbbbbb");
      // Order 2: B's S follows 5 s later: S(B) with heading, token B -> C
      // carries B's own record (not A's).
      b.run(0,34.5);
      CHECK(landed(b)=="TS" && token_of(d)==token_line(BOOT_B));
      CHECK(has(b.j.last("\"event\":\"token\""),"\"reason\":\"moved\",\"result\":\"written\""));
      { G3Record r;CHECK(g3_load(d.c_str(),&r)==G3_LOAD_OK && r.state=='S' && !strcmp(r.boot_id,BOOT_B) &&
                         (r.flags&G3_FLAG_HEADING) && r.heading_cdeg==29000); }
      const std::string x=temp_dir();
      write_file(x+"/"+G3_LAST_START_NAME,read_file(d+"/"+G3_LAST_START_NAME));
      write_file(x+"/"+G3_STORE_NAME,read_file(d+"/"+G3_STORE_NAME));
      ChainBoot c(x,BOOT_C);
      CHECK(c.t.state()==G3_CARRIED && same(c.t.reason(),"chain_depart") && !strcmp(c.prev,BOOT_B));
      CHECK(has(c.j.last("g3_store"),"\"h\":2900")); }
    // While moving the token keeps the departure mark's gate: a lag of
    // 150 ms while BETA replaces holds it (and the mark); 100 ms lets both go.
    { const std::string d=temp_dir();chain_boot_a(d);
      ChainBoot b(d,BOOT_B);b.replacing=true;b.lag_ns=150000000ULL;b.run(20,10);
      CHECK(b.landed.empty() && token_of(d)==token_line(BOOT_A));
      b.lag_ns=G3_GARAGE_REPLACING_MARK_LAG_NS;b.run(20,0.1);CHECK(landed(b)=="T");b.run(20,5.1);CHECK(landed(b)=="TM"); }
    // A parked S that is spaced (replacing_rate) or blocked by the idle rule
    // also holds the diverted token: the S gate is the whole S gate.
    { const std::string d=temp_dir();chain_boot_a(d);
      ChainBoot b(d,BOOT_B);b.gps=true;b.replacing=true;b.lag_ns=150000000ULL;b.run(20,3);
      b.block="journal_lagging";b.run(0,20);CHECK(b.landed.empty());
      b.block=0;b.run(0,0.2);CHECK(landed(b)=="T"); }
    // P3: token write failures have their own limit and never turn the store
    // off; after three the worker makes ONE synchronous attempt (landed 't'),
    // and the boot's own S still lands after it.
    { const std::string d=temp_dir();chain_boot_a(d);
      ChainBoot b(d,BOOT_B);b.gps=true;b.force_token=G3_IO_RENAME;b.run(20,30);
      CHECK(landed(b)=="Mt" && b.t.store_on() && b.t.token_done() && token_of(d)==token_line(BOOT_B));
      CHECK(b.j.count("\"event\":\"token\"")==4 && b.j.count("\"result\":\"failed\"")==3 &&
            has(b.j.last("\"event\":\"token\""),"\"reason\":\"moved\",\"result\":\"sync\"") &&
            b.j.count("\"disabled\":true")==0);
      b.run(0,10);CHECK(landed(b)=="MtS");
      chain_expect(d,"chain_depart","bbbbbbbb"); }
    // ... never while BETA replaces (no worker file I/O on the G1 path): it
    // stays due and runs once BETA stops replacing.
    { const std::string d=temp_dir();chain_boot_a(d);
      ChainBoot b(d,BOOT_B);b.force_token=G3_IO_RENAME;b.replacing=true;b.run(20,30);
      CHECK(landed(b)=="M" && !b.t.token_sync_due(true) && same(b.t.token_sync_due(false),"token_failed") && b.t.token_wanted());
      // No further store-thread token attempt after the limit (it waits for the synchronous one).
      CHECK(b.j.count("\"result\":\"failed\"")==3);
      b.replacing=false;b.run(20,0.1);CHECK(landed(b)=="Mt" && !b.t.token_sync_due(false)); }
    // ... and when the synchronous attempt fails too, the S is no longer held
    // for the token: S(B) with token A on disk -> C refuses (not_prev_boot)
    // instead of carrying A's stale S.
    { const std::string d=temp_dir();chain_boot_a(d);
      ChainBoot b(d,BOOT_B);b.gps=true;b.force_token=G3_IO_RENAME;b.sync_fail=true;b.run(20,30);b.run(0,10);
      CHECK(landed(b)=="MS" && b.t.store_on() && !b.t.token_wanted() && !b.t.token_done() &&
            token_of(d)==token_line(BOOT_A) && b.j.count("\"result\":\"sync_failed\"")==1);
      { G3Record r;CHECK(g3_load(d.c_str(),&r)==G3_LOAD_OK && r.state=='S' && !strcmp(r.boot_id,BOOT_B)); }
      chain_expect(d,"not_prev_boot","aaaaaaaa"); }
    // P3: the store went off for record failures (three failed marks) while
    // the token is due (moved): one synchronous attempt ("store_off").
    { const std::string d=temp_dir();chain_boot_a(d);
      ChainBoot b(d,BOOT_B);b.force_store=G3_IO_WRITE;b.run(0.36,17);
      CHECK(b.landed.empty() && !b.t.store_on() && b.t.token_wanted() && !b.t.token_sync_due(false));
      b.run(0.36,4);
      CHECK(landed(b)=="t" && token_of(d)==token_line(BOOT_B) && b.j.count("\"result\":\"sync\"")==1);
      chain_expect(d,"not_prev_boot","bbbbbbbb"); }
    // ... or wants an S record (turned at a park) that the store can no
    // longer write: trigger "record", synchronous attempt.
    { const std::string d=temp_dir();chain_boot_a(d);
      ChainBoot b(d,BOOT_B);b.force_store=G3_IO_WRITE;b.run(0.36,16);
      CHECK(b.landed.empty() && !b.t.store_on() && b.dist<1.7);
      b.gps=true;b.heading=300;b.run(0,8);
      CHECK(landed(b)=="t" && token_of(d)==token_line(BOOT_B) &&
            has(b.j.last("\"event\":\"token\""),"\"reason\":\"record\",\"result\":\"sync\""));
      chain_expect(d,"not_prev_boot","bbbbbbbb"); }
    // P4: a caller without a target drops the token request; once the token
    // landed (here: the synchronous path) S and M requests reach that caller
    // again (HEAD: the token line stayed pending and blocked them forever).
    { G3Tracker t;t.start(0,true,"m",BOOT_B,"none",100,G3_LOAD_MISSING,g3_record(),true,BOOT_A,"lazy");Rows j;
      G3Sample s=drive(1*S,10);s.token_why="moved";t.tick(j,s,0,0);
      char b[G3_RECORD_MAX];size_t n;uint32_t q;
      CHECK(!t.take_request(b,sizeof b,&n,&q) && !t.in_flight(&q));
      G3IoResult ok={G3_IO_OK,0,0,G3_STEP_NONE,0,0};t.token_sync(j,2*S,"capture_stop",ok);CHECK(t.token_done());
      t.tick(j,park(30*S,10,20*S),0,0);
      CHECK(t.take_request(b,sizeof b,&n,&q) && std::string(b,n).find(" S ")!=std::string::npos); }
    // A token request still pending when the synchronous write landed is
    // dropped (no second, needless token write through the store thread).
    { G3Tracker t;t.start(0,true,"m",BOOT_B,"none",100,G3_LOAD_MISSING,g3_record(),true,BOOT_A,"lazy");Rows j;
      G3Sample s=drive(1*S,10);s.token_why="moved";t.tick(j,s,0,0);
      G3IoResult ok={G3_IO_OK,0,0,G3_STEP_NONE,0,0};t.token_sync(j,2*S,"capture_stop",ok);
      char b[G3_RECORD_MAX];size_t n;uint32_t q;unsigned target=99;
      CHECK(!t.take_request(b,sizeof b,&n,&q,0,&target) && !t.in_flight_token()); }
    // R1: the raw 2 m threshold is inclusive (exactly 2.000 m: moved).
    { G3TokenWatch w;w.start(0,0.01,-100.0);uint16_t raw[4];raw_wheels(3.6,raw);
      w.wheel(1*S,raw);w.speed_mps=1.0;w.distance_m=1.5;w.wheel(1*S+500000000ULL,raw);
      CHECK(w.distance_m==2.0 && same(w.check(2*S),"moved")); }
    { G3TokenWatch w;w.start(0,0.01,-100.0);uint16_t raw[4];raw_wheels(3.6,raw);
      w.wheel(1*S,raw);w.speed_mps=1.0;w.distance_m=1.4;w.wheel(1*S+500000000ULL,raw);
      CHECK(w.distance_m<2.0 && !w.check(2*S)); }
    // R2: the pipeline distance branch alone (the raw watch saw nothing):
    // exactly 2.000 m requests the token ("moved"), 1.99 m does not.
    { G3Tracker t;t.start(0,true,"m",BOOT_B,"none",100,G3_LOAD_MISSING,g3_record(),true,BOOT_A,"lazy");Rows j;
      G3Sample s=drive(1*S,1.99);s.token_why=0;t.tick(j,s,0,0);
      char b[G3_RECORD_MAX];size_t n;uint32_t q;unsigned target=99;
      CHECK(!t.take_request(b,sizeof b,&n,&q,0,&target) && !t.token_why());
      s=drive(2*S,2.0);s.token_why=0;t.tick(j,s,0,0);
      CHECK(t.take_request(b,sizeof b,&n,&q,0,&target) && target==G3_TARGET_TOKEN && same(t.token_why(),"moved")); }
}

int main(int argc,char** argv) {
    if(argc==3 && !strcmp(argv[1],"--two-boot"))return two_boot(argv[2]);
    codec();
    file_io();
    chain_and_switch();
    restore_rules();
    first_fix_checks();
    course_and_session();
    io_rule();
    store_policy();
    carried_reference();
    park_rules();
    fsync_policy();
    gated_write();
    store_thread();
    two_boot_roundtrip();
    pipeline_motion();
    pipeline_park_detection();
    pipeline_boot_motion();
    moving_at_boot();
    yaw_chain_breaks();
    rule_f_discard();
    rule_f_discard_in_flight();
    g3_send_rules();
    garage_codec_restore();
    garage_stored_uncertainty();
    garage_store();
    garage_send();
    garage_latch_pipeline();
    last_start_token();
    token_codec();
    token_watch();
    token_chain();
    token_gate();
    token_fifth();
    g3_send_adapter();   // configures the process-global adapter: last
    std::printf("test_g3: %u checks passed\n",checks);
    return 0;
}
