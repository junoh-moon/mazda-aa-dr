#ifndef MX5DR_TEST_STORAGE_FIXTURE_H
#define MX5DR_TEST_STORAGE_FIXTURE_H
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <errno.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fstream>
#include <iterator>
#include <string>

// Replace only the OS free-space query in the included production logger.
// Files, writes, rotation and failure transitions still execute normally.
static unsigned storage_query_mode;
static uint64_t storage_available;
static int fixture_statvfs(const char* path,struct statvfs* out) {
    if(!storage_query_mode)return statvfs(path,out);
    if(storage_query_mode==2){errno=EIO;return -1;}
    memset(out,0,sizeof *out);
    out->f_bsize=out->f_frsize=storage_query_mode==3?0:4096;
    out->f_bavail=storage_available/4096;
    return 0;
}
static std::string storage_read(const std::string& path) {
    std::ifstream f(path.c_str());
    return std::string(std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>());
}
template<class Journal> static void check_storage(const char* stream,const char* scenario) {
    (void)fixture_statvfs;
    char root[]="/tmp/mx5dr-storage-XXXXXX";assert(mkdtemp(root));
    const std::string logs=std::string(root)+"/logs";
    assert(!mkdir(logs.c_str(),0700));
    const std::string current=logs+"/"+stream+".0.jsonl";
    const std::string marker=logs+"/"+stream+".storage.json";
    {std::ofstream f(current.c_str());f<<"previous capture\n";}
    const bool during=!strcmp(scenario,"during");
    const bool healthy=!strcmp(scenario,"healthy");
    storage_query_mode=1;storage_available=64ULL*1024*1024;
    {
        Journal j(root);
        if(during){j.line("{\"kind\":\"before\"}");j.flush();assert(!j.failed);}
        const std::string before=storage_read(current);
        storage_available=healthy?9ULL*1024*1024:7ULL*1024*1024;
        if(!strcmp(scenario,"query"))storage_query_mode=2;
        if(!strcmp(scenario,"invalid"))storage_query_mode=3;
        j.line("{\"kind\":\"after\"}");j.flush();
        if(healthy) {
            assert(!j.failed);
            assert(storage_read(current).find("after")!=std::string::npos);
        } else {
            assert(j.failed); // The old writer grows/rotates the log here.
            assert(storage_read(current)==before);
            const std::string stopped=storage_read(marker);
            assert(stopped.size()>0 && stopped.size()<1024);
            assert(stopped.find("\"kind\":\"storage_stop\"")!=std::string::npos);
            assert(stopped.find("\"reserve_bytes\":8388608")!=std::string::npos);
            const char* reason=storage_query_mode==2?"space_query_failed":
                storage_query_mode==3?"space_info_invalid":"low_space";
            assert(stopped.find(reason)!=std::string::npos);
            storage_query_mode=1;storage_available=64ULL*1024*1024;
            j.line("{\"kind\":\"must_not_restart\"}");j.flush();
            assert(storage_read(current)==before && storage_read(marker)==stopped);
        }
    }
    if(!healthy) {
        const std::string stopped=storage_read(marker);
        Journal restarted(root);
        restarted.line("{\"kind\":\"restarted\"}");restarted.flush();
        assert(!restarted.failed);
        // A later worker may collect again, but must not erase the previous
        // storage failure from the trial's retained evidence.
        assert(storage_read(marker)==stopped);
    }
    storage_query_mode=0;
    for(unsigned i=0;i<3;++i)unlink((logs+"/"+stream+"."+char('0'+i)+".jsonl").c_str());
    unlink(marker.c_str());
    assert(!rmdir(logs.c_str())&&!rmdir(root));
    printf("Storage %s %s: reserve, preserved capture and bounded stop evidence passed\n",stream,scenario);
}
template<class Journal> static void check_real_storage(const char* directory,const char* stream) {
    // Integration runner supplies a dedicated 16 MiB tmpfs. No query override
    // or ENOSPC stand-in: exercise the kernel's actual remaining-space report.
    storage_query_mode=0;
    const std::string root=std::string(directory)+"/capture",logs=root+"/logs";
    assert(!mkdir(root.c_str(),0700)&&!mkdir(logs.c_str(),0700));
    struct statvfs v;assert(!statvfs(logs.c_str(),&v));
    const uint64_t before=uint64_t(v.f_bavail)*v.f_frsize;
    assert(before>9ULL*1024*1024 && before<=16ULL*1024*1024);
    const std::string row="{\"kind\":\"fixture\",\"data\":\""+std::string(1024,'a')+"\"}";
    unsigned count=0;
    {
        Journal j(root.c_str());
        while(!j.failed && count<32768){j.line(row.c_str());++count;}
        j.flush();assert(j.failed && count>1 && count<32768);
    }
    assert(!statvfs(logs.c_str(),&v));
    const uint64_t after=uint64_t(v.f_bavail)*v.f_frsize;
    assert(after>=8ULL*1024*1024 && after<before);
    const std::string marker=logs+"/"+stream+".storage.json";
    assert(storage_read(marker).find("low_space")!=std::string::npos);
    printf("Real tmpfs %s: rows_attempted=%u free_before=%llu free_after=%llu reserve=8388608\n",
           stream,count,(unsigned long long)before,(unsigned long long)after);
    for(unsigned i=0;i<3;++i)unlink((logs+"/"+stream+"."+char('0'+i)+".jsonl").c_str());
    unlink(marker.c_str());assert(!rmdir(logs.c_str())&&!rmdir(root.c_str()));
}
#endif
