// JournalRing (2026-10-06): order across the two classes, drop-oldest of
// diagnostic rows only, evidence FULL, wrap-around and classification.
#include "runtime/journal_ring.h"
#include <atomic>
#include <cassert>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using mx5::runtime::JournalRing;
using mx5::runtime::journal_evidence_row;

static std::string row(const char* kind,unsigned i,size_t pad=0) {
    char b[128];snprintf(b,sizeof b,"{\"kind\":\"%s\",\"i\":%u,\"p\":\"",kind,i);
    return std::string(b)+std::string(pad,'x')+"\"}";
}
static void classification() {
    const char* evidence[]={"{\"kind\":\"beta_state\",\"x\":1}","{\"kind\":\"beta_anchor\"}",
        "{\"kind\":\"beta_hold\"}","{\"kind\":\"beta_session_storage\"}","{\"kind\":\"beta_summary\"}",
        "{\"kind\":\"beta_reverse_latch\"}","{\"kind\":\"boot\",\"schema\":1}","{\"kind\":\"position\",\"call\":1}",
        "{\"kind\":\"capture_end\"}","{\"kind\":\"capture_incomplete\"}","{\"kind\":\"shadow_boot\"}",
        "{\"kind\":\"shadow_disabled\"}","{\"kind\":\"storage_stop\",\"stream\":\"trace\"}",
        "{\"kind\":\"send\",\"call\":1,\"type\":1,\"length\":48,\"choice\":3,\"reason\":0,\"request\":{\"choice\":0}}",
        "{\"kind\":\"send\",\"call\":1,\"choice\":4}","{\"kind\":\"send\",\"call\":1}"};
    for(size_t i=0;i<sizeof evidence/sizeof evidence[0];++i)
        assert(journal_evidence_row(evidence[i],strlen(evidence[i])));
    const char* diagnostic[]={"{\"kind\":\"send\",\"call\":1,\"type\":3,\"choice\":0,\"reason\":0}",
        "{\"kind\":\"motion_batch\",\"events\":[]}","{\"kind\":\"health\"}","{\"kind\":\"shadow\"}",
        "{\"kind\":\"positional\"}","{\"kind\":\"bootx\"}","{\"kind\":\"shadow_input_reset\"}","not json","{}"};
    for(size_t i=0;i<sizeof diagnostic/sizeof diagnostic[0];++i)
        assert(!journal_evidence_row(diagnostic[i],strlen(diagnostic[i])));
}

static uint64_t now_ns() { struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return uint64_t(t.tv_sec)*1000000000ULL+uint64_t(t.tv_nsec); }
// Held and written-not-flushed rows stay timed until flushed() (2026-10-07):
// the journal lag counts rows still in the consumer's hands or in stdio.
static void held_and_unflushed() {
    static unsigned char d[400],e[400];
    JournalRing ring(d,sizeof d,e,sizeof e);
    char out[JournalRing::MAX_ROW+1];size_t n;uint64_t seq,at;bool evidence;
    assert(ring.push("{\"kind\":\"health\"}",17,false,100)==JournalRing::PUSHED);
    assert(ring.push("{\"kind\":\"health\"}",17,false,200)==JournalRing::PUSHED);
    // Popped: no longer queued, but held, in the same critical section.
    assert(ring.pop(out,sizeof out,&n,&seq,&evidence,&at) && at==100);
    JournalRing::Stats s=ring.stats();
    assert(s.oldest_push_ns==200 && s.held_push_ns==100 && s.unflushed_push_ns==0);
    ring.row_written();
    s=ring.stats();assert(s.held_push_ns==0 && s.unflushed_push_ns==100);
    assert(ring.pop(out,sizeof out,&n,&seq,&evidence,&at) && at==200);
    s=ring.stats();assert(s.oldest_push_ns==0 && s.held_push_ns==200 && s.unflushed_push_ns==100);
    ring.flushed();   // covers only rows handed to stdio, not the held one
    s=ring.stats();assert(s.held_push_ns==200 && s.unflushed_push_ns==0);
    ring.row_written();
    s=ring.stats();assert(s.held_push_ns==0 && s.unflushed_push_ns==200);
    // A held row not marked written before the next pop is still counted.
    assert(ring.push("{\"kind\":\"health\"}",17,false,300)==JournalRing::PUSHED);
    assert(ring.pop(out,sizeof out,&n,&seq,&evidence,&at) && at==300);
    assert(ring.push("{\"kind\":\"health\"}",17,false,400)==JournalRing::PUSHED);
    assert(ring.pop(out,sizeof out,&n,&seq,&evidence,&at) && at==400);
    s=ring.stats();assert(s.held_push_ns==400 && s.unflushed_push_ns==200);
    ring.flushed();ring.row_written();
    s=ring.stats();assert(s.held_push_ns==0 && s.unflushed_push_ns==400);
    ring.flushed();
    s=ring.stats();assert(!s.oldest_push_ns && !s.held_push_ns && !s.unflushed_push_ns);
    puts("journal ring: held and written-not-flushed rows stay timed until flushed()");
}
// wait(): a request stored before notify() but after the consumer's own
// check must not be slept through (2026-10-07 re-review).
struct Requests { JournalRing* ring; std::atomic<uint64_t> target,done; std::atomic<unsigned> stop,gap; };
static bool requests_pending(void* p) {
    Requests& r=*static_cast<Requests*>(p);
    return r.stop.load() || r.target.load()>r.done.load();
}
static void* request_consumer(void* p) {
    Requests& r=*static_cast<Requests*>(p);
    for(unsigned i=0;;++i) {
        const bool stop=r.stop.load()!=0;
        const uint64_t t=r.target.load();
        if(t>r.done.load())r.done.store(t);
        if(stop)break;
        // Injected interleaving: widen the window between the check above
        // and wait() taking the mutex.
        if(r.gap.load() && i%3==0) { if(i%2)sched_yield();else { const struct timespec g={0,20000};nanosleep(&g,0); } }
        r.ring->wait(5000000000ULL,requests_pending,&r);
    }
    return 0;
}
static void wait_requests() {
    static unsigned char d[400],e[400];
    JournalRing ring(d,sizeof d,e,sizeof e);
    Requests r;r.ring=&ring;r.target.store(0);r.done.store(0);r.stop.store(0);r.gap.store(0);
    // Deterministic interleaving in one thread: the consumer has checked
    // (nothing to do), the request and its notify() land, then it waits.
    uint64_t begin=now_ns();
    ring.wait(50000000ULL);                        // nothing pending: sleeps the timeout
    assert(now_ns()-begin>=40000000ULL);
    r.target.store(1);ring.notify();               // signal with nobody waiting
    begin=now_ns();
    ring.wait(50000000ULL);                        // without a predicate the request is missed
    const uint64_t missed=now_ns()-begin;
    begin=now_ns();
    ring.wait(5000000000ULL,requests_pending,&r);  // with it: returns at once
    const uint64_t seen=now_ns()-begin;
    assert(missed>=40000000ULL && seen<20000000ULL);
    r.target.store(0);
    r.stop.store(1);ring.notify();
    begin=now_ns();ring.wait(5000000000ULL,requests_pending,&r);
    assert(now_ns()-begin<20000000ULL);
    r.stop.store(0);
    // Stress: many requests against a consumer thread with an injected gap;
    // a lost wake-up would cost the 5 s timeout.
    r.gap.store(1);
    pthread_t thread;assert(pthread_create(&thread,0,request_consumer,&r)==0);
    uint64_t worst=0;const unsigned N=3000;
    for(unsigned i=1;i<=N;++i) {
        const uint64_t t0=now_ns();
        r.target.store(i);ring.notify();
        while(r.done.load()<i) {
            assert(now_ns()-t0<2000000000ULL);
            if(i%7==0)sched_yield();
        }
        const uint64_t spent=now_ns()-t0;if(spent>worst)worst=spent;
    }
    const uint64_t t0=now_ns();
    r.stop.store(1);ring.notify();
    assert(pthread_join(thread,0)==0);
    const uint64_t join=now_ns()-t0;
    printf("journal ring wait: missed-without-predicate %.1f ms, with %.3f ms; %u requests worst %.2f ms; stop %.2f ms\n",
           missed/1e6,seen/1e6,N,worst/1e6,join/1e6);
    assert(worst<2000000000ULL && join<2000000000ULL);
}
// BULK class (2026-10-08): the paced persistent RAW window. Global order
// with the other classes, drop-oldest within its own ring, never timed for
// the journal lag (queued, held or unflushed), while a prompt row queued
// behind bulk rows is timed from its own push. Without a bulk ring a BULK
// row is queued as diagnostic.
static void bulk_class() {
    static unsigned char d[4096],e[4096],b[1024];
    JournalRing r(d,sizeof d,e,sizeof e,b,sizeof b);
    char out[JournalRing::MAX_ROW+1];size_t n;uint64_t seq,push;bool evidence,bulk;
    for(unsigned i=0;i<3;++i) {
        const std::string x=row("motion_batch",i);
        assert(r.push_class(x.data(),x.size(),JournalRing::BULK,100+i)==JournalRing::PUSHED);
    }
    JournalRing::Stats st=r.stats();
    assert(!st.oldest_push_ns && st.bulk_used>0 && st.used==st.bulk_used);
    const std::string p=row("position",9);
    assert(r.push(p.data(),p.size(),true,200)==JournalRing::PUSHED);
    st=r.stats();
    assert(st.oldest_push_ns==200);                 // the prompt row behind the bulk rows
    // Pop order is the push order; a popped bulk row is not held.
    assert(r.pop(out,sizeof out,&n,&seq,&evidence,&push,&bulk) && seq==0 && bulk && !evidence && push==100);
    st=r.stats();assert(!st.held_push_ns && st.oldest_push_ns==200);
    r.row_written();assert(!r.stats().unflushed_push_ns);
    assert(r.pop(out,sizeof out,&n,&seq,&evidence,&push,&bulk) && seq==1 && bulk);r.row_written();
    assert(r.pop(out,sizeof out,&n,&seq,&evidence,&push,&bulk) && seq==2 && bulk);r.row_written();
    assert(!r.stats().unflushed_push_ns);
    assert(r.pop(out,sizeof out,&n,&seq,&evidence,&push,&bulk) && seq==3 && !bulk && evidence);
    assert(r.stats().held_push_ns==200);r.row_written();
    assert(r.stats().unflushed_push_ns==200);r.flushed();
    // Overflow drops the oldest bulk rows only (counted), never prompt rows.
    unsigned pushed=0;
    for(unsigned i=0;i<40;++i) {
        const std::string x=row("motion_batch",100+i,40);
        const JournalRing::Result res=r.push_class(x.data(),x.size(),JournalRing::BULK,300+i);
        assert(res==JournalRing::PUSHED || res==JournalRing::PUSHED_AFTER_DROP);++pushed;
    }
    st=r.stats();assert(st.dropped_rows>0 && st.dropped_bulk_rows==st.dropped_rows && st.bulk_used<=sizeof b && !st.oldest_push_ns);
    // No bulk ring: BULK rows are diagnostic (and timed).
    static unsigned char d2[1024],e2[1024];
    JournalRing plain(d2,sizeof d2,e2,sizeof e2);
    const std::string x=row("motion_batch",1);
    assert(plain.push_class(x.data(),x.size(),JournalRing::BULK,500)==JournalRing::PUSHED);
    assert(plain.stats().oldest_push_ns==500 && !plain.stats().bulk_used);
    assert(plain.pop(out,sizeof out,&n,&seq,&evidence,&push,&bulk) && !bulk && !evidence);
    puts("journal ring: BULK rows keep the global order, drop oldest, and are never timed as lag");
}
int main() {
    classification();
    static unsigned char d[600],e[400];
    JournalRing ring(d,sizeof d,e,sizeof e);
    char out[JournalRing::MAX_ROW+1];size_t n;uint64_t seq;bool evidence;
    assert(!ring.pop(out,sizeof out,&n,&seq,&evidence));
    // Interleaved classes come out in push order.
    std::vector<std::string> pushed;
    for(unsigned i=0;i<6;++i) {
        const std::string r=row(i%3?"health":"beta_state",i);
        assert(ring.push(r.data(),r.size(),i%3==0)==JournalRing::PUSHED);pushed.push_back(r);
    }
    for(unsigned i=0;i<6;++i) {
        assert(ring.pop(out,sizeof out,&n,&seq,&evidence));
        assert(seq==i && n==pushed[i].size() && std::string(out)==pushed[i] && evidence==(i%3==0));
    }
    assert(!ring.pop(out,sizeof out,&n,&seq,&evidence));
    // Many laps: wrap-around keeps every byte.
    for(unsigned i=0;i<500;++i) {
        const std::string r=row("motion_batch",i,i%97);
        assert(ring.push(r.data(),r.size(),false)==JournalRing::PUSHED);
        assert(ring.pop(out,sizeof out,&n,&seq,&evidence) && std::string(out)==r && !evidence && seq==6+i);
    }
    // Overflow: the oldest diagnostic rows are dropped, evidence never.
    const uint64_t base=506;
    std::vector<std::string> diag,evid;
    for(unsigned i=0;i<40;++i) { diag.push_back(row("health",i,20)); }
    for(unsigned i=0;i<3;++i) { evid.push_back(row("beta_hold",i,20)); }
    unsigned drops=0;
    for(unsigned i=0;i<40;++i) {
        if(i==10 || i==20 || i==30) assert(ring.push(evid[i/10-1].data(),evid[i/10-1].size(),true)==JournalRing::PUSHED);
        const JournalRing::Result r=ring.push(diag[i].data(),diag[i].size(),false);
        assert(r==JournalRing::PUSHED || r==JournalRing::PUSHED_AFTER_DROP);
        if(r==JournalRing::PUSHED_AFTER_DROP)++drops;
    }
    const JournalRing::Stats s=ring.stats();
    assert(drops && s.dropped_rows>0 && s.next_seq==base+43 && s.high_water<=sizeof d+sizeof e);
    std::vector<uint64_t> seqs;unsigned evidence_out=0;std::vector<std::string> survivors;
    while(ring.pop(out,sizeof out,&n,&seq,&evidence)) {
        if(!seqs.empty())assert(seq>seqs.back());                   // order preserved
        seqs.push_back(seq);
        if(evidence) { assert(std::string(out)==evid[evidence_out]);++evidence_out; }
        else survivors.push_back(out);
    }
    assert(evidence_out==3 && survivors.size()+s.dropped_rows==40);
    // The survivors are exactly the newest diagnostic rows (drop OLDEST).
    for(size_t i=0;i<survivors.size();++i)assert(survivors[i]==diag[40-survivors.size()+i]);
    // An evidence row that does not fit is FULL, never a silent drop.
    for(unsigned i=0;;++i) {
        const std::string r=row("beta_state",i,50);
        const JournalRing::Result result=ring.push(r.data(),r.size(),true);
        if(result==JournalRing::FULL)break;
        assert(result==JournalRing::PUSHED && i<10);
    }
    assert(ring.stats().dropped_rows==s.dropped_rows);
    // Push times: the oldest queued row's time (journal lag), per pop.
    {
        static unsigned char d2[400],e2[400];
        JournalRing timed(d2,sizeof d2,e2,sizeof e2);
        assert(timed.stats().oldest_push_ns==0);
        assert(timed.push("{\"kind\":\"health\"}",17,false,500)==JournalRing::PUSHED);
        assert(timed.push("{\"kind\":\"beta_state\"}",21,true,300)==JournalRing::PUSHED);
        assert(timed.push("{\"kind\":\"health\"}",17,false,900)==JournalRing::PUSHED);
        assert(timed.stats().oldest_push_ns==300);
        uint64_t at=0;
        assert(timed.pop(out,sizeof out,&n,&seq,&evidence,&at) && seq==0 && at==500);
        assert(timed.pop(out,sizeof out,&n,&seq,&evidence,&at) && seq==1 && at==300 && evidence);
        assert(timed.stats().oldest_push_ns==900);
        timed.notify();timed.wait(1000000);                     // returns: a row is queued
        assert(timed.pop(out,sizeof out,&n,&seq,&evidence,&at) && at==900);
        assert(timed.stats().oldest_push_ns==0);
    }
    std::string huge(JournalRing::MAX_ROW+1,'x');
    assert(ring.push(huge.data(),huge.size(),false)==JournalRing::TOO_LARGE);
    std::string big(700,'x');   // larger than the diagnostic ring itself
    assert(ring.push(big.data(),big.size(),false)==JournalRing::TOO_LARGE);
    held_and_unflushed();
    bulk_class();
    wait_requests();
    puts("journal ring: push order across classes, wrap-around, drop-oldest diagnostic, evidence FULL passed");
    return 0;
}
