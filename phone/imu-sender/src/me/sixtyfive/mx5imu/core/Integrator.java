package me.sixtyfive.mx5imu.core;

/**
 * Integrates calibrated gyro samples about the downward gravity axis into windows of at least 20 ms
 * of sensor time (spec v1). Single-threaded: call every method from the sensor thread.
 *
 * Sign: Android's TYPE_GRAVITY reports the reaction vector (about +9.81 on z when the phone lies
 * face up), i.e. it points UP. The downward unit vector is therefore -gravity/|gravity|. With the
 * right-handed phone axes, a clockwise turn seen from above has its angular velocity pointing down,
 * so omega . down is positive: clockwise (heading increasing) is positive, as the spec requires.
 */
public final class Integrator {
    public static final long WINDOW_NS = 20_000_000L;
    /** A sensor-time gap longer than this is not integrated across; the next window restarts (frame_reset). */
    public static final long GAP_NS = 100_000_000L;
    /** The OS bias estimate is treated as valid for this long after its last uncalibrated-gyro event. */
    public static final long BIAS_FRESH_NS = 1_000_000_000L;
    /** Gravity magnitudes outside this range (m/s^2) are rejected as invalid. */
    public static final double GRAVITY_MIN = 4.0, GRAVITY_MAX = 15.0;
    /** Stationary hint: the largest calibrated rate magnitude in the window is below this (rad/s). */
    public static final double STATIONARY_RAD_S = 0.005;

    private boolean havePrev;
    private long prevT, winStart;
    private double sumRad, maxRate;
    private int samples;
    private boolean gravityThroughout;
    private boolean resetPending = true;
    private final double[] down = new double[3];
    private boolean downValid;
    private final double[] bias = new double[3];
    private long biasT;
    private boolean haveBias;
    public long ignoredSamples, gaps;

    /** Drop the partial window and mark the next emitted window with frame_reset. */
    public void reset() {
        havePrev = false;
        sumRad = 0;
        samples = 0;
        maxRate = 0;
        resetPending = true;
    }

    public void gravity(double gx, double gy, double gz) {
        double n = Math.sqrt(gx * gx + gy * gy + gz * gz);
        if (!(n >= GRAVITY_MIN && n <= GRAVITY_MAX)) { downValid = false; return; }
        down[0] = -gx / n;
        down[1] = -gy / n;
        down[2] = -gz / n;
        downValid = true;
    }

    /** OS bias estimate from TYPE_GYROSCOPE_UNCALIBRATED values[3..5] (rad/s, phone axes). */
    public void bias(long tNs, double bx, double by, double bz) {
        bias[0] = bx;
        bias[1] = by;
        bias[2] = bz;
        biasT = tNs;
        haveBias = true;
    }

    /** Adds one calibrated gyro sample; returns a closed window or null. */
    public Window gyro(long tNs, double wx, double wy, double wz) {
        if (!havePrev) { start(tNs); return null; }
        if (tNs <= prevT) { ignoredSamples++; return null; }
        if (tNs - prevT > GAP_NS) {
            gaps++;
            Window w = samples > 0 ? close() : null;
            reset();
            start(tNs);
            return w;
        }
        double dt = (tNs - prevT) * 1e-9;
        if (downValid) sumRad += (wx * down[0] + wy * down[1] + wz * down[2]) * dt;
        else gravityThroughout = false;
        double rate = Math.sqrt(wx * wx + wy * wy + wz * wz);
        if (rate > maxRate) maxRate = rate;
        samples++;
        prevT = tNs;
        if (tNs - winStart >= WINDOW_NS) return close();
        return null;
    }

    private void start(long tNs) {
        havePrev = true;
        prevT = tNs;
        winStart = tNs;
        sumRad = 0;
        samples = 0;
        maxRate = 0;
        gravityThroughout = downValid;
    }

    private Window close() {
        int flags = 0;
        int biasU = 0;
        if (haveBias && downValid && prevT - biasT <= BIAS_FRESH_NS && biasT - prevT <= BIAS_FRESH_NS) {
            flags |= Protocol.FLAG_BIAS_VALID;
            biasU = clampToInt(Math.round((bias[0] * down[0] + bias[1] * down[1] + bias[2] * down[2]) * 1e6));
        }
        if (maxRate < STATIONARY_RAD_S) flags |= Protocol.FLAG_STATIONARY;
        if (gravityThroughout && downValid) flags |= Protocol.FLAG_GRAVITY_VALID;
        if (resetPending) flags |= Protocol.FLAG_FRAME_RESET;
        resetPending = false;
        Window w = new Window(flags, prevT, clampToInt(Math.round(sumRad * 1e6)), (prevT - winStart) / 1000L, biasU, samples);
        winStart = prevT;
        sumRad = 0;
        samples = 0;
        maxRate = 0;
        gravityThroughout = downValid;
        return w;
    }

    static int clampToInt(long v) {
        return v > Integer.MAX_VALUE ? Integer.MAX_VALUE : v < Integer.MIN_VALUE ? Integer.MIN_VALUE : (int) v;
    }
}
