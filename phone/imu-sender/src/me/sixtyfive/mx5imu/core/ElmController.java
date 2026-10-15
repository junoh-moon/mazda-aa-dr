package me.sixtyfive.mx5imu.core;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.regex.Pattern;

/**
 * Drives one connected ELM327-class adapter as a passive single-id CAN monitor
 * (validation/OBD_YAW_CHANNEL_DESIGN_2026-10-15.md, sections 2 and 7). run() blocks until stop() or
 * a fatal condition (IOException); the caller (ObdLink) reconnects.
 *
 * Init: ATZ, ATE0, ATL0, ATS0, ATH1, ATSP6, ATCAF0, ATCRA &lt;id&gt;, then ATMA. Only AT commands and a
 * single 'X' (which stops a running monitor) are ever written: no OBD request is sent, so the
 * adapter never transmits a diagnostic frame on the car's bus (SAFE_COMMAND is enforced).
 *
 * Watchdog: no frame of the id for STALL_MS after frames were flowing, or an unexpected prompt
 * (after BUFFER FULL), restarts the monitor (X, prompt, ATMA). More than MAX_RESTARTS restarts in
 * RESTART_WINDOW_MS, or no frame within NO_TRAFFIC_MS of a monitor start, re-runs the whole init;
 * MAX_REINITS consecutive re-inits without a frame end the run (the link reconnects with backoff).
 */
public final class ElmController {
    /** Commands we may write: AT commands only. */
    public static final Pattern SAFE_COMMAND = Pattern.compile("AT[A-Z0-9 ]{1,16}");

    public interface Listener {
        /** One CAN frame of the monitored id (controller thread). */
        void frame(ElmLineParser.Event e);
        /** The monitor (re)started; frames before and after are not contiguous. */
        void monitorStarted(int count, String why);
        /** A diagnostic event for the app log. */
        void event(String line);
    }

    public static final class Timing {
        public final long cmdMs, atzMs, stopMs, stallMs, noTrafficMs, restartWindowMs;
        public Timing(long cmdMs, long atzMs, long stopMs, long stallMs, long noTrafficMs, long restartWindowMs) {
            this.cmdMs = cmdMs; this.atzMs = atzMs; this.stopMs = stopMs; this.stallMs = stallMs;
            this.noTrafficMs = noTrafficMs; this.restartWindowMs = restartWindowMs;
        }
        public static final Timing DEFAULT = new Timing(2000, 5000, 1000, 1000, 10000, 10000);
    }

    public static final int MAX_RESTARTS = 5, MAX_REINITS = 3;

    private final Transport transport;
    private final Clock clock;
    private final Listener listener;
    private final int canId;
    private final Timing timing;
    private final LinkedBlockingQueue<ElmLineParser.Event> queue = new LinkedBlockingQueue<>();
    private final ElmLineParser parser = new ElmLineParser();
    private volatile boolean stopped;
    private volatile IOException readError;
    private OutputStream out;
    private Thread reader;
    private final List<String> written = new ArrayList<>();
    private final long[] restartTimes = new long[MAX_RESTARTS + 1];
    private int restartIdx, monitors;

    public long canFrames, foreignFrames, bufferFull, errorLines, textLines, malformedLines, restarts, inits, stalls;
    public volatile String version = "";

    public ElmController(Transport transport, Clock clock, Listener listener, int canId, Timing timing) {
        this.transport = transport;
        this.clock = clock;
        this.listener = listener;
        this.canId = canId;
        this.timing = timing;
    }

    /** Every string written so far (tests assert the command set). */
    public synchronized List<String> written() { return new ArrayList<>(written); }

    public void stop() {
        stopped = true;
        transport.close();
        if (reader != null) reader.interrupt();
    }

    public void run() throws IOException {
        InputStream in = transport.input();
        out = transport.output();
        reader = new Thread(() -> readLoop(in), "obd-rx");
        reader.setDaemon(true);
        reader.start();
        try {
            int idleReinits = 0;
            while (!stopped) {
                inits++;
                initialise();
                boolean gotFrames = monitor();
                if (stopped) break;
                idleReinits = gotFrames ? 0 : idleReinits + 1;
                if (idleReinits >= MAX_REINITS) throw new IOException("no_traffic after " + idleReinits + " inits");
            }
        } finally {
            transport.close();
        }
    }

    private void readLoop(InputStream in) {
        byte[] buf = new byte[1024];
        try {
            while (!stopped) {
                int n = in.read(buf);
                if (n < 0) { readError = new IOException("eof"); break; }
                long rx = clock.nanos();
                List<ElmLineParser.Event> evs;
                synchronized (parser) { evs = parser.feed(buf, 0, n, rx); }
                queue.addAll(evs);
            }
        } catch (IOException e) {
            readError = e;
        }
        queue.add(new ElmLineParser.Event(ElmLineParser.Kind.TEXT, "<closed>", -1, null, clock.nanos()));
    }

    private ElmLineParser.Event poll(long ms) throws IOException {
        if (stopped) throw new IOException("stopped");
        ElmLineParser.Event e;
        try { e = queue.poll(ms, TimeUnit.MILLISECONDS); }
        catch (InterruptedException x) { Thread.currentThread().interrupt(); throw new IOException("interrupted"); }
        if (e == null && readError != null) throw readError;
        if (e != null && e.kind == ElmLineParser.Kind.TEXT && "<closed>".equals(e.text) && e.id == -1) {
            throw readError != null ? readError : new IOException("closed");
        }
        return e;
    }

    private synchronized void write(String s) throws IOException {
        written.add(s);
        out.write(s.getBytes(StandardCharsets.US_ASCII));
        out.flush();
    }

    private void command(String cmd) throws IOException {
        if (!SAFE_COMMAND.matcher(cmd).matches()) throw new IllegalArgumentException("unsafe command " + cmd);
        write(cmd + "\r");
    }

    /** Sends cmd and collects the response lines until the prompt; null on timeout. */
    private List<ElmLineParser.Event> transact(String cmd, long timeoutMs) throws IOException {
        command(cmd);
        List<ElmLineParser.Event> got = new ArrayList<>();
        long end = clock.nanos() + timeoutMs * 1_000_000L;
        while (!stopped) {
            long left = (end - clock.nanos()) / 1_000_000L;
            if (left <= 0) return null;
            ElmLineParser.Event e = poll(Math.min(left, 100));
            if (e == null) continue;
            if (e.kind == ElmLineParser.Kind.PROMPT) return got;
            if (e.kind == ElmLineParser.Kind.CAN) continue;  // leftovers of an earlier monitor
            got.add(e);
        }
        throw new IOException("stopped");
    }

    private static boolean has(List<ElmLineParser.Event> r, ElmLineParser.Kind k) {
        for (ElmLineParser.Event e : r) if (e.kind == k) return true;
        return false;
    }

    private static String text(List<ElmLineParser.Event> r) {
        StringBuilder b = new StringBuilder();
        for (ElmLineParser.Event e : r) b.append(b.length() == 0 ? "" : "|").append(e.text);
        return b.toString();
    }

    /** Stops a running monitor: one 'X', then wait for the prompt (absent when the adapter was idle). */
    private void interrupt() throws IOException {
        write("X");
        long end = clock.nanos() + timing.stopMs * 1_000_000L;
        while (!stopped && clock.nanos() < end) {
            ElmLineParser.Event e = poll(50);
            if (e != null && e.kind == ElmLineParser.Kind.PROMPT) return;
        }
    }

    /**
     * Runs the init sequence. A '?' to the first command after an interrupt is retried once (the
     * stop character may have stayed in the adapter's input buffer).
     */
    private void initialise() throws IOException {
        interrupt();
        queue.clear();
        List<ElmLineParser.Event> r = transact("ATZ", timing.atzMs);
        if (r != null && has(r, ElmLineParser.Kind.UNKNOWN_COMMAND)) r = transact("ATZ", timing.atzMs);
        if (r == null) throw new IOException("init ATZ: no prompt");
        version = text(r);
        listener.event("elm_version " + version);
        String[] cmds = {"ATE0", "ATL0", "ATS0", "ATH1", "ATSP6", "ATCAF0", String.format(Locale.ROOT, "ATCRA %03X", canId)};
        for (String c : cmds) {
            r = transact(c, timing.cmdMs);
            boolean ok = r != null && has(r, ElmLineParser.Kind.OK);
            if (!ok && c.equals("ATS0")) {  // spaces are optional: the parser reads both forms
                listener.event("elm_optional_failed " + c + " " + (r == null ? "timeout" : text(r)));
                continue;
            }
            if (!ok) throw new IOException("init " + c + ": " + (r == null ? "timeout" : text(r)));
        }
    }

    /** Starts ATMA; '?' (a leftover stop character) is retried once. */
    private void startMonitor(String why) throws IOException {
        command("ATMA");
        monitors++;
        listener.monitorStarted(monitors, why);
    }

    /** Runs the monitor until a re-init is needed; returns whether any frame of the id arrived. */
    private boolean monitor() throws IOException {
        boolean any = false;
        startMonitor("init");
        long monitorStart = clock.nanos(), lastFrame = 0;
        boolean questionRetried = false;
        while (!stopped) {
            ElmLineParser.Event e = poll(50);
            long now = clock.nanos();
            if (e != null) {
                switch (e.kind) {
                    case CAN:
                        if (e.id == canId) {
                            canFrames++;
                            any = true;
                            lastFrame = now;
                            listener.frame(e);
                        } else foreignFrames++;
                        break;
                    case BUFFER_FULL:
                        bufferFull++;
                        listener.event("elm_buffer_full");
                        break;
                    case UNKNOWN_COMMAND:
                        if (!questionRetried) {
                            questionRetried = true;
                            listener.event("elm_atma_question_retry");
                            if (poll(timing.stopMs) != null) { /* the prompt after '?' */ }
                            startMonitor("question");
                            monitorStart = clock.nanos();
                        }
                        break;
                    case PROMPT:
                        // The monitor ended by itself (BUFFER FULL or an adapter reset).
                        if (!restart("prompt")) return any;
                        monitorStart = clock.nanos();
                        lastFrame = 0;
                        break;
                    case ERROR:
                        errorLines++;
                        break;
                    case MALFORMED:
                        malformedLines++;
                        break;
                    default:
                        textLines++;
                        if (textLines <= 20) listener.event("elm_text " + e.text);
                        break;
                }
                continue;
            }
            if (lastFrame != 0 && now - lastFrame > timing.stallMs * 1_000_000L) {
                stalls++;
                listener.event("elm_stall ms=" + (now - lastFrame) / 1_000_000L);
                interrupt();
                if (!restart("stall")) return any;
                monitorStart = clock.nanos();
                lastFrame = 0;
            } else if (lastFrame == 0 && now - monitorStart > timing.noTrafficMs * 1_000_000L) {
                listener.event("elm_no_traffic ms=" + (now - monitorStart) / 1_000_000L);
                return any;
            }
        }
        return any;
    }

    /** Restarts ATMA unless restarts are too frequent (then false: the caller re-inits). */
    private boolean restart(String why) throws IOException {
        restarts++;
        long now = clock.nanos();
        restartTimes[restartIdx % restartTimes.length] = now;
        restartIdx++;
        if (restartIdx > MAX_RESTARTS) {
            long oldest = restartTimes[restartIdx % restartTimes.length];
            if (now - oldest < timing.restartWindowMs * 1_000_000L) {
                listener.event("elm_restart_storm reinit");
                restartIdx = 0;
                return false;
            }
        }
        startMonitor(why);
        return true;
    }
}
