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
static const uint64_t DROP_PERIOD_NS = 60 * SEC_NS;
// Longest row of each kind, by construction (numbers are clamped, strings are
// bounded); tests/collector/test_phone_imu.cpp formats the extremes.
static const size_t MAX_SYNC_ROW = 128, MAX_LINK_ROW = 200, MAX_BT_ROW = 128, MAX_DROP_ROW = 220, MAX_BOOT_ROW = 160;
inline double budget_bph() {
    Bucket b[R_KINDS]; init_buckets(b);
    return b[R_PIMU].max_rows_per_hour() * double(Aggregator::max_row_bytes()) +
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
    const char* bus_address;
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
        init_buckets(buckets_);
        memset(rate_drop_, 0, sizeof rate_drop_);
        memset(path_, 0, sizeof path_); memset(owner_, 0, sizeof owner_);
        sig_other_ = unexpected_ = ping_drop_ = unmatched_ = 0; last_drop_row_ = 0; drop_sig_ = 0;
        bds_state_ = BDS_SEARCH; bds_pid_ = 0; bds_seen_ = bds_missing_ = 0; ready_ = false; next_scan_ = 0;
        quiesced_ = false; cand_pid_ = 0; cand_kind_ = -1; query_period_ = QUERY_NS;
        next_query_ = 0; next_bus_ = 0; query_fail_ = 0; last_frame_ = 0; sec_start_ = 0; sec_bytes_ = 0; pending_out_ = 0;
        memset(&noted_, 0, sizeof noted_);
        last_cs_ = last_st_ = 0xFFFFFFFFu; last_poll_path_[0] = 0;
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
    enum BdsState { BDS_SEARCH, BDS_CONFIG, BDS_STOCK, BDS_FAILED };
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
    void link_row(uint64_t now, const char* ev, const char* why, int err, unsigned cs, unsigned st) {
        char p[24], w[24], row[ROW];
        clean(path_, p, sizeof p); clean(why ? why : "", w, sizeof w);
        snprintf(row, sizeof row, "{\"kind\":\"pimu_link\",\"m\":%llu,\"ev\":\"%s\",\"why\":\"%s\",\"path\":\"%s\",\"svc\":%u,\"cs\":%lld,\"st\":%lld,\"err\":%d}",
                 (unsigned long long)(now / MS_NS), ev, w, p, SERVICE_ID, cs == 0xFFFFFFFFu ? -1LL : (long long)cs,
                 st == 0xFFFFFFFFu ? -1LL : (long long)st, err);
        emit(R_LINK, now, row);
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
    // 1 = bds with our config, 0 = bds with other args, -1 = not bds/unreadable.
    int bds_cmdline(long pid) {
        char p[256], buf[1024];
        snprintf(p, sizeof p, "%s/%ld/cmdline", o_.proc_root, pid);
        int fd = open(p, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
        if (fd < 0) return -1;
        ssize_t n = read(fd, buf, sizeof buf - 1);
        close(fd);
        if (n <= 0) return -1;
        buf[n] = 0;
        const char* base = strrchr(buf, '/');
        if (strcmp(base ? base + 1 : buf, "bds")) return -1;
        const char* end = buf + n;
        bool flag = false;
        for (const char* a = buf + strlen(buf) + 1; a < end; a += strlen(a) + 1) {
            if (flag) return strcmp(a, o_.xml_path) ? 0 : 1;
            flag = !strcmp(a, "-configFilePath");
        }
        return 0;
    }
    // Start time (clock ticks since boot, /proc/<pid>/stat field 22); 0 when unknown.
    unsigned long long start_ticks(long pid) {
        char p[256], buf[1024];
        snprintf(p, sizeof p, "%s/%ld/stat", o_.proc_root, pid);
        int fd = open(p, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
        if (fd < 0) return 0;
        ssize_t n = read(fd, buf, sizeof buf - 1);
        close(fd);
        if (n <= 0) return 0;
        buf[n] = 0;
        const char* q = strrchr(buf, ')');
        if (!q) return 0;
        unsigned field = 2;  // q points at the end of field 2 (comm)
        for (++q; *q && field < 22; ++q) if (*q == ' ') ++field;
        return field == 22 ? strtoull(q, 0, 10) : 0;
    }
    // The bds process: our-config processes win over others; among several,
    // the oldest (a fork of bds running system() has the same cmdline for a
    // moment before its exec, and is younger).
    long scan_bds(int* kind) {
        DIR* d = opendir(o_.proc_root);
        long found = 0; *kind = -1;
        unsigned long long best = 0;
        if (!d) return 0;
        for (unsigned seen = 0; seen < 8192; ++seen) {
            struct dirent* e = readdir(d);
            if (!e) break;
            const char* n = e->d_name;
            if (!n[0] || strspn(n, "0123456789") != strlen(n) || strlen(n) > 9) continue;
            long pid = atol(n);
            int k = bds_cmdline(pid);
            if (k < 0 || k < *kind) continue;
            unsigned long long st = start_ticks(pid);
            if (k > *kind || (st ? st : ~0ULL) < (best ? best : ~0ULL) || (st == best && pid < found)) {
                *kind = k; found = pid; best = st;
            }
        }
        closedir(d);
        return found;
    }
    void bt_fail(uint64_t now, const char* why) {
        bds_state_ = BDS_FAILED;
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
    // gone (crash and SM restart, args change, vanished), this boot never
    // opens the service PTY again and sends no PING, so the phone's breaker
    // counts every further session as a failure and stops reconnecting.
    void quiesce(uint64_t now, const char* why) {
        if (quiesced_) return;
        quiesced_ = true;
        close_link(now, "quiesce", 0);
        bt_row(now, "quiesce", why, (now - bds_seen_) / MS_NS);
    }
    // A pid becomes the bds only after two consecutive scans (1 s apart) saw it.
    bool confirmed(long pid, int kind) {
        bool same = pid && pid == cand_pid_ && kind == cand_kind_;
        cand_pid_ = pid; cand_kind_ = kind;
        return same;
    }
    void bds_tick(uint64_t now) {
        if (bds_state_ == BDS_STOCK || bds_state_ == BDS_FAILED || now < next_scan_) return;
        next_scan_ = now + SEC_NS;
        if (bds_state_ == BDS_SEARCH) {
            int kind; long pid = scan_bds(&kind);
            if (kind < 0 || !confirmed(pid, kind)) return;
            bds_pid_ = pid;
            if (kind == 0) { bds_state_ = BDS_STOCK; bt_row(now, "stock", "args", 0); return; }
            bds_seen_ = now; bds_missing_ = 0; bds_state_ = BDS_CONFIG; cand_pid_ = 0;
            bt_row(now, "config", "seen", 0);
        }
        // BDS_CONFIG
        if (bds_cmdline(bds_pid_) != 1) {
            quiesce(now, "bds_gone");
            if (!bds_missing_) bds_missing_ = now;
            int kind; long pid = scan_bds(&kind);
            if (kind >= 0 && pid != bds_pid_ && confirmed(pid, kind)) {
                bds_pid_ = pid; bt_fail(now, kind == 1 ? "restart" : "args_changed"); return;
            }
            if (kind < 0) cand_pid_ = 0;
            if (now - bds_missing_ >= o_.vanish_ns) bt_fail(now, "vanished");
            return;
        }
        bds_missing_ = 0; cand_pid_ = 0;
        if (!ready_) {
            struct stat st;
            if (!stat(o_.ready_flag, &st)) { ready_ = true; bt_row(now, "ready", "flag", (now - bds_seen_) / MS_NS); }
            else if (now - bds_seen_ >= o_.ready_timeout_ns) bt_fail(now, "not_ready");
        }
    }
    bool bt_active() const { return !quiesced_ && (bds_state_ == BDS_CONFIG || (bds_state_ == BDS_FAILED && bds_pid_)); }

    // ---- D-Bus -------------------------------------------------------------------
    void bus_close() { if (conn_) { dbus_connection_close(conn_); dbus_connection_unref(conn_); conn_ = 0; } owner_[0] = 0; }
    void bus_open(uint64_t now) {
        if (conn_ && dbus_connection_get_is_connected(conn_)) return;
        bus_close();
        if (now < next_bus_) return;
        next_bus_ = now + 10 * SEC_NS;
        DBusError e; dbus_error_init(&e);
        conn_ = dbus_connection_open_private(o_.bus_address, &e);
        if (conn_ && !dbus_bus_register(conn_, &e)) { dbus_connection_close(conn_); dbus_connection_unref(conn_); conn_ = 0; }
        if (conn_) {
            dbus_connection_set_exit_on_disconnect(conn_, 0);
            dbus_bus_add_match(conn_, "type='signal',interface='com.jci.bca',member='ConnectionStatusResp'", &e);
            if (dbus_error_is_set(&e)) { dbus_error_free(&e); dbus_error_init(&e); }
            dbus_bus_add_match(conn_, "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='com.jci.bca'", &e);
            refresh_owner();
        }
        if (dbus_error_is_set(&e)) dbus_error_free(&e);
        if (!conn_) link_row(now, "bus", "unavailable", 0, 0xFFFFFFFFu, 0xFFFFFFFFu);
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
    static const unsigned CS_CONNECTING = 2, CS_CONNECTED = 3;
    void handle_status(uint64_t now, const char* via, unsigned svc, unsigned cs, unsigned st, const char* path) {
        if (svc != SERVICE_ID) { ++sig_other_; return; }
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
    // QueryConnectStatus is a call into the stock BCA: 5 s after start or a
    // disconnect, doubling to 60 s while nothing connects; a signal for our
    // service resets it to 5 s (signals are the primary path, polling only the
    // fallback for a missed or never-broadcast signal).
    void query(uint64_t now) {
        next_query_ = now + query_period_;
        query_period_ = query_period_ * 2 > QUERY_MAX_NS ? QUERY_MAX_NS : query_period_ * 2;
        DBusMessage* m = dbus_message_new_method_call("com.jci.bca", "/com/jci/bca", "com.jci.bca", "QueryConnectStatus");
        if (!m) return;
        dbus_message_set_auto_start(m, 0);
        dbus_uint32_t svc = SERVICE_ID;
        DBusError e; dbus_error_init(&e);
        DBusMessage* r = 0;
        if (dbus_message_append_args(m, DBUS_TYPE_UINT32, &svc, DBUS_TYPE_INVALID))
            r = dbus_connection_send_with_reply_and_block(conn_, m, 350, &e);
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
                if (ok) { query_fail_ = 0; handle_status(now, "poll", unsigned(v[0]), unsigned(v[1]), unsigned(v[3]), path); }
            }
            dbus_message_unref(r);
        }
        if (!ok) {
            if (query_fail_ < 1000) ++query_fail_;
            if (query_fail_ == 1 || query_fail_ == 3) link_row(now, "poll_error", dbus_error_is_set(&e) ? "error" : "signature", 0, 0xFFFFFFFFu, 0xFFFFFFFFu);
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
#ifdef MX5_COLLECTOR_TESTING
        // Test hook: hold the old reader open for a while before the new open,
        // so a reordered close is visible as a hangup on the PTY master.
        if (fd_ >= 0 && getenv("MX5_TEST_REOPEN_DELAY_MS")) usleep(1000u * unsigned(atoi(getenv("MX5_TEST_REOPEN_DELAY_MS"))));
#endif
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
    Bucket buckets_[R_KINDS];
    unsigned long long rate_drop_[R_KINDS], sig_other_, unexpected_, ping_drop_, unmatched_, drop_sig_;
    uint64_t last_drop_row_;
    BdsState bds_state_; long bds_pid_; uint64_t bds_seen_, bds_missing_, next_scan_; bool ready_, quiesced_;
    long cand_pid_; int cand_kind_; uint64_t query_period_;
    int fd_; char path_[32]; uint64_t last_frame_, sec_start_; size_t sec_bytes_;
    Decoder dec_; Decoder::Counts noted_; Aggregator agg_; Pinger pinger_;
    uint8_t out_[SYNC_LEN]; size_t pending_out_;
    DBusConnection* conn_; char owner_[256]; uint64_t next_query_, next_bus_; unsigned query_fail_;
    unsigned last_cs_, last_st_; char last_poll_path_[32];
};

}}  // namespace mx5::pimu
#endif
