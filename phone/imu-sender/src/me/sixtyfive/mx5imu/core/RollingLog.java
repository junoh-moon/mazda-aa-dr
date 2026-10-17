package me.sixtyfive.mx5imu.core;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;
import java.util.concurrent.ConcurrentHashMap;

/**
 * Small private diagnostic log: events only (connections, errors, counters), never sensor samples.
 * Two files of at most HALF bytes each, so the total stays at or below 1 MiB.
 *
 * The A/B comparison rows (AbCompare: 10 s sums of the gyro and OBD windows, no samples, no address,
 * no location) go to a separate ring, ab(): its own two files of at most AB_HALF bytes each, so they
 * never rotate the connection/breaker events of the main log away. Export writes both.
 *
 * Locking: the export (MainActivity, its own RollingLog instances) and the writer (SenderService) are different
 * objects on the same files, so every instance of one ring shares one lock, keyed by the absolute path of its
 * current file. A rotation can therefore never land between the copy of the older and the current file of an
 * export, which would leave out one half of the ring (review 2026-10-17, finding 6). The export never clears a
 * ring: successive exports overlap and the analyzer (tools/pimu_ab.py) drops the exact duplicates.
 */
public final class RollingLog {
    public static final long HALF = 512L * 1024L;
    /** Half of the A/B ring (total at most 512 KiB, about 9 h of rows at the 10 s cadence). */
    public static final long AB_HALF = 256L * 1024L;
    private static final ConcurrentHashMap<String, Object> LOCKS = new ConcurrentHashMap<>();
    /** Test hook: run inside copyTo between the older and the current file (null in the app). */
    static volatile Runnable betweenFilesForTest;
    private final File current, previous;
    private final Clock clock;
    private final long half;
    private final Object lock;

    public RollingLog(File dir, Clock clock) { this(dir, "log", HALF, clock); }

    private RollingLog(File dir, String base, long half, Clock clock) {
        this.current = new File(dir, base + ".txt");
        this.previous = new File(dir, base + ".1.txt");
        this.clock = clock;
        this.half = half;
        this.lock = LOCKS.computeIfAbsent(current.getAbsolutePath(), k -> new Object());
    }

    /** The separate ring of the A/B comparison rows (ab.txt, ab.1.txt). */
    public static RollingLog ab(File dir, Clock clock) { return new RollingLog(dir, "ab", AB_HALF, clock); }

    /** The byte cap of this ring (two halves). */
    public long cap() { return 2 * half; }

    public void event(String line) {
        String stamp = new SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss.SSSZ", Locale.ROOT).format(new Date());
        byte[] b = (stamp + " t=" + clock.nanos() / 1_000_000L + "ms " + line.replace('\n', ' ') + "\n")
                .getBytes(StandardCharsets.UTF_8);
        if (b.length > half) return;
        synchronized (lock) {
            if (current.length() + b.length > half) {
                if (previous.exists() && !previous.delete()) return;
                if (!current.renameTo(previous)) current.delete();
            }
            try (OutputStream o = new FileOutputStream(current, true)) {
                o.write(b);
            } catch (IOException ignored) {
                // A diagnostic log must never stop the sender.
            }
        }
    }

    /** Writes the older file then the current one into dest (for export), then each of also's the same way. */
    public void exportTo(File dest, RollingLog... also) throws IOException {
        try (OutputStream o = new FileOutputStream(dest)) {
            copyTo(o);
            for (RollingLog l : also) l.copyTo(o);
        }
    }

    private void copyTo(OutputStream o) throws IOException {
        synchronized (lock) {
            boolean first = true;
            for (File f : new File[] {previous, current}) {
                if (!first) {
                    Runnable hook = betweenFilesForTest;
                    if (hook != null) hook.run();
                }
                first = false;
                if (!f.exists()) continue;
                try (InputStream in = new FileInputStream(f)) {
                    byte[] buf = new byte[8192];
                    int n;
                    while ((n = in.read(buf)) > 0) o.write(buf, 0, n);
                }
            }
        }
    }

    public long totalBytes() {
        synchronized (lock) { return current.length() + previous.length(); }
    }
}
