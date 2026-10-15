package me.sixtyfive.mx5imu.core;

import java.io.IOException;
import java.util.Locale;

/**
 * OBD yaw source thread (stage 2, RECORD-ONLY; validation/OBD_YAW_CHANNEL_DESIGN_2026-10-15.md):
 * opens the adapter, runs ElmController as a 0x079 monitor, decodes each frame (Can079), measures
 * loss (ObdLossMeter) and turns samples into v1 windows (ObdIntegrator) for the sink. Reconnects with
 * backoff MIN..MAX ms (doubling; reset after a run that delivered frames). Nothing is ever written
 * to the adapter except ElmController's AT commands.
 */
public final class ObdLink {
    public interface Sink {
        /** One OBD window (OBD thread). */
        void window(Window w);
        /** A diagnostic line for the app log. */
        void event(String line);
    }

    private final Transport.Connector connector;
    private final Clock clock;
    private final Sink sink;
    private final ElmController.Timing timing;
    private final long minBackoffMs, maxBackoffMs;
    private final ObdLossMeter meter = new ObdLossMeter(Can079.PERIOD_US);
    private final ObdIntegrator integrator = new ObdIntegrator();
    private volatile boolean running;
    private volatile ElmController current;
    private Thread thread;
    private volatile String state = "꺼짐";
    public volatile long connects, connectFailures, runs;
    private final long[] decodeStatus = new long[Can079.Status.values().length];
    private long qfNot3;

    public ObdLink(Transport.Connector connector, Clock clock, Sink sink, ElmController.Timing timing, long minBackoffMs, long maxBackoffMs) {
        this.connector = connector;
        this.clock = clock;
        this.sink = sink;
        this.timing = timing;
        this.minBackoffMs = minBackoffMs;
        this.maxBackoffMs = maxBackoffMs;
    }

    public synchronized void start() {
        if (running) return;
        running = true;
        thread = new Thread(this::loop, "obd-link");
        thread.start();
    }

    public void stop() {
        Thread t;
        synchronized (this) {
            running = false;
            t = thread;
        }
        connector.abort();
        ElmController c = current;
        if (c != null) c.stop();
        if (t != null) {
            t.interrupt();
            try { t.join(3000); } catch (InterruptedException e) { Thread.currentThread().interrupt(); }
        }
        state = "꺼짐";
    }

    public String state() { return state; }

    private void loop() {
        long backoff = minBackoffMs;
        while (running) {
            state = "OBD 연결 중";
            Transport t;
            try {
                t = connector.open();
            } catch (IOException e) {
                connectFailures++;
                sink.event("obd_connect_failed next_ms=" + backoff + " " + e.getMessage());
                state = "OBD 재시도 대기 " + backoff / 1000 + "초";
                if (!sleep(backoff)) break;
                backoff = Math.min(maxBackoffMs, backoff * 2);
                continue;
            }
            connects++;
            runs++;
            long framesBefore;
            synchronized (this) { framesBefore = meter.frames; }
            ElmController c = new ElmController(t, clock, new ElmController.Listener() {
                @Override public void frame(ElmLineParser.Event e) { onFrame(e); }
                @Override public void monitorStarted(int count, String why) {
                    synchronized (ObdLink.this) { meter.breakChain(); integrator.reset(); }
                    state = "OBD 수신 중";
                    sink.event("obd_monitor_start n=" + count + " why=" + why);
                }
                @Override public void event(String line) { sink.event(line); }
            }, Can079.ID, timing);
            current = c;
            String why = "stopped";
            try {
                state = "OBD 초기화 중";
                c.run();
            } catch (IOException | RuntimeException e) {
                why = String.valueOf(e.getMessage());
            } finally {
                current = null;
                t.close();
            }
            long got;
            synchronized (this) { got = meter.frames - framesBefore; }
            sink.event(String.format(Locale.ROOT, "obd_run_end why=%s frames=%d elm=%s can=%d foreign=%d buffer_full=%d restarts=%d inits=%d stalls=%d err=%d text=%d malformed=%d | %s",
                    why, got, c.version, c.canFrames, c.foreignFrames, c.bufferFull, c.restarts, c.inits, c.stalls, c.errorLines,
                    c.textLines, c.malformedLines, summary()));
            if (!running) break;
            if (got > 0) backoff = minBackoffMs;
            state = "OBD 재시도 대기 " + backoff / 1000 + "초";
            if (!sleep(backoff)) break;
            if (got == 0) backoff = Math.min(maxBackoffMs, backoff * 2);
        }
        state = "꺼짐";
    }

    private void onFrame(ElmLineParser.Event e) {
        Window w;
        synchronized (this) {
            Can079.Sample s = Can079.decode(e.id, e.data);
            decodeStatus[s.status.ordinal()]++;
            if (s.qf >= 0 && s.qf != 3) qfNot3++;
            if (!s.ok()) { integrator.rejected++; return; }
            long lost = meter.add(s.counter, e.rxNs);
            w = integrator.add(s, e.rxNs, meter.lastAmbiguous ? Math.max(1, lost) : lost);
        }
        if (w != null) sink.window(w);
    }

    private boolean sleep(long ms) {
        try { Thread.sleep(ms); return running; }
        catch (InterruptedException e) { Thread.currentThread().interrupt(); return false; }
    }

    /** Loss meter and decode counters, one line. */
    public synchronized String summary() {
        StringBuilder b = new StringBuilder(meter.summary());
        b.append(" decode");
        for (Can079.Status s : Can079.Status.values()) b.append(' ').append(s.name().toLowerCase(Locale.ROOT)).append('=').append(decodeStatus[s.ordinal()]);
        b.append(" qf_not3=").append(qfNot3).append(" windows=").append(integrator.windows);
        return b.toString();
    }

    public synchronized double fps() { return meter.fps(); }
    public synchronized double lossPercent() { return meter.lossPercent(); }
    public synchronized long frames() { return meter.frames; }
    public synchronized String verdict() { return meter.verdict(); }
}
