package me.sixtyfive.mx5imu.core;

/**
 * Decoder of the MX-5 ND HS-CAN frame 0x079 (DSC/ABS, nominal 50 Hz) yaw candidate
 * (validation/OBD_YAW_CHANNEL_DESIGN_2026-10-15.md, section 4).
 *
 * Layout (byte numbers from 0, bits from the MSB of each byte, big-endian):
 *   raw12   = ((b3 & 3) << 10) | (b4 << 2) | (b5 >> 6)      DBC Motorola start bit 25, length 12
 *   counter = b6 & 0x0F                                       rolling counter (equals 0x078 b6 >> 4)
 *   qf      = b0 >> 6                                         quality bits of the lateral-acceleration field
 * The public DBC's YawRate_Corr is bytes 3..4 only (10 significant bits, 0.223437 deg/s, -114.4); the
 * 12-bit form keeps the two low bits it drops and shares 0x078's integer lattice
 * (validation/YAW_DECODE_2026-10-10, public log; not the owner's car).
 *
 * Units: the CMU's own 0x078 contract is used (src/navigation/pipeline.cpp, README):
 * rad/s = -(raw - 2047) * 0.000658615, i.e. about 26.5 counts per deg/s, CLOCKWISE (heading
 * increasing) POSITIVE; the count rises in left turns (public ND log, corr -0.97 with right steering).
 * The zero is NOT learned here: frames carry the nominal centre 2047 and the CMU logs keep the sum,
 * so the zero is estimated offline at stops (stage 2 is record-only).
 */
public final class Can079 {
    public static final int ID = 0x079;
    public static final int NOMINAL_ZERO = 2047;
    /** Product constant: 0.000658615 rad/s per count = 658.615 urad/s per count. */
    public static final double URAD_S_PER_COUNT = 658.615;
    /** Producer period: 0x079 is a 50 Hz frame (public log: about 49.5 frames/s through a logger with gaps). */
    public static final long PERIOD_US = 20_000L;
    /** Minimum data length that carries the yaw field (bytes 0..5); the counter needs byte 6. */
    public static final int MIN_LEN = 6;

    public enum Status { OK, WRONG_ID, SHORT, RESERVED_BITS, SENTINEL }

    public static final class Sample {
        public final Status status;
        public final int raw12;      // -1 unless decoded
        public final int counter;    // 0..15, or -1 without byte 6
        public final int qf;         // 0..3, or -1
        Sample(Status status, int raw12, int counter, int qf) {
            this.status = status;
            this.raw12 = raw12;
            this.counter = counter;
            this.qf = qf;
        }
        public boolean ok() { return status == Status.OK; }
        /** Clockwise-positive yaw rate at the nominal zero, deg/s. */
        public double cwDegPerS() { return -(raw12 - NOMINAL_ZERO) * URAD_S_PER_COUNT * 1e-6 * 180.0 / Math.PI; }
    }

    private Can079() {}

    /**
     * Decodes one CAN frame. Rejected (not a sample): another id, fewer than 6 data bytes, non-zero
     * upper six bits of byte 3 (always 0 in the public log; the field contract is unknown beyond it),
     * and the all-zeros / all-ones values 0x000 / 0xFFF, treated as possible invalid sentinels
     * (INFERENCE: the public log stays far inside 1737..2925 for the same lattice).
     */
    public static Sample decode(int id, byte[] d) {
        if (id != ID) return new Sample(Status.WRONG_ID, -1, -1, -1);
        if (d == null || d.length < MIN_LEN) return new Sample(Status.SHORT, -1, -1, -1);
        int b0 = d[0] & 0xFF, b3 = d[3] & 0xFF, b4 = d[4] & 0xFF, b5 = d[5] & 0xFF;
        int counter = d.length >= 7 ? (d[6] & 0x0F) : -1;
        int qf = b0 >> 6;
        if ((b3 >> 2) != 0) return new Sample(Status.RESERVED_BITS, -1, counter, qf);
        int raw = ((b3 & 3) << 10) | (b4 << 2) | (b5 >> 6);
        if (raw == 0 || raw == 0xFFF) return new Sample(Status.SENTINEL, raw, counter, qf);
        return new Sample(Status.OK, raw, counter, qf);
    }

    /** The public DBC's coarse reading of the same bytes (deg/s, its own sign), for cross-checks only. */
    public static double dbcYawRateCorr(byte[] d) {
        return (((d[3] & 0xFF) << 8) | (d[4] & 0xFF)) * 0.223437 - 114.4;
    }
}
