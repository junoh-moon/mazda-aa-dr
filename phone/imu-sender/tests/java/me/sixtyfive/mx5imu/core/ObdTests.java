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
 * Usage: ObdTests OBD_VECTORS.json OUT_STREAM.bin
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
        integrator(vec);
        selector();
        protocolFlags();
        controllerInit();
        controllerRestarts();
        controllerQuestionStop();
        controllerNoTraffic();
        controllerRefusedCommand();
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
        check(m.verdict().equals("GOOD"), "clean: GOOD");
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
        check(!m.verdict().equals("GOOD"), "a stall prevents GOOD");
        // heavy loss: every second frame lost for 20 s -> implied rate consistent, loss 50 %
        ObdLossMeter h = new ObdLossMeter(20000);
        long t2 = 0;
        int c2 = 0;
        h.add(c2, t2);
        for (int i = 0; i < 500; i++) { c2 = (c2 + 2) & 15; t2 += 2 * P; h.add(c2, t2); }
        check(h.counterConsistent() && Math.abs(h.lossPercent() - 50) < 0.5 && Math.abs(h.fps() - 25) < 0.1, "half lost: counter says 50 %");
        check(h.verdict().equals("BAD"), "25 frames/s: BAD");
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
        eq(new ObdLossMeter(20000).verdict().equals("NONE") ? 1 : 0, 1, "no frames: NONE");
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
        final LinkedBlockingQueue<byte[]> toPhone = new LinkedBlockingQueue<>();
        final ByteArrayOutputStream fromPhone = new ByteArrayOutputStream();
        final List<String> commands = new CopyOnWriteArrayList<>();
        final Set<String> refuse = new HashSet<>();
        volatile boolean closed, echo = true, monitoring, retainStop, startMonitoring;
        final List<String> script;
        volatile int scriptPos;
        final AtomicInteger monitorStarts = new AtomicInteger();
        private final StringBuilder cmd = new StringBuilder();
        private Thread emitter;
        final long lineMs;

        FakeElm(List<String> script, long lineMs) { this.script = script; this.lineMs = lineMs; }

        void send(String s) { if (!closed) toPhone.add(s.getBytes(StandardCharsets.US_ASCII)); }

        final InputStream in = new InputStream() {
            byte[] cur;
            int pos;
            @Override public int read() throws IOException { byte[] b = new byte[1]; return read(b, 0, 1) < 0 ? -1 : b[0] & 0xFF; }
            @Override public int read(byte[] b, int off, int len) throws IOException {
                while (cur == null || pos >= cur.length) {
                    if (closed) throw new IOException("closed");
                    try { cur = toPhone.poll(20, TimeUnit.MILLISECONDS); } catch (InterruptedException e) { throw new InterruptedIOException(); }
                    pos = 0;
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
                case "ATZ": echo = true; send("\r\rELM327 v1.5\r\r>"); return;
                case "ATE0": echo = false; send("OK\r\r>"); return;
                case "ATMA":
                    monitoring = true;
                    monitorStarts.incrementAndGet();
                    startEmitter();
                    return;
                default: send("OK\r\r>");
            }
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
        List<String> want = java.util.Arrays.asList("XATZ", "ATZ", "ATE0", "ATL0", "ATS0", "ATH1", "ATSP6", "ATCAF0", "ATCRA079", "ATMA");
        check(elm.commands.subList(0, Math.min(10, elm.commands.size())).equals(want), "init sequence " + elm.commands);
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
