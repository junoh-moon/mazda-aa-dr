package me.sixtyfive.mx5imu.core;

import java.io.ByteArrayOutputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.PipedInputStream;
import java.io.PipedOutputStream;
import java.io.PrintWriter;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.Locale;
import java.util.Random;
import java.util.concurrent.atomic.AtomicLong;

/**
 * Host end-to-end driver: runs the real Integrator and Session against a fake transport whose input
 * is a CMU ping stream produced by e2e_check.py, and writes the phone's output bytes plus the raw
 * synthetic samples (so the Python side recomputes the spec integration independently).
 * Usage: E2EDump PINGS.bin OUT.bin SAMPLES.csv
 */
public final class E2EDump {
    public static void main(String[] args) throws Exception {
        byte[] pings = Files.readAllBytes(Paths.get(args[0]));
        final PipedOutputStream feed = new PipedOutputStream();
        final PipedInputStream in = new PipedInputStream(feed, 4096);
        final ByteArrayOutputStream out = new ByteArrayOutputStream();
        final long base = 1_000_000_000_000L - System.nanoTime();
        Clock clock = () -> base + System.nanoTime();
        Transport t = new Transport() {
            @Override public InputStream input() { return in; }
            @Override public OutputStream output() { return out; }
            @Override public void close() {
                try { in.close(); } catch (IOException ignored) {}
                try { feed.close(); } catch (IOException ignored) {}
            }
        };
        AtomicLong seq = new AtomicLong(0xFFFFFFF0L);
        Session s = new Session(t, clock, seq);
        s.start();
        final java.util.concurrent.CountDownLatch done = new java.util.concurrent.CountDownLatch(1);
        Thread feeder = new Thread(() -> {
            try {
                for (int i = 0; i < pings.length; i += 5) {
                    feed.write(pings, i, Math.min(5, pings.length - i));
                    feed.flush();
                    Thread.sleep(2);
                }
                done.await(); // a PipedInputStream reports an error once its writer thread has ended
            } catch (Exception ignored) {
                // closed by the session at the end
            }
        });
        feeder.start();

        // The phone sends no DATA before the first PING of the session (spec): start the sensor
        // stream once the session is armed, as the service would see it.
        long armBy = System.currentTimeMillis() + 5000;
        while (!s.pinged() && System.currentTimeMillis() < armBy) Thread.sleep(1);
        if (!s.pinged()) throw new AssertionError("no ping arrived");
        Integrator integ = new Integrator();
        Random rnd = new Random(4242);
        int offered = 0;
        try (PrintWriter csv = new PrintWriter(new FileOutputStream(args[2]))) {
            long tNs = 77_000_000_000L;
            for (int i = 0; i < 600; i++) {
                double time = i * 0.005;
                double pitch = Math.toRadians(25 + 5 * Math.sin(time)), roll = Math.toRadians(-8);
                double[] down = {Math.sin(roll) * Math.cos(pitch), -Math.cos(roll) * Math.cos(pitch), -Math.sin(pitch)};
                double n = Math.sqrt(down[0] * down[0] + down[1] * down[1] + down[2] * down[2]);
                for (int k = 0; k < 3; k++) down[k] /= n;
                if (i % 2 == 0) {
                    double gx = -9.81 * down[0], gy = -9.81 * down[1], gz = -9.81 * down[2];
                    integ.gravity(gx, gy, gz);
                    csv.printf(Locale.ROOT, "g,%d,%s,%s,%s%n", tNs, gx, gy, gz);
                }
                if (i % 20 == 0) {
                    double bx = 0.001, by = -0.0005, bz = 0.0002;
                    integ.bias(tNs, bx, by, bz);
                    csv.printf(Locale.ROOT, "b,%d,%s,%s,%s%n", tNs, bx, by, bz);
                }
                double yaw = 0.3 * Math.sin(2 * Math.PI * time / 1.5) - 0.05;
                double perp = 0.2 * Math.cos(3 * time);
                double[] w = {yaw * down[0] + perp * down[1], yaw * down[1] - perp * down[0], yaw * down[2]};
                csv.printf(Locale.ROOT, "w,%d,%s,%s,%s%n", tNs, w[0], w[1], w[2]);
                Window win = integ.gyro(tNs, w[0], w[1], w[2]);
                if (win != null) {
                    if (!s.offer(win)) throw new AssertionError("offer refused: " + s.closeReason());
                    offered++;
                }
                tNs += 4_900_000L + rnd.nextInt(200_000);
                if (i % 4 == 0) Thread.sleep(1);
            }
        }
        long deadline = System.currentTimeMillis() + 10_000;
        while ((s.framesSent.get() < offered || s.pongsSent.get() < 6) && System.currentTimeMillis() < deadline) Thread.sleep(5);
        s.close("done");
        done.countDown();
        s.join(2000);
        feeder.join(2000);
        if (s.framesSent.get() != offered || s.pongsSent.get() != 6 || s.crcErrors() != 1 || s.droppedBeforePing.get() != 0) {
            throw new AssertionError("frames " + s.framesSent.get() + "/" + offered + " pongs " + s.pongsSent.get() + " crc " + s.crcErrors());
        }
        try (FileOutputStream o = new FileOutputStream(args[1])) { o.write(out.toByteArray()); }
        System.out.println("e2e dump frames=" + offered + " pongs=" + s.pongsSent.get() + " seq_end=" + Long.toHexString(seq.get()));
    }
}
