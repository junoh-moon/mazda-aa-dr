#ifndef MX5_RUNTIME_G3_HEADING_H
#define MX5_RUNTIME_G3_HEADING_H
// G3 first-connection heading, RECORD-ONLY (validation/G3_RECORD_ONLY_2026-10-11.md,
// design: validation/G3_FIRST_CONNECTION_HEADING_2026-10-11.md section 5/6).
// The BETA product persists its yaw-integrated BODY heading (with its honest
// uncertainty) when the car is parked, restores it at the next boot and
// journals the heading it WOULD send at the first moments of a session and
// its error against the first policy-M-accepted GPS course. Nothing here
// changes a send, an OEM contract, errno or timing:
//  * G3Tracker runs on the journal worker on the MODEL tick (after the BETA
//    tick); it only reads pipeline views and observations and writes
//    diagnostic journal rows (g3_store, g3_first). It never calls j.fail(): a
//    row that does not fit is dropped and counted.
//  * The store file is written only by a dedicated detached store thread
//    (one-slot mailbox below), never by the worker, the journal writer, an
//    OEM thread or a tap; for a park of >= 15 s (garage carry: 5 s) with all
//    four wheels exactly zero and, with the garage carry, for the departure
//    mark after such a park (see G3_GARAGE_*), under the idle rule, and every
//    I/O step is re-checked against a cancel generation and a stop flag.
//    Every failure degrades to "no store" and is journaled.
//  * Restore reads one small file once at worker start (startup slow path);
//    the worker also reads the previous-product-boot token there. With the
//    garage carry the token is rewritten LAZILY (G3_TOKEN_*: through the
//    store thread once this boot moved, was blind or wants its first S
//    record; synchronously only at a terminal fault or capture stop, or
//    once after G3_TOKEN_FAILURES_MAX failed token writes or a store that
//    went off while the token is due, never while BETA replaces) and a
//    small worker-boot file is written at start (g3_worker_boot_mark); with
//    a marker the token is swapped at start as before (g3_last_start_swap,
//    one small write without fsync).
// MODEL evidence only: no qualified claim, no Domain::QUALIFIED.
#include "adapter/adapter.h"
#include "navigation/pipeline.h"
#include <atomic>
#include <cmath>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace mx5 { namespace runtime {

// ---- named constants (reasons in the record) -------------------------------
// Store file in <root>/logs (the directory the runtime already writes and the
// menu 3 export already archives); the temporary name is dot-prefixed.
static const char G3_STORE_NAME[]="g3-heading.v1";
static const char G3_STORE_TEMP[]=".g3-heading.v1.tmp";
// Owner off switch: <root>/g3-off (any type). Read once at worker start.
static const char G3_OFF_NAME[]="g3-off";
// Guard decline record (read only, for the boot-chain check).
static const char G3_GUARD_DECISION[]="guard/last-decision";
static const unsigned G3_STORE_VERSION=1;
static const size_t G3_RECORD_MAX=256;
// Restore limits. 30 deg: the design's injection limit (G3 design section 5).
static const double G3_UNC_MAX_DEG=30.0;
// Age bounds unknown events (age itself is physically irrelevant); 7 days.
static const uint64_t G3_AGE_MAX_S=7ULL*24*3600;
// Position continuity at the first GPS fix: farther than this plus the
// stored position budget plus the wheel distance driven since boot = moved.
static const double G3_MOVED_MIN_M=50.0;
// I/O discipline (review 2026-10-11, H1): with the g3-garage-off marker the
// ONLY store write is the PARKED write: all four wheels EXACTLY zero (the
// beta.8 stop-latch definition) for at least G3_PARKED_NS with fresh wheel
// samples throughout (navigation::G3_WHEEL_GAP_NS), at most once per stop,
// and only while the store I/O is idle-safe (g3_io_block_reason); no record
// is written while moving or creeping. The garage carry (default, below)
// RELAXES this invariant: the parked write needs only 5 s and may run while
// BETA replaces (no fsync), and the departure mark is written WHILE THE CAR
// MOVES (no fsync, also while BETA replaces). Bounds: G3_GARAGE_REPLACING_*.
static const uint64_t G3_PARKED_NS=15000000000ULL;
// A parked stop is written when the car moved at least this far since the
// last record, or (M1) when the valid heading differs from the last record
// by at least G3_HEADING_CHANGE_DEG (a short parking adjustment).
static const double G3_MOVING_MARK_M=2.0;
static const double G3_HEADING_CHANGE_DEG=1.0;
// fsync only for long stops (third review, 2026-10-11): "parked" (>= 15 s at
// exactly zero) includes red lights and traffic jams, and the store fsync is
// the only fsync the runtime does while driving (the journal fsyncs only at
// capture stop). A stop of 15-60 s gets temp write + rename WITHOUT fsync:
// still atomic (rename), durability left to the kernel/relfs writeback. When
// the same stop passes G3_FSYNC_MIN_STOP_S, the record is written once more
// WITH the single file fsync. Both writes obey every cancel rule.
static const double G3_FSYNC_MIN_STOP_S=60.0;
static const uint64_t G3_FSYNC_MIN_STOP_NS=uint64_t(G3_FSYNC_MIN_STOP_S*1e9);
// At most one store write per 5 s.
static const uint64_t G3_WRITE_MIN_NS=5000000000ULL;
// Store I/O waits while the journal writer still has rows older than this
// (its own age flush bound is 250 ms).
static const uint64_t G3_WRITER_IDLE_LAG_NS=300000000ULL;
// The dedicated store thread polls its one-slot mailbox at this period.
static const uint64_t G3_STORE_POLL_NS=100000000ULL;
// M4: the yaw zero drifts after engine start (trip6a: -2.5 deg/s mean over
// 10 s with all wheels at zero), far above the rule-5 budget (0.11 deg/s).
// g3_first rows also journal "unc_d": the propagated uncertainty plus this
// drift over the first G3_STARTUP_DRIFT_WINDOW_S seconds of motion. Neither
// value qualifies a heading; err_p is the measurement.
static const double G3_STARTUP_DRIFT_DEG_S=2.5;
static const double G3_STARTUP_DRIFT_WINDOW_S=60.0;
// g3_first "t30" row: this long after the first LOCATION send.
static const uint64_t G3_FIRST_WINDOW_NS=30000000000ULL;
// The first M-accepted course CONFIRMS a carried heading within this error.
static const double G3_CONFIRM_MAX_DEG=30.0;
// Store writes stop for the boot after this many consecutive failures.
static const unsigned G3_WRITE_FAILURES_MAX=3;
// A clock value below this (2020-09-13) is not a usable wall time.
static const uint64_t G3_PLAUSIBLE_UTC_S=1600000000ULL;
// Garage carry (validation/G3_GARAGE_EXIT_FIX_2026-10-17.md). BETA only,
// default on; the owner switch <root>/g3-garage-off (any type, read once at
// worker start) restores the beta.16 store/restore/send rules exactly.
//  * Departure mark: once the car has moved G3_GARAGE_DEPART_M (or turned
//    G3_GARAGE_DEPART_DEG) after the S record on disk, the record is replaced
//    by a state 'M' record without heading (no fsync). An 'S' record on disk
//    therefore means "parked since it was written" for every boot that ran
//    this rule (flag G3_FLAG_DEPART in the record).
//  * The parked write needs G3_GARAGE_PARKED_NS of exact standstill and may
//    run (without fsync) while BETA replaces (GPS lost / NO_FIX in a garage);
//    the fsync write never runs while BETA replaces. 2026-10-17 trip: the
//    final garage park lasted 7.5 s before the sensors stopped and BETA was
//    ENGAGED, so beta.16 kept a record from 320 s and 1.2 km earlier.
//  * Restore: a G3_FLAG_DEPART record (S, heading, chain ok, other boot) is
//    CARRIED with reason chain_depart when no wall clock proves its age (the
//    CMU clock is 1970 until GPS; never in a garage). A known age > 7 days
//    still rejects; the first-fix position and GPS age checks still apply.
//    Only when the record's boot is the PREVIOUS PRODUCT BOOT (review P1):
//    <root>/logs/g3-last-start names the newest product boot that could
//    have moved the car without this boot seeing it, so an S record written
//    before such a boot (journal failure, no G3 tick, a g3-off or
//    g3-garage-off marker, a drive) is not carried. Lazy token (2026-10-18,
//    validation/G3_GARAGE_EXIT_FIX_2026-10-17.md "fourth fix"): a garage-on
//    boot reads it at start but rewrites it with its own id only when it
//    observed >= G3_TOKEN_MOVED_M of raw wheel distance, was blind (a raw
//    wheel gap > G3_TOKEN_GAP_NS that ended, or no raw wheel event for
//    G3_TOKEN_SILENT_NS), wants its first S record (the token lands BEFORE
//    any S record of this boot, so a record and the token name the same
//    writer; that diverted request takes the S request's gate, without the
//    departure mark's writer-lag rule), or ends observation (journal/audit
//    fault, capture stop, no store thread: synchronous; also once after
//    G3_TOKEN_FAILURES_MAX failed token writes or when the store went off
//    while the token is due). Token write failures never turn the store
//    off; when the synchronous attempt failed too, the boot's S records are
//    written without the token (the next boot refuses them: not_prev_boot). A boot that only stood (ignition on, no
//    drive) leaves the token, so the next boot still carries the record of
//    the drive before it. A second worker start in the same boot (worker-
//    boot file names this boot) or a lost worker-boot write swaps at once.
//    A boot with either marker or without the BETA core swaps at start (the
//    beta.16/HEAD behaviour, also so a marker boot never lets a stale S
//    through).
//  * Uncertainty of the GARAGE CHAIN (review C2/P4): the stored uncertainty
//    of every record a garage-on boot writes (they all carry flag 4) and the
//    propagation of a carried flag-4 record grow with the rotation term
//    G3_GARAGE_ROTATION_PER_RAD (2 % instead of BETA's 10 %) and the time
//    term max(BETA, G3_GARAGE_YAW_ERROR_RAD_S); the rail allowance is
//    unchanged; limit G3_GARAGE_UNC_MAX_DEG for flag-4 records. A carried
//    record WITHOUT flag 4 (written by beta.16 or with the marker) keeps the
//    BETA terms and the 30 deg limit. With the marker a flag-4 record is
//    rejected as a format error, exactly as beta.16 decodes it, so no build
//    applies the 30 deg rules to a 2 % record. 2 %: the 2026-10-17 E-Mart
//    chain (1,392 deg of |rotation|) left about 15-20 deg of error in the
//    digest model (one sample). 0.0023 rad/s: the RMS of the 93 parked yaw
//    zeros (stops >= 5 s, 2026-10-10/11/17) around the integration zero 2048
//    is 3.40 counts = 0.00224 rad/s, above BETA's 0.002 (pre-registered,
//    validation/G3_GARAGE_EXIT_FIX_2026-10-17.md "P4").
//  * I/O while BETA replaces (review P3, the G1 tunnel path): a parked S
//    request needs G3_GARAGE_REPLACING_S_GAP_NS since the previous S request
//    (any S request: also a cancelled or failed one and the 60 s fsync
//    rewrite), except the rule F rewrite of a tainted heading and (third
//    review C1) while the record on disk is an S with heading, i.e. the car
//    left it without a landed departure mark: the new S then replaces a
//    record that would otherwise be carried stale. The exemption is bounded
//    by the 5 s request spacing (G3_WRITE_MIN_NS), not by departures: one S
//    per stop while the mark keeps failing, and a cancelled S request is
//    retried after 5 s. The departure mark is never spaced (it is what keeps an S
//    record meaning "parked since"); it is requested while the car moves or
//    within the first 5 s of a stop (before the stop counts as parked) and
//    needs a journal writer lag <= G3_GARAGE_REPLACING_MARK_LAG_NS.
static const char G3_GARAGE_OFF_NAME[]="g3-garage-off";
static const uint64_t G3_GARAGE_PARKED_NS=5000000000ULL;
static const double G3_GARAGE_DEPART_M=0.5;
static const double G3_GARAGE_DEPART_DEG=2.0;
static const double G3_GARAGE_ROTATION_PER_RAD=0.02;
static const double G3_GARAGE_YAW_ERROR_RAD_S=0.0023;
// The propagated value is an uncertainty BUDGET, not a guaranteed envelope
// of the real error: in 3 of 9 private chains with a bound <= 60 deg the
// real carried error was larger (validation/G3_UNC_LIMIT_2026-10-18.md); the
// window's end conditions (trust, conflict, m_course, caps) bound the cost.
static const double G3_GARAGE_UNC_MAX_DEG=60.0;
static const uint64_t G3_GARAGE_REPLACING_S_GAP_NS=30000000000ULL;
static const uint64_t G3_GARAGE_REPLACING_MARK_LAG_NS=100000000ULL;
// Previous-product-boot token (review P1): <root>/logs/g3-last-start.
static const char G3_LAST_START_NAME[]="g3-last-start";
static const char G3_LAST_START_TEMP[]=".g3-last-start.tmp";
// The synchronous (terminal) token write uses its own temporary name, so it
// never races an in-flight store-thread token write on one temporary file.
static const char G3_LAST_START_SYNC_TEMP[]=".g3-last-start.sync.tmp";
// Lazy token (garage carry only). Worker-boot file: the boot id of the
// newest garage-on worker start ("mx5g3worker 1 <boot_id>"); a start that
// finds its own boot there is a second worker start in this boot (the time
// between the two workers was not observed: swap at once).
static const char G3_WORKER_BOOT_NAME[]="g3-worker-boot";
static const char G3_WORKER_BOOT_TEMP[]=".g3-worker-boot.tmp";
// Swap triggers from the RAW wheel stream (G3TokenWatch: every received
// wheel event, admitted to the MODEL or not; without an AA session the
// pipeline sees no wheel event at all, so the pipeline distance alone would
// call every idle boot blind). Moved: G3_MOVING_MARK_M of mean wheel
// distance (the G3Motion::distance_m rule). Gap: a raw receipt gap above
// navigation::G3_BOOT_GAP_MAX_NS (2 s: too short to start, turn and stop
// again; the largest receipt gap in the 14 boots of the 2026-10-17 export
// was 1.03 s) that ENDED with a new event. Silent: no raw wheel event for
// G3_TOKEN_SILENT_NS while the worker runs (the stream never started, or
// stopped and stayed away): 30 s, three times the longest sensor-stop to
// journal-end tail of those 14 boots (9.6 s at ignition off); an ignition
// that stays in accessory mode longer swaps (cost only).
static const double G3_TOKEN_MOVED_M=G3_MOVING_MARK_M;
static const uint64_t G3_TOKEN_SILENT_NS=30000000000ULL;
static const uint32_t G3_TOKEN_SEQ_BASE=0x80000000u;
// Lazy token write failures (fifth fix, review P3): counted apart from the
// record failures (a failed token write never turns the store off by itself,
// so the boot's own S record can still land); after this many the token
// is handed to ONE synchronous write on the worker (G3Tracker::token_sync_due,
// reason "token_failed", only while BETA does not replace), as is a store
// that went off for record failures while the token is due ("store_off").
static const unsigned G3_TOKEN_FAILURES_MAX=3;
// Store thread mailbox targets.
enum G3Target { G3_TARGET_STORE=0, G3_TARGET_TOKEN=1 };

// ---- CRC32 (IEEE 802.3, reflected 0xEDB88320) --------------------------------
inline uint32_t g3_crc32(const void* data,size_t n) {
    const unsigned char* p=static_cast<const unsigned char*>(data);
    uint32_t c=0xFFFFFFFFu;
    for(size_t i=0;i<n;++i) {
        c^=p[i];
        for(unsigned k=0;k<8;++k)c=(c>>1)^(0xEDB88320u&(0u-(c&1u)));
    }
    return c^0xFFFFFFFFu;
}

// ---- record ------------------------------------------------------------------
// One text line:
//   mx5g3 <version> <seq> <boot_id> <S|M> <flags> <heading_cdeg> <unc_cdeg>
//         <lat_e7> <lon_e7> <pos_err_dm> <utc_s> <rt_s> <mono_ms> <chain> <crc32>
// flags: 1 heading valid, 2 position valid, 4 written by a build that marks
// departures (garage carry: an S record means parked since). utc_s: GPS-derived wall time at
// the write (0 unknown); rt_s: CLOCK_REALTIME at the write; mono_ms: the
// writing boot's CLOCK_MONOTONIC; chain: the guard's last-decision token at
// the writing boot ("none", "unread" or 8 hex). crc32 covers every byte
// before the space that precedes it.
enum { G3_FLAG_HEADING=1, G3_FLAG_POSITION=2, G3_FLAG_DEPART=4 };
struct G3Record {
    unsigned version;
    uint32_t seq;
    char boot_id[37];
    char state;             // 'S' stopped, 'M' moving
    unsigned flags;
    int32_t heading_cdeg;   // body heading 0..35999 (0.01 deg)
    int32_t unc_cdeg;       // honest heading uncertainty 0..18000
    int32_t lat_e7, lon_e7;
    uint32_t pos_err_dm;    // position budget (0.1 m)
    uint64_t utc_s, rt_s, mono_ms;
    char chain[9];
};
inline G3Record g3_record() {
    G3Record r;
    memset(&r,0,sizeof r);
    r.version=G3_STORE_VERSION;r.state='M';
    strcpy(r.boot_id,"unknown");strcpy(r.chain,"unread");
    return r;
}
// Returns the line length (with '\n'), 0 when it does not fit.
inline size_t g3_encode(const G3Record& r,char* out,size_t cap) {
    if(!out || cap<2)return 0;
    const int n=snprintf(out,cap,"mx5g3 %u %lu %s %c %u %ld %ld %ld %ld %lu %llu %llu %llu %s",
        r.version,(unsigned long)r.seq,r.boot_id,r.state,r.flags,(long)r.heading_cdeg,(long)r.unc_cdeg,
        (long)r.lat_e7,(long)r.lon_e7,(unsigned long)r.pos_err_dm,(unsigned long long)r.utc_s,
        (unsigned long long)r.rt_s,(unsigned long long)r.mono_ms,r.chain);
    if(n<=0 || size_t(n)+10>=cap)return 0;
    const uint32_t crc=g3_crc32(out,size_t(n));
    const int m=snprintf(out+n,cap-size_t(n)," %08lx\n",(unsigned long)crc);
    if(m!=10)return 0;
    return size_t(n)+10;
}

enum G3Load {
    G3_LOAD_OK=0, G3_LOAD_MISSING, G3_LOAD_UNREADABLE, G3_LOAD_NOT_REGULAR, G3_LOAD_TOO_LARGE,
    G3_LOAD_EMPTY, G3_LOAD_TRUNCATED, G3_LOAD_MAGIC, G3_LOAD_VERSION_OLD, G3_LOAD_VERSION_FUTURE,
    G3_LOAD_FORMAT, G3_LOAD_CHECKSUM
};
inline const char* g3_load_name(G3Load l) {
    static const char* const names[]={"ok","missing","unreadable","not_regular","too_large","empty",
        "truncated","magic","version_old","version_future","format","checksum"};
    return unsigned(l)<sizeof names/sizeof names[0]?names[l]:"unknown";
}
namespace g3_detail {
inline bool parse_u64(const char* s,size_t n,uint64_t max,uint64_t* out) {
    if(!n || n>20)return false;
    uint64_t v=0;
    for(size_t i=0;i<n;++i) {
        if(s[i]<'0' || s[i]>'9')return false;
        if(n>1 && s[0]=='0')return false;
        const uint64_t d=uint64_t(s[i]-'0');
        if(v>(UINT64_MAX-d)/10)return false;
        v=v*10+d;
    }
    if(v>max)return false;
    *out=v;return true;
}
inline bool parse_i64(const char* s,size_t n,int64_t low,int64_t high,int64_t* out) {
    bool neg=false;
    if(n && s[0]=='-') { neg=true;++s;--n; if(n==1 && s[0]=='0')return false; }
    uint64_t v;
    if(!parse_u64(s,n,uint64_t(INT64_MAX),&v))return false;
    const int64_t x=neg?-int64_t(v):int64_t(v);
    if(x<low || x>high)return false;
    *out=x;return true;
}
inline bool hex8(const char* s,size_t n) {
    if(n!=8)return false;
    for(size_t i=0;i<8;++i)
        if(!((s[i]>='0' && s[i]<='9') || (s[i]>='a' && s[i]<='f')))return false;
    return true;
}
inline bool boot_token(const char* s,size_t n) {
    if(n==7 && !memcmp(s,"unknown",7))return true;
    if(n!=36)return false;
    for(size_t i=0;i<36;++i) {
        if(i==8 || i==13 || i==18 || i==23) { if(s[i]!='-')return false; }
        else if(!((s[i]>='0' && s[i]<='9') || (s[i]>='a' && s[i]<='f') || (s[i]>='A' && s[i]<='F')))
            return false;
    }
    return true;
}
} // namespace g3_detail

// Strict decoder: exactly one record line, every field in range.
inline G3Load g3_decode(const char* buf,size_t n,G3Record* out) {
    using namespace g3_detail;
    if(!n)return G3_LOAD_EMPTY;
    static const char magic[]="mx5g3 ";
    const size_t mn=sizeof magic-1;
    if(n<mn)return memcmp(buf,magic,n)?G3_LOAD_MAGIC:G3_LOAD_TRUNCATED;
    if(memcmp(buf,magic,mn))return G3_LOAD_MAGIC;
    // Version first: a different version may have a different layout.
    size_t at=mn,end=at;
    while(end<n && buf[end]!=' ' && buf[end]!='\n')++end;
    uint64_t version;
    if(!parse_u64(buf+at,end-at,0xFFFFFFFFu,&version))return end>=n?G3_LOAD_TRUNCATED:G3_LOAD_FORMAT;
    if(version<G3_STORE_VERSION)return G3_LOAD_VERSION_OLD;
    if(version>G3_STORE_VERSION)return G3_LOAD_VERSION_FUTURE;
    if(buf[n-1]!='\n')return G3_LOAD_TRUNCATED;
    for(size_t i=0;i+1<n;++i)if(buf[i]=='\n' || buf[i]=='\0')return G3_LOAD_FORMAT;
    // Tokens: exactly 16, single spaces.
    const char* tok[16];size_t len[16];size_t count=0;
    at=0;
    while(at<n-1) {
        end=at;
        while(end<n-1 && buf[end]!=' ')++end;
        if(end==at || count==16)return G3_LOAD_FORMAT;
        tok[count]=buf+at;len[count]=end-at;++count;
        if(end==n-1)break;
        at=end+1;
        if(at==n-1)return G3_LOAD_FORMAT; // trailing space
    }
    if(count<16)return G3_LOAD_TRUNCATED;
    if(!hex8(tok[15],len[15]))return G3_LOAD_FORMAT;
    uint32_t crc=0;
    for(size_t i=0;i<8;++i) {
        const char c=tok[15][i];
        crc=(crc<<4)|uint32_t(c<='9'?c-'0':c-'a'+10);
    }
    const size_t covered=size_t(tok[15]-buf)-1;
    if(g3_crc32(buf,covered)!=crc)return G3_LOAD_CHECKSUM;
    G3Record r=g3_record();
    r.version=unsigned(version);
    uint64_t u;int64_t x;
    if(len[0]!=5 || memcmp(tok[0],"mx5g3",5))return G3_LOAD_MAGIC;
    if(!parse_u64(tok[2],len[2],0xFFFFFFFFu,&u))return G3_LOAD_FORMAT;
    r.seq=uint32_t(u);
    if(!boot_token(tok[3],len[3]))return G3_LOAD_FORMAT;
    memcpy(r.boot_id,tok[3],len[3]);r.boot_id[len[3]]=0;
    if(len[4]!=1 || (tok[4][0]!='S' && tok[4][0]!='M'))return G3_LOAD_FORMAT;
    r.state=tok[4][0];
    if(!parse_u64(tok[5],len[5],7,&u))return G3_LOAD_FORMAT;
    r.flags=unsigned(u);
    if(!parse_i64(tok[6],len[6],0,35999,&x))return G3_LOAD_FORMAT;
    r.heading_cdeg=int32_t(x);
    if(!parse_i64(tok[7],len[7],0,18000,&x))return G3_LOAD_FORMAT;
    r.unc_cdeg=int32_t(x);
    if(!parse_i64(tok[8],len[8],-900000000LL,900000000LL,&x))return G3_LOAD_FORMAT;
    r.lat_e7=int32_t(x);
    if(!parse_i64(tok[9],len[9],-1800000000LL,1800000000LL,&x))return G3_LOAD_FORMAT;
    r.lon_e7=int32_t(x);
    if(!parse_u64(tok[10],len[10],0xFFFFFFFFu,&u))return G3_LOAD_FORMAT;
    r.pos_err_dm=uint32_t(u);
    if(!parse_u64(tok[11],len[11],1ULL<<40,&r.utc_s) || !parse_u64(tok[12],len[12],1ULL<<40,&r.rt_s) ||
       !parse_u64(tok[13],len[13],1ULL<<53,&r.mono_ms))return G3_LOAD_FORMAT;
    if(len[14]==4 && !memcmp(tok[14],"none",4))strcpy(r.chain,"none");
    else if(len[14]==6 && !memcmp(tok[14],"unread",6))strcpy(r.chain,"unread");
    else if(hex8(tok[14],len[14])) { memcpy(r.chain,tok[14],8);r.chain[8]=0; }
    else return G3_LOAD_FORMAT;
    if(out)*out=r;
    return G3_LOAD_OK;
}

// ---- file I/O (store thread / startup only) ----------------------------------
enum G3IoStage { G3_IO_OK=0, G3_IO_PATH, G3_IO_OPEN, G3_IO_WRITE, G3_IO_FSYNC, G3_IO_CLOSE,
                 G3_IO_RENAME, G3_IO_CANCELLED, G3_IO_STALE, G3_IO_STOPPED };
inline const char* g3_io_stage_name(int s) {
    static const char* const names[]={"ok","path","open","write","fsync","close","rename","cancelled",
                                      "stale","stopped"};
    return s>=0 && unsigned(s)<sizeof names/sizeof names[0]?names[s]:"unknown";
}
// I/O steps that are gated (review v2: re-check before every I/O step).
enum G3Step { G3_STEP_NONE=0, G3_STEP_OPEN, G3_STEP_FSYNC, G3_STEP_RENAME };
inline const char* g3_step_name(int s) {
    static const char* const names[]={"none","open","fsync","rename"};
    return s>=0 && unsigned(s)<sizeof names/sizeof names[0]?names[s]:"unknown";
}
// stage/error: outcome; step: the gate that refused (cancelled/stopped);
// start_ns/end_ns: CLOCK_MONOTONIC around the whole attempt.
struct G3IoResult { int stage; int error; uint64_t latency_ns; int step; uint64_t start_ns, end_ns; };
inline uint64_t g3_clock_ns(clockid_t id) {
    struct timespec t;
    if(clock_gettime(id,&t))return 0;
    return uint64_t(t.tv_sec)*1000000000ULL+uint64_t(t.tv_nsec);
}
inline uint64_t g3_realtime_s() { return g3_clock_ns(CLOCK_REALTIME)/1000000000ULL; }
// Called before each gated step: G3_IO_OK continues, G3_IO_CANCELLED or
// G3_IO_STOPPED abandons the write (the temporary file is removed and the
// previous record is never replaced).
typedef int (*G3StepGate)(void* context,int step);
// Tests only: latency injection inside the fsync step (a blocked fsync).
typedef void (*G3FsyncHook)();
// write temp (O_EXCL, never followed) + ONE fsync of the file + close +
// rename. No directory fsync (review v2): this is record-only data, and a
// rename lost to a power cut only costs the carried heading of the next boot
// (the previous record, or none, is restored instead). The repository's relfs
// notes (STARTUP_RECOVERY / EMPTY_CAPTURE 2026-10-02) only name the mount and
// say nothing about rename durability. Runs only on the dedicated store thread.
// name/temp_name: the published file and its temporary name in dir.
inline G3IoResult g3_write_atomic_at(const char* dir,const char* name,const char* temp_name,const char* data,
                                     size_t n,G3StepGate gate=0,void* context=0,G3FsyncHook hook=0,bool sync=true) {
    G3IoResult r={G3_IO_OK,0,0,G3_STEP_NONE,0,0};
    r.start_ns=g3_clock_ns(CLOCK_MONOTONIC);
    char temp[512],path[512];
    const int a=dir && temp_name?snprintf(temp,sizeof temp,"%s/%s",dir,temp_name):-1;
    const int b=dir && name?snprintf(path,sizeof path,"%s/%s",dir,name):-1;
    int fd=-1;bool made=false;
    if(a<=0 || size_t(a)>=sizeof temp || b<=0 || size_t(b)>=sizeof path || !data || !n || n>G3_RECORD_MAX) {
        r.stage=G3_IO_PATH;r.error=EINVAL;
    }
    int g=G3_IO_OK;
    if(!r.stage && gate && (g=gate(context,G3_STEP_OPEN))!=G3_IO_OK) { r.stage=g;r.step=G3_STEP_OPEN; }
    if(!r.stage) {
        // A cut write may leave the temporary name; unlink never follows it.
        unlink(temp);
        fd=open(temp,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
        if(fd<0) { r.stage=G3_IO_OPEN;r.error=errno; } else made=true;
    }
    if(!r.stage) {
        size_t done=0;
        while(done<n) {
            const ssize_t w=write(fd,data+done,n-done);
            if(w<0 && errno==EINTR)continue;
            if(w<=0) { r.stage=G3_IO_WRITE;r.error=w<0?errno:EIO;break; }
            done+=size_t(w);
        }
    }
    if(!r.stage && gate && (g=gate(context,G3_STEP_FSYNC))!=G3_IO_OK) { r.stage=g;r.step=G3_STEP_FSYNC; }
    // sync=false (a stop of 15-60 s): the fsync step is skipped; the gate
    // before it still runs.
    if(!r.stage && sync) {
        if(hook)hook();
        if(fsync(fd)) { r.stage=G3_IO_FSYNC;r.error=errno; }
    }
    if(fd>=0 && close(fd) && !r.stage) { r.stage=G3_IO_CLOSE;r.error=errno; }
    if(!r.stage && gate && (g=gate(context,G3_STEP_RENAME))!=G3_IO_OK) { r.stage=g;r.step=G3_STEP_RENAME; }
    if(!r.stage && rename(temp,path)) { r.stage=G3_IO_RENAME;r.error=errno; }
    if(r.stage && made)unlink(temp);
    r.end_ns=g3_clock_ns(CLOCK_MONOTONIC);
    r.latency_ns=r.end_ns>r.start_ns?r.end_ns-r.start_ns:0;
    return r;
}
inline G3IoResult g3_write_atomic(const char* dir,const char* data,size_t n,G3StepGate gate=0,void* context=0,
                                  G3FsyncHook hook=0,bool sync=true) {
    return g3_write_atomic_at(dir,G3_STORE_NAME,G3_STORE_TEMP,data,n,gate,context,hook,sync);
}
inline G3Load g3_load(const char* dir,G3Record* out) {
    char path[512];
    const int p=dir?snprintf(path,sizeof path,"%s/%s",dir,G3_STORE_NAME):-1;
    if(p<=0 || size_t(p)>=sizeof path)return G3_LOAD_UNREADABLE;
    const int fd=open(path,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if(fd<0)return errno==ENOENT?G3_LOAD_MISSING:errno==ELOOP?G3_LOAD_NOT_REGULAR:G3_LOAD_UNREADABLE;
    struct stat st;
    if(fstat(fd,&st)) { close(fd);return G3_LOAD_UNREADABLE; }
    if(!S_ISREG(st.st_mode)) { close(fd);return G3_LOAD_NOT_REGULAR; }
    if(st.st_size<0 || uint64_t(st.st_size)>G3_RECORD_MAX) { close(fd);return G3_LOAD_TOO_LARGE; }
    char buf[G3_RECORD_MAX+1];
    size_t n=0;
    for(;;) {
        const ssize_t got=read(fd,buf+n,sizeof buf-n);
        if(got<0 && errno==EINTR)continue;
        if(got<0) { close(fd);return G3_LOAD_UNREADABLE; }
        if(!got)break;
        n+=size_t(got);
        if(n==sizeof buf) { close(fd);return G3_LOAD_TOO_LARGE; }
    }
    close(fd);
    return g3_decode(buf,n,out);
}
// The guard's decline record names the newest Linux boot in which it refused
// to start the product (it is rewritten by every decline, never by a selected
// boot). Token: the first 8 hex digits of its boot_id, "none" when there is no
// such record, "unread" when it cannot be read or parsed.
inline void g3_chain_token(const char* root,char out[9]) {
    strcpy(out,"unread");
    char path[512];
    const int p=root?snprintf(path,sizeof path,"%s/%s",root,G3_GUARD_DECISION):-1;
    if(p<=0 || size_t(p)>=sizeof path)return;
    const int fd=open(path,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if(fd<0) { if(errno==ENOENT)strcpy(out,"none");return; }
    struct stat st;char buf[257];ssize_t n=-1;
    if(!fstat(fd,&st) && S_ISREG(st.st_mode) && st.st_size>=0 && st.st_size<=256) {
        do n=read(fd,buf,sizeof buf-1); while(n<0 && errno==EINTR);
    }
    close(fd);
    static const char head[]="mx5dr-last-decision-v1\nboot_id=";
    const size_t h=sizeof head-1;
    if(n<0 || size_t(n)<h+8 || memcmp(buf,head,h))return;
    char token[9];
    for(size_t i=0;i<8;++i) {
        char c=buf[h+i];
        if(c>='A' && c<='F')c=char(c-'A'+'a');
        if(!((c>='0' && c<='9') || (c>='a' && c<='f')))return;
        token[i]=c;
    }
    token[8]=0;strcpy(out,token);
}
// Owner off switch: present (or not provably absent) = off.
inline bool g3_switch_on(const char* root) {
    char path[512];
    const int p=root?snprintf(path,sizeof path,"%s/%s",root,G3_OFF_NAME):-1;
    if(p<=0 || size_t(p)>=sizeof path)return false;
    struct stat st;
    if(!lstat(path,&st))return false;
    return errno==ENOENT;
}
// Garage carry switch: <root>/g3-garage-off present (or not provably absent) = off.
inline bool g3_garage_on(const char* root) {
    char path[512];
    const int p=root?snprintf(path,sizeof path,"%s/%s",root,G3_GARAGE_OFF_NAME):-1;
    if(p<=0 || size_t(p)>=sizeof path)return false;
    struct stat st;
    if(!lstat(path,&st))return false;
    return errno==ENOENT;
}
// Previous-product-boot token (review P1): <dir>/g3-last-start holds one
// line "mx5g3start 1 <boot_id>\n". g3_last_start_read: prev = that boot id
// ("none": missing, "unread": unreadable or malformed).
inline void g3_last_start_read(const char* dir,char prev[37]) {
    strcpy(prev,"unread");
    char path[512];
    const int a=dir?snprintf(path,sizeof path,"%s/%s",dir,G3_LAST_START_NAME):-1;
    if(a<=0 || size_t(a)>=sizeof path)return;
    static const char head[]="mx5g3start 1 ";
    const size_t h=sizeof head-1;
    const int fd=open(path,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if(fd<0) { if(errno==ENOENT)strcpy(prev,"none");return; }
    struct stat st;char buf[80];ssize_t n=-1;
    if(!fstat(fd,&st) && S_ISREG(st.st_mode) && st.st_size>=0 && st.st_size<=64) {
        do n=read(fd,buf,sizeof buf); while(n<0 && errno==EINTR);
    }
    close(fd);
    if(n==ssize_t(h+37) && !memcmp(buf,head,h) && buf[h+36]=='\n' && g3_detail::boot_token(buf+h,36)) {
        memcpy(prev,buf+h,36);prev[36]=0;
    }
}
// The token line for boot_id (0: not a boot id, e.g. "unknown").
inline size_t g3_last_start_line(const char* boot_id,char* out,size_t cap) {
    if(!boot_id || strlen(boot_id)!=36 || !g3_detail::boot_token(boot_id,36) || cap<64)return 0;
    const int m=snprintf(out,cap,"mx5g3start 1 %s\n",boot_id);
    return m==50?size_t(m):0;
}
// Writes this boot's token (temporary file O_EXCL, write, close, rename; NO
// fsync: a lost write leaves the older id, so the next boot refuses
// chain_depart for this boot's record: the safe side, and an older record
// keeps its own older writer, see the lazy-token residual). A boot_id that is
// not a boot id writes nothing. temp_name: G3_LAST_START_TEMP (start path,
// store thread) or G3_LAST_START_SYNC_TEMP (terminal path).
inline G3IoResult g3_last_start_write_io(const char* dir,const char* boot_id,
                                         const char* temp_name=G3_LAST_START_TEMP) {
    char line[64];
    const size_t m=g3_last_start_line(boot_id,line,sizeof line);
    if(!m) { G3IoResult r={G3_IO_PATH,EINVAL,0,G3_STEP_NONE,0,0};return r; }
    return g3_write_atomic_at(dir,G3_LAST_START_NAME,temp_name,line,m,0,0,0,false);
}
inline bool g3_last_start_write(const char* dir,const char* boot_id,const char* temp_name=G3_LAST_START_TEMP) {
    return g3_last_start_write_io(dir,boot_id,temp_name).stage==G3_IO_OK;
}
// The eager swap (beta.16/HEAD behaviour, used with either marker, without
// the BETA core and for a second worker start of one boot): reads prev, then
// writes this boot's id. Startup slow path only. Returns true when the new
// token was renamed into place.
inline bool g3_last_start_swap(const char* dir,const char* boot_id,char prev[37]) {
    g3_last_start_read(dir,prev);
    return g3_last_start_write(dir,boot_id);
}
// Worker-boot file (lazy token only, startup slow path): FIRST: the file
// named another boot (or none) and now names this one; RESTART: it already
// named this boot (a second worker start in this boot); UNMARKED: it could
// not be written (or the boot id is unknown). RESTART and UNMARKED swap the
// token at once (the conservative side).
enum G3WorkerStart { G3_WORKER_FIRST=0, G3_WORKER_RESTART, G3_WORKER_UNMARKED };
inline const char* g3_worker_start_name(G3WorkerStart w) {
    return w==G3_WORKER_FIRST?"lazy":w==G3_WORKER_RESTART?"eager_restart":"eager_unmarked";
}
inline G3WorkerStart g3_worker_boot_mark(const char* dir,const char* boot_id) {
    if(!boot_id || strlen(boot_id)!=36 || !g3_detail::boot_token(boot_id,36))return G3_WORKER_UNMARKED;
    char path[512];
    const int a=dir?snprintf(path,sizeof path,"%s/%s",dir,G3_WORKER_BOOT_NAME):-1;
    if(a<=0 || size_t(a)>=sizeof path)return G3_WORKER_UNMARKED;
    static const char head[]="mx5g3worker 1 ";
    const size_t h=sizeof head-1;
    const int fd=open(path,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if(fd>=0) {
        struct stat st;char buf[80];ssize_t n=-1;
        if(!fstat(fd,&st) && S_ISREG(st.st_mode) && st.st_size>=0 && st.st_size<=64) {
            do n=read(fd,buf,sizeof buf); while(n<0 && errno==EINTR);
        }
        close(fd);
        if(n==ssize_t(h+37) && !memcmp(buf,head,h) && buf[h+36]=='\n' && !memcmp(buf+h,boot_id,36))
            return G3_WORKER_RESTART;
    }
    char line[64];
    const int m=snprintf(line,sizeof line,"%s%s\n",head,boot_id);
    if(m!=int(h+37))return G3_WORKER_UNMARKED;
    return g3_write_atomic_at(dir,G3_WORKER_BOOT_NAME,G3_WORKER_BOOT_TEMP,line,size_t(m),0,0,0,false).stage==G3_IO_OK?
        G3_WORKER_FIRST:G3_WORKER_UNMARKED;
}
// Lazy-token swap triggers from the raw wheel stream (see G3_TOKEN_*). Fed
// by the worker with every received WHEELS event (admitted to the MODEL or
// excluded before the session/bus admission), polled on the G3 tick. why():
// the first trigger, sticky ("moved", "blind_gap", "blind_silent",
// "wheel_bad"); 0 while none. Observation only, no I/O.
struct G3TokenWatch {
    uint64_t start_ns, last_ns;
    double kmh_per_count, zero_kmh, speed_mps, distance_m;
    bool active, have;
    const char* why;
    G3TokenWatch() { reset(); }
    void reset() {
        start_ns=last_ns=0;kmh_per_count=zero_kmh=speed_mps=distance_m=0;active=have=false;why=0;
    }
    void start(uint64_t now,double kmh_count,double zero) {
        reset();active=true;start_ns=now;kmh_per_count=kmh_count;zero_kmh=zero;
    }
    // received_ns: the worker's receipt time; raw: the four wheel counts.
    void wheel(uint64_t received_ns,const uint16_t raw[4]) {
        if(!active)return;
        double kmh=0;bool bad=!std::isfinite(kmh_per_count) || !std::isfinite(zero_kmh);
        for(unsigned i=0;i<4 && !bad;++i) {
            const double w=raw[i]*kmh_per_count+zero_kmh;
            if(raw[i]>40000 || !std::isfinite(w) || w<0)bad=true;
            kmh+=w*0.25;
        }
        if(bad) { if(!why)why="wheel_bad";return; }
        if(have && received_ns>last_ns) {
            const uint64_t gap=received_ns-last_ns;
            if(gap>navigation::G3_BOOT_GAP_MAX_NS && !why)why="blind_gap";
            distance_m+=speed_mps*double(gap<navigation::G3_SPEED_GAP_MAX_NS?gap:navigation::G3_SPEED_GAP_MAX_NS)/1e9;
        }
        if(!have || received_ns>=last_ns) { last_ns=received_ns;speed_mps=kmh/3.6;have=true; }
        if(distance_m>=G3_TOKEN_MOVED_M && !why)why="moved";
    }
    const char* check(uint64_t now) {
        if(!active)return 0;
        const uint64_t last=have?last_ns:start_ns;
        if(!why && now>last && now-last>G3_TOKEN_SILENT_NS)why="blind_silent";
        return why;
    }
};

// ---- store I/O idle rule and the dedicated store thread -----------------------
// Why the parked write must wait now (0: it may be handed to the store thread).
// replacing: BETA is in GPS_LOST, ENGAGED, NO_FIX or SPEED_ENGAGED (its
// provenance depends on journal_current); draining: a persistent RAW window
// drain or queued bulk/prompt journal rows; lag_ns: the journal writer's
// oldest unwritten/unflushed row.
inline const char* g3_io_block_reason(bool store_ready,bool replacing,bool journal_current,bool writer_ok,
                                      bool draining,uint64_t lag_ns) {
    if(!store_ready)return "no_store_thread";
    if(replacing)return "beta_replacing";
    if(!journal_current)return "journal_lagging";
    if(!writer_ok)return "no_writer";
    if(draining)return "journal_draining";
    if(lag_ns>G3_WRITER_IDLE_LAG_NS)return "writer_busy";
    return 0;
}
// One-slot hand-off between the worker and the dedicated store thread. It
// shares nothing with the journal writer (thread, ring or mutex). line/len/
// seq/epoch/gen/posted_ns are written by the worker only while pending is 0
// and read by the store thread only while it is 1 (release/acquire); the
// outcome is stored before done_seq/done_epoch, then pending returns to 0.
// Cancellation (review v2): the worker bumps cancel_gen whenever the idle
// rule fails, a wheel moves or the wheel stream is stale while a write is in
// flight; stop is set at capture stop and worker exit. The store thread
// re-checks both before opening the temporary file, before the fsync and
// before the rename, and drops a request picked up later than stale_ns.
// epoch: the worker generation, so an old worker's result is never consumed
// by a new tracker (ABA). Static storage, process lifetime, never joined.
static const uint64_t G3_STORE_STALE_NS=500000000ULL;
struct G3Mailbox {
    std::atomic<unsigned> pending, stop, started;
    std::atomic<uint32_t> cancel_gen;
    std::atomic<uint32_t> done_seq, done_epoch;
    std::atomic<int> done_stage, done_errno, done_step;
    std::atomic<uint64_t> done_ns, done_start, done_end, runs;
    char dir[480];
    char line[G3_RECORD_MAX];
    size_t len;
    uint32_t seq, epoch, gen;
    unsigned sync;           // 1: the fsync step runs (a stop of >= 60 s)
    unsigned target;         // G3_TARGET_STORE (the record) or G3_TARGET_TOKEN (g3-last-start)
    uint64_t posted_ns;
    G3FsyncHook fsync_hook;  // tests only
    uint64_t poll_ns, stale_ns;
};
// Before the thread starts (single caller).
inline bool g3_mailbox_init(G3Mailbox& m,const char* dir,G3FsyncHook hook) {
    const int n=dir?snprintf(m.dir,sizeof m.dir,"%s",dir):-1;
    if(n<=0 || size_t(n)>=sizeof m.dir)return false;
    m.fsync_hook=hook;m.poll_ns=G3_STORE_POLL_NS;m.stale_ns=G3_STORE_STALE_NS;m.len=0;m.seq=0;m.epoch=0;
    m.target=G3_TARGET_STORE;
    return true;
}
inline bool g3_mailbox_idle(const G3Mailbox& m) { return !m.pending.load(std::memory_order_acquire); }
inline bool g3_mailbox_post(G3Mailbox& m,const char* s,size_t n,uint32_t seq,uint32_t epoch,uint64_t now,
                            bool sync=true,unsigned target=G3_TARGET_STORE) {
    if(!g3_mailbox_idle(m) || m.stop.load(std::memory_order_acquire) || !n || n>sizeof m.line || !seq ||
       (target!=G3_TARGET_STORE && target!=G3_TARGET_TOKEN))return false;
    memcpy(m.line,s,n);m.len=n;m.seq=seq;m.epoch=epoch;m.posted_ns=now;m.sync=sync?1:0;m.target=target;
    m.gen=m.cancel_gen.load(std::memory_order_acquire);
    m.pending.store(1,std::memory_order_release);
    return true;
}
inline bool g3_mailbox_result(const G3Mailbox& m,uint32_t seq,uint32_t epoch,G3IoResult* r) {
    if(m.done_seq.load(std::memory_order_acquire)!=seq || m.done_epoch.load(std::memory_order_acquire)!=epoch)
        return false;
    r->stage=m.done_stage.load(std::memory_order_relaxed);
    r->error=m.done_errno.load(std::memory_order_relaxed);
    r->step=m.done_step.load(std::memory_order_relaxed);
    r->latency_ns=m.done_ns.load(std::memory_order_relaxed);
    r->start_ns=m.done_start.load(std::memory_order_relaxed);
    r->end_ns=m.done_end.load(std::memory_order_relaxed);
    return true;
}
// The gate the store thread applies before every I/O step.
inline int g3_mailbox_gate(void* context,int) {
    const G3Mailbox& m=*static_cast<const G3Mailbox*>(context);
    if(m.stop.load(std::memory_order_acquire))return G3_IO_STOPPED;
    if(m.cancel_gen.load(std::memory_order_acquire)!=m.gen)return G3_IO_CANCELLED;
    return G3_IO_OK;
}
// Store thread: one pending request, if any.
inline bool g3_mailbox_run_once(G3Mailbox& m,uint64_t now) {
    if(!m.pending.load(std::memory_order_acquire))return false;
    G3IoResult r={G3_IO_STALE,0,0,G3_STEP_NONE,now,now};
    if(!(now>=m.posted_ns && now-m.posted_ns>m.stale_ns))
        r=m.target==G3_TARGET_TOKEN?
            g3_write_atomic_at(m.dir,G3_LAST_START_NAME,G3_LAST_START_TEMP,m.line,m.len,g3_mailbox_gate,&m,0,false):
            g3_write_atomic(m.dir,m.line,m.len,g3_mailbox_gate,&m,m.fsync_hook,m.sync!=0);
    m.done_stage.store(r.stage,std::memory_order_relaxed);
    m.done_errno.store(r.error,std::memory_order_relaxed);
    m.done_step.store(r.step,std::memory_order_relaxed);
    m.done_ns.store(r.latency_ns,std::memory_order_relaxed);
    m.done_start.store(r.start_ns,std::memory_order_relaxed);
    m.done_end.store(r.end_ns,std::memory_order_relaxed);
    m.runs.fetch_add(1,std::memory_order_relaxed);
    m.done_epoch.store(m.epoch,std::memory_order_release);
    m.done_seq.store(m.seq,std::memory_order_release);
    m.pending.store(0,std::memory_order_release);
    return true;
}
inline void* g3_store_main(void* argument) {
    G3Mailbox& m=*static_cast<G3Mailbox*>(argument);
    while(!m.stop.load(std::memory_order_acquire)) {
        if(g3_mailbox_run_once(m,g3_clock_ns(CLOCK_MONOTONIC)))continue;
        const uint64_t p=m.poll_ns?m.poll_ns:G3_STORE_POLL_NS;
        struct timespec t={time_t(p/1000000000ULL),long(p%1000000000ULL)};
        nanosleep(&t,0);
    }
    return 0;
}

// ---- tracker inputs ------------------------------------------------------------
static const double G3_PI=3.14159265358979323846;
inline double g3_wrap360(double d) { d=fmod(d,360.0);if(d<0)d+=360.0;return d>=360.0?0.0:d; }
inline double g3_wrap180(double d) { d=fmod(d+180.0,360.0);if(d<0)d+=360.0;return d-180.0; }
struct G3Sample {
    uint64_t now, rt_s;
    bool heading_valid;          // BETA core seeded with a finite body heading
    double heading_deg, unc_deg; // body heading (north 0, clockwise) and honest uncertainty
    bool position_valid;
    double lat, lon, pos_err_m;
    navigation::G3Motion motion;
    bool wheels_fresh;           // the newest drained wheel event is recent enough (no stale stream)
    bool wheels_moving;          // the newest drained wheel event has a wheel above exactly zero
    int reverse;                 // reverse latch now: -1 unknown, 0, 1
    double yaw_error_rad_s, rotation_per_rad; // heading budget growth (BETA rule 5)
    bool beta_replacing;         // BETA replaces or overlays sends now (garage carry: no fsync write)
    uint64_t writer_lag_ns;      // journal writer's oldest unwritten row (garage carry: the mark while replacing)
    const char* token_why;       // lazy token: G3TokenWatch::check() (0: no raw-stream trigger yet)
};
inline G3Sample g3_sample(const navigation::Pipeline& nav,uint64_t now,uint64_t rt_s) {
    G3Sample s;
    memset(&s,0,sizeof s);
    s.now=now;s.rt_s=rt_s;s.motion=nav.g3_motion();
    const uint64_t last=s.motion.last_speed_ns;
    s.wheels_fresh=last && now>=last && now-last<=navigation::G3_WHEEL_GAP_NS+nav.reorder_ns();
    s.wheels_moving=s.motion.last_moving_ns && s.motion.last_moving_ns==last;
    s.reverse=nav.reverse_latched()?nav.latch_value():-1;
    const BetaProfile& p=nav.beta_profile_in_use();
    s.yaw_error_rad_s=p.yaw_error_rad_s;s.rotation_per_rad=p.rotation_budget_per_rad;
    if(nav.beta_seeded()) {
        const mx5_dr_snapshot& e=nav.beta_estimate();
        const double unc=nav.beta_heading_budget_rad();
        if(e.reason==MX5_DR_OK && std::isfinite(e.body_heading_rad) && std::isfinite(unc) && unc>=0) {
            s.heading_valid=true;s.heading_deg=g3_wrap360(e.body_heading_rad*180.0/G3_PI);
            s.unc_deg=unc*180.0/G3_PI;
        }
        const double err=e.error_budget_m+nav.beta_rotation_budget_m();
        if(e.reason==MX5_DR_OK && std::isfinite(e.latitude_deg) && std::isfinite(e.longitude_deg) &&
           std::isfinite(err) && err>=0 && fabs(e.latitude_deg)<=90 && fabs(e.longitude_deg)<=180) {
            s.position_valid=true;s.lat=e.latitude_deg;s.lon=e.longitude_deg;s.pos_err_m=err;
        }
    }
    return s;
}
// The first policy-M-accepted GPS course: an ACCEPTED beta_anchor record whose
// heading came from the GPS course (1 seed, 5 resync, 9 legacy gate).
struct G3Course { uint64_t mono_ns; double course_deg, kmh; unsigned source; };
inline bool g3_find_course(const navigation::Pipeline& nav,uint64_t* seen,G3Course* out) {
    const uint64_t latest=nav.beta_anchor_sequence();
    if(latest<*seen)*seen=0;
    for(uint64_t seq=*seen+1;seq<=latest;++seq) {
        navigation::BetaAnchorRecord r;
        if(!nav.beta_anchor_record(seq,&r) || r.gate!=navigation::BETA_GATE_ACCEPTED ||
           !std::isfinite(r.heading_deg))continue;
        const unsigned code=navigation::beta_heading_source_code(r.heading_source);
        if(code!=1 && code!=5 && code!=9)continue;
        *seen=seq;
        out->mono_ns=r.mono_ns;out->course_deg=g3_wrap360(r.heading_deg);
        out->kmh=std::isfinite(r.kmh)?r.kmh:-1;out->source=code;
        return true;
    }
    *seen=latest;
    return false;
}

// ---- tracker -------------------------------------------------------------------
// UNKNOWN: nothing carried (reason). CARRIED: the stored heading is valid and
// continuity holds (boot chain ok and age known). CARRIED_UNVERIFIED: valid
// but continuity or age cannot be established (yet). CONFIRMED / REJECTED:
// the first M-accepted GPS course agreed / disagreed, or a later check
// (first-fix position or GPS age, uncertainty growth) rejected it.
enum G3State { G3_OFF=0, G3_UNKNOWN, G3_CARRIED, G3_CARRIED_UNVERIFIED, G3_CONFIRMED, G3_REJECTED };
inline const char* g3_state_name(G3State s) {
    static const char* const names[]={"OFF","UNKNOWN","CARRIED","CARRIED_UNVERIFIED","CONFIRMED","REJECTED"};
    return unsigned(s)<sizeof names/sizeof names[0]?names[s]:"INVALID";
}

class G3Tracker {
public:
    G3Tracker() { clear(); }
    G3State state() const { return state_; }
    const char* reason() const { return reason_; }
    bool active() const { return state_!=G3_OFF; }
    bool wants_course() const { return active() && !course_done_; }
    bool store_on() const { return store_on_; }
    uint64_t rows_dropped() const { return dropped_; }
    char disk_state() const { return disk_; }
    // Read-only views for the G3 bearing send (g3_send.h). Valid after
    // tick(): the propagated values are those of the latest MODEL tick.
    bool have_carried() const { return have_carried_; }
    double carried_deg() const { return carried_deg_; }
    double carried_unc() const { return carried_unc_; }
    double propagated_unc() const { return prop_unc_; }
    double carried_travel(const G3Sample& s) const { return travel(s); }
    uint64_t session_ns() const { return session_ns_; }
    bool age_known() const { return age_known_; }
    uint64_t age_s() const { return age_s_; }
    // Garage carry (G3_GARAGE_*): the switch, and whether the carried
    // heading came from a departure-marked record (its own limit).
    bool garage() const { return garage_; }
    bool garage_carry() const { return garage_carry_; }
    double unc_max() const { return garage_carry_?G3_GARAGE_UNC_MAX_DEG:G3_UNC_MAX_DEG; }
    // Lazy previous-product-boot token (G3_TOKEN_*): this boot rewrites it
    // later (lazy), it still has to (wanted: no write landed and no terminal
    // attempt yet), and the first trigger.
    bool token_lazy() const { return token_lazy_; }
    bool token_done() const { return token_done_; }
    bool token_wanted() const { return token_lazy_ && !token_done_ && !token_closed_; }
    const char* token_why() const { return token_why_; }
    const char* boot_id() const { return boot_id_; }
    // Review P3: the token is due (a trigger exists) but the store thread
    // cannot write it any more: G3_TOKEN_FAILURES_MAX failed token writes
    // ("token_failed") or the store went off for record failures
    // ("store_off"). The worker then makes the one synchronous attempt
    // (token_sync). 0 while a token write is in flight, none is due, or
    // BETA replaces (no worker file I/O on the G1 path: it stays due and
    // runs once BETA stops replacing, or at a terminal point).
    const char* token_sync_due(bool beta_replacing) const {
        if(beta_replacing || !token_wanted() || !token_why_ || (in_flight_ && pending_token_))return 0;
        return token_failures_>=G3_TOKEN_FAILURES_MAX?"token_failed":!store_on_?"store_off":0;
    }

    // Worker start. stored is read only when load == G3_LOAD_OK. prev_boot:
    // the previous product boot (g3-last-start as read at this start; garage
    // carry only). swap (garage carry only, journaled as "swap"): 0 = the
    // caller swapped the token at start (beta.16/HEAD); "lazy" = this
    // tracker requests the token write itself (G3_TOKEN_*); any other value
    // (g3_worker_start_name) = the caller swapped at start for that reason.
    void start(uint64_t now,bool enabled,const char* off_reason,const char* boot_id,const char* chain_now,
               uint64_t rt_now_s,G3Load load,const G3Record& stored,bool garage=false,const char* prev_boot=0,
               const char* swap=0) {
        clear();
        start_ns_=now;garage_=enabled && garage;
        if(!enabled) { state_=G3_OFF;reason_=off_reason?off_reason:"off";return; }
        store_on_=true;
        copy(boot_id_,sizeof boot_id_,boot_id?boot_id:"unknown");
        copy(chain_now_,sizeof chain_now_,chain_now?chain_now:"unread");
        copy(prev_boot_,sizeof prev_boot_,prev_boot?prev_boot:"unread");
        token_mode_=garage_?swap:0;
        token_lazy_=garage_ && swap && !strcmp(swap,"lazy");
        // Nothing to write when the token already names this boot or the
        // boot id is unknown (no record of an unknown boot is ever carried).
        { char line[64];
          token_done_=!token_lazy_ || !g3_last_start_line(boot_id_,line,sizeof line) || !strcmp(prev_boot_,boot_id_); }
        // The g3-garage-off marker restores beta.16 exactly: beta.16 decodes
        // a flag-4 record as a format error (flags > 3), so no build applies
        // the 30 deg rules to a record grown with the garage terms.
        if(!garage_ && load==G3_LOAD_OK && (stored.flags&G3_FLAG_DEPART))load=G3_LOAD_FORMAT;
        load_=load;have_stored_=load==G3_LOAD_OK;
        if(have_stored_) {
            stored_=stored;next_seq_=stored.seq+1;if(!next_seq_)next_seq_=1;disk_=stored.state;
            disk_flags_=stored.state=='S'?stored.flags:0;disk_cdeg_=stored.heading_cdeg;
        }
        state_=G3_UNKNOWN;
        if(!have_stored_) { reason_=g3_load_name(load);return; }
        if(stored.state!='S') { reason_="moving_at_end";return; }
        if(!(stored.flags&G3_FLAG_HEADING)) { reason_="no_heading";return; }
        const bool depart=garage_ && (stored.flags&G3_FLAG_DEPART);
        if(stored.unc_cdeg>(depart?G3_GARAGE_UNC_MAX_DEG:G3_UNC_MAX_DEG)*100) { reason_="unc";return; }
        if(strcmp(boot_id_,"unknown") && !strcmp(stored.boot_id,boot_id_))chain_="same_boot";
        else if(!strcmp(stored.chain,"unread") || !strcmp(chain_now_,"unread"))chain_="unknown";
        else if(strcmp(stored.chain,chain_now_))chain_="broken";
        else chain_="ok";
        if(!strcmp(chain_,"broken")) { reason_="chain_broken";return; }
        if(stored.rt_s>=G3_PLAUSIBLE_UTC_S && rt_now_s>=G3_PLAUSIBLE_UTC_S && rt_now_s>=stored.rt_s) {
            age_known_=true;age_s_=rt_now_s-stored.rt_s;age_src_="rt";
            if(age_s_>G3_AGE_MAX_S) { reason_="age";return; }
        }
        have_carried_=true;garage_carry_=depart;
        carried_deg_=stored.heading_cdeg/100.0;carried_unc_=stored.unc_cdeg/100.0;
        // Garage carry (review P1): a departure-marked record is carried only
        // when its boot is the previous product boot; otherwise a product boot
        // in between may have moved the car without marking it.
        const bool prev_ok=strcmp(stored.boot_id,"unknown") && strlen(prev_boot_)==36 &&
            !strcmp(stored.boot_id,prev_boot_);
        if(depart && !strcmp(chain_,"ok") && !prev_ok) { state_=G3_CARRIED_UNVERIFIED;reason_="not_prev_boot"; }
        else if(!strcmp(chain_,"ok") && age_known_) { state_=G3_CARRIED;reason_="chain_ok"; }
        // Garage carry: the departure-marked S record says the car stood
        // since the write, as far as the previous product boot observed it;
        // no wall clock is needed (a known age still counts).
        else if(depart && !strcmp(chain_,"ok")) { state_=G3_CARRIED;reason_="chain_depart"; }
        else {
            state_=G3_CARRIED_UNVERIFIED;
            reason_=!strcmp(chain_,"same_boot")?"same_boot":strcmp(chain_,"ok")?"chain_unknown":"age_unknown";
        }
    }
    // After shadow_boot: one g3_store row with the restore decision.
    template<class J> void journal_start(J& j,uint64_t now) {
        const char* event=state_==G3_OFF?"off":!have_stored_ && load_==G3_LOAD_MISSING?"missing":
            state_==G3_UNKNOWN?"rejected":"loaded";
        char st[48],age[32];
        if(have_stored_)snprintf(st,sizeof st,",\"st\":\"%c\",\"hv\":%u,\"h\":%ld,\"unc\":%ld",stored_.state,
                                 stored_.flags&G3_FLAG_HEADING,lround(stored_.heading_cdeg/10.0),
                                 lround(stored_.unc_cdeg/10.0));
        else st[0]=0;
        if(age_known_)snprintf(age,sizeof age,"%llu",(unsigned long long)age_s_);else strcpy(age,"null");
        char boot[9],prev[64];
        copy(boot,sizeof boot,have_stored_?stored_.boot_id:"none");
        // Garage carry: the previous product boot (8 hex, "none" or "unread")
        // and, when the caller says so, how the token is maintained ("swap").
        if(garage_) {
            char p[9],w[24];copy(p,sizeof p,prev_boot_);copy(w,sizeof w,token_mode_?token_mode_:"");
            if(token_mode_)snprintf(prev,sizeof prev,",\"prev\":\"%s\",\"swap\":\"%s\"",p,w);
            else snprintf(prev,sizeof prev,",\"prev\":\"%s\"",p);
        }
        else prev[0]=0;
        row(j,",\"mono_ns\":%llu,\"event\":\"%s\",\"reason\":\"%s\",\"g3\":\"%s\"%s,\"age_s\":%s,"
              "\"age_src\":\"%s\",\"boot\":\"%s\",\"chain\":\"%s\",\"seq\":%lu%s}",
            "g3_store",(unsigned long long)now,event,reason_,g3_state_name(state_),st,age,age_src_,boot,chain_,
            (unsigned long)(have_stored_?stored_.seq:0),prev);
    }
    // shadow_boot fields (",\"g3\":{...}").
    size_t boot_fields(char* out,size_t cap) const {
        const int n=snprintf(out,cap,",\"g3\":{\"state\":\"%s\",\"reason\":\"%s\"%s}",g3_state_name(state_),reason_,
                             garage_?",\"garage\":1":"");
        if(n<0 || size_t(n)>=cap) { if(cap)out[0]=0;return 0; }
        return size_t(n);
    }
    // Every POSITION observation: GPS time and the first-fix continuity check.
    template<class J> void position(J& j,const adapter::Observation& o,const navigation::G3Motion& m) {
        if(!active() || o.kind!=adapter::Observation::POSITION || o.position_class!=adapter::POSITION_FIX)return;
        const adapter::PositionInput& p=o.position;
        if((p.mode!=1 && p.mode!=2) || !p.utc_seconds || !std::isfinite(p.latitude_deg) ||
           !std::isfinite(p.longitude_deg) || fabs(p.latitude_deg)>90 || fabs(p.longitude_deg)>180)return;
        have_utc_=true;last_utc_s_=p.utc_seconds;last_utc_mono_=o.mono_ns;
        if(first_fix_)return;
        first_fix_=true;
        if(state_!=G3_CARRIED && state_!=G3_CARRIED_UNVERIFIED)return;
        const char* reject=0;
        if(stored_.utc_s>=G3_PLAUSIBLE_UTC_S) {
            if(p.utc_seconds<stored_.utc_s)reject="age_negative";
            else {
                age_known_=true;age_s_=p.utc_seconds-stored_.utc_s;age_src_="gps";
                if(age_s_>G3_AGE_MAX_S)reject="age";
            }
        }
        double limit=-1;
        if(stored_.flags&G3_FLAG_POSITION) {
            moved_m_=distance_m(p.latitude_deg,p.longitude_deg,stored_.lat_e7/1e7,stored_.lon_e7/1e7);
            limit=G3_MOVED_MIN_M+stored_.pos_err_dm/10.0+(std::isfinite(m.distance_m)?m.distance_m:0);
            pos_check_=moved_m_>limit?"moved":"ok";
            if(!reject && moved_m_>limit)reject="moved";
        } else pos_check_="no_position";
        if(reject) { state_=G3_REJECTED;reason_=reject; }
        else if(state_==G3_CARRIED_UNVERIFIED && !strcmp(pos_check_,"ok") && age_known_) {
            state_=G3_CARRIED;reason_="position_ok";
        }
        char d[32],lim[32],age[32];
        number(moved_m_,d,"%.1f");number(limit,lim,"%.1f");
        if(age_known_)snprintf(age,sizeof age,"%llu",(unsigned long long)age_s_);else strcpy(age,"null");
        row(j,",\"mono_ns\":%llu,\"event\":\"checked\",\"reason\":\"%s\",\"g3\":\"%s\",\"pos\":\"%s\","
              "\"d_m\":%s,\"limit_m\":%s,\"age_s\":%s,\"age_src\":\"%s\",\"m_b\":%.1f}",
            "g3_store",(unsigned long long)o.mono_ns,reason_,g3_state_name(state_),pos_check_,d,lim,age,age_src_,
            std::isfinite(m.distance_m)?m.distance_m:0.0);
    }
    // Every SEND observation: the session start (first LOCATION send) and the
    // stock original bearing (bytes 40/44) and speed (32/36).
    void send(const adapter::Observation& o) {
        if(!active() || o.kind!=adapter::Observation::SEND || o.type!=1 || !o.has_payload)return;
        stock_valid_=true;stock_has_=o.original[40]!=0;
        stock_deg_=get32(o.original+44)/1e6;
        stock_speed_=o.original[32]?int32_t(get32(o.original+36))/1e3:-1;
        if(!session_ns_)session_ns_=o.mono_ns?o.mono_ns:1;
        if(o.mono_ns>=session_ns_ && o.mono_ns-session_ns_<=G3_FIRST_WINDOW_NS) {
            ++stock_n_;
            if(!stock_has_)++stock_none_;
            else if(get32(o.original+44)==0)++stock_zero_;
        }
    }
    // MODEL tick (after the BETA tick). course: the first M-accepted course,
    // when one was found this tick. io_block: g3_io_block_reason() now (0 =
    // a parked write may be requested this tick).
    template<class J> void tick(J& j,const G3Sample& s,const G3Course* course,const char* io_block) {
        if(!active())return;
        if(have_carried_)propagate(s);
        if((state_==G3_CARRIED || state_==G3_CARRIED_UNVERIFIED) && prop_unc_>unc_max()) {
            state_=G3_REJECTED;reason_="unc_grew";
            row(j,",\"mono_ns\":%llu,\"event\":\"checked\",\"reason\":\"%s\",\"g3\":\"%s\",\"punc\":%ld,\"m_b\":%.1f}",
                "g3_store",(unsigned long long)s.now,reason_,g3_state_name(state_),lround(prop_unc_*10),
                s.motion.distance_m);
        }
        // Start-up movement check (NNG: a first sensor epoch at speed resets the
        // saved state): the car may have turned while the boot was not
        // observing it (G3Motion::boot_motion), so the carried heading is unknown.
        if((state_==G3_CARRIED || state_==G3_CARRIED_UNVERIFIED) && s.motion.boot_motion) {
            state_=G3_REJECTED;reason_="moving_at_boot";
            row(j,",\"mono_ns\":%llu,\"event\":\"checked\",\"reason\":\"%s\",\"g3\":\"%s\",\"boot_motion\":%u,"
                  "\"gap_ms\":%llu,\"m_b\":%.1f}",
                "g3_store",(unsigned long long)s.now,reason_,g3_state_name(state_),s.motion.boot_motion,
                (unsigned long long)(s.motion.boot_gap_ns/1000000ULL),s.motion.distance_m);
        }
        if(session_ns_ && !start_done_) { start_done_=true;session_m0_=s.motion.distance_m;first_row(j,s,"start",0); }
        if(start_done_ && !t30_done_ && s.now>=session_ns_ && s.now-session_ns_>=G3_FIRST_WINDOW_NS) {
            t30_done_=true;first_row(j,s,"t30",0);
        }
        if(course && !course_done_) {
            course_done_=true;
            if(state_==G3_CARRIED || state_==G3_CARRIED_UNVERIFIED) {
                const double err=g3_wrap180(travel(s)-course->course_deg);
                state_=fabs(err)<=G3_CONFIRM_MAX_DEG?G3_CONFIRMED:G3_REJECTED;
                reason_=state_==G3_CONFIRMED?"course_ok":"course_error";
            }
            first_row(j,s,"course",course);
        }
        reference(s);
        store(j,s,io_block);
    }
    // Writer handoff: the newest request, once. in_flight() until its result.
    // sync: whether this write runs the fsync step (a stop of >= 60 s).
    // target: G3_TARGET_STORE or G3_TARGET_TOKEN (the lazy token line); a
    // caller that cannot route a token request (no target) gets none, so a
    // token line never reaches the record file. Review P4: the refused token
    // request is DROPPED (not left pending, which would block every later S
    // and M request of the boot) with the request spacing it took restored,
    // so the next tick raises it again. Such a caller therefore never gets
    // an S record of a lazy boot before the token landed (by the
    // synchronous path); the product and the replay always pass a target.
    bool take_request(char* out,size_t cap,size_t* n,uint32_t* seq,bool* sync=0,unsigned* target=0) {
        if(pending_ && pending_token_ && !target) {
            pending_=false;pending_token_=false;last_request_ns_=token_prev_request_ns_;
            return false;
        }
        if(!pending_ || !store_on_ || cap<pending_len_)return false;
        memcpy(out,pending_line_,pending_len_);*n=pending_len_;*seq=pending_rec_.seq;
        if(sync)*sync=pending_sync_;
        if(target)*target=pending_token_?G3_TARGET_TOKEN:G3_TARGET_STORE;
        pending_=false;in_flight_=true;cancel_why_=0;
        return true;
    }
    // Rule F taint end (v4): the in-flight write carries a heading built
    // before a G3Motion::discards change. The worker adds this to its cancel
    // reasons: the store thread then stops before its next gated step (the
    // fsync or the rename); a write already renamed is rewritten by store().
    const char* discard_cancel(const G3Sample& s) const {
        return in_flight_ && (pending_rec_.flags&G3_FLAG_HEADING) && pending_discards_!=s.motion.discards?
            "rule_f_discard":0;
    }
    // The worker cancelled the in-flight write; the first reason is journaled.
    void note_cancel(const char* why) { if(in_flight_ && !cancel_why_)cancel_why_=why; }
    bool in_flight(uint32_t* seq) const { if(in_flight_)*seq=pending_rec_.seq;return in_flight_; }
    // Garage carry: the in-flight write is a departure mark (static content:
    // the worker does not cancel it for a moving wheel or a stale stream) /
    // runs the fsync step.
    bool in_flight_mark() const { return in_flight_ && pending_mark_; }
    // Static content (the departure mark or the lazy token line): not
    // cancelled for a moving wheel or a stale stream.
    bool in_flight_static() const { return in_flight_ && (pending_mark_ || pending_token_); }
    bool in_flight_token() const { return in_flight_ && pending_token_; }
    bool in_flight_sync() const { return in_flight_ && pending_sync_; }
    // The store thread's mailbox refused the request: nothing in flight; the
    // stop may try again (not counted as a write failure).
    void post_failed() {
        if(!in_flight_)return;
        in_flight_=false;
        if(pending_token_) { pending_token_=false;return; }   // requested again (token_why_ stays)
        if(pending_sync_)episode_synced_=false;
        if(!episode_stored_)episode_done_=false;
    }
    // The outcome of the in-flight request. written: published. cancelled
    // (idle rule failed / car moved / stale stream), stale (picked up too
    // late): nothing replaced, this stop may retry; stopped (capture stop or
    // worker exit): writes end for this boot; anything else is an I/O failure.
    template<class J> void store_result(J& j,uint64_t now,uint32_t seq,const G3IoResult& r) {
        if(!in_flight_ || seq!=pending_rec_.seq)return;
        in_flight_=false;
        const bool published=r.stage==G3_IO_OK;
        const bool cancelled=r.stage==G3_IO_CANCELLED || r.stage==G3_IO_STALE || r.stage==G3_IO_STOPPED;
        if(pending_token_) {
            // Lazy token: landed = this boot is the previous product boot of
            // the next one; cancelled/stale: requested again; stopped: no more
            // store I/O this boot; a failure counts toward the token's own
            // limit (review P3: never toward the record failures that turn
            // the store off), after which token_sync_due() hands it to the
            // worker's one synchronous attempt.
            pending_token_=false;
            if(published) { token_done_=true;token_failures_=0; }
            else if(cancelled) { if(r.stage==G3_IO_STOPPED)store_on_=false; }
            else ++token_failures_;
            token_row(j,now,published?"written":cancelled?"cancelled":"failed",r);
            return;
        }
        if(published && pending_mark_) {
            // Departure mark: the S record is gone; the motion reference of
            // the last S record (g3_since, the 2 m rule) stays.
            failures_=0;disk_=pending_rec_.state;disk_flags_=pending_rec_.flags;disk_cdeg_=0;written_=true;
            disk_synced_=false;++writes_;
        } else if(published) {
            failures_=0;disk_=pending_rec_.state;
            m_ref_=pending_m_;turn_ref_=pending_turn_;aturn_ref_=pending_aturn_;written_ns_=now?now:1;
            disk_flags_=pending_rec_.flags;disk_cdeg_=pending_rec_.heading_cdeg;written_=true;
            disk_discards_=pending_discards_;
            disk_synced_=pending_sync_;episode_stored_=true;
            ++writes_;
        } else if(pending_mark_) {
            // A departure mark that did not land is requested again (rate limit).
            if(!cancelled && ++failures_>=G3_WRITE_FAILURES_MAX)store_on_=false;
            if(r.stage==G3_IO_STOPPED)store_on_=false;
        } else {
            // This stop may retry after the rate limit (the short write, or
            // the fsync write once the stop is long).
            if(pending_sync_)episode_synced_=false;
            if(!episode_stored_)episode_done_=false;
            if(cancelled) { if(r.stage==G3_IO_STOPPED)store_on_=false; }
            else if(++failures_>=G3_WRITE_FAILURES_MAX)store_on_=false;
        }
        char ref[96];
        if(pending_ref_ns_ && now>=pending_ref_ns_)
            snprintf(ref,sizeof ref,",\"ref_s\":%.1f",(now-pending_ref_ns_)/1e9);
        else strcpy(ref,",\"ref_s\":null");
        const size_t rn=strlen(ref);
        if(pending_beta_valid_)snprintf(ref+rn,sizeof ref-rn,",\"beta\":[%ld,%ld]",
            lround(g3_wrap360(pending_beta_deg_)*10),lround(pending_beta_unc_*10));
        else snprintf(ref+rn,sizeof ref-rn,",\"beta\":null");
        char extra[160];
        if(published)extra[0]=0;
        else if(cancelled)snprintf(extra,sizeof extra,",\"reason\":\"%s\",\"at\":\"%s\",\"why\":\"%s\"%s",
                                   g3_io_stage_name(r.stage),g3_step_name(r.step),
                                   r.stage==G3_IO_STOPPED?"stop_flag":r.stage==G3_IO_STALE?"late_pickup":
                                   cancel_why_?cancel_why_:"unknown",store_on_?"":",\"disabled\":true");
        else snprintf(extra,sizeof extra,",\"stage\":\"%s\",\"errno\":%d%s",g3_io_stage_name(r.stage),
                      r.error,store_on_?"":",\"disabled\":true");
        row(j,",\"mono_ns\":%llu,\"event\":\"%s\",\"seq\":%lu,\"st\":\"%c\",\"hv\":%u,\"h\":%ld,\"unc\":%ld,"
              "\"sync\":%d,\"m_b\":%.1f,\"wait_s\":%.1f,\"lat_ms\":%.1f,\"start_ns\":%llu,\"end_ns\":%llu%s%s%s}",
            "g3_store",(unsigned long long)now,published?"written":cancelled?"cancelled":"failed",(unsigned long)seq,
            pending_rec_.state,pending_rec_.flags&G3_FLAG_HEADING,lround(pending_rec_.heading_cdeg/10.0),
            lround(pending_rec_.unc_cdeg/10.0),pending_sync_?1:0,pending_m_,pending_wait_s_,r.latency_ns/1e6,
            (unsigned long long)r.start_ns,(unsigned long long)r.end_ns,ref,extra,
            garage_?(pending_mark_?",\"garage\":1,\"mark\":1":",\"garage\":1"):"");
    }
    // No store thread: the store is off for this boot (never written inline).
    template<class J> void store_unavailable(J& j,uint64_t now,const char* why) {
        if(!store_on_)return;
        store_on_=false;pending_=false;pending_token_=false;
        row(j,",\"mono_ns\":%llu,\"event\":\"unavailable\",\"reason\":\"%s\",\"g3\":\"%s\"}",
            "g3_store",(unsigned long long)now,why,g3_state_name(state_));
    }
    // Lazy token, synchronous write by the worker (no store thread, journal or
    // audit fault, capture stop: the boot stops observing, so the rest of it
    // counts as blind). One attempt per tracker; journaled like a store row.
    template<class J> void token_sync(J& j,uint64_t now,const char* why,const G3IoResult& r) {
        if(!token_wanted())return;
        token_closed_=true;
        if(r.stage==G3_IO_OK)token_done_=true;
        // A token request not yet handed over is obsolete (review P4).
        if(pending_ && pending_token_ && !in_flight_) { pending_=false;pending_token_=false; }
        if(!token_why_)token_why_=why;
        token_row(j,now,r.stage==G3_IO_OK?"sync":"sync_failed",r);
    }

private:
    G3State state_;
    bool garage_, garage_carry_, pending_mark_;
    // Lazy token: this boot writes it; it landed (or nothing to write); a
    // terminal attempt was made; the request in flight is the token line.
    bool token_lazy_, token_done_, token_closed_, pending_token_;
    const char* token_why_;
    const char* token_mode_;
    uint32_t token_seq_;
    unsigned token_failures_;
    uint64_t token_prev_request_ns_;
    const char* reason_;
    const char* chain_;
    const char* age_src_;
    const char* pos_check_;
    char boot_id_[37], chain_now_[9], prev_boot_[37];
    G3Load load_;
    bool have_stored_, have_carried_, age_known_, first_fix_, have_utc_;
    G3Record stored_;
    uint64_t start_ns_, age_s_, last_utc_s_, last_utc_mono_;
    double carried_deg_, carried_unc_, prop_body_, prop_unc_, moved_m_;
    // session
    uint64_t session_ns_;
    double session_m0_;
    bool start_done_, t30_done_, course_done_;
    bool stock_valid_, stock_has_;
    double stock_deg_, stock_speed_;
    unsigned stock_n_, stock_zero_, stock_none_;
    // store
    bool store_on_, pending_, in_flight_, written_;
    // Stop episode (identified by the newest moving wheel time): whether it
    // needs a parked write, whether one was requested, and why it waited.
    uint64_t episode_id_;
    bool episode_needed_, episode_done_, episode_parked_, episode_stored_;
    // fsync bookkeeping: the request in flight runs the fsync step; this stop
    // already requested its fsync write; the last published record was synced.
    bool pending_sync_, episode_synced_, disk_synced_;
    const char* cancel_why_;
    // Motion at the last record (written this boot, or the boot start for a
    // restored record) and when it was written (0: restored or none).
    double turn_ref_, aturn_ref_, pending_turn_, pending_aturn_;
    uint64_t written_ns_;
    const char* episode_block_;
    double pending_wait_s_;
    char disk_;
    unsigned disk_flags_;
    int32_t disk_cdeg_;
    double m_ref_, pending_m_;
    uint64_t last_request_ns_, last_s_request_ns_;
    uint32_t next_seq_;
    unsigned failures_;
    uint64_t writes_, dropped_;
    G3Record pending_rec_;
    char pending_line_[G3_RECORD_MAX];
    size_t pending_len_;
    // Stored heading (G3_FIRST_DRIVE_2026-10-11): the BETA body heading at a
    // reference tick, carried by the G3Motion yaw integral while moving (the
    // rule-5 growth of propagate()) until the yaw chain breaks. ref_*: the
    // reference; head_*: this tick's carried value; pending_beta_*: BETA's own
    // value at the request (journal only).
    bool ref_valid_, head_valid_, pending_beta_valid_;
    double ref_deg_, ref_unc_, ref_turn_, ref_aturn_, ref_moving_s_, ref_rail_;
    uint32_t ref_breaks_;
    // Rule F discards (G3Motion::discards) when the pending / the published
    // record was built: a heading built before a discard may come from a
    // seed 180 deg wrong; it is dropped (pending) or rewritten (on disk).
    uint32_t pending_discards_, disk_discards_;
    uint64_t ref_ns_, pending_ref_ns_;
    double head_deg_, head_unc_, pending_beta_deg_, pending_beta_unc_;

    void clear() {
        state_=G3_OFF;reason_="not_started";chain_="none";age_src_="none";pos_check_="none";
        garage_=garage_carry_=pending_mark_=false;
        token_lazy_=token_done_=token_closed_=pending_token_=false;token_why_=0;token_mode_=0;
        token_seq_=G3_TOKEN_SEQ_BASE;token_failures_=0;token_prev_request_ns_=0;
        strcpy(boot_id_,"unknown");strcpy(chain_now_,"unread");strcpy(prev_boot_,"unread");
        load_=G3_LOAD_MISSING;have_stored_=have_carried_=age_known_=first_fix_=have_utc_=false;
        stored_=g3_record();
        start_ns_=age_s_=last_utc_s_=last_utc_mono_=0;
        carried_deg_=carried_unc_=prop_body_=prop_unc_=0;moved_m_=-1;
        session_ns_=0;session_m0_=0;start_done_=t30_done_=course_done_=false;
        stock_valid_=stock_has_=false;stock_deg_=0;stock_speed_=-1;stock_n_=stock_zero_=stock_none_=0;
        store_on_=pending_=in_flight_=written_=false;disk_=0;disk_flags_=0;disk_cdeg_=0;
        episode_id_=0;episode_needed_=episode_done_=episode_parked_=episode_stored_=false;episode_block_="none";
        pending_sync_=episode_synced_=disk_synced_=false;cancel_why_=0;
        pending_wait_s_=0;turn_ref_=aturn_ref_=pending_turn_=pending_aturn_=0;written_ns_=0;
        m_ref_=pending_m_=0;last_request_ns_=last_s_request_ns_=0;next_seq_=1;failures_=0;writes_=dropped_=0;
        pending_rec_=g3_record();pending_line_[0]=0;pending_len_=0;
        ref_valid_=head_valid_=pending_beta_valid_=false;
        ref_deg_=ref_unc_=ref_turn_=ref_aturn_=ref_moving_s_=ref_rail_=0;ref_breaks_=0;ref_ns_=pending_ref_ns_=0;
        pending_discards_=disk_discards_=0;
        head_deg_=head_unc_=pending_beta_deg_=pending_beta_unc_=0;
    }
    static void copy(char* out,size_t cap,const char* in) {
        size_t n=strlen(in);if(n>=cap)n=cap-1;
        memcpy(out,in,n);out[n]=0;
    }
    static uint32_t get32(const uint8_t* p) {
        return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);
    }
    static void number(double v,char out[32],const char* fmt) {
        if(std::isfinite(v) && v>=0)snprintf(out,32,fmt,v);else strcpy(out,"null");
    }
    static double distance_m(double lat1,double lon1,double lat2,double lon2) {
        double dlon=lon1-lon2;
        if(dlon>180)dlon-=360;
        if(dlon< -180)dlon+=360;
        const double north=(lat1-lat2)*111320.0;
        const double east=dlon*111320.0*cos((lat1+lat2)*0.5*G3_PI/180.0);
        return sqrt(north*north+east*east);
    }
    // Stored body heading plus the yaw integrated while moving since boot;
    // its uncertainty grows by the BETA rule-5 terms (k*t + c_rot*|rotation|).
    void propagate(const G3Sample& s) {
        const navigation::G3Motion& m=s.motion;
        const double turn=std::isfinite(m.turn_rad)?m.turn_rad:0;
        prop_body_=g3_wrap360(carried_deg_+turn*180.0/G3_PI);
        // Garage chain terms only for a carried flag-4 record (review C2).
        const double grow=time_coef(s,garage_carry_)*(std::isfinite(m.moving_s)?m.moving_s:0)+
            rotation_coef(s,garage_carry_)*(std::isfinite(m.abs_turn_rad)?m.abs_turn_rad:0)+
            (std::isfinite(m.rail_excess_rad)?m.rail_excess_rad:0);   // yaw rail allowance
        prop_unc_=carried_unc_+grow*180.0/G3_PI;
    }
    // The heading a parked record stores. A BETA sample (re)sets the
    // reference when its honest uncertainty is not larger than the carried
    // one, or when the two disagree beyond both bounds; otherwise the carried
    // value is kept (BETA's budget also grows at a standstill and in low-speed
    // GPS blends, the carried one only while a wheel moves). With BETA invalid
    // (unseeded after a cadence fence or before its first accepted course) the
    // reference is carried. A yaw chain break (G3Motion::breaks) invalidates
    // it until the next valid BETA sample: never seeded or broken = no heading.
    void reference(const G3Sample& s) {
        const navigation::G3Motion& m=s.motion;
        if(ref_valid_ && m.breaks!=ref_breaks_)ref_valid_=false;
        const double turn=std::isfinite(m.turn_rad)?m.turn_rad:0;
        const double aturn=std::isfinite(m.abs_turn_rad)?m.abs_turn_rad:0;
        const double moving=std::isfinite(m.moving_s)?m.moving_s:0;
        const double rail=std::isfinite(m.rail_excess_rad)?m.rail_excess_rad:0;
        head_valid_=false;
        if(ref_valid_) {
            head_deg_=g3_wrap360(ref_deg_+(turn-ref_turn_)*180.0/G3_PI);
            // The stored uncertainty of a garage-on boot (its records carry
            // flag 4) grows with the garage chain terms (review C2).
            const double grow=time_coef(s,garage_)*(moving-ref_moving_s_)+rotation_coef(s,garage_)*(aturn-ref_aturn_)+
                (rail-ref_rail_);
            head_unc_=ref_unc_+(grow>0?grow:0)*180.0/G3_PI;
            head_valid_=std::isfinite(head_deg_) && std::isfinite(head_unc_);
        }
        if(s.heading_valid && (!head_valid_ || s.unc_deg<=head_unc_ ||
                               fabs(g3_wrap180(s.heading_deg-head_deg_))>s.unc_deg+head_unc_)) {
            ref_valid_=true;ref_deg_=g3_wrap360(s.heading_deg);ref_unc_=s.unc_deg;
            ref_turn_=turn;ref_aturn_=aturn;ref_moving_s_=moving;ref_rail_=rail;ref_breaks_=m.breaks;ref_ns_=s.now?s.now:1;
            head_valid_=true;head_deg_=ref_deg_;head_unc_=ref_unc_;
        }
    }
    // Heading uncertainty growth terms. chain: the garage chain (the stored
    // value of a garage-on boot, or a carried flag-4 record): rotation 2 %,
    // time term never below BETA's; otherwise the BETA rule-5 terms.
    static double rotation_coef(const G3Sample& s,bool chain) {
        return chain?G3_GARAGE_ROTATION_PER_RAD:s.rotation_per_rad;
    }
    static double time_coef(const G3Sample& s,bool chain) {
        return chain && s.yaw_error_rad_s<G3_GARAGE_YAW_ERROR_RAD_S?G3_GARAGE_YAW_ERROR_RAD_S:s.yaw_error_rad_s;
    }
    // Travel direction of the carried heading: body + 180 deg while the
    // reverse latch says reverse; an unknown latch is journaled as forward.
    double travel(const G3Sample& s) const { return g3_wrap360(prop_body_+(s.reverse==1?180.0:0.0)); }
    // A bounded diagnostic row; one that does not fit is dropped and counted
    // (never j.fail(): G3 must not change the fail-closed behaviour).
    template<class J> void row(J& j,const char* fmt,const char* kind,...) __attribute__((format(printf,3,5)));
    template<class J> void first_row(J& j,const G3Sample& s,const char* event,const G3Course* c) {
        char t[32],ms[32],carried[48],body[48],tr[32],stock[64],crs[64],errs[96];
        const bool session=session_ns_ && s.now>=session_ns_;
        if(session)snprintf(t,sizeof t,"%.1f",(s.now-session_ns_)/1e9);else strcpy(t,"null");
        if(start_done_)snprintf(ms,sizeof ms,"%.1f",s.motion.distance_m-session_m0_);else strcpy(ms,"null");
        if(have_carried_) {
            snprintf(carried,sizeof carried,"[%ld,%ld]",lround(carried_deg_*10),lround(carried_unc_*10));
            snprintf(body,sizeof body,"[%ld,%ld]",lround(prop_body_*10),lround(prop_unc_*10));
            snprintf(tr,sizeof tr,"%ld",lround(travel(s)*10));
        } else { strcpy(carried,"null");strcpy(body,"null");strcpy(tr,"null"); }
        char uncd[24];
        if(have_carried_) {
            const double mv=std::isfinite(s.motion.moving_s)?s.motion.moving_s:0;
            snprintf(uncd,sizeof uncd,"%ld",lround((prop_unc_+G3_STARTUP_DRIFT_DEG_S*
                (mv<G3_STARTUP_DRIFT_WINDOW_S?mv:G3_STARTUP_DRIFT_WINDOW_S))*10));
        } else strcpy(uncd,"null");
        if(stock_valid_)snprintf(stock,sizeof stock,"[%d,%ld,%ld]",stock_has_?1:0,lround(stock_deg_*10),
                                 stock_speed_>=0?lround(stock_speed_*100):-1L);
        else strcpy(stock,"null");
        if(c) {
            snprintf(crs,sizeof crs,",\"course\":[%ld,%ld,%u]",lround(c->course_deg*10),
                     c->kmh>=0?lround(c->kmh*10):-1L,c->source);
            char ec[16],ep[16],es[16];
            if(have_carried_) {
                snprintf(ec,sizeof ec,"%ld",lround(g3_wrap180(carried_deg_-c->course_deg)*10));
                snprintf(ep,sizeof ep,"%ld",lround(g3_wrap180(travel(s)-c->course_deg)*10));
            } else { strcpy(ec,"null");strcpy(ep,"null"); }
            if(stock_valid_ && stock_has_)snprintf(es,sizeof es,"%ld",lround(g3_wrap180(stock_deg_-c->course_deg)*10));
            else strcpy(es,"null");
            snprintf(errs,sizeof errs,",\"err_c\":%s,\"err_p\":%s,\"err_s\":%s",ec,ep,es);
        } else {
            crs[0]=0;
            if(have_carried_ && stock_valid_ && stock_has_)
                snprintf(errs,sizeof errs,",\"d_sp\":%ld",lround(g3_wrap180(stock_deg_-travel(s))*10));
            else strcpy(errs,",\"d_sp\":null");
        }
        row(j,",\"mono_ns\":%llu,\"event\":\"%s\",\"g3\":\"%s\",\"reason\":\"%s\",\"t_s\":%s,\"m_s\":%s,"
              "\"m_b\":%.1f,\"v\":%.2f,\"rev\":[%d,%d],\"carried\":%s,\"body\":%s,\"unc_d\":%s,\"travel\":%s,\"stock\":%s,"
              "\"stock_n\":%u,\"stock_zero\":%u,\"stock_none\":%u%s%s}",
            "g3_first",(unsigned long long)s.now,event,g3_state_name(state_),reason_,t,ms,s.motion.distance_m,
            std::isfinite(s.motion.speed_mps)?s.motion.speed_mps:0.0,s.motion.first_move_reverse,s.reverse,
            carried,body,uncd,tr,stock,stock_n_,stock_zero_,stock_none_,crs,errs);
    }
    // g3_since: what happened to the car since the last record (distance,
    // net body rotation and |rotation| integrated while moving, the record's
    // age) without any file I/O.
    template<class J> void since(J& j,const G3Sample& s,const char* event,const char* reason) {
        const navigation::G3Motion& m=s.motion;
        char age[32];
        if(written_ns_ && s.now>=written_ns_)snprintf(age,sizeof age,"%.1f",(s.now-written_ns_)/1e9);
        else if(have_stored_ && age_known_ && s.now>=start_ns_)
            snprintf(age,sizeof age,"%.1f",double(age_s_)+(s.now-start_ns_)/1e9);
        else strcpy(age,"null");
        row(j,",\"mono_ns\":%llu,\"event\":\"%s\",\"reason\":\"%s\",\"g3\":\"%s\",\"m\":%.1f,\"rot\":%ld,"
              "\"arot\":%ld,\"age_s\":%s,\"m_b\":%.1f}",
            "g3_since",(unsigned long long)s.now,event,reason,g3_state_name(state_),
            std::isfinite(m.distance_m)?m.distance_m-m_ref_:0.0,
            lround((std::isfinite(m.turn_rad)?m.turn_rad-turn_ref_:0.0)*1800.0/G3_PI),
            lround((std::isfinite(m.abs_turn_rad)?m.abs_turn_rad-aturn_ref_:0.0)*1800.0/G3_PI),age,
            std::isfinite(m.distance_m)?m.distance_m:0.0);
    }
    // Store policy (see the constants): the PARKED write (garage carry: and
    // the departure mark, depart()). Builds at most one request per stop; the
    // runtime hands it to the store thread.
    template<class J> void store(J& j,const G3Sample& s,const char* io_block) {
        const navigation::G3Motion& m=s.motion;
        // A stop: a run of exactly-zero wheel samples without a gap above
        // G3_WHEEL_GAP_NS (pipeline G3Motion::zero_since_ns, 0 while moving).
        const uint64_t stop_id=m.zero_since_ns;
        // Rule F discard (v3): a not yet handed-over request whose heading was
        // built before the discard is dropped (this stop may request again).
        if(pending_ && (pending_rec_.flags&G3_FLAG_HEADING) && pending_discards_!=m.discards) {
            pending_=false;episode_done_=false;
        }
        if(stop_id!=episode_id_) {
            // The previous stop ended. Review MEDIUM-2: one g3_since row (no
            // I/O) for a park that needed a record and never got one, and for
            // the start of motion after a stored park.
            if(episode_parked_ && episode_needed_ && !episode_done_)since(j,s,"unstored",episode_block_);
            else if(episode_parked_ && episode_stored_)since(j,s,"left","stored");
            episode_id_=stop_id;episode_needed_=episode_done_=episode_parked_=episode_stored_=episode_synced_=false;
            episode_block_="none";
        }
        // Parked: >= 15 s (garage carry: 5 s) of exactly-zero samples and the
        // stream still fresh.
        const uint64_t parked_ns=garage_?G3_GARAGE_PARKED_NS:G3_PARKED_NS;
        const bool parked=stop_id && s.wheels_fresh && !s.wheels_moving && m.last_speed_ns>stop_id &&
            m.last_speed_ns-stop_id>=parked_ns;
        if(parked)episode_parked_=true;
        // Lazy token (G3_TOKEN_*): a raw-stream trigger (moved / blind) or
        // the pipeline's own 2 m: request the token write before anything
        // else this boot stores (the departure mark may go first: an M
        // record is never carried). The pipeline branch is a second source
        // on purpose (review R2): with the raw watch fed it is normally
        // redundant (the watch sees a superset of the wheel events), but the
        // pipeline integrates events the watch skips (a receipt time older
        // than the last one) and covers a caller whose watch saw nothing
        // (test_g3 token_gate exercises it alone). Mark gate here; the
        // request diverted from a park S request below takes the S gate.
        if(token_lazy_ && !token_done_) {
            if(!token_why_ && s.token_why)token_why_=s.token_why;
            if(!token_why_ && std::isfinite(m.distance_m) && m.distance_m>=G3_TOKEN_MOVED_M)token_why_="moved";
            if(token_why_ && token_request(s,io_block))return;
        }
        if(!parked) { if(garage_)depart(s,io_block);return; }
        const bool long_stop=m.last_speed_ns-stop_id>=G3_FSYNC_MIN_STOP_NS;
        // Garage carry: no fsync while BETA replaces (the write itself may run).
        const bool sync_ok=!(garage_ && s.beta_replacing);
        bool want=false,sync=false;
        // Rule F discard (v3): the record on disk (written in this boot) holds
        // a heading built before a discard: rewrite it at this stop, even
        // after an earlier write of the same stop, with the current
        // (normally invalid) heading.
        const bool stale=(disk_flags_&G3_FLAG_HEADING) && written_ && disk_discards_!=m.discards;
        if(stale && !pending_ && !in_flight_)episode_done_=false;
        if(!episode_done_) {
            const bool moved=std::isfinite(m.distance_m) && m.distance_m-m_ref_>=G3_MOVING_MARK_M;
            const bool turned=head_valid_ && (!(disk_flags_&G3_FLAG_HEADING) ||
                fabs(g3_wrap180(head_deg_-disk_cdeg_/100.0))>=G3_HEADING_CHANGE_DEG);
            if(!moved && !turned && !stale)episode_done_=true;   // nothing new to store
            else { episode_needed_=true;want=true;sync=long_stop && sync_ok; }
        }
        // The stop passed 60 s: the record written in this boot without fsync
        // is written once more with the fsync.
        if(!want && long_stop && written_ && !disk_synced_ && !episode_synced_ && sync_ok) { want=true;sync=true; }
        if(!want)return;
        if(!store_on_) {
            // Review P3: this boot wants an S record the store cannot write;
            // the token is due (token_sync_due "store_off": the synchronous
            // attempt), so the next boot refuses the older record on disk.
            if(token_lazy_ && !token_done_ && !token_why_)token_why_="record";
            episode_block_="store_off";return;
        }
        if(pending_ || in_flight_)return;
        if(last_request_ns_ && s.now>=last_request_ns_ && s.now-last_request_ns_<G3_WRITE_MIN_NS) {
            episode_block_="rate";return;
        }
        if(io_block) { episode_block_=io_block;return; }
        // Garage carry while BETA replaces (review P3, tunnel jams): at most
        // one S request per G3_GARAGE_REPLACING_S_GAP_NS since the previous
        // S request (also a cancelled or failed one and the 60 s fsync
        // rewrite); the departure mark is not spaced. Exempt: the rule F
        // rewrite of a tainted heading, and (third review C1) any S request
        // while the record on disk is an S WITH heading: the car left that
        // stop without a landed departure mark (idle rule, 5 s spacing,
        // writer lag), so the old S would be carried by the next boot; the
        // new S corrects it. Bound: the 5 s request spacing (G3_WRITE_MIN_NS),
        // i.e. one S per stop while the mark keeps failing, and a cancelled
        // request is retried after 5 s (not "one extra write per departure").
        // A spaced final park therefore leaves the mark on disk only if the
        // mark landed (then no carry).
        const bool disk_heading_s=disk_=='S' && (disk_flags_&G3_FLAG_HEADING);
        if(garage_ && s.beta_replacing && !stale && !disk_heading_s && last_s_request_ns_ &&
           s.now>=last_s_request_ns_ && s.now-last_s_request_ns_<G3_GARAGE_REPLACING_S_GAP_NS) {
            episode_block_="replacing_rate";return;
        }
        // Lazy token: the token must name this boot BEFORE its first S record
        // lands (the next boot compares the record's boot id with it), so the
        // first S request becomes a token request; the S follows after it
        // landed (rate limit). Fifth fix (review C3): this diverted request
        // uses the S request's gate (checked above: store on, nothing pending,
        // 5 s spacing, idle rule, the replacing 30 s S spacing and its
        // exemptions), NOT the departure mark's 100 ms writer-lag rule, so a
        // boot that drives with a writer lag of 100-300 ms while BETA
        // replaces still lands token + S at its final park (HEAD~1 landed
        // its S there; the lag rule left the older boot's S on disk and it
        // was carried stale). After the one synchronous attempt failed
        // (token_closed_ without token_done_) the S is no longer diverted: an
        // S of this boot with the older token on disk is refused by the next
        // boot (not_prev_boot) and replaces the older S, the safe order.
        if(token_lazy_ && !token_done_ && !token_closed_) {
            if(!token_why_)token_why_="record";
            token_request(s,io_block,true);
            episode_block_="token";return;
        }
        const char state='S';
        G3Record r=g3_record();
        r.seq=next_seq_++;if(!next_seq_)next_seq_=1;
        copy(r.boot_id,sizeof r.boot_id,boot_id_);r.state=state;
        if(garage_)r.flags|=G3_FLAG_DEPART;
        if(head_valid_) {
            r.flags|=G3_FLAG_HEADING;
            r.heading_cdeg=int32_t(lround(g3_wrap360(head_deg_)*100))%36000;
            const double unc=head_unc_*100;
            r.unc_cdeg=unc>18000?18000:int32_t(lround(unc));
        }
        pending_beta_valid_=s.heading_valid;pending_beta_deg_=s.heading_deg;pending_beta_unc_=s.unc_deg;
        pending_discards_=m.discards;
        pending_ref_ns_=head_valid_?ref_ns_:0;
        if(s.position_valid) {
            r.flags|=G3_FLAG_POSITION;
            r.lat_e7=int32_t(lround(s.lat*1e7));r.lon_e7=int32_t(lround(s.lon*1e7));
            const double e=s.pos_err_m*10;
            r.pos_err_dm=e>4294967295.0?4294967295u:uint32_t(lround(e));
        }
        if(have_utc_ && s.now>=last_utc_mono_)r.utc_s=last_utc_s_+(s.now-last_utc_mono_)/1000000000ULL;
        r.rt_s=s.rt_s;r.mono_ms=s.now/1000000ULL;
        copy(r.chain,sizeof r.chain,chain_now_);
        const size_t n=g3_encode(r,pending_line_,sizeof pending_line_);
        if(!n)return;
        pending_len_=n;pending_rec_=r;pending_=true;pending_mark_=false;pending_token_=false;pending_m_=m.distance_m;
        pending_turn_=m.turn_rad;pending_aturn_=m.abs_turn_rad;
        pending_wait_s_=(m.last_speed_ns>stop_id+parked_ns?m.last_speed_ns-stop_id-parked_ns:0)/1e9;
        last_request_ns_=s.now?s.now:1;episode_done_=true;last_s_request_ns_=last_request_ns_;
        pending_sync_=sync;if(sync)episode_synced_=true;
    }
    // Lazy token request: the same gate as the departure mark (store on,
    // nothing pending or in flight, the 5 s request spacing, the idle rule;
    // while BETA replaces a journal writer lag <= 100 ms), never fsync.
    // s_gate (review C3): the request diverted from a park S request takes
    // the S request's gate instead (no writer-lag rule; the caller checked
    // the rest). Not after the token's own failure limit (token_sync_due) or
    // the synchronous attempt.
    bool token_request(const G3Sample& s,const char* io_block,bool s_gate=false) {
        if(!store_on_ || pending_ || in_flight_ || token_closed_ || token_failures_>=G3_TOKEN_FAILURES_MAX)return false;
        if(last_request_ns_ && s.now>=last_request_ns_ && s.now-last_request_ns_<G3_WRITE_MIN_NS)return false;
        if(io_block)return false;
        if(!s_gate && s.beta_replacing && s.writer_lag_ns>G3_GARAGE_REPLACING_MARK_LAG_NS)return false;
        const size_t n=g3_last_start_line(boot_id_,pending_line_,sizeof pending_line_);
        if(!n)return false;
        pending_len_=n;pending_rec_=g3_record();pending_rec_.seq=token_seq_++;
        if(token_seq_<G3_TOKEN_SEQ_BASE)token_seq_=G3_TOKEN_SEQ_BASE;
        pending_=true;pending_token_=true;pending_mark_=false;pending_sync_=false;
        pending_m_=s.motion.distance_m;pending_turn_=s.motion.turn_rad;pending_aturn_=s.motion.abs_turn_rad;
        pending_wait_s_=0;pending_beta_valid_=false;pending_ref_ns_=0;pending_discards_=s.motion.discards;
        token_prev_request_ns_=last_request_ns_;last_request_ns_=s.now?s.now:1;
        return true;
    }
    template<class J> void token_row(J& j,uint64_t now,const char* result,const G3IoResult& r) {
        char p[9];copy(p,sizeof p,prev_boot_);
        char extra[160];
        if(r.stage==G3_IO_OK)extra[0]=0;
        else snprintf(extra,sizeof extra,",\"stage\":\"%s\",\"at\":\"%s\",\"errno\":%d%s%s%s%s",
                      g3_io_stage_name(r.stage),g3_step_name(r.step),r.error,
                      r.stage==G3_IO_CANCELLED?",\"why\":\"":"",
                      r.stage==G3_IO_CANCELLED?(cancel_why_?cancel_why_:"unknown"):"",
                      r.stage==G3_IO_CANCELLED?"\"":"",store_on_?"":",\"disabled\":true");
        row(j,",\"mono_ns\":%llu,\"event\":\"token\",\"reason\":\"%s\",\"result\":\"%s\",\"g3\":\"%s\","
              "\"prev\":\"%s\",\"garage\":1,\"sync\":0,\"lat_ms\":%.1f,\"start_ns\":%llu,\"end_ns\":%llu%s}",
            "g3_store",(unsigned long long)now,token_why_?token_why_:"none",result,g3_state_name(state_),p,
            r.latency_ns/1e6,(unsigned long long)r.start_ns,(unsigned long long)r.end_ns,extra);
    }
    // Garage carry departure mark: the car moved (G3_GARAGE_DEPART_M) or
    // turned (G3_GARAGE_DEPART_DEG) since the S record with heading on disk
    // (written this boot, or restored): request one 'M' record without
    // heading (no fsync), retried under the rate limit and the idle rule.
    void depart(const G3Sample& s,const char* io_block) {
        const navigation::G3Motion& m=s.motion;
        if(!store_on_ || disk_!='S' || !(disk_flags_&G3_FLAG_HEADING) || pending_ || in_flight_)return;
        const bool moved=std::isfinite(m.distance_m) && m.distance_m-m_ref_>=G3_GARAGE_DEPART_M;
        const bool turned=std::isfinite(m.turn_rad) &&
            fabs(m.turn_rad-turn_ref_)*180.0/G3_PI>=G3_GARAGE_DEPART_DEG;
        if(!moved && !turned)return;
        if(last_request_ns_ && s.now>=last_request_ns_ && s.now-last_request_ns_<G3_WRITE_MIN_NS)return;
        if(io_block)return;
        // While BETA replaces (review P3): only with an idle journal writer.
        if(s.beta_replacing && s.writer_lag_ns>G3_GARAGE_REPLACING_MARK_LAG_NS)return;
        G3Record r=g3_record();
        r.seq=next_seq_++;if(!next_seq_)next_seq_=1;
        copy(r.boot_id,sizeof r.boot_id,boot_id_);r.state='M';r.flags=G3_FLAG_DEPART;
        if(have_utc_ && s.now>=last_utc_mono_)r.utc_s=last_utc_s_+(s.now-last_utc_mono_)/1000000000ULL;
        r.rt_s=s.rt_s;r.mono_ms=s.now/1000000ULL;
        copy(r.chain,sizeof r.chain,chain_now_);
        const size_t n=g3_encode(r,pending_line_,sizeof pending_line_);
        if(!n)return;
        pending_len_=n;pending_rec_=r;pending_=true;pending_mark_=true;pending_token_=false;pending_sync_=false;
        pending_m_=m.distance_m;pending_turn_=m.turn_rad;pending_aturn_=m.abs_turn_rad;pending_wait_s_=0;
        pending_beta_valid_=s.heading_valid;pending_beta_deg_=s.heading_deg;pending_beta_unc_=s.unc_deg;
        pending_discards_=m.discards;pending_ref_ns_=0;
        last_request_ns_=s.now?s.now:1;
    }
};
// The worker's cancel decision for an in-flight store write (runtime.cpp,
// after each tick). Garage carry: a write without fsync is not cancelled
// because BETA replaces (cancel_ignores_replacing() feeds the idle rule), and
// a departure mark or the lazy token line (static content) not for a moving
// wheel or a stale stream.
inline bool g3_cancel_ignores_replacing(const G3Tracker& g3) { return g3.garage() && !g3.in_flight_sync(); }
inline const char* g3_cancel_reason(const char* io_cancel,const G3Sample& s,const G3Tracker& g3) {
    if(io_cancel)return io_cancel;
    if(s.wheels_moving && !g3.in_flight_static())return "wheels_moving";
    if(!s.wheels_fresh && !g3.in_flight_static())return "wheels_stale";
    return g3.discard_cancel(s);   // rule F taint end (v4)
}
template<class J> void G3Tracker::row(J& j,const char* fmt,const char* kind,...) {
    char line[768];
    const int head=snprintf(line,sizeof line,"{\"kind\":\"%s\"",kind);
    va_list ap;
    va_start(ap,kind);
    const int n=head>0 && size_t(head)<sizeof line?vsnprintf(line+head,sizeof line-size_t(head),fmt,ap):-1;
    va_end(ap);
    if(n<=0 || size_t(head)+size_t(n)>=sizeof line) { ++dropped_;return; }
    j.line(line);
}

} }
#endif
