package me.sixtyfive.mx5imu.core;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.List;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicLong;

/**
 * One connected RFCOMM session. A reader thread answers every valid PING at once with a PONG; a
 * writer thread sends queued DATA frames in order. Only DATA and PONG frames are ever written, and
 * no DATA frame before the first PING of the session (windows offered earlier are discarded and
 * counted): a CMU that does not receive us never gets data. The session closes on an I/O error, when
 * no PING arrives within 15 s of the connect ("no_ping"), on 10 s without a valid PING after that
 * ("silence") or when the transmit queue is full (the CMU stopped reading).
 */
public final class Session {
    public static final long SILENCE_NS = 10_000_000_000L;
    public static final long FIRST_PING_NS = 15_000_000_000L;
    public static final int QUEUE_FRAMES = 500; // 10 s at 50 Hz

    private final Transport transport;
    private final Clock clock;
    private final AtomicLong seq;
    private final Object outLock = new Object();
    private final ArrayBlockingQueue<Window> queue = new ArrayBlockingQueue<>(QUEUE_FRAMES);
    private final CountDownLatch closed = new CountDownLatch(1);
    private final FrameParser parser = new FrameParser();
    private final long startNs;
    private volatile String closeReason;
    private volatile long lastPingNs, firstPingNs;
    private Thread reader, writer;
    private InputStream in;
    private OutputStream out;

    public final AtomicLong framesSent = new AtomicLong(), pongsSent = new AtomicLong(), pingsReceived = new AtomicLong(),
            unexpectedFrames = new AtomicLong(), droppedBeforePing = new AtomicLong();

    public Session(Transport transport, Clock clock, AtomicLong seq) {
        this.transport = transport;
        this.clock = clock;
        this.seq = seq;
        this.startNs = clock.nanos();
        this.lastPingNs = startNs;
    }

    public void start() throws IOException {
        in = transport.input();
        out = transport.output();
        reader = new Thread(this::readLoop, "imu-rx");
        writer = new Thread(this::writeLoop, "imu-tx");
        reader.start();
        writer.start();
    }

    /** Queues a window for sending; returns false (and closes the session) when the queue is full. */
    public boolean offer(Window w) {
        if (closeReason != null) return false;
        if (firstPingNs == 0) { droppedBeforePing.incrementAndGet(); return true; }
        if (queue.offer(w)) return true;
        close("tx_stall");
        return false;
    }

    /** Closes without a first PING in 15 s or on 10 s without a valid PING; true if (now) closed. */
    public boolean checkSilence() {
        long now = clock.nanos();
        if (closeReason == null && firstPingNs == 0 && now - startNs >= FIRST_PING_NS) close("no_ping");
        if (closeReason == null && firstPingNs != 0 && now - lastPingNs >= SILENCE_NS) close("silence");
        return closeReason != null;
    }

    public void close(String reason) {
        synchronized (this) {
            if (closeReason != null) return;
            closeReason = reason;
        }
        transport.close();
        if (writer != null) writer.interrupt();
        closed.countDown();
    }

    public boolean awaitClosed(long ms) throws InterruptedException { return closed.await(ms, TimeUnit.MILLISECONDS); }
    public String closeReason() { return closeReason; }
    public long lastPingNs() { return lastPingNs; }
    /** Elapsed-realtime ns of the first valid PING, 0 when none arrived. */
    public long firstPingNs() { return firstPingNs; }
    public boolean pinged() { return firstPingNs != 0; }
    /** ms between the first and the last PING (0 without PINGs). */
    public long pingSpanMs() { return firstPingNs == 0 ? 0 : (lastPingNs - firstPingNs) / 1_000_000L; }
    public long crcErrors() { synchronized (parser) { return parser.crcErrors; } }
    public long unknownVersion() { synchronized (parser) { return parser.unknownVersion; } }

    /** Joins the session threads after close (bounded). */
    public void join(long ms) throws InterruptedException {
        if (reader != null) reader.join(ms);
        if (writer != null) writer.join(ms);
    }

    private void readLoop() {
        byte[] buf = new byte[512];
        try {
            while (closeReason == null) {
                int n = in.read(buf);
                if (n < 0) { close("eof"); return; }
                long rxNs = clock.nanos();
                List<Frame> frames;
                synchronized (parser) { frames = parser.feed(buf, 0, n); }
                for (Frame f : frames) {
                    if (f.kind != Frame.Kind.PING) { unexpectedFrames.incrementAndGet(); continue; }
                    lastPingNs = rxNs;
                    if (firstPingNs == 0) firstPingNs = rxNs == 0 ? 1 : rxNs;
                    pingsReceived.incrementAndGet();
                    byte[] pong = Protocol.pongFor(f, rxNs);
                    synchronized (outLock) { out.write(pong); out.flush(); }
                    pongsSent.incrementAndGet();
                }
            }
        } catch (IOException e) {
            close("read_error");
        }
    }

    private void writeLoop() {
        try {
            while (closeReason == null) {
                Window w = queue.poll(250, TimeUnit.MILLISECONDS);
                if (w == null) continue;
                byte[] frame = w.encode(seq.get() & 0xFFFFFFFFL);
                synchronized (outLock) { out.write(frame); out.flush(); }
                seq.set((seq.get() + 1) & 0xFFFFFFFFL);
                framesSent.incrementAndGet();
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        } catch (IOException e) {
            close("write_error");
        }
    }
}
