package me.sixtyfive.mx5imu.core;

/**
 * Maps CAN 0x079 yaw samples onto PHONE IMU CHANNEL v1 DATA windows without changing the wire format
 * (validation/OBD_YAW_CHANNEL_DESIGN_2026-10-15.md, section 6). One received CAN frame = one window:
 *   flags       FLAG_SOURCE_OBD, plus FLAG_OBD_GAP when frames were lost just before it, plus
 *               FLAG_FRAME_RESET on the first window after a (re)start; bits 0..2 always 0
 *   t_end_ns    phone elapsedRealtimeNanos when the adapter's line arrived (receipt time, never a
 *               producer time)
 *   dtheta_urad -(raw12 - 2047) * 658.615 urad/s * 0.020 s, clockwise positive, rounded with the
 *               rounding error carried to the next window (the running sum stays within 0.5 urad)
 *   window_us   20000: the nominal producer period this sample covers (not the receipt spacing), so
 *               the CMU's per-second sum of window_us is the covered time and shows loss directly
 *   bias        0 (the zero is estimated offline at stops)
 * Single-threaded: call from the OBD thread.
 */
public final class ObdIntegrator {
    private double carry;
    private boolean resetPending = true;
    public long windows, rejected;

    /** The next window starts a new stream (adapter or link restart). */
    public void reset() {
        carry = 0;
        resetPending = true;
    }

    /** Returns the window of one decoded sample, or null when the sample is not usable. */
    public Window add(Can079.Sample s, long rxNs, long lostBefore) {
        if (!s.ok()) { rejected++; return null; }
        double exact = -(s.raw12 - Can079.NOMINAL_ZERO) * Can079.URAD_S_PER_COUNT * (Can079.PERIOD_US * 1e-6) + carry;
        long rounded = Math.round(exact);
        carry = exact - rounded;
        int flags = Protocol.FLAG_SOURCE_OBD;
        if (lostBefore > 0) flags |= Protocol.FLAG_OBD_GAP;
        if (resetPending) flags |= Protocol.FLAG_FRAME_RESET;
        resetPending = false;
        windows++;
        return new Window(flags, rxNs, Integrator.clampToInt(rounded), Can079.PERIOD_US, 0, 1);
    }
}
