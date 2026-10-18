// Phone IMU channel v1, CMU-side receiver (stage 1: RECORD-ONLY), included
// only by collector.cpp. validation/PHONE_IMU_CHANNEL_2026-10-14.md.
//
// One thread of the collector (the only process of this package with D-Bus):
//  * bds watch: finds the stock bds process in /proc; only when it runs with
//    our -configFilePath does anything else start (a stock-argument boot stays
//    inert). Not ready within 60 s or a second bds instance with our config
//    (SM restart) writes logs/bt-fail; the guard then starts bds with the stock
//    arguments on every later boot until menu 1.
//  * PTY path: the BCA signal com.jci.bca.ConnectionStatusResp(serviceId 15,
//    ..., (ay) terminalPath) from the bus owner of com.jci.bca, and
//    QueryConnectStatus(15) every 5 s while not connected as the fallback.
//    Both live on the HMI bus (unix:path=/tmp/dbus_hmi_socket): the stock BCA
//    owns com.jci.bca on the service bus too, but registers /com/jci/bca and
//    sends ConnectionStatusResp only on its HMI connection, so a call on the
//    service bus gets UnknownMethod and no signal ever arrives there
//    (validation/BCA_QUERY_CONNECT_STATUS_RE_2026-10-17.md). The receiver's
//    one private D-Bus connection is used for nothing but BCA.
//  * reader: opens the slave, sets it raw, ALWAYS reads (bds blocks when its
//    PTY is not drained), parses frames, sends PING every 5 s, aggregates 1 Hz
//    bins and queues compact rows. Rows are written by the collector main loop
//    (never by this thread), so a slow flash write never stops the reading.
// Rows go to logs/pimu.0.jsonl and pimu.1.jsonl (2 x 1 MiB) with the beta.14
// token-bucket discipline; worst case below 400 KB per hour (budget_bph()).
// Nothing the product sends or decides reads these rows.
#ifndef MX5_COLLECTOR_PHONE_IMU_RX_H
#define MX5_COLLECTOR_PHONE_IMU_RX_H
#include "phone_imu.h"
#include "bus_register.h"
#include "../runtime/storage.h"
#include "../runtime/worker_thread.h"
#include <dbus/dbus.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#ifndef MX5_COLLECTOR_TESTING
#include <linux/capability.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif

namespace mx5 { namespace pimu {

static const char* const BT_XML = "/data_persist/mx5-aa-dr/bt/BdsConfiguration.xml";
static const char* const BDS_PATH = "/jci/bds/bds";
static const unsigned SERVICE_ID = 15;           // our serialPort id in the generated XML
static const uint64_t SEC_NS = 1000000000ULL, MS_NS = 1000000ULL;
static const uint64_t SILENCE_NS = 10 * SEC_NS;  // spec: reconnect after 10 s of silence
static const uint64_t QUERY_NS = 5 * SEC_NS;     // QueryConnectStatus period while not connected
static const uint64_t QUERY_MAX_NS = 60 * SEC_NS;    // poll backoff cap
static const uint64_t VANISH_NS = 30 * SEC_NS;   // bds with our config absent this long = failure
static const size_t OVERLOAD_BPS = 16384;        // ~10x the 1.6 kB/s data rate: discard beyond
static const size_t READ_BUDGET = 65536;         // bytes per loop turn, then the loop turns again
static const unsigned RING = 64, ROW = 768;
static const size_t FILE_CAP = 1048576;          // per pimu.N.jsonl file, two files

inline uint64_t mono_ns() {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return 0;
    return uint64_t(t.tv_sec) * SEC_NS + uint64_t(t.tv_nsec);
}

// Row budget (beta.14 discipline): one bucket per row kind.
enum RowKind { R_PIMU = 0, R_SYNC = 1, R_LINK = 2, R_BT = 3, R_KINDS = 4 };
inline void init_buckets(Bucket b[R_KINDS]) {
    b[R_PIMU].init(10 * SEC_NS, 6);   // one 10 s batch row per 10 s, partial rows at link changes
    b[R_SYNC].init(5 * SEC_NS, 2);    // one pong per 5 s ping
    b[R_LINK].init(10 * SEC_NS, 6);   // connect/disconnect/signal/poll changes
    b[R_BT].init(60 * SEC_NS, 4);     // bds watch events
}
// pimu_link rows with a D-Bus error name/message have their own bucket (the
// drops are counted with R_LINK so the pimu_drop schema is unchanged).
inline void init_err_bucket(Bucket& b) { b.init(300 * SEC_NS, 4); }
static const uint64_t DROP_PERIOD_NS = 60 * SEC_NS;
// Longest row of each kind, by construction (numbers are clamped, strings are
// bounded); tests/collector/test_phone_imu.cpp formats the extremes.
// A pimu_link row with a D-Bus error carries the error name (<= 64) and the
// start of its message (<= 80), both sanitised.
static const size_t ERR_NAME = 64, ERR_MSG = 80;
static const size_t MAX_LINK_ERR_ROW = 384;
static const size_t MAX_SYNC_ROW = 128, MAX_LINK_ROW = 200, MAX_BT_ROW = 128, MAX_DROP_ROW = 220, MAX_BOOT_ROW = 160;
// Repeated D-Bus failures: one row per distinct error name, then every 10th
// failure (the count goes into "n", shown up to 9999). *count saturates at
// 999999, which is not a multiple of 10: no row storm there. last holds
// ERR_NAME + 1 bytes.
inline bool err_row_due(const char* name, char* last, unsigned* count) {
    if (*count < 999999) ++*count;
    bool distinct = strncmp(name, last, ERR_NAME) != 0;
    if (distinct) { strncpy(last, name, ERR_NAME); last[ERR_NAME] = 0; }
    return distinct || *count % 10 == 0;
}
inline double budget_bph() {
    Bucket b[R_KINDS]; init_buckets(b);
    Bucket e; init_err_bucket(e);
    return e.max_rows_per_hour() * MAX_LINK_ERR_ROW + b[R_PIMU].max_rows_per_hour() * double(Aggregator::max_row_bytes()) +
           b[R_SYNC].max_rows_per_hour() * MAX_SYNC_ROW + b[R_LINK].max_rows_per_hour() * MAX_LINK_ROW +
           b[R_BT].max_rows_per_hour() * MAX_BT_ROW + (3600.0e9 / double(DROP_PERIOD_NS) + 1) * MAX_DROP_ROW + MAX_BOOT_ROW;
}

// logs/pimu.0.jsonl (newest) and pimu.1.jsonl. Only the collector main thread
// calls it. Same file rules as the collector journal: no symlinks, regular
// files only, storage reserve, a failure stops this stream only.
class PimuJournal {
public:
    explicit PimuJournal(const char* root) : failed(false), root_(root), f_(0), written_(0) {}
    ~PimuJournal() { if (f_) fclose(f_); }
    void line(const char* s) {
        if (failed) return;
        size_t n = strlen(s);
        if (n + 1 > FILE_CAP) { failed = true; return; }
        const mx5::runtime::StorageSpace space = mx5::runtime::storage_space(root_, n + 1);
        if (space.reason) { mx5::runtime::record_storage_stop(root_, "pimu", space); failed = true; return; }
        if (!f_ || written_ + n + 1 > FILE_CAP) rotate();
        if (!f_ || failed) return;
        if (fwrite(s, 1, n, f_) != n || fputc('\n', f_) == EOF) { failed = true; return; }
        written_ += n + 1;
    }
    void flush() { if (f_ && fflush(f_)) failed = true; }
    bool failed;
private:
    void rotate() {
        if (f_) { bool bad = fflush(f_) != 0; if (fclose(f_)) bad = true; f_ = 0; if (bad) { failed = true; return; } }
        char a[512], b[512];
        snprintf(a, sizeof a, "%s/logs/pimu.0.jsonl", root_);
        snprintf(b, sizeof b, "%s/logs/pimu.1.jsonl", root_);
        struct stat st;
        if (!lstat(a, &st)) {
            if (!S_ISREG(st.st_mode)) { failed = true; return; }
            if (!lstat(b, &st) && !S_ISREG(st.st_mode)) { failed = true; return; }
            if (rename(a, b)) { failed = true; return; }
        }
        int fd = open(a, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode)) { if (fd >= 0) close(fd); failed = true; return; }
        f_ = fdopen(fd, "w");
        if (!f_) { close(fd); failed = true; return; }
        written_ = 0;
    }
    const char* root_;
    FILE* f_;
    size_t written_;
};

struct RxOptions {
    const char* root;          // package directory (logs/, bt/)
    const char* bca_bus_address;  // the stock BCA's D-Bus (HMI bus "unix:path=/tmp/dbus_hmi_socket")
    const char* proc_root;     // "/proc"
    const char* ready_flag;    // "/tmp/bds_ready_flag"
    const char* xml_path;      // the -configFilePath value bds must carry
    const char* boot_id;
    uint64_t ready_timeout_ns; // 60 s
    bool raise_cap;            // raise CAP_DAC_OVERRIDE around the slave open
    uid_t pty_owner;           // 0 on the CMU (bds runs as uid 0)
    const char* pts_dir;       // "/dev/pts/" (tests point it elsewhere)
    uint64_t vanish_ns;        // VANISH_NS (tests shorten it)
};

class Receiver {
public:
    explicit Receiver(const RxOptions& o) : o_(o), stop_(false), started_(false), fd_(-1), conn_(0) {
        pthread_mutex_init(&mu_, 0);
        head_ = tail_ = 0; ring_drop_ = 0;
        init_buckets(buckets_); init_err_bucket(err_bucket_);
        memset(rate_drop_, 0, sizeof rate_drop_);
        memset(path_, 0, sizeof path_); memset(owner_, 0, sizeof owner_);
        sig_other_ = unexpected_ = ping_drop_ = unmatched_ = 0; last_drop_row_ = 0; drop_sig_ = 0;
        bds_state_ = BDS_SEARCH; bds_pid_ = 0; bds_seen_ = bds_missing_ = 0; ready_ = false; next_scan_ = 0;
        quiesced_ = failed_ = opened_once_ = gone_ = false; misses_ = 0; me_.pid = 0; me_.start = 0; me_.ppid = 0; query_period_ = QUERY_NS;
        next_query_ = 0; next_bus_ = 0; query_fail_ = 0; last_frame_ = 0; sec_start_ = 0; sec_bytes_ = 0; pending_out_ = 0;
        memset(&noted_, 0, sizeof noted_);
        last_cs_ = last_st_ = 0xFFFFFFFFu; last_poll_path_[0] = 0;
        bus_fail_ = 0; last_bus_err_[0] = last_query_err_[0] = 0; other_rows_ = 0; other_svc_ = other_cs_ = -1;
    }
    ~Receiver() { stop(); pthread_mutex_destroy(&mu_); }
    bool start() {
        if (started_) return true;
        // Explicit stack (CMU default thread stack is 128 KiB; tests/build/test_thread_policy.py).
        started_ = mx5::runtime::create_thread(&thread_, run_thread, this, 256u << 10, false);
        return started_;
    }
    void stop() {
        if (!started_) return;
        stop_ = true; pthread_join(thread_, 0); started_ = false;
    }
    // Collector main thread: writes the queued rows. Never called by the reader.
    void drain(PimuJournal& j) {
        char row[ROW];
        while (pop(row)) j.line(row);
        j.flush();
    }

private:
    enum BdsState { BDS_SEARCH, BDS_CONFIG, BDS_STOCK };
    static void* run_thread(void* p) { static_cast<Receiver*>(p)->run(); return 0; }

    // ---- queue ---------------------------------------------------------------
    void push(const char* row) {
        pthread_mutex_lock(&mu_);
        if (head_ - tail_ >= RING) ++ring_drop_;
        else { copy(ring_[head_ % RING], ROW, row); ++head_; }
        pthread_mutex_unlock(&mu_);
    }
    bool pop(char* row) {
        pthread_mutex_lock(&mu_);
        bool ok = tail_ != head_;
        if (ok) { memcpy(row, ring_[tail_ % RING], ROW); ++tail_; }
        pthread_mutex_unlock(&mu_);
        return ok;
    }
    void emit(RowKind k, uint64_t now, const char* row) {
        if (!buckets_[k].take(now)) { ++rate_drop_[k]; return; }
        push(row);
    }
    void maybe_drop_row(uint64_t now, bool force) {
        unsigned long long ring;
        pthread_mutex_lock(&mu_); ring = ring_drop_; pthread_mutex_unlock(&mu_);
        unsigned long long sig = rate_drop_[0] + rate_drop_[1] + rate_drop_[2] + rate_drop_[3] + ring + sig_other_ + unexpected_ + ping_drop_ + unmatched_;
        if (sig == drop_sig_ || (!force && last_drop_row_ && now - last_drop_row_ < DROP_PERIOD_NS)) return;
        drop_sig_ = sig; last_drop_row_ = now;
        char row[ROW];
        snprintf(row, sizeof row, "{\"kind\":\"pimu_drop\",\"m\":%llu,\"d\":[%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu]}",
                 (unsigned long long)(now / MS_NS), c(rate_drop_[R_PIMU]), c(rate_drop_[R_SYNC]), c(rate_drop_[R_LINK]), c(rate_drop_[R_BT]),
                 c(ring), c(sig_other_), c(unexpected_), c(ping_drop_), c(unmatched_));
        push(row);
    }
    static void copy(char* dst, size_t cap, const char* src) { size_t n = strlen(src); if (n >= cap) n = cap - 1; memcpy(dst, src, n); dst[n] = 0; }
    static unsigned long long c(unsigned long long v) { return v > 9999999999ULL ? 9999999999ULL : v; }

    // ---- small helpers ---------------------------------------------------------
    bool pts_path(const char* p) const {
        size_t k = strlen(o_.pts_dir);
        if (strncmp(p, o_.pts_dir, k)) return false;
        size_t n = strlen(p + k);
        return n >= 1 && n <= 4 && strspn(p + k, "0123456789") == n;
    }
    static void clean(const char* in, char* out, size_t cap) {  // JSON-safe, bounded
        size_t j = 0;
        for (size_t i = 0; in[i] && j + 1 < cap; ++i) { char ch = in[i]; out[j++] = (ch >= 32 && ch < 127 && ch != '"' && ch != '\\') ? ch : '_'; }
        out[j] = 0;
    }
    // Message text: readable (a double quote becomes ', a backslash /), JSON-safe, bounded.
    static void clean_msg(const char* in, char* out, size_t cap) {
        size_t j = 0;
        for (size_t i = 0; in[i] && j + 1 < cap; ++i) {
            char ch = in[i];
            out[j++] = ch == '"' ? '\'' : ch == '\\' ? '/' : (ch >= 32 && ch < 127) ? ch : '_';
        }
        out[j] = 0;
    }
    // svc/cs/st: -1 = not applicable. name/msg (D-Bus error) only when name is set.
    void link_row(uint64_t now, const char* ev, const char* why, int err, unsigned cs, unsigned st,
                  long long svc = SERVICE_ID, const char* name = 0, const char* msg = 0, unsigned n = 0) {
        char p[24], w[24], nm[ERR_NAME + 1], ms[ERR_MSG + 1], extra[ERR_NAME + ERR_MSG + 48], row[ROW];
        clean(path_, p, sizeof p); clean(why ? why : "", w, sizeof w);
        extra[0] = 0;
        if (name) {
            clean(name, nm, sizeof nm); clean_msg(msg ? msg : "", ms, sizeof ms);
            snprintf(extra, sizeof extra, ",\"name\":\"%s\",\"msg\":\"%s\",\"n\":%u", nm, ms, n > 9999 ? 9999u : n);
        }
        snprintf(row, sizeof row, "{\"kind\":\"pimu_link\",\"m\":%llu,\"ev\":\"%s\",\"why\":\"%s\",\"path\":\"%s\",\"svc\":%lld,\"cs\":%lld,\"st\":%lld,\"err\":%d%s}",
                 (unsigned long long)(now / MS_NS), ev, w, p, svc, cs == 0xFFFFFFFFu ? -1LL : (long long)cs,
                 st == 0xFFFFFFFFu ? -1LL : (long long)st, err, extra);
        if (!name) emit(R_LINK, now, row);
        else if (err_bucket_.take(now)) push(row);
        else ++rate_drop_[R_LINK];
    }
    void bt_row(uint64_t now, const char* ev, const char* why, unsigned long long after_ms) {
        char row[ROW];
        snprintf(row, sizeof row, "{\"kind\":\"bt_watch\",\"m\":%llu,\"ev\":\"%s\",\"why\":\"%s\",\"pid\":%ld,\"after_ms\":%llu}",
                 (unsigned long long)(now / MS_NS), ev, why, (long)bds_pid_, c(after_ms));
        emit(R_BT, now, row);
    }

    // ---- bds watch -------------------------------------------------------------
    // Match: argv[0] has basename "bds" (SM starts /jci/bds/bds with an absolute
    // path; any directory is accepted so a different launcher spelling cannot
    // hide it) and -configFilePath is followed by exactly our XML path.
    // A process is identified by (pid, start time = /proc/<pid>/stat field 22),
    // so a reused pid is a different process.
    enum Probe { P_OURS = 1, P_OTHER_ARGS = 0, P_GONE = -1, P_UNKNOWN = -2 };
    // ENOENT/ESRCH = the entry is gone; any other open/read error (EACCES,
    // EMFILE, ENOMEM, EAGAIN, EIO, ...) is UNKNOWN and never counts as gone.
    static int gone_or_unknown(int err) { return err == ENOENT || err == ESRCH ? P_GONE : P_UNKNOWN; }
    int read_small(long pid, const char* what, char* buf, size_t cap, ssize_t* n) {
        char p[256];
        snprintf(p, sizeof p, "%s/%ld/%s", o_.proc_root, pid, what);
        int fd = open(p, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
        if (fd < 0) return gone_or_unknown(errno);
        *n = read(fd, buf, cap - 1);
        int err = errno;
        close(fd);
        if (*n < 0) return gone_or_unknown(err);
        buf[*n] = 0;
        return 0;
    }
    // Probe one pid: kind of bds, start time, parent pid.
    int probe(long pid, unsigned long long* start, long* ppid) {
        char buf[1024];
        ssize_t n = 0;
        *start = 0; *ppid = 0;
        int r = read_small(pid, "stat", buf, sizeof buf, &n);
        if (r) return r;
        const char* q = strrchr(buf, ')');
        if (!q) return P_GONE;
        unsigned field = 2;  // q points at the end of field 2 (comm)
        for (++q; *q && field < 22; ++q) {
            if (*q == ' ') {
                ++field;
                if (field == 4) *ppid = atol(q + 1);
                if (field == 22) *start = strtoull(q + 1, 0, 10);
            }
        }
        r = read_small(pid, "cmdline", buf, sizeof buf, &n);
        if (r) return r;
        if (n <= 0) return P_GONE;                     // a zombie has an empty cmdline
        const char* base = strrchr(buf, '/');
        if (strcmp(base ? base + 1 : buf, "bds")) return P_GONE;   // the pid now runs something else
        const char* end = buf + n;
        bool flag = false;
        for (const char* a = buf + strlen(buf) + 1; a < end; a += strlen(a) + 1) {
            if (flag) return strcmp(a, o_.xml_path) ? P_OTHER_ARGS : P_OURS;
            flag = !strcmp(a, "-configFilePath");
        }
        return P_OTHER_ARGS;
    }
    int bds_cmdline(long pid) { unsigned long long st; long pp; int k = probe(pid, &st, &pp); return k == P_OURS ? 1 : k == P_OTHER_ARGS ? 0 : -1; }
    struct Seen { long pid; unsigned long long start; long ppid; };
    // Every bds process: our-config processes into cfg[], the oldest with other
    // args into *stock. Returns the number of our-config processes (<= 8).
    unsigned scan_bds(Seen cfg[8], Seen* stock) {
        DIR* d = opendir(o_.proc_root);
        unsigned n = 0; stock->pid = 0;
        if (!d) return 0;
        for (unsigned seen = 0; seen < 8192; ++seen) {
            struct dirent* e = readdir(d);
            if (!e) break;
            const char* name = e->d_name;
            if (!name[0] || strspn(name, "0123456789") != strlen(name) || strlen(name) > 9) continue;
            Seen x; x.pid = atol(name);
            int k = probe(x.pid, &x.start, &x.ppid);
            if (k == P_OURS && n < 8) cfg[n++] = x;
            else if (k == P_OTHER_ARGS && (!stock->pid || x.pid < stock->pid)) *stock = x;
        }
        closedir(d);
        return n;
    }
    // The bds is the oldest process (smallest start time, then pid): a fork of
    // bds running system() has the same cmdline for a moment and is younger.
    static Seen oldest(const Seen* v, unsigned n) {
        Seen best = v[0];
        for (unsigned i = 1; i < n; ++i)
            if (v[i].start < best.start || (v[i].start == best.start && v[i].pid < best.pid)) best = v[i];
        return best;
    }
    void bt_fail(uint64_t now, const char* why) {
        char tmp[512], dst[512], dir[512], body[160];
        snprintf(tmp, sizeof tmp, "%s/logs/.bt-fail.%ld", o_.root, (long)getpid());
        snprintf(dst, sizeof dst, "%s/logs/bt-fail", o_.root);
        snprintf(dir, sizeof dir, "%s/logs", o_.root);
        int n = snprintf(body, sizeof body, "mx5dr-bt-fail-v1\nboot_id=%s\nreason=%s\n", o_.boot_id, why);
        unlink(tmp);
        int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        bool ok = fd >= 0 && write(fd, body, size_t(n)) == n;
        if (fd >= 0) { if (fsync(fd)) ok = false; if (close(fd)) ok = false; }
        if (ok) ok = rename(tmp, dst) == 0; else unlink(tmp);
        if (ok) {  // the rename itself must survive a power cut (like the guard's atomic_file)
            int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            ok = dfd >= 0 && fsync(dfd) == 0;
            if (dfd >= 0) close(dfd);
        }
        bt_row(now, "fail", ok ? why : "write_failed", (now - bds_seen_) / MS_NS);
    }
    // In-boot circuit breaker: once the bds that runs our configuration is
    // gone (crash and SM restart, args change, vanished) or never became ready,
    // this boot never opens the service PTY again, sends no PING, makes no
    // BCA query and closes its D-Bus connection, so the phone's breaker counts
    // every further session as a failure and stops reconnecting.
    void quiesce(uint64_t now, const char* why) {
        if (quiesced_) return;
        quiesced_ = true;
        close_link(now, "quiesce", 0);
        bus_close();
        bt_row(now, "quiesce", why, (now - bds_seen_) / MS_NS);
    }
    void fail_once(uint64_t now, const char* why) { if (!failed_) { failed_ = true; bt_fail(now, why); } }
    // Watched in every state for the whole boot. Only one process is the bds:
    // the oldest with our config at the first sighting, identified by (pid,
    // start time). It is GONE after two consecutive ticks whose probe says the
    // entry is missing, a zombie, another program, or another start time (pid
    // reuse); an unreadable entry for another reason (EACCES, EMFILE, ...)
    // neither counts nor resets. Other processes with our cmdline are never
    // part of the identity: the disappearance of the bds is a crash whatever
    // else is running (a real fork child is younger and its parent is the bds;
    // it never keeps a crash hidden). Once gone: quiesce at once; a process
    // with our config and another identity = restart, a bds with other args =
    // args_changed, nothing for vanish_ns = vanished (bt-fail once).
    void bds_tick(uint64_t now) {
        if (bds_state_ == BDS_STOCK || now < next_scan_) return;
        next_scan_ = now + SEC_NS;
        Seen cfg[8], stock;
        if (bds_state_ == BDS_SEARCH) {
            unsigned n = scan_bds(cfg, &stock);
            if (!n) {
                if (stock.pid) { bds_pid_ = stock.pid; bds_state_ = BDS_STOCK; bt_row(now, "stock", "args", 0); }
                return;
            }
            me_ = oldest(cfg, n); bds_pid_ = me_.pid; bds_seen_ = now; bds_state_ = BDS_CONFIG; misses_ = 0;
            bt_row(now, "config", "seen", 0);
            check_ready(now);
            return;
        }
        if (!gone_) {
            unsigned long long st; long pp;
            int k = probe(me_.pid, &st, &pp);
            if (k == P_OURS && st == me_.start) { misses_ = 0; check_ready(now); return; }
            if (k == P_UNKNOWN) return;                            // transient: no verdict
            if (k == P_OTHER_ARGS && st == me_.start) misses_ = 2;  // same process, args changed
            else if (++misses_ < 2) return;                        // gone only on the second miss
            gone_ = true;
            bds_missing_ = now;
            quiesce(now, "bds_gone");
        }
        unsigned n = scan_bds(cfg, &stock);
        for (unsigned i = 0; i < n; ++i)
            if (cfg[i].pid != me_.pid || cfg[i].start != me_.start) { bds_pid_ = cfg[i].pid; fail_once(now, "restart"); return; }
        if (stock.pid) { bds_pid_ = stock.pid; fail_once(now, "args_changed"); return; }
        if (now - bds_missing_ >= o_.vanish_ns) fail_once(now, "vanished");
    }
    void check_ready(uint64_t now) {
        if (ready_ || bds_state_ != BDS_CONFIG) return;
        struct stat st;
        if (!stat(o_.ready_flag, &st)) { ready_ = true; bt_row(now, "ready", "flag", (now - bds_seen_) / MS_NS); }
        else if (now - bds_seen_ >= o_.ready_timeout_ns) { quiesce(now, "not_ready"); fail_once(now, "not_ready"); ready_ = true; }
    }
    bool bt_active() const { return !quiesced_ && bds_state_ == BDS_CONFIG; }

    // ---- D-Bus -------------------------------------------------------------------
    // conn_: the receiver's private connection to the BCA's bus (HMI). It
    // carries only BCA traffic (GetNameOwner/NameOwnerChanged for com.jci.bca,
    // ConnectionStatusResp, QueryConnectStatus); the collector's position
    // polling keeps its own service-bus connection in the main thread. An
    // absent, refusing or silent socket fails closed: no PTY, a retry every
    // 10 s, a pimu_link bus row per distinct error name (then every 10th
    // attempt). A connect attempt is bounded (bus_register: 2 s handshake +
    // 1 s Hello, then refresh_owner 350 ms).
    void bus_close() { if (conn_) { dbus_connection_close(conn_); dbus_connection_unref(conn_); conn_ = 0; } owner_[0] = 0; }
    void bus_open(uint64_t now) {
        if (conn_ && dbus_connection_get_is_connected(conn_)) return;
        bus_close();
        if (now < next_bus_) return;
        next_bus_ = now + 10 * SEC_NS;
        DBusError e; dbus_error_init(&e);
        conn_ = dbus_connection_open_private(o_.bca_bus_address, &e);
        if (conn_ && !bus_register(&e)) { dbus_connection_close(conn_); dbus_connection_unref(conn_); conn_ = 0; }
        if (!conn_) {
            const char* name = dbus_error_is_set(&e) && e.name ? e.name : "unknown";
            if (err_row_due(name, last_bus_err_, &bus_fail_))
                link_row(now, "bus", "unavailable", 0, 0xFFFFFFFFu, 0xFFFFFFFFu, SERVICE_ID, name, dbus_error_is_set(&e) ? e.message : "", bus_fail_);
        }
        if (conn_) {
            bus_fail_ = 0; last_bus_err_[0] = 0;
            dbus_connection_set_exit_on_disconnect(conn_, 0);
            // No DBusError: dbus_bus_add_match then only queues the AddMatch
            // call and never blocks (with an error it waits up to the 25 s
            // libdbus default for a daemon that answered Hello and then went
            // silent). Its outcome was never used; the replies are popped and
            // dropped by bus_tick. refresh_owner()'s call flushes both
            // AddMatch calls first, so NameOwnerChanged is matched before the
            // owner is read.
            dbus_bus_add_match(conn_, "type='signal',interface='com.jci.bca',member='ConnectionStatusResp'", 0);
            dbus_bus_add_match(conn_, "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='com.jci.bca'", 0);
            refresh_owner();
        }
        if (dbus_error_is_set(&e)) dbus_error_free(&e);
    }
    // Bounded replacement of dbus_bus_register() (bus_register.h, shared with
    // the collector's main thread): the handshake gets 2 s in 50 ms steps with
    // the PTY drained and PINGed between the steps, Hello gets 1 s.
    struct RegisterPolicy {
        Receiver& rx;
        bool stopping() const { return rx.stop_; }
        void idle(uint64_t t) { rx.service_link(t); }
    };
    bool bus_register(DBusError* e) {
        RegisterPolicy policy = {*this};
        return mx5::collector::bounded_bus_register(conn_, e, policy, "receiver stopping");
    }
    // The PTY keeps its reader while bus_register waits (bds blocks when its
    // PTY is not drained): a non-blocking read and the PING/silence tick.
    void service_link(uint64_t now) {
        if (fd_ < 0) return;
        read_link(now);
        if (fd_ >= 0) link_tick(now);
    }
    void refresh_owner() {
        owner_[0] = 0;
        DBusMessage* m = dbus_message_new_method_call("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "GetNameOwner");
        if (!m) return;
        const char* name = "com.jci.bca";
        dbus_message_set_auto_start(m, 0);
        if (dbus_message_append_args(m, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID)) {
            DBusError e; dbus_error_init(&e);
            DBusMessage* r = dbus_connection_send_with_reply_and_block(conn_, m, 350, &e);
            const char* owner = 0;
            if (r && dbus_message_get_args(r, 0, DBUS_TYPE_STRING, &owner, DBUS_TYPE_INVALID) && owner && strlen(owner) < sizeof owner_)
                strcpy(owner_, owner);
            if (r) dbus_message_unref(r);
            dbus_error_free(&e);
        }
        dbus_message_unref(m);
    }
    // terminalPath struct (ay): bytes up to the first NUL, at most 31.
    static bool read_path(DBusMessageIter* it, char* out, size_t cap) {
        out[0] = 0;
        if (dbus_message_iter_get_arg_type(it) != DBUS_TYPE_STRUCT) return false;
        DBusMessageIter st, arr; dbus_message_iter_recurse(it, &st);
        if (dbus_message_iter_get_arg_type(&st) != DBUS_TYPE_ARRAY || dbus_message_iter_get_element_type(&st) != DBUS_TYPE_BYTE) return false;
        dbus_message_iter_recurse(&st, &arr);
        const unsigned char* bytes = 0; int n = 0;
        dbus_message_iter_get_fixed_array(&arr, &bytes, &n);
        size_t k = 0;
        while (k < size_t(n) && k + 1 < cap && bytes[k]) { out[k] = char(bytes[k]); ++k; }
        out[k] = 0;
        return true;
    }
    // BCA connStatus values (INFERENCE: the padded name table of svcjcibca.so in
    // this order: IDLE, CONNECTABLE, CONNECTING, CONNECTED, ABORTED,
    // DISCONNECTING, DISCONNECTED, FAILED, TIMEOUT). Only CONNECTED opens the
    // PTY; CONNECTING keeps the state; anything else closes our reader. A
    // wrong guess fails safe: the service is never opened, and every status
    // value is journaled for the parked check.
    // FACT (BCA_getServiceConnectStatus, validation/BCA_QUERY_CONNECT_STATUS_RE_2026-10-17.md):
    // a QueryConnectStatus reply only ever carries 3 (CONNECTED) or 6
    // (DISCONNECTED); CONNECTING (2) can come only from a signal. A reply for a
    // service missing from the BCA table is serviceId 0, connStatus 6.
    static const unsigned CS_CONNECTING = 2, CS_CONNECTED = 3;
    void handle_status(uint64_t now, const char* via, unsigned svc, unsigned cs, unsigned st, const char* path) {
        if (svc != SERVICE_ID) {
            ++sig_other_;
            // A poll reply for another service (0 = the BCA table lacks 15): one
            // row per distinct (svc, cs), at most 4 per boot; never opened.
            long long s = (long long)dbus_int32_t(svc), c = (long long)dbus_int32_t(cs);
            if (!strcmp(via, "poll") && other_rows_ < 4 && (s != other_svc_ || c != other_cs_)) {
                ++other_rows_; other_svc_ = s; other_cs_ = c;
                char saved[32]; memcpy(saved, path_, sizeof saved); path_[0] = 0;
                link_row(now, "poll", "other", 0, unsigned(c), st, s);
                memcpy(path_, saved, sizeof saved);
            }
            return;
        }
        bool valid = pts_path(path);
        if (cs != last_cs_ || st != last_st_ || strcmp(path, last_poll_path_)) {
            last_cs_ = cs; last_st_ = st; copy(last_poll_path_, sizeof last_poll_path_, path);
            char saved[32]; memcpy(saved, path_, sizeof saved);
            copy(path_, sizeof path_, path);
            link_row(now, via, cs != CS_CONNECTED ? "not_connected" : valid ? "pty" : (path[0] ? "bad_path" : "no_path"), 0, cs, st);
            memcpy(path_, saved, sizeof saved);
        }
        if (quiesced_) return;
        if (cs != CS_CONNECTED) {
            if (cs != CS_CONNECTING && fd_ >= 0 && (!path[0] || !strcmp(path, path_))) close_link(now, "bca_status", 0);
            return;
        }
        if (!valid) return;
        if (fd_ >= 0 && !strcmp(path, path_)) return;
        open_link(now, path, fd_ >= 0 ? "replace" : "connect");
    }
    void bus_tick(uint64_t now) {
        if (!bt_active()) return;
        bus_open(now);
        if (!conn_) return;
        dbus_connection_read_write(conn_, 0);
        for (unsigned k = 0; k < 64; ++k) {
            DBusMessage* m = dbus_connection_pop_message(conn_);
            if (!m) break;
            if (dbus_message_is_signal(m, "org.freedesktop.DBus", "NameOwnerChanged")) refresh_owner();
            else if (dbus_message_is_signal(m, "com.jci.bca", "ConnectionStatusResp")) {
                const char* sender = dbus_message_get_sender(m);
                if (!owner_[0]) refresh_owner();
                if (!sender || !owner_[0] || strcmp(sender, owner_)) ++unexpected_;
                else {
                    DBusMessageIter it; dbus_uint32_t v[4]; char path[32]; bool ok = dbus_message_iter_init(m, &it);
                    for (int i = 0; ok && i < 4; ++i) {
                        ok = dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_UINT32;
                        if (ok) { dbus_message_iter_get_basic(&it, &v[i]); ok = dbus_message_iter_next(&it); }
                    }
                    if (ok && read_path(&it, path, sizeof path)) {
                        if (v[0] == SERVICE_ID) { query_period_ = QUERY_NS; if (next_query_ > now + QUERY_NS) next_query_ = now + QUERY_NS; }
                        handle_status(now, "signal", v[0], v[1], v[3], path);
                    }
                    else ++unexpected_;
                }
            }
            dbus_message_unref(m);
        }
        if (fd_ < 0 && now >= next_query_) query(now);
    }
    // QueryConnectStatus is a call into the stock BCA: every 5 s until the
    // first PTY of the boot was opened, then 5 s after a disconnect doubling
    // to 60 s while nothing connects; a signal for our
    // service resets it to 5 s (signals are the primary path, polling only the
    // fallback for a missed or never-broadcast signal).
    void query(uint64_t now) {
        next_query_ = now + query_period_;
        // Until the first PTY of this boot is open the phone waits only 15 s
        // for a first PING: keep 5 s; afterwards back off to 60 s.
        if (opened_once_) query_period_ = query_period_ * 2 > QUERY_MAX_NS ? QUERY_MAX_NS : query_period_ * 2;
        DBusMessage* m = dbus_message_new_method_call("com.jci.bca", "/com/jci/bca", "com.jci.bca", "QueryConnectStatus");
        if (!m) return;
        dbus_message_set_auto_start(m, 0);
        dbus_uint32_t svc = SERVICE_ID;
        DBusError e; dbus_error_init(&e);
        DBusMessage* r = 0;
        if (dbus_message_append_args(m, DBUS_TYPE_UINT32, &svc, DBUS_TYPE_INVALID))
            r = dbus_connection_send_with_reply_and_block(conn_, m, 350, &e);  // the stock handler is synchronous (52 ms offline); keep <= 1 s
        dbus_message_unref(m);
        bool ok = false;
        if (r) {
            DBusMessageIter it, st; dbus_int32_t v[4]; char path[32];
            if (dbus_message_iter_init(r, &it) && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_STRUCT) {
                dbus_message_iter_recurse(&it, &st); ok = true;
                for (int i = 0; ok && i < 4; ++i) {
                    ok = dbus_message_iter_get_arg_type(&st) == DBUS_TYPE_INT32;
                    if (ok) { dbus_message_iter_get_basic(&st, &v[i]); ok = dbus_message_iter_next(&st); }
                }
                if (ok) ok = read_path(&st, path, sizeof path);
                if (ok) { query_fail_ = 0; last_query_err_[0] = 0; handle_status(now, "poll", unsigned(v[0]), unsigned(v[1]), unsigned(v[3]), path); }
            }
            dbus_message_unref(r);
        }
        if (!ok) {
            // The D-Bus error name tells the cause apart (UnknownMethod with
            // signature "u" = wrong bus/no object, NoReply = timeout,
            // ServiceUnknown/NameHasNoOwner = no BCA on this bus, Failed = BCA
            // internal); a reply of the wrong shape is "signature".
            bool is_err = dbus_error_is_set(&e);
            const char* name = is_err && e.name ? e.name : "signature";
            if (err_row_due(name, last_query_err_, &query_fail_))
                link_row(now, "poll_error", is_err ? "error" : "signature", 0, 0xFFFFFFFFu, 0xFFFFFFFFu, SERVICE_ID,
                         name, is_err && e.message ? e.message : "", query_fail_);
        }
        dbus_error_free(&e);
    }

    // ---- link --------------------------------------------------------------------
    int open_slave(const char* path, int* err) {
#ifndef MX5_COLLECTOR_TESTING
        // The slave belongs to uid 0 with devpts mode=600 (vehicle /proc/mounts).
        // CAP_DAC_OVERRIDE is effective only for this open (permitted set only otherwise).
        if (o_.raise_cap) set_cap(true);
#endif
#ifdef MX5_COLLECTOR_TESTING
        // Test hook right before the open syscall: any reader closed before
        // this point leaves the PTY without a reader for the delay, which the
        // test sees as a hangup/EIO on the master side.
        if (getenv("MX5_TEST_REOPEN_DELAY_MS")) usleep(1000u * unsigned(atoi(getenv("MX5_TEST_REOPEN_DELAY_MS"))));
#endif
        int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
        *err = fd < 0 ? errno : 0;
#ifndef MX5_COLLECTOR_TESTING
        if (o_.raise_cap) set_cap(false);
#endif
        if (fd < 0) return -1;
        struct stat st;
        // Unix98 pty slaves: character devices with majors 136..143.
        unsigned major = fstat(fd, &st) ? 0 : unsigned((st.st_rdev >> 8) & 0xfff);
        if (major < 136 || major > 143 || !S_ISCHR(st.st_mode) || st.st_uid != o_.pty_owner) { close(fd); *err = -1; return -1; }
        return fd;
    }
#ifndef MX5_COLLECTOR_TESTING
    static void set_cap(bool on) {
        struct __user_cap_header_struct h; struct __user_cap_data_struct d[2];
        memset(&h, 0, sizeof h); memset(d, 0, sizeof d);
        h.version = _LINUX_CAPABILITY_VERSION_3; h.pid = 0;
        d[0].permitted = 1u << CAP_DAC_OVERRIDE;
        d[0].effective = on ? 1u << CAP_DAC_OVERRIDE : 0;
        syscall(SYS_capset, &h, d);
    }
#endif
    void open_link(uint64_t now, const char* path, const char* ev) {
        if (quiesced_) return;
        int err = 0, fd = open_slave(path, &err);
        char saved[32]; memcpy(saved, path_, sizeof saved);
        copy(path_, sizeof path_, path);
        if (fd < 0) { link_row(now, "open_fail", err == -1 ? "not_pty" : "open", err, 0xFFFFFFFFu, 0xFFFFFFFFu); memcpy(path_, saved, sizeof saved); return; }
        struct termios t; char why[24] = "raw";
        if (!tcgetattr(fd, &t)) {
            snprintf(why, sizeof why, "lflag_%x", unsigned(t.c_lflag) & 0xffffu);
            cfmakeraw(&t);
            if (tcsetattr(fd, TCSANOW, &t)) strcpy(why, "raw_failed");
        } else strcpy(why, "no_termios");
        // A replaced reader is closed only after the new one is open: the
        // stock bds never sees a moment without a reader of our service.
        if (fd_ >= 0) { finish_rows(now); close(fd_); }
        opened_once_ = true;
        fd_ = fd; last_frame_ = now; dec_ = Decoder(); memset(&noted_, 0, sizeof noted_); pinger_.reset(); pending_out_ = 0;
        link_row(now, ev, why, 0, 0xFFFFFFFFu, 0xFFFFFFFFu);
        agg_.start(now);
    }
    void finish_rows(uint64_t now) {
        char row[ROW];
        flush_counts(0);
        if (agg_.close(now, row, sizeof row)) emit(R_PIMU, now, row);
    }
    void close_link(uint64_t now, const char* why, int err) {
        if (fd_ < 0) return;
        finish_rows(now);
        close(fd_); fd_ = -1;
        link_row(now, "disconnect", why, err, 0xFFFFFFFFu, 0xFFFFFFFFu);
        path_[0] = 0; last_cs_ = last_st_ = 0xFFFFFFFFu; last_poll_path_[0] = 0;
        next_query_ = now + QUERY_NS; query_period_ = QUERY_NS;
    }
    void flush_counts(uint64_t dropped) {
        Decoder::Counts d = dec_.counts;
        // A bad_type frame is a whole CRC-valid frame: counted with the version errors.
        Decoder::Counts delta = { d.crc_error - noted_.crc_error, (d.bad_version - noted_.bad_version) + (d.bad_type - noted_.bad_type), 0,
                                  d.skipped - noted_.skipped, 0, 0 };
        noted_ = d;
        agg_.note(delta, dropped);
    }
    void handle_sync(uint64_t now, const Sync& s) {
        int64_t rtt, off;
        if (s.type != SYNC_PONG) { ++unexpected_; return; }
        if (!pinger_.pong(s, now, &rtt, &off)) { ++unmatched_; return; }
        char row[ROW];
        snprintf(row, sizeof row, "{\"kind\":\"pimu_sync\",\"m\":%llu,\"q\":%u,\"rtt_us\":%lld,\"off_us\":%lld}",
                 (unsigned long long)(now / MS_NS), s.seq, (long long)(rtt / 1000), (long long)(off / 1000));
        emit(R_SYNC, now, row);
    }
    void feed(uint64_t now, const uint8_t* p, size_t n) {
        char row[ROW];
        while (n) {
            size_t k = dec_.feed(p, n); p += k; n -= k;
            Data d; Sync s;
            for (;;) {
                Decoder::Event ev = dec_.next(&d, &s);
                if (ev == Decoder::NONE) break;
                last_frame_ = now;
                if (agg_.advance(now, row, sizeof row)) emit(R_PIMU, now, row);
                if (ev == Decoder::DATA) { if (d.version == VERSION) agg_.add(now, d); }
                else handle_sync(now, s);
            }
        }
        flush_counts(0);
    }
    void read_link(uint64_t now) {
        uint8_t buf[4096];
        size_t total = 0;
        if (now - sec_start_ >= SEC_NS) { sec_start_ = now; sec_bytes_ = 0; }
        while (total < READ_BUDGET) {
            ssize_t r = read(fd_, buf, sizeof buf);
            if (r < 0 && errno == EINTR) continue;
            if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
            if (r <= 0) { close_link(now, r == 0 ? "eof" : "read_error", r == 0 ? 0 : errno); return; }
            total += size_t(r);
            size_t keep = sec_bytes_ >= OVERLOAD_BPS ? 0 : OVERLOAD_BPS - sec_bytes_;
            if (keep > size_t(r)) keep = size_t(r);
            sec_bytes_ += size_t(r);
            if (keep) feed(now, buf, keep);
            if (size_t(r) > keep) {  // overload: read and discard, never stop reading
                dec_.drop_partial();
                flush_counts(size_t(r) - keep);
            }
        }
    }
    void write_ping(uint64_t now) {
        if (pending_out_) {
            ssize_t w = write(fd_, out_ + (SYNC_LEN - pending_out_), pending_out_);
            if (w > 0) pending_out_ -= size_t(w);
            return;
        }
        if (quiesced_ || !pinger_.due(now)) return;
        pinger_.make(now, out_);
        ssize_t w = write(fd_, out_, SYNC_LEN);
        if (w < 0) { ++ping_drop_; return; }
        pending_out_ = SYNC_LEN - size_t(w);
    }
    void link_tick(uint64_t now) {
        if (fd_ < 0) return;
        char row[ROW];
        if (agg_.advance(now, row, sizeof row)) emit(R_PIMU, now, row);
        if (now - last_frame_ >= SILENCE_NS) {
            // Spec: reconnect after 10 s of silence. The CMU cannot redial the
            // phone; it reopens its reader of the same PTY (new fd first, so
            // bds is never without a reader) and waits for frames or a new path.
            char p[32]; strcpy(p, path_);
            open_link(now, p, "silence");  // on failure the old reader stays open
            last_frame_ = now;
            return;
        }
        write_ping(now);
    }

    void run() {
        boot_row();
        while (!stop_) {
            uint64_t now = mono_ns();
            bds_tick(now);
            bus_tick(now);
            if (fd_ >= 0) {
                struct pollfd p; p.fd = fd_; p.events = POLLIN; p.revents = 0;
                int r = poll(&p, 1, 50);
                now = mono_ns();
                if (r > 0 && (p.revents & POLLIN)) read_link(now);
                if (fd_ >= 0 && r > 0 && (p.revents & (POLLHUP | POLLERR | POLLNVAL)) && !(p.revents & POLLIN)) close_link(now, "hangup", 0);
                link_tick(now);
            } else {
                struct timespec t = {0, 50000000};
                nanosleep(&t, 0);
            }
            maybe_drop_row(now, false);
        }
        uint64_t now = mono_ns();
        close_link(now, "stop", 0);
        maybe_drop_row(now, true);
        bus_close();
    }
    void boot_row() {
        char row[ROW];
        snprintf(row, sizeof row, "{\"kind\":\"pimu_boot\",\"schema\":1,\"m\":%llu,\"boot_id\":\"%s\",\"svc\":%u,\"budget_bph\":%llu,\"cap\":%d}",
                 (unsigned long long)(mono_ns() / MS_NS), o_.boot_id, SERVICE_ID, (unsigned long long)budget_bph(), o_.raise_cap ? 1 : 0);
        push(row);
    }

    RxOptions o_;
    volatile bool stop_;
    bool started_;
    pthread_t thread_;
    pthread_mutex_t mu_;
    char ring_[RING][ROW];
    unsigned long long head_, tail_, ring_drop_;
    Bucket buckets_[R_KINDS], err_bucket_;
    unsigned long long rate_drop_[R_KINDS], sig_other_, unexpected_, ping_drop_, unmatched_, drop_sig_;
    uint64_t last_drop_row_;
    BdsState bds_state_; long bds_pid_; uint64_t bds_seen_, bds_missing_, next_scan_; bool ready_, quiesced_, failed_, opened_once_;
    Seen me_; unsigned misses_; bool gone_; uint64_t query_period_;
    int fd_; char path_[32]; uint64_t last_frame_, sec_start_; size_t sec_bytes_;
    Decoder dec_; Decoder::Counts noted_; Aggregator agg_; Pinger pinger_;
    uint8_t out_[SYNC_LEN]; size_t pending_out_;
    DBusConnection* conn_; char owner_[256]; uint64_t next_query_, next_bus_; unsigned query_fail_;
    unsigned last_cs_, last_st_; char last_poll_path_[32];
    unsigned bus_fail_; char last_bus_err_[ERR_NAME + 1], last_query_err_[ERR_NAME + 1];
    unsigned other_rows_; long long other_svc_, other_cs_;
};

}}  // namespace mx5::pimu
#endif
