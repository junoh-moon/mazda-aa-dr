#ifndef MX5_RUNTIME_ZLIB_DYN_H
#define MX5_RUNTIME_ZLIB_DYN_H
// Optional gzip through the CMU's own zlib (validation/LOG_COMPRESSION_2026-11-04.md).
// 74.00.324A ships /usr/lib/libz.so.1 (zlib 1.2.5); the vehicle diagnostics of
// 2026-11-04 show it already mapped by jciAAPA, jciVBS, jciLDS and
// aap_service. The toolchain has zlib.h but no libz, so nothing links against
// it: the collector process (and its --gzip/--gunzip helper mode, run by the
// export) loads it with dlopen. The AA runtime never includes this header.
// When the library or a symbol is missing, load() fails and callers keep the
// uncompressed path. Single gzip member per call; gunzip accepts concatenated
// members. Bounded stack buffers, no allocation by this code (zlib allocates
// its own state through its default allocator).
#include <dlfcn.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

namespace mx5 { namespace zlibdyn {

struct Api {
    void* handle;
    int (*deflate_init2)(z_streamp,int,int,int,int,int,const char*,int);
    int (*deflate_step)(z_streamp,int);
    int (*deflate_end)(z_streamp);
    int (*inflate_init2)(z_streamp,int,const char*,int);
    int (*inflate_step)(z_streamp,int);
    int (*inflate_end)(z_streamp);
    int (*inflate_reset)(z_streamp);
};

// dlsym returns void*; copy it into the function pointer without type punning.
template<class F> inline bool sym(void* h,const char* name,F& f) {
    void* p=dlsym(h,name);
    memcpy(&f,&p,sizeof p);
    return p!=0;
}

inline bool load(Api& a,const char* path="libz.so.1") {
    memset(&a,0,sizeof a);
    a.handle=dlopen(path,RTLD_NOW|RTLD_LOCAL);
    if(!a.handle)return false;
    bool ok=sym(a.handle,"deflateInit2_",a.deflate_init2)&sym(a.handle,"deflate",a.deflate_step)&
            sym(a.handle,"deflateEnd",a.deflate_end)&sym(a.handle,"inflateInit2_",a.inflate_init2)&
            sym(a.handle,"inflate",a.inflate_step)&sym(a.handle,"inflateEnd",a.inflate_end)&
            sym(a.handle,"inflateReset",a.inflate_reset);
    if(ok)return true;
    dlclose(a.handle);
    memset(&a,0,sizeof a);
    return false;
}

inline bool write_all(int fd,const unsigned char* p,size_t n) {
    while(n) {
        ssize_t w=write(fd,p,n);
        if(w<0&&errno==EINTR)continue;
        if(w<=0)return false;
        p+=w;n-=size_t(w);
    }
    return true;
}

// in -> out as one gzip member (level 6). True only when all input was read,
// the stream ended and every output byte was written.
inline bool gzip_fd(const Api& a,int in,int out,unsigned long long* in_bytes=0,unsigned long long* out_bytes=0) {
    if(!a.handle)return false;
    unsigned char ibuf[16384],obuf[16384];
    z_stream s;memset(&s,0,sizeof s);
    // windowBits 15+16: gzip wrapper. memLevel 8 is zlib's default (~256 KiB state).
    if(a.deflate_init2(&s,6,Z_DEFLATED,15+16,8,Z_DEFAULT_STRATEGY,ZLIB_VERSION,int(sizeof s))!=Z_OK)return false;
    unsigned long long total_in=0,total_out=0;bool ok=true,eof=false;
    while(ok) {
        if(!eof&&s.avail_in==0) {
            ssize_t r=read(in,ibuf,sizeof ibuf);
            if(r<0&&errno==EINTR)continue;
            if(r<0){ok=false;break;}
            if(r==0)eof=true;
            s.next_in=ibuf;s.avail_in=unsigned(r);total_in+=unsigned(r);
        }
        s.next_out=obuf;s.avail_out=sizeof obuf;
        int rc=a.deflate_step(&s,eof?Z_FINISH:Z_NO_FLUSH);
        if(rc==Z_STREAM_ERROR){ok=false;break;}
        size_t have=sizeof obuf-s.avail_out;
        if(have&&!write_all(out,obuf,have)){ok=false;break;}
        total_out+=have;
        if(rc==Z_STREAM_END)break;
    }
    a.deflate_end(&s);
    if(in_bytes)*in_bytes=total_in;
    if(out_bytes)*out_bytes=total_out;
    return ok;
}

// in -> out; accepts concatenated members. Fails on a truncated or corrupt
// stream (after writing what was decoded) and on trailing non-gzip data.
inline bool gunzip_fd(const Api& a,int in,int out) {
    if(!a.handle)return false;
    unsigned char ibuf[16384],obuf[16384];
    z_stream s;memset(&s,0,sizeof s);
    if(a.inflate_init2(&s,15+16,ZLIB_VERSION,int(sizeof s))!=Z_OK)return false;
    bool ok=true,eof=false,ended=false,any=false;
    while(ok) {
        if(s.avail_in==0&&!eof) {
            ssize_t r=read(in,ibuf,sizeof ibuf);
            if(r<0&&errno==EINTR)continue;
            if(r<0){ok=false;break;}
            if(r==0)eof=true;
            s.next_in=ibuf;s.avail_in=unsigned(r);
        }
        if(s.avail_in==0&&eof){ok=ended&&any;break;}
        if(ended) {
            // Another member follows.
            if(a.inflate_reset(&s)!=Z_OK){ok=false;break;}
            ended=false;
        }
        s.next_out=obuf;s.avail_out=sizeof obuf;
        int rc=a.inflate_step(&s,Z_NO_FLUSH);
        if(rc!=Z_OK&&rc!=Z_STREAM_END&&!(rc==Z_BUF_ERROR&&s.avail_in==0&&!eof)){ok=false;break;}
        size_t have=sizeof obuf-s.avail_out;
        if(have&&!write_all(out,obuf,have)){ok=false;break;}
        if(rc==Z_STREAM_END){ended=true;any=true;}
    }
    a.inflate_end(&s);
    return ok;
}

}}  // namespace mx5::zlibdyn

#endif
