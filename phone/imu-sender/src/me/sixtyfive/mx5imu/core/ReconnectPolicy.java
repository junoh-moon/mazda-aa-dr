package me.sixtyfive.mx5imu.core;

/**
 * When to try the next RFCOMM connection (pure logic, times in ms of elapsedRealtime, not thread-safe).
 *
 * Backoff 2 s doubling to 30 s. It returns to 2 s only after a HEALTHY session (PINGs received for
 * at least {@link Breaker#healthyMs}), never merely because a connect succeeded or the ACL came up.
 * While the device's ACL link is known to be down nothing is attempted. With an unknown ACL state
 * (service start) the policy tries with backoff and, after UNKNOWN_GIVE_UP consecutive failures,
 * waits for ACL_CONNECTED instead of paging an absent car forever.
 *
 * Circuit breaker (validation/PHONE_IMU_CHANNEL_2026-10-14.md section 3a). A FAILURE needs the car
 * to be present: a connect that fails while the car's ACL link is UP, or a session after a
 * successful RFCOMM connect that ends without any PING (the session waits 15 s for the first one).
 * A session that received a PING is never a failure; connect failures with the car absent or
 * unknown only back off. After {@link Breaker#tripFailures} consecutive failures nothing is attempted
 * for {@link Breaker#openMs}; the second trip LOCKS until {@link #retryNow} (the app's 'retry now'
 * button), which sets the trip count to 1 so the next trip locks again (3 more failures per press). Opening the app, a service restart or
 * a phone reboot never clears it: the state is persisted ({@link #snapshot}/{@link #restore}). The
 * trip count is forgotten only after a healthy session (PINGs over {@link Breaker#healthyMs}) at
 * least {@link #TRIP_MEMORY_MS} after the last trip. The backoff returns to the minimum only after
 * a healthy session. ACL_CONNECTED gives an immediate attempt only with no failure pending and the
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
    public static final long TRIP_MEMORY_MS = 24L * 3600_000L;
    static final String RECORD = "pimu-breaker-v1";
    private final long minMs, maxMs;
    private final Breaker breaker;
    private boolean enabled, adapterOn = true, connected, locked;
    private Acl acl = Acl.UNKNOWN;
    private long backoffMs, nextAtMs, openUntilMs, connectAtMs = -1, aclDownAtMs = -1, lastTripMs = Long.MIN_VALUE / 4;
    private int unknownFailures, failures, trips, changes;

    public ReconnectPolicy(long minMs, long maxMs) { this(minMs, maxMs, Breaker.DEFAULT); }

    public ReconnectPolicy(long minMs, long maxMs, Breaker breaker) {
        this.minMs = minMs;
        this.maxMs = maxMs;
        this.breaker = breaker;
        this.backoffMs = minMs;
    }

    /** Enabling (service start) resets the backoff only; the breaker state is kept. */
    public void setEnabled(boolean on, long now) {
        enabled = on;
        if (on) { backoffMs = minMs; nextAtMs = Math.max(now, openUntilMs); unknownFailures = 0; }
    }

    /** The app's explicit 'retry now': clears OPEN/LOCKED, the next trip locks again. */
    public void retryNow(long now) {
        failures = 0;
        locked = false;
        openUntilMs = 0;
        trips = 1;                                   // the next trip (-> 2) locks
        backoffMs = minMs;
        nextAtMs = now;
        changes++;
    }

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
        if (acl == Acl.UP) failure(now);                 // the car is there: our service failed
        else if (acl == Acl.UNKNOWN && ++unknownFailures >= UNKNOWN_GIVE_UP) acl = Acl.DOWN;
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
        // A session that received a PING is never a failure, whatever ended it: the CMU was alive and
        // answering (ACL flapping with a healthy CMU must not lock, interop rig F4). A bds crash loop
        // is stopped by the CMU quiesce, after which no PING comes and the no-PING sessions count.
        if (!pinged) failure(now);
        else if (pingSpanMs >= breaker.healthyMs) {
            if (failures != 0) changes++;
            failures = 0;
            backoffMs = minMs;
            if (trips > 0 && now - lastTripMs >= TRIP_MEMORY_MS) { trips = 0; changes++; }
        }
        scheduleBackoff(now);
    }

    private void failure(long now) {
        changes++;
        if (++failures < breaker.tripFailures) return;
        failures = 0;
        lastTripMs = now;
        trips = Math.min(trips + 1, 2);              // reachable: 0, 1, 2 (= locked)
        if (trips == 2) locked = true;
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

    /** Milliseconds until the next attempt, or -1 when waiting for an event (ACL, adapter, enable, retry). */
    public long waitMs(long now) {
        if (!enabled || !adapterOn || connected || locked || acl == Acl.DOWN) return -1;
        return Math.max(0, Math.max(nextAtMs, openUntilMs) - now);
    }

    private boolean eligible(long now) {
        return enabled && adapterOn && !connected && acl != Acl.DOWN && state(now) == BreakerState.CLOSED;
    }

    /** Counter of breaker-state changes (failures, trips, resets) for persisting. */
    public int changes() { return changes; }

    /**
     * Persistable breaker state. Times are stored both as elapsedRealtime and as wall time with the
     * phone's boot count, so a phone reboot (elapsedRealtime restarts) keeps the remaining time.
     */
    public String snapshot(long now, long wallMs, long bootCount) {
        return RECORD + " " + failures + " " + trips + " " + (locked ? 1 : 0) + " " + openUntilMs + " " +
                (openUntilMs > now ? wallMs + (openUntilMs - now) : 0) + " " + lastTripMs + " " +
                (wallMs - (now - lastTripMs)) + " " + bootCount;
    }

    /** Restores a snapshot; an unreadable record keeps one trip (the next trip locks). */
    public void restore(String record, long now, long wallMs, long bootCount) {
        if (record == null || record.isEmpty()) return;
        String[] f = record.trim().split(" ");
        try {
            if (f.length != 9 || !RECORD.equals(f[0])) throw new NumberFormatException(record);
            int fl = Integer.parseInt(f[1]), tr = Integer.parseInt(f[2]);
            boolean lk = "1".equals(f[3]);
            long openEl = Long.parseLong(f[4]), openWall = Long.parseLong(f[5]);
            long tripEl = Long.parseLong(f[6]), tripWall = Long.parseLong(f[7]), boot = Long.parseLong(f[8]);
            // Reachable states only: locked exactly when trips == 2.
            if (fl < 0 || tr < 0 || fl >= breaker.tripFailures || tr > 2 || lk != (tr == 2)) throw new NumberFormatException(record);
            failures = fl;
            trips = tr;
            locked = lk;
            if (boot == bootCount && bootCount >= 0) {
                openUntilMs = Math.min(openEl, now + breaker.openMs);
                lastTripMs = tripEl;
            } else {                                   // another phone boot: use the wall clock
                long left = openWall == 0 ? 0 : Math.max(0, Math.min(breaker.openMs, openWall - wallMs));
                openUntilMs = left > 0 ? now + left : 0;
                lastTripMs = now - Math.max(0, Math.min(TRIP_MEMORY_MS, wallMs - tripWall));
            }
        } catch (NumberFormatException e) {          // unreadable: one trip kept, the next trip locks
            failures = 0;
            trips = 1;
            locked = false;
            openUntilMs = 0;
        }
        nextAtMs = Math.max(nextAtMs, openUntilMs);
    }

    public Acl acl() { return acl; }
    public long currentBackoffMs() { return backoffMs; }
    public int consecutiveFailures() { return failures; }
    public int trips() { return trips; }
    public long openUntilMs() { return openUntilMs; }
}
