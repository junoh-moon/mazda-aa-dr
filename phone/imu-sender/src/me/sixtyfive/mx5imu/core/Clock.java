package me.sixtyfive.mx5imu.core;

/** Monotonic clock in ns (SystemClock.elapsedRealtimeNanos on the phone). */
public interface Clock {
    long nanos();

    Clock SYSTEM = System::nanoTime;
}
