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
//    choice != ORIGINAL, the boot/lifecycle rows and POSITION rows, except
//    that the persistent profile passes RAW-context POSITION rows as
//    diagnostic while BETA is not live, log_profile.h) are never dropped. If
//    one does not fit, push() reports FULL and the caller must treat it as a
//    journal failure (disable mutation, OBSERVE).
//  * DIAGNOSTIC rows (motion batches, ORIGINAL sends, health, MODEL
//    diagnostics) make room by dropping the OLDEST diagnostic rows. The
//    writer sees the resulting sequence gap and journals one counter row at
//    the exact place of the loss.
// The writer pops in global sequence order, so the order of rows in the file
// equals the order of push() calls.
//
// Short lock: one mutex guards both rings; it is held only for index updates
// and one memcpy of at most MAX_ROW bytes. No allocation, I/O or sleep while
// holding it; the backing storage is supplied once by the owner. A condition
// variable (CLOCK_MONOTONIC) wakes the writer on a push or notify(); the
// producer only signals it, it never waits.
//
// Each row carries its push time (the worker's clock), so the worker can
// measure how old the oldest unflushed row is (journal lag, runtime.cpp).
// The ring also keeps, under the same mutex, the push time of the row the
// consumer popped but has not written yet (set in the pop's critical section,
// so there is no instant in which a popped row is invisible to stats()) and
// the oldest push time of rows written to stdio since the consumer's last
// fflush (row_written()/flushed(), 2026-10-07 re-review).
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

namespace mx5 { namespace runtime {

class JournalRing {
public:
    // Largest accepted row (request_log.h OBSERVATION_JSON_CAPACITY).
    static const size_t MAX_ROW=8192;
    static const size_t HEADER=20;  // u32 length, u64 sequence, u64 push ns
    enum Result { PUSHED=0, PUSHED_AFTER_DROP, FULL, TOO_LARGE };
    JournalRing(unsigned char* diagnostic,size_t diagnostic_bytes,
                unsigned char* evidence,size_t evidence_bytes)
        : next_seq_(0),dropped_rows_(0),dropped_bytes_(0),held_push_ns_(0),unflushed_push_ns_(0),
          high_water_(0) {
        pthread_mutex_init(&mutex_,0);
        pthread_condattr_t attr;
        cond_ok_=!pthread_condattr_init(&attr);
        if(cond_ok_) {
            cond_ok_=!pthread_condattr_setclock(&attr,CLOCK_MONOTONIC) && !pthread_cond_init(&cond_,&attr);
            pthread_condattr_destroy(&attr);
        }
        ring_[0].init(diagnostic,diagnostic_bytes);ring_[1].init(evidence,evidence_bytes);
    }
    ~JournalRing() { if(cond_ok_)pthread_cond_destroy(&cond_);pthread_mutex_destroy(&mutex_); }
    // Producer. n excludes any terminator. FULL only for an evidence row.
    Result push(const char* row,size_t n,bool evidence,uint64_t now_ns=0) {
        Ring& r=ring_[evidence?1:0];
        const size_t total=HEADER+n;
        if(n>MAX_ROW || total>r.cap)return TOO_LARGE;
        pthread_mutex_lock(&mutex_);
        Result result=PUSHED;
        if(r.cap-r.used<total) {
            if(evidence) { pthread_mutex_unlock(&mutex_);return FULL; }
            while(r.cap-r.used<total) {           // drop oldest diagnostic rows
                Header h;r.header(&h);
                r.head=(r.head+HEADER+h.len)%r.cap;r.used-=HEADER+h.len;
                ++dropped_rows_;dropped_bytes_+=h.len;
            }
            result=PUSHED_AFTER_DROP;
        }
        unsigned char header[HEADER];
        const uint32_t len=uint32_t(n);const uint64_t seq=next_seq_++;
        memcpy(header,&len,4);memcpy(header+4,&seq,8);memcpy(header+12,&now_ns,8);
        r.put(header,HEADER);r.put(row,n);
        const size_t used=ring_[0].used+ring_[1].used;
        if(used>high_water_)high_water_=used;
        if(cond_ok_)pthread_cond_signal(&cond_);
        pthread_mutex_unlock(&mutex_);
        return result;
    }
    // Wake the consumer (flush/stop requests).
    void notify() {
        pthread_mutex_lock(&mutex_);
        if(cond_ok_)pthread_cond_signal(&cond_);
        pthread_mutex_unlock(&mutex_);
    }
    // Consumer: wait until a row is queued, notify() or timeout_ns elapses.
    // pending (optional) is evaluated under the ring mutex before sleeping:
    // a request whose state the notifier stored BEFORE calling notify() (a
    // flush target, a stop flag) is then never lost, even when it arrived
    // between the consumer's own check and this call (2026-10-07).
    typedef bool (*Pending)(void* context);
    void wait(uint64_t timeout_ns,Pending pending=0,void* context=0) {
        pthread_mutex_lock(&mutex_);
        if(!ring_[0].used && !ring_[1].used && !(pending && pending(context))) {
            if(cond_ok_) {
                struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
                const uint64_t ns=uint64_t(t.tv_nsec)+timeout_ns%1000000000ULL;
                t.tv_sec+=time_t(timeout_ns/1000000000ULL+ns/1000000000ULL);t.tv_nsec=long(ns%1000000000ULL);
                pthread_cond_timedwait(&cond_,&mutex_,&t);
            } else {
                pthread_mutex_unlock(&mutex_);
                const struct timespec pause={0,20000000};nanosleep(&pause,0);
                return;
            }
        }
        pthread_mutex_unlock(&mutex_);
    }
    // Consumer: the row with the lowest sequence, copied to out (capacity
    // >= MAX_ROW+1, NUL-terminated). false when both rings are empty.
    // push_ns: the row's push time (optional). The row stays counted as
    // held (stats().held_push_ns) until row_written(); a row still held from
    // an earlier pop is then counted as written but not flushed.
    bool pop(char* out,size_t capacity,size_t* n,uint64_t* seq,bool* evidence,uint64_t* push_ns=0) {
        pthread_mutex_lock(&mutex_);
        int pick=-1;Header best=Header();
        for(int i=0;i<2;++i) {
            if(!ring_[i].used)continue;
            Header h;ring_[i].header(&h);
            if(pick<0 || h.seq<best.seq) { pick=i;best=h; }
        }
        if(pick<0 || capacity<=best.len) { pthread_mutex_unlock(&mutex_);return false; }
        Ring& r=ring_[pick];
        r.get((r.head+HEADER)%r.cap,out,best.len);out[best.len]=0;
        r.head=(r.head+HEADER+best.len)%r.cap;r.used-=HEADER+best.len;
        fold_held();
        held_push_ns_=best.push_ns;
        pthread_mutex_unlock(&mutex_);
        *n=best.len;*seq=best.seq;*evidence=pick==1;
        if(push_ns)*push_ns=best.push_ns;
        return true;
    }
    // Consumer: the held row was handed to stdio (written, not flushed).
    void row_written() {
        pthread_mutex_lock(&mutex_);
        fold_held();
        pthread_mutex_unlock(&mutex_);
    }
    // Consumer: every row handed to stdio before this call reached the
    // kernel (successful fflush). A row still held is not covered.
    void flushed() {
        pthread_mutex_lock(&mutex_);
        unflushed_push_ns_=0;
        pthread_mutex_unlock(&mutex_);
    }
    // oldest_push_ns: push time of the oldest row still queued (0: empty).
    // held_push_ns: the popped row not yet written (0: none).
    // unflushed_push_ns: oldest row written since the last flushed() (0: none).
    struct Stats { uint64_t next_seq,dropped_rows,dropped_bytes,oldest_push_ns,held_push_ns,unflushed_push_ns;
                   size_t used,high_water; };
    Stats stats() {
        pthread_mutex_lock(&mutex_);
        uint64_t oldest=0;
        for(int i=0;i<2;++i) {
            if(!ring_[i].used)continue;
            Header h;ring_[i].header(&h);
            if(h.push_ns && (!oldest || h.push_ns<oldest))oldest=h.push_ns;
        }
        const Stats s={next_seq_,dropped_rows_,dropped_bytes_,oldest,held_push_ns_,unflushed_push_ns_,
                       ring_[0].used+ring_[1].used,high_water_};
        pthread_mutex_unlock(&mutex_);
        return s;
    }
private:
    // Mutex held. A held row becomes "written, not yet flushed".
    void fold_held() {
        if(held_push_ns_ && (!unflushed_push_ns_ || held_push_ns_<unflushed_push_ns_))
            unflushed_push_ns_=held_push_ns_;
        held_push_ns_=0;
    }
    struct Header { uint32_t len;uint64_t seq,push_ns; };
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
        void header(Header* out) const {
            unsigned char h[HEADER];get(head,h,HEADER);
            memcpy(&out->len,h,4);memcpy(&out->seq,h+4,8);memcpy(&out->push_ns,h+12,8);
        }
    };
    pthread_mutex_t mutex_;
    pthread_cond_t cond_;
    bool cond_ok_;
    Ring ring_[2];
    uint64_t next_seq_,dropped_rows_,dropped_bytes_,held_push_ns_,unflushed_push_ns_;
    size_t high_water_;
    JournalRing(const JournalRing&);
    JournalRing& operator=(const JournalRing&);
};

// Row class by its leading kind (every journal row starts with {"kind":"..."}).
// Evidence by kind: BETA rows, POSITION rows, SEND rows with choice !=
// ORIGINAL (0) and the boot/lifecycle/stop rows. Everything else is
// diagnostic. The caller may still queue a RAW-context row as diagnostic.
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
