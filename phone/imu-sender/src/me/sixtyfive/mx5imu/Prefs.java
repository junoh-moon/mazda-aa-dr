package me.sixtyfive.mx5imu;

import android.content.Context;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.os.Build;

final class Prefs {
    private static final String FILE = "sender";

    private Prefs() {}

    private static SharedPreferences p(Context c) { return c.getSharedPreferences(FILE, Context.MODE_PRIVATE); }
    static boolean enabled(Context c) { return p(c).getBoolean("enabled", false); }
    static String address(Context c) { return p(c).getString("address", null); }
    static String name(Context c) { return p(c).getString("name", ""); }
    /** BLUETOOTH_CONNECT exists from API 31; on API 30 the install-time BLUETOOTH permission applies. */
    static boolean bluetoothGranted(Context c) {
        return Build.VERSION.SDK_INT < 31
                || c.checkSelfPermission(android.Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED;
    }

    static String[] runtimePermissions() {
        if (Build.VERSION.SDK_INT >= 33) {
            return new String[] {android.Manifest.permission.BLUETOOTH_CONNECT, android.Manifest.permission.POST_NOTIFICATIONS};
        }
        if (Build.VERSION.SDK_INT >= 31) return new String[] {android.Manifest.permission.BLUETOOTH_CONNECT};
        return new String[0];
    }

    static boolean allGranted(Context c) {
        for (String p : runtimePermissions()) if (c.checkSelfPermission(p) != PackageManager.PERMISSION_GRANTED) return false;
        return true;
    }

    static void setEnabled(Context c, boolean on) { p(c).edit().putBoolean("enabled", on).apply(); }
    static void setDevice(Context c, String address, String name) {
        p(c).edit().putString("address", address).putString("name", name == null ? "" : name).apply();
    }
}
