#ifndef MX5DR_RUNTIME_STORAGE_H
#define MX5DR_RUNTIME_STORAGE_H
#include "boot_id.h"
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

namespace mx5 { namespace runtime {
// Local logging policy, not a measured OEM minimum. The extra 64 KiB covers
// these two writers' stdio buffers, next records and small stop diagnostics.
// Other processes can consume space concurrently; this is not a disk quota.
static const uint64_t STORAGE_RESERVE_BYTES=8ULL*1024*1024;
static const uint64_t STORAGE_MARGIN_BYTES=64ULL*1024;
struct StorageSpace {
    const char* reason;
    uint64_t available;
    int error;
    bool known;
};
inline StorageSpace storage_space(const char* root,size_t next_bytes) {
    StorageSpace s={0,0,0,false};
    char path[512];
    const int n=snprintf(path,sizeof path,"%s/logs",root);
    if(n<0 || size_t(n)>=sizeof path){s.reason="space_info_invalid";return s;}
    struct statvfs v;
    if(statvfs(path,&v)){s.reason="space_query_failed";s.error=errno;return s;}
    const uint64_t block=v.f_frsize?v.f_frsize:v.f_bsize;
    if(!block || uint64_t(v.f_bavail)>UINT64_MAX/block){s.reason="space_info_invalid";return s;}
    s.available=uint64_t(v.f_bavail)*block;s.known=true;
    // Both journals already reject records larger than the 8 MiB file cap.
    // Keep arithmetic bounded on both the host and the 32-bit target.
    if(next_bytes>8388608U){s.reason="space_info_invalid";return s;}
    if(s.available<STORAGE_RESERVE_BYTES+STORAGE_MARGIN_BYTES+next_bytes)
        s.reason="low_space";
    return s;
}
inline void record_storage_stop(const char* root,const char* stream,const StorageSpace& s) {
    // One fixed, sub-1-KiB record per stream. Best effort even when free-space
    // inspection itself failed; never rotate/delete the retained capture here.
    // A later healthy worker does not erase this failure. The parked check
    // distinguishes the boot/start boundary; an export preserves its history.
    char path[512],line[768],boot[37],available[32];
    const int p=snprintf(path,sizeof path,"%s/logs/%s.storage.json",root,stream);
    if(p<0 || size_t(p)>=sizeof path)return;
    read_boot_id(boot);
    struct timespec t;
    const uint64_t now=clock_gettime(CLOCK_MONOTONIC,&t)?0:
        uint64_t(t.tv_sec)*1000000000ULL+t.tv_nsec;
    if(s.known)snprintf(available,sizeof available,"%llu",(unsigned long long)s.available);
    else snprintf(available,sizeof available,"null");
    const int n=snprintf(line,sizeof line,
        "{\"kind\":\"storage_stop\",\"stream\":\"%s\",\"boot_id\":\"%s\",\"pid\":%ld,"
        "\"mono_ns\":%llu,\"reason\":\"%s\",\"available_bytes\":%s,"
        "\"reserve_bytes\":%llu,\"margin_bytes\":%llu,\"syscall_errno\":%d}\n",
        stream,boot,(long)getpid(),(unsigned long long)now,s.reason,available,
        (unsigned long long)STORAGE_RESERVE_BYTES,(unsigned long long)STORAGE_MARGIN_BYTES,s.error);
    if(n<=0 || size_t(n)>=sizeof line)return;
    const int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC,0600);
    if(fd<0)return;
    struct stat st;
    if(!fstat(fd,&st) && S_ISREG(st.st_mode)) {
        size_t done=0;
        while(done<size_t(n)) {
            const ssize_t w=write(fd,line+done,size_t(n)-done);
            if(w<0 && errno==EINTR)continue;
            if(w<=0)break;
            done+=size_t(w);
        }
        if(done==size_t(n))fsync(fd);
    }
    close(fd);
}
} }
#endif
