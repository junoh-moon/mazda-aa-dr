package me.sixtyfive.mx5imu.core;

import java.io.IOException;
import java.util.concurrent.atomic.AtomicLong;

/**
 * Connection thread: waits for the ReconnectPolicy, opens the transport, runs one Session until it
 * closes, then reports the disconnection (and any circuit-breaker change) and waits again. Events
 * (ACL, adapter, stop, app reopened) wake it.
 */
public final class Link {
    public interface Listener {
        void connected(Session s);
        void disconnected(Session s, String reason);
        void connectFailed(String error, long nextWaitMs, ReconnectPolicy.Acl acl);
        /** The circuit breaker opened (OPEN, until untilMs on the link clock) or locked (LOCKED). */
        default void breaker(ReconnectPolicy.BreakerState state, long untilMs) {}
    }

    /** Where the breaker state survives the process (the service's Prefs); tests may keep it in memory. */
    public interface Store {
        String load();
        void save(String record);
        long wallMs();
        long bootCount();
    }

    private final Transport.Connector connector;
    private final Listener listener;
    private final Clock clock;
    private final ReconnectPolicy policy;
    private final AtomicLong seq = new AtomicLong();
    private final long pollMs;
    private Store store;
    private int saved = -1;
    private Thread thread;
    private volatile boolean running;
    private volatile Session session;

    public Link(Transport.Connector connector, Listener listener, Clock clock, long minBackoffMs, long maxBackoffMs, long pollMs) {
        this(connector, listener, clock, minBackoffMs, maxBackoffMs, pollMs, ReconnectPolicy.Breaker.DEFAULT);
    }

    public Link(Transport.Connector connector, Listener listener, Clock clock, long minBackoffMs, long maxBackoffMs, long pollMs,
                ReconnectPolicy.Breaker breaker) {
        this.connector = connector;
        this.listener = listener;
        this.clock = clock;
        this.policy = new ReconnectPolicy(minBackoffMs, maxBackoffMs, breaker);
        this.pollMs = pollMs;
    }

    private long nowMs() { return clock.nanos() / 1_000_000L; }

    /** Before start(): restore and later persist the breaker through the store. */
    public synchronized void setStore(Store s) {
        store = s;
        if (s != null) { policy.restore(s.load(), nowMs(), s.wallMs(), s.bootCount()); saved = policy.changes(); }
    }

    private void persist() {  // caller holds the lock
        if (store == null || saved == policy.changes()) return;
        saved = policy.changes();
        store.save(policy.snapshot(nowMs(), store.wallMs(), store.bootCount()));
    }

    public synchronized void start() {
        if (running) return;
        running = true;
        policy.setEnabled(true, nowMs());
        thread = new Thread(this::loop, "imu-link");
        thread.start();
    }

    public void stop() {
        Thread t;
        synchronized (this) {
            if (!running) return;
            running = false;
            policy.setEnabled(false, nowMs());
            t = thread;
            notifyAll();
        }
        connector.abort();
        Session s = session;
        if (s != null) s.close("stopped");
        try { t.join(5000); } catch (InterruptedException e) { Thread.currentThread().interrupt(); }
    }

    public synchronized void aclConnected() { policy.aclConnected(nowMs()); notifyAll(); }
    public synchronized void aclDisconnected() { policy.aclDisconnected(nowMs()); notifyAll(); }
    /** The app's explicit 'retry now' button: clears an OPEN or LOCKED breaker (one more trip locks). */
    public synchronized void retryNow() { policy.retryNow(nowMs()); persist(); notifyAll(); }
    public synchronized ReconnectPolicy.BreakerState breakerState() { return policy.state(nowMs()); }
    public synchronized long openUntilMs() { return policy.openUntilMs(); }
    public synchronized void adapter(boolean on) {
        policy.adapter(on, nowMs());
        notifyAll();
        if (!on) { Session s = session; if (s != null) s.close("adapter_off"); }
    }

    public Session session() { return session; }
    public AtomicLong seq() { return seq; }
    public synchronized ReconnectPolicy.Acl acl() { return policy.acl(); }

    private void loop() {
        while (true) {
            synchronized (this) {
                while (running && !policy.shouldAttempt(nowMs())) {
                    long w = policy.waitMs(nowMs());
                    try { wait(w < 0 ? 0 : Math.max(1, w)); } catch (InterruptedException e) { return; }
                }
                if (!running) return;
            }
            Transport t;
            try {
                t = connector.open();
            } catch (IOException | RuntimeException e) {
                long wait;
                ReconnectPolicy.Acl acl;
                ReconnectPolicy.BreakerState before, after;
                long until;
                synchronized (this) {
                    before = policy.state(nowMs());
                    policy.connectFailed(nowMs());
                    persist();
                    wait = policy.waitMs(nowMs()); acl = policy.acl();
                    after = policy.state(nowMs()); until = policy.openUntilMs();
                }
                listener.connectFailed(e.getClass().getSimpleName() + ": " + e.getMessage(), wait, acl);
                if (after != before) listener.breaker(after, until);
                continue;
            }
            Session s = new Session(t, clock, seq);
            synchronized (this) {
                if (!running) { t.close(); return; }
                policy.connected(nowMs());
                session = s;
            }
            try { s.start(); } catch (IOException e) { s.close("stream_error"); }
            if (s.closeReason() == null) listener.connected(s);
            try {
                while (!s.checkSilence()) s.awaitClosed(pollMs);
            } catch (InterruptedException e) {
                s.close("interrupted");
            }
            try { s.join(2000); } catch (InterruptedException e) { Thread.currentThread().interrupt(); }
            ReconnectPolicy.BreakerState before, after;
            long until;
            synchronized (this) {
                session = null;
                before = policy.state(nowMs());
                policy.sessionEnded(nowMs(), s.pinged(), s.pingSpanMs());
                persist();
                after = policy.state(nowMs()); until = policy.openUntilMs();
            }
            listener.disconnected(s, s.closeReason());
            if (after != before) listener.breaker(after, until);
        }
    }
}
