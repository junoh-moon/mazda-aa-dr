package me.sixtyfive.mx5imu.core;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/** Minimal JSON reader for the test vectors (objects, arrays, strings, integers, decimals, true/false/null). */
final class MiniJson {
    private final String s;
    private int p;

    private MiniJson(String s) { this.s = s; }

    static Object parse(String text) {
        MiniJson j = new MiniJson(text);
        Object v = j.value();
        j.ws();
        if (j.p != text.length()) throw new IllegalArgumentException("trailing data at " + j.p);
        return v;
    }

    private void ws() { while (p < s.length() && Character.isWhitespace(s.charAt(p))) p++; }

    private Object value() {
        ws();
        char c = s.charAt(p);
        if (c == '{') {
            p++;
            Map<String, Object> m = new LinkedHashMap<>();
            ws();
            if (s.charAt(p) == '}') { p++; return m; }
            while (true) {
                ws();
                String k = string();
                ws();
                expect(':');
                m.put(k, value());
                ws();
                if (s.charAt(p) == ',') { p++; continue; }
                expect('}');
                return m;
            }
        }
        if (c == '[') {
            p++;
            List<Object> l = new ArrayList<>();
            ws();
            if (s.charAt(p) == ']') { p++; return l; }
            while (true) {
                l.add(value());
                ws();
                if (s.charAt(p) == ',') { p++; continue; }
                expect(']');
                return l;
            }
        }
        if (c == '"') return string();
        if (s.startsWith("true", p)) { p += 4; return Boolean.TRUE; }
        if (s.startsWith("false", p)) { p += 5; return Boolean.FALSE; }
        if (s.startsWith("null", p)) { p += 4; return null; }
        int start = p;
        if (s.charAt(p) == '-') p++;
        while (p < s.length() && Character.isDigit(s.charAt(p))) p++;
        if (p < s.length() && ".eE".indexOf(s.charAt(p)) >= 0) {
            // Decimals (the OBD decode vectors) are Doubles; integers stay Longs.
            while (p < s.length() && "0123456789.eE+-".indexOf(s.charAt(p)) >= 0) p++;
            return Double.parseDouble(s.substring(start, p));
        }
        return Long.parseLong(s.substring(start, p));
    }

    private void expect(char c) {
        if (s.charAt(p) != c) throw new IllegalArgumentException("expected " + c + " at " + p);
        p++;
    }

    private String string() {
        expect('"');
        StringBuilder b = new StringBuilder();
        while (true) {
            char c = s.charAt(p++);
            if (c == '"') return b.toString();
            if (c != '\\') { b.append(c); continue; }
            char e = s.charAt(p++);
            switch (e) {
                case 'n': b.append('\n'); break;
                case 't': b.append('\t'); break;
                case 'u': b.append((char) Integer.parseInt(s.substring(p, p + 4), 16)); p += 4; break;
                default: b.append(e);
            }
        }
    }
}
