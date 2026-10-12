// G3 record-only heading carry-over host tests (validation/G3_RECORD_ONLY_2026-10-11.md):
// store codec, atomic file I/O and its failure paths, the guard chain token,
// the off switch, the restore / continuity / course state machine, the store
// policy, journal rows and the pipeline's G3Motion observation. Synthetic
// inputs only; not vehicle, phone or DHU evidence.
//   test_g3                 every unit check
//   test_g3 --two-boot DIR  prints a synthetic two-boot JSONL (real tracker
//                           rows, real store file in DIR) for the analyzer test
#include "runtime/g3_heading.h"
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
        "mx5g3 1 7 aaaaaaaa-1111-2222-3333-444444444444 S 4 100 1 0 0 0 0 0 0 none",       // flags
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
    std::printf("test_g3: %u checks passed\n",checks);
    return 0;
}
