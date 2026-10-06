#ifndef MX5_RUNTIME_JOURNAL_RING_H
#define MX5_RUNTIME_JOURNAL_RING_H
// Journal rows in flight from the worker to the journal writer thread
// (2026-10-06, validation/WORKER_STALL_STALE_2026-10-06.md: a write backlog
// blocked the receiving worker in fwrite/fflush/statvfs for 70-770 ms).
//
// One producer (the journal worker) and one consumer (the writer thread).
// OEM threads never touch this ring: they only push fixed Observation values
// into the adapter's lock-free JournalQueue, which the worker formats.
//
// Two preallocated byte rings share one global row sequence:
//  * EVIDENCE rows (BETA evidence: every beta_* row, every SEND row with
//    choice != ORIGINAL, POSITION rows and the boot/lifecycle rows) are never
//    dropped. If one does not fit, push() reports FULL and the caller must
//    treat it as a journal failure (disable mutation, OBSERVE).
//  * DIAGNOSTIC rows (motion batches, ORIGINAL sends, health, MODEL
//    diagnostics) make room by dropping the OLDEST diagnostic rows. The
//    writer sees the resulting sequence gap and journals one counter row at
//    the exact place of the loss.
// The writer pops in global sequence order, so the order of rows in the file
// equals the order of push() calls.
//
// Short lock: one mutex guards both rings; it is held only for index updates
// and one memcpy of at most MAX_ROW bytes. No allocation, I/O or sleep while
// holding it; the backing storage is supplied once by the owner.
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace mx5 { namespace runtime {

class JournalRing {
public:
    // Largest accepted row (request_log.h OBSERVATION_JSON_CAPACITY).
    static const size_t MAX_ROW=8192;
    static const size_t HEADER=12;               // u32 length + u64 sequence
    enum Result { PUSHED=0, PUSHED_AFTER_DROP, FULL, TOO_LARGE };
    JournalRing(unsigned char* diagnostic,size_t diagnostic_bytes,
                unsigned char* evidence,size_t evidence_bytes)
        : next_seq_(0),dropped_rows_(0),dropped_bytes_(0),high_water_(0) {
        pthread_mutex_init(&mutex_,0);
        ring_[0].init(diagnostic,diagnostic_bytes);ring_[1].init(evidence,evidence_bytes);
    }
    ~JournalRing() { pthread_mutex_destroy(&mutex_); }
    // Producer. n excludes any terminator. FULL only for an evidence row.
    Result push(const char* row,size_t n,bool evidence) {
        Ring& r=ring_[evidence?1:0];
        const size_t total=HEADER+n;
        if(n>MAX_ROW || total>r.cap)return TOO_LARGE;
        pthread_mutex_lock(&mutex_);
        Result result=PUSHED;
        if(r.cap-r.used<total) {
            if(evidence) { pthread_mutex_unlock(&mutex_);return FULL; }
            while(r.cap-r.used<total) {           // drop oldest diagnostic rows
                uint32_t len;uint64_t seq;r.header(&len,&seq);
                r.head=(r.head+HEADER+len)%r.cap;r.used-=HEADER+len;
                ++dropped_rows_;dropped_bytes_+=len;
            }
            result=PUSHED_AFTER_DROP;
        }
        unsigned char header[HEADER];
        const uint32_t len=uint32_t(n);const uint64_t seq=next_seq_++;
        memcpy(header,&len,4);memcpy(header+4,&seq,8);
        r.put(header,HEADER);r.put(row,n);
        const size_t used=ring_[0].used+ring_[1].used;
        if(used>high_water_)high_water_=used;
        pthread_mutex_unlock(&mutex_);
        return result;
    }
    // Consumer: the row with the lowest sequence, copied to out (capacity
    // >= MAX_ROW+1, NUL-terminated). false when both rings are empty.
    bool pop(char* out,size_t capacity,size_t* n,uint64_t* seq,bool* evidence) {
        pthread_mutex_lock(&mutex_);
        int pick=-1;uint64_t best=0;uint32_t best_len=0;
        for(int i=0;i<2;++i) {
            if(!ring_[i].used)continue;
            uint32_t len;uint64_t s;ring_[i].header(&len,&s);
            if(pick<0 || s<best) { pick=i;best=s;best_len=len; }
        }
        if(pick<0 || capacity<=best_len) { pthread_mutex_unlock(&mutex_);return false; }
        Ring& r=ring_[pick];
        r.get((r.head+HEADER)%r.cap,out,best_len);out[best_len]=0;
        r.head=(r.head+HEADER+best_len)%r.cap;r.used-=HEADER+best_len;
        pthread_mutex_unlock(&mutex_);
        *n=best_len;*seq=best;*evidence=pick==1;
        return true;
    }
    struct Stats { uint64_t next_seq,dropped_rows,dropped_bytes; size_t used,high_water; };
    Stats stats() {
        pthread_mutex_lock(&mutex_);
        const Stats s={next_seq_,dropped_rows_,dropped_bytes_,ring_[0].used+ring_[1].used,high_water_};
        pthread_mutex_unlock(&mutex_);
        return s;
    }
private:
    struct Ring {
        unsigned char* buf;size_t cap,head,tail,used;
        void init(unsigned char* b,size_t c) { buf=b;cap=b?c:0;head=tail=used=0; }
        void put(const void* p,size_t n) {
            const unsigned char* s=static_cast<const unsigned char*>(p);
            const size_t first=n<cap-tail?n:cap-tail;
            memcpy(buf+tail,s,first);memcpy(buf,s+first,n-first);
            tail=(tail+n)%cap;used+=n;
        }
        void get(size_t at,void* p,size_t n) const {
            unsigned char* d=static_cast<unsigned char*>(p);
            const size_t first=n<cap-at?n:cap-at;
            memcpy(d,buf+at,first);memcpy(d+first,buf,n-first);
        }
        void header(uint32_t* len,uint64_t* seq) const {
            unsigned char h[HEADER];get(head,h,HEADER);memcpy(len,h,4);memcpy(seq,h+4,8);
        }
    };
    pthread_mutex_t mutex_;
    Ring ring_[2];
    uint64_t next_seq_,dropped_rows_,dropped_bytes_;
    size_t high_water_;
    JournalRing(const JournalRing&);
    JournalRing& operator=(const JournalRing&);
};

// Row class by its leading kind (every journal row starts with {"kind":"..."}).
// Evidence: BETA rows, POSITION rows, SEND rows with choice != ORIGINAL (0)
// and the boot/lifecycle/stop rows. Everything else is diagnostic.
inline bool journal_evidence_row(const char* s,size_t n) {
    static const char prefix[]="{\"kind\":\"";
    const size_t p=sizeof prefix-1;
    if(n<=p || memcmp(s,prefix,p))return false;
    const char* k=s+p;const size_t rest=n-p;
    struct Kind { const char* name;size_t len; };
    static const Kind always[]={
        {"beta_",5},{"position\"",9},{"boot\"",5},{"shadow_boot\"",12},{"capture_end\"",12},
        {"capture_incomplete\"",19},{"shadow_disabled\"",16},{"storage_stop\"",13}};
    for(size_t i=0;i<sizeof always/sizeof always[0];++i)
        if(rest>=always[i].len && !memcmp(k,always[i].name,always[i].len))return true;
    if(rest>=5 && !memcmp(k,"send\"",5)) {
        static const char choice[]=",\"choice\":";
        for(size_t i=p;i+sizeof choice-1<n;++i)
            if(!memcmp(s+i,choice,sizeof choice-1)) {
                const char c=s[i+sizeof choice-1];
                return c!='0' || (i+sizeof choice<n && s[i+sizeof choice]>='0' && s[i+sizeof choice]<='9');
            }
        return true;   // unparseable SEND: keep it
    }
    return false;
}

} }
#endif
