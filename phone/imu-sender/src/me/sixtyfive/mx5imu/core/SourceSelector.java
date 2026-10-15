package me.sixtyfive.mx5imu.core;

/**
 * Chooses which source's windows go to the CMU: the phone gyro or the OBD 0x079 yaw
 * (validation/OBD_YAW_CHANNEL_DESIGN_2026-10-15.md, section 8). One stream carries one source at a
 * time, because the CMU sums dtheta per second regardless of source. Thread-safe.
 *
 * OBD is healthy when at least MIN_FRAMES_2S of its frames arrived in the last 2 s and the newest is
 * at most STALE_MS old. PHONE -> OBD after HOLD_MS of continuous health (hysteresis); OBD -> PHONE as
 * soon as it is unhealthy. With mode PHONE_ONLY the OBD path is never chosen. The first window of a
 * newly chosen source carries frame_reset, so the CMU does not count the switch as a sequence gap.
 */
public final class SourceSelector {
    public enum Source { PHONE, OBD }
    public static final long WINDOW_NS = 2_000_000_000L, STALE_NS = 500_000_000L, HOLD_NS = 3_000_000_000L;
    /** 80 % of the nominal 100 frames in 2 s. */
    public static final int MIN_FRAMES_2S = 80;

    private final boolean obdAllowed;
    private final long[] ring = new long[256];
    private int head, count;
    private long healthySince = -1;
    private Source current = Source.PHONE;
    private boolean resetNext;
    public long switches, phoneWindowsSent, obdWindowsSent, phoneWindowsHeld, obdWindowsHeld;

    public SourceSelector(boolean obdAllowed) { this.obdAllowed = obdAllowed; }

    /** Records the receipt time of one OBD frame. */
    public synchronized void obdFrame(long rxNs) {
        ring[head] = rxNs;
        head = (head + 1) % ring.length;
        if (count < ring.length) count++;
    }

    private int recent(long now) {
        int n = 0;
        for (int i = 1; i <= count; i++) {
            long t = ring[(head - i + ring.length) % ring.length];
            if (now - t > WINDOW_NS) break;
            n++;
        }
        return n;
    }

    public synchronized boolean obdHealthy(long now) {
        if (!obdAllowed || count == 0) return false;
        long newest = ring[(head - 1 + ring.length) % ring.length];
        return now - newest <= STALE_NS && recent(now) >= MIN_FRAMES_2S;
    }

    /** Re-evaluates the source at time now; returns the current one. */
    public synchronized Source update(long now) {
        boolean healthy = obdHealthy(now);
        if (healthy && healthySince < 0) healthySince = now;
        if (!healthy) healthySince = -1;
        Source want = current;
        if (current == Source.PHONE && healthy && now - healthySince >= HOLD_NS) want = Source.OBD;
        if (current == Source.OBD && !healthy) want = Source.PHONE;
        if (want != current) {
            current = want;
            switches++;
            resetNext = true;
        }
        return current;
    }

    public synchronized Source current() { return current; }

    /**
     * Returns the window to send for source `from` at time now, or null when that source is not
     * the chosen one (the window is held back and counted).
     */
    public synchronized Window admit(Source from, Window w, long now) {
        if (update(now) != from) {
            if (from == Source.PHONE) phoneWindowsHeld++; else obdWindowsHeld++;
            return null;
        }
        if (from == Source.PHONE) phoneWindowsSent++; else obdWindowsSent++;
        if (resetNext) {
            resetNext = false;
            return w.withFlags(Protocol.FLAG_FRAME_RESET);
        }
        return w;
    }
}
