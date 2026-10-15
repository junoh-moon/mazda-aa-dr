package me.sixtyfive.mx5imu.core;

import java.util.zip.CRC32;

/** PHONE IMU CHANNEL v1 frame layout (little-endian, CRC-32 IEEE over all preceding bytes). */
public final class Protocol {
    public static final String SERVICE_UUID = "fae57779-63aa-4424-bf71-66ce5951b0be";
    public static final int VERSION = 1;
    public static final int MAGIC0 = 0x4D;
    public static final int DATA_MAGIC1 = 0x58; // 'MX'
    public static final int PING_MAGIC1 = 0x50; // 'MP'
    public static final int DATA_LEN = 32;
    public static final int PING_LEN = 28;
    public static final int FLAG_BIAS_VALID = 1;
    public static final int FLAG_STATIONARY = 2;
    public static final int FLAG_GRAVITY_VALID = 4;
    public static final int FLAG_FRAME_RESET = 8;
    public static final int FLAG_MASK = 15;
    public static final int TYPE_PING = 0;
    public static final int TYPE_PONG = 1;

    private Protocol() {}

    public static long crc32(byte[] b, int off, int len) {
        CRC32 c = new CRC32();
        c.update(b, off, len);
        return c.getValue();
    }

    static void putU32(byte[] b, int off, long v) {
        for (int i = 0; i < 4; i++) b[off + i] = (byte) (v >>> (8 * i));
    }

    static void putI64(byte[] b, int off, long v) {
        for (int i = 0; i < 8; i++) b[off + i] = (byte) (v >>> (8 * i));
    }

    static long getU32(byte[] b, int off) {
        long v = 0;
        for (int i = 3; i >= 0; i--) v = (v << 8) | (b[off + i] & 0xFFL);
        return v;
    }

    static long getI64(byte[] b, int off) {
        long v = 0;
        for (int i = 7; i >= 0; i--) v = (v << 8) | (b[off + i] & 0xFFL);
        return v;
    }

    /** Encodes one DATA frame. seq and windowUs are taken modulo 2^32; flags must use only bits 0..3. */
    public static byte[] encodeData(int flags, long seq, long tEndNs, int dthetaUrad, long windowUs, int biasUradS) {
        if ((flags & ~FLAG_MASK) != 0) throw new IllegalArgumentException("reserved flag bits");
        byte[] b = new byte[DATA_LEN];
        b[0] = (byte) MAGIC0;
        b[1] = (byte) DATA_MAGIC1;
        b[2] = (byte) VERSION;
        b[3] = (byte) flags;
        putU32(b, 4, seq);
        putI64(b, 8, tEndNs);
        putU32(b, 16, dthetaUrad);
        putU32(b, 20, windowUs);
        putU32(b, 24, biasUradS);
        putU32(b, 28, crc32(b, 0, 28));
        return b;
    }

    public static byte[] encodePingPong(int type, long seq, long tCmuNs, long tPhoneNs) {
        byte[] b = new byte[PING_LEN];
        b[0] = (byte) MAGIC0;
        b[1] = (byte) PING_MAGIC1;
        b[2] = (byte) VERSION;
        b[3] = (byte) type;
        putU32(b, 4, seq);
        putI64(b, 8, tCmuNs);
        putI64(b, 16, tPhoneNs);
        putU32(b, 24, crc32(b, 0, 24));
        return b;
    }

    /** The pong copies the ping's seq and t_cmu_ns unchanged and adds the phone reception time. */
    public static byte[] pongFor(Frame ping, long tPhoneNs) {
        if (ping.kind != Frame.Kind.PING) throw new IllegalArgumentException("not a ping");
        return encodePingPong(TYPE_PONG, ping.seq, ping.tCmuNs, tPhoneNs);
    }
}
