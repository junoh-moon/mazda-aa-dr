package me.sixtyfive.mx5imu.core;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.InterruptedIOException;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Map;
import java.util.Random;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;

/** Plain-JVM tests of the Android-free core (no JUnit, no Gradle). Usage: CoreTests GOLDEN.json TMPDIR */
public final class CoreTests {
    private static int passed, failed;

    private static void check(boolean ok, String what) {
        if (ok) passed++;
        else { failed++; System.out.println("FAIL: " + what); }
    }

    private static void eq(long got, long want, String what) { check(got == want, what + " got " + got + " want " + want); }

    public static void main(String[] args) throws Exception {
        crc();
        golden(args[0]);
        parserResync();
        integratorFlat();
        integratorTilted();
        integratorFlags();
        integratorGapAndOrder();
        sessionSeqAndPong();
        sessionSilence();
        sessionStall();
        policy();
        link();
        linkBreaker();
        rollingLog(new File(args[1]));
        System.out.println("core tests: " + passed + " passed, " + failed + " failed");
        if (failed != 0) System.exit(1);
    }

    static byte[] hex(String h) {
        byte[] b = new byte[h.length() / 2];
        for (int i = 0; i < b.length; i++) b[i] = (byte) Integer.parseInt(h.substring(2 * i, 2 * i + 2), 16);
        return b;
    }

    static String hex(byte[] b) {
        StringBuilder s = new StringBuilder();
        for (byte x : b) s.append(String.format("%02x", x & 0xFF));
        return s.toString();
    }

    static void crc() {
        eq(Protocol.crc32("123456789".getBytes(StandardCharsets.US_ASCII), 0, 9), 0xCBF43926L, "crc32 check value");
    }

    @SuppressWarnings("unchecked")
    static void golden(String path) throws IOException {
        List<Object> entries = (List<Object>) MiniJson.parse(new String(Files.readAllBytes(Paths.get(path)), StandardCharsets.UTF_8));
        int data = 0, pp = 0, corrupt = 0;
        for (Object o : entries) {
            Map<String, Object> e = (Map<String, Object>) o;
            String name = (String) e.get("name"), kind = (String) e.get("kind");
            Map<String, Object> f = (Map<String, Object>) e.get("fields");
            byte[] bytes = hex((String) e.get("hex"));
            FrameParser p = new FrameParser();
            List<Frame> got = p.feed(bytes, 0, bytes.length);
            if (kind.equals("corrupt")) {
                corrupt++;
                String expect = (String) f.get("expect");
                long counter = expect.equals("crc_error") ? p.crcErrors : expect.equals("unknown_version") ? p.unknownVersion
                        : expect.equals("bad_type") ? p.badType : -1;
                check(got.isEmpty() && counter >= 1, name + ": corrupt frame rejected as " + expect);
                continue;
            }
            check(got.size() == 1 && p.pending() == 0, name + ": one frame decoded");
            if (got.size() != 1) continue;
            Frame fr = got.get(0);
            eq((Long) f.get("version"), 1, name + " version");
            eq(fr.seq, (Long) f.get("seq"), name + " seq");
            eq(fr.crc, (Long) f.get("crc32"), name + " crc32");
            byte[] re;
            if (kind.equals("data")) {
                data++;
                check(fr.kind == Frame.Kind.DATA && "MX".equals(f.get("magic")), name + " kind");
                eq(fr.flags, (Long) f.get("flags"), name + " flags");
                eq(fr.tEndNs, (Long) f.get("t_end_ns"), name + " t_end_ns");
                eq(fr.dthetaUrad, (Long) f.get("dtheta_urad"), name + " dtheta");
                eq(fr.windowUs, (Long) f.get("window_us"), name + " window_us");
                eq(fr.biasUradS, (Long) f.get("bias_urad_s"), name + " bias");
                re = Protocol.encodeData(((Long) f.get("flags")).intValue(), (Long) f.get("seq"), (Long) f.get("t_end_ns"),
                        ((Long) f.get("dtheta_urad")).intValue(), (Long) f.get("window_us"), ((Long) f.get("bias_urad_s")).intValue());
            } else {
                pp++;
                int type = ((Long) f.get("type")).intValue();
                check(fr.kind == (type == 0 ? Frame.Kind.PING : Frame.Kind.PONG) && kind.equals(type == 0 ? "ping" : "pong"), name + " kind");
                eq(fr.tCmuNs, (Long) f.get("t_cmu_ns"), name + " t_cmu");
                eq(fr.tPhoneNs, (Long) f.get("t_phone_ns"), name + " t_phone");
                re = Protocol.encodePingPong(type, (Long) f.get("seq"), (Long) f.get("t_cmu_ns"), (Long) f.get("t_phone_ns"));
                if (type == 0) {
                    byte[] pong = Protocol.pongFor(fr, 555);
                    Frame back = new FrameParser().feed(pong, 0, pong.length).get(0);
                    check(back.kind == Frame.Kind.PONG && back.seq == fr.seq && back.tCmuNs == fr.tCmuNs && back.tPhoneNs == 555,
                            name + " pong copies seq and t_cmu");
                }
            }
            check(hex(re).equals(e.get("hex")), name + ": encoder reproduces golden bytes");
        }
        check(data >= 10 && pp >= 4 && corrupt >= 3, "golden file has >=10 data, >=4 ping/pong, >=3 corrupt");
    }

    static void parserResync() {
        Random r = new Random(7);
        ByteArrayOutputStream s = new ByteArrayOutputStream();
        List<byte[]> frames = new ArrayList<>();
        for (int i = 0; i < 50; i++) {
            byte[] f = i % 5 == 0 ? Protocol.encodePingPong(0, i, i * 1000L, 0) : Protocol.encodeData(5, i, 1000L * i, -i, 20000, i);
            frames.add(f);
            int junk = r.nextInt(6);
            for (int k = 0; k < junk; k++) s.write(r.nextInt(256));
            if (i % 7 == 3) { s.write('M'); s.write('X'); s.write(1); } // false magic
            s.write(f, 0, f.length);
        }
        byte[] all = s.toByteArray();
        FrameParser p = new FrameParser();
        List<Frame> got = new ArrayList<>();
        for (int off = 0; off < all.length; ) {
            int n = Math.min(all.length - off, 1 + r.nextInt(40));
            got.addAll(p.feed(all, off, n));
            off += n;
        }
        eq(got.size(), 50, "resync: all valid frames recovered");
        boolean ordered = true;
        for (int i = 0; i < got.size(); i++) ordered &= got.get(i).seq == i;
        check(ordered, "resync: order preserved");
        check(p.crcErrors >= 7, "resync: false magics counted as crc errors");
    }

    // Down unit d; Android TYPE_GRAVITY reading = -9.81 d.
    static void feedGravity(Integrator in, double[] d) { in.gravity(-9.81 * d[0], -9.81 * d[1], -9.81 * d[2]); }

    static List<Window> spin(Integrator in, double[] down, double rate, double[] extra, long t0, int n, long dtNs) {
        List<Window> out = new ArrayList<>();
        for (int i = 0; i < n; i++) {
            double[] w = {rate * down[0] + extra[0], rate * down[1] + extra[1], rate * down[2] + extra[2]};
            Window x = in.gyro(t0 + i * dtNs, w[0], w[1], w[2]);
            if (x != null) out.add(x);
        }
        return out;
    }

    static long sum(List<Window> ws) { long s = 0; for (Window w : ws) s += w.dthetaUrad; return s; }

    static void integratorFlat() {
        // Phone flat, screen up: gravity reads (0,0,+9.81). Clockwise seen from above = angular velocity
        // pointing down = negative z. That must give POSITIVE dtheta.
        Integrator in = new Integrator();
        in.gravity(0, 0, 9.81);
        List<Window> ws = new ArrayList<>();
        for (int i = 0; i < 201; i++) { Window w = in.gyro(1_000_000_000L + i * 5_000_000L, 0, 0, -0.5); if (w != null) ws.add(w); }
        eq(ws.size(), 50, "flat: 200 intervals of 5 ms give 50 windows");
        check(ws.get(0).dthetaUrad == 10000 && ws.get(0).windowUs == 20000, "flat: 4 x 5 ms x 0.5 rad/s = 10000 urad");
        eq(sum(ws), 500000, "flat clockwise total +0.5 rad");
        check((ws.get(0).flags & Protocol.FLAG_FRAME_RESET) != 0 && (ws.get(1).flags & Protocol.FLAG_FRAME_RESET) == 0, "flat: frame_reset only first");
        check((ws.get(3).flags & Protocol.FLAG_GRAVITY_VALID) != 0, "flat: gravity_valid");
        for (int i = 1; i < ws.size(); i++) {
            if (ws.get(i).tEndNs - ws.get(i).windowUs * 1000 != ws.get(i - 1).tEndNs) { check(false, "flat: windows chain"); break; }
        }
        Integrator ccw = new Integrator();
        ccw.gravity(0, 0, 9.81);
        List<Window> c = spin(ccw, new double[] {0, 0, 1}, 0.5, new double[3], 0, 201, 5_000_000L); // omega up = counter-clockwise
        eq(sum(c), -500000, "flat counter-clockwise total -0.5 rad");
        Integrator down = new Integrator();
        down.gravity(0, 0, -9.81); // screen facing down: down axis is +z
        List<Window> d = spin(down, new double[] {0, 0, 1}, 0.25, new double[3], 0, 201, 5_000_000L);
        eq(sum(d), 250000, "screen-down phone, clockwise from above still positive");
    }

    static void integratorTilted() {
        Random r = new Random(11);
        for (int trial = 0; trial < 20; trial++) {
            double[] d = {r.nextGaussian(), r.nextGaussian(), r.nextGaussian()};
            double n = Math.sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            for (int k = 0; k < 3; k++) d[k] /= n;
            double[] perp = {r.nextGaussian(), r.nextGaussian(), r.nextGaussian()};
            double dot = perp[0] * d[0] + perp[1] * d[1] + perp[2] * d[2];
            for (int k = 0; k < 3; k++) perp[k] = 0.3 * (perp[k] - dot * d[k]); // pitch/roll rates, orthogonal to gravity
            Integrator in = new Integrator();
            feedGravity(in, d);
            double rate = (r.nextDouble() - 0.5) * 2;
            List<Window> ws = spin(in, d, rate, perp, 5_000_000_000L, 401, 5_000_000L);
            long want = Math.round(rate * 2.0 * 1e6);
            check(Math.abs(sum(ws) - want) <= ws.size(), "tilted mount " + trial + ": yaw about gravity only, got " + sum(ws) + " want " + want);
        }
        // Portrait holder, screen facing the driver, tilted back 30 degrees: gravity reads 9.81*(0,cos30,sin30).
        Integrator in = new Integrator();
        double c30 = Math.cos(Math.toRadians(30)), s30 = Math.sin(Math.toRadians(30));
        in.gravity(0, 9.81 * c30, 9.81 * s30);
        List<Window> ws = spin(in, new double[] {0, -c30, -s30}, 0.1, new double[3], 0, 101, 5_000_000L);
        eq(sum(ws), 50000, "portrait 30 deg holder: clockwise 0.1 rad/s x 0.5 s");
    }

    static void integratorFlags() {
        Integrator in = new Integrator();
        List<Window> none = spin(in, new double[] {0, 0, 1}, 0.2, new double[3], 0, 9, 5_000_000L);
        check(none.size() == 2 && none.get(0).dthetaUrad == 0 && (none.get(0).flags & Protocol.FLAG_GRAVITY_VALID) == 0,
                "no gravity yet: dtheta 0 and gravity_valid clear");
        in.gravity(0, 0, 0.5);
        Window w = spin(in, new double[] {0, 0, 1}, 0.2, new double[3], 45_000_000L, 4, 5_000_000L).get(0);
        check((w.flags & Protocol.FLAG_GRAVITY_VALID) == 0, "implausible gravity magnitude is invalid");
        Integrator b = new Integrator();
        b.gravity(0, 0, 9.81);
        b.bias(0, 0.0003, -0.0002, -0.001);
        List<Window> bw = spin(b, new double[] {0, 0, 1}, 0, new double[3], 0, 9, 5_000_000L);
        check((bw.get(0).flags & Protocol.FLAG_BIAS_VALID) != 0 && bw.get(0).biasUradS == 1000, "bias projected on down: -z 0.001 -> +1000 urad/s");
        check((bw.get(0).flags & Protocol.FLAG_STATIONARY) != 0, "stationary hint when rates are zero");
        List<Window> stale = spin(b, new double[] {0, 0, 1}, 0.2, new double[3], 2_000_000_000L, 9, 5_000_000L);
        check(stale.size() >= 1 && (stale.get(stale.size() - 1).flags & (Protocol.FLAG_BIAS_VALID | Protocol.FLAG_STATIONARY)) == 0
                && stale.get(stale.size() - 1).biasUradS == 0, "bias older than 1 s is invalid; moving is not stationary");
    }

    static void integratorGapAndOrder() {
        Integrator in = new Integrator();
        in.gravity(0, 0, 9.81);
        List<Window> a = spin(in, new double[] {0, 0, 1}, -1, new double[3], 0, 7, 5_000_000L); // 0..30 ms
        eq(a.size(), 1, "gap: one full window before");
        check(in.gyro(30_000_000L, 0, 0, 5) == null && in.ignoredSamples == 1, "duplicate timestamp ignored");
        Window partial = in.gyro(500_000_000L, 0, 0, -1); // 470 ms gap: closes the partial window (25..30 ms)
        check(partial != null && partial.tEndNs == 30_000_000L && partial.windowUs == 10000 && partial.dthetaUrad == 10000,
                "gap closes the partial window without integrating across the gap");
        List<Window> b = spin(in, new double[] {0, 0, 1}, -1, new double[3], 505_000_000L, 4, 5_000_000L);
        check(b.size() == 1 && (b.get(0).flags & Protocol.FLAG_FRAME_RESET) != 0 && b.get(0).windowUs == 20000 && in.gaps == 1,
                "after a gap the next window restarts with frame_reset");
        in.reset();
        List<Window> c = spin(in, new double[] {0, 0, 1}, -1, new double[3], 600_000_000L, 5, 5_000_000L);
        check(c.size() == 1 && (c.get(0).flags & Protocol.FLAG_FRAME_RESET) != 0, "reset (reconnect) sets frame_reset");
        check(Integrator.clampToInt(1L << 40) == Integer.MAX_VALUE && Integrator.clampToInt(-(1L << 40)) == Integer.MIN_VALUE, "dtheta clamps");
    }

    /** Fake socket: input fed from a queue (blocks until closed), output captured or blocking. */
    static final class FakeTransport implements Transport {
        final LinkedBlockingQueue<byte[]> rx = new LinkedBlockingQueue<>();
        final ByteArrayOutputStream tx = new ByteArrayOutputStream();
        volatile boolean closed, blockWrites;
        final AtomicInteger closes = new AtomicInteger();
        private final Object gate = new Object();

        final InputStream in = new InputStream() {
            byte[] cur;
            int pos;
            @Override public int read() throws IOException { byte[] b = new byte[1]; return read(b, 0, 1) < 0 ? -1 : b[0] & 0xFF; }
            @Override public int read(byte[] b, int off, int len) throws IOException {
                while (cur == null || pos >= cur.length) {
                    if (closed) throw new IOException("closed");
                    try { cur = rx.poll(20, TimeUnit.MILLISECONDS); } catch (InterruptedException e) { throw new InterruptedIOException(); }
                    pos = 0;
                    if (cur != null && cur.length == 0) return -1;
                }
                int n = Math.min(len, cur.length - pos);
                System.arraycopy(cur, pos, b, off, n);
                pos += n;
                return n;
            }
        };
        final OutputStream out = new OutputStream() {
            @Override public void write(int b) throws IOException { write(new byte[] {(byte) b}, 0, 1); }
            @Override public void write(byte[] b, int off, int len) throws IOException {
                synchronized (gate) {
                    while (blockWrites && !closed) { try { gate.wait(20); } catch (InterruptedException e) { throw new InterruptedIOException(); } }
                }
                if (closed) throw new IOException("closed");
                synchronized (tx) { tx.write(b, off, len); }
            }
        };

        @Override public InputStream input() { return in; }
        @Override public OutputStream output() { return out; }
        @Override public void close() { closed = true; closes.incrementAndGet(); }
        byte[] sent() { synchronized (tx) { return tx.toByteArray(); } }
    }

    static final class FakeClock implements Clock {
        final AtomicLong t = new AtomicLong(1_000_000_000L);
        @Override public long nanos() { return t.get(); }
    }

    static boolean waitFor(java.util.function.BooleanSupplier c, long ms) throws InterruptedException {
        long end = System.currentTimeMillis() + ms;
        while (!c.getAsBoolean()) { if (System.currentTimeMillis() > end) return false; Thread.sleep(2); }
        return true;
    }

    static void sessionSeqAndPong() throws Exception {
        FakeTransport t = new FakeTransport();
        FakeClock clk = new FakeClock();
        AtomicLong seq = new AtomicLong(0xFFFFFFFEL);
        Session s = new Session(t, clk, seq);
        s.start();
        Integrator in = new Integrator();
        in.gravity(0, 0, 9.81);
        List<Window> ws = spin(in, new double[] {0, 0, 1}, -0.1, new double[3], 0, 17, 5_000_000L);
        // No DATA before the first PING of the session: windows are discarded and counted.
        for (Window w : ws) check(s.offer(w), "offer before the first ping is accepted (discarded)");
        Thread.sleep(50);
        eq(s.framesSent.get(), 0, "no DATA before the first PING");
        eq(s.droppedBeforePing.get(), 4, "windows before the first PING counted");
        byte[] ping = Protocol.encodePingPong(0, 0xABCDEF01L, 123456789L, 0);
        clk.t.set(2_000_000_000L);
        t.rx.add(new byte[] {1, 2, 'M'});                // garbage, then a ping split in two reads
        t.rx.add(java.util.Arrays.copyOfRange(ping, 0, 9));
        t.rx.add(java.util.Arrays.copyOfRange(ping, 9, 28));
        check(waitFor(() -> s.pinged(), 2000), "first ping arms the session");
        check((ws.get(0).flags & Protocol.FLAG_FRAME_RESET) != 0 && (ws.get(1).flags & Protocol.FLAG_FRAME_RESET) == 0,
                "integrator: frame_reset on its first window only");
        // The integrator's frame_reset window was discarded before the PING: the session sets
        // frame_reset on its first DATA actually sent (interop rig F1).
        for (Window w : new Window[] {ws.get(1), ws.get(2), ws.get(3), ws.get(1)}) s.offer(w);
        check(waitFor(() -> s.framesSent.get() == 4, 2000), "session: 4 frames sent after the PING");
        byte[] bad = ping.clone();
        bad[12] ^= 1;
        t.rx.add(bad);                                    // corrupt ping: no pong
        t.rx.add(Protocol.encodeData(0, 1, 2, 3, 4, 5));  // a DATA frame from the CMU is unexpected
        t.rx.add(Protocol.encodePingPong(1, 9, 9, 9));    // a PONG from the CMU is unexpected
        check(waitFor(() -> s.unexpectedFrames.get() == 2, 2000), "session: unexpected frames counted");
        byte[] out = t.sent();
        List<Frame> fr = new FrameParser().feed(out, 0, out.length);
        List<Long> seqs = new ArrayList<>();
        int pongs = 0;
        for (Frame f : fr) {
            if (f.kind == Frame.Kind.DATA) seqs.add(f.seq);
            else if (f.kind == Frame.Kind.PONG) {
                pongs++;
                check(f.seq == 0xABCDEF01L && f.tCmuNs == 123456789L && f.tPhoneNs == 2_000_000_000L, "pong echoes seq/t_cmu, adds reception time");
            } else check(false, "phone never sends a PING");
        }
        eq(pongs, 1, "exactly one pong for one valid ping");
        check(fr.get(0).kind == Frame.Kind.PONG, "the first frame on the wire is the PONG, no DATA before it");
        List<Integer> resetFlags = new ArrayList<>();
        for (Frame f : fr) if (f.kind == Frame.Kind.DATA) resetFlags.add(f.flags & Protocol.FLAG_FRAME_RESET);
        check(resetFlags.equals(java.util.Arrays.asList(8, 0, 0, 0)), "frame_reset on the first sent DATA only: " + resetFlags);
        check(seqs.equals(java.util.Arrays.asList(0xFFFFFFFEL, 0xFFFFFFFFL, 0L, 1L)), "seq wraps: " + seqs);
        eq(s.crcErrors(), 1, "corrupt ping counted");
        eq(s.lastPingNs(), 2_000_000_000L, "last ping time");
        s.close("test");
        s.join(1000);
        check(s.offer(ws.get(0)) == false, "no offers after close");
        eq(seq.get(), 2, "seq counter continues for the next session");
    }

    static void sessionSilence() throws Exception {
        FakeTransport t = new FakeTransport();
        FakeClock clk = new FakeClock();
        Session s = new Session(t, clk, new AtomicLong());
        s.start();
        clk.t.addAndGet(6_000_000_000L);
        t.rx.add(Protocol.encodePingPong(0, 1, 1, 0));
        check(waitFor(() -> s.pingsReceived.get() == 1, 2000), "silence: ping received");
        Window w0 = new Window(4, 1, 1, 20000, 0, 4);
        clk.t.addAndGet(10_500_000_000L); // one lost PING: data still flows
        s.offer(w0);
        check(waitFor(() -> s.framesSent.get() == 1, 2000), "10.5 s after a ping: DATA still sent");
        clk.t.addAndGet(500_000_000L);    // 11 s: the CMU reader may be stalled; never fill the bds PTY
        s.offer(w0);
        Thread.sleep(50);
        eq(s.droppedStalePing.get(), 1, "11 s after the last PING: DATA paused (counted)");
        eq(s.framesSent.get(), 1, "paused window not sent");
        t.rx.add(Protocol.encodePingPong(0, 2, 2, 0));    // the PING comes back: DATA resumes
        check(waitFor(() -> s.pingsReceived.get() == 2, 2000), "gap: second ping");
        s.offer(w0);
        check(waitFor(() -> s.framesSent.get() == 2, 2000), "gap: DATA resumes");
        byte[] gapOut = t.sent();
        List<Frame> gapFr = new FrameParser().feed(gapOut, 0, gapOut.length);
        List<Integer> gapFlags = new ArrayList<>();
        for (Frame f : gapFr) if (f.kind == Frame.Kind.DATA) gapFlags.add(f.flags & Protocol.FLAG_FRAME_RESET);
        check(gapFlags.equals(java.util.Arrays.asList(8, 8)), "frame_reset on the first DATA and again after the PING gap: " + gapFlags);
        clk.t.addAndGet(14_000_000_000L); // 14 s since the last ping: one or two lost PINGs keep it open
        check(!s.checkSilence(), "silence: 14 s after a ping still open");
        clk.t.addAndGet(1_000_000_000L);
        check(s.checkSilence() && "silence".equals(s.closeReason()) && t.closed, "silence: closed after 15 s (three PING periods) without a ping");
        s.join(1000);
        FakeTransport t2 = new FakeTransport();
        FakeClock c2 = new FakeClock();
        Session s2 = new Session(t2, c2, new AtomicLong());
        s2.start();
        c2.t.addAndGet(14_999_000_000L);
        check(!s2.checkSilence(), "first ping: still waiting 14.999 s after connect");
        c2.t.addAndGet(1_000_000L);
        check(s2.checkSilence() && "no_ping".equals(s2.closeReason()) && !s2.pinged(), "first ping: closed 15 s after connect without a PING");
        s2.join(1000);
        FakeTransport t3 = new FakeTransport();
        Session s3 = new Session(t3, new FakeClock(), new AtomicLong());
        s3.start();
        t3.rx.add(new byte[0]);
        check(waitFor(() -> "eof".equals(s3.closeReason()), 2000), "eof closes the session");
        s3.join(1000);
    }

    static void sessionStall() throws Exception {
        FakeTransport t = new FakeTransport();
        t.blockWrites = true;
        Session s = new Session(t, new FakeClock(), new AtomicLong());
        s.start();
        t.rx.add(Protocol.encodePingPong(0, 1, 1, 0));   // armed by a PING (its PONG write blocks too)
        check(waitFor(() -> s.pinged(), 2000), "stall: armed");
        Window w = new Window(4, 1, 1, 20000, 0, 4);
        int accepted = 0;
        for (int i = 0; i < Session.QUEUE_FRAMES + 5; i++) if (s.offer(w)) accepted++;
        check(accepted >= Session.QUEUE_FRAMES && accepted <= Session.QUEUE_FRAMES + 1 && "tx_stall".equals(s.closeReason()),
                "full transmit queue (CMU not reading) closes with tx_stall");
        s.join(2000);
    }

    static final long M = 60_000L, H = 3_600_000L;

    static void policy() {
        ReconnectPolicy p = new ReconnectPolicy(2000, 30000);
        eq(p.waitMs(0), -1, "disabled: wait for an event");
        p.setEnabled(true, 0);
        check(p.shouldAttempt(0), "enabled: immediate first attempt");
        p.aclConnected(0);
        p.connectFailed(0);
        eq(p.waitMs(0), 2000, "backoff 2 s after a failed connect");
        p.connectFailed(2000);
        eq(p.waitMs(2000), 4000, "backoff doubles");
        p.aclConnected(2500);
        check(!p.shouldAttempt(2500), "ACL_CONNECTED gives no immediate attempt while failures are pending");
        p.connected(6000);
        check(!p.shouldAttempt(6000), "no attempts while connected");
        p.sessionEnded(70000, true, 61000);                  // healthy: 64 s, PINGs over 61 s
        eq(p.consecutiveFailures(), 0, "healthy session clears the failures");
        eq(p.waitMs(70000), 2000, "healthy session: backoff back to 2 s");
        p.connected(72000);
        p.sessionEnded(142000, true, 35000);                 // 70 s, PINGs over 35 s: neutral
        eq(p.consecutiveFailures(), 0, "long session with short PING span is not a failure");
        eq(p.waitMs(142000), 4000, "neutral session does not reset the backoff");
        p.aclConnected(143000);
        check(p.shouldAttempt(143000), "ACL_CONNECTED: immediate attempt with no failure pending");
        p.aclDisconnected(144000);
        eq(p.waitMs(150000), -1, "ACL down: no attempts");
        p.adapter(false, 151000);
        eq(p.waitMs(152000), -1, "adapter off: no attempts");
        p.adapter(true, 153000);
        check(p.shouldAttempt(300000) && p.acl() == ReconnectPolicy.Acl.UNKNOWN, "adapter on: unknown ACL");
        ReconnectPolicy u = new ReconnectPolicy(2000, 30000);
        u.setEnabled(true, 0);
        for (int i = 0; i < ReconnectPolicy.UNKNOWN_GIVE_UP; i++) u.connectFailed(0);
        check(u.acl() == ReconnectPolicy.Acl.DOWN && u.waitMs(100000) == -1, "unknown ACL: stop paging after 5 failures");
        eq(u.consecutiveFailures(), 0, "car absent (ACL unknown): connect failures are not breaker failures");
        p.setEnabled(false, 0);
        eq(p.waitMs(400000), -1, "disabled again: no attempts");
        breaker();
        persistence();
        retryBound();
        crashLoop();
    }

    static void breaker() {
        ReconnectPolicy p = new ReconnectPolicy(2000, 30000);
        p.setEnabled(true, 0);
        p.aclConnected(0);
        p.connected(0);
        p.sessionEnded(15000, false, 0);                  // no PING (closed after 15 s)
        eq(p.consecutiveFailures(), 1, "no-ping session is a failure");
        p.connected(20000);
        p.aclDisconnected(25000);
        p.sessionEnded(28000, true, 3000);                 // PINGs, then the ACL dropped after 8 s
        eq(p.consecutiveFailures(), 1, "a session with a PING is never a failure (ACL flapping)");
        p.connected(40000);
        p.sessionEnded(55000, false, 0);
        eq(p.consecutiveFailures(), 2, "second no-ping session");
        p.aclConnected(56000);
        check(!p.shouldAttempt(56000), "no immediate retry on ACL_CONNECTED after failures");
        p.connected(90000);
        p.sessionEnded(100000, false, 0);
        check(p.state(100000) == ReconnectPolicy.BreakerState.OPEN && p.trips() == 1, "third failure trips the breaker");
        check(!p.shouldAttempt(100000 + 59 * M), "open: no attempt for 60 min");
        p.aclConnected(110000);
        check(!p.shouldAttempt(110001), "open: ACL_CONNECTED never retries");
        eq(p.waitMs(110000), 100000 + 60 * M - 110000, "open: waits until the breaker closes");
        long t = 100000 + 60 * M;
        check(p.state(t) == ReconnectPolicy.BreakerState.CLOSED && p.shouldAttempt(t), "breaker closes after 60 min");
        for (int i = 0; i < 3; i++) p.connectFailed(t + i);   // ACL is UP: the car is there
        check(p.state(t + 10) == ReconnectPolicy.BreakerState.LOCKED && p.waitMs(t + 999_999_999L) == -1,
                "second trip: locked until 'retry now'");
        p.aclConnected(t + 20);
        check(!p.shouldAttempt(t + 10_000_000L), "locked: ACL_CONNECTED never retries");
        p.setEnabled(true, t + 30);
        check(p.state(t + 40) == ReconnectPolicy.BreakerState.LOCKED, "a service (re)start does not clear the breaker");
        p.retryNow(t + 50);
        check(p.state(t + 60) == ReconnectPolicy.BreakerState.CLOSED && p.shouldAttempt(t + 60), "'retry now' clears it");
        for (int i = 0; i < 3; i++) p.connectFailed(t + 100 + i);
        check(p.state(t + 200) == ReconnectPolicy.BreakerState.LOCKED, "after 'retry now' the next trip locks at once");
        // ACL flapping every 20 s with a healthy CMU (interop rig F4): never a failure, never locks.
        ReconnectPolicy a = new ReconnectPolicy(2000, 30000);
        a.setEnabled(true, 0);
        a.aclConnected(0);
        long ft = 0;
        for (int i = 0; i < 2000; i++) {                  // > 11 h of 20 s flaps
            if (!a.shouldAttempt(ft)) { ft += Math.max(1, a.waitMs(ft) < 0 ? 1000 : a.waitMs(ft)); a.aclConnected(ft); continue; }
            a.connected(ft);
            a.aclDisconnected(ft + 20000);
            a.sessionEnded(ft + 20100, true, 15000);          // PINGs at 1, 6, 11, 16 s
            ft += 20100 + 1000;
            a.aclConnected(ft);
        }
        check(a.state(ft) == ReconnectPolicy.BreakerState.CLOSED && a.consecutiveFailures() == 0 && a.trips() == 0,
                "ACL flapping every 20 s with PINGs: never trips");
        // Absent car (Fable S7/S8): at home after a phone reboot nothing trips; one failure in the car later does not lock.
        ReconnectPolicy h = new ReconnectPolicy(2000, 30000);
        long now = 0;
        h.setEnabled(true, 0);
        while (now < 3 * H) {
            if (h.shouldAttempt(now)) { now += 6000; h.connectFailed(now); continue; }
            long w = h.waitMs(now);
            if (w < 0) break;
            now += Math.max(1, w);
        }
        check(h.state(now) == ReconnectPolicy.BreakerState.CLOSED && h.consecutiveFailures() == 0 && h.trips() == 0,
                "car absent at start: the breaker never counts");
        h.aclConnected(90 * M);
        check(h.shouldAttempt(90 * M), "in the car later: immediate attempt");
        h.connectFailed(90 * M + 6000);
        check(h.state(90 * M + 6001) == ReconnectPolicy.BreakerState.CLOSED && h.consecutiveFailures() == 1,
                "one failed first connect in the car: counted once, not locked");
        // A healthy session forgets the trip only 24 h after it.
        ReconnectPolicy q = new ReconnectPolicy(2000, 30000);
        q.setEnabled(true, 0);
        q.aclConnected(0);
        for (int i = 0; i < 3; i++) q.connectFailed(i);
        q.connected(2 * H);
        q.sessionEnded(2 * H + 2 * M, true, 90000);
        eq(q.trips(), 1, "healthy session within 24 h keeps the trip");
        q.connected(30 * H);
        q.sessionEnded(30 * H + 2 * M, true, 90000);
        eq(q.trips(), 0, "healthy session 24 h after the trip forgets it");
    }

    static void persistence() {
        ReconnectPolicy p = new ReconnectPolicy(2000, 30000);
        p.setEnabled(true, 0);
        p.aclConnected(0);
        for (int i = 0; i < 3; i++) p.connectFailed(1000 + i);   // OPEN until 1002 + 60 min
        String rec = p.snapshot(5000, 1_760_000_000_000L, 7);
        check(rec.startsWith("pimu-breaker-v1 0 1 0 "), "snapshot: " + rec);
        ReconnectPolicy same = new ReconnectPolicy(2000, 30000);
        same.restore(rec, 6000, 1_760_000_001_000L, 7);       // process restart in the same phone boot
        check(same.state(6000) == ReconnectPolicy.BreakerState.OPEN && same.openUntilMs() == p.openUntilMs(), "restore: same boot");
        ReconnectPolicy reboot = new ReconnectPolicy(2000, 30000);
        reboot.restore(rec, 50, 1_760_000_000_000L + 10 * M, 8);  // phone rebooted 10 min later
        long left = reboot.openUntilMs() - 50;
        check(reboot.state(50) == ReconnectPolicy.BreakerState.OPEN && left > 49 * M && left <= 50 * M, "restore after a reboot: " + left);
        ReconnectPolicy late = new ReconnectPolicy(2000, 30000);
        late.restore(rec, 50, 1_760_000_000_000L + 2 * H, 8);
        check(late.state(50) == ReconnectPolicy.BreakerState.CLOSED && late.trips() == 1, "reboot after the open time: closed, trip kept");
        p.connectFailed(70 * M);
        p.connectFailed(70 * M + 1);
        p.connectFailed(70 * M + 2);
        ReconnectPolicy locked = new ReconnectPolicy(2000, 30000);
        locked.restore(p.snapshot(71 * M, 1_760_000_000_000L, 7), 0, 1_760_000_100_000L, 9);
        check(locked.state(0) == ReconnectPolicy.BreakerState.LOCKED, "locked survives a reboot");
        // retry now -> snapshot -> restore: still CLOSED, one trip (v3 re-locked here).
        locked.retryNow(10);
        String afterRetry = locked.snapshot(20, 1_760_000_200_000L, 9);
        check(afterRetry.startsWith("pimu-breaker-v1 0 1 0 "), "retry now stores trips 1, unlocked: " + afterRetry);
        for (long boot2 : new long[] {9, 10}) {                 // process restart, phone reboot
            ReconnectPolicy r = new ReconnectPolicy(2000, 30000);
            r.restore(afterRetry, 30, 1_760_000_201_000L, boot2);
            check(r.state(30) == ReconnectPolicy.BreakerState.CLOSED && r.trips() == 1, "retry now survives a restart (boot " + boot2 + ")");
        }
        // retry now -> one more trip -> restart: LOCKED (v3 stored trips 3 and silently unlocked).
        locked.aclConnected(40);
        for (int i = 0; i < 3; i++) locked.connectFailed(50 + i);
        check(locked.state(60) == ReconnectPolicy.BreakerState.LOCKED && locked.trips() == 2, "retry now: the next trip locks");
        String relocked = locked.snapshot(60, 1_760_000_300_000L, 9);
        ReconnectPolicy rr = new ReconnectPolicy(2000, 30000);
        rr.restore(relocked, 70, 1_760_000_301_000L, 9);
        check(rr.state(70) == ReconnectPolicy.BreakerState.LOCKED, "retry now -> trip -> restart: still locked: " + relocked);
        roundTripProperty();
        ReconnectPolicy bad = new ReconnectPolicy(2000, 30000);
        bad.restore("pimu-breaker-v1 x", 0, 0, 1);
        check(bad.trips() == 1 && bad.state(0) == ReconnectPolicy.BreakerState.CLOSED, "corrupt record: one trip kept (next trip locks)");
    }

    /** Every reachable state (random event sequences): snapshot -> restore (same boot) -> snapshot is exact. */
    static void roundTripProperty() {
        java.util.Random rnd = new java.util.Random(16);
        int checked = 0, lockedSeen = 0, openSeen = 0, mismatches = 0;
        for (int run = 0; run < 400; run++) {
            ReconnectPolicy p = new ReconnectPolicy(2000, 30000);
            long now = 0;
            p.setEnabled(true, 0);
            for (int step = 0; step < 60; step++) {
                now += 1 + rnd.nextInt(30 * 60_000);
                switch (rnd.nextInt(8)) {
                    case 0: p.aclConnected(now); break;
                    case 1: p.aclDisconnected(now); break;
                    case 2: p.connectFailed(now); break;
                    case 3: p.connected(now); p.sessionEnded(now + 20000, false, 0); now += 20000; break;
                    case 4: p.connected(now); p.sessionEnded(now + 90000, true, 80000); now += 90000; break;
                    case 5: p.connected(now); p.sessionEnded(now + 30000, true, 20000); now += 30000; break;
                    case 6: if (rnd.nextInt(4) == 0) p.retryNow(now); break;
                    default: p.aclConnected(now); p.connectFailed(now); break;
                }
                String a = p.snapshot(now, 1_760_000_000_000L + now, 5);
                ReconnectPolicy q = new ReconnectPolicy(2000, 30000);
                q.restore(a, now, 1_760_000_000_000L + now, 5);
                String b = q.snapshot(now, 1_760_000_000_000L + now, 5);
                if (!a.equals(b) || q.state(now) != p.state(now) || q.trips() > 2) mismatches++;
                if (p.state(now) == ReconnectPolicy.BreakerState.LOCKED) lockedSeen++;
                if (p.state(now) == ReconnectPolicy.BreakerState.OPEN) openSeen++;
                if (!(p.trips() <= 2 && (p.trips() == 2) == (p.state(now) == ReconnectPolicy.BreakerState.LOCKED))) mismatches++;
                checked++;
            }
        }
        System.out.println("breaker round trip: " + checked + " states, " + lockedSeen + " locked, " + openSeen + " open, " + mismatches + " mismatches");
        check(mismatches == 0 && lockedSeen > 0 && openSeen > 0, "snapshot -> restore is exact for every reachable state");
    }

    /**
     * 'retry now' bound: a crash on every connect, the CMU quiesced; the owner presses the button
     * once LOCKED, again and again; the process restarts every 10 min. Each press allows exactly 3
     * more connects (crashes) before LOCKED again.
     */
    static void retryBound() {
        String stored = "";
        long now = 0, wall0 = 1_760_000_000_000L;
        ReconnectPolicy p = new ReconnectPolicy(2000, 30000);
        p.setEnabled(true, 0);
        p.aclConnected(0);
        int crashes = 0, presses = 0;
        java.util.List<Integer> perPress = new java.util.ArrayList<>();
        long nextRestart = 10 * M;
        while (presses <= 5 && now < 30 * 24 * H) {
            if (now >= nextRestart) {
                stored = p.snapshot(now, wall0 + now, 1);
                p = new ReconnectPolicy(2000, 30000); p.restore(stored, now, wall0 + now, 1); p.setEnabled(true, now); p.aclConnected(now);
                nextRestart += 10 * M;
            }
            if (p.state(now) == ReconnectPolicy.BreakerState.LOCKED) {
                if (presses > 0) perPress.add(crashes);
                crashes = 0; presses++;
                p.retryNow(now);
                continue;
            }
            if (p.shouldAttempt(now)) {
                p.connected(now); crashes++;
                p.sessionEnded(now + 300, false, 0);
                now += 300 + 8000; p.aclConnected(now);
                continue;
            }
            now += 1000;
        }
        System.out.println("retry now bound: crashes per press " + perPress);
        check(!perPress.isEmpty() && perPress.stream().allMatch(c -> c == 3), "each 'retry now' allows exactly 3 crashes: " + perPress);
    }

    /**
     * Independent crash-bound simulation (discrete events, ms). One CMU boot per 45-min drive every
     * 3 h for 30 days. The phone process is killed and restarted every 30 min (state restored from the
     * stored record), the phone reboots every 7 h, the owner opens the app every 2 h (no effect) and
     * never presses 'retry now'. Scenarios (interop rig): CRASH_EVERY = bds dies whenever our service
     * is connected (no PING ever); HEALTHY_THEN_CRASH = the first session of each drive gets PINGs for
     * 65 s, then bds dies and every later connect crashes it again; with the CMU quiesce after the
     * first death no later session gets a PING. FLAP20/FLAP75 = healthy CMU, the ACL link drops every
     * 20/75 s (radio trouble): sessions get PINGs, no crash. Returns {crashes, max per day, max per
     * drive, locked(0/1)}.
     */
    enum Scn { CRASH_EVERY, HEALTHY_THEN_CRASH, FLAP20, FLAP75 }

    static long[] crashSim(Scn scn) {
        long day = 24 * H, end = 30 * day, wall0 = 1_760_000_000_000L, boot = 1, bootStart = 0;
        String stored;
        ReconnectPolicy p = new ReconnectPolicy(2000, 30000);
        p.setEnabled(true, 0);
        int crashes = 0, maxDrive = 0, maxDay = 0, dayCount = 0, driveCrash = 0, driveSessions = 0;
        long dayStart = 0, nextRestart = 30 * M, nextReboot = 7 * H, now = 0, driveEnd = 0, nextDrive = 10 * M, aclBackAt = -1;
        boolean inCar = false, cmuQuiet = false;
        while (now < end) {
            long el = now - bootStart;
            if (now >= dayStart + day) { maxDay = Math.max(maxDay, dayCount); dayCount = 0; dayStart += day; }
            if (now >= nextReboot) {
                stored = p.snapshot(el, wall0 + now, boot);
                boot++; bootStart = now; nextReboot += 7 * H; el = 0;
                p = new ReconnectPolicy(2000, 30000); p.restore(stored, 0, wall0 + now, boot); p.setEnabled(true, 0);
                if (inCar) p.aclConnected(0);
            } else if (now >= nextRestart) {
                stored = p.snapshot(el, wall0 + now, boot);
                p = new ReconnectPolicy(2000, 30000); p.restore(stored, el, wall0 + now, boot); p.setEnabled(true, el);
                if (inCar) p.aclConnected(el);
                nextRestart += 30 * M;
            }
            if (!inCar && now >= nextDrive) { inCar = true; cmuQuiet = false; driveCrash = 0; driveSessions = 0; driveEnd = now + 45 * M; p.aclConnected(el); }
            if (inCar && now >= driveEnd) { inCar = false; nextDrive = now + 3 * H; p.aclDisconnected(el); maxDrive = Math.max(maxDrive, driveCrash); aclBackAt = -1; }
            if (inCar && aclBackAt >= 0 && now >= aclBackAt) { p.aclConnected(el); aclBackAt = -1; }
            if (inCar && aclBackAt < 0 && p.shouldAttempt(el)) {
                p.connected(el);
                long len;
                boolean pinged;
                long span;
                boolean crash;
                if (scn == Scn.FLAP20 || scn == Scn.FLAP75) {
                    len = scn == Scn.FLAP20 ? 20000 : 75000; pinged = true; span = len - 5000; crash = false;
                } else if (scn == Scn.HEALTHY_THEN_CRASH && driveSessions == 0) {
                    len = 66000; pinged = true; span = 65000; crash = true;
                } else {
                    len = 300; pinged = false; span = 0; crash = true;   // dies on the connect: no PING
                }
                if (cmuQuiet) { pinged = false; span = 0; }
                driveSessions++;
                if (crash) { crashes++; dayCount++; driveCrash++; cmuQuiet = true; }
                p.aclDisconnected(el + len - 100);
                p.sessionEnded(el + len, pinged, span);
                aclBackAt = now + len + (crash ? 8000 : 1000);
                now += len;
                continue;
            }
            now += 1000;
        }
        maxDay = Math.max(maxDay, dayCount);
        return new long[] {crashes, maxDay, maxDrive, p.state(now - bootStart) == ReconnectPolicy.BreakerState.LOCKED ? 1 : 0};
    }

    static void crashLoop() {
        for (Scn scn : Scn.values()) {
            long[] r = crashSim(scn);
            System.out.println("crash bound simulation " + scn + ": total " + r[0] + " in 30 days, max per 24 h " + r[1] +
                    ", max per drive " + r[2] + ", locked " + (r[3] == 1));
            switch (scn) {
                case CRASH_EVERY:
                    check(r[2] <= 3 && r[1] <= 6 && r[0] <= 6 && r[3] == 1, "crash on every connect: <= 3 per drive, 6 in total, then locked");
                    break;
                case HEALTHY_THEN_CRASH:
                    // each drive: one healthy session (resets the failures, keeps the trip), its crash, 3 failures
                    check(r[2] <= 4 && r[1] <= 8 && r[0] <= 8 && r[3] == 1, "healthy then crash: <= 4 per drive, 8 in total, then locked");
                    break;
                default:
                    check(r[0] == 0 && r[3] == 0, scn + ": no crash and never locked");
            }
        }
    }

    static void link() throws Exception {
        final List<Long> attempts = Collections.synchronizedList(new ArrayList<>());
        final AtomicInteger failFirst = new AtomicInteger(2);
        final List<FakeTransport> opened = Collections.synchronizedList(new ArrayList<>());
        final Object blocker = new Object();
        final boolean[] blockOpen = {false};
        Transport.Connector conn = new Transport.Connector() {
            @Override public Transport open() throws IOException {
                attempts.add(System.nanoTime());
                synchronized (blocker) {
                    if (blockOpen[0]) {
                        try { blocker.wait(); } catch (InterruptedException e) { throw new InterruptedIOException(); }
                        throw new IOException("aborted");
                    }
                }
                if (failFirst.getAndDecrement() > 0) throw new IOException("page timeout");
                FakeTransport t = new FakeTransport();
                opened.add(t);
                return t;
            }
            @Override public void abort() { synchronized (blocker) { blocker.notifyAll(); } }
        };
        final List<String> events = Collections.synchronizedList(new ArrayList<>());
        Link.Listener l = new Link.Listener() {
            @Override public void connected(Session s) { events.add("connected"); }
            @Override public void disconnected(Session s, String reason) { events.add("disconnected:" + reason); }
            @Override public void connectFailed(String error, long nextWaitMs, ReconnectPolicy.Acl acl) { events.add("failed:" + nextWaitMs); }
        };
        Link link = new Link(conn, l, Clock.SYSTEM, 40, 400, 10);
        link.start();
        check(waitFor(() -> events.contains("connected"), 3000), "link: connects after two failures");
        check(attempts.size() == 3 && attempts.get(1) - attempts.get(0) >= 35_000_000L && attempts.get(2) - attempts.get(1) >= 75_000_000L,
                "link: backoff 40 ms then 80 ms between attempts");
        opened.get(0).rx.add(new byte[0]); // CMU closes the stream
        check(waitFor(() -> events.contains("disconnected:eof") && opened.size() == 2, 3000), "link: reconnects after EOF");
        check(waitFor(() -> events.stream().filter("connected"::equals).count() == 2, 2000), "link: second session connected");
        link.stop();
        check(events.get(events.size() - 1).equals("disconnected:stopped") && opened.get(1).closed, "link: stop closes the session");

        // After a failure ACL_CONNECTED does not shorten the backoff; stop() aborts a blocking open.
        attempts.clear();
        events.clear();
        failFirst.set(100);
        Link slow = new Link(conn, l, Clock.SYSTEM, 400, 30000, 10);
        slow.start();
        check(waitFor(() -> attempts.size() == 1 && events.size() == 1, 2000), "acl: first attempt failed");
        // The first failure happened with the car absent (ACL unknown): not a breaker failure, so
        // ACL_CONNECTED gives an immediate attempt; that one fails with the car present.
        long before = System.nanoTime();
        slow.aclConnected();
        check(waitFor(() -> attempts.size() == 2, 1000) && attempts.get(1) - before < 300_000_000L,
                "acl: car arrives after absent-car failures: immediate attempt");
        Thread.sleep(50);
        slow.aclConnected();
        Thread.sleep(150);
        eq(attempts.size(), 2, "acl: ACL_CONNECTED gives no immediate attempt after a car-present failure");
        synchronized (blocker) { blockOpen[0] = true; }
        check(waitFor(() -> attempts.size() == 3, 3000), "acl: blocking open in progress after the backoff");
        long t0 = System.nanoTime();
        slow.stop();
        check(System.nanoTime() - t0 < 2_000_000_000L, "stop aborts a blocking connect");
        synchronized (blocker) { blockOpen[0] = false; }
    }

    /** Link wiring: sessions that never receive a PING trip the breaker; the listener hears it. */
    static void linkBreaker() throws Exception {
        final List<FakeTransport> opened = Collections.synchronizedList(new ArrayList<>());
        Transport.Connector conn = new Transport.Connector() {
            @Override public Transport open() {
                FakeTransport t = new FakeTransport();
                opened.add(t);
                t.rx.add(new byte[0]);                       // the CMU side closes at once (bds crash)
                return t;
            }
            @Override public void abort() {}
        };
        final List<String> events = Collections.synchronizedList(new ArrayList<>());
        Link.Listener l = new Link.Listener() {
            @Override public void connected(Session s) { events.add("connected"); }
            @Override public void disconnected(Session s, String reason) { events.add("disconnected:" + reason); }
            @Override public void connectFailed(String error, long nextWaitMs, ReconnectPolicy.Acl acl) {}
            @Override public void breaker(ReconnectPolicy.BreakerState state, long untilMs) { events.add("breaker:" + state); }
        };
        final String[] stored = {""};
        final Link.Store store = new Link.Store() {
            @Override public String load() { return stored[0]; }
            @Override public void save(String r) { stored[0] = r; }
            @Override public long wallMs() { return System.currentTimeMillis(); }
            @Override public long bootCount() { return 3; }
        };
        ReconnectPolicy.Breaker fast = new ReconnectPolicy.Breaker(3, 300, 60_000, 30_000);
        Link link = new Link(conn, l, Clock.SYSTEM, 10, 40, 5, fast);
        link.setStore(store);
        link.start();
        check(waitFor(() -> events.contains("breaker:OPEN"), 3000), "link: three no-ping sessions open the breaker");
        eq(opened.size(), 3, "link: exactly three attempts before the breaker opened");
        link.aclConnected();
        Thread.sleep(100);
        eq(opened.size(), 3, "link: ACL_CONNECTED does not retry while open");
        check(waitFor(() -> events.contains("breaker:LOCKED"), 3000), "link: second trip locks");
        eq(opened.size(), 6, "link: six attempts in total");
        link.aclConnected();
        Thread.sleep(400);
        eq(opened.size(), 6, "link: locked, no attempts");
        link.stop();
        check(stored[0].startsWith("pimu-breaker-v1 0 2 1 "), "link: locked state stored: " + stored[0]);
        // Process restart (or the app opened and the service restarted): still locked.
        Link again = new Link(conn, l, Clock.SYSTEM, 10, 40, 5, fast);
        again.setStore(store);
        again.start();
        again.aclConnected();
        Thread.sleep(300);
        eq(opened.size(), 6, "link: restored locked breaker, no attempts after a restart");
        again.retryNow();
        check(waitFor(() -> opened.size() >= 7, 2000), "link: 'retry now' resumes the attempts");
        again.stop();
    }

    static void rollingLog(File dir) throws IOException {
        File d = new File(dir, "rollog");
        check(d.mkdirs() || d.isDirectory(), "log dir");
        RollingLog log = new RollingLog(d, new FakeClock());
        StringBuilder line = new StringBuilder();
        for (int i = 0; i < 200; i++) line.append('x');
        for (int i = 0; i < 15000; i++) log.event("event " + i + " " + line);
        check(log.totalBytes() <= 1024L * 1024L && log.totalBytes() > RollingLog.HALF, "log stays at or below 1 MiB: " + log.totalBytes());
        File exp = new File(d, "export.txt");
        log.exportTo(exp);
        String s = new String(Files.readAllBytes(exp.toPath()), StandardCharsets.UTF_8);
        check(s.contains("event 14999 ") && !s.contains("event 0 "), "export keeps the newest events, older rotated away");
    }
}
