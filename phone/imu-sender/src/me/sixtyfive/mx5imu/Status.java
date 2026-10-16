package me.sixtyfive.mx5imu;

import me.sixtyfive.mx5imu.core.ObdArbiter;

/** Process-wide status for the setup screen (written by the service, polled by the activity). */
final class Status {
    static volatile boolean running, connected;
    static volatile String state = "꺼짐";
    static volatile String lastError = "";
    /** Circuit-breaker message for the screen ("" while closed). */
    static volatile String breaker = "";
    static volatile long framesSentTotal, pongsTotal, connections, lastPingElapsedMs, crcErrors;
    static volatile int gyroRateHz;
    /** Stage 2 OBD source: link state, received 0x079 frames/s, loss %, the source sent to the CMU, last parked check. */
    static volatile String obdState = "", source = "PHONE", obdCheck = "";
    static volatile double obdFps, obdLossPct;
    /**
     * Who holds the single-client adapter (the sender's link or the parked check; they run on different
     * threads). Its monitor is the lock for every check-then-set; a session start during the check is
     * deferred until the check ends (ObdArbiter).
     */
    static final ObdArbiter OBD = new ObdArbiter();

    private Status() {}
}
