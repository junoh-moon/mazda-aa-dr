// Phone IMU channel v1, CMU side (stage 1: RECORD-ONLY).
// Protocol: validation/PHONE_IMU_CHANNEL_2026-10-14.md (shared spec, verbatim).
// The phone is the RFCOMM client of the serial service 'MX5DR IMU'; the stock
// bds daemon exposes the link as a PTY. This header holds the wire format,
// the resynchronising stream decoder, the 1 Hz aggregation into compact rows
// and the token-bucket row budget. No I/O, no allocation, no clocks: the
// receiver (phone_imu_rx.cpp) feeds bytes and CLOCK_MONOTONIC times.
// Nothing here is read by the product: the dead reckoning keeps using the
// car's own yaw (0x078); these rows are only journaled.
#ifndef MX5_COLLECTOR_PHONE_IMU_H
#define MX5_COLLECTOR_PHONE_IMU_H
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace mx5 { namespace pimu {

static const unsigned DATA_LEN = 32, SYNC_LEN = 28, MAX_FRAME = 32;
static const uint8_t VERSION = 1;
static const uint8_t FLAG_BIAS_VALID = 1, FLAG_STATIONARY = 2, FLAG_GRAVITY_VALID = 4, FLAG_FRAME_RESET = 8;
static const uint8_t SYNC_PING = 0, SYNC_PONG = 1;

// CRC-32 (IEEE 802.3, reflected 0xEDB88320, init/xorout 0xFFFFFFFF) = zlib crc32.
inline uint32_t crc32(const uint8_t* p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return c ^ 0xFFFFFFFFu;
}
inline uint32_t get_u32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
inline uint64_t get_u64(const uint8_t* p) { return uint64_t(get_u32(p)) | uint64_t(get_u32(p + 4)) << 32; }
inline void put_u32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24); }
inline void put_u64(uint8_t* p, uint64_t v) { put_u32(p, uint32_t(v)); put_u32(p + 4, uint32_t(v >> 32)); }

struct Data {
    uint8_t version, flags;
    uint32_t seq;
    int64_t t_end_ns;
    int32_t dtheta_urad;
    uint32_t window_us;
    int32_t bias_urad_s;
    uint32_t crc;
};
struct Sync {
    uint8_t version, type;
    uint32_t seq;
    int64_t t_cmu_ns, t_phone_ns;
    uint32_t crc;
};

inline void encode_data(const Data& d, uint8_t out[DATA_LEN]) {
    out[0] = 'M'; out[1] = 'X'; out[2] = d.version; out[3] = d.flags;
    put_u32(out + 4, d.seq); put_u64(out + 8, uint64_t(d.t_end_ns));
    put_u32(out + 16, uint32_t(d.dtheta_urad)); put_u32(out + 20, d.window_us);
    put_u32(out + 24, uint32_t(d.bias_urad_s)); put_u32(out + 28, crc32(out, 28));
}
inline void encode_sync(const Sync& s, uint8_t out[SYNC_LEN]) {
    out[0] = 'M'; out[1] = 'P'; out[2] = s.version; out[3] = s.type;
    put_u32(out + 4, s.seq); put_u64(out + 8, uint64_t(s.t_cmu_ns)); put_u64(out + 16, uint64_t(s.t_phone_ns));
    put_u32(out + 24, crc32(out, 24));
}
inline Data decode_data(const uint8_t* p) {
    Data d;
    d.version = p[2]; d.flags = p[3]; d.seq = get_u32(p + 4); d.t_end_ns = int64_t(get_u64(p + 8));
    d.dtheta_urad = int32_t(get_u32(p + 16)); d.window_us = get_u32(p + 20);
    d.bias_urad_s = int32_t(get_u32(p + 24)); d.crc = get_u32(p + 28);
    return d;
}
inline Sync decode_sync(const uint8_t* p) {
    Sync s;
    s.version = p[2]; s.type = p[3]; s.seq = get_u32(p + 4);
    s.t_cmu_ns = int64_t(get_u64(p + 8)); s.t_phone_ns = int64_t(get_u64(p + 16)); s.crc = get_u32(p + 24);
    return s;
}

// Resynchronising decoder. A candidate starts at 'M' followed by 'X' (data)
// or 'P' (ping/pong). Version != 1: counted as bad_version, the two magic
// bytes are skipped (the length of another version is unknown). Version 1
// with a CRC mismatch: counted as crc_error, one byte is skipped and the
// search resumes. A CRC-valid MP frame of another type is counted as
// bad_type and dropped whole. Every byte that is not part of an accepted frame is counted
// in `skipped`. RFCOMM is ordered and reliable, so a bad CRC means corrupt or
// foreign bytes, never reordering.
class Decoder {
public:
    enum Event { NONE = 0, DATA = 1, SYNC = 2 };
    struct Counts { uint64_t crc_error, bad_version, bad_type, skipped, frames, syncs; };
    Decoder() { reset(); memset(&counts, 0, sizeof counts); }
    void reset() { n_ = 0; }  // drop partial bytes (counted as skipped)
    void drop_partial() { counts.skipped += n_; n_ = 0; }
    // Appends at most `len` bytes; returns how many were consumed. Call next()
    // until NONE after each feed.
    size_t feed(const uint8_t* p, size_t len) {
        size_t room = sizeof buf_ - n_, take = len < room ? len : room;
        memcpy(buf_ + n_, p, take); n_ += take;
        return take;
    }
    Event next(Data* data, Sync* sync) {
        for (;;) {
            size_t i = 0;
            while (i < n_ && buf_[i] != 'M') ++i;
            consume(i, true);
            if (n_ < 2) return NONE;
            if (buf_[1] != 'X' && buf_[1] != 'P') { consume(1, true); continue; }
            if (n_ < 3) return NONE;
            if (buf_[2] != VERSION) { ++counts.bad_version; consume(2, true); continue; }
            const unsigned len = buf_[1] == 'X' ? DATA_LEN : SYNC_LEN;
            if (n_ < len) return NONE;
            if (crc32(buf_, len - 4) != get_u32(buf_ + len - 4)) { ++counts.crc_error; consume(1, true); continue; }
            if (len == DATA_LEN) { *data = decode_data(buf_); ++counts.frames; consume(len, false); return DATA; }
            if (buf_[3] != SYNC_PING && buf_[3] != SYNC_PONG) { ++counts.bad_type; consume(len, false); continue; }
            *sync = decode_sync(buf_); ++counts.syncs; consume(len, false); return SYNC;
        }
    }
    Counts counts;
private:
    void consume(size_t k, bool skipped) {
        if (skipped) counts.skipped += k;
        memmove(buf_, buf_ + k, n_ - k); n_ -= k;
    }
    uint8_t buf_[256];
    size_t n_;
};

// Token bucket in milli-tokens (beta.14 discipline: a limited row is dropped
// and counted, never queued).
struct Bucket {
    uint64_t refill_ns, last_ns; unsigned burst, milli; bool started;
    void init(uint64_t refill, unsigned b) { refill_ns = refill; burst = b; milli = b * 1000u; last_ns = 0; started = false; }
    bool take(uint64_t now) {
        if (!started) { started = true; last_ns = now; }
        if (now > last_ns) {
            uint64_t add = (now - last_ns) / (refill_ns / 1000ULL);
            if (add) {
                uint64_t m = milli + add; milli = unsigned(m > burst * 1000ULL ? burst * 1000ULL : m);
                last_ns += add * (refill_ns / 1000ULL);
            }
        }
        if (milli < 1000u) return false;
        milli -= 1000u; return true;
    }
    // Rows per hour this bucket can ever pass.
    double max_rows_per_hour() const { return 3600.0e9 / double(refill_ns) + burst; }
};

inline long long clamp_ll(long long v, long long lo, long long hi) { return v < lo ? lo : v > hi ? hi : v; }

// One-second bins by CMU receipt time (CLOCK_MONOTONIC), ten bins per row.
// Row bytes are bounded by clamping every printed number (see max_row_bytes).
class Aggregator {
public:
    static const uint64_t SEC = 1000000000ULL, MS = 1000000ULL;
    static const unsigned BINS = 10;
    struct Bin { long long n, dth, wu, gap; unsigned fl; };
    Aggregator() { reset(); }
    void reset() { open_ = false; nb_ = 0; have_seq_ = false; memset(bins_, 0, sizeof bins_); ce_ = bv_ = sk_ = dr_ = fr_ = 0; bias_ = 0; pt_ = 0; s0_ = s1_ = 0; }
    bool open() const { return open_; }
    // Starts the row clock at `now` (link connect) if no row is open.
    void start(uint64_t now) { if (!open_) { open_ = true; t0_ = now; nb_ = 0; memset(bins_, 0, sizeof bins_); } }
    // Index of the bin for `now`; flushes into `row` when the 10 s row is full.
    // Returns true when a row was produced (the caller writes it).
    bool advance(uint64_t now, char* row, size_t cap) {
        if (!open_ || now < t0_) return false;
        uint64_t idx = (now - t0_) / SEC;
        if (idx < BINS) { if (idx + 1 > nb_) nb_ = unsigned(idx + 1); return false; }
        nb_ = BINS;
        format(row, cap);
        uint64_t rows = idx / BINS;  // whole empty rows are skipped, not printed
        t0_ += rows * BINS * SEC;
        nb_ = unsigned(idx - rows * BINS) + 1;
        memset(bins_, 0, sizeof bins_); have_seq_ = false; ce_ = bv_ = sk_ = dr_ = fr_ = 0;
        return true;
    }
    // Final partial row (link close or stop). Returns false when nothing is open.
    bool close(uint64_t now, char* row, size_t cap) {
        if (!open_) return false;
        if (now >= t0_) { uint64_t idx = (now - t0_) / SEC; nb_ = unsigned(idx + 1 > BINS ? BINS : idx + 1); }
        if (!nb_) nb_ = 1;
        format(row, cap); open_ = false; have_seq_ = false; ce_ = bv_ = sk_ = dr_ = fr_ = 0;
        return true;
    }
    void add(uint64_t now, const Data& d) {
        Bin* b = bin(now); if (!b) return;
        if (have_seq_ && !(d.flags & FLAG_FRAME_RESET)) {
            uint32_t step = d.seq - s1_;  // wraps
            if (step > 1 && step < 0x80000000u) b->gap += step - 1;
        } else if (!have_seq_) s0_ = d.seq;
        if (d.flags & FLAG_FRAME_RESET) ++fr_;
        have_seq_ = true; s1_ = d.seq;
        b->n++; b->dth += d.dtheta_urad; b->wu += d.window_us; b->fl |= d.flags;
        bias_ = d.bias_urad_s; pt_ = d.t_end_ns;
    }
    void note(const Decoder::Counts& delta, uint64_t dropped) { ce_ += delta.crc_error; bv_ += delta.bad_version; sk_ += delta.skipped; dr_ += dropped; }
    // Worst case of one row: every number at its clamp limit.
    static size_t max_row_bytes() {
        Aggregator a; a.open_ = true; a.t0_ = UINT64_MAX / 2; a.nb_ = BINS; a.have_seq_ = true;
        a.s0_ = a.s1_ = 4294967295u; a.bias_ = -2147483647 - 1; a.pt_ = -9223372036854775807LL - 1;
        a.ce_ = a.bv_ = a.sk_ = a.dr_ = a.fr_ = ~0ULL;
        for (unsigned i = 0; i < BINS; ++i) { a.bins_[i].n = ~0ULL >> 1; a.bins_[i].dth = -(1LL << 62); a.bins_[i].wu = 1LL << 62; a.bins_[i].gap = 1LL << 62; a.bins_[i].fl = 255; }
        char row[1024]; a.format(row, sizeof row); return strlen(row) + 1;
    }
private:
    Bin* bin(uint64_t now) {
        if (!open_ || now < t0_) return 0;
        uint64_t idx = (now - t0_) / SEC;
        return idx < BINS ? &bins_[idx] : 0;
    }
    void format(char* row, size_t cap) {
        static const long long C = 999999LL, DTH = 99999999999LL, BIG = 9999999999LL;
        int k = snprintf(row, cap, "{\"kind\":\"pimu\",\"m\":%llu,\"s0\":%lld,\"s1\":%lld,\"b\":%d,\"pt\":%lld,\"ce\":%lld,\"bv\":%lld,\"sk\":%lld,\"dr\":%lld,\"fr\":%lld,\"d\":[",
                         (unsigned long long)(t0_ / MS), have_seq_ ? (long long)s0_ : -1LL, have_seq_ ? (long long)s1_ : -1LL, bias_, (long long)pt_,
                         clamp_ll((long long)ce_, 0, BIG), clamp_ll((long long)bv_, 0, BIG), clamp_ll((long long)sk_, 0, BIG),
                         clamp_ll((long long)dr_, 0, BIG), clamp_ll((long long)fr_, 0, BIG));
        for (unsigned i = 0; i < nb_ && k > 0 && size_t(k) < cap; ++i) {
            const Bin& b = bins_[i];
            k += snprintf(row + k, cap - size_t(k), "%s[%lld,%lld,%lld,%lld,%u]", i ? "," : "", clamp_ll(b.n, 0, 999),
                          clamp_ll(b.dth, -DTH, DTH), clamp_ll((b.wu + 500) / 1000, 0, C), clamp_ll(b.gap, 0, C), b.fl & 255u);
        }
        if (k > 0 && size_t(k) < cap) snprintf(row + k, cap - size_t(k), "]}");
    }
    bool open_, have_seq_;
    uint64_t t0_;
    unsigned nb_;
    Bin bins_[BINS];
    uint64_t ce_, bv_, sk_, dr_, fr_;
    int32_t bias_;
    int64_t pt_;
    uint32_t s0_, s1_;
};

// Ping bookkeeping: CMU sends every 5 s; a pong is accepted only for one of
// the last four pings with the same seq and the same echoed t_cmu_ns.
class Pinger {
public:
    static const uint64_t PERIOD_NS = 5000000000ULL;
    Pinger() : seq_(0), last_(0), n_(0) { memset(sent_, 0, sizeof sent_); }
    void reset() { last_ = 0; n_ = 0; memset(sent_, 0, sizeof sent_); }
    bool due(uint64_t now) const { return !last_ || now - last_ >= PERIOD_NS; }
    void make(uint64_t now, uint8_t out[SYNC_LEN]) {
        Sync s; s.version = VERSION; s.type = SYNC_PING; s.seq = ++seq_; s.t_cmu_ns = int64_t(now); s.t_phone_ns = 0; s.crc = 0;
        encode_sync(s, out); last_ = now;
        sent_[n_ % 4].seq = s.seq; sent_[n_ % 4].t = now; sent_[n_ % 4].used = true; ++n_;
    }
    // rtt and offset = t_phone - (t_cmu_send + rtt/2), in ns. False if unmatched.
    bool pong(const Sync& s, uint64_t now, int64_t* rtt, int64_t* offset) {
        if (s.type != SYNC_PONG) return false;
        for (unsigned i = 0; i < 4; ++i) {
            Sent& e = sent_[i];
            if (!e.used || e.seq != s.seq || int64_t(e.t) != s.t_cmu_ns || now < e.t) continue;
            e.used = false;
            *rtt = int64_t(now - e.t); *offset = s.t_phone_ns - (int64_t(e.t) + *rtt / 2);
            return true;
        }
        return false;
    }
private:
    struct Sent { uint32_t seq; uint64_t t; bool used; };
    uint32_t seq_; uint64_t last_; unsigned n_; Sent sent_[4];
};

}}  // namespace mx5::pimu
#endif
