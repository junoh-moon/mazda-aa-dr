package me.sixtyfive.mx5imu.core;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Random;
import java.util.Set;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;
import java.util.regex.Pattern;

/**
 * Record-only A/B comparison rows (AbCompare, RollingLog.ab; design note section 16), plain JVM.
 * Usage: AbTests TMPDIR
 */
public final class AbTests {
    private static int passed, failed;
    private static final long S = 1_000_000_000L, MS = 1_000_000L;
    private static final SourceSelector.Source PHONE = SourceSelector.Source.PHONE, OBD = SourceSelector.Source.OBD;

    private static void check(boolean ok, String what) {
        if (ok) passed++;
        else { failed++; System.out.println("FAIL: " + what); }
    }

    private static void eq(long got, long want, String what) { check(got == want, what + " got " + got + " want " + want); }

    private static void eqs(String got, String want, String what) { check(want.equals(got), what + " got [" + got + "] want [" + want + "]"); }

    public static void main(String[] args) throws Exception {
        File tmp = new File(args[0]);
        rowFormat();
        flags();
        intervalAndRate();
        disabledAndReset();
        emptyInterval();
        pendingCap();
        selField();
        wireIdentity();
        threadSafety();
        noAddressNoLocation();
        ring(tmp);
        budget(tmp);
        lateWindowAfterDisconnect();
        exportRace(tmp);
        System.out.println("ab tests: " + passed + " passed, " + failed + " failed");
        if (failed != 0) System.exit(1);
    }

    static Window gyro(int dth, long us, boolean grav, boolean stat) {
        int f = (grav ? Protocol.FLAG_GRAVITY_VALID : 0) | (stat ? Protocol.FLAG_STATIONARY : 0);
        return new Window(f, 0, dth, us, 0, 4);
    }

    static Window obd(int dth, int extraFlags) {
        return new Window(Protocol.FLAG_SOURCE_OBD | extraFlags, 0, dth, Can079.PERIOD_US, 0, 1);
    }

    static AbCompare on() {
        AbCompare a = new AbCompare();
        a.setEnabled(true);
        return a;
    }

    static Map<String, String> parse(String row) {
        Map<String, String> m = new HashMap<>();
        // only failures are counted here (parse runs for every row of the larger tests)
        if (row == null) { check(false, "a row is queued"); return m; }
        String[] p = row.split(" ");
        if (!p[0].equals("ab")) check(false, "row starts with ab: " + row);
        for (int i = 1; i < p.length; i++) {
            int e = p[i].indexOf('=');
            if (e <= 0) check(false, "key=value: " + p[i]);
            else m.put(p[i].substring(0, e), p[i].substring(e + 1));
        }
        return m;
    }

    static void rowFormat() {
        AbCompare a = on();
        long t0 = 5 * S;
        a.add(PHONE, gyro(100, 20_000, true, false), true, t0);
        a.add(PHONE, gyro(-30, 20_500, true, true), true, t0 + 20 * MS);
        a.add(OBD, obd(-13, 0), false, t0 + 25 * MS);
        a.add(OBD, obd(40, 0), false, t0 + 45 * MS);
        a.add(OBD, obd(1, 0), false, t0 + 65 * MS);
        a.poll(t0 + 9_999 * MS);
        check(a.take() == null, "no row before 10 s");
        a.poll(t0 + 10_250 * MS);
        eqs(a.take(), "ab iv_ms=10250 sel=PHONE g_n=2 g_dur_us=40500 g_dth_urad=70 g_grav=1 g_st=1 o_n=3 o_dur_us=60000 o_dth_urad=28 o_gap=0",
                "both-sided row, exact text");
        check(a.take() == null, "one row per interval");
        // one side missing
        a.add(PHONE, gyro(5, 20_000, true, false), true, t0 + 11 * S);
        a.poll(t0 + 20_250 * MS);
        eqs(a.take(), "ab iv_ms=10000 sel=PHONE g_n=1 g_dur_us=20000 g_dth_urad=5 g_grav=1 g_st=0 miss=obd", "gyro-only row flagged miss=obd");
        a.add(OBD, obd(-7, 0), true, t0 + 21 * S);
        a.poll(t0 + 30_250 * MS);
        eqs(a.take(), "ab iv_ms=10000 sel=OBD o_n=1 o_dur_us=20000 o_dth_urad=-7 o_gap=0 miss=gyro", "obd-only row flagged miss=gyro");
        eq(a.oneSided, 2, "one-sided rows counted");
        eq(a.rows, 3, "rows counted");
        // large sums do not overflow an int (10 s of 100 deg/s, both sides)
        AbCompare b = on();
        for (int i = 0; i < 500; i++) {
            b.add(PHONE, gyro(34_907, 20_000, true, false), false, S + i * 20 * MS);
            b.add(OBD, obd(34_907, 0), true, S + i * 20 * MS);
        }
        for (int i = 0; i < 70_000; i++) b.add(PHONE, gyro(Integer.MAX_VALUE, 20_000, true, false), false, S + 9 * S);
        b.poll(S + 10 * S);
        Map<String, String> m = parse(b.take());
        eqs(m.get("o_dth_urad"), "17453500", "OBD sum 17.45 rad");
        eqs(m.get("g_dth_urad"), Long.toString(500L * 34_907 + 70_000L * Integer.MAX_VALUE), "gyro sum in long, no int overflow");
    }

    static void flags() {
        AbCompare a = on();
        a.add(PHONE, gyro(1, 20_000, true, true), false, 0);
        a.add(PHONE, gyro(1, 20_000, false, true), false, MS);
        a.add(PHONE, gyro(1, 20_000, true, false), false, 2 * MS);
        a.add(OBD, obd(1, 0), true, 3 * MS);
        a.poll(10 * S);
        Map<String, String> m = parse(a.take());
        eqs(m.get("g_grav"), "0", "one gravity-invalid window clears g_grav");
        eqs(m.get("g_st"), "2", "stationary windows counted");
        eqs(m.get("o_gap"), "0", "no loss flag");
        // obd_gap and the integrator's frame_reset (monitor restart) both set o_gap
        for (int f : new int[] {Protocol.FLAG_OBD_GAP, Protocol.FLAG_FRAME_RESET}) {
            AbCompare b = on();
            b.add(OBD, obd(1, 0), true, 0);
            b.add(OBD, obd(1, f), true, MS);
            b.add(PHONE, gyro(1, 20_000, true, false), false, MS);
            b.poll(10 * S);
            eqs(parse(b.take()).get("o_gap"), "1", "o_gap on flag " + f);
            // the flag does not leak into the next interval
            b.add(OBD, obd(1, 0), true, 11 * S);
            b.add(PHONE, gyro(1, 20_000, true, false), false, 11 * S);
            b.poll(20 * S);
            eqs(parse(b.take()).get("o_gap"), "0", "o_gap cleared at the next interval");
        }
        // g_grav of the next interval starts at 1 again
        AbCompare c = on();
        c.add(PHONE, gyro(1, 20_000, false, false), false, 0);
        c.poll(10 * S);
        eqs(parse(c.take()).get("g_grav"), "0", "gravity invalid");
        c.add(PHONE, gyro(1, 20_000, true, false), false, 11 * S);
        c.poll(20 * S);
        eqs(parse(c.take()).get("g_grav"), "1", "g_grav restarts per interval");
        // the selector's switch frame_reset is not an OBD loss: the window ab sees is the producer's
        SourceSelector sel = new SourceSelector(true);
        AbCompare d = on();
        long t = 0;
        int resetsSent = 0;
        for (int i = 0; i < 400; i++, t += 20 * MS) {
            sel.obdFrame(t);
            Window x = AbCompare.admit(sel, d, OBD, obd(1, 0), t);
            if (x != null && (x.flags & Protocol.FLAG_FRAME_RESET) != 0) resetsSent++;
            AbCompare.admit(sel, d, PHONE, gyro(1, 20_000, true, false), t);
        }
        check(sel.switches == 1 && resetsSent == 1, "selector switched to OBD inside the interval and sent frame_reset");
        d.poll(10 * S);
        Map<String, String> r = parse(d.take());
        eqs(r.get("o_gap"), "0", "switch frame_reset (selector) is not counted as an OBD gap");
        eqs(r.get("sel"), "MIX", "both sources sent in the switch interval");
        eqs(r.get("o_n"), "400", "held OBD windows (before the switch) are counted too");
        eqs(r.get("g_n"), "400", "held gyro windows (after the switch) are counted too");
    }

    static void intervalAndRate() {
        // A random schedule over 3 h of polls (1 s tick with jitter, stalls) and windows: rows >= 10 s apart, <= 360/h.
        Random r = new Random(7);
        AbCompare a = on();
        long t = 0, lastRow = Long.MIN_VALUE, minGap = Long.MAX_VALUE;
        int rows = 0;
        while (t < 3 * 3600 * S) {
            int n = r.nextInt(60);
            for (int i = 0; i < n; i++) a.add(r.nextBoolean() ? PHONE : OBD, r.nextBoolean() ? gyro(1, 20_000, true, false) : obd(1, 0), true, t);
            if (r.nextInt(50) == 0) a.reset();
            a.poll(t);
            if (a.take() != null) {
                rows++;
                if (lastRow != Long.MIN_VALUE) minGap = Math.min(minGap, t - lastRow);
                lastRow = t;
            }
            t += r.nextInt(10) == 0 ? (long) r.nextInt(4000) * MS : 1000 * MS + r.nextInt(50) * MS;
        }
        check(rows > 600, "rows produced: " + rows);
        check(rows <= 3 * 360, "at most 360 rows per hour: " + rows + " in 3 h");
        check(minGap >= AbCompare.INTERVAL_NS, "rows at least 10 s apart: min gap ms " + minGap / MS);
        // polling every 1 ms never closes an interval early
        AbCompare b = on();
        int got = 0;
        for (long u = 0; u < 60 * S; u += MS) {
            if (u % (20 * MS) == 0) b.add(PHONE, gyro(1, 20_000, true, false), true, u);
            b.poll(u);
            if (b.take() != null) got++;
        }
        eq(got, 5, "60 s of 1 ms polling: rows at 10, 20, 30, 40, 50 s");
    }

    static void disabledAndReset() {
        AbCompare a = new AbCompare();
        a.add(PHONE, gyro(9, 20_000, true, false), true, 0);
        a.poll(20 * S);
        check(a.take() == null && a.rows == 0, "disabled by default: no row");
        a.setEnabled(true);
        a.add(PHONE, gyro(9, 20_000, true, false), true, 21 * S);
        a.setEnabled(false);
        a.setEnabled(true);
        a.add(PHONE, gyro(2, 20_000, true, false), true, 25 * S);
        a.poll(34 * S);
        check(a.take() == null, "disable drops the open interval (the new one opened at 25 s)");
        a.poll(35 * S);
        eqs(parse(a.take()).get("g_dth_urad"), "2", "only the window after re-enable");
        // reset (session change): the partial interval is dropped, the next row holds only post-reset windows
        AbCompare b = on();
        b.add(PHONE, gyro(1000, 20_000, true, false), true, 0);
        b.add(OBD, obd(1000, 0), true, 0);
        b.poll(10 * S);
        String done = null;
        b.add(PHONE, gyro(500, 20_000, true, false), true, 11 * S);
        b.reset();
        b.add(PHONE, gyro(3, 20_000, true, false), true, 12 * S);
        b.add(OBD, obd(4, 0), true, 12 * S);
        b.poll(21 * S);
        check(b.take() != null, "a row completed before the reset stays queued");
        done = b.take();
        check(done == null, "no row 9 s after the reset");
        b.poll(22 * S);
        Map<String, String> m = parse(b.take());
        eqs(m.get("g_dth_urad"), "3", "post-reset gyro only");
        eqs(m.get("o_dth_urad"), "4", "post-reset OBD only");
        eqs(m.get("iv_ms"), "10000", "interval opened at the first post-reset window");
        eq(b.resets, 1, "reset counted");
    }

    static void emptyInterval() {
        AbCompare a = on();
        a.add(PHONE, gyro(1, 20_000, true, false), true, 0);
        a.poll(10 * S);
        check(a.take() != null, "first row");
        a.poll(20 * S);
        check(a.take() == null && a.emptyIntervals == 1, "no window in the next interval: no row");
        a.add(OBD, obd(6, 0), true, 95 * S);
        a.poll(104 * S);
        check(a.take() == null, "after an empty interval the next opens at the next window");
        a.poll(105 * S);
        Map<String, String> m = parse(a.take());
        eqs(m.get("iv_ms"), "10000", "fresh interval, no stale 85 s span");
        eqs(m.get("miss"), "gyro", "obd only");
    }

    static void pendingCap() {
        AbCompare a = on();
        for (int i = 0; i < AbCompare.MAX_PENDING + 3; i++) {
            a.add(PHONE, gyro(i, 20_000, true, false), true, i * 10 * S);
            a.poll(i * 10 * S + 10 * S - 1);
            a.poll((i + 1) * 10 * S);
        }
        eq(a.dropped, 3, "rows beyond the queue are dropped and counted");
        int n = 0;
        while (a.take() != null) n++;
        eq(n, AbCompare.MAX_PENDING, "queue holds MAX_PENDING rows");
        check(a.summary().contains("ab_dropped=3"), "summary shows drops: " + a.summary());
    }

    static void selField() {
        String[] want = {"NONE", "PHONE", "OBD", "MIX"};
        for (int k = 0; k < 4; k++) {
            AbCompare a = on();
            a.add(PHONE, gyro(1, 20_000, true, false), (k & 1) != 0, 0);
            a.add(OBD, obd(1, 0), (k & 2) != 0, 0);
            a.poll(10 * S);
            eqs(parse(a.take()).get("sel"), want[k], "sel from what was sent");
        }
    }

    /** What would go on the wire, as hex of each admitted window encoded with the session seq ("-" = held). */
    static List<String> drive(AbCompare ab, long seed, long[] counters) {
        Random r = new Random(seed);
        SourceSelector sel = new SourceSelector(true);
        List<String> out = new ArrayList<>();
        long seq = 0, obdNext = 7 * MS, gyroNext = 0, t = 0;
        Integrator integ = new Integrator();
        integ.gravity(0.3, 0.2, 9.7);
        ObdIntegrator oi = new ObdIntegrator();
        boolean obdUp = true;
        for (int step = 0; step < 200_000; step++) {
            if (r.nextInt(3000) == 0) obdUp = !obdUp;                   // adapter outages: switches both ways
            t += 5 * MS;
            boolean churn = r.nextInt(20_000) == 0;                      // drawn in both runs: the same random stream
            if (ab != null && churn) ab.reset();                         // session churn of the accumulator only
            if (t >= gyroNext) {
                gyroNext = t + 5 * MS;
                Window w = integ.gyro(t, r.nextGaussian() * 0.1, r.nextGaussian() * 0.1, r.nextGaussian() * 0.3);
                if (w != null) {
                    synchronized (sel) {
                        Window x = ab == null ? sel.admit(PHONE, w, t) : AbCompare.admit(sel, ab, PHONE, w, t);
                        out.add(x == null ? "-" : CoreTests.hex(x.encode(seq++)));
                    }
                }
            }
            while (obdUp && t >= obdNext) {
                obdNext += 20 * MS;
                byte[] d = {0, 0, 0, (byte) r.nextInt(3), (byte) r.nextInt(256), (byte) (r.nextInt(4) << 6), (byte) (step & 15), 0};
                Can079.Sample s = Can079.decode(Can079.ID, d);
                long rx = t + r.nextInt(60) * MS;
                Window w = oi.add(s, rx, r.nextInt(200) == 0 ? 1 : 0);
                if (w == null) continue;
                sel.obdFrame(w.tEndNs);
                synchronized (sel) {
                    Window x = ab == null ? sel.admit(OBD, w, t) : AbCompare.admit(sel, ab, OBD, w, t);
                    out.add(x == null ? "-" : CoreTests.hex(x.encode(seq++)));
                }
            }
            if (!obdUp) obdNext = t;
            if (ab != null && step % 200 == 0) {
                ab.poll(t);
                while (ab.take() != null) counters[0]++;
            }
        }
        counters[1] = sel.switches;
        counters[2] = sel.phoneWindowsSent;
        counters[3] = sel.obdWindowsSent;
        counters[4] = sel.phoneWindowsHeld + sel.obdWindowsHeld;
        return out;
    }

    static void wireIdentity() {
        for (long seed : new long[] {1, 2, 3}) {
            long[] off = new long[5], onc = new long[5];
            List<String> a = drive(null, seed, off);
            List<String> b = drive(on(), seed, onc);
            check(a.equals(b), "wire output byte-identical with the A/B accumulator on and off, seed " + seed);
            check(Arrays.equals(Arrays.copyOfRange(off, 1, 5), Arrays.copyOfRange(onc, 1, 5)), "selector counters identical, seed " + seed);
            check(off[1] >= 4, "the run switched sources (" + off[1] + " switches), seed " + seed);
            check(onc[0] > 50, "the accumulator produced rows (" + onc[0] + "), seed " + seed);
            check(a.size() > 50_000, "frames compared: " + a.size());
        }
        // disabled accumulator: identical too, and no rows
        long[] off = new long[5], dis = new long[5];
        check(drive(null, 9, off).equals(drive(new AbCompare(), 9, dis)), "wire identical with the accumulator present but disabled");
        eq(dis[0], 0, "disabled: no rows");
    }

    static void threadSafety() throws InterruptedException {
        final AbCompare a = on();
        final AtomicLong now = new AtomicLong(0);
        final int n = 400_000;
        final List<String> rows = new ArrayList<>();
        Thread g = new Thread(() -> { for (int i = 0; i < n; i++) a.add(PHONE, gyro(3, 20_000, (i % 1000) != 0, (i & 1) == 0), (i & 2) == 0, now.get()); });
        Thread o = new Thread(() -> { for (int i = 0; i < n; i++) a.add(OBD, obd(-5, 0), true, now.get()); });
        final boolean[] stop = {false};
        Thread p = new Thread(() -> {
            while (true) {
                synchronized (stop) { if (stop[0]) return; }
                now.addAndGet(S);
                a.poll(now.get());
                for (String r = a.take(); r != null; r = a.take()) rows.add(r);
            }
        });
        g.start(); o.start(); p.start();
        g.join(); o.join();
        synchronized (stop) { stop[0] = true; }
        p.join();
        now.addAndGet(100 * S);
        a.poll(now.get());
        for (String r = a.take(); r != null; r = a.take()) rows.add(r);
        long gn = 0, gd = 0, gdur = 0, on = 0, od = 0, gst = 0;
        for (String r : rows) {
            Map<String, String> m = parse(r);
            if (m.containsKey("g_n")) {
                gn += Long.parseLong(m.get("g_n"));
                gd += Long.parseLong(m.get("g_dth_urad"));
                gdur += Long.parseLong(m.get("g_dur_us"));
                gst += Long.parseLong(m.get("g_st"));
            }
            if (m.containsKey("o_n")) {
                on += Long.parseLong(m.get("o_n"));
                od += Long.parseLong(m.get("o_dth_urad"));
            }
        }
        eq(a.dropped, 0, "threads: no row dropped (rows taken each poll)");
        eq(gn, n, "threads: every gyro window in exactly one row");
        eq(gd, 3L * n, "threads: gyro dtheta conserved");
        eq(gdur, 20_000L * n, "threads: gyro duration conserved");
        eq(gst, n / 2, "threads: stationary count conserved");
        eq(on, n, "threads: every OBD window in exactly one row");
        eq(od, -5L * n, "threads: OBD dtheta conserved");
        check(rows.size() > 1, "threads: several rows " + rows.size());
    }

    static final Pattern MAC = Pattern.compile("(?i)[0-9a-f]{2}(:[0-9a-f]{2}){5}");

    static void noAddressNoLocation() {
        Set<String> allowed = new HashSet<>(Arrays.asList("iv_ms", "sel", "g_n", "g_dur_us", "g_dth_urad", "g_grav", "g_st", "o_n",
                "o_dur_us", "o_dth_urad", "o_gap", "miss"));
        Random r = new Random(3);
        AbCompare a = on();
        int checked = 0;
        for (int k = 0; k < 200; k++) {
            int m = r.nextInt(3);
            if (m != 1) a.add(PHONE, gyro(r.nextInt(), (long) r.nextInt(30_000), r.nextBoolean(), r.nextBoolean()), r.nextBoolean(), k * 10 * S);
            if (m != 0) a.add(OBD, obd(r.nextInt(), r.nextBoolean() ? Protocol.FLAG_OBD_GAP : 0), r.nextBoolean(), k * 10 * S);
            a.poll(k * 10 * S + 10 * S);
            String row = a.take();
            if (row == null) continue;
            checked++;
            check(!MAC.matcher(row).find(), "no Bluetooth address: " + row);
            check(row.matches("ab( [a-z_]+=[A-Za-z0-9-]+)+"), "only key=value tokens: " + row);
            check(allowed.containsAll(parse(row).keySet()), "only the documented keys: " + row);
            check(!row.contains("lat") && !row.contains("lon") && !row.contains("device"), "no location or device field: " + row);
        }
        check(checked > 150, "rows checked " + checked);
    }

    static File freshDir(File tmp, String name) {
        File d = new File(tmp, name);
        File[] old = d.listFiles();
        if (old != null) for (File f : old) if (!f.delete()) check(false, "clear " + f);
        check(d.mkdirs() || d.isDirectory(), "dir " + name);
        return d;
    }

    static void ring(File tmp) throws IOException {
        File d = freshDir(tmp, "abring");
        CoreTests.FakeClock clock = new CoreTests.FakeClock();
        RollingLog main = new RollingLog(d, clock), ab = RollingLog.ab(d, clock);
        main.event("connected seq=0");
        main.event("breaker_open failures=3 retry_in_min=60");
        String row = "ab iv_ms=10000 sel=OBD g_n=500 g_dur_us=10000000 g_dth_urad=-1234567 g_grav=1 g_st=0 o_n=500 o_dur_us=10000000 o_dth_urad=-1230000 o_gap=0";
        for (int i = 0; i < 20_000; i++) ab.event(row + " #" + i);   // about 55 h of rows
        check(ab.totalBytes() <= ab.cap() && ab.totalBytes() > RollingLog.AB_HALF, "ab ring bounded: " + ab.totalBytes());
        eq(ab.cap(), 512L * 1024L, "ab ring cap 512 KiB");
        eq(main.cap(), 1024L * 1024L, "main log cap stays 1 MiB");
        check(new File(d, "log.txt").length() < 200, "ab rows never go into the main log");
        File exp = new File(d, "export.txt");
        main.exportTo(exp, ab);
        String s = new String(Files.readAllBytes(exp.toPath()), StandardCharsets.UTF_8);
        int c = s.indexOf("connected seq=0"), b = s.indexOf("breaker_open"), last = s.indexOf(" #19999");
        check(c >= 0 && b > c, "export keeps the connection and breaker events");
        check(last > b && !s.contains(" #0\n"), "export holds the newest ab rows after the main log, oldest rotated away");
        File only = new File(d, "export1.txt");
        main.exportTo(only);
        check(!new String(Files.readAllBytes(only.toPath()), StandardCharsets.UTF_8).contains("ab iv_ms"), "exportTo without extra rings: main only");
    }

    /**
     * Review 2026-10-17 finding 7: disconnected() resets the accumulator, then one sensor event that had read the
     * session as active adds a window. The service's tick polls with live = (session != null): no junk row.
     */
    static void lateWindowAfterDisconnect() {
        AbCompare a = on();
        a.add(PHONE, gyro(10, 20_000, true, false), true, 0);
        a.add(OBD, obd(10, 0), true, 0);
        a.poll(10 * S, true);
        String done = a.take();
        check(done != null, "row of the live session");
        a.reset();                                                    // disconnected()
        a.add(PHONE, gyro(77, 20_000, true, false), true, 10 * S + 5 * MS);   // the late event
        a.poll(11 * S, false);                                        // tick: session gone
        eq(a.idleDrops, 1, "late window dropped by the idle tick");
        for (long t = 12 * S; t <= 40 * S; t += S) a.poll(t, false);
        check(a.take() == null, "no junk one-window row after a disconnect");
        eq(a.rows, 1, "only the live row counted");
        eq(a.idleDrops, 1, "an empty interval is not counted again");
        check(a.summary().contains("ab_idle_drops=1"), "summary shows idle drops: " + a.summary());
        // a row completed while live stays queued through an idle poll
        AbCompare b = on();
        b.add(PHONE, gyro(3, 20_000, true, false), true, 0);
        b.poll(10 * S, true);
        b.poll(11 * S, false);
        check(b.take() != null && b.idleDrops == 0, "completed row survives the idle poll");
        // the next session (connected(): reset, then windows) produces rows again
        a.reset();
        a.add(PHONE, gyro(4, 20_000, true, false), true, 50 * S);
        a.add(OBD, obd(4, 0), true, 50 * S);
        a.poll(60 * S, true);
        Map<String, String> m = parse(a.take());
        eqs(m.get("g_dth_urad"), "4", "next session's row holds only its own windows");
        eqs(m.get("iv_ms"), "10000", "next session's interval opened at its first window");
        // poll(now) is poll(now, true)
        AbCompare c = on();
        c.add(PHONE, gyro(1, 20_000, true, false), true, 0);
        c.poll(10 * S);
        check(c.take() != null, "poll(now) keeps the live behaviour");
    }

    /**
     * Review 2026-10-17 finding 6: the export (MainActivity: its own RollingLog instances) and the writer
     * (SenderService) are different objects. A rotation between the copy of ab.1.txt and ab.txt must not leave out
     * a half: the test hook makes a writer thread try to rotate exactly there.
     */
    static void exportRace(File tmp) throws Exception {
        File d = freshDir(tmp, "abrace");
        CoreTests.FakeClock clock = new CoreTests.FakeClock();
        final RollingLog writer = RollingLog.ab(d, clock);
        RollingLog exporter = RollingLog.ab(d, clock);
        File cur = new File(d, "ab.txt");
        int i = 0;
        String fmt = "ab iv_ms=10000 sel=OBD g_n=500 g_dur_us=10000000 g_dth_urad=-1234567 g_grav=1 g_st=0 o_n=500 #%06d";
        long line = -1;
        boolean rotated = false;
        while (true) {
            long before = cur.length();
            writer.event(String.format(Locale.ROOT, fmt, i++));
            if (line < 0) line = cur.length();
            if (cur.length() < before) rotated = true;
            if (rotated && cur.length() + line > RollingLog.AB_HALF) break;   // the next row rotates
        }
        final int next = i;
        final AtomicBoolean once = new AtomicBoolean();
        final Thread[] w = new Thread[1];
        RollingLog.betweenFilesForTest = () -> {
            if (!once.compareAndSet(false, true)) return;
            w[0] = new Thread(() -> writer.event(String.format(Locale.ROOT, fmt, next)));
            w[0].start();
            try { w[0].join(300); } catch (InterruptedException e) { Thread.currentThread().interrupt(); }
        };
        File exp = new File(d, "export.txt");
        try {
            exporter.exportTo(exp);
        } finally {
            RollingLog.betweenFilesForTest = null;
        }
        if (w[0] != null) w[0].join();
        check(once.get(), "hook ran between the two files");
        List<Integer> nums = new ArrayList<>();
        for (String l : new String(Files.readAllBytes(exp.toPath()), StandardCharsets.UTF_8).split("\n")) {
            int k = l.lastIndexOf('#');
            if (k >= 0) nums.add(Integer.parseInt(l.substring(k + 1)));
        }
        boolean contiguous = !nums.isEmpty();
        for (int k = 1; k < nums.size(); k++) if (nums.get(k) != nums.get(k - 1) + 1) contiguous = false;
        check(contiguous, "export holds both halves without a gap (" + nums.size() + " rows)");
        check(!nums.isEmpty() && nums.get(nums.size() - 1) == next - 1, "export ends at the last row before the blocked rotation");
        check(nums.size() > RollingLog.AB_HALF / line, "export holds more than one half: " + nums.size());
        check(new String(Files.readAllBytes(cur.toPath()), StandardCharsets.UTF_8).contains(String.format(Locale.ROOT, "#%06d", next)),
                "the blocked writer wrote its row after the export");
    }

    static void budget(File tmp) throws IOException {
        File d = freshDir(tmp, "abbudget");
        CoreTests.FakeClock clock = new CoreTests.FakeClock();
        clock.t.set(4_302_205_167L * MS);   // a phone elapsedRealtime of 50 days, as in the owner's export
        RollingLog ab = RollingLog.ab(d, clock);
        AbCompare a = on();
        // a typical driving row: 500 gyro windows, 500 OBD frames, 100 deg/s worth of digits
        for (int i = 0; i < 500; i++) {
            a.add(PHONE, gyro(-34_907, 20_013, true, false), false, i * 20 * MS);
            a.add(OBD, obd(-34_907, 0), true, i * 20 * MS);
        }
        a.poll(10_400 * MS);
        String row = a.take();
        ab.event(row);
        long line = new File(d, "ab.txt").length();
        System.out.println("ab budget: typical full row " + line + " B/line, 360 rows/h = " + (360 * line) + " B/h, ring "
                + RollingLog.AB_HALF * 2 + " B = " + String.format(java.util.Locale.ROOT, "%.1f", RollingLog.AB_HALF * 2.0 / (360.0 * line)) + " h");
        check(line <= 200, "a full row line stays at or below 200 B: " + line);
        check(360 * line <= 72_000, "at most about 70 KB/h at the 360 rows/h cap: " + 360 * line);
        check(RollingLog.AB_HALF * 2 / (360 * line) >= 7, "ring holds at least 7 h of full rows");
    }
}
