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
        for (Window w : ws) s.offer(w);
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
        clk.t.addAndGet(9_000_000_000L); // 15 s since start, 9 s since the ping
        check(!s.checkSilence(), "silence: 9 s after a ping still open");
        clk.t.addAndGet(1_000_000_000L);
        check(s.checkSilence() && "silence".equals(s.closeReason()) && t.closed, "silence: closed after 10 s without a ping");
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
        // A session with PINGs for 61 s resets the failures and the backoff.
        p.connected(6000);
        check(!p.shouldAttempt(6000), "no attempts while connected");
        p.sessionEnded(70000, true, 61000);
        eq(p.consecutiveFailures(), 0, "healthy session clears the failures");
        eq(p.waitMs(70000), 2000, "healthy session: backoff back to 2 s");
        // A pinged but short session (40 s) is neither healthy nor a failure: the backoff keeps growing.
        p.connected(72000);
        p.sessionEnded(112000, true, 35000);
        eq(p.consecutiveFailures(), 0, "short pinged session is not a failure");
        eq(p.waitMs(112000), 4000, "short session does not reset the backoff");
        p.aclConnected(113000);
        check(p.shouldAttempt(113000), "ACL_CONNECTED: immediate attempt with no failure pending");
        p.aclDisconnected(114000);
        eq(p.waitMs(120000), -1, "ACL down: no attempts");
        p.adapter(false, 121000);
        eq(p.waitMs(122000), -1, "adapter off: no attempts");
        p.adapter(true, 123000);
        check(p.shouldAttempt(200000) && p.acl() == ReconnectPolicy.Acl.UNKNOWN, "adapter on: unknown ACL");
        ReconnectPolicy u = new ReconnectPolicy(2000, 30000, new ReconnectPolicy.Breaker(100, 3_600_000, 60_000, 30_000));
        u.setEnabled(true, 0);
        for (int i = 0; i < ReconnectPolicy.UNKNOWN_GIVE_UP; i++) u.connectFailed(0);
        check(u.acl() == ReconnectPolicy.Acl.DOWN && u.waitMs(100000) == -1, "unknown ACL: stop paging after 5 failures");
        p.setEnabled(false, 0);
        eq(p.waitMs(300000), -1, "disabled again: no attempts");
        breaker();
        crashLoop();
    }

    static void breaker() {
        // Failures: a session without PING, a session ending (or ACL dropping) within 30 s, a failed connect.
        ReconnectPolicy p = new ReconnectPolicy(2000, 30000);
        p.setEnabled(true, 0);
        p.aclConnected(0);
        p.connected(0);
        p.sessionEnded(15000, false, 0);                  // no PING (closed after 15 s)
        eq(p.consecutiveFailures(), 1, "no-ping session is a failure");
        p.connected(20000);
        p.aclDisconnected(25000);                          // ACL drop 5 s after the connect
        p.sessionEnded(45000, true, 20000);
        eq(p.consecutiveFailures(), 2, "early ACL drop is a failure");
        p.aclConnected(46000);
        check(!p.shouldAttempt(46000), "no immediate retry on ACL_CONNECTED after failures");
        p.connected(60000);
        p.sessionEnded(70000, true, 9000);                 // ended 10 s after the connect
        check(p.state(70000) == ReconnectPolicy.BreakerState.OPEN && p.trips() == 1, "third failure trips the breaker");
        check(!p.shouldAttempt(70000 + 59 * 60_000L), "open: no attempt for 60 min");
        p.aclConnected(80000);
        check(!p.shouldAttempt(80001), "open: ACL_CONNECTED never retries");
        eq(p.waitMs(80000), 70000 + 60 * 60_000L - 80000, "open: waits until the breaker closes");
        long t = 70000 + 60 * 60_000L;
        check(p.state(t) == ReconnectPolicy.BreakerState.CLOSED && p.shouldAttempt(t), "breaker closes after 60 min");
        for (int i = 0; i < 3; i++) p.connectFailed(t + i);
        check(p.state(t + 10) == ReconnectPolicy.BreakerState.LOCKED && p.waitMs(t + 999_999_999L) == -1,
                "second trip: stopped until the app is reopened");
        p.aclConnected(t + 20);
        check(!p.shouldAttempt(t + 10_000_000L), "locked: ACL_CONNECTED never retries");
        p.resetBreaker();
        check(p.state(t + 30) == ReconnectPolicy.BreakerState.CLOSED && p.shouldAttempt(t + 10_000_000L), "app reopened: breaker cleared");
        // A failure followed by a healthy session starts the count again.
        ReconnectPolicy q = new ReconnectPolicy(2000, 30000);
        q.setEnabled(true, 0);
        q.connectFailed(0);
        q.connectFailed(1);
        q.connected(10000);
        q.sessionEnded(100000, true, 80000);
        q.connectFailed(100001);
        check(q.state(100002) == ReconnectPolicy.BreakerState.CLOSED && q.consecutiveFailures() == 1, "healthy session resets the count");
    }

    /**
     * 'bds crash on connect -> SM restart -> phone retries' (discrete events, fake time). The bds
     * crashes on every RFCOMM connect (worst case: the crash is in the SDP/RFCOMM stage, which the
     * QEMU parse check cannot reach); the crash drops the ACL link, SM restarts bds after ~3 s and the
     * ACL comes back ~5 s later. The CMU (collector) stops opening the PTY and sending PINGs after
     * the first restart (phone_imu_rx.h quiesce), so no session is ever healthy. Count the crashes
     * the phone causes in 24 h of one CMU boot with ACL_CONNECTED broadcasts: the bound is
     * 2 trips x 3 failures = 6, then nothing until the app is reopened.
     */
    static void crashLoop() {
        ReconnectPolicy p = new ReconnectPolicy(2000, 30000);
        long now = 0, end = 24L * 3600_000L;
        p.setEnabled(true, now);
        p.aclConnected(now);
        int crashes = 0;
        long aclBackAt = -1;
        while (now < end) {
            if (aclBackAt >= 0 && now >= aclBackAt) { p.aclConnected(now); aclBackAt = -1; }
            if (p.shouldAttempt(now)) {
                p.connected(now);
                crashes++;                                   // bds dies on the connect
                p.aclDisconnected(now + 200);
                p.sessionEnded(now + 300, false, 0);         // read error, no PING
                aclBackAt = now + 8000;                      // SM restart + ACL reconnect
                now += 300;
                continue;
            }
            long w = p.waitMs(now);
            long next = w < 0 ? (aclBackAt >= 0 ? aclBackAt : end) : now + Math.max(1, w);
            if (aclBackAt >= 0) next = Math.min(next, aclBackAt);
            now = Math.max(next, now + 1);
        }
        eq(crashes, 6, "crash loop: bounded by 2 trips x 3 failures per app start");
        check(p.state(end) == ReconnectPolicy.BreakerState.LOCKED, "crash loop: locked until the app is reopened");
        // Same loop when the CMU keeps pinging for 40 s before each crash (not quiesced): still bounded.
        ReconnectPolicy r = new ReconnectPolicy(2000, 30000);
        now = 0;
        r.setEnabled(true, 0);
        r.aclConnected(0);
        crashes = 0;
        for (int i = 0; i < 1000 && now < end; i++) {
            long w = r.waitMs(now);
            if (w < 0) break;
            now += w;
            if (!r.shouldAttempt(now)) { now += 1; continue; }
            r.connected(now);
            crashes++;
            r.sessionEnded(now + 40000, true, 35000);   // PINGs for 35 s, then the crash
            now += 40000;
        }
        check(crashes > 6, "without the CMU quiesce a pinged-then-crash loop is NOT bounded by the phone alone (" + crashes + ")");
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
        ReconnectPolicy.Breaker noTrip = new ReconnectPolicy.Breaker(1000, 1, 60_000, 0);
        Link link = new Link(conn, l, Clock.SYSTEM, 40, 400, 10, noTrip);
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
        Link slow = new Link(conn, l, Clock.SYSTEM, 400, 30000, 10, noTrip);
        slow.start();
        check(waitFor(() -> attempts.size() == 1 && events.size() == 1, 2000), "acl: first attempt failed");
        slow.aclConnected();
        Thread.sleep(150);
        eq(attempts.size(), 1, "acl: ACL_CONNECTED gives no immediate attempt after a failure");
        synchronized (blocker) { blockOpen[0] = true; }
        check(waitFor(() -> attempts.size() == 2, 2000), "acl: blocking open in progress after the backoff");
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
        Link link = new Link(conn, l, Clock.SYSTEM, 10, 40, 5, new ReconnectPolicy.Breaker(3, 300, 60_000, 30_000));
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
        link.resetBreaker();
        check(waitFor(() -> opened.size() >= 7, 2000), "link: app reopened, attempts resume");
        link.stop();
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
