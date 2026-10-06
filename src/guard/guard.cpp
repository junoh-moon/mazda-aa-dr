// Pre-Service-Manager gate: one-boot trial (arm/consumed) or the persistent
// BETA product (persist/persist-state) with an automatic reset fail-safe.
// No experimental DSO is loaded here.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../runtime/sha256.h"
#include <sys/stat.h>
#include <sys/file.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <algorithm>

namespace {
std::string prefix;
const char *const names[]={"/data_persist/mx5-aa-dr/libmx5dr.so", "/data_persist/mx5-aa-dr/mx5dr.conf", "/jci/sm/sm.conf", "/data_persist/mx5-aa-dr/guard/normal.trial", "/jci/sm/sm_WCP.conf", "/data_persist/mx5-aa-dr/guard/wcp.trial", "/data_persist/mx5-aa-dr/libmx5dr-vimtap.so", "/data_persist/mx5-aa-dr/libmx5dr-ldstap.so"};
const char *const token="/data_persist/mx5-aa-dr/libmx5dr.so";
const char *const tap_token="/data_persist/mx5-aa-dr/libmx5dr-vimtap.so";
const char *const lds_token="/data_persist/mx5-aa-dr/libmx5dr-ldstap.so";
int gd=-1;
// Why the last check declined, as a short code for the owner (menu 2).
std::string why;
const char *const short_names[]={"libmx5dr.so","mx5dr.conf","sm.conf","normal.trial","sm_WCP.conf","wcp.trial","libmx5dr-vimtap.so","libmx5dr-ldstap.so"};
uid_t expected_owner(){
#ifdef MX5DR_GUARD_TESTING
 return geteuid(); // Fixture files belong to the host test user; production stays root-only.
#else
 return 0;
#endif
}
bool safe_stat(int fd,bool directory){struct stat s;return fstat(fd,&s)==0 && s.st_uid==expected_owner() && !(s.st_mode&022) && (directory?S_ISDIR(s.st_mode):S_ISREG(s.st_mode));}
bool stock_alias(const std::string &path,const char *absolute,const char *relative,bool &link){
 struct stat st;if(lstat(path.c_str(),&st))return false;link=S_ISLNK(st.st_mode);
 if(!link)return S_ISDIR(st.st_mode);
 char target[64];ssize_t n=readlink(path.c_str(),target,sizeof target);
 if(n<0||n==ssize_t(sizeof target))return false;
 const std::string value(target,size_t(n));return value==absolute||value==relative;
}
// The installation directory itself, through the stock persistent aliases.
int open_base(){
 // Preserve both stock aliases, including when mapping a test fixture root.
 bool link=false;std::string storage=prefix+"/data_persist";
 if(!stock_alias(storage,"/mnt/data_persist","mnt/data_persist",link))return -1;
 if(link){
  if(!stock_alias(prefix+"/mnt","/tmp/mnt","tmp/mnt",link))return -1;
  storage=prefix+(link?"/tmp/mnt/data_persist":"/mnt/data_persist");
 }
 // Only our installation subtree must be UID 0 owned and non-writable by cmu.
 int fd=open((storage+"/mx5-aa-dr").c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
 if(fd<0)return -1;
 if(!safe_stat(fd,true)){close(fd);return -1;}
 return fd;
}
int trusted(const std::string &path,bool directory=false){
 // OEM sm.conf and its parents are 0775 in this exact firmware. Bind their
 // bytes in the arm manifest; do not impose our private-directory permissions
 // on the stock OS or chown/chmod it. Reject a replaced final symlink.
 if(path==prefix+"/jci/sm/sm.conf"||path==prefix+"/jci/sm/sm_WCP.conf"){
  int fd=open(path.c_str(),O_RDONLY|O_NOFOLLOW|O_CLOEXEC);struct stat st;
  if(fd>=0&&(fstat(fd,&st)||!S_ISREG(st.st_mode))){close(fd);return -1;}
  return fd;
 }
 const std::string base=prefix+"/data_persist/mx5-aa-dr";
 if(path.compare(0,base.size()+1,base+"/")!=0)return -1;
 int fd=open_base();
 if(fd<0)return -1;
 size_t i=base.size()+1;
 while(i<path.size()){
  size_t end=path.find('/',i);bool last=end==std::string::npos;
  std::string c=path.substr(i,last?path.size()-i:end-i);
  if(c.empty()||c=="."||c==".."){close(fd);return -1;}
  int n=openat(fd,c.c_str(),O_RDONLY|O_NOFOLLOW|O_CLOEXEC|((!last||directory)?O_DIRECTORY:0));close(fd);fd=n;
  if(fd<0)return -1;
  if(!safe_stat(fd,!last||directory)){close(fd);return -1;}
  if(last)break;
  i=end+1;
 }
 return fd;
}
bool read_fd(int fd,std::string &s,size_t cap){s.clear();char b[4096];for(;;){ssize_t n=read(fd,b,sizeof b);if(n<0){if(errno==EINTR)continue;return false;}if(!n)return true;if(s.size()+size_t(n)>cap)return false;s.append(b,size_t(n));}}
bool read_file(const std::string&p,std::string&s,size_t cap){int fd=trusted(p);if(fd<0)return false;bool ok=read_fd(fd,s,cap);close(fd);return ok;}
bool digest(const std::string&p,std::string&out){std::string s;if(!read_file(p,s,32*1024*1024))return false;char h[65];mx5_sha256_bytes(s.data(),s.size(),h);out=h;return true;}
bool write_all(int fd,const std::string&s){size_t n=0;while(n<s.size()){ssize_t w=write(fd,s.data()+n,s.size()-n);if(w<0&&errno==EINTR)continue;if(w<=0)return false;n+=size_t(w);}return true;}
// Test-only: block at a named stage so the time bound can be exercised.
void hang_point(const char*stage){
#ifdef MX5DR_GUARD_TESTING
 const char*h=getenv("MX5DR_GUARD_HANG");
 if(h&&!strcmp(h,stage))for(;;)pause();
#else
 (void)stage;
#endif
}
bool sync_dir(int fd,const char*stage){
#ifdef MX5DR_GUARD_TESTING
 const char*fail=getenv("MX5DR_GUARD_FAIL_FSYNC");
 if(fail&&(std::string(",")+fail+",").find(std::string(",")+stage+",")!=std::string::npos){errno=EIO;return false;}
#else
 (void)stage;
#endif
 hang_point("fsync");
 return fsync(fd)==0;
}
bool atomic_file(const char*name,const std::string&s,bool*published=0){
 if(published)*published=false;
 char temp[96];snprintf(temp,sizeof temp,".%s.%ld",name,(long)getpid());
 // A power cut can leave this name behind and a later boot can reuse the PID.
 // Holding the guard lock, the name is ours: drop it (never followed), then O_EXCL.
 unlinkat(gd,temp,0);
 int fd=openat(gd,temp,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
 if(fd<0)return false;
 bool ok=write_all(fd,s)&&fsync(fd)==0;
 int e=close(fd);ok=ok&&e==0;
 if(ok){
  ok=renameat(gd,temp,gd,name)==0;
  if(ok){if(published)*published=true;ok=sync_dir(gd,name);}
 }
 if(!ok)unlinkat(gd,temp,0);
 return ok;
}
bool manifest(std::string&s){
 s="mx5dr-one-boot-v3\n";
 for(unsigned i=0;i<sizeof(names)/sizeof(names[0]);i++){
  std::string h;if(!digest(prefix+names[i],h)){why=std::string("input_unreadable:")+short_names[i];return false;}
  if(i==2||i==4){
   // Bind a template to the snapshot from which the editor created it. Merely
   // hashing today's baseline and yesterday's template would authorize both.
   std::string source;
   const char *name=i==2?"normal.source.sha256":"wcp.source.sha256";
   if(!read_file(prefix+"/data_persist/mx5-aa-dr/guard/"+name,source,65)||source!=h+"\n"){why=std::string("baseline_edited:")+short_names[i];return false;}
  }
  s+=h+"\n";
 }
 return true;
}
bool valid_boot_record(const std::string&s){if(s.size()!=37||s[36]!='\n')return false;for(size_t i=0;i<36;i++){if(i==8||i==13||i==18||i==23){if(s[i]!='-')return false;}else if(!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f')))return false;}return true;}
bool boot_id(std::string&s){std::string p=prefix+"/proc/sys/kernel/random/boot_id";int fd=open(p.c_str(),O_RDONLY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return false;bool ok=read_fd(fd,s,80);close(fd);return ok&&valid_boot_record(s);}
bool owned_read(const char*name,std::string&s){int fd=openat(gd,name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return false;bool ok=safe_stat(fd,false)&&read_fd(fd,s,1024);close(fd);return ok;}
bool last_boot_record(std::string&s){struct stat st;if(fstatat(gd,"last-boot",&st,AT_SYMLINK_NOFOLLOW)){return errno==ENOENT;}return owned_read("last-boot",s)&&valid_boot_record(s);}
bool baseline_clean(){for(unsigned i=2;i<=4;i+=2){std::string s;if(!read_file(prefix+names[i],s,1024*1024)||s.find(token)!=std::string::npos||s.find(tap_token)!=std::string::npos||s.find(lds_token)!=std::string::npos){why=std::string("baseline_has_token:")+short_names[i];return false;}}return true;}
// Temporary names of atomic_file() that a power cut can leave behind.
const char*const temp_names[]={"arm","last-boot","persist","persist-state","last-decision",0};
bool stale_temp_name(const char*n){
 if(n[0]!='.')return false;
 for(const char*const*k=temp_names;*k;k++){
  size_t l=strlen(*k);
  if(strncmp(n+1,*k,l)||n[1+l]!='.')continue;
  const char*d=n+2+l;size_t digits=strspn(d,"0123456789");
  if(digits>=1&&digits<=10&&!d[digits])return true;
 }
 return false;
}
// Under the guard lock: remove our own stale temporaries (regular, owned,
// never followed). Bounded scan; best effort.
void clean_stale_temps(){
 int fd=dup(gd);DIR*dir=fd<0?0:fdopendir(fd);
 if(!dir){if(fd>=0)close(fd);return;}
 bool removed=false;
 for(unsigned seen=0;seen<256;seen++){
  struct dirent*e=readdir(dir);if(!e)break;
  if(!stale_temp_name(e->d_name))continue;
  struct stat st;
  if(fstatat(gd,e->d_name,&st,AT_SYMLINK_NOFOLLOW)==0&&S_ISREG(st.st_mode)&&st.st_uid==expected_owner())
   removed=unlinkat(gd,e->d_name,0)==0||removed;
 }
 closedir(dir);
 if(removed)fsync(gd);
}
// ---- Persistent BETA product mode ------------------------------------------
// The persistent files never contain our preload either: every boot the guard
// decides again, from root-owned state, whether to publish a /tmp trial.
#ifndef MX5DR_GUARD_HEALTHY_RULE
#define MX5DR_GUARD_HEALTHY_RULE 0
#endif
// Boot-loop rule: without the runtime's `healthy` marker for
// HEALTHY_RULE_ATTEMPTS attempts, an attempt counts as failed. OFF in this
// build: no runtime writes the marker yet, so only the SM reset-report rule
// detects failures. The manifest records the value; a mismatch declines.
const bool healthy_rule=MX5DR_GUARD_HEALTHY_RULE!=0;
const unsigned TRIP_FAILURES=2;          // consecutive failed_reset attempts
const unsigned HEALTHY_RULE_ATTEMPTS=3;  // attempts since the last healthy marker
// Guard-owned confirmation (no runtime cooperation): the owned autostart
// block runs `confirm` CONFIRM_DELAY_S after a selection; it records the boot
// only while the SM still runs with that boot's trial configuration.
const unsigned UNCONFIRMED_TRIP=3;       // consecutive unconfirmed attempts
const unsigned RECENT_WINDOW=10;         // attempts kept in `recent`
const unsigned RECENT_RESET_TRIP=3;      // failed_reset attempts within the window
const unsigned MAX_COUNT=999999;
const unsigned MAX_REPORTS=64;           // *.out entries considered in /data
const size_t REPORT_READ_CAP=1024*1024;  // bytes hashed per report
bool uuid36(const std::string&s){return s.size()==36&&valid_boot_record(s+"\n");}
std::string persist_text(const std::string&expected){
 return std::string("mx5dr-persist-v1\nmode=BETA\nhealthy_rule=")+(healthy_rule?"on":"off")+"\n"+expected;
}
struct State{
 std::string enabled_boot,probation,recent,attempt_boot,attempt_reports,attempt_trial,confirmed_boot,previous,healthy_previous,tripped;
 unsigned fail_count,unconfirmed,attempts;
};
const char*const state_keys[]={"enabled_boot","probation","fail_count","unconfirmed_count","recent","attempts_since_healthy",
 "attempt_boot","attempt_reports","attempt_trial","confirmed_boot","previous","healthy_previous","tripped"};
const size_t STATE_KEYS=sizeof(state_keys)/sizeof(state_keys[0]);
bool number(const std::string&s,unsigned&n){if(s.empty()||s.size()>6||(s.size()>1&&s[0]=='0'))return false;n=0;for(size_t i=0;i<s.size();i++){if(s[i]<'0'||s[i]>'9')return false;n=n*10+unsigned(s[i]-'0');}return true;}
bool hex64(const std::string&s){if(s.size()!=64)return false;for(size_t i=0;i<64;i++)if(!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f')))return false;return true;}
bool one_of(const std::string&s,const char*const*set){for(;*set;set++)if(s==*set)return true;return false;}
const char*const previous_values[]={"none","confirmed","healthy","unconfirmed","failed_reset","failed_bootloop",0};
const char*const healthy_values[]={"none","yes","no",0};
const char*const tripped_values[]={"no","probation","reset_reports","reset_reports_repeated","unconfirmed","boot_loop","runtime_disabled",0};
// The exact path shape the autostart block accepts, under the fixture prefix.
bool trial_path(const std::string&s){
 const std::string head=prefix+"/tmp/mx5dr-trial-";
 if(s.size()!=head.size()+6+8||s.compare(0,head.size(),head)||s.compare(head.size()+6,8,"/sm.conf"))return false;
 for(size_t i=head.size();i<head.size()+6;i++){char c=s[i];if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')))return false;}
 return true;
}
unsigned recent_resets(const std::string&r){unsigned n=0;for(size_t i=0;i<r.size();i++)if(r[i]=='R')n++;return n;}
std::string state_text(const State&s){
 char counts[96];snprintf(counts,sizeof counts,"fail_count=%u\nunconfirmed_count=%u\n",s.fail_count,s.unconfirmed);
 char attempts[48];snprintf(attempts,sizeof attempts,"attempts_since_healthy=%u\n",s.attempts);
 return "mx5dr-persist-state-v2\nenabled_boot="+s.enabled_boot+"\nprobation="+s.probation+"\n"+counts+"recent="+s.recent+"\n"+attempts+
  "attempt_boot="+s.attempt_boot+"\nattempt_reports="+s.attempt_reports+"\nattempt_trial="+s.attempt_trial+"\nconfirmed_boot="+s.confirmed_boot+
  "\nprevious="+s.previous+"\nhealthy_previous="+s.healthy_previous+"\ntripped="+s.tripped+"\n";
}
// The only parser of persist-state; `status` prints what it accepts.
bool parse_state(const std::string&raw,State&s){
 if(raw.empty()||raw[raw.size()-1]!='\n')return false;
 std::vector<std::string> lines;size_t at=0;
 while(at<raw.size()){size_t end=raw.find('\n',at);lines.push_back(raw.substr(at,end-at));at=end+1;}
 if(lines.size()!=STATE_KEYS+1||lines[0]!="mx5dr-persist-state-v2")return false;
 std::string v[STATE_KEYS];
 for(size_t i=0;i<STATE_KEYS;i++){std::string k=std::string(state_keys[i])+"=";if(lines[i+1].compare(0,k.size(),k))return false;v[i]=lines[i+1].substr(k.size());}
 s.enabled_boot=v[0];s.probation=v[1];s.recent=v[4];s.attempt_boot=v[6];s.attempt_reports=v[7];s.attempt_trial=v[8];s.confirmed_boot=v[9];
 s.previous=v[10];s.healthy_previous=v[11];s.tripped=v[12];
 if(!uuid36(s.enabled_boot)||(s.probation!="yes"&&s.probation!="no"))return false;
 if(!number(v[2],s.fail_count)||!number(v[3],s.unconfirmed)||!number(v[5],s.attempts))return false;
 if(s.fail_count>TRIP_FAILURES||s.unconfirmed>UNCONFIRMED_TRIP)return false;
 if(s.recent!="-"&&(s.recent.empty()||s.recent.size()>RECENT_WINDOW||s.recent.find_first_not_of("RUCB")!=std::string::npos))return false;
 bool none=s.attempt_boot=="none";
 if(none!=(s.attempt_reports=="none")||none!=(s.attempt_trial=="none"))return false;
 if(!none&&(!uuid36(s.attempt_boot)||!hex64(s.attempt_reports)||!trial_path(s.attempt_trial)))return false;
 if(s.confirmed_boot!="none"&&s.confirmed_boot!=s.attempt_boot)return false;
 if(!one_of(s.previous,previous_values)||!one_of(s.healthy_previous,healthy_values)||!one_of(s.tripped,tripped_values))return false;
 // A tripped state never carries an open attempt; an active one is below every cap.
 if(s.tripped!="no"?!none:(s.fail_count>=TRIP_FAILURES||s.unconfirmed>=UNCONFIRMED_TRIP||
    recent_resets(s.recent)>=RECENT_RESET_TRIP))return false;
 return state_text(s)==raw;
}
// Mode is bound by the manifest hash too; persistent product mode is BETA only.
bool beta_config(){std::string s;return read_file(prefix+"/data_persist/mx5-aa-dr/mx5dr.conf",s,1024)&&s.compare(0,10,"mode=BETA\n")==0;}
bool absent(const char*name){struct stat st;return fstatat(gd,name,&st,AT_SYMLINK_NOFOLLOW)!=0&&errno==ENOENT;}
// The logs directory belongs to the collector account: untrusted, never followed.
int open_logs(){int bd=open_base();if(bd<0)return -1;int ld=openat(bd,"logs",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);close(bd);return ld;}
// `healthy` = "boot_id=<uuid>\nuptime_s=<n>\n", written by the runtime after
// stable runtime. Untrusted data: regular, small, exact format, same boot.
bool healthy_marker(const std::string&boot){
 int ld=open_logs();if(ld<0)return false;
 int fd=openat(ld,"healthy",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_NOCTTY|O_CLOEXEC);close(ld);if(fd<0)return false;
 struct stat st;std::string s;
 bool ok=fstat(fd,&st)==0&&S_ISREG(st.st_mode)&&st.st_size<=128&&read_fd(fd,s,128);close(fd);
 if(!ok||s.size()<56||s.compare(0,8,"boot_id=")||s.substr(8,36)!=boot||s.compare(44,10,"\nuptime_s=")||s[s.size()-1]!='\n')return false;
 std::string up=s.substr(54,s.size()-55);
 if(up.empty()||up.size()>10||(up.size()>1&&up[0]=='0'))return false;
 unsigned long long n=0;for(size_t i=0;i<up.size();i++){if(up[i]<'0'||up[i]>'9')return false;n=n*10+unsigned(up[i]-'0');}
 return n>=120;
}
// Fail closed: anything but a definitely absent runtime stop marker is present.
bool runtime_disabled(){int ld=open_logs();if(ld<0)return false;struct stat st;bool present=fstatat(ld,"disable-next-start",&st,AT_SYMLINK_NOFOLLOW)==0||errno!=ENOENT;close(ld);return present;}
// A capture freeze (menu 3) belongs to its boot. Remove only an empty stop
// directory and a regular acknowledgement; never follow or recurse. Best effort.
void clear_stale_capture_stop(){
 int ld=open_logs();if(ld<0)return;struct stat st;
 if(fstatat(ld,"capture.done",&st,AT_SYMLINK_NOFOLLOW)==0&&S_ISREG(st.st_mode))unlinkat(ld,"capture.done",0);
 if(fstatat(ld,"capture.stop",&st,AT_SYMLINK_NOFOLLOW)==0&&S_ISDIR(st.st_mode))unlinkat(ld,"capture.stop",AT_REMOVEDIR);
 fsync(ld);close(ld);
}
// Fingerprint of the Service Manager's reset reports (/data/*.out, written just
// before it stops the watchdog). No clock ordering: names, types, sizes, inodes
// and content digests; mtimes take part only as opaque equality values.
bool report_fingerprint(std::string&out){
 bool link=false;std::string data=prefix+"/data";
 if(!stock_alias(data,"/mnt/data","mnt/data",link))return false;
 if(link){
  if(!stock_alias(prefix+"/mnt","/tmp/mnt","tmp/mnt",link))return false;
  data=prefix+(link?"/tmp/mnt/data":"/mnt/data");
 }
 int dfd=open(data.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(dfd<0)return false;
 int scan=dup(dfd);DIR*dir=scan<0?0:fdopendir(scan);
 if(!dir){if(scan>=0)close(scan);close(dfd);return false;}
 std::vector<std::string> found;bool ok=true;
 hang_point("reports");
 for(unsigned seen=0;;seen++){
  // /data holds a few dozen entries; an unbounded directory declines.
  if(seen>=4096){ok=false;break;}
  errno=0;struct dirent*e=readdir(dir);
  if(!e){if(errno)ok=false;break;}
  std::string n=e->d_name;
  if(n.size()<5||n.size()>64||n.compare(n.size()-4,4,".out"))continue;
  if(found.size()>=MAX_REPORTS){ok=false;break;}
  found.push_back(n);
 }
 closedir(dir);
 std::sort(found.begin(),found.end());
 std::string list="mx5dr-reports-v1\n";
 for(size_t i=0;ok&&i<found.size();i++){
  struct stat st;if(fstatat(dfd,found[i].c_str(),&st,AT_SYMLINK_NOFOLLOW)){ok=false;break;}
  char meta[160];snprintf(meta,sizeof meta,"\t%o %lld %llu %lld.%09ld ",unsigned(st.st_mode&S_IFMT),(long long)st.st_size,(unsigned long long)st.st_ino,(long long)st.st_mtim.tv_sec,long(st.st_mtim.tv_nsec));
  std::string body;
  if(S_ISREG(st.st_mode)){
   int fd=openat(dfd,found[i].c_str(),O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_NOCTTY|O_CLOEXEC);if(fd<0){ok=false;break;}
   char b[4096];
   while(body.size()<REPORT_READ_CAP){ssize_t n=read(fd,b,sizeof b);if(n<0){if(errno==EINTR)continue;ok=false;break;}if(!n)break;size_t room=REPORT_READ_CAP-body.size();body.append(b,size_t(n)<room?size_t(n):room);}
   close(fd);
  }
  char h[65];mx5_sha256_bytes(body.data(),body.size(),h);
  list+=found[i]+meta+h+"\n";
 }
 close(dfd);
 if(!ok)return false;
 char h[65];mx5_sha256_bytes(list.data(),list.size(),h);out=h;return true;
}
// Judge the previous product attempt. The result is committed only in the one
// atomic state write that also replaces the attempt, so nothing counts twice.
//   R failed_reset: the SM reset reports changed since that selection.
//   B failed_bootloop: healthy rule only (off in this build).
//   C confirmed (or healthy): `confirm` recorded that boot.
//   U unconfirmed: neither a new report nor a confirmation.
// Only a confirmed attempt resets the consecutive counters and ends probation.
void evaluate(State&s,const std::string&reports){
 if(s.attempt_boot=="none"){s.previous="none";s.healthy_previous="none";return;}
 bool reset=s.attempt_reports!=reports;
 bool confirmed=s.confirmed_boot==s.attempt_boot;
 bool healthy=healthy_marker(s.attempt_boot);
 s.healthy_previous=healthy?"yes":"no";
 if(healthy)s.attempts=0;
 char mark;
 if(reset){mark='R';s.previous="failed_reset";if(s.fail_count<TRIP_FAILURES)s.fail_count++;}
 else if(healthy_rule&&!healthy&&s.attempts>=HEALTHY_RULE_ATTEMPTS){mark='B';s.previous="failed_bootloop";if(s.fail_count<TRIP_FAILURES)s.fail_count++;}
 else if(confirmed){mark='C';s.previous=healthy?"healthy":"confirmed";if(healthy||!healthy_rule)s.fail_count=0;}
 else{mark='U';s.previous="unconfirmed";if(s.unconfirmed<UNCONFIRMED_TRIP)s.unconfirmed++;}
 if(confirmed)s.unconfirmed=0;
 s.recent=(s.recent=="-"?std::string():s.recent)+mark;
 if(s.recent.size()>RECENT_WINDOW)s.recent.erase(0,s.recent.size()-RECENT_WINDOW);
 bool probation_failed=s.probation=="yes"&&!confirmed;
 if(confirmed)s.probation="no";
 if(probation_failed)s.tripped="probation";
 else if(s.fail_count>=TRIP_FAILURES)s.tripped=mark=='B'?"boot_loop":"reset_reports";
 else if(recent_resets(s.recent)>=RECENT_RESET_TRIP)s.tripped="reset_reports_repeated";
 else if(s.unconfirmed>=UNCONFIRMED_TRIP)s.tripped="unconfirmed";
}
// The SM of this boot still runs with the published trial: argv is
// "/jci/sm/sm -f <trial> ..." (vehicle ps 2026-10-04 shows exactly this form).
bool sm_running(const std::string&trial){
 const std::string want=trial.substr(prefix.size());
 int pd=open((prefix+"/proc").c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(pd<0)return false;
 int scan=dup(pd);DIR*dir=scan<0?0:fdopendir(scan);
 if(!dir){if(scan>=0)close(scan);close(pd);return false;}
 bool found=false;
 for(unsigned seen=0;!found&&seen<8192;seen++){
  struct dirent*e=readdir(dir);if(!e)break;
  const char*n=e->d_name;if(!n[0]||strspn(n,"0123456789")!=strlen(n)||strlen(n)>10)continue;
  int fd=openat(pd,(std::string(n)+"/cmdline").c_str(),O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_NOCTTY|O_CLOEXEC);if(fd<0)continue;
  std::string c;bool ok=read_fd(fd,c,4096);close(fd);if(!ok)continue;
  std::vector<std::string> argv;size_t at=0;
  while(at<c.size()&&argv.size()<8){size_t end=c.find('\0',at);if(end==std::string::npos)end=c.size();argv.push_back(c.substr(at,end-at));at=end+1;}
  if(argv.size()>=3&&argv[0]=="/jci/sm/sm"&&argv[1]=="-f"&&argv[2]==want)found=true;
 }
 closedir(dir);close(pd);
 return found;
}
// Exclusive /tmp/mx5dr-trial-XXXXXX/sm.conf, file and directory fsynced.
int stage_trial(const std::string&content,std::vector<char>&d){
 std::string t=prefix+"/tmp/mx5dr-trial-XXXXXX";d.assign(t.begin(),t.end());d.push_back(0);
 struct stat tmpst;if(lstat((prefix+"/tmp").c_str(),&tmpst)||!S_ISDIR(tmpst.st_mode))return -1;
 if(!mkdtemp(&d[0]))return -1;
 int td=open(&d[0],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(td<0){rmdir(&d[0]);return -1;}
 int fd=openat(td,"sm.conf",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0644);
 bool ok=fd>=0&&write_all(fd,content)&&fsync(fd)==0;
 if(fd>=0&&close(fd))ok=false;
 if(ok)ok=fchmod(td,0755)==0&&sync_dir(td,"trial-dir");
 if(!ok){unlinkat(td,"sm.conf",0);close(td);rmdir(&d[0]);return -1;}
 return td;
}
void fresh_state(State&s,const std::string&boot){
 s.enabled_boot=boot;s.probation="yes";s.fail_count=0;s.unconfirmed=0;s.attempts=0;s.recent="-";
 s.attempt_boot="none";s.attempt_reports="none";s.attempt_trial="none";s.confirmed_boot="none";
 s.previous="none";s.healthy_previous="none";s.tripped="no";
}
int enable_persistent(const std::string&expected){
 // One policy at a time: a pending one-boot arm must be removed first.
 if(!absent("arm")||!beta_config())return 2;
 std::string last,id;if(!last_boot_record(last)||!boot_id(id))return 2;
 // Probation: the first product boot after enabling must be confirmed.
 State s;fresh_state(s,id.substr(0,36));
 // Fresh state first; without `persist` it authorizes nothing.
 if(!atomic_file("persist-state",state_text(s)))return 2;
 if(atomic_file("persist",persist_text(expected)))return 0;
 // rename may have succeeded before fsync failed. Revoke the published name.
 bool removed=unlinkat(gd,"persist",0)==0||errno==ENOENT;
 if(!removed||!sync_dir(gd,"persist-cancel"))return 3;
 return 2;
}
// Which part of the persistent manifest no longer matches today's inputs.
std::string persist_mismatch(const std::string&p,const std::string&expected){
 const std::string want=persist_text(expected);
 if(p==want)return "";
 size_t head=want.find("mx5dr-one-boot-v3\n");
 if(p.size()!=want.size()||p.compare(0,head,want,0,head))return "persist_invalid";
 for(unsigned i=0;i<8;i++){
  size_t at=head+18+i*65;
  if(p.compare(at,65,want,at,65))return std::string("binding_changed:")+short_names[i];
 }
 return "persist_invalid";
}
#define DECLINE(code) do{why=(code);return 2;}while(0)
int select_persistent(unsigned index,const std::string&expected){
 std::string p,id,last,raw,reports;State s;
 if(!absent("arm"))DECLINE("arm_present");
 if(!owned_read("persist",p))DECLINE("persist_unreadable");
 std::string mismatch=persist_mismatch(p,expected);if(!mismatch.empty())DECLINE(mismatch);
 if(!beta_config())DECLINE("config_not_beta");
 if(!boot_id(id))DECLINE("boot_id_invalid");
 if(!last_boot_record(last))DECLINE("last_boot_invalid");
 if(last==id)DECLINE("same_boot");
 if(!owned_read("persist-state",raw)||!parse_state(raw,s))DECLINE("state_invalid");
 const std::string boot=id.substr(0,36);
 // Never in the enabling Linux boot, never twice in one boot, never once tripped.
 if(s.enabled_boot==boot)DECLINE("enabled_this_boot");
 if(s.attempt_boot==boot)DECLINE("same_boot");
 if(s.tripped!="no")DECLINE("tripped:"+s.tripped);
 if(!report_fingerprint(reports))DECLINE("reports_unreadable");
 evaluate(s,reports);
 if(s.tripped=="no"&&runtime_disabled())s.tripped="runtime_disabled";
 if(s.tripped!="no"){
  // Stock baseline from now on; the evidence stays. Re-enabling is menu 1.
  // An undurable trip record is re-derived from the same evidence next boot.
  s.attempt_boot="none";s.attempt_reports="none";s.attempt_trial="none";s.confirmed_boot="none";
  atomic_file("persist-state",state_text(s));
  DECLINE("tripped:"+s.tripped);
 }
 std::string content;if(!read_file(prefix+names[index],content,1024*1024))DECLINE("template_unreadable");
 std::vector<char> d;int td=stage_trial(content,d);if(td<0)DECLINE("trial_write_failed");
 std::string result=std::string(&d[0])+"/sm.conf";
 s.attempt_boot=boot;s.attempt_reports=reports;s.attempt_trial=result;s.confirmed_boot="none";
 if(s.attempts<MAX_COUNT)s.attempts++;
 // No path is published until the attempt record and boot marker are durable.
 bool ok=atomic_file("persist-state",state_text(s));
 bool last_published=false;
 if(ok)ok=atomic_file("last-boot",id,&last_published);
 if(!ok){
  // A durable attempt record without a published path only makes the next
  // evaluation stricter (it can never be confirmed). The visible success
  // marker is revoked.
  bool revoked=true;
  if(last_published){
   revoked=unlinkat(gd,"last-boot",0)==0||errno==ENOENT;
   if(revoked)revoked=sync_dir(gd,"last-boot-cancel");
  }
  unlinkat(td,"sm.conf",0);close(td);rmdir(&d[0]);
  why="state_write_failed";
  return revoked?2:3;
 }
 close(td);
 clear_stale_capture_stop();
 if(printf("%s\n",result.c_str())<0||fflush(stdout))return 2;
 return 0;
}
// Best effort, root-owned: why this boot runs stock (menu 2 shows it).
void record_decision(const std::string&code){
 std::string id;
 std::string boot=boot_id(id)?id.substr(0,36):"none";
 std::string clean=code.empty()?"unknown":code;
 for(size_t i=0;i<clean.size();i++){char c=clean[i];if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c==':'||c=='.'||c=='-'))clean[i]='_';}
 atomic_file("last-decision","mx5dr-last-decision-v1\nboot_id="+boot+"\nreason="+clean.substr(0,64)+"\n");
}
// What a selection in a NEW boot would decline for now (boot fences aside).
std::string verify_reason(bool inputs,const std::string&expected){
 if(!inputs)return why.empty()?"inputs_invalid":why;
 if(!absent("arm"))return "arm_present";
 std::string p,raw;State s;
 if(!owned_read("persist",p))return "persist_unreadable";
 std::string mismatch=persist_mismatch(p,expected);if(!mismatch.empty())return mismatch;
 if(!beta_config())return "config_not_beta";
 if(!owned_read("persist-state",raw)||!parse_state(raw,s))return "state_invalid";
 if(s.tripped!="no")return "tripped:"+s.tripped;
 std::string reports;if(!report_fingerprint(reports))return "reports_unreadable";
 if(runtime_disabled())return "runtime_disabled_pending";
 return "ok";
}
std::string reason_text(const std::string&code){
 size_t colon=code.find(':');std::string kind=code.substr(0,colon),what=colon==std::string::npos?"":code.substr(colon+1);
 if(kind=="ok")return "ok";
 if(kind=="baseline_edited")return "bindings changed: "+what+" edited by another tool: run menu 1";
 if(kind=="binding_changed")return "bindings changed: "+what+" differs from install: run menu 1";
 if(kind=="input_unreadable")return "file missing or unsafe: "+what+": run menu 1";
 if(kind=="baseline_has_token")return "stock "+what+" carries our preload: run menu 4, then 1";
 if(kind=="tripped")return "tripped ("+what+"): run menu 3, then menu 1";
 if(kind=="runtime_disabled_pending")return "runtime asked to stop: next boot trips: run menu 3";
 if(kind=="arm_present")return "a one-boot trial is armed: run menu 1";
 if(kind=="config_not_beta")return "config is not BETA: run menu 1";
 return code+": run menu 3 and report";
}
// Run by the owned autostart block about 90 s after a committed selection.
// Records this boot only if it is the open attempt, the selection was
// committed (last-boot) and the SM still runs with that trial. Idempotent.
int confirm_persistent(){
 std::string p,raw,id,last;State s;
 if(!owned_read("persist",p)||!owned_read("persist-state",raw)||!parse_state(raw,s))return 2;
 if(!boot_id(id)||!last_boot_record(last)||last!=id)return 2;
 const std::string boot=id.substr(0,36);
 if(s.tripped!="no"||s.attempt_boot!=boot)return 2;
 if(s.confirmed_boot==boot)return 0;
 if(!sm_running(s.attempt_trial))return 2;
 s.confirmed_boot=boot;
 return atomic_file("persist-state",state_text(s))?0:2;
}
// Hard bound for every invocation: the Service Manager waits for `select`.
// SIGALRM's default action ends the process without cleanup; every path
// before the printed trial path is fail-closed (no path => stock baseline).
const unsigned GUARD_DEADLINE_S=10;
int run(int argc,char**argv){
 unsigned deadline=GUARD_DEADLINE_S;
#ifdef MX5DR_GUARD_TESTING
 const char*a=getenv("MX5DR_GUARD_ALARM");if(a&&atoi(a)>0&&atoi(a)<=60)deadline=unsigned(atoi(a));
#endif
 signal(SIGALRM,SIG_DFL);alarm(deadline);
 if(geteuid()!=expected_owner()||argc<2)return 2;
#ifdef MX5DR_GUARD_TESTING
 const char*p=getenv("MX5DR_GUARD_ROOT");if(!p||p[0]!='/'||!p[1])return 2;prefix=p;struct stat marker;if(lstat((prefix+"/.mx5dr-fixture").c_str(),&marker)||!S_ISREG(marker.st_mode))return 2;
#endif
 hang_point("start");
 gd=trusted(prefix+"/data_persist/mx5-aa-dr/guard",true);if(gd<0)return 2;
 // Read-only: never takes the lock, never writes.
 if(!strcmp(argv[1],"verify")){
  if(argc!=2)return 2;
  std::string expected;bool inputs=baseline_clean()&&manifest(expected);
  std::string code=verify_reason(inputs,expected);
  if(printf("verify=%s\nmessage=%s\n",code.c_str(),reason_text(code).c_str())<0||fflush(stdout))return 2;
  return code=="ok"?0:1;
 }
 int lock=openat(gd,"lock",O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);if(lock<0||!safe_stat(lock,false))return 2;
 // `confirm` runs in the background and may meet a parked menu action: retry.
 bool confirm=!strcmp(argv[1],"confirm");
 for(unsigned tries=0;flock(lock,LOCK_EX|LOCK_NB);tries++){if(!confirm||tries>=5)return 2;sleep(1);}
 clean_stale_temps();
 if(confirm)return argc==2?confirm_persistent():2;
 std::string expected;bool inputs=baseline_clean()&&manifest(expected);
 bool persistent_select=!strcmp(argv[1],"select")&&!absent("persist");
 if(!inputs){if(persistent_select)record_decision(why);return 2;}
 if(!strcmp(argv[1],"check")){
  std::string id,last;return argc==2&&boot_id(id)&&last_boot_record(last)?0:2;
 }
 if(!strcmp(argv[1],"arm")){
  if(argc!=2)return 2;
  std::string last,id;if(!last_boot_record(last)||!boot_id(id))return 2;
  if(atomic_file("arm",expected))return 0;
  // rename may have succeeded before fsync failed. Revoke the published name.
  bool removed=unlinkat(gd,"arm",0)==0||errno==ENOENT;
  if(!removed||!sync_dir(gd,"arm-cancel"))return 3;
  return 2;
 }
 if(!strcmp(argv[1],"enable"))return argc==2?enable_persistent(expected):2;
 if(strcmp(argv[1],"select")||argc!=3)return 2;
 unsigned index;if(!strcmp(argv[2],"/jci/sm/sm.conf"))index=3;else if(!strcmp(argv[2],"/jci/sm/sm_WCP.conf"))index=5;else return 2;
 // An installed persistent manifest selects the product policy; otherwise one-boot.
 if(persistent_select){
  int r=select_persistent(index,expected);
  if(r)record_decision(why);
  return r;
 }
 std::string arm,id,last,armed;
 if(!owned_read("arm",arm)||arm!=expected||!boot_id(id)||!last_boot_record(last)||
    !owned_read("armed-boot",armed)||!valid_boot_record(armed))return 2;
 // The installer publishes this marker after arming. A Service Manager restart
 // in that same Linux boot must not consume the future-boot trial.
 if(last==id||armed==id)return 2;
 std::string content;if(!read_file(prefix+names[index],content,1024*1024))return 2;
 // Temporary parent is a real directory. mkdtemp gives an unguessable exclusive child.
 std::string t=prefix+"/tmp/mx5dr-trial-XXXXXX";std::vector<char> d(t.begin(),t.end());d.push_back(0);
 struct stat tmpst;if(lstat((prefix+"/tmp").c_str(),&tmpst)||!S_ISDIR(tmpst.st_mode))return 2;
 if(!mkdtemp(&d[0]))return 2;
 int td=open(&d[0],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(td<0)return 2;
 int fd=openat(td,"sm.conf",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0644);
 bool ok=fd>=0&&write_all(fd,content)&&fsync(fd)==0;
 if(fd>=0&&close(fd))ok=false;
 if(ok)ok=fchmod(td,0755)==0&&sync_dir(td,"trial-dir");
 // No path is published until arm consumption and boot marker are durable.
 if(ok)ok=renameat(gd,"arm",gd,"consumed")==0&&sync_dir(gd,"consume-dir");
 bool last_published=false;
 if(ok)ok=atomic_file("last-boot",id,&last_published);
 if(!ok){
  // rename can succeed before the directory fsync fails. The SM receives no
  // trial path, so remove the visible success marker as well.
  bool revoked=true;
  if(last_published){
   revoked=unlinkat(gd,"last-boot",0)==0||errno==ENOENT;
   if(revoked)revoked=sync_dir(gd,"last-boot-cancel");
  }
  unlinkat(td,"sm.conf",0);close(td);rmdir(&d[0]);
  return revoked?2:3;
 }
 close(td);std::string result=std::string(&d[0])+"/sm.conf";
 if(printf("%s\n",result.c_str())<0||fflush(stdout))return 2;
 return 0;
}
}
int main(int argc,char**argv){int r=run(argc,argv);if(r==3)fprintf(stderr,"mx5dr guard: unable to confirm durable rollback; inspect storage before reboot\n");else if(r)fprintf(stderr,"mx5dr guard: no trial path published\n");return r;}
