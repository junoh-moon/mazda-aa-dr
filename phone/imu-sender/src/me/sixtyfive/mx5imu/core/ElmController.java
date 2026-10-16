package me.sixtyfive.mx5imu.core;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.regex.Pattern;

/**
 * Drives one connected ELM327-class adapter as a passive single-id CAN monitor
 * (validation/OBD_YAW_CHANNEL_DESIGN_2026-10-15.md, sections 2 and 7). run() blocks until stop() or
 * a fatal condition (IOException); the caller (ObdLink) reconnects.
 *
 * Init: ATZ, ATDP (logged only: which protocol the adapter kept over the reset), ATE0, ATL0, ATS0,
 * ATH1, ATSP6, ATCAF0, ATCRA &lt;id&gt;, then ATMA. Only AT commands and a single 'X' (which stops a
 * running monitor) are ever written: no OBD request is sent, so the app never asks the adapter to
 * transmit a diagnostic frame on the car's bus (SAFE_COMMAND is enforced).
 *
 * Watchdog: no frame of the id for STALL_MS after frames were flowing, or the prompt that follows
 * BUFFER FULL, restarts the monitor (X, prompt, ATMA). A prompt that arrives within
 * LATE_STOP_FACTOR x stopMs after our own 'X' is the (late, e.g. Bluetooth-delayed) answer to that
 * stop, not a reset: the stall path has already sent its bare ATMA, so the monitor simply goes on.
 * Any sign that the adapter itself reset (a 'LV RESET' or 'ELM327' banner line, an echoed ATMA when
 * the init verified that echo is off, or any other prompt NOT preceded by BUFFER FULL) re-runs the
 * whole init at once, never a bare ATMA: after a reset the adapter is back at its defaults (echo on,
 * no headers, no filter, possibly automatic protocol). A clone that answers OK to ATE0 but keeps
 * echoing is logged once (elm_echo_persists) and its echoed text is plain text. More than MAX_RESTARTS
 * restarts in RESTART_WINDOW_MS, or no frame within NO_TRAFFIC_MS of a monitor start, also re-runs
 * the whole init; MAX_REINITS consecutive re-inits without a frame end the run (the link reconnects
 * with backoff). A normal stop() sends a best-effort 'X' (bounded by STOP_X_MS) when a monitor runs,
 * so the adapter is left at its prompt for the next client.
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
    /** Bound of the best-effort stop character in stop(); entries kept in written() (oldest dropped). */
    public static final long STOP_X_MS = 200;
    public static final int WRITTEN_CAP = 256;
    /** A prompt within LATE_STOP_FACTOR x stopMs after our 'X' answers that 'X' (not an adapter reset). */
    public static final int LATE_STOP_FACTOR = 2;
    /** Extra waits/resends when the ATZ reply is a bare prompt (a late answer to the stop character). */
    public static final int ATZ_BARE_PROMPT_TRIES = 2;

    private final Transport transport;
    private final Clock clock;
    private final Listener listener;
    private final int canId;
    private final Timing timing;
    private final LinkedBlockingQueue<ElmLineParser.Event> queue = new LinkedBlockingQueue<>();
    private final ElmLineParser parser = new ElmLineParser();
    private volatile boolean stopped;
    /** Counted down by stop() once its best-effort 'X' was written or abandoned (run() waits for it). */
    private final CountDownLatch stopXDone = new CountDownLatch(1);
    private volatile IOException readError;
    private OutputStream out;
    private Thread reader;
    private final ArrayDeque<String> written = new ArrayDeque<>();
    /** True while an ATMA we sent may still be running (cleared when its prompt is consumed). */
    private volatile boolean monitorActive;
    private final long[] restartTimes = new long[MAX_RESTARTS + 1];
    private int restartIdx, monitors;
    /**
     * Receipt-clock time of our last stop 'X' whose prompt has not been seen yet (Long.MIN_VALUE: none;
     * cleared by that prompt and by the ATZ answer, which the ordered byte stream delivers after it),
     * and whether ATMA was sent after it. CAN lines do not clear it: frames the adapter sent before it
     * read the 'X' may still arrive ahead of the prompt.
     */
    private volatile long lastStopXNs = Long.MIN_VALUE;
    private volatile boolean atmaSinceStopX;
    private boolean echoPersistsLogged;

    public long canFrames, foreignFrames, bufferFull, errorLines, textLines, malformedLines, restarts, inits, stalls;
    /** Adapter resets seen while monitoring (re-init at once); stray prompts drained before a command. */
    public long adapterResets, strayPrompts, writtenTotal;
    /** Prompts taken as the late answer to our own stop 'X'; ATZ replies that were only a prompt. */
    public long lateStopPrompts, atzBarePrompts;
    /** Whether the last init verified that echo is off (an echoed ATMA then means an adapter reset). */
    public volatile boolean echoVerifiedOff;
    /** The adapter's ATDP answer after the last ATZ (does the clone keep ATSP6 over a reset?). */
    public volatile String protocolAfterReset = "";
    public volatile String version = "";

    public ElmController(Transport transport, Clock clock, Listener listener, int canId, Timing timing) {
        this.transport = transport;
        this.clock = clock;
        this.listener = listener;
        this.canId = canId;
        this.timing = timing;
    }

    /** The last WRITTEN_CAP strings written (tests assert the command set); writtenTotal counts all. */
    public synchronized List<String> written() { return new ArrayList<>(written); }

    /**
     * Stops the controller. When a monitor may be running, one best-effort 'X' is written first so the
     * adapter returns to its prompt (another app can then use it); the write runs on a helper thread
     * and is abandoned after STOP_X_MS, so a stuck Bluetooth write cannot delay the stop.
     */
    public void stop() {
        // stopped is set BEFORE monitorActive is read, and startMonitor() sets monitorActive BEFORE its
        // ATMA write checks stopped (under the write lock): so either that ATMA is refused, or this stop
        // sees the monitor and its 'X' (the only write allowed after stop) goes out after the ATMA.
        boolean wasStopped = stopped;
        stopped = true;
        boolean sendX = !wasStopped && monitorActive && out != null;
        if (sendX) {
            Thread x = new Thread(() -> {
                try { write("X", true); } catch (IOException | RuntimeException ignored) {}
            }, "obd-stop-x");
            x.setDaemon(true);
            x.start();
            try { x.join(STOP_X_MS); } catch (InterruptedException e) { Thread.currentThread().interrupt(); }
        }
        stopXDone.countDown();
        monitorActive = false;
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
            // After stop(), let its 'X' reach the adapter before the transport is closed here (or by
            // ObdLink right after run() returns): bounded by STOP_X_MS plus a small margin.
            if (stopped) {
                try { stopXDone.await(STOP_X_MS + 100, TimeUnit.MILLISECONDS); }
                catch (InterruptedException e) { Thread.currentThread().interrupt(); }
            }
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

    /**
     * The only path to the adapter. After stop() nothing is written except stop()'s own 'X'
     * (allowAfterStop); the stopped check runs under the same lock as the write, so a command racing
     * with stop() is either written before stop()'s 'X' or not at all.
     */
    private synchronized void write(String s, boolean allowAfterStop) throws IOException {
        if (stopped && !allowAfterStop) throw new IOException("stopped");
        written.addLast(s);
        writtenTotal++;
        while (written.size() > WRITTEN_CAP) written.removeFirst();
        out.write(s.getBytes(StandardCharsets.US_ASCII));
        out.flush();
    }

    private void command(String cmd) throws IOException {
        if (!SAFE_COMMAND.matcher(cmd).matches()) throw new IllegalArgumentException("unsafe command " + cmd);
        // After stop() (which may already have sent its 'X') nothing more goes to the adapter, so a
        // restart racing with stop() cannot leave the adapter monitoring again (checked inside write()).
        write(cmd + "\r", false);
    }

    /**
     * Sends cmd and collects the response lines until the prompt; null on timeout. Events already
     * queued before the command cannot be its response: they are dropped first, so a clone that prints
     * two prompts (e.g. after ATZ) does not make the next command take the stray prompt as its answer.
     */
    private List<ElmLineParser.Event> transact(String cmd, long timeoutMs) throws IOException {
        ElmLineParser.Event old;
        while ((old = queue.poll()) != null) {
            if (old.kind == ElmLineParser.Kind.PROMPT) strayPrompts++;
            else if (old.kind == ElmLineParser.Kind.TEXT && "<closed>".equals(old.text) && old.id == -1) {
                queue.add(old);  // keep the reader's end marker for poll()
                break;
            }
        }
        command(cmd);
        return collect(timeoutMs);
    }

    /** Collects response lines until the next prompt without writing anything; null on timeout. */
    private List<ElmLineParser.Event> collect(long timeoutMs) throws IOException {
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

    /** The response text without the echo of the command (echo is still on right after ATZ). */
    private static String textWithout(List<ElmLineParser.Event> r, String echo) {
        List<ElmLineParser.Event> k = new ArrayList<>();
        for (ElmLineParser.Event e : r) if (!echo.equals(e.text)) k.add(e);
        return text(k);
    }

    /**
     * Stops a running monitor: one 'X', then wait for the prompt (absent when the adapter was idle).
     * With deliverFrames (the stall path), frames of the id that the adapter sent before it read the
     * 'X' and that arrive during the wait (Bluetooth delay) are delivered, not dropped; returns how many.
     */
    private int interrupt(boolean deliverFrames) throws IOException {
        write("X", false);
        lastStopXNs = clock.nanos();
        atmaSinceStopX = false;
        int delivered = 0;
        long end = clock.nanos() + timing.stopMs * 1_000_000L;
        while (!stopped && clock.nanos() < end) {
            ElmLineParser.Event e = poll(50);
            if (e == null) continue;
            if (e.kind == ElmLineParser.Kind.PROMPT) { monitorActive = false; lastStopXNs = Long.MIN_VALUE; break; }
            if (deliverFrames && e.kind == ElmLineParser.Kind.CAN && e.id == canId) {
                canFrames++;
                delivered++;
                listener.frame(e);
            }
        }
        return delivered;
    }

    /**
     * Runs the init sequence. A '?' to the first command after an interrupt is retried once (the
     * stop character may have stayed in the adapter's input buffer).
     */
    private void initialise() throws IOException {
        interrupt(false);
        queue.clear();
        List<ElmLineParser.Event> r = transact("ATZ", timing.atzMs);
        if (r != null && has(r, ElmLineParser.Kind.UNKNOWN_COMMAND)) r = transact("ATZ", timing.atzMs);
        // A reply that is only a prompt (no banner or other text line) is not the ATZ answer: it is the
        // late answer to the stop character (Bluetooth delay > stopMs). Wait once more for the real
        // answer; if none comes, send ATZ again. Bounded by ATZ_BARE_PROMPT_TRIES: at most 4 ATZ writes
        // in all (first, the '?' retry, 2 in this loop) and at most 6 x atzMs of waiting.
        for (int i = 0; r != null && !hasReplyText(r, "ATZ") && i < ATZ_BARE_PROMPT_TRIES; i++) {
            atzBarePrompts++;
            listener.event("elm_atz_bare_prompt try=" + (i + 1));
            List<ElmLineParser.Event> more = collect(timing.atzMs);
            r = more != null ? more : transact("ATZ", timing.atzMs);
        }
        if (r == null) throw new IOException("init ATZ: no prompt");
        lastStopXNs = Long.MIN_VALUE;   // the byte stream is ordered: a late stop prompt came before the ATZ answer
        version = text(r);
        listener.event("elm_version " + version);
        // Which protocol the adapter came back with after ATZ (6 kept vs AUTO): logged, never required.
        r = transact("ATDP", timing.cmdMs);
        protocolAfterReset = r == null ? "timeout" : textWithout(r, "ATDP");
        listener.event("elm_protocol_after_reset " + protocolAfterReset);
        String[] cmds = {"ATE0", "ATL0", "ATS0", "ATH1", "ATSP6", "ATCAF0", String.format(Locale.ROOT, "ATCRA %03X", canId)};
        echoVerifiedOff = false;
        boolean echoSeen = false;
        for (String c : cmds) {
            r = transact(c, timing.cmdMs);
            // Echo must be off for every command after ATE0 (ATE0's own reply is still echoed).
            if (!c.equals("ATE0") && (r == null || echoes(r, c))) echoSeen = true;
            boolean ok = r != null && has(r, ElmLineParser.Kind.OK);
            if (!ok && c.equals("ATS0")) {  // spaces are optional: the parser reads both forms
                listener.event("elm_optional_failed " + c + " " + (r == null ? "timeout" : text(r)));
                continue;
            }
            if (!ok) throw new IOException("init " + c + ": " + (r == null ? "timeout" : text(r)));
        }
        echoVerifiedOff = !echoSeen;
        if (echoSeen && !echoPersistsLogged) {
            echoPersistsLogged = true;
            listener.event("elm_echo_persists (an echoed ATMA is plain text, not a reset sign)");
        }
    }

    /**
     * Whether the reply holds a line other than the echo of cmd (with or without a leftover 'X') and
     * other than STOPPED: a late answer to the stop character is [STOPPED, prompt] on adapters that
     * print STOPPED when a monitor is interrupted, and must not be taken as the ATZ answer.
     */
    private static boolean hasReplyText(List<ElmLineParser.Event> r, String cmd) {
        for (ElmLineParser.Event e : r) {
            if (e.kind == ElmLineParser.Kind.STOPPED) continue;
            String t = e.text == null ? "" : e.text.replace(" ", "");
            if (!t.isEmpty() && !t.equals(cmd) && !t.equals("X" + cmd)) return true;
        }
        return false;
    }

    /** Whether the reply holds the echo of cmd (spaces ignored, as the adapter may print them). */
    private static boolean echoes(List<ElmLineParser.Event> r, String cmd) {
        String c = cmd.replace(" ", "");
        for (ElmLineParser.Event e : r) {
            String t = e.text == null ? "" : e.text.replace(" ", "");
            if (t.equals(c) || t.equals("X" + c)) return true;
        }
        return false;
    }

    /** Starts ATMA; '?' (a leftover stop character) is retried once. */
    private void startMonitor(String why) throws IOException {
        // monitorActive goes up BEFORE the ATMA is written, so a stop() landing between the write and
        // this method's end still sends its 'X'; restored when the ATMA was not written.
        boolean before = monitorActive;
        monitorActive = true;
        try {
            command("ATMA");
        } catch (IOException | RuntimeException e) {
            monitorActive = before;
            throw e;
        }
        atmaSinceStopX = true;
        monitors++;
        listener.monitorStarted(monitors, why);
    }

    /** Runs the monitor until a re-init is needed; returns whether any frame of the id arrived. */
    private boolean monitor() throws IOException {
        boolean any = false;
        startMonitor("init");
        long monitorStart = clock.nanos(), lastFrame = 0;
        boolean questionRetried = false, afterBufferFull = false;
        while (!stopped) {
            ElmLineParser.Event e = poll(50);
            long now = clock.nanos();
            if (stopped) break;   // e.g. the prompt that answers stop()'s own 'X'
            if (e != null) {
                switch (e.kind) {
                    case CAN:
                        afterBufferFull = false;
                        if (e.id == canId) {
                            canFrames++;
                            any = true;
                            lastFrame = now;
                            listener.frame(e);
                        } else foreignFrames++;
                        break;
                    case BUFFER_FULL:
                        bufferFull++;
                        afterBufferFull = true;
                        listener.event("elm_buffer_full");
                        break;
                    case UNKNOWN_COMMAND:
                        if (!questionRetried) {
                            questionRetried = true;
                            listener.event("elm_atma_question_retry");
                            if (poll(timing.stopMs) != null) { /* the prompt after '?' */ }
                            monitorActive = false;
                            startMonitor("question");
                            monitorStart = clock.nanos();
                        }
                        break;
                    case PROMPT:
                        if (!afterBufferFull && lateStopPrompt(e.rxNs)) {
                            // The late answer to our own stall 'X'; the stall path already sent its
                            // bare ATMA (settings intact), which this prompt predates: keep monitoring.
                            // The no-traffic watchdog still re-inits if no frame follows.
                            lateStopPrompts++;
                            listener.event("elm_late_stop_prompt ms=" + (e.rxNs - lastStopXNs) / 1_000_000L);
                            lastStopXNs = Long.MIN_VALUE;   // one late prompt per 'X'
                            break;
                        }
                        monitorActive = false;
                        if (!afterBufferFull) {
                            // Not the prompt of BUFFER FULL: the adapter reset or ended the monitor for an
                            // unknown reason, so its settings cannot be trusted. Full init, no bare ATMA.
                            adapterResets++;
                            listener.event("elm_unexpected_prompt reinit");
                            return any;
                        }
                        afterBufferFull = false;
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
                        if (adapterResetLine(e.text, echoVerifiedOff)) {
                            adapterResets++;
                            listener.event("elm_adapter_reset reinit line=" + e.text);
                            return any;
                        }
                        break;
                }
                continue;
            }
            if (lastFrame != 0 && now - lastFrame > timing.stallMs * 1_000_000L) {
                stalls++;
                listener.event("elm_stall ms=" + (now - lastFrame) / 1_000_000L);
                if (interrupt(true) > 0) any = true;
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

    /**
     * A monitor-time text line that only an adapter back at its defaults prints: the low-voltage reset
     * notice, the ATZ/power-on banner, or an echo of ATMA, the last only when our init verified that
     * echo is off (a clone that keeps echoing despite ATE0 echoes every ATMA).
     */
    static boolean adapterResetLine(String text, boolean echoVerifiedOff) {
        String t = text == null ? "" : text.trim().toUpperCase(Locale.ROOT);
        return t.contains("LV RESET") || t.contains("ELM327") || (echoVerifiedOff && t.endsWith("ATMA"));
    }

    /** A prompt received within LATE_STOP_FACTOR x stopMs after our stop 'X', with an ATMA sent since. */
    private boolean lateStopPrompt(long rxNs) {
        long x = lastStopXNs;
        return x != Long.MIN_VALUE && atmaSinceStopX && rxNs >= x
                && rxNs - x <= LATE_STOP_FACTOR * timing.stopMs * 1_000_000L;
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
