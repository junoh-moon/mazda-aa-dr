package me.sixtyfive.mx5imu.core;

import java.util.ArrayList;
import java.util.List;

/**
 * Stream decoder: resynchronises on the magic bytes and the CRC. A candidate whose CRC fails is
 * counted and the scan moves on by one byte; a CRC-valid candidate with another version is counted
 * and dropped whole; a CRC-valid MP frame with a type other than 0/1 is counted as bad_type.
 */
public final class FrameParser {
    private byte[] buf = new byte[256];
    private int len;
    public long frames, crcErrors, unknownVersion, badType, skippedBytes;

    public List<Frame> feed(byte[] data, int off, int n) {
        if (len + n > buf.length) {
            byte[] nb = new byte[Math.max(buf.length * 2, len + n)];
            System.arraycopy(buf, 0, nb, 0, len);
            buf = nb;
        }
        System.arraycopy(data, off, buf, len, n);
        len += n;
        List<Frame> out = new ArrayList<>();
        int pos = 0;
        while (len - pos >= 2) {
            int m0 = buf[pos] & 0xFF, m1 = buf[pos + 1] & 0xFF;
            int need;
            if (m0 == Protocol.MAGIC0 && m1 == Protocol.DATA_MAGIC1) need = Protocol.DATA_LEN;
            else if (m0 == Protocol.MAGIC0 && m1 == Protocol.PING_MAGIC1) need = Protocol.PING_LEN;
            else { pos++; skippedBytes++; continue; }
            if (len - pos < need) break;
            long want = Protocol.getU32(buf, pos + need - 4);
            if (Protocol.crc32(buf, pos, need - 4) != want) { crcErrors++; pos++; skippedBytes++; continue; }
            if ((buf[pos + 2] & 0xFF) != Protocol.VERSION) { unknownVersion++; pos += need; continue; }
            Frame f = decodeChecked(buf, pos, need);
            if (f == null) { badType++; pos += need; continue; }
            frames++;
            out.add(f);
            pos += need;
        }
        System.arraycopy(buf, pos, buf, 0, len - pos);
        len -= pos;
        return out;
    }

    /** Pending undecoded bytes (an incomplete frame or a lone magic byte). */
    public int pending() { return len; }

    private static Frame decodeChecked(byte[] b, int p, int need) {
        int flags = b[p + 3] & 0xFF;
        long seq = Protocol.getU32(b, p + 4);
        if (need == Protocol.DATA_LEN) {
            return new Frame(Frame.Kind.DATA, flags, seq, Protocol.getI64(b, p + 8), (int) Protocol.getU32(b, p + 16),
                    Protocol.getU32(b, p + 20), (int) Protocol.getU32(b, p + 24), 0, 0, Protocol.getU32(b, p + 28));
        }
        Frame.Kind k;
        if (flags == Protocol.TYPE_PING) k = Frame.Kind.PING;
        else if (flags == Protocol.TYPE_PONG) k = Frame.Kind.PONG;
        else return null;
        return new Frame(k, flags, seq, 0, 0, 0, 0, Protocol.getI64(b, p + 8), Protocol.getI64(b, p + 16),
                Protocol.getU32(b, p + 24));
    }
}
