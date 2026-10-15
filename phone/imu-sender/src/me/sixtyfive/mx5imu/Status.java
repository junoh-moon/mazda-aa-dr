package me.sixtyfive.mx5imu;

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
    static volatile boolean obdServiceRunning, obdCheckRunning;

    private Status() {}
}
