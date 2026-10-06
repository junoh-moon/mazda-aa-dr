// JournalRing (2026-10-06): order across the two classes, drop-oldest of
// diagnostic rows only, evidence FULL, wrap-around and classification.
#include "runtime/journal_ring.h"
#include <cassert>
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
        "{\"kind\":\"shadow_disabled\"}",
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
    std::string huge(JournalRing::MAX_ROW+1,'x');
    assert(ring.push(huge.data(),huge.size(),false)==JournalRing::TOO_LARGE);
    std::string big(700,'x');   // larger than the diagnostic ring itself
    assert(ring.push(big.data(),big.size(),false)==JournalRing::TOO_LARGE);
    puts("journal ring: push order across classes, wrap-around, drop-oldest diagnostic, evidence FULL passed");
    return 0;
}
