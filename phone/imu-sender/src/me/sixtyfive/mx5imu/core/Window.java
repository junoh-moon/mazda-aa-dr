package me.sixtyfive.mx5imu.core;

/** One closed 20 ms integration window, before the sender assigns its sequence number. */
public final class Window {
    public final int flags;
    public final long tEndNs;
    public final int dthetaUrad;
    public final long windowUs;
    public final int biasUradS;
    public final int samples;

    Window(int flags, long tEndNs, int dthetaUrad, long windowUs, int biasUradS, int samples) {
        this.flags = flags;
        this.tEndNs = tEndNs;
        this.dthetaUrad = dthetaUrad;
        this.windowUs = windowUs;
        this.biasUradS = biasUradS;
        this.samples = samples;
    }

    public byte[] encode(long seq) {
        return Protocol.encodeData(flags, seq, tEndNs, dthetaUrad, windowUs, biasUradS);
    }
}
