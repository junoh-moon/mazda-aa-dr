package me.sixtyfive.mx5imu.core;

import java.io.ByteArrayOutputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InterruptedIOException;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * Stage-2 OBD yaw source tests (validation/OBD_YAW_CHANNEL_DESIGN_2026-10-15.md), plain JVM.
 * Usage: ObdTests OBD_VECTORS.json OUT_STREAM.bin OWNER_DOC.md (docs/OBD_YAW_CHECK_KO.md: verdict table check)
 *   decode vectors (shared with the independent Python decoder make_obd_vectors.py), line parser,
 *   loss meter, integrator, source selector, ElmController/ObdLink against a fake ELM327; writes the
 *   v1 DATA frames of a scripted monitor run to OUT_STREAM.bin for the Python cross-check.
 */
public final class ObdTests {
    private static int passed, failed;

    private static void check(boolean ok, String what) {
        if (ok) passed++;
        else { failed++; System.out.println("FAIL: " + what); }
    }

    private static void eq(long got, long want, String what) { check(got == want, what + " got " + got + " want " + want); }

    static final ElmController.Timing FAST = new ElmController.Timing(500, 500, 300, 250, 800, 3000);

    public static void main(String[] args) throws Exception {
        Map<String, Object> vec = vectors(args[0]);
        decodeVectors(vec);
        lineVectors(vec);
        parserStream();
        lossMeter();
        verdictTable(args[2]);
        integrator(vec);
        selector();
        protocolFlags();
        controllerInit();
        controllerRestarts();
        controllerQuestionStop();
        controllerNoTraffic();
        controllerRefusedCommand();
        controllerAdapterReset();
        controllerRestartStorm();
        controllerDoublePrompt();
        controllerAtmaAlwaysRefused();
        controllerStopSendsX();
        controllerLateStopPrompt();
        controllerLateStopPromptAtInit();
        controllerEchoPersists();
        controllerNoAtmaAfterStop();
        linkConnectGiveUp();
        linkSlowRetryAfterGiveUp();
        arbiterDefersSessionStart();
        linkWaitsForPredecessor();
        linkAmbiguousGapFlag();
        observedVectors(vec);
        linkEndToEnd(vec, args[1]);
        System.out.println("obd tests: " + passed + " passed, " + failed + " failed");
        if (failed != 0) System.exit(1);
    }

    @SuppressWarnings("unchecked")
    static Map<String, Object> vectors(String path) throws IOException {
        return (Map<String, Object>) MiniJson.parse(new String(Files.readAllBytes(Paths.get(path)), StandardCharsets.UTF_8));
    }

    @SuppressWarnings("unchecked")
    static List<Map<String, Object>> list(Map<String, Object> m, String k) { return (List<Map<String, Object>>) m.get(k); }

    static double num(Object o) { return o instanceof Long ? (Long) o : (Double) o; }

    static byte[] hex(String h) {
        byte[] b = new byte[h.length() / 2];
        for (int i = 0; i < b.length; i++) b[i] = (byte) Integer.parseInt(h.substring(2 * i, 2 * i + 2), 16);
        return b;
    }

    // ---- decode vectors ----

    static void decodeVectors(Map<String, Object> vec) {
        int n = 0;
        for (Map<String, Object> e : list(vec, "can")) {
            String name = (String) e.get("name");
            ElmLineParser.Event ev = ElmLineParser.classify(((String) e.get("line")).trim().toUpperCase(Locale.ROOT), 7);
            check(ev.kind == ElmLineParser.Kind.CAN, name + ": line is a CAN frame");
            eq(ev.id, (Long) e.get("id"), name + ": id");
            check(java.util.Arrays.equals(ev.data, hex((String) e.get("data"))), name + ": data bytes");
            Can079.Sample s = Can079.decode(ev.id, ev.data);
            check(s.status.name().equals(e.get("status")), name + ": status " + s.status + " want " + e.get("status"));
            eq(s.raw12, (Long) e.get("raw12"), name + ": raw12");
            eq(s.counter, (Long) e.get("counter"), name + ": counter");
            eq(s.qf, (Long) e.get("qf"), name + ": qf");
            if (s.ok()) {
                check(Math.abs(s.cwDegPerS() - num(e.get("cw_dps"))) < 1e-6, name + ": cw deg/s " + s.cwDegPerS());
            }
            if (e.containsKey("dbc_yawrate_corr_dps")) {
                check(Math.abs(Can079.dbcYawRateCorr(ev.data) - num(e.get("dbc_yawrate_corr_dps"))) < 1e-6, name + ": DBC cross-check");
            }
            n++;
        }
        check(n >= 15, "at least 15 CAN vectors");
        // Sign: the count rises in left turns (public ND log), clockwise is positive.
        check(Can079.decode(0x079, hex("DF285A02DB5536A5")).cwDegPerS() < 0, "left turn (raw above zero) is negative (counter-clockwise)");
        // Scale: 26.5 counts per deg/s within 0.1 %.
        double perDeg = 256.0 / Math.abs(Can079.decode(0x079, hex("DF285A01FF5530A5")).cwDegPerS() - Can079.decode(0x079, hex("DF285A023F5530A5")).cwDegPerS());
        check(Math.abs(perDeg - 26.5) < 0.03, "about 26.5 counts per deg/s: " + perDeg);
    }

    static void lineVectors(Map<String, Object> vec) {
        for (Map<String, Object> e : list(vec, "lines")) {
            String t = (String) e.get("text");
            ElmLineParser.Event ev = ElmLineParser.classify(t.trim().toUpperCase(Locale.ROOT), 1);
            check(ev.kind.name().equals(e.get("kind")), "line '" + t + "' is " + ev.kind + " want " + e.get("kind"));
        }
    }

    static void parserStream() {
        ElmLineParser p = new ElmLineParser();
        byte[] all = "ATZ\r\rELM327 v1.5\r\r>OK\r\r>079DF285A01FFD530A5\r079 DF 28\n\r\0BUFFER FULL\r\r>".getBytes(StandardCharsets.US_ASCII);
        List<ElmLineParser.Event> got = new ArrayList<>();
        for (int i = 0; i < all.length; i += 3) got.addAll(p.feed(all, i, Math.min(3, all.length - i), i));  // split arbitrarily
        List<String> kinds = new ArrayList<>();
        for (ElmLineParser.Event e : got) kinds.add(e.kind.name());
        check(kinds.equals(java.util.Arrays.asList("TEXT", "TEXT", "PROMPT", "OK", "PROMPT", "CAN", "CAN", "BUFFER_FULL", "PROMPT")),
                "stream split in 3-byte chunks: " + kinds);
        eq(p.nulBytes, 1, "NUL byte ignored and counted");
        check(got.get(5).rxNs > 0 && got.get(5).data.length == 8, "receipt time is the chunk that completed the line");
        ElmLineParser q = new ElmLineParser();
        StringBuilder longLine = new StringBuilder();
        for (int i = 0; i < 200; i++) longLine.append('A');
        byte[] lb = (longLine + "\rOK\r").getBytes(StandardCharsets.US_ASCII);
        List<ElmLineParser.Event> l = q.feed(lb, 0, lb.length, 5);
        check(l.size() == 2 && l.get(0).kind == ElmLineParser.Kind.MALFORMED && l.get(1).kind == ElmLineParser.Kind.OK && q.overflows == 1,
                "an over-long line is discarded and counted; the next line parses");
    }

    // ---- loss meter ----

    static void lossMeter() {
        final long P = 20_000_000L;
        ObdLossMeter m = new ObdLossMeter(20000);
        long t = 1_000_000_000L;
        int c = 0;
        eq(m.add(c, t), 0, "first frame: no loss");
        for (int i = 1; i < 600; i++) { c = (c + 1) & 15; t += P; if (m.add(c, t) != 0) { check(false, "step 1: no loss at " + i); break; } }
        check(m.counterConsistent() && Math.abs(m.fps() - 50) < 0.1 && m.lossPercent() < 1e-9, "12 s clean at 50 Hz: consistent, no loss");
        check(!m.verdict(t + P).equals("GOOD"), "12 s is too short a span for GOOD (30 s minimum)");
        for (int i = 0; i < 1000; i++) { c = (c + 1) & 15; t += P; m.add(c, t); }
        check(m.verdict(t + P).equals("GOOD"), "32 s clean: GOOD " + m.verdict(t + P));
        check(Math.abs(m.wallCoverage(t + P) - 1.0) < 0.001, "clean: wall coverage 1.0: " + m.wallCoverage(t + P));
        // lose 3 frames
        c = (c + 4) & 15; t += 4 * P;
        eq(m.add(c, t), 3, "counter step 4: three frames lost");
        eq(m.counterLost, 3, "counter loss total");
        // counter wrap
        c = 14; t += P; m.add(c, t); c = 15; t += P; m.add(c, t); c = 0; t += P;
        eq(m.add(c, t), 0, "wrap 15 -> 0 is a step of one");
        // repeated counter is counted, not dropped
        t += P / 4;
        eq(m.add(c, t), 0, "repeated counter: no loss");
        eq(m.duplicates, 1, "repeated counter counted");
        // a long receipt gap is ambiguous (buffering or whole cycles)
        c = (c + 1) & 15; t += 600_000_000L;
        m.add(c, t);
        check(m.lastAmbiguous && m.ambiguousGaps == 1 && m.stalls500ms == 1 && m.maxGapNs == 600_000_000L, "600 ms gap: ambiguous, stall counted");
        check(!m.verdict(t + P).equals("GOOD"), "a stall prevents GOOD");
        // a repeated counter after a long (ambiguous) gap is a whole lost cycle, not a duplicate
        eq(m.add(c, t + 600_000_000L), 15, "same counter after 600 ms: 15 lost (whole cycle), not a duplicate");
        // heavy loss: every second frame lost for 20 s -> implied rate consistent, loss 50 %
        ObdLossMeter h = new ObdLossMeter(20000);
        long t2 = 0;
        int c2 = 0;
        h.add(c2, t2);
        for (int i = 0; i < 500; i++) { c2 = (c2 + 2) & 15; t2 += 2 * P; h.add(c2, t2); }
        check(h.counterConsistent() && Math.abs(h.lossPercent() - 50) < 0.5 && Math.abs(h.fps() - 25) < 0.1, "half lost: counter says 50 %");
        check(h.verdict(t2 + 2 * P).equals("BAD"), "25 frames/s: BAD (wall coverage 50 %)");
        // counter that does not step per frame (always 0): implied rate wrong -> receipt rate only
        ObdLossMeter z = new ObdLossMeter(20000);
        long t3 = 0;
        for (int i = 0; i <= 700; i++) { z.add(0, t3); t3 += P; }
        check(!z.counterConsistent() || z.lossPercent() < 1, "constant counter: no false loss");
        // no counter (6-byte frames): timing gaps only
        ObdLossMeter n = new ObdLossMeter(20000);
        n.add(-1, 0);
        n.add(-1, P);
        eq(n.add(-1, P + 200_000_000L), 9, "no counter: a 200 ms receipt gap is 9 lost frames");
        // chains: restarts do not count as loss and do not add the time between chains
        ObdLossMeter r = new ObdLossMeter(20000);
        r.add(1, 0); r.add(2, P);
        r.breakChain();
        eq(r.add(9, 5_000_000_000L), 0, "after breakChain: no loss inferred");
        eq(r.elapsedNs(), P, "elapsed excludes the time between chains");
        eq(r.chainBreaks, 1, "chain break counted");
        eq(new ObdLossMeter(20000).verdict(5_000_000_000L).equals("NONE") ? 1 : 0, 1, "no frames: NONE");
        // F2 (review E8): 20 clean 2 s chains, each followed by a 1 s hole (BUFFER FULL / stall
        // restarts). The in-chain measures see 50 fps and 0 % loss; the wall clock sees 1/3 missing.
        ObdLossMeter e8 = new ObdLossMeter(20000);
        long t8 = 0;
        int c8 = 0;
        for (int chain = 0; chain < 20; chain++) {
            for (int i = 0; i < 100; i++) { e8.add(c8, t8); c8 = (c8 + 1) & 15; t8 += P; }
            e8.breakChain();
            t8 += 1_000_000_000L;
            c8 = (c8 + 50) & 15;
        }
        long end8 = t8;   // 60 s after the first frame
        check(Math.abs(e8.fps() - 50) < 0.1 && e8.lossPercent() < 1e-9, "E8: in-chain fps 50 and loss 0 (the blind spot)");
        check(!e8.verdict(end8).equals("GOOD"), "E8: 20 x (2 s chain + 1 s hole) is NOT GOOD: " + e8.verdict(end8));
        check(e8.verdict(end8).equals("MARGINAL"), "E8: wall coverage 2/3 is MARGINAL: " + e8.verdict(end8));
        check(Math.abs(e8.wallCoverage(end8) - 2000.0 / 3000.0) < 0.001, "E8: wall coverage " + e8.wallCoverage(end8));
        eq(e8.restarts(), 20, "E8: 20 restarts reported");
        // a 40 s clean run with 3 restarts of 20 ms holes: coverage fine, but restarts > 1 -> not GOOD
        ObdLossMeter rs = new ObdLossMeter(20000);
        long t9 = 0;
        int c9 = 0;
        for (int i = 0; i < 2000; i++) {
            if (i % 600 == 599) rs.breakChain();
            rs.add(c9, t9); c9 = (c9 + 1) & 15; t9 += P;
        }
        check(rs.wallCoverage(t9) > 0.99 && rs.restarts() == 3 && rs.verdict(t9).equals("MARGINAL"),
                "3 restarts in a clean 40 s: MARGINAL (restarts " + rs.restarts() + ", " + rs.verdict(t9) + ")");
    }

    // ---- integrator ----

    @SuppressWarnings("unchecked")
    static void integrator(Map<String, Object> vec) {
        ObdIntegrator in = new ObdIntegrator();
        double exactSum = 0;
        long sum = 0;
        int k = 0;
        for (Map<String, Object> e : list(vec, "can")) {
            if (!"OK".equals(e.get("status"))) continue;
            Can079.Sample s = Can079.decode(((Long) e.get("id")).intValue(), hex((String) e.get("data")));
            Window w = in.add(s, 1000L + k, k == 2 ? 1 : 0);
            exactSum += num(e.get("dtheta_urad_20ms"));
            sum += w.dthetaUrad;
            check(Math.abs(sum - exactSum) <= 0.5 + 1e-9, e.get("name") + ": running sum within 0.5 urad of exact (" + sum + " vs " + exactSum + ")");
            eq(w.windowUs, 20000, "window_us is the 20 ms producer period");
            eq(w.biasUradS, 0, "bias 0");
            eq(w.tEndNs, 1000L + k, "t_end is the receipt time");
            check((w.flags & 7) == 0 && (w.flags & Protocol.FLAG_SOURCE_OBD) != 0, "OBD flag set, bits 0..2 clear");
            check(((w.flags & Protocol.FLAG_FRAME_RESET) != 0) == (k == 0), "frame_reset only on the first window");
            check(((w.flags & Protocol.FLAG_OBD_GAP) != 0) == (k == 2), "gap flag only where frames were lost");
            k++;
        }
        check(k >= 10, "integrated at least 10 OK vectors");
        // the stream sum of the vector file
        Map<String, Object> st = (Map<String, Object>) vec.get("stream");
        List<Object> names = (List<Object>) st.get("names");
        ObdIntegrator in2 = new ObdIntegrator();
        long s2 = 0;
        for (Object nm : names) for (Map<String, Object> e : list(vec, "can")) if (nm.equals(e.get("name")))
            s2 += in2.add(Can079.decode(0x079, hex((String) e.get("data"))), 0, 0).dthetaUrad;
        check(Math.abs(s2 - num(st.get("sum_dtheta_urad"))) <= 0.5, "vector stream sum " + s2 + " vs " + st.get("sum_dtheta_urad"));
        // 10 minutes at a constant one-count offset: no rounding drift
        ObdIntegrator d = new ObdIntegrator();
        Can079.Sample one = Can079.decode(0x079, hex("DF285A01FF9530A5"));  // raw 2046 (not on the lattice; scale only)
        long total = 0;
        for (int i = 0; i < 30000; i++) total += d.add(one, i, 0).dthetaUrad;
        double want = 30000 * 658.615 * 0.020;
        check(Math.abs(total - want) <= 0.5, "30000 windows at +1 count: " + total + " vs " + want);
        eq(d.add(Can079.decode(0x078, hex("DF285A01FF9530A5")), 0, 0) == null ? 1 : 0, 1, "rejected sample gives no window");
        eq(d.rejected, 1, "rejected counted");
        d.reset();
        check((d.add(one, 0, 0).flags & Protocol.FLAG_FRAME_RESET) != 0, "reset marks the next window");
    }

    // ---- selector ----

    static void selector() {
        final long P = 20_000_000L, S = 1_000_000_000L;
        SourceSelector off = new SourceSelector(false);
        for (long t = 0; t < 10 * S; t += P) off.obdFrame(t);
        check(off.update(10 * S) == SourceSelector.Source.PHONE, "OBD not allowed: always PHONE");
        SourceSelector s = new SourceSelector(true);
        Window w = new Window(Protocol.FLAG_SOURCE_OBD, 0, 1, 20000, 0, 1);
        Window g = new Window(Protocol.FLAG_GRAVITY_VALID, 0, 1, 20000, 0, 4);
        check(s.admit(SourceSelector.Source.PHONE, g, 0) == g, "phone first");
        long t = 0;
        for (; t < 2 * S; t += P) s.obdFrame(t);
        check(s.update(t) == SourceSelector.Source.PHONE, "healthy for less than 3 s: still PHONE");
        for (; t < 5 * S + P; t += P) { s.obdFrame(t); s.update(t); }
        check(s.current() == SourceSelector.Source.OBD, "healthy for 3 s: OBD");
        check(s.admit(SourceSelector.Source.PHONE, g, t) == null, "phone held while OBD is chosen");
        Window first = s.admit(SourceSelector.Source.OBD, w, t);
        check(first != null && (first.flags & Protocol.FLAG_FRAME_RESET) != 0, "first OBD window after the switch carries frame_reset");
        Window second = s.admit(SourceSelector.Source.OBD, w, t);
        check(second == w, "later OBD windows unchanged");
        // stale for 600 ms -> PHONE immediately
        Window back = s.admit(SourceSelector.Source.PHONE, g, t + 600_000_000L);
        check(back != null && (back.flags & Protocol.FLAG_FRAME_RESET) != 0 && s.current() == SourceSelector.Source.PHONE,
                "OBD stale: back to PHONE with frame_reset");
        // 30 frames/s (60 in 2 s) is unhealthy
        SourceSelector slow = new SourceSelector(true);
        for (long u = 0; u < 10 * S; u += 33_333_333L) { slow.obdFrame(u); slow.update(u); }
        check(slow.current() == SourceSelector.Source.PHONE, "30 frames/s never selects OBD");
        eq(s.switches, 2, "two switches counted");
        // STALE_NS on its own: a Bluetooth burst leaves >= 80 frames in the 2 s window, but the newest
        // is 0.6 s old. Freshness, not the frame count, must take the selector back to PHONE.
        SourceSelector st = new SourceSelector(true);
        long u = 0;
        for (; u < 6 * S; u += P) { st.obdFrame(u); st.update(u); }
        check(st.current() == SourceSelector.Source.OBD, "stale test: OBD chosen after 6 s of frames");
        for (int i = 0; i < 100; i++) { u += P / 2; st.obdFrame(u); st.update(u); }   // 1 s delivered at 100/s
        long last = u;
        check(st.update(last + 400_000_000L) == SourceSelector.Source.OBD, "newest 0.4 s old, window full: still OBD");
        SourceSelector.Source after = st.update(last + 600_000_000L);
        check(after == SourceSelector.Source.PHONE, "newest 0.6 s old with >= 80 frames in 2 s: back to PHONE (STALE_NS 0.5 s)");
        // MIN_FRAMES_2S on its own: 37.5 frames/s (75 in 2 s, fresh) is not healthy
        SourceSelector mid = new SourceSelector(true);
        for (long v = 0; v < 10 * S; v += 26_666_667L) { mid.obdFrame(v); mid.update(v); }
        check(mid.current() == SourceSelector.Source.PHONE, "75 frames in 2 s (< 80) never selects OBD");
    }

    static void protocolFlags() {
        byte[] f = Protocol.encodeData(Protocol.FLAG_SOURCE_OBD | Protocol.FLAG_OBD_GAP | Protocol.FLAG_FRAME_RESET, 5, 7, -26, 20000, 0);
        eq(f[3] & 0xFF, 0x38, "flags byte carries bits 3, 4, 5");
        boolean threw = false;
        try { Protocol.encodeData(64, 0, 0, 0, 0, 0); } catch (IllegalArgumentException e) { threw = true; }
        check(threw, "bit 6 stays reserved");
        FrameParser p = new FrameParser();
        List<Frame> got = p.feed(f, 0, f.length);
        check(got.size() == 1 && got.get(0).flags == 0x38 && got.get(0).dthetaUrad == -26, "decoder keeps the OBD flags");
    }

    // ---- fake ELM327 ----

    /**
     * A scripted ELM327 v1.5 behind a Transport. Echo is on until ATE0. Every complete command gets a
     * response and a prompt; ATMA starts the monitor script, which emits lines with delays. Any byte
     * received while monitoring stops it (prompt); retainStop keeps that byte in the command buffer
     * (some clones), otherwise it is discarded.
     */
    static final class FakeElm implements Transport {
        /** Adapter-to-phone chunks with their delivery time (latencyMs: a congested Bluetooth link). */
        final LinkedBlockingQueue<long[]> due = new LinkedBlockingQueue<>();
        final LinkedBlockingQueue<byte[]> toPhone = new LinkedBlockingQueue<>();
        /** Constant adapter-to-phone delay of every byte (order kept), 0 = none. */
        volatile long latencyMs;
        /** A clone that answers OK to ATE0 but keeps echoing every command. */
        volatile boolean ignoreAte0;
        final ByteArrayOutputStream fromPhone = new ByteArrayOutputStream();
        final List<String> commands = new CopyOnWriteArrayList<>();
        final Set<String> refuse = new HashSet<>();
        volatile boolean closed, echo = true, monitoring, retainStop, startMonitoring;
        /** Settings that an adapter reset (ATZ, 'RESET' script line) puts back to the defaults. */
        volatile boolean headers, filterSet, keepProtocol = true, doublePromptAfterAtz, blockWrites, leftoverOnAtz;
        volatile String protocol = "AUTO";
        /** commands.size() when the last 'RESET'/'BANNER'/'PROMPT'/'SILENTRESET' script line ran (-1: never). */
        volatile int resetAt = -1;
        final List<String> script;
        volatile int scriptPos;
        final AtomicInteger monitorStarts = new AtomicInteger();
        private final StringBuilder cmd = new StringBuilder();
        private Thread emitter;
        final long lineMs;

        FakeElm(List<String> script, long lineMs) { this.script = script; this.lineMs = lineMs; }

        synchronized void send(String s) {
            if (closed) return;
            due.add(new long[] {System.nanoTime() + latencyMs * 1_000_000L});
            toPhone.add(s.getBytes(StandardCharsets.US_ASCII));
        }

        final InputStream in = new InputStream() {
            byte[] cur;
            int pos;
            @Override public int read() throws IOException { byte[] b = new byte[1]; return read(b, 0, 1) < 0 ? -1 : b[0] & 0xFF; }
            @Override public int read(byte[] b, int off, int len) throws IOException {
                while (cur == null || pos >= cur.length) {
                    if (closed) throw new IOException("closed");
                    try { cur = toPhone.poll(20, TimeUnit.MILLISECONDS); } catch (InterruptedException e) { throw new InterruptedIOException(); }
                    pos = 0;
                    if (cur != null) {
                        long at = due.poll()[0];   // same order as toPhone (both added under the fake's lock)
                        while (System.nanoTime() < at) {
                            if (closed) throw new IOException("closed");
                            try { Thread.sleep(1); } catch (InterruptedException e) { throw new InterruptedIOException(); }
                        }
                    }
                }
                int n = Math.min(len, cur.length - pos);
                System.arraycopy(cur, pos, b, off, n);
                pos += n;
                return n;
            }
        };

        final OutputStream out = new OutputStream() {
            @Override public void write(int b) throws IOException { write(new byte[] {(byte) b}, 0, 1); }
            @Override public synchronized void write(byte[] b, int off, int len) throws IOException {
                while (blockWrites) sleep(5);   // a stuck Bluetooth write (not even close() frees it)
                if (closed) throw new IOException("closed");
                fromPhone.write(b, off, len);
                for (int i = off; i < off + len; i++) onByte((char) (b[i] & 0xFF));
            }
        };

        private synchronized void onByte(char c) {
            if (monitoring) {
                monitoring = false;
                send("\r>");
                if (retainStop) cmd.append(c);
                return;
            }
            if (c != '\r') { cmd.append(c); return; }
            String raw = cmd.toString();
            cmd.setLength(0);
            String k = raw.replace(" ", "").toUpperCase(Locale.ROOT);
            commands.add(k);
            if (echo) send(raw + "\r");
            if (refuse.contains(k) || !k.startsWith("AT")) { send("?\r\r>"); return; }
            switch (k) {
                case "ATZ":
                    resetSettings();
                    if (leftoverOnAtz) send("079DF285A01FFD530A5\r");
                    send("\r\rELM327 v1.5\r\r>" + (doublePromptAfterAtz ? "\r>" : ""));
                    return;
                case "ATE0": echo = ignoreAte0; send("OK\r\r>"); return;
                case "ATH1": headers = true; send("OK\r\r>"); return;
                case "ATSP6": protocol = "ISO 15765-4 (CAN 11/500)"; send("OK\r\r>"); return;
                case "ATCRA079": filterSet = true; send("OK\r\r>"); return;
                case "ATDP": send(protocol + "\r\r>"); return;
                case "ATMA":
                    monitoring = true;
                    monitorStarts.incrementAndGet();
                    startEmitter();
                    return;
                default: send("OK\r\r>");
            }
        }

        /** What ATZ or a low-voltage reset does: echo on, no headers, no filter, protocol kept or AUTO. */
        private void resetSettings() {
            echo = true;
            headers = false;
            filterSet = false;
            if (!keepProtocol) protocol = "AUTO";
        }

        private void startEmitter() {
            if (emitter != null) return;
            emitter = new Thread(() -> {
                while (!closed) {
                    if (!monitoring || scriptPos >= script.size()) { sleep(5); continue; }
                    String l = script.get(scriptPos);
                    if (l.startsWith("SLEEP ")) { scriptPos++; sleep(Long.parseLong(l.substring(6))); continue; }
                    synchronized (this) {
                        if (!monitoring) continue;
                        scriptPos++;
                        if (l.equals("BUFFER FULL")) { monitoring = false; send("BUFFER FULL\r\r>"); continue; }
                        // Adapter resets in the middle of a monitor: settings back to defaults.
                        if (l.equals("RESET")) { monitoring = false; resetSettings(); resetAt = commands.size(); send("\r\rLV RESET\r\r>"); continue; }
                        if (l.equals("BANNER")) { monitoring = false; resetSettings(); resetAt = commands.size(); send("\r\rELM327 v1.5\r\r>"); continue; }
                        if (l.equals("PROMPT")) { monitoring = false; resetAt = commands.size(); send("\r>"); continue; }
                        if (l.equals("SILENTRESET")) { monitoring = false; resetSettings(); resetAt = commands.size(); continue; }
                        send(l + "\r");
                    }
                    sleep(lineMs);
                }
            }, "fake-elm");
            emitter.setDaemon(true);
            emitter.start();
        }

        static void sleep(long ms) { try { Thread.sleep(ms); } catch (InterruptedException e) { Thread.currentThread().interrupt(); } }

        String written() { synchronized (out) { return new String(fromPhone.toByteArray(), StandardCharsets.US_ASCII); } }

        @Override public InputStream input() { return in; }
        @Override public OutputStream output() { return out; }
        @Override public void close() { closed = true; }
    }

    static final class Recorder implements ElmController.Listener {
        final List<ElmLineParser.Event> frames = new CopyOnWriteArrayList<>();
        final List<String> events = new CopyOnWriteArrayList<>();
        final List<String> starts = new CopyOnWriteArrayList<>();
        @Override public void frame(ElmLineParser.Event e) { frames.add(e); }
        @Override public void monitorStarted(int count, String why) { starts.add(why); }
        @Override public void event(String line) { events.add(line); }
    }

    static List<String> lines079(int n, int counter0) {
        List<String> l = new ArrayList<>();
        for (int i = 0; i < n; i++) l.add(String.format(Locale.ROOT, "079DF285A01FFD53%XA5", (counter0 + i) & 15));
        return l;
    }

    static Thread runAsync(ElmController c, List<Throwable> err) {
        Thread t = new Thread(() -> {
            try { c.run(); } catch (Throwable e) { err.add(e); }
        }, "elm-test");
        t.start();
        return t;
    }

    static boolean waitFor(java.util.function.BooleanSupplier c, long ms) throws InterruptedException {
        long end = System.currentTimeMillis() + ms;
        while (!c.getAsBoolean()) { if (System.currentTimeMillis() > end) return false; Thread.sleep(2); }
        return true;
    }

    /** Every byte the controller wrote is an AT command line or the single stop character. */
    static boolean onlySafeWrites(List<String> written) {
        for (String s : written) {
            if (s.equals("X")) continue;
            if (!s.endsWith("\r")) return false;
            if (!ElmController.SAFE_COMMAND.matcher(s.substring(0, s.length() - 1)).matches()) return false;
        }
        return true;
    }

    static void controllerInit() throws Exception {
        List<String> script = new ArrayList<>(lines079(40, 0));
        script.add(2, "07800112233445566");       // a foreign id leaking through
        script.add(3, "079DF285A01FFD531A5<RX ERROR");
        FakeElm elm = new FakeElm(script, 2);
        Recorder r = new Recorder();
        ElmController c = new ElmController(elm, Clock.SYSTEM, r, 0x079, FAST);
        List<Throwable> err = Collections.synchronizedList(new ArrayList<>());
        Thread t = runAsync(c, err);
        check(waitFor(() -> r.frames.size() >= 40, 3000), "init + monitor: 40 frames received (" + r.frames.size() + ")");
        c.stop();
        t.join(2000);
        // An idle adapter keeps the stop character: "XATZ" -> '?', then ATZ is retried once.
        List<String> want = java.util.Arrays.asList("XATZ", "ATZ", "ATDP", "ATE0", "ATL0", "ATS0", "ATH1", "ATSP6", "ATCAF0", "ATCRA079", "ATMA");
        check(elm.commands.subList(0, Math.min(11, elm.commands.size())).equals(want), "init sequence " + elm.commands);
        eq(c.foreignFrames, 1, "foreign id counted, not delivered");
        eq(c.errorLines, 1, "RX ERROR line counted");
        check(onlySafeWrites(c.written()), "only AT commands and 'X' were written: " + c.written());
        check(!elm.written().matches("(?s).*\r[0-9A-Fa-f ]+\r.*"), "no OBD request (hex-only line) on the wire");
        check(c.version.contains("ELM327"), "version recorded: " + c.version);
        check(r.starts.size() == 1 && r.starts.get(0).equals("init"), "one monitor start");
    }

    static void controllerRestarts() throws Exception {
        List<String> script = new ArrayList<>(lines079(20, 0));
        script.add("BUFFER FULL");
        script.addAll(lines079(20, 4));
        script.add("SLEEP 600");                     // a stall longer than FAST.stallMs (250 ms)
        script.addAll(lines079(20, 8));
        FakeElm elm = new FakeElm(script, 2);
        Recorder r = new Recorder();
        ElmController c = new ElmController(elm, Clock.SYSTEM, r, 0x079, FAST);
        List<Throwable> err = Collections.synchronizedList(new ArrayList<>());
        Thread t = runAsync(c, err);
        check(waitFor(() -> r.frames.size() >= 60, 5000), "frames continue after BUFFER FULL and a stall (" + r.frames.size() + ")");
        c.stop();
        t.join(2000);
        eq(c.bufferFull, 1, "BUFFER FULL counted");
        check(c.stalls >= 1, "stall detected");
        check(r.starts.contains("prompt") && r.starts.contains("stall"), "restarts after prompt and stall: " + r.starts);
        check(elm.monitorStarts.get() >= 3, "ATMA re-sent");
        check(onlySafeWrites(c.written()), "restarts write only AT commands and 'X'");
    }

    static void controllerQuestionStop() throws Exception {
        // A clone that keeps the stop character: "X" + "ATMA" -> "?", retried once.
        List<String> script = new ArrayList<>(lines079(10, 0));
        script.add("SLEEP 600");
        script.addAll(lines079(10, 10));
        FakeElm elm = new FakeElm(script, 2);
        elm.retainStop = true;
        Recorder r = new Recorder();
        ElmController c = new ElmController(elm, Clock.SYSTEM, r, 0x079, FAST);
        List<Throwable> err = Collections.synchronizedList(new ArrayList<>());
        Thread t = runAsync(c, err);
        check(waitFor(() -> r.frames.size() >= 20, 5000), "retained stop character: monitor resumes (" + r.frames.size() + ")");
        c.stop();
        t.join(2000);
        check(r.events.contains("elm_atma_question_retry"), "'?' after the stop character retried: " + r.events);
    }

    static void controllerNoTraffic() throws Exception {
        FakeElm elm = new FakeElm(new ArrayList<>(), 2);   // ignition off: the bus is silent
        Recorder r = new Recorder();
        ElmController c = new ElmController(elm, Clock.SYSTEM, r, 0x079, FAST);
        long t0 = System.currentTimeMillis();
        String why = null;
        try { c.run(); } catch (IOException e) { why = e.getMessage(); }
        long ms = System.currentTimeMillis() - t0;
        check(why != null && why.startsWith("no_traffic"), "silent bus ends the run: " + why);
        eq(c.inits, ElmController.MAX_REINITS, "three inits before giving up");
        check(ms < 10000, "bounded time " + ms + " ms");
        check(elm.closed, "transport closed");
    }

    static void controllerRefusedCommand() throws Exception {
        FakeElm elm = new FakeElm(lines079(5, 0), 2);
        elm.refuse.add("ATCAF0");
        Recorder r = new Recorder();
        ElmController c = new ElmController(elm, Clock.SYSTEM, r, 0x079, FAST);
        String why = null;
        try { c.run(); } catch (IOException e) { why = e.getMessage(); }
        check(why != null && why.startsWith("init ATCAF0"), "a refused required command fails the init: " + why);
        check(!elm.commands.contains("ATMA"), "no monitor without the full init");
        FakeElm e2 = new FakeElm(lines079(30, 0), 2);
        e2.refuse.add("ATS0");                         // spaces stay on: optional
        Recorder r2 = new Recorder();
        ElmController c2 = new ElmController(e2, Clock.SYSTEM, r2, 0x079, FAST);
        List<Throwable> err = Collections.synchronizedList(new ArrayList<>());
        Thread t = runAsync(c2, err);
        check(waitFor(() -> r2.frames.size() >= 30, 3000), "ATS0 refused: monitor still runs");
        c2.stop();
        t.join(2000);
    }

    /**
     * F1: an adapter reset in the middle of ATMA (low-voltage reset, power-on banner, a prompt that is
     * not the BUFFER FULL prompt, or a silent reset seen only as an echoed ATMA) must lead to a full
     * init (ATZ first), never to a bare ATMA on an adapter back at its defaults.
     */
    static void controllerAdapterReset() throws Exception {
        for (String kind : new String[] {"RESET", "BANNER", "PROMPT"}) {
            List<String> script = new ArrayList<>(lines079(20, 0));
            script.add(kind);
            script.addAll(lines079(40, 4));
            FakeElm elm = new FakeElm(script, 2);
            elm.keepProtocol = false;
            Recorder r = new Recorder();
            ElmController c = new ElmController(elm, Clock.SYSTEM, r, 0x079, FAST);
            List<Throwable> err = Collections.synchronizedList(new ArrayList<>());
            Thread t = runAsync(c, err);
            check(waitFor(() -> r.frames.size() >= 60, 5000), kind + ": frames resume after the reset (" + r.frames.size() + ")");
            c.stop();
            t.join(2000);
            int at = elm.resetAt;
            check(at > 0 && at < elm.commands.size(), kind + ": reset happened and commands followed " + elm.commands);
            if (at <= 0 || at >= elm.commands.size()) continue;
            List<String> after = elm.commands.subList(at, elm.commands.size());
            check(after.get(0).endsWith("ATZ"), kind + ": the first command after the reset is ATZ, not ATMA: " + after);
            int atz = -1;
            for (int i = 0; i < after.size(); i++) if (after.get(i).equals("ATZ")) { atz = i; break; }
            int firstAtma = after.indexOf("ATMA");
            check(atz >= 0 && firstAtma > atz, kind + ": no bare ATMA before the full init: " + after);
            check(after.contains("ATCRA079") && after.indexOf("ATCRA079") < firstAtma, kind + ": filter re-set before ATMA: " + after);
            check(c.inits == 2, kind + ": exactly one re-init: " + c.inits);
            check(c.adapterResets == 1, kind + ": adapter reset counted: " + c.adapterResets + " " + r.events + " " + elm.commands);
            check(onlySafeWrites(c.written()), kind + ": only AT commands and 'X'");
            check(c.protocolAfterReset.equals("AUTO"), kind + ": ATDP after ATZ logged (clone that forgets ATSP6): " + c.protocolAfterReset);
        }
        // A silent reset (no text, no prompt): the stall restart's ATMA is echoed (echo is on again),
        // which must also lead straight to the full init.
        List<String> script = new ArrayList<>(lines079(20, 0));
        script.add("SILENTRESET");
        script.addAll(lines079(40, 4));
        FakeElm elm = new FakeElm(script, 2);
        Recorder r = new Recorder();
        ElmController c = new ElmController(elm, Clock.SYSTEM, r, 0x079, FAST);
        List<Throwable> err = Collections.synchronizedList(new ArrayList<>());
        Thread t = runAsync(c, err);
        check(waitFor(() -> r.frames.size() >= 60, 6000), "silent reset: frames resume (" + r.frames.size() + ")");
        c.stop();
        t.join(2000);
        List<String> after = elm.resetAt >= 0 ? elm.commands.subList(elm.resetAt, elm.commands.size()) : new ArrayList<>();
        int atz = after.indexOf("ATZ");
        int atmaBefore = 0;
        for (int i = 0; i < (atz < 0 ? after.size() : atz); i++) if (after.get(i).endsWith("ATMA")) atmaBefore++;
        check(atz >= 0 && atmaBefore <= 1, "silent reset: at most the one stall ATMA, then ATZ: " + after);
        check(r.events.toString().contains("elm_adapter_reset reinit"), "silent reset: echoed ATMA recognised: " + r.events);
        check(elm.protocol.startsWith("ISO"), "default fake keeps ATSP6 over ATZ");
    }

    /** Six restarts inside the restart window (BUFFER FULL storm) re-run the whole init. */
    static void controllerRestartStorm() throws Exception {
        List<String> script = new ArrayList<>(lines079(5, 0));
        for (int i = 0; i < 7; i++) { script.add("BUFFER FULL"); script.addAll(lines079(3, i)); }
        script.addAll(lines079(30, 0));
        FakeElm elm = new FakeElm(script, 2);
        Recorder r = new Recorder();
        ElmController c = new ElmController(elm, Clock.SYSTEM, r, 0x079, FAST);
        List<Throwable> err = Collections.synchronizedList(new ArrayList<>());
        Thread t = runAsync(c, err);
        check(waitFor(() -> r.frames.size() >= 5 + 21 + 30, 6000), "storm: frames continue (" + r.frames.size() + ")");
        c.stop();
        t.join(2000);
        check(r.events.contains("elm_restart_storm reinit"), "storm: logged " + r.events);
        check(c.inits >= 2, "storm: the 6th restart within the window re-runs the init: inits " + c.inits);
        int atz = 0;
        for (String k : elm.commands) if (k.endsWith("ATZ")) atz++;
        check(atz >= 3, "storm: ATZ sent again (" + elm.commands + ")");
        check(c.adapterResets == 0, "storm: BUFFER FULL prompts are not adapter resets " + r.events);
    }

    /** F6: a clone that prints two prompts after ATZ; the stray prompt must not answer ATE0. */
    static void controllerDoublePrompt() throws Exception {
        FakeElm elm = new FakeElm(lines079(30, 0), 2);
        elm.doublePromptAfterAtz = true;
        elm.leftoverOnAtz = true;            // and a leftover monitor line before the banner
        Recorder r = new Recorder();
        ElmController c = new ElmController(elm, Clock.SYSTEM, r, 0x079, FAST);
        List<Throwable> err = Collections.synchronizedList(new ArrayList<>());
        Thread t = runAsync(c, err);
        check(waitFor(() -> r.frames.size() >= 30, 4000), "double prompt after ATZ: init completes, frames flow (" + r.frames.size() + ") " + err);
        c.stop();
        t.join(2000);
        check(c.strayPrompts >= 1, "stray prompt drained: " + c.strayPrompts);
        check(!c.version.contains("079"), "leftover CAN line is not part of the version text: " + c.version);
    }

    /** '?' to ATMA forever: one retry per monitor, then a full init; the run ends, bounded. */
    static void controllerAtmaAlwaysRefused() throws Exception {
        FakeElm elm = new FakeElm(lines079(5, 0), 2);
        elm.refuse.add("ATMA");
        Recorder r = new Recorder();
        ElmController c = new ElmController(elm, Clock.SYSTEM, r, 0x079, FAST);
        List<Throwable> err = Collections.synchronizedList(new ArrayList<>());
        Thread t = runAsync(c, err);
        t.join(15000);
        boolean ended = !t.isAlive();
        if (!ended) { c.stop(); t.join(2000); }
        check(ended, "ATMA always refused: the run ends by itself");
        int atma = 0;
        for (String k : elm.commands) if (k.endsWith("ATMA")) atma++;
        check(atma <= 2 * ElmController.MAX_REINITS, "ATMA retried at most once per init: " + atma);
    }

    /** F5: a normal stop leaves the adapter at its prompt (one 'X'), bounded when the write hangs. */
    static void controllerStopSendsX() throws Exception {
        FakeElm elm = new FakeElm(lines079(400, 0), 5);
        Recorder r = new Recorder();
        ElmController c = new ElmController(elm, Clock.SYSTEM, r, 0x079, FAST);
        List<Throwable> err = Collections.synchronizedList(new ArrayList<>());
        Thread t = runAsync(c, err);
        check(waitFor(() -> r.frames.size() >= 10, 3000), "stop test: monitoring");
        c.stop();
        t.join(2000);
        List<String> w = c.written();
        check(!w.isEmpty() && w.get(w.size() - 1).equals("X"), "normal stop: last write is the stop character: " + w);
        check(!elm.monitoring, "normal stop: the adapter left monitoring");
        // a hanging Bluetooth write: stop() still returns within the bound
        FakeElm hang = new FakeElm(lines079(400, 0), 5);
        Recorder r2 = new Recorder();
        ElmController c2 = new ElmController(hang, Clock.SYSTEM, r2, 0x079, FAST);
        Thread t2 = runAsync(c2, err);
        check(waitFor(() -> r2.frames.size() >= 10, 3000), "stop test 2: monitoring");
        hang.blockWrites = true;
        long t0 = System.nanoTime();
        c2.stop();
        long ms = (System.nanoTime() - t0) / 1_000_000L;
        t2.join(2000);
        check(ms < ElmController.STOP_X_MS + 300, "stop with a hanging write returns in " + ms + " ms");
        check(hang.closed, "stop with a hanging write: transport closed");
        hang.blockWrites = false;   // release the abandoned helper thread
        // an idle controller (no monitor) writes no 'X' on stop
        FakeElm idle = new FakeElm(new ArrayList<>(), 2);
        idle.refuse.add("ATCAF0");
        ElmController c3 = new ElmController(idle, Clock.SYSTEM, new Recorder(), 0x079, FAST);
        try { c3.run(); } catch (IOException expected) { /* refused init */ }
        int before = c3.written().size();
        c3.stop();
        eq(c3.written().size(), before, "stop without a running monitor writes nothing");
    }

    /**
     * Review 2 R1: adapter-to-phone latency (400 ms) above stopMs (FAST: 300 ms). The prompt that answers
     * a stall's 'X' arrives after the stall restart's ATMA; it must be taken as the late answer to our
     * stop (the monitor goes on), not as an adapter reset (full re-init, which under the same latency
     * used to fail with 'init ATE0' and end the run).
     */
    static void controllerLateStopPrompt() throws Exception {
        List<String> script = new ArrayList<>(lines079(20, 0));
        script.add("SLEEP 600");                   // stall > FAST.stallMs 250
        script.addAll(lines079(40, 4));
        script.add("SLEEP 600");
        script.addAll(lines079(40, 12));
        FakeElm elm = new FakeElm(script, 2);
        elm.latencyMs = 400;
        Recorder r = new Recorder();
        ElmController c = new ElmController(elm, Clock.SYSTEM, r, 0x079, FAST);
        List<Throwable> err = Collections.synchronizedList(new ArrayList<>());
        Thread t = runAsync(c, err);
        check(waitFor(() -> r.frames.size() >= 100, 15000), "late stop prompt (400 ms > stopMs): all 100 frames arrive (" + r.frames.size() + ")");
        boolean alive = t.isAlive();
        String errBefore = err.toString();
        c.stop();
        t.join(3000);
        check(alive && errBefore.equals("[]"), "late stop prompt: the run kept the connection: " + errBefore);
        check(!r.events.toString().contains("init ATE0"), "late stop prompt: no 'init ATE0' failure: " + r.events);
        eq(c.adapterResets, 0, "late stop prompt: not counted as an adapter reset");
        eq(c.inits, 1, "late stop prompt: no full re-init");
        check(c.lateStopPrompts >= 1 && r.events.toString().contains("elm_late_stop_prompt"), "late stop prompt logged: " + c.lateStopPrompts + " " + r.events);
        check(onlySafeWrites(c.written()), "late stop prompt: only AT commands and 'X'");
        // A prompt that is NOT within 2 x stopMs of our 'X' is still an adapter reset (F1 unchanged):
        // covered by controllerAdapterReset ('PROMPT' script line, no 'X' before it).
    }

    /**
     * Review 2 R1(b): the adapter is still monitoring when we connect (a previous client left ATMA
     * running) and the latency exceeds stopMs, so the init's 'X' is answered only after ATZ was sent.
     * That bare prompt must not be taken as the ATZ answer (it shifted every later answer by one).
     */
    static void controllerLateStopPromptAtInit() throws Exception {
        FakeElm elm = new FakeElm(lines079(40, 0), 2);
        elm.latencyMs = 400;
        elm.monitoring = true;
        Recorder r = new Recorder();
        ElmController c = new ElmController(elm, Clock.SYSTEM, r, 0x079, FAST);
        List<Throwable> err = Collections.synchronizedList(new ArrayList<>());
        Thread t = runAsync(c, err);
        check(waitFor(() -> r.frames.size() >= 40, 10000), "late prompt at init: init completes and frames flow (" + r.frames.size() + ") " + err + " " + r.events);
        c.stop();
        t.join(3000);
        check(c.version.contains("ELM327"), "late prompt at init: the ATZ answer is the banner, not the bare prompt: '" + c.version + "'");
        check(c.atzBarePrompts >= 1, "late prompt at init: the bare prompt was recognised: " + c.atzBarePrompts);
        eq(c.inits, 1, "late prompt at init: one init");
        check(c.protocolAfterReset.equals("AUTO"), "late prompt at init: ATDP got its own answer (AUTO before any ATSP6), not the banner: " + c.protocolAfterReset);
    }

    /**
     * Review 2 R2: a clone that answers OK to ATE0 but keeps echoing. The echoed ATMA must not be read
     * as an adapter reset (it used to re-init forever and never monitor).
     */
    static void controllerEchoPersists() throws Exception {
        FakeElm elm = new FakeElm(lines079(40, 0), 2);
        elm.ignoreAte0 = true;
        Recorder r = new Recorder();
        ElmController c = new ElmController(elm, Clock.SYSTEM, r, 0x079, FAST);
        List<Throwable> err = Collections.synchronizedList(new ArrayList<>());
        Thread t = runAsync(c, err);
        check(waitFor(() -> r.frames.size() >= 40, 5000), "echo persists: the monitor starts and frames flow (" + r.frames.size() + ") " + r.events);
        c.stop();
        t.join(2000);
        check(!c.echoVerifiedOff, "echo persists: echo not verified off");
        eq(c.adapterResets, 0, "echo persists: the echoed ATMA is not an adapter reset");
        int logged = 0;
        for (String e : r.events) if (e.startsWith("elm_echo_persists")) logged++;
        eq(logged, 1, "echo persists: logged once");
        check(ElmController.adapterResetLine("ATMA", true) && !ElmController.adapterResetLine("ATMA", false)
                && ElmController.adapterResetLine("LV RESET", false), "echoed ATMA is a reset sign only when echo was verified off");
    }

    /**
     * Review 2 R3: stop() lands after the monitor loop's stopped check but before the restart's ATMA
     * (forced here: the controller's clock calls stop() from inside restart()). No ATMA may follow the
     * stop character, or the adapter is left monitoring.
     */
    static void controllerNoAtmaAfterStop() throws Exception {
        List<String> script = new ArrayList<>(lines079(20, 0));
        script.add("BUFFER FULL");
        script.addAll(lines079(20, 4));
        FakeElm elm = new FakeElm(script, 2);
        final ElmController[] holder = new ElmController[1];
        final long[] writtenAtStop = {-1};
        final java.util.concurrent.atomic.AtomicBoolean armed = new java.util.concurrent.atomic.AtomicBoolean();
        Clock clk = () -> {
            if (armed.get() && "elm-test".equals(Thread.currentThread().getName())) {
                for (StackTraceElement f : Thread.currentThread().getStackTrace()) {
                    if (f.getClassName().endsWith("ElmController") && f.getMethodName().equals("restart") && armed.compareAndSet(true, false)) {
                        holder[0].stop();
                        writtenAtStop[0] = holder[0].writtenTotal;
                        break;
                    }
                }
            }
            return System.nanoTime();
        };
        ElmController.Listener l = new ElmController.Listener() {
            @Override public void frame(ElmLineParser.Event e) {}
            @Override public void monitorStarted(int count, String why) {}
            @Override public void event(String line) { if (line.equals("elm_buffer_full")) armed.set(true); }
        };
        ElmController c = new ElmController(elm, clk, l, 0x079, FAST);
        holder[0] = c;
        List<Throwable> err = Collections.synchronizedList(new ArrayList<>());
        Thread t = runAsync(c, err);
        t.join(5000);
        check(!t.isAlive(), "stop inside restart: the run ended");
        check(writtenAtStop[0] > 0, "stop inside restart: stop() was forced inside restart()");
        eq(c.writtenTotal, writtenAtStop[0], "stop inside restart: no ATMA (nothing) written after stop(): " + c.written());
    }

    /**
     * Review 2 R5: after the give-up the link keeps one slow attempt every GIVEUP_RETRY_MS (5 minutes,
     * injected clock) instead of never trying again in that session; a free adapter is picked up again.
     */
    static void linkSlowRetryAfterGiveUp() throws Exception {
        final java.util.concurrent.atomic.AtomicLong now = new java.util.concurrent.atomic.AtomicLong(1_000_000_000L);
        Clock clk = now::get;
        AtomicInteger opens = new AtomicInteger();
        final java.util.concurrent.atomic.AtomicBoolean free = new java.util.concurrent.atomic.AtomicBoolean();
        Transport.Connector conn = new Transport.Connector() {
            @Override public Transport open() throws IOException {
                opens.incrementAndGet();
                if (!free.get()) throw new IOException("busy (Car Scanner holds it)");
                FakeElm e = new FakeElm(new ArrayList<>(), 1);
                e.refuse.add("ATCAF0");     // connected; the run ends at once
                return e;
            }
            @Override public void abort() {}
        };
        final List<String> ev = new CopyOnWriteArrayList<>();
        ObdLink link = new ObdLink(conn, clk, new ObdLink.Sink() {
            @Override public void window(Window w) {}
            @Override public void event(String line) { ev.add(line); }
        }, FAST, 10, 40);
        link.start();
        check(waitFor(() -> link.gaveUp, 3000), "slow retry: the link gives up fast retries");
        eq(opens.get(), ObdLink.MAX_CONNECT_FAILURES, "slow retry: 5 fast attempts");
        now.addAndGet((ObdLink.GIVEUP_RETRY_MS - 1000) * 1_000_000L);
        Thread.sleep(1200);
        eq(opens.get(), ObdLink.MAX_CONNECT_FAILURES, "slow retry: no attempt before 5 minutes");
        now.addAndGet(1000 * 1_000_000L);
        check(waitFor(() -> opens.get() == ObdLink.MAX_CONNECT_FAILURES + 1, 2000), "slow retry: one attempt at 5 minutes (" + opens.get() + ")");
        Thread.sleep(1200);
        eq(opens.get(), ObdLink.MAX_CONNECT_FAILURES + 1, "slow retry: at most one attempt per 5 minutes");
        check(link.gaveUp && ev.toString().contains("obd_connect_slow_retry n=1"), "slow retry logged: " + ev);
        free.set(true);
        now.addAndGet(ObdLink.GIVEUP_RETRY_MS * 1_000_000L);
        check(waitFor(() -> link.connects >= 1, 2000), "slow retry: a freed adapter is connected again (" + link.connects + ")");
        link.stop();
        check(link.slowRetries >= 2, "slow retries counted: " + link.slowRetries);
    }

    /**
     * Review 2 R6: a CMU session that starts while the parked check holds the adapter is deferred, not
     * dropped: its link starts as soon as the check ends, unless the session ended first.
     */
    static void arbiterDefersSessionStart() {
        ObdArbiter a = new ObdArbiter();
        check(a.tryStartCheck(), "arbiter: the check starts on a free adapter");
        AtomicInteger started = new AtomicInteger();
        Runnable startLink = () -> { if (a.startServiceOrDefer(() -> {})) started.incrementAndGet(); };
        check(!a.startServiceOrDefer(startLink), "arbiter: a session start during the check is deferred");
        check(!a.serviceRunning() && a.hasDeferred(), "arbiter: not running, start pending");
        a.endCheck();
        eq(started.get(), 1, "arbiter: the deferred start runs once when the check ends");
        check(a.serviceRunning() && !a.hasDeferred(), "arbiter: the session's link now holds the adapter");
        check(!a.tryStartCheck(), "arbiter: the check is refused while the link holds the adapter");
        a.serviceStopped();
        check(a.tryStartCheck(), "arbiter: free again after the link stopped");
        AtomicInteger late = new AtomicInteger();
        check(!a.startServiceOrDefer(late::incrementAndGet), "arbiter: second deferral");
        a.cancelDeferred();   // the session ended before the check
        a.endCheck();
        eq(late.get(), 0, "arbiter: a cancelled deferred start does not run");
        check(!a.serviceRunning() && !a.checkRunning(), "arbiter: idle at the end");
    }

    /**
     * Review 2 R4: the verdict rule's boundary cases, asserted on ObdLossMeter.verdictOf and compared
     * row by row with the table between the verdict-cases markers in the owner document, plus the
     * result-line example of that document and the 100 % display clamp.
     */
    static final Object[][] VERDICT_CASES = {
        // coverage %, restarts, loss %, stalls over 0.5 s, span s, expected
        {90.0, 1L, 5.0, 0L, 30L, "GOOD"},
        {89.0, 0L, 0.0, 0L, 60L, "MARGINAL"},
        {95.0, 2L, 0.0, 0L, 60L, "MARGINAL"},
        {95.0, 0L, 5.1, 0L, 60L, "MARGINAL"},
        {95.0, 0L, 0.0, 1L, 60L, "MARGINAL"},
        {95.0, 0L, 0.0, 0L, 29L, "MARGINAL"},
        {60.0, 5L, 0.0, 0L, 60L, "MARGINAL"},
        {59.0, 2L, 0.0, 0L, 60L, "BAD"},
        {50.0, 0L, 0.0, 0L, 60L, "BAD"},
    };

    static void verdictTable(String docPath) throws IOException {
        for (Object[] k : VERDICT_CASES) {
            String got = ObdLossMeter.verdictOf(1000, (Long) k[4] * 1_000_000_000L, (Double) k[0] / 100, (Double) k[2], (Long) k[3], (Long) k[1]);
            check(got.equals(k[5]), "verdict case " + java.util.Arrays.toString(k) + " -> " + got);
        }
        eq(ObdLossMeter.verdictOf(0, 60_000_000_000L, 1, 0, 0, 0).equals("NONE") ? 1 : 0, 1, "verdict: no frames is NONE");
        String doc = new String(Files.readAllBytes(Paths.get(docPath)), StandardCharsets.UTF_8);
        int b = doc.indexOf("<!-- verdict-cases:begin -->"), e = doc.indexOf("<!-- verdict-cases:end -->");
        check(b >= 0 && e > b, "owner doc has the verdict-cases table");
        if (b < 0 || e <= b) return;
        List<String[]> rows = new ArrayList<>();
        for (String line : doc.substring(b, e).split("\n")) {
            String[] cells = line.trim().split("\\|");
            if (cells.length < 7 || !cells[1].trim().matches("[0-9.]+")) continue;   // header, rule, prose
            String[] r = new String[6];
            for (int i = 0; i < 6; i++) r[i] = cells[i + 1].trim();
            rows.add(r);
        }
        eq(rows.size(), VERDICT_CASES.length, "owner doc verdict table: same number of rows as the code table");
        for (int i = 0; i < Math.min(rows.size(), VERDICT_CASES.length); i++) {
            Object[] k = VERDICT_CASES[i];
            String[] r = rows.get(i);
            boolean same = Double.parseDouble(r[0]) == (Double) k[0] && Long.parseLong(r[1]) == (Long) k[1]
                    && Double.parseDouble(r[2]) == (Double) k[2] && Long.parseLong(r[3]) == (Long) k[3] && Long.parseLong(r[4]) == (Long) k[4];
            String word = ObdLossMeter.verdictWord((String) k[5]);
            check(same && r[5].startsWith(word), "owner doc row " + (i + 1) + " = code case " + java.util.Arrays.toString(k) + " (" + word + "): " + String.join(" | ", r));
            String fromDoc = ObdLossMeter.verdictOf(1000, Long.parseLong(r[4]) * 1_000_000_000L, Double.parseDouble(r[0]) / 100,
                    Double.parseDouble(r[2]), Long.parseLong(r[3]), Long.parseLong(r[1]));
            check(r[5].startsWith(ObdLossMeter.verdictWord(fromDoc)), "owner doc row " + (i + 1) + " agrees with verdictOf: " + fromDoc);
        }
        // the result-line examples of the owner doc are exactly what the app prints
        String good = ObdLossMeter.resultLine("GOOD", 0.98, 0, 0.0, 2900, new ArrayList<>());
        check(doc.contains("`" + good + "`"), "owner doc shows the GOOD result line: " + good);
        List<String> why = ObdLossMeter.notGoodReasons(60_000_000_000L, 0.85, 0.0, 0, 2);
        String marginal = ObdLossMeter.resultLine("MARGINAL", 0.85, 2, 0.0, 2550, why);
        check(doc.contains("`" + marginal + "`"), "owner doc shows the MARGINAL result line: " + marginal);
        check(ObdLossMeter.resultLine("MARGINAL", 1.3, 0, 0.0, 2, ObdLossMeter.notGoodReasons(0, 1.3, 0, 0, 0)).contains("덮개율 100%(기준 100)"),
                "displayed coverage is clamped to 100 %");
    }

    /** F3: an absent adapter is paged at most MAX_CONNECT_FAILURES times per link (CMU session). */
    static void linkConnectGiveUp() throws Exception {
        AtomicInteger opens = new AtomicInteger();
        Transport.Connector absent = new Transport.Connector() {
            @Override public Transport open() throws IOException { opens.incrementAndGet(); throw new IOException("page timeout"); }
            @Override public void abort() {}
        };
        final List<String> ev = new CopyOnWriteArrayList<>();
        ObdLink.Sink sink = new ObdLink.Sink() {
            @Override public void window(Window w) {}
            @Override public void event(String line) { ev.add(line); }
        };
        ObdLink link = new ObdLink(absent, Clock.SYSTEM, sink, FAST, 10, 40);
        long t0 = System.currentTimeMillis();
        link.start();
        check(waitFor(() -> link.gaveUp, 3000), "absent adapter: the link gives up");
        long ms = System.currentTimeMillis() - t0;
        Thread.sleep(300);
        eq(opens.get(), ObdLink.MAX_CONNECT_FAILURES, "absent adapter: exactly 5 connect attempts in this session");
        check(ev.toString().contains("obd_connect_giveup failures=5"), "giveup logged: " + ev);
        check(ms >= 10 + 20 + 40 + 40 - 5, "the backoff is kept between attempts (" + ms + " ms)");
        link.stop();
        // the next CMU session makes a new link, which tries again
        ObdLink next = new ObdLink(absent, Clock.SYSTEM, sink, FAST, 10, 40);
        next.start();
        check(waitFor(() -> next.gaveUp, 3000), "next session: tries again and gives up again");
        eq(opens.get(), 2 * ObdLink.MAX_CONNECT_FAILURES, "next session: 5 more attempts");
        next.stop();
        // failures that are not consecutive do not add up: open fails, works (no frames), fails ...
        AtomicInteger n = new AtomicInteger();
        Transport.Connector flaky = new Transport.Connector() {
            @Override public Transport open() throws IOException {
                int k = n.incrementAndGet();
                if (k % 4 != 0) throw new IOException("busy");
                FakeElm e = new FakeElm(new ArrayList<>(), 1);
                e.refuse.add("ATCAF0");     // run ends at once (a connected but refusing adapter)
                return e;
            }
            @Override public void abort() {}
        };
        ObdLink fl = new ObdLink(flaky, Clock.SYSTEM, sink, FAST, 5, 10);
        fl.start();
        check(waitFor(() -> n.get() >= 14, 5000), "flaky adapter: keeps trying (" + n.get() + ")");
        check(!fl.gaveUp, "flaky adapter: a successful connect resets the failure count");
        fl.stop();
    }

    /** F8: a new session's link waits for the previous link's stop before opening the adapter. */
    static void linkWaitsForPredecessor() throws Exception {
        final long[] stopDone = {0};
        Thread oldStop = new Thread(() -> { FakeElm.sleep(300); stopDone[0] = System.nanoTime(); }, "old-stop");
        final long[] firstOpen = {0};
        Transport.Connector conn = new Transport.Connector() {
            @Override public Transport open() throws IOException {
                if (firstOpen[0] == 0) firstOpen[0] = System.nanoTime();
                throw new IOException("busy");
            }
            @Override public void abort() {}
        };
        ObdLink link = new ObdLink(conn, Clock.SYSTEM, new ObdLink.Sink() {
            @Override public void window(Window w) {}
            @Override public void event(String line) {}
        }, FAST, 10, 20);
        oldStop.start();
        link.start(oldStop);
        check(waitFor(() -> firstOpen[0] != 0, 3000), "predecessor test: the new link connects eventually");
        link.stop();
        check(stopDone[0] != 0 && firstOpen[0] > stopDone[0], "the new link opened only after the old link's stop finished");
    }

    /**
     * A receipt gap of more than 8 periods with a counter step of one is ambiguous (Bluetooth delay
     * or a whole lost counter cycle): the window after it carries obd_gap although no loss is counted.
     */
    static void linkAmbiguousGapFlag() throws Exception {
        List<String> script = new ArrayList<>(lines079(10, 0));
        script.add("SLEEP 350");
        script.addAll(lines079(10, 10));            // the counter continues: step 1 across the gap
        final FakeElm elm = new FakeElm(script, 2);
        AtomicInteger opens = new AtomicInteger();
        Transport.Connector conn = new Transport.Connector() {
            @Override public Transport open() throws IOException {
                if (opens.getAndIncrement() > 0) throw new IOException("busy");
                return elm;
            }
            @Override public void abort() {}
        };
        final List<Window> ws = new CopyOnWriteArrayList<>();
        // stall watchdog 1.5 s so the 350 ms gap is not a restart
        ObdLink link = new ObdLink(conn, Clock.SYSTEM, new ObdLink.Sink() {
            @Override public void window(Window w) { ws.add(w); }
            @Override public void event(String line) {}
        }, new ElmController.Timing(500, 500, 300, 1500, 3000, 3000), 50, 100);
        link.start();
        check(waitFor(() -> ws.size() >= 20, 5000), "ambiguous gap: 20 windows (" + ws.size() + ")");
        link.stop();
        int gaps = 0, at = -1;
        for (int i = 0; i < ws.size(); i++) if ((ws.get(i).flags & Protocol.FLAG_OBD_GAP) != 0) { gaps++; at = i; }
        check(gaps == 1 && at == 10, "ambiguous 350 ms gap with counter step 1: obd_gap on the window after it only (" + gaps + " at " + at + ")");
    }

    /**
     * OBSERVED vectors: real 0x079 frames with their 0x078 partner from the public ND CAN log
     * (validation/YAW_DECODE_2026-10-10, sha256 18983a36...). They show that the byte layout applied
     * to captured bytes lands on the same integer lattice as the firmware-derived 0x078 layout and
     * that the counters pair; they do NOT prove the scale, the sign or the owner's car.
     */
    @SuppressWarnings("unchecked")
    static void observedVectors(Map<String, Object> vec) {
        List<Map<String, Object>> obs = list(vec, "observed");
        check(obs != null && obs.size() >= 3 && obs.size() <= 6, "3..6 observed frame pairs");
        if (obs == null) return;
        for (Map<String, Object> o : obs) {
            String name = (String) o.get("name");
            byte[] d79 = hex((String) o.get("data079")), d78 = hex((String) o.get("data078"));
            Can079.Sample s = Can079.decode(0x079, d79);
            check(s.ok(), name + ": decodes OK");
            eq(s.raw12, (Long) o.get("raw079"), name + ": raw12 as recorded");
            int raw78 = ((d78[1] & 1) << 11) | ((d78[2] & 0xFF) << 3) | ((d78[3] & 0xFF) >> 5);   // firmware 0x078 layout
            eq(raw78, (Long) o.get("raw078"), name + ": 0x078 raw (firmware layout)");
            eq(s.counter, (d78[6] & 0xFF) >> 4, name + ": counter pairs with the 0x078 counter");
            long n79 = Math.round((s.raw12 - 2047.00190) / 1.52751423), n78 = Math.round((raw78 - 2047.00190) / 1.52751423);
            eq(Math.round(2047.00190 + 1.52751423 * n79), s.raw12, name + ": 0x079 on the shared lattice");
            eq(Math.round(2047.00190 + 1.52751423 * n78), raw78, name + ": 0x078 on the shared lattice");
            long dn = n79 - n78;
            check(dn >= 0 && dn <= 3, name + ": lattice step difference in 0..3: " + dn);
            eq(dn, (Long) o.get("lattice_diff"), name + ": lattice difference as recorded");
            check(Math.abs(s.cwDegPerS() - num(o.get("cw_dps"))) < 1e-6, name + ": deg/s equals the independent Python decoder");
        }
    }

    // ---- ObdLink end to end; writes the encoded stream for the Python cross-check ----

    @SuppressWarnings("unchecked")
    static void linkEndToEnd(Map<String, Object> vec, String outPath) throws Exception {
        List<String> script = new ArrayList<>();
        for (Map<String, Object> e : list(vec, "can")) script.add((String) e.get("line"));
        script.add("BUFFER FULL");
        script.addAll(lines079(30, 3));
        final FakeElm elm = new FakeElm(script, 3);
        AtomicInteger opens = new AtomicInteger();
        Transport.Connector conn = new Transport.Connector() {
            @Override public Transport open() throws IOException {
                if (opens.getAndIncrement() > 0) throw new IOException("adapter busy");  // later attempts fail
                return elm;
            }
            @Override public void abort() {}
        };
        final List<Window> ws = new CopyOnWriteArrayList<>();
        final List<String> ev = new CopyOnWriteArrayList<>();
        ObdLink link = new ObdLink(conn, Clock.SYSTEM, new ObdLink.Sink() {
            @Override public void window(Window w) { ws.add(w); }
            @Override public void event(String line) { ev.add(line); }
        }, FAST, 50, 100);
        link.start();
        long okVectors = 0;
        for (Map<String, Object> e : list(vec, "can")) if ("OK".equals(e.get("status")) && ((Long) e.get("id")) == 0x079) okVectors++;
        final long want = okVectors + 30;
        check(waitFor(() -> ws.size() >= want, 5000), "link: one window per usable frame (" + ws.size() + " of " + want + ")");
        link.stop();
        eq(ws.size(), want, "rejected vectors produce no window");
        check(link.summary().contains("reserved_bits=1") && link.summary().contains("sentinel=2") && link.summary().contains("short=1"),
                "decode rejects counted: " + link.summary());
        int resets = 0;
        for (Window w : ws) if ((w.flags & Protocol.FLAG_FRAME_RESET) != 0) resets++;
        eq(resets, 2, "frame_reset at the first window and after the BUFFER FULL restart");
        check(ev.toString().contains("obd_run_end"), "run summary logged");
        try (FileOutputStream o = new FileOutputStream(outPath)) {
            long seq = 0;
            for (Window w : ws) o.write(w.encode(seq++));
        }
    }
}
