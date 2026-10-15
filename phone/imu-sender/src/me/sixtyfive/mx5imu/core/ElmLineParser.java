package me.sixtyfive.mx5imu.core;

import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * Splits the byte stream of an ELM327-class adapter into lines and prompts, and classifies each
 * line (validation/OBD_YAW_CHANNEL_DESIGN_2026-10-15.md, section 3). Single-threaded.
 *
 * Lines end at CR or LF (ATL0 gives CR only; LF is accepted for ATL1). '>' is the prompt: the
 * adapter waits for a command. NUL bytes (some clones pad with them) are ignored. A line longer
 * than MAX_LINE is discarded and counted as an overflow. CAN lines are recognised in every format
 * the init sequence can leave behind: ATH1 with spaces off ("07900112233445566") or on
 * ("079 00 11 22 ..."), and with ATD1's DLC digit ("079 8 00 11 ..."). Hex is case-insensitive.
 */
public final class ElmLineParser {
    public static final int MAX_LINE = 96;

    public enum Kind {
        /** An 11-bit CAN frame: id and data bytes are set. */
        CAN,
        /** The '>' prompt. */
        PROMPT,
        OK,
        /** "?": the adapter did not understand the command. */
        UNKNOWN_COMMAND,
        /** "BUFFER FULL": the adapter stopped monitoring because its buffer overflowed (frames lost). */
        BUFFER_FULL,
        /** "STOPPED": monitoring or a request was interrupted by our input. */
        STOPPED,
        /** A frame line with "<RX ERROR" / "<DATA ERROR", or CAN ERROR, BUS ERROR, DATA ERROR, FB ERROR. */
        ERROR,
        /** NO DATA, SEARCHING..., the version banner, LV RESET, ACT ALERT, echoed commands and any other text. */
        TEXT,
        /** A line that looks like hex but is not a well-formed 11-bit CAN line, or an over-long line. */
        MALFORMED
    }

    public static final class Event {
        public final Kind kind;
        public final String text;   // the trimmed line (upper case), "" for PROMPT
        public final int id;        // CAN only, else -1
        public final byte[] data;   // CAN only, else null
        public final long rxNs;     // receipt time of the chunk that completed the line

        Event(Kind kind, String text, int id, byte[] data, long rxNs) {
            this.kind = kind;
            this.text = text;
            this.id = id;
            this.data = data;
            this.rxNs = rxNs;
        }
    }

    private final StringBuilder line = new StringBuilder();
    private boolean overflow;
    public long overflows, nulBytes;

    /** Feeds bytes received at rxNs; returns the completed events in order. */
    public List<Event> feed(byte[] b, int off, int len, long rxNs) {
        List<Event> out = new ArrayList<>();
        for (int i = off; i < off + len; i++) {
            int c = b[i] & 0xFF;
            if (c == 0) { nulBytes++; continue; }
            if (c == '\r' || c == '\n') { finish(out, rxNs); continue; }
            if (c == '>') { finish(out, rxNs); out.add(new Event(Kind.PROMPT, "", -1, null, rxNs)); continue; }
            if (line.length() >= MAX_LINE) { overflow = true; continue; }
            line.append((char) c);
        }
        return out;
    }

    private void finish(List<Event> out, long rxNs) {
        if (overflow) {
            overflows++;
            overflow = false;
            line.setLength(0);
            out.add(new Event(Kind.MALFORMED, "", -1, null, rxNs));
            return;
        }
        String t = line.toString().trim().toUpperCase(Locale.ROOT);
        line.setLength(0);
        if (t.isEmpty()) return;
        out.add(classify(t, rxNs));
    }

    /** Classifies one trimmed, upper-case line. */
    public static Event classify(String t, long rxNs) {
        if (t.equals("OK")) return new Event(Kind.OK, t, -1, null, rxNs);
        if (t.equals("?")) return new Event(Kind.UNKNOWN_COMMAND, t, -1, null, rxNs);
        if (t.equals("BUFFER FULL")) return new Event(Kind.BUFFER_FULL, t, -1, null, rxNs);
        if (t.equals("STOPPED")) return new Event(Kind.STOPPED, t, -1, null, rxNs);
        if (t.contains("<RX ERROR") || t.contains("<DATA ERROR") || t.equals("CAN ERROR") || t.equals("BUS ERROR")
                || t.equals("DATA ERROR") || t.equals("FB ERROR") || t.equals("RX ERROR")) {
            return new Event(Kind.ERROR, t, -1, null, rxNs);
        }
        String h = t.replace(" ", "");
        if (!isHex(h)) return new Event(Kind.TEXT, t, -1, null, rxNs);
        // An 11-bit header is three hex digits; the rest is data, optionally preceded by ATD1's DLC digit.
        if (h.length() < 3) return new Event(Kind.MALFORMED, t, -1, null, rxNs);
        int id = Integer.parseInt(h.substring(0, 3), 16);
        String rest = h.substring(3);
        if (id > 0x7FF) return new Event(Kind.MALFORMED, t, -1, null, rxNs);
        if (rest.length() % 2 == 1) {
            int dlc = Character.digit(rest.charAt(0), 16);
            if (dlc > 8 || rest.length() - 1 != 2 * dlc) return new Event(Kind.MALFORMED, t, -1, null, rxNs);
            rest = rest.substring(1);
        }
        if (rest.length() > 16) return new Event(Kind.MALFORMED, t, -1, null, rxNs);
        byte[] data = new byte[rest.length() / 2];
        for (int i = 0; i < data.length; i++) data[i] = (byte) Integer.parseInt(rest.substring(2 * i, 2 * i + 2), 16);
        return new Event(Kind.CAN, t, id, data, rxNs);
    }

    private static boolean isHex(String s) {
        if (s.isEmpty()) return false;
        for (int i = 0; i < s.length(); i++) if (Character.digit(s.charAt(i), 16) < 0) return false;
        return true;
    }

    /** Pending characters of an unfinished line. */
    public int pending() { return line.length(); }
}
