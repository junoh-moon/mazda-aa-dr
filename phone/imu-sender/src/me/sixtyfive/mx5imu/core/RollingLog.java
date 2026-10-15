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

/**
 * Small private diagnostic log: events only (connections, errors, counters), never sensor samples.
 * Two files of at most HALF bytes each, so the total stays at or below 1 MiB.
 */
public final class RollingLog {
    public static final long HALF = 512L * 1024L;
    private final File current, previous;
    private final Clock clock;

    public RollingLog(File dir, Clock clock) {
        this.current = new File(dir, "log.txt");
        this.previous = new File(dir, "log.1.txt");
        this.clock = clock;
    }

    public synchronized void event(String line) {
        String stamp = new SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss.SSSZ", Locale.ROOT).format(new Date());
        byte[] b = (stamp + " t=" + clock.nanos() / 1_000_000L + "ms " + line.replace('\n', ' ') + "\n")
                .getBytes(StandardCharsets.UTF_8);
        if (b.length > HALF) return;
        if (current.length() + b.length > HALF) {
            if (previous.exists() && !previous.delete()) return;
            if (!current.renameTo(previous)) current.delete();
        }
        try (OutputStream o = new FileOutputStream(current, true)) {
            o.write(b);
        } catch (IOException ignored) {
            // A diagnostic log must never stop the sender.
        }
    }

    /** Writes the older file then the current one into dest (for export). */
    public synchronized void exportTo(File dest) throws IOException {
        try (OutputStream o = new FileOutputStream(dest)) {
            for (File f : new File[] {previous, current}) {
                if (!f.exists()) continue;
                try (InputStream in = new FileInputStream(f)) {
                    byte[] buf = new byte[8192];
                    int n;
                    while ((n = in.read(buf)) > 0) o.write(buf, 0, n);
                }
            }
        }
    }

    public synchronized long totalBytes() { return current.length() + previous.length(); }
}
