package me.sixtyfive.mx5imu;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

/**
 * BOOT_COMPLETED and MY_PACKAGE_REPLACED are exemptions from the Android 12+ background
 * foreground-service start restriction, and Android 15 does not forbid the connectedDevice type
 * from BOOT_COMPLETED (only dataSync, camera, mediaPlayback, phoneCall, mediaProjection, microphone).
 */
public final class BootReceiver extends BroadcastReceiver {
    @Override public void onReceive(Context context, Intent intent) {
        String a = intent.getAction();
        if (!Intent.ACTION_BOOT_COMPLETED.equals(a) && !Intent.ACTION_MY_PACKAGE_REPLACED.equals(a)) return;
        if (!Prefs.enabled(context) || Prefs.address(context) == null) return;
        SenderService.start(context, "boot:" + a);
    }
}
