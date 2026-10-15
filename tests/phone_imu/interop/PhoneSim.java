package me.sixtyfive.mx5imu.core;

import java.io.BufferedReader;
import java.io.DataOutputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.Random;

/**
 * Host "fake phone" of the phone IMU interop rig (tests/phone_imu/interop/rig.py): the REAL pure-Java
 * core of phone/imu-sender (Link, ReconnectPolicy, Session, Integrator, FrameParser, Protocol) with the
 * Android pieces replaced. The RFCOMM socket is a TCP connection to the rig's fake bds; the sensor
 * thread feeds synthetic 200 Hz gyro, gravity and uncalibrated-gyro bias samples of a known yaw-rate
 * profile; Bluetooth broadcasts (ACL, adapter) and the app's "retry now" come as stdin commands. The wiring
 * mirrors SenderService (Link(…, 2000, 30000, 500), integrator.reset() at connect and disconnect,
 * windows offered to the active session only). Not product code.
 *
 * Args: --port N --offset-ns N --accel X --profile "dur_s:deg_s,..." --tilt-deg roll,pitch
 *       --noise-rad-s X --seed N --tee FILE [--breaker trip,openMs,healthyMs,earlyMs]
 *       [--backoff minMs,maxMs] [--connect-timeout-ms N]
 * stdin: "acl up|down", "adapter on|off", "retry_now", "app_opened" (no effect on the breaker), "quit".
 * stdout: one JSON object per line ("ev" plus fields; "t_ms" is System.nanoTime()/1e6 = CLOCK_MONOTONIC).
 * Session clock = System.nanoTime() + offset (the simulated elapsedRealtimeNanos offset to the CMU clock).
 * Sensor time = sensorBase + accel * real elapsed (simulated, may run faster than the session clock).
 */
public final class PhoneSim implements Link.Listener {
    private static final Object OUT = new Object();
    private final long offsetNs;
    private final Clock clock;
    private final double accel;
    private final double[] segDur, segRate;
    private final double profileLen;
    private final double[][] m = new double[3][3];
    private final double noise;
    private final Random rnd;
    private final DataOutputStream tee;
    private final int port, connectTimeoutMs;
    private Link link;
    private volatile Session active;
    private volatile boolean resetIntegrator, quit;
    private final Integrator integrator = new Integrator();
    private long sensorBase;

    PhoneSim(String[] a) throws IOException {
        String profile = "60:0", tilt = "0,0", teePath = null, breaker = null, backoff = "2000,30000";
        long off = 0, seed = 1;
        double acc = 1, nz = 0;
        int p = 0, cto = 3000;
        for (int i = 0; i + 1 < a.length; i += 2) {
            switch (a[i]) {
                case "--port": p = Integer.parseInt(a[i + 1]); break;
                case "--offset-ns": off = Long.parseLong(a[i + 1]); break;
                case "--accel": acc = Double.parseDouble(a[i + 1]); break;
                case "--profile": profile = a[i + 1]; break;
                case "--tilt-deg": tilt = a[i + 1]; break;
                case "--noise-rad-s": nz = Double.parseDouble(a[i + 1]); break;
                case "--seed": seed = Long.parseLong(a[i + 1]); break;
                case "--tee": teePath = a[i + 1]; break;
                case "--breaker": breaker = a[i + 1]; break;
                case "--backoff": backoff = a[i + 1]; break;
                case "--connect-timeout-ms": cto = Integer.parseInt(a[i + 1]); break;
                default: throw new IllegalArgumentException("unknown arg " + a[i]);
            }
        }
        port = p; offsetNs = off; accel = acc; noise = nz; rnd = new Random(seed); connectTimeoutMs = cto;
        final long o = off;
        clock = () -> System.nanoTime() + o;
        String[] segs = profile.split(",");
        segDur = new double[segs.length]; segRate = new double[segs.length];
        double len = 0;
        for (int i = 0; i < segs.length; i++) {
            String[] kv = segs[i].split(":");
            segDur[i] = Double.parseDouble(kv[0]);
            segRate[i] = Math.toRadians(Double.parseDouble(kv[1]));
            len += segDur[i];
        }
        profileLen = len;
        String[] t = tilt.split(",");
        double r = Math.toRadians(Double.parseDouble(t[0])), q = Math.toRadians(Double.parseDouble(t[1]));
        // m = Ry(q) * Rx(r): world (ENU, z up) vector -> phone axes of a tilted mount.
        double[][] rx = {{1, 0, 0}, {0, Math.cos(r), -Math.sin(r)}, {0, Math.sin(r), Math.cos(r)}};
        double[][] ry = {{Math.cos(q), 0, Math.sin(q)}, {0, 1, 0}, {-Math.sin(q), 0, Math.cos(q)}};
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) { double s = 0; for (int k = 0; k < 3; k++) s += ry[i][k] * rx[k][j]; m[i][j] = s; }
        tee = teePath == null ? null : new DataOutputStream(new FileOutputStream(teePath));
        String[] bo = backoff.split(",");
        ReconnectPolicy.Breaker br = ReconnectPolicy.Breaker.DEFAULT;
        if (breaker != null) {
            String[] b = breaker.split(",");
            br = new ReconnectPolicy.Breaker(Integer.parseInt(b[0]), Long.parseLong(b[1]), Long.parseLong(b[2]), Long.parseLong(b[3]));
        }
        link = new Link(new TcpConnector(), this, clock, Long.parseLong(bo[0]), Long.parseLong(bo[1]), 500, br);
    }

    static void emit(String ev, String fields) {
        synchronized (OUT) {
            System.out.println("{\"ev\":\"" + ev + "\",\"t_ms\":" + String.format(Locale.ROOT, "%.3f", System.nanoTime() / 1e6)
                    + (fields.isEmpty() ? "" : "," + fields) + "}");
            System.out.flush();
        }
    }

    /** Clockwise yaw rate (rad/s) at simulated time s (seconds since the sensor start); the profile repeats. */
    double rate(double s) {
        double x = s % profileLen;
        for (int i = 0; i < segDur.length; i++) { if (x < segDur[i]) return segRate[i]; x -= segDur[i]; }
        return segRate[segRate.length - 1];
    }

    double[] toPhone(double x, double y, double z) {
        return new double[] {m[0][0] * x + m[0][1] * y + m[0][2] * z, m[1][0] * x + m[1][1] * y + m[1][2] * z,
                m[2][0] * x + m[2][1] * y + m[2][2] * z};
    }

    // ---- transport ----
    final class TcpTransport implements Transport {
        final Socket s;
        TcpTransport(Socket s) { this.s = s; }
        @Override public InputStream input() throws IOException { return s.getInputStream(); }
        @Override public OutputStream output() throws IOException {
            final OutputStream o = s.getOutputStream();
            return new OutputStream() {
                @Override public void write(int b) throws IOException { write(new byte[] {(byte) b}, 0, 1); }
                @Override public void write(byte[] b, int off, int len) throws IOException {
                    o.write(b, off, len);
                    if (tee != null) synchronized (tee) { tee.writeLong(System.nanoTime()); tee.writeInt(len); tee.write(b, off, len); }
                }
                @Override public void flush() throws IOException { o.flush(); if (tee != null) synchronized (tee) { tee.flush(); } }
            };
        }
        @Override public void close() { try { s.close(); } catch (IOException e) { /* closing */ } }
    }

    final class TcpConnector implements Transport.Connector {
        private volatile Socket pending;
        @Override public Transport open() throws IOException {
            Socket s = new Socket();
            pending = s;
            try {
                s.setTcpNoDelay(true);
                s.connect(new InetSocketAddress("127.0.0.1", port), connectTimeoutMs);
            } catch (IOException e) {
                s.close();
                throw e;
            } finally {
                pending = null;
            }
            return new TcpTransport(s);
        }
        @Override public void abort() { Socket s = pending; if (s != null) try { s.close(); } catch (IOException e) { /* aborting */ } }
    }

    // ---- Link.Listener ----
    @Override public void connected(Session s) {
        resetIntegrator = true;
        active = s;
        emit("connected", "\"seq\":" + (link.seq().get() & 0xFFFFFFFFL));
    }

    @Override public void disconnected(Session s, String reason) {
        if (active == s) active = null;
        resetIntegrator = true;
        long fp = s.firstPingNs();
        emit("disconnected", String.format(Locale.ROOT,
                "\"reason\":\"%s\",\"frames\":%d,\"pongs\":%d,\"pings\":%d,\"crc_errors\":%d,\"unknown_version\":%d,\"unexpected\":%d,"
                        + "\"dropped_before_ping\":%d,\"ping_span_ms\":%d,\"first_ping_ms\":%s,\"breaker\":\"%s\",\"acl\":\"%s\"",
                reason, s.framesSent.get(), s.pongsSent.get(), s.pingsReceived.get(), s.crcErrors(), s.unknownVersion(),
                s.unexpectedFrames.get(), s.droppedBeforePing.get(), s.pingSpanMs(),
                fp == 0 ? "null" : String.format(Locale.ROOT, "%.3f", (fp - offsetNs) / 1e6), link.breakerState(), link.acl()));
    }

    @Override public void connectFailed(String error, long nextWaitMs, ReconnectPolicy.Acl acl) {
        emit("connect_failed", "\"next_ms\":" + nextWaitMs + ",\"acl\":\"" + acl + "\",\"error\":\"" + error.replace('"', '\'') + "\"");
    }

    @Override public void breaker(ReconnectPolicy.BreakerState state, long untilMs) {
        emit("breaker", "\"state\":\"" + state + "\",\"until_ms\":" + untilMs);
    }

    // ---- sensors ----
    private void sensorLoop() {
        long real0 = System.nanoTime();
        sensorBase = clock.nanos();
        emit("sensor_start", "\"sensor_base_ns\":" + sensorBase + ",\"accel\":" + accel);
        final long stepNs = 5_000_000L; // 200 Hz
        long k = 0;
        double[] bPhone = {0.0021, -0.0013, 0.0007}; // OS bias estimate (rad/s, phone axes)
        while (!quit) {
            long target = (long) ((System.nanoTime() - real0) * accel);
            while (k * stepNs <= target && !quit) {
                long tSim = k * stepNs;
                long tNs = sensorBase + tSim;
                Session s = active;
                if (s != null) {
                    if (resetIntegrator) { integrator.reset(); resetIntegrator = false; }
                    double[] g = toPhone(0, 0, 9.80665);           // TYPE_GRAVITY points up
                    if (k % 4 == 0) { integrator.gravity(g[0], g[1], g[2]); integrator.bias(tNs, bPhone[0], bPhone[1], bPhone[2]); }
                    double r = rate(tSim * 1e-9);
                    double[] w = toPhone(0, 0, -r);                  // clockwise = rotation about down
                    if (noise > 0) for (int i = 0; i < 3; i++) w[i] += noise * rnd.nextGaussian();
                    Window win = integrator.gyro(tNs, w[0], w[1], w[2]);
                    if (win != null) s.offer(win);
                } else if (resetIntegrator) { integrator.reset(); resetIntegrator = false; }
                k++;
            }
            try { Thread.sleep(2); } catch (InterruptedException e) { return; }
        }
    }

    void run() throws Exception {
        Thread sensor = new Thread(this::sensorLoop, "sensor");
        sensor.start();
        link.start();
        emit("started", "\"offset_ns\":" + offsetNs);
        BufferedReader in = new BufferedReader(new InputStreamReader(System.in, StandardCharsets.UTF_8));
        String line;
        while ((line = in.readLine()) != null) {
            line = line.trim();
            if (line.equals("acl up")) link.aclConnected();
            else if (line.equals("acl down")) link.aclDisconnected();
            else if (line.equals("adapter on")) link.adapter(true);
            else if (line.equals("adapter off")) link.adapter(false);
            else if (line.equals("retry_now")) link.retryNow();
            else if (line.equals("app_opened")) emit("app_opened", "");   // opening the app never clears the breaker
            else if (line.equals("state")) emit("state", "\"breaker\":\"" + link.breakerState() + "\",\"acl\":\"" + link.acl() + "\"");
            else if (line.equals("quit")) break;
            else { emit("bad_command", "\"cmd\":\"" + line.replace('"', '\'') + "\""); continue; }
            emit("cmd", "\"cmd\":\"" + line + "\"");
        }
        quit = true;
        link.stop();
        sensor.join(2000);
        if (tee != null) synchronized (tee) { tee.flush(); tee.close(); }
        emit("quit", "");
    }

    public static void main(String[] a) throws Exception {
        List<String> args = new ArrayList<>(java.util.Arrays.asList(a));
        new PhoneSim(args.toArray(new String[0])).run();
        System.exit(0);
    }
}
