package me.sixtyfive.mx5imu.core;

/**
 * Who may hold the single-client OBD adapter: the sender's per-session link or the parked check, never
 * both (validation/OBD_YAW_CHANNEL_DESIGN_2026-10-15.md, section 13). The monitor of this object is
 * the lock for every check-then-set. When a CMU session starts while the check runs, the session's
 * start is deferred, not dropped: endCheck() runs it (outside the lock) as soon as the check ends,
 * unless the session ended first (cancelDeferred).
 */
public final class ObdArbiter {
    private boolean serviceRunning, checkRunning;
    private Runnable deferred;
    public long deferredStarts;

    /** The parked check may start only when neither it nor the sender's link holds the adapter. */
    public synchronized boolean tryStartCheck() {
        if (serviceRunning || checkRunning) return false;
        checkRunning = true;
        return true;
    }

    /**
     * The sender's link wants the adapter: true = granted now (serviceRunning set); false = the check
     * holds it, and whenCheckEnds will run once when the check ends (replacing an older deferred start).
     */
    public synchronized boolean startServiceOrDefer(Runnable whenCheckEnds) {
        if (checkRunning) {
            deferred = whenCheckEnds;
            deferredStarts++;
            return false;
        }
        deferred = null;
        serviceRunning = true;
        return true;
    }

    /** Ends the check and runs a deferred service start, if any (on the caller's thread, outside the lock). */
    public void endCheck() {
        Runnable r;
        synchronized (this) {
            checkRunning = false;
            r = deferred;
            deferred = null;
        }
        if (r != null) r.run();
    }

    /** The session that asked for a deferred start ended: drop it. */
    public synchronized void cancelDeferred() { deferred = null; }

    public synchronized void serviceStopped() { serviceRunning = false; }

    public synchronized boolean serviceRunning() { return serviceRunning; }

    public synchronized boolean checkRunning() { return checkRunning; }

    public synchronized boolean hasDeferred() { return deferred != null; }
}
