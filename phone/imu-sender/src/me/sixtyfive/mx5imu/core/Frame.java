package me.sixtyfive.mx5imu.core;

/** One decoded v1 frame. Unused fields are zero for the other kind. */
public final class Frame {
    public enum Kind { DATA, PING, PONG }

    public final Kind kind;
    public final int flags;
    public final long seq;          // u32
    public final long tEndNs;       // DATA
    public final int dthetaUrad;    // DATA
    public final long windowUs;     // DATA, u32
    public final int biasUradS;     // DATA
    public final long tCmuNs;       // PING/PONG
    public final long tPhoneNs;     // PING/PONG
    public final long crc;          // u32

    Frame(Kind kind, int flags, long seq, long tEndNs, int dthetaUrad, long windowUs, int biasUradS,
            long tCmuNs, long tPhoneNs, long crc) {
        this.kind = kind;
        this.flags = flags;
        this.seq = seq;
        this.tEndNs = tEndNs;
        this.dthetaUrad = dthetaUrad;
        this.windowUs = windowUs;
        this.biasUradS = biasUradS;
        this.tCmuNs = tCmuNs;
        this.tPhoneNs = tPhoneNs;
        this.crc = crc;
    }
}
