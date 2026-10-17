package me.sixtyfive.mx5imu.core;

import java.util.ArrayDeque;

/**
 * RECORD-ONLY A/B comparison of the phone gyro and the OBD 0x079 yaw (validation/OBD_YAW_CHANNEL_DESIGN_2026-10-15.md,
 * section 16). The CMU only ever receives the source SourceSelector chose, so the two sources never exist side by
 * side anywhere else. This accumulator sees every window of BOTH producers where the selector sees them
 * (admit(), under the selector's lock, before anything is sent) and sums them per interval of at least INTERVAL_NS
 * of monotonic time. It never changes, delays or drops a window: admit() returns exactly what
 * SourceSelector.admit returns.
 *
 * One row per closed interval in which at least one producer delivered a window, for the app's separate A/B ring
 * (RollingLog.ab). Both sides present:
 *   ab iv_ms=I sel=S g_n=N g_dur_us=D g_dth_urad=T g_grav=0|1 g_st=K o_n=N o_dur_us=D o_dth_urad=T o_gap=0|1
 * One side missing: that side's fields are left out and miss=obd or miss=gyro is appended.
 *   iv_ms       interval length (ms, monotonic); the RollingLog prefix t=...ms is the interval end
 *   sel         which source the selector SENT in the interval: PHONE, OBD, MIX (both, a switch) or NONE
 *   g_n, o_n    windows of each producer (gyro: closed 20 ms integration windows; OBD: one per CAN frame)
 *   *_dur_us    sum of window_us (gyro: sensor time covered; OBD: 20000 per frame = covered producer time)
 *   *_dth_urad  sum of dtheta_urad, the wire value, CLOCKWISE POSITIVE seen from above (heading increasing) on
 *               both sides: gyro = sum (omega . (-gravity/|gravity|)) dt (Integrator); OBD = -(raw12 - 2047) *
 *               658.615 urad/s * 0.020 s (ObdIntegrator, nominal zero 2047, not zero-corrected)
 *   g_grav      1 when every gyro window had gravity_valid
 *   g_st        gyro windows with the phone's stationary hint (largest rate < 0.005 rad/s)
 *   o_gap       1 when any OBD window had obd_gap (frames lost before it) or frame_reset (monitor restart)
 * Rows hold no Bluetooth address, no location and no sample: only these interval sums.
 *
 * Rate: an interval closes only in poll() and only after INTERVAL_NS since it opened, the next one opens at that
 * close, so rows are at least INTERVAL_NS apart: at most 360 per hour. Rows wait in a queue of MAX_PENDING for
 * the caller to write (the main thread; no file I/O on the sensor or OBD thread); a full queue drops the new row
 * and counts it. Thread-safe: add() runs on the sensor and OBD threads, poll()/take() on the main thread.
 */
public final class AbCompare {
    public static final long INTERVAL_NS = 10_000_000_000L;
    public static final int MAX_PENDING = 8;

    private boolean enabled, open;
    private long startNs, lastRowNs = Long.MIN_VALUE;
    private long gN, gSt, gDurUs, gDth;
    private boolean gGravAll;
    private long oN, oDurUs, oDth;
    private boolean oGap;
    private long phoneSent, obdSent;
    private final ArrayDeque<String> pending = new ArrayDeque<>();
    public long rows, oneSided, emptyIntervals, resets, dropped;

    /**
     * The selector's decision for one window, plus the record-only accumulation (ab may be null = off). The
     * caller holds the selector's lock, as for SourceSelector.admit. Returns exactly sel.admit(from, w, now).
     */
    public static Window admit(SourceSelector sel, AbCompare ab, SourceSelector.Source from, Window w, long now) {
        Window x = sel.admit(from, w, now);
        if (ab != null) ab.add(from, w, x != null, now);
        return x;
    }

    /** On only while an OBD adapter is configured; turning it off drops the open interval. */
    public synchronized void setEnabled(boolean on) {
        enabled = on;
        if (!on) clearInterval();
    }

    public synchronized boolean enabled() { return enabled; }

    /** Adds one window of a producer (as produced, before the selector's flags); sent = the selector passed it. */
    public synchronized void add(SourceSelector.Source from, Window w, boolean sent, long now) {
        if (!enabled) return;
        if (!open) {
            open = true;
            startNs = now;
        }
        if (from == SourceSelector.Source.PHONE) {
            if (gN == 0) gGravAll = true;
            gN++;
            gDurUs += w.windowUs;
            gDth += w.dthetaUrad;
            if ((w.flags & Protocol.FLAG_GRAVITY_VALID) == 0) gGravAll = false;
            if ((w.flags & Protocol.FLAG_STATIONARY) != 0) gSt++;
            if (sent) phoneSent++;
        } else {
            oN++;
            oDurUs += w.windowUs;
            oDth += w.dthetaUrad;
            if ((w.flags & (Protocol.FLAG_OBD_GAP | Protocol.FLAG_FRAME_RESET)) != 0) oGap = true;
            if (sent) obdSent++;
        }
    }

    /** Closes the interval when at least INTERVAL_NS have passed since it opened; queues its row. */
    public synchronized void poll(long now) {
        if (!enabled || !open || now - startNs < INTERVAL_NS) return;
        if (lastRowNs != Long.MIN_VALUE && now - lastRowNs < INTERVAL_NS) return;  // defensive: never above the rate
        if (gN == 0 && oN == 0) {
            emptyIntervals++;
            clearInterval();
            return;
        }
        String row = format(now - startNs);
        if (pending.size() >= MAX_PENDING) dropped++;
        else pending.add(row);
        rows++;
        if (gN == 0 || oN == 0) oneSided++;
        lastRowNs = now;
        clearInterval();
        open = true;     // the next interval starts at this close
        startNs = now;
    }

    /** The next queued row, or null. */
    public synchronized String take() { return pending.poll(); }

    /** Session change or source restart of the whole service: the open interval is dropped (no row). */
    public synchronized void reset() {
        resets++;
        clearInterval();
    }

    private void clearInterval() {
        open = false;
        gN = gSt = gDurUs = gDth = 0;
        gGravAll = false;
        oN = oDurUs = oDth = 0;
        oGap = false;
        phoneSent = obdSent = 0;
    }

    private String format(long ivNs) {
        StringBuilder b = new StringBuilder(160);
        String sel = phoneSent > 0 && obdSent > 0 ? "MIX" : phoneSent > 0 ? "PHONE" : obdSent > 0 ? "OBD" : "NONE";
        b.append("ab iv_ms=").append(ivNs / 1_000_000L).append(" sel=").append(sel);
        if (gN > 0) {
            b.append(" g_n=").append(gN).append(" g_dur_us=").append(gDurUs).append(" g_dth_urad=").append(gDth)
                    .append(" g_grav=").append(gGravAll ? 1 : 0).append(" g_st=").append(gSt);
        }
        if (oN > 0) {
            b.append(" o_n=").append(oN).append(" o_dur_us=").append(oDurUs).append(" o_dth_urad=").append(oDth)
                    .append(" o_gap=").append(oGap ? 1 : 0);
        }
        if (oN == 0) b.append(" miss=obd");
        else if (gN == 0) b.append(" miss=gyro");
        return b.toString();
    }

    /** Counters for the periodic obd_stats line. */
    public synchronized String summary() {
        return "ab_rows=" + rows + " ab_one_sided=" + oneSided + " ab_dropped=" + dropped + " ab_resets=" + resets;
    }
}
