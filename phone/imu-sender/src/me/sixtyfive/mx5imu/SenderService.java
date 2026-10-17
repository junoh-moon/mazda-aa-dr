package me.sixtyfive.mx5imu;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothManager;
import android.bluetooth.BluetoothProfile;
import android.bluetooth.BluetoothSocket;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ServiceInfo;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.Looper;
import android.os.PowerManager;
import android.os.SystemClock;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.List;
import java.util.Locale;
import java.util.UUID;
import me.sixtyfive.mx5imu.core.AbCompare;
import me.sixtyfive.mx5imu.core.Clock;
import me.sixtyfive.mx5imu.core.ElmController;
import me.sixtyfive.mx5imu.core.Integrator;
import me.sixtyfive.mx5imu.core.Link;
import me.sixtyfive.mx5imu.core.ObdLink;
import me.sixtyfive.mx5imu.core.Protocol;
import me.sixtyfive.mx5imu.core.ReconnectPolicy;
import me.sixtyfive.mx5imu.core.RollingLog;
import me.sixtyfive.mx5imu.core.Session;
import me.sixtyfive.mx5imu.core.SourceSelector;
import me.sixtyfive.mx5imu.core.Transport;
import me.sixtyfive.mx5imu.core.Window;

/**
 * connectedDevice foreground service. While enabled it keeps an RFCOMM client connection to the
 * chosen CMU; sensors, the partial wake lock and the sender run only while connected.
 * Stage 2 (validation/OBD_YAW_CHANNEL_DESIGN_2026-10-15.md): when an OBD-II adapter is chosen, the
 * OBD link (CAN 0x079 monitor) also runs while connected, and SourceSelector sends either the gyro
 * windows or the OBD windows (FLAG_SOURCE_OBD) on the same stream. The CMU only records them.
 * While an OBD adapter is configured, AbCompare also sums the windows of BOTH sources per 10 s into the app's
 * separate A/B ring (record-only, phone side only; nothing sent changes; design note section 16).
 */
public final class SenderService extends Service implements SensorEventListener, Link.Listener {
    static final String ACTION_STOP = "me.sixtyfive.mx5imu.STOP";
    /** The app's 'retry now' button: clears the circuit breaker (validation/PHONE_IMU_CHANNEL_2026-10-14.md). */
    static final String ACTION_RETRY_NOW = "me.sixtyfive.mx5imu.RETRY_NOW";
    private static final String CHANNEL = "link";
    private static final int NOTE_ID = 1;
    private static final int GYRO_US = 5000;      // 200 Hz, no HIGH_SAMPLING_RATE_SENSORS needed
    private static final int GRAVITY_US = 10000;  // 100 Hz
    private static final int BIAS_US = 100000;    // 10 Hz; only the bias fields are used
    static final Clock CLOCK = SystemClock::elapsedRealtimeNanos;

    private final Handler main = new Handler(Looper.getMainLooper());
    private RollingLog log;
    /** The A/B comparison rows (separate ring; written on the main thread by tick). */
    private RollingLog abLog;
    private final AbCompare ab = new AbCompare();
    private Link link;
    private BluetoothAdapter adapter;
    private String address;
    private HandlerThread sensorThread;
    private Handler sensorHandler;
    private SensorManager sensors;
    private final Integrator integrator = new Integrator(); // sensor thread only
    private volatile Session active;
    private volatile SourceSelector selector = new SourceSelector(false);
    private volatile ObdLink obd;
    /** The thread stopping the previous session's link; the next link waits for it (no overlap). */
    private volatile Thread obdStopping;
    private PowerManager.WakeLock wake;
    private BroadcastReceiver receiver;
    private int ticks, gyroCount;
    private long sessionFramesBase;
    private String noteText = "";

    static void start(Context c, String why) {
        try {
            c.startForegroundService(new Intent(c, SenderService.class).putExtra("why", why));
        } catch (RuntimeException e) {
            // ForegroundServiceStartNotAllowedException (API 31+) or SecurityException.
            Status.lastError = "서비스 시작 거부: " + e.getClass().getSimpleName();
            new RollingLog(c.getFilesDir(), CLOCK).event("service_start_denied why=" + why + " " + e);
        }
    }

    static void stop(Context c) {
        c.startService(new Intent(c, SenderService.class).setAction(ACTION_STOP));
    }

    @Override public void onCreate() {
        super.onCreate();
        log = new RollingLog(getFilesDir(), CLOCK);
        abLog = RollingLog.ab(getFilesDir(), CLOCK);
        NotificationChannel ch = new NotificationChannel(CHANNEL, "IMU 송신 상태", NotificationManager.IMPORTANCE_LOW);
        ch.setShowBadge(false);
        getSystemService(NotificationManager.class).createNotificationChannel(ch);
        sensors = getSystemService(SensorManager.class);
        sensorThread = new HandlerThread("imu-sensors", android.os.Process.THREAD_PRIORITY_URGENT_DISPLAY);
        sensorThread.start();
        sensorHandler = new Handler(sensorThread.getLooper());
        wake = getSystemService(PowerManager.class).newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "mx5imu:link");
        wake.setReferenceCounted(false);
    }

    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent != null && ACTION_STOP.equals(intent.getAction())) {
            Prefs.setEnabled(this, false);
            log.event("disabled_from_notification_or_app");
            stopSelf();
            return START_NOT_STICKY;
        }
        if (intent != null && ACTION_RETRY_NOW.equals(intent.getAction())) {
            Link l = link;
            if (l != null) {
                log.event("breaker_retry_now");
                l.retryNow();
                Status.breaker = "";
                updateNote("연결 대기 중");
                return START_STICKY;
            }
            // not running: the button already updated the stored breaker; start normally below
        }
        String why = intent == null ? "sticky_restart" : intent.getStringExtra("why");
        if (!Prefs.enabled(this) || Prefs.address(this) == null) { stopSelf(); return START_NOT_STICKY; }
        if (!Prefs.bluetoothGranted(this)) {
            fail("no_bluetooth_connect_permission", "Bluetooth 권한이 없습니다");
            return START_NOT_STICKY;
        }
        try {
            startForeground(NOTE_ID, note("연결 대기 중"), ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE);
        } catch (RuntimeException e) {
            fail("foreground_denied " + e, "포그라운드 시작 거부: " + e.getClass().getSimpleName());
            return START_NOT_STICKY;
        }
        if (link == null) begin(Prefs.address(this), why);
        return START_STICKY;
    }

    private void fail(String event, String shown) {
        log.event(event);
        Status.lastError = shown;
        Status.running = false;
        stopSelf();
    }

    private void begin(String addr, String why) {
        adapter = getSystemService(BluetoothManager.class).getAdapter();
        if (adapter == null) { fail("no_bluetooth_adapter", "Bluetooth 장치가 없습니다"); return; }
        address = addr;
        final BluetoothDevice device;
        try { device = adapter.getRemoteDevice(addr); }
        catch (IllegalArgumentException e) { fail("bad_address " + addr, "기기 주소가 잘못됐습니다"); return; }
        Status.running = true;
        Status.state = "연결 대기 중";
        log.event("service_start why=" + why + " device=" + addr);
        link = new Link(new RfcommConnector(device), this, CLOCK, 2000, 30000, 500);
        final Context ctx = this;
        link.setStore(new Link.Store() {
            @Override public String load() { return Prefs.breaker(ctx); }
            @Override public void save(String record) { Prefs.setBreaker(ctx, record); }
            @Override public long wallMs() { return System.currentTimeMillis(); }
            @Override public long bootCount() { return Prefs.bootCount(ctx); }
        });
        ReconnectPolicy.BreakerState restored = link.breakerState();
        log.event("breaker_restored state=" + restored + " record=" + Prefs.breaker(this));
        if (restored == ReconnectPolicy.BreakerState.LOCKED) breaker(restored, 0);
        else if (restored == ReconnectPolicy.BreakerState.OPEN) breaker(restored, link.openUntilMs());
        receiver = new BroadcastReceiver() {
            @Override public void onReceive(Context c, Intent i) { onBluetoothEvent(i); }
        };
        IntentFilter f = new IntentFilter();
        f.addAction(BluetoothDevice.ACTION_ACL_CONNECTED);
        f.addAction(BluetoothDevice.ACTION_ACL_DISCONNECTED);
        f.addAction(BluetoothAdapter.ACTION_STATE_CHANGED);
        registerReceiver(receiver, f, Context.RECEIVER_NOT_EXPORTED);
        link.start();
        if (!adapter.isEnabled()) link.adapter(false);
        probeProfiles(device);
        main.post(tick);
    }

    /** If hands-free or media audio is already connected to the CMU, its ACL link is up. */
    private void probeProfiles(final BluetoothDevice device) {
        for (final int profile : new int[] {BluetoothProfile.HEADSET, BluetoothProfile.A2DP}) {
            adapter.getProfileProxy(this, new BluetoothProfile.ServiceListener() {
                @Override public void onServiceConnected(int p, BluetoothProfile proxy) {
                    try {
                        List<BluetoothDevice> devices = proxy.getConnectedDevices();
                        if (devices.contains(device) && link != null) {
                            log.event("acl_up_by_profile " + p);
                            link.aclConnected();
                        }
                    } catch (SecurityException e) {
                        log.event("profile_probe_denied " + p);
                    } finally {
                        adapter.closeProfileProxy(p, proxy);
                    }
                }
                @Override public void onServiceDisconnected(int p) {}
            }, profile);
        }
    }

    private void onBluetoothEvent(Intent i) {
        Link l = link;
        if (l == null) return;
        String a = i.getAction();
        if (BluetoothAdapter.ACTION_STATE_CHANGED.equals(a)) {
            int s = i.getIntExtra(BluetoothAdapter.EXTRA_STATE, BluetoothAdapter.ERROR);
            if (s == BluetoothAdapter.STATE_ON) { log.event("adapter_on"); l.adapter(true); }
            else if (s == BluetoothAdapter.STATE_TURNING_OFF || s == BluetoothAdapter.STATE_OFF) { log.event("adapter_off"); l.adapter(false); }
            return;
        }
        BluetoothDevice d = Build.VERSION.SDK_INT >= 33
                ? i.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE, BluetoothDevice.class)
                : legacyDevice(i);
        if (d == null || !d.getAddress().equalsIgnoreCase(address)) return;
        if (BluetoothDevice.ACTION_ACL_CONNECTED.equals(a)) { log.event("acl_connected"); l.aclConnected(); }
        else if (BluetoothDevice.ACTION_ACL_DISCONNECTED.equals(a)) { log.event("acl_disconnected"); l.aclDisconnected(); }
    }

    @SuppressWarnings("deprecation")
    private static BluetoothDevice legacyDevice(Intent i) { return i.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE); }

    private final Runnable tick = new Runnable() {
        @Override public void run() {
            if (link == null) return;
            Session s = active;
            if (s != null) {
                Status.framesSentTotal = sessionFramesBase + s.framesSent.get();
                Status.pongsTotal = s.pongsSent.get();
                Status.crcErrors = s.crcErrors();
                Status.lastPingElapsedMs = (CLOCK.nanos() - s.lastPingNs()) / 1_000_000L;
            }
            ObdLink o = obd;
            SourceSelector sel = selector;
            Status.source = sel.update(CLOCK.nanos()).name();
            if (o != null) {
                Status.obdState = o.state();
                Status.obdFps = o.fps();
                Status.obdLossPct = o.lossPercent();
            }
            // a window added after disconnected() (an event that read the session before it ended) opens no row
            ab.poll(CLOCK.nanos(), s != null);
            for (String row = ab.take(); row != null; row = ab.take()) abLog.event(row);
            if (ticks % 60 == 59 && o != null) {
                log.event("obd_stats source=" + Status.source + " switches=" + sel.switches + " phone_sent=" + sel.phoneWindowsSent
                        + " obd_sent=" + sel.obdWindowsSent + " obd_held=" + sel.obdWindowsHeld + " " + ab.summary() + " | " + o.summary());
            }
            if (++ticks % 60 == 0 && s != null) {
                log.event(String.format(Locale.ROOT, "stats frames=%d pongs=%d crc_errors=%d unknown_version=%d unexpected=%d gyro_hz=%d",
                        s.framesSent.get(), s.pongsSent.get(), s.crcErrors(), s.unknownVersion(), s.unexpectedFrames.get(), Status.gyroRateHz));
            }
            main.postDelayed(this, 1000);
        }
    };

    // ---- Link.Listener (called on the link thread) ----

    @Override public void connected(Session s) {
        ab.reset();
        ab.setEnabled(Prefs.obdAddress(this) != null);
        active = s;
        Status.connected = true;
        Status.connections++;
        Status.state = "CMU 연결됨";
        Status.lastError = "";
        wake.acquire();
        log.event("connected seq=" + (s == null ? 0 : linkSeq()));
        sensorHandler.post(this::registerSensors);
        startObd();
        updateNote("CMU 연결됨 · 자이로 송신 중");
    }

    private long linkSeq() { Link l = link; return l == null ? 0 : l.seq().get() & 0xFFFFFFFFL; }

    @Override public void disconnected(Session s, String reason) {
        if (active == s) active = null;
        ab.reset();
        sessionFramesBase += s.framesSent.get();
        Status.connected = false;
        Status.state = "연결 대기 중";
        sensorHandler.post(this::unregisterSensors);
        stopObd();
        if (wake.isHeld()) wake.release();
        log.event(String.format(Locale.ROOT, "disconnected reason=%s frames=%d pongs=%d crc_errors=%d unknown_version=%d unexpected=%d dropped_before_ping=%d dropped_stale_ping=%d ping_span_ms=%d",
                reason, s.framesSent.get(), s.pongsSent.get(), s.crcErrors(), s.unknownVersion(), s.unexpectedFrames.get(),
                s.droppedBeforePing.get(), s.droppedStalePing.get(), s.pingSpanMs()));
        if (!"stopped".equals(reason)) Status.lastError = "연결 끊김: " + reason;
        updateNote("연결 대기 중");
    }

    @Override public void connectFailed(String error, long nextWaitMs, ReconnectPolicy.Acl acl) {
        Status.lastError = "연결 실패: " + error;
        Status.state = nextWaitMs < 0 ? "차량 연결 대기 중" : "재시도 대기 " + (nextWaitMs / 1000) + "초";
        log.event("connect_failed acl=" + acl + " next_ms=" + nextWaitMs + " " + error);
    }

    @Override public void breaker(ReconnectPolicy.BreakerState state, long untilMs) {
        if (state == ReconnectPolicy.BreakerState.OPEN) {
            long min = Math.max(1, (untilMs - CLOCK.nanos() / 1_000_000L + 59_999) / 60_000);
            log.event("breaker_open failures=3 retry_in_min=" + min);
            Status.breaker = "연속 실패 3회: " + min + "분 동안 연결 중지";
            updateNote("연결 반복 실패 · " + min + "분 동안 중지");
        } else if (state == ReconnectPolicy.BreakerState.LOCKED) {
            log.event("breaker_locked second_trip until_retry_now");
            Status.breaker = "연결 중지됨: 앱의 '지금 다시 시도'를 누르면 다시 시도합니다";
            updateNote("연결 중지됨 · 앱에서 '지금 다시 시도'");
        } else {
            log.event("breaker_closed");
            Status.breaker = "";
            updateNote("연결 대기 중");
        }
    }

    // ---- sensors (sensor thread) ----

    private void registerSensors() {
        sensors.unregisterListener(this);
        integrator.reset();
        Sensor gyro = sensors.getDefaultSensor(Sensor.TYPE_GYROSCOPE);
        Sensor gravity = sensors.getDefaultSensor(Sensor.TYPE_GRAVITY);
        Sensor uncal = sensors.getDefaultSensor(Sensor.TYPE_GYROSCOPE_UNCALIBRATED);
        boolean g = gyro != null && sensors.registerListener(this, gyro, GYRO_US, 0, sensorHandler);
        boolean v = gravity != null && sensors.registerListener(this, gravity, GRAVITY_US, 0, sensorHandler);
        boolean b = uncal != null && sensors.registerListener(this, uncal, BIAS_US, 0, sensorHandler);
        log.event("sensors gyro=" + g + (gyro == null ? "" : "(" + gyro.getName() + ",min_us=" + gyro.getMinDelay() + ")")
                + " gravity=" + v + " uncal=" + b);
        if (!g) Status.lastError = "자이로 센서를 등록하지 못했습니다";
        gyroCount = 0;
        sensorHandler.postDelayed(rate, 1000);
    }

    private final Runnable rate = new Runnable() {
        @Override public void run() {
            Status.gyroRateHz = gyroCount;
            gyroCount = 0;
            sensorHandler.postDelayed(this, 1000);
        }
    };

    private void unregisterSensors() {
        sensors.unregisterListener(this);
        sensorHandler.removeCallbacks(rate);
        integrator.reset();
        Status.gyroRateHz = 0;
    }

    @Override public void onSensorChanged(SensorEvent e) {
        Session s = active;
        if (s == null) return;
        switch (e.sensor.getType()) {
            case Sensor.TYPE_GRAVITY:
                integrator.gravity(e.values[0], e.values[1], e.values[2]);
                break;
            case Sensor.TYPE_GYROSCOPE_UNCALIBRATED:
                integrator.bias(e.timestamp, e.values[3], e.values[4], e.values[5]);
                break;
            case Sensor.TYPE_GYROSCOPE:
                gyroCount++;
                Window w = integrator.gyro(e.timestamp, e.values[0], e.values[1], e.values[2]);
                if (w != null) send(s, SourceSelector.Source.PHONE, w);
                break;
            default:
                break;
        }
    }

    @Override public void onAccuracyChanged(Sensor sensor, int accuracy) {}

    // ---- stage 2: OBD 0x079 source (record-only at the CMU) ----

    /** Offers a window of one source; the selector passes only the chosen source (order kept by its lock). */
    private void send(Session s, SourceSelector.Source from, Window w) {
        SourceSelector sel = selector;
        synchronized (sel) {
            Window x = AbCompare.admit(sel, ab, from, w, CLOCK.nanos());
            if (x != null) s.offer(x);
        }
    }

    /** The deferred start of this session's OBD link while the parked check holds the adapter (or null). */
    private volatile Runnable obdAfterCheck;

    private void startObd() {
        String obdAddr = Prefs.obdAddress(this);
        if (obdAddr == null) { selector = new SourceSelector(false); return; }
        final BluetoothDevice dev;
        try { dev = adapter.getRemoteDevice(obdAddr); }
        catch (IllegalArgumentException e) { log.event("obd_bad_address " + obdAddr); selector = new SourceSelector(false); return; }
        selector = new SourceSelector(true);
        ObdLink o = new ObdLink(new SppConnector(dev), CLOCK, new ObdLink.Sink() {
            @Override public void window(Window w) {
                SourceSelector sel = selector;
                sel.obdFrame(w.tEndNs);
                Session s = active;
                if (s != null) send(s, SourceSelector.Source.OBD, w);
            }
            @Override public void event(String line) { log.event(line); }
        }, ElmController.Timing.DEFAULT, 2000, 30000);
        final Session session = active;
        final Runnable later = () -> main.post(() -> {
            // Only for the session that asked, and only while it is still the active one.
            if (link == null || session == null || active != session || obd != null) return;
            log.event("obd_start_after_check");
            startObd();
        });
        synchronized (Status.OBD) {  // atomic with the parked check's tryStartCheck()
            if (!Status.OBD.startServiceOrDefer(later)) {
                selector = new SourceSelector(false);
                obdAfterCheck = later;
                log.event("obd_deferred check_running (starts when the check ends)");
                return;
            }
            obdAfterCheck = null;
            obd = o;
        }
        log.event("obd_start device=" + obdAddr);
        o.start(obdStopping);
    }

    private void stopObd() {
        synchronized (Status.OBD) {  // a start deferred for this session must not run after it ended
            if (obdAfterCheck != null) { Status.OBD.cancelDeferred(); obdAfterCheck = null; }
        }
        final ObdLink o = obd;
        obd = null;
        if (o == null) return;
        Thread t = new Thread(() -> {
            o.stop();
            log.event("obd_stop | " + o.summary());
            synchronized (Status.OBD) {
                if (obd == null) {  // a new connection may already run its own link
                    Status.OBD.serviceStopped();
                    Status.obdState = "";
                }
            }
        }, "obd-stop");
        obdStopping = t;
        t.start();
    }

    // ---- notification ----

    private Notification note(String text) {
        noteText = text;
        PendingIntent open = PendingIntent.getActivity(this, 0, new Intent(this, MainActivity.class),
                PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        PendingIntent stop = PendingIntent.getService(this, 1, new Intent(this, SenderService.class).setAction(ACTION_STOP),
                PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        Notification.Builder b = new Notification.Builder(this, CHANNEL)
                .setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
                .setContentTitle("MX5DR IMU 송신")
                .setContentText(text)
                .setOngoing(true)
                .setContentIntent(open)
                .addAction(new Notification.Action.Builder(null, "송신 끄기", stop).build());
        if (Build.VERSION.SDK_INT >= 31) b.setForegroundServiceBehavior(Notification.FOREGROUND_SERVICE_IMMEDIATE);
        return b.build();
    }

    private void updateNote(String text) {
        main.post(() -> {
            if (link == null || text.equals(noteText)) return;
            getSystemService(NotificationManager.class).notify(NOTE_ID, note(text));
        });
    }

    @Override public void onDestroy() {
        main.removeCallbacks(tick);
        stopObd();
        final Link l = link;
        link = null;
        if (receiver != null) { try { unregisterReceiver(receiver); } catch (IllegalArgumentException ignored) {} receiver = null; }
        Status.running = false;
        Status.connected = false;
        Status.state = "꺼짐";
        final HandlerThread t = sensorThread;
        final PowerManager.WakeLock w = wake;
        new Thread(() -> {
            if (l != null) l.stop();
            sensors.unregisterListener(this);
            if (w.isHeld()) w.release();
            t.quitSafely();
        }, "imu-shutdown").start();
        log.event("service_stop");
        super.onDestroy();
    }

    @Override public IBinder onBind(Intent intent) { return null; }

    /** RFCOMM client per spec: secure socket to the service record with the spec UUID. */
    private static final class RfcommConnector implements Transport.Connector {
        private final BluetoothDevice device;
        private volatile BluetoothSocket pending;

        RfcommConnector(BluetoothDevice device) { this.device = device; }

        @Override public Transport open() throws IOException {
            final BluetoothSocket s = device.createRfcommSocketToServiceRecord(UUID.fromString(Protocol.SERVICE_UUID));
            pending = s;
            try {
                s.connect();
            } catch (IOException e) {
                try { s.close(); } catch (IOException ignored) {}
                throw e;
            } finally {
                pending = null;
            }
            return new Transport() {
                @Override public InputStream input() throws IOException { return s.getInputStream(); }
                @Override public OutputStream output() throws IOException { return s.getOutputStream(); }
                @Override public void close() { try { s.close(); } catch (IOException ignored) {} }
            };
        }

        @Override public void abort() {
            BluetoothSocket s = pending;
            if (s != null) try { s.close(); } catch (IOException ignored) {}
        }
    }
}
