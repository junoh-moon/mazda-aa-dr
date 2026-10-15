package me.sixtyfive.mx5imu.core;

/**
 * When to try the next RFCOMM connection (pure logic, times in ms, not thread-safe).
 *
 * Backoff 2 s doubling to 30 s. It returns to 2 s only after a HEALTHY session (PINGs received for
 * at least {@link Breaker#healthyMs}), never merely because a connect succeeded or the ACL came up.
 * While the device's ACL link is known to be down nothing is attempted. With an unknown ACL state
 * (service start) the policy tries with backoff and, after UNKNOWN_GIVE_UP consecutive failures,
 * waits for ACL_CONNECTED instead of paging an absent car forever.
 *
 * Circuit breaker (validation/PHONE_IMU_CHANNEL_2026-10-14.md): a FAILURE is a connect that fails,
 * a session that ends without any PING, or a session that ends or whose ACL link drops within
 * {@link Breaker#earlyAclDropMs} of the RFCOMM connect (what a bds crash on our service looks like
 * from the phone). After {@link Breaker#tripFailures} consecutive failures nothing is attempted for
 * {@link Breaker#openMs}; the second trip stops all attempts until {@link #resetBreaker} (the app is
 * opened again). ACL_CONNECTED gives an immediate attempt only with no failure pending and the
 * breaker closed.
 */
public final class ReconnectPolicy {
    public enum Acl { UNKNOWN, UP, DOWN }
    public enum BreakerState { CLOSED, OPEN, LOCKED }

    /** Breaker thresholds (the defaults are the product values; tests shorten them). */
    public static final class Breaker {
        public final int tripFailures;
        public final long openMs, healthyMs, earlyAclDropMs;
        public Breaker(int tripFailures, long openMs, long healthyMs, long earlyAclDropMs) {
            this.tripFailures = tripFailures;
            this.openMs = openMs;
            this.healthyMs = healthyMs;
            this.earlyAclDropMs = earlyAclDropMs;
        }
        public static final Breaker DEFAULT = new Breaker(3, 60L * 60_000L, 60_000L, 30_000L);
    }

    public static final int UNKNOWN_GIVE_UP = 5;
    private final long minMs, maxMs;
    private final Breaker breaker;
    private boolean enabled, adapterOn = true, connected, locked;
    private Acl acl = Acl.UNKNOWN;
    private long backoffMs, nextAtMs, openUntilMs, connectAtMs = -1, aclDownAtMs = -1;
    private int unknownFailures, failures, trips;

    public ReconnectPolicy(long minMs, long maxMs) { this(minMs, maxMs, Breaker.DEFAULT); }

    public ReconnectPolicy(long minMs, long maxMs, Breaker breaker) {
        this.minMs = minMs;
        this.maxMs = maxMs;
        this.breaker = breaker;
        this.backoffMs = minMs;
    }

    /** Enabling (service start) or opening the app clears the breaker. */
    public void setEnabled(boolean on, long now) {
        enabled = on;
        if (on) { backoffMs = minMs; nextAtMs = now; unknownFailures = 0; resetBreaker(); }
    }

    public void resetBreaker() { failures = 0; trips = 0; locked = false; openUntilMs = 0; }

    public void adapter(boolean on, long now) {
        adapterOn = on;
        if (!on) acl = Acl.DOWN;
        else { acl = Acl.UNKNOWN; unknownFailures = 0; nextAtMs = Math.max(nextAtMs, now); }
    }

    public void aclConnected(long now) {
        acl = Acl.UP;
        if (failures == 0 && state(now) == BreakerState.CLOSED) nextAtMs = now;
    }

    public void aclDisconnected(long now) {
        acl = Acl.DOWN;
        aclDownAtMs = now;
    }

    public void connectFailed(long now) {
        if (acl == Acl.UNKNOWN && ++unknownFailures >= UNKNOWN_GIVE_UP) acl = Acl.DOWN;
        failure(now);
        scheduleBackoff(now);
    }

    public void connected(long now) {
        connected = true;
        connectAtMs = now;
        unknownFailures = 0;
        if (acl == Acl.UNKNOWN) acl = Acl.UP;
    }

    /**
     * A session ended. pinged: at least one valid PING was received; pingSpanMs: last minus first PING.
     */
    public void sessionEnded(long now, boolean pinged, long pingSpanMs) {
        connected = false;
        // The ACL broadcast can arrive after the socket error that ended the session, so a session
        // that itself ended within the window counts as well.
        boolean early = connectAtMs >= 0 && (now - connectAtMs <= breaker.earlyAclDropMs ||
                (aclDownAtMs >= connectAtMs && aclDownAtMs - connectAtMs <= breaker.earlyAclDropMs));
        if (!pinged || early) failure(now);
        else if (pingSpanMs >= breaker.healthyMs) { failures = 0; backoffMs = minMs; }
        scheduleBackoff(now);
    }

    private void failure(long now) {
        if (++failures < breaker.tripFailures) return;
        failures = 0;
        if (++trips >= 2) locked = true;
        else openUntilMs = now + breaker.openMs;
    }

    private void scheduleBackoff(long now) {
        nextAtMs = Math.max(now + backoffMs, openUntilMs);
        backoffMs = Math.min(backoffMs * 2, maxMs);
    }

    public BreakerState state(long now) {
        if (locked) return BreakerState.LOCKED;
        return now < openUntilMs ? BreakerState.OPEN : BreakerState.CLOSED;
    }

    public boolean shouldAttempt(long now) {
        return eligible(now) && now >= nextAtMs;
    }

    /** Milliseconds until the next attempt, or -1 when waiting for an event (ACL, adapter, enable, app). */
    public long waitMs(long now) {
        if (!enabled || !adapterOn || connected || locked || acl == Acl.DOWN) return -1;
        return Math.max(0, Math.max(nextAtMs, openUntilMs) - now);
    }

    private boolean eligible(long now) {
        return enabled && adapterOn && !connected && acl != Acl.DOWN && state(now) == BreakerState.CLOSED;
    }

    public Acl acl() { return acl; }
    public long currentBackoffMs() { return backoffMs; }
    public int consecutiveFailures() { return failures; }
    public int trips() { return trips; }
    public long openUntilMs() { return openUntilMs; }
}
