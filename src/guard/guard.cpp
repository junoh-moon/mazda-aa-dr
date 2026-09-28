// Pre-Service-Manager one-boot gate. No experimental DSO is loaded here.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../runtime/sha256.h"
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

namespace {
std::string prefix;
const char *const names[]={"/data_persist/mx5-aa-dr/libmx5dr.so", "/data_persist/mx5-aa-dr/mx5dr.conf", "/jci/sm/sm.conf", "/data_persist/mx5-aa-dr/guard/normal.trial", "/jci/sm/sm_WCP.conf", "/data_persist/mx5-aa-dr/guard/wcp.trial"};
const char *const token="/data_persist/mx5-aa-dr/libmx5dr.so";
int gd=-1;
uid_t expected_owner(){
#ifdef MX5DR_GUARD_TESTING
 return geteuid(); // Fixture files belong to the host test user; production stays root-only.
#else
 return 0;
#endif
}
bool safe_stat(int fd,bool directory){struct stat s;return fstat(fd,&s)==0 && s.st_uid==expected_owner() && !(s.st_mode&022) && (directory?S_ISDIR(s.st_mode):S_ISREG(s.st_mode));}
// Walk every component without following symlinks; no mutable helper is sourced.
int trusted(const std::string &path,bool directory=false){
 int fd=open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(fd<0)return -1;
 size_t i=1;
 while(i<path.size()){size_t end=path.find('/',i);bool last=end==std::string::npos;std::string c=path.substr(i,last?path.size()-i:end-i);
  if(c.empty()||c=="."||c==".."){close(fd);return -1;}
  int n=openat(fd,c.c_str(),O_RDONLY|O_NOFOLLOW|O_CLOEXEC|((!last||directory)?O_DIRECTORY:0));close(fd);fd=n;if(fd<0)return -1;
  // /tmp may be traversed only as an ancestor of the host-only fixture root.
#ifdef MX5DR_GUARD_TESTING
  bool fixture_ancestor=!prefix.empty() && path.substr(0,last?path.size():end).size()<prefix.size();
#else
  bool fixture_ancestor=false;
#endif
  if(!fixture_ancestor&&!safe_stat(fd,!last||directory)){close(fd);return -1;}
  if(last)break;
  i=end+1;
 }
 return fd;
}
bool read_fd(int fd,std::string &s,size_t cap){s.clear();char b[4096];for(;;){ssize_t n=read(fd,b,sizeof b);if(n<0){if(errno==EINTR)continue;return false;}if(!n)return true;if(s.size()+size_t(n)>cap)return false;s.append(b,size_t(n));}}
bool read_file(const std::string&p,std::string&s,size_t cap){int fd=trusted(p);if(fd<0)return false;bool ok=read_fd(fd,s,cap);close(fd);return ok;}
bool digest(const std::string&p,std::string&out){std::string s;if(!read_file(p,s,32*1024*1024))return false;char h[65];mx5_sha256_bytes(s.data(),s.size(),h);out=h;return true;}
bool write_all(int fd,const std::string&s){size_t n=0;while(n<s.size()){ssize_t w=write(fd,s.data()+n,s.size()-n);if(w<0&&errno==EINTR)continue;if(w<=0)return false;n+=size_t(w);}return true;}
bool sync_dir(int fd,const char*stage){
#ifdef MX5DR_GUARD_TESTING
 const char*fail=getenv("MX5DR_GUARD_FAIL_FSYNC");
 if(fail&&(std::string(",")+fail+",").find(std::string(",")+stage+",")!=std::string::npos){errno=EIO;return false;}
#else
 (void)stage;
#endif
 return fsync(fd)==0;
}
bool atomic_file(const char*name,const std::string&s){char temp[96];snprintf(temp,sizeof temp,".%s.%ld",name,(long)getpid());int fd=openat(gd,temp,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);if(fd<0)return false;bool ok=write_all(fd,s)&&fsync(fd)==0;int e=close(fd);ok=ok&&e==0;if(ok)ok=renameat(gd,temp,gd,name)==0&&sync_dir(gd,name);if(!ok)unlinkat(gd,temp,0);return ok;}
bool manifest(std::string&s){s="mx5dr-one-boot-v1\n";for(unsigned i=0;i<6;i++){std::string h;if(!digest(prefix+names[i],h))return false;s+=h+"\n";}return true;}
bool boot_id(std::string&s){std::string p=prefix+"/proc/sys/kernel/random/boot_id";int fd=open(p.c_str(),O_RDONLY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return false;bool ok=read_fd(fd,s,80);close(fd);if(!ok)return false;if(!s.empty()&&s[s.size()-1]=='\n')s.resize(s.size()-1);if(s.size()!=36)return false;for(size_t i=0;i<s.size();i++){if(i==8||i==13||i==18||i==23){if(s[i]!='-')return false;}else if(!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f')))return false;}s+='\n';return true;}
bool owned_read(const char*name,std::string&s){int fd=openat(gd,name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return false;bool ok=safe_stat(fd,false)&&read_fd(fd,s,1024);close(fd);return ok;}
bool baseline_clean(){for(unsigned i=2;i<=4;i+=2){std::string s;if(!read_file(prefix+names[i],s,1024*1024)||s.find(token)!=std::string::npos)return false;}return true;}
int run(int argc,char**argv){if(geteuid()!=expected_owner()||argc<2)return 2;
#ifdef MX5DR_GUARD_TESTING
 const char*p=getenv("MX5DR_GUARD_ROOT");if(!p||p[0]!='/'||!p[1])return 2;prefix=p;struct stat marker;if(lstat((prefix+"/.mx5dr-fixture").c_str(),&marker)||!S_ISREG(marker.st_mode))return 2;
#endif
 gd=trusted(prefix+"/data_persist/mx5-aa-dr/guard",true);if(gd<0)return 2;
 int lock=openat(gd,"lock",O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);if(lock<0||!safe_stat(lock,false)||flock(lock,LOCK_EX|LOCK_NB))return 2;
 std::string expected;if(!baseline_clean()||!manifest(expected))return 2;
 if(!strcmp(argv[1],"arm")){
  if(argc!=2)return 2;
  if(atomic_file("arm",expected))return 0;
  // rename may have succeeded before fsync failed. Revoke the published name.
  bool removed=unlinkat(gd,"arm",0)==0||errno==ENOENT;
  if(!removed||!sync_dir(gd,"arm-cancel"))return 3;
  return 2;
 }
 if(strcmp(argv[1],"select")||argc!=3)return 2;
 unsigned index;if(!strcmp(argv[2],"/jci/sm/sm.conf"))index=3;else if(!strcmp(argv[2],"/jci/sm/sm_WCP.conf"))index=5;else return 2;
 std::string arm,id,last;if(!owned_read("arm",arm)||arm!=expected||!boot_id(id))return 2;
 if(owned_read("last-boot",last)&&last==id)return 2;
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
 if(ok)ok=atomic_file("last-boot",id);
 if(!ok){unlinkat(td,"sm.conf",0);close(td);rmdir(&d[0]);return 2;}
 close(td);std::string result=std::string(&d[0])+"/sm.conf";
 if(printf("%s\n",result.c_str())<0||fflush(stdout))return 2;
 return 0;
}
}
int main(int argc,char**argv){int r=run(argc,argv);if(r==3)fprintf(stderr,"mx5dr guard: unable to confirm durable disarm; inspect storage before reboot\n");else if(r)fprintf(stderr,"mx5dr guard: no trial path published\n");return r;}
