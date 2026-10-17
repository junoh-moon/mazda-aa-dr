// Phone IMU channel v1: decoder, aggregation, ping/pong and row budget.
// No arguments: unit checks. "decode-frames": each stdin line is one frame
// (hex) for a fresh decoder; prints one JSON object per line (the decoded
// fields, the re-encoded hex and the decoder counts). "decode-stream": all
// lines are chunks of one stream; prints one JSON line per event and the
// final counts. tests/collector/test_phone_imu.py compares both with
// tests/phone_imu/golden_frames.json.
#define MX5_COLLECTOR_TESTING
#include "../../src/collector/phone_imu_rx.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace mx5::pimu;

static std::vector<uint8_t> unhex(const std::string& s) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < s.size(); i += 2) out.push_back(uint8_t(strtoul(s.substr(i, 2).c_str(), 0, 16)));
    return out;
}
static std::string hex(const uint8_t* p, size_t n) {
    std::string out; char b[3];
    for (size_t i = 0; i < n; ++i) { snprintf(b, sizeof b, "%02x", p[i]); out += b; }
    return out;
}
static void print_counts(const Decoder::Counts& c) {
    printf("{\"counts\":{\"crc_error\":%llu,\"unknown_version\":%llu,\"bad_type\":%llu,\"skipped\":%llu,\"frames\":%llu,\"syncs\":%llu}}\n",
           (unsigned long long)c.crc_error, (unsigned long long)c.bad_version, (unsigned long long)c.bad_type, (unsigned long long)c.skipped,
           (unsigned long long)c.frames, (unsigned long long)c.syncs);
}
static void print_event(Decoder::Event ev, const Data& d, const Sync& s) {
    if (ev == Decoder::DATA) {
        uint8_t b[DATA_LEN]; encode_data(d, b);
        printf("{\"kind\":\"data\",\"version\":%u,\"flags\":%u,\"seq\":%u,\"t_end_ns\":%lld,\"dtheta_urad\":%d,\"window_us\":%u,\"bias_urad_s\":%d,\"crc32\":%u,\"reencoded\":\"%s\"}\n",
               d.version, d.flags, d.seq, (long long)d.t_end_ns, d.dtheta_urad, d.window_us, d.bias_urad_s, d.crc, hex(b, DATA_LEN).c_str());
    } else {
        uint8_t b[SYNC_LEN]; encode_sync(s, b);
        printf("{\"kind\":\"%s\",\"version\":%u,\"type\":%u,\"seq\":%u,\"t_cmu_ns\":%lld,\"t_phone_ns\":%lld,\"crc32\":%u,\"reencoded\":\"%s\"}\n",
               s.type == SYNC_PING ? "ping" : s.type == SYNC_PONG ? "pong" : "sync", s.version, s.type, s.seq, (long long)s.t_cmu_ns,
               (long long)s.t_phone_ns, s.crc, hex(b, SYNC_LEN).c_str());
    }
}
static int decode(bool stream) {
    char line[65536];
    Decoder dec;
    while (fgets(line, sizeof line, stdin)) {
        std::string s(line); while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        std::vector<uint8_t> b = unhex(s);
        if (!stream) dec = Decoder();
        size_t off = 0; bool any = false;
        while (off < b.size() || !b.size()) {
            off += dec.feed(b.data() + off, b.size() - off);
            Data d; Sync y; Decoder::Event ev;
            while ((ev = dec.next(&d, &y)) != Decoder::NONE) { print_event(ev, d, y); any = true; }
            if (!b.size()) break;
        }
        if (!stream) { if (!any) printf("{\"kind\":\"none\"}\n"); print_counts(dec.counts); }
    }
    if (stream) print_counts(dec.counts);
    return 0;
}

static Data frame(uint32_t seq, int32_t dth, uint8_t flags = FLAG_BIAS_VALID | FLAG_GRAVITY_VALID) {
    Data d; d.version = 1; d.flags = flags; d.seq = seq; d.t_end_ns = 1000 + seq; d.dtheta_urad = dth; d.window_us = 20000; d.bias_urad_s = 5; d.crc = 0;
    return d;
}
static const uint64_t S = 1000000000ULL;

static void unit() {
    // CRC-32 check value of "123456789" (IEEE): 0xCBF43926.
    assert(crc32((const uint8_t*)"123456789", 9) == 0xCBF43926u);
    // Resync through garbage, a fake magic and a truncated frame.
    uint8_t a[DATA_LEN], p[SYNC_LEN];
    encode_data(frame(7, -100), a);
    Sync s; s.version = 1; s.type = SYNC_PONG; s.seq = 3; s.t_cmu_ns = 5; s.t_phone_ns = 9; s.crc = 0; encode_sync(s, p);
    std::vector<uint8_t> st = {'x', 'M', 'M', 'X', 1, 0, 0};       // 'MX' + version 1 but bad CRC later
    st.insert(st.end(), a, a + DATA_LEN);
    st.insert(st.end(), p, p + SYNC_LEN);
    st.insert(st.end(), a, a + 10);                               // truncated tail
    Decoder dec; Data d; Sync y; unsigned data = 0, sync = 0;
    for (size_t i = 0; i < st.size(); ++i) {                    // byte-by-byte feed
        dec.feed(&st[i], 1);
        Decoder::Event ev;
        while ((ev = dec.next(&d, &y)) != Decoder::NONE) { if (ev == Decoder::DATA) { ++data; assert(d.seq == 7 && d.dtheta_urad == -100); } else { ++sync; assert(y.seq == 3); } }
    }
    assert(data == 1 && sync == 1);
    assert(dec.counts.crc_error == 1);   // the fake 'MX',1 candidate
    dec.drop_partial();
    assert(dec.counts.skipped == 17);  // 'x', 'M', the 5-byte fake frame start, the 10-byte tail

    // Aggregation: 1 s bins by receipt time, seq gaps with wrap, frame reset.
    Aggregator g; char row[1024];
    uint64_t t0 = 100 * S;
    g.start(t0);
    g.add(t0 + 1, frame(4294967294u, 10));
    g.add(t0 + 2, frame(4294967295u, 20));
    g.add(t0 + 3, frame(1, -5));                // wrapped: seq 0 missing -> gap 1
    g.add(t0 + S + 1, frame(2, 7));
    g.add(t0 + S + 2, frame(40, 1, FLAG_FRAME_RESET | FLAG_BIAS_VALID));  // reset: no gap
    assert(!g.advance(t0 + 9 * S, row, sizeof row));
    assert(g.advance(t0 + 10 * S, row, sizeof row));
    std::string r(row);
    assert(r.find("\"m\":100000,") != std::string::npos);
    assert(r.find("\"s0\":4294967294,\"s1\":40,") != std::string::npos);
    assert(r.find("\"fr\":1,") != std::string::npos);
    assert(r.find("\"d\":[[3,25,60,1,5],[2,8,40,0,13],[0,0,0,0,0]") != std::string::npos);
    assert(std::count(r.begin(), r.end(), '[') == 11);
    // A frame 25 s later: the next row starts at the 20 s boundary.
    assert(g.advance(t0 + 25 * S, row, sizeof row));
    g.add(t0 + 25 * S, frame(41, 3));
    assert(g.close(t0 + 25 * S + 1, row, sizeof row));
    r = row;
    assert(r.find("\"m\":120000,") != std::string::npos);
    assert(r.find("\"d\":[[0,0,0,0,0],[0,0,0,0,0],[0,0,0,0,0],[0,0,0,0,0],[0,0,0,0,0],[1,3,20,0,5]]") != std::string::npos);
    assert(!g.close(t0 + 30 * S, row, sizeof row));

    // Ping/pong: offset = t_phone - (t_send + rtt/2); unmatched pongs refused.
    Pinger pg; uint8_t ping[SYNC_LEN];
    assert(pg.due(1));
    pg.make(10 * S, ping);
    Sync q = decode_sync(ping);
    assert(q.type == SYNC_PING && q.t_phone_ns == 0 && q.t_cmu_ns == int64_t(10 * S) && q.crc == crc32(ping, 24));
    assert(!pg.due(10 * S + 4 * S) && pg.due(15 * S));
    Sync pong = q; pong.type = SYNC_PONG; pong.t_phone_ns = 500 * int64_t(S);
    int64_t rtt, off;
    Sync wrong = pong; wrong.t_cmu_ns += 1;
    assert(!pg.pong(wrong, 10 * S + 40000000, &rtt, &off));
    assert(pg.pong(pong, 10 * S + 40000000, &rtt, &off));
    assert(rtt == 40000000 && off == 500 * int64_t(S) - int64_t(10 * S) - 20000000);
    assert(!pg.pong(pong, 10 * S + 50000000, &rtt, &off));  // used once

    // Token bucket: burst, then one per refill period.
    Bucket b; b.init(10 * S, 2);
    assert(b.take(0) && b.take(0) && !b.take(1));
    assert(!b.take(9 * S) && b.take(10 * S) && !b.take(11 * S));
    assert(b.take(31 * S) && b.take(31 * S) && !b.take(31 * S));

    // Row budget: every row kind at its longest stays within its bound and
    // the hourly worst case stays below 400 KB.
    size_t agg = Aggregator::max_row_bytes();
    assert(agg < ROW);
    char big[ROW];
    snprintf(big, sizeof big, "{\"kind\":\"pimu_sync\",\"m\":%llu,\"q\":%u,\"rtt_us\":%lld,\"off_us\":%lld}",
             18446744073709551615ULL / MS_NS, 4294967295u, -9223372036854775807LL / 1000, -9223372036854775807LL / 1000);
    assert(strlen(big) + 1 <= MAX_SYNC_ROW);
    // pimu_link at its longest (signed svc of a reply for another service).
    snprintf(big, sizeof big, "{\"kind\":\"pimu_link\",\"m\":%llu,\"ev\":\"%s\",\"why\":\"%s\",\"path\":\"%s\",\"svc\":%lld,\"cs\":%lld,\"st\":%lld,\"err\":%d}",
             18446744073709551615ULL / MS_NS, "silence_reopen_xx", "12345678901234567890123", "1234567890123456789012", -2147483647LL - 1,
             4294967294LL, 4294967294LL, -2147483647 - 1);
    assert(strlen(big) + 1 <= MAX_LINK_ROW);
    // ... and with a D-Bus error name (64), message (80) and count (own bucket).
    std::string name(ERR_NAME, 'N'), msg(ERR_MSG, 'M');
    snprintf(big, sizeof big, "{\"kind\":\"pimu_link\",\"m\":%llu,\"ev\":\"%s\",\"why\":\"%s\",\"path\":\"%s\",\"svc\":%lld,\"cs\":%lld,\"st\":%lld,\"err\":%d"
             ",\"name\":\"%s\",\"msg\":\"%s\",\"n\":%u}",
             18446744073709551615ULL / MS_NS, "silence_reopen_xx", "12345678901234567890123", "1234567890123456789012", -2147483647LL - 1,
             4294967294LL, 4294967294LL, -2147483647 - 1, name.c_str(), msg.c_str(), 9999u);
    assert(strlen(big) + 1 <= MAX_LINK_ERR_ROW);
    snprintf(big, sizeof big, "{\"kind\":\"bt_watch\",\"m\":%llu,\"ev\":\"%s\",\"why\":\"%s\",\"pid\":%ld,\"after_ms\":%llu}",
             18446744073709551615ULL / MS_NS, "config", "args_changed", 2147483647L, 9999999999ULL);
    assert(strlen(big) + 1 <= MAX_BT_ROW);
    snprintf(big, sizeof big, "{\"kind\":\"pimu_drop\",\"m\":%llu,\"d\":[%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu]}",
             18446744073709551615ULL / MS_NS, 9999999999ULL, 9999999999ULL, 9999999999ULL, 9999999999ULL, 9999999999ULL,
             9999999999ULL, 9999999999ULL, 9999999999ULL, 9999999999ULL);
    assert(strlen(big) + 1 <= MAX_DROP_ROW);
    snprintf(big, sizeof big, "{\"kind\":\"pimu_boot\",\"schema\":1,\"m\":%llu,\"boot_id\":\"%s\",\"svc\":%u,\"budget_bph\":%llu,\"cap\":%d}",
             18446744073709551615ULL / MS_NS, "12345678-1234-1234-1234-123456789abc", SERVICE_ID, 9999999ULL, 1);
    assert(strlen(big) + 1 <= MAX_BOOT_ROW);
    // D-Bus failure rows: one per distinct error name, then every 10th failure.
    {
        char last[ERR_NAME + 1] = ""; unsigned n = 0; std::vector<unsigned> due;
        const char* um = "org.freedesktop.DBus.Error.UnknownMethod";
        for (unsigned i = 1; i <= 25; ++i) if (err_row_due(i == 12 ? "org.freedesktop.DBus.Error.NoReply" : um, last, &n)) due.push_back(n);
        std::vector<unsigned> want = {1, 10, 12, 13, 20};   // 12 NoReply, 13 UnknownMethod again
        assert(due == want);
        std::string longname(200, 'L');                       // compared on its first 64 bytes only
        assert(err_row_due(longname.c_str(), last, &n) && strlen(last) == ERR_NAME && !err_row_due(longname.c_str(), last, &n));
        n = 999990; last[0] = 0;
        unsigned rows = 0;
        for (unsigned i = 0; i < 100; ++i) rows += err_row_due(um, last, &n);
        assert(n == 999999 && rows == 1);                    // the first (distinct) only: saturated, no storm
    }
    printf("pimu_budget max_pimu_row=%zu budget_bph=%.0f\n", agg, budget_bph());
    assert(budget_bph() < 400000.0);
    printf("PASS test_phone_imu\n");
}

int main(int argc, char** argv) {
    if (argc == 2 && !strcmp(argv[1], "decode-frames")) return decode(false);
    if (argc == 2 && !strcmp(argv[1], "decode-stream")) return decode(true);
    if (argc == 2 && !strcmp(argv[1], "budget")) { printf("%.0f %zu\n", budget_bph(), Aggregator::max_row_bytes()); return 0; }
    if (argc != 1) return 64;
    unit();
    return 0;
}
