package me.sixtyfive.mx5imu.core;

import java.util.Locale;

/**
 * Self-measured frame loss of the OBD path (adapter + Bluetooth) for one CAN id with a nominal
 * period (validation/OBD_YAW_CHANNEL_DESIGN_2026-10-15.md, section 5). Single-threaded.
 *
 * Two independent measures: (1) receipt rate against the nominal 50 Hz, and (2) the frame's 4-bit
 * rolling counter: a step of k > 1 between consecutive received frames means k - 1 lost frames.
 * A receipt gap longer than half the counter's cycle (8 periods) is ambiguous: frames delayed in a
 * buffer and whole lost counter cycles look the same, so such gaps are only counted (ambiguousGaps,
 * lastAmbiguous) and the counter-based loss then undercounts; the implied-rate check below catches
 * that. Whether the counter
 * really steps by one per frame is an INFERENCE (it matches 0x078's counter in the public log);
 * the meter checks it: the implied producer rate (received + counter-lost) / elapsed must be
 * within 10 % of nominal, otherwise counterConsistent() is false and only the receipt rate counts.
 */
public final class ObdLossMeter {
    public static final long GAP_NS = 100_000_000L, STALL_NS = 500_000_000L;
    private final long periodNs;
    private boolean have;
    private long firstNs, lastNs;
    private int lastCounter = -1;
    // per whole receipt second
    private long secStart, secFrames;
    /** True when the last frame followed a receipt gap of more than 8 periods (counter may alias). */
    public boolean lastAmbiguous;
    public long ambiguousGaps;
    public long frames, counterLost, timingLost, duplicates, gaps100ms, stalls500ms, maxGapNs, chainBreaks;
    public long minSecondFrames = Long.MAX_VALUE, maxSecondFrames, wholeSeconds;
    public final long[] stepHist = new long[16];

    public ObdLossMeter(long periodUs) { this.periodNs = periodUs * 1000L; }

    /**
     * Records one received frame (counter 0..15, or -1 when absent); returns the number of frames
     * estimated lost immediately before it (0 for the first frame of a chain and for a repeated counter).
     */
    public long add(int counter, long rxNs) {
        frames++;
        if (!have) {
            have = true;
            firstNs = rxNs;
            lastNs = rxNs;
            lastCounter = counter;
            secStart = rxNs;
            secFrames = 1;
            return 0;
        }
        long dt = rxNs - lastNs;
        if (dt < 0) dt = 0;
        while (rxNs - secStart >= 1_000_000_000L) {
            wholeSeconds++;
            minSecondFrames = Math.min(minSecondFrames, secFrames);
            maxSecondFrames = Math.max(maxSecondFrames, secFrames);
            secFrames = 0;
            secStart += 1_000_000_000L;
        }
        secFrames++;
        if (dt > maxGapNs) maxGapNs = dt;
        if (dt > GAP_NS) gaps100ms++;
        if (dt > STALL_NS) stalls500ms++;
        long lost;
        lastAmbiguous = dt > 8 * periodNs;
        if (lastAmbiguous) ambiguousGaps++;
        if (counter >= 0 && lastCounter >= 0) {
            int step = (counter - lastCounter) & 15;
            stepHist[step]++;
            if (step == 0 && dt <= 8 * periodNs) {
                // A repeated counter: counted, never dropped (the counter contract is an inference).
                duplicates++;
                lost = 0;
            } else {
                lost = step == 0 ? 15 : step - 1;
            }
            counterLost += lost;
        } else {
            long periods = Math.round((double) dt / periodNs);
            lost = dt > GAP_NS ? Math.max(0, periods - 1) : 0;
            timingLost += lost;
        }
        lastNs = rxNs;
        lastCounter = counter;
        return lost;
    }

    /** The next frame starts a new chain (adapter restart): no loss is inferred across the break. */
    public void breakChain() {
        if (!have) return;
        elapsedBefore += lastNs - firstNs;
        lastCounter = -1;
        have = false;
        chainBreaks++;
    }

    private long elapsedBefore;

    /** Receipt time summed over all chains (ns); the time between chains is not counted. */
    public long elapsedNs() { return elapsedBefore + (have ? lastNs - firstNs : 0); }

    public long lost() { return counterLost + timingLost; }

    /** Inter-frame intervals inside chains (the first frame of each chain opens no interval). */
    private long intervals() { return Math.max(0, frames - chainBreaks - (have ? 1 : 0)); }

    /** Received frames per second over the elapsed receipt time (0 before 1 s). */
    public double fps() {
        long e = elapsedNs();
        return e < 1_000_000_000L ? 0 : intervals() * 1e9 / e;
    }

    /** Implied producer rate from received + counter-lost frames. */
    public double impliedHz() {
        long e = elapsedNs();
        return e < 1_000_000_000L ? 0 : (intervals() + lost()) * 1e9 / e;
    }

    public double nominalHz() { return 1e9 / periodNs; }

    /** True when at least 10 s were measured and the implied rate is within 10 % of nominal. */
    public boolean counterConsistent() {
        return elapsedNs() >= 10_000_000_000L && Math.abs(impliedHz() - nominalHz()) <= 0.1 * nominalHz();
    }

    /** Loss fraction in percent: counter-based when consistent, else from the receipt rate. */
    public double lossPercent() {
        if (frames == 0) return 100;
        if (counterConsistent()) return 100.0 * lost() / (frames + lost());
        double f = fps();
        return f <= 0 ? 0 : Math.max(0, 100.0 * (1 - f / nominalHz()));
    }

    public String summary() {
        return String.format(Locale.ROOT,
                "frames=%d elapsed_s=%.1f fps=%.1f implied_hz=%.1f loss_pct=%.1f counter_ok=%b counter_lost=%d timing_lost=%d dup=%d ambiguous=%d "
                        + "gaps100=%d stalls500=%d max_gap_ms=%d sec_min=%d sec_max=%d breaks=%d step1=%d",
                frames, elapsedNs() / 1e9, fps(), impliedHz(), lossPercent(), counterConsistent(), counterLost, timingLost, duplicates, ambiguousGaps,
                gaps100ms, stalls500ms, maxGapNs / 1_000_000L, wholeSeconds == 0 ? 0 : minSecondFrames, maxSecondFrames, chainBreaks,
                stepHist[1]);
    }

    /**
     * Parked-check verdict (docs/OBD_YAW_CHECK_KO.md): NONE without frames, GOOD at >= 45 frames/s
     * with <= 5 % loss and no stall over 500 ms, MARGINAL at >= 30 frames/s, else BAD.
     */
    public String verdict() {
        if (frames == 0) return "NONE";
        double f = fps(), l = lossPercent();
        if (f >= 45 && l <= 5 && stalls500ms == 0) return "GOOD";
        if (f >= 30) return "MARGINAL";
        return "BAD";
    }
}
