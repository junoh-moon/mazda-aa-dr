package me.sixtyfive.mx5imu;

import android.app.Activity;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothManager;
import android.content.ClipData;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.view.View;
import android.view.WindowInsets;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.ScrollView;
import android.widget.TextView;
import java.io.File;
import java.util.Locale;
import java.util.Set;
import me.sixtyfive.mx5imu.core.ReconnectPolicy;
import me.sixtyfive.mx5imu.core.RollingLog;

/** One-time setup screen: permissions, choose the CMU among bonded devices, on/off, status, log export. */
public final class MainActivity extends Activity {
    private static final int PERMS = 7;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private TextView status, deviceHint;
    private RadioGroup devices;
    private Button toggle, permit, retry;

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        final int pad = (int) (20 * getResources().getDisplayMetrics().density);
        layout.setOnApplyWindowInsetsListener((v, insets) -> {
            android.graphics.Insets s = insets.getInsets(WindowInsets.Type.systemBars());
            v.setPadding(pad + s.left, pad + s.top, pad + s.right, pad + s.bottom);
            return insets;
        });
        TextView title = new TextView(this);
        title.setText("MX5DR IMU 송신기");
        title.setTextSize(25);
        layout.addView(title);
        TextView about = new TextView(this);
        about.setText("이 앱은 휴대폰 자이로의 회전량을 Bluetooth로 차량 CMU에 보냅니다. "
                + "현재 단계에서는 CMU가 기록만 하며 내비게이션 위치나 차량 동작은 바꾸지 않습니다.\n"
                + "위치 권한과 인터넷 권한을 사용하지 않으며 센서 값을 휴대폰에 저장하지 않습니다.\n"
                + "처음 한 번만 아래 순서대로 설정하십시오. 1) 권한 허용 2) 차량(CMU) 선택 3) 송신 켜기 "
                + "4) 배터리 설정에서 이 앱을 '제한 없음'으로 두십시오.");
        about.setTextSize(15);
        layout.addView(about);
        permit = new Button(this);
        permit.setText("권한 허용");
        permit.setOnClickListener(v -> { if (Prefs.runtimePermissions().length > 0) requestPermissions(Prefs.runtimePermissions(), PERMS); });
        layout.addView(permit);
        deviceHint = new TextView(this);
        deviceHint.setTextSize(15);
        layout.addView(deviceHint);
        devices = new RadioGroup(this);
        layout.addView(devices);
        toggle = new Button(this);
        toggle.setOnClickListener(v -> onToggle());
        layout.addView(toggle);
        // Only this button clears a tripped circuit breaker; opening the app does not.
        retry = new Button(this);
        retry.setText("지금 다시 시도");
        retry.setOnClickListener(v -> retryNow());
        layout.addView(retry);
        Button battery = new Button(this);
        battery.setText("배터리 설정 열기");
        battery.setOnClickListener(v -> startActivity(new Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
                Uri.fromParts("package", getPackageName(), null))));
        layout.addView(battery);
        Button export = new Button(this);
        export.setText("진단 기록 내보내기");
        export.setOnClickListener(v -> exportLog());
        layout.addView(export);
        status = new TextView(this);
        status.setTextSize(15);
        layout.addView(status);
        ScrollView scroll = new ScrollView(this);
        scroll.addView(layout);
        setContentView(scroll);
    }

    private boolean bluetoothGranted() { return Prefs.bluetoothGranted(this); }

    @Override protected void onResume() {
        super.onResume();
        listDevices();
        ui.post(refresh);
    }

    @Override protected void onPause() {
        ui.removeCallbacks(refresh);
        super.onPause();
    }

    @Override public void onRequestPermissionsResult(int code, String[] perms, int[] results) {
        super.onRequestPermissionsResult(code, perms, results);
        listDevices();
    }

    private void listDevices() {
        devices.removeAllViews();
        permit.setEnabled(!Prefs.allGranted(this));
        if (!bluetoothGranted()) { deviceHint.setText("먼저 '권한 허용'을 누르고 '근처 기기' 권한을 허용하십시오."); return; }
        BluetoothManager m = getSystemService(BluetoothManager.class);
        BluetoothAdapter a = m == null ? null : m.getAdapter();
        if (a == null) { deviceHint.setText("이 휴대폰에서 Bluetooth를 찾지 못했습니다."); return; }
        Set<BluetoothDevice> bonded;
        try { bonded = a.getBondedDevices(); } catch (SecurityException e) { deviceHint.setText("Bluetooth 권한이 없습니다."); return; }
        if (bonded == null || bonded.isEmpty()) { deviceHint.setText("페어링된 기기가 없습니다. Bluetooth를 켜고 차량과 먼저 페어링하십시오."); return; }
        deviceHint.setText("차량(CMU)을 한 번 선택하십시오. 보통 차량 이름 또는 'Mazda'로 표시됩니다.");
        String chosen = Prefs.address(this);
        for (BluetoothDevice d : bonded) {
            RadioButton r = new RadioButton(this);
            r.setId(View.generateViewId());
            String name;
            try { name = d.getName(); } catch (SecurityException e) { name = null; }
            final String label = name == null ? "(이름 없음)" : name;
            r.setText(label + "\n" + d.getAddress());
            r.setChecked(d.getAddress().equals(chosen));
            r.setOnClickListener(v -> {
                boolean changed = !d.getAddress().equals(Prefs.address(this));
                Prefs.setDevice(this, d.getAddress(), label);
                if (changed && Prefs.enabled(this)) { SenderService.stop(this); Prefs.setEnabled(this, false); }
                updateToggle();
            });
            devices.addView(r);
        }
    }

    private void onToggle() {
        if (Prefs.enabled(this)) {
            Prefs.setEnabled(this, false);
            SenderService.stop(this);
        } else {
            if (!bluetoothGranted()) { status.setText("Bluetooth 권한이 필요합니다."); return; }
            if (Prefs.address(this) == null) { status.setText("먼저 차량(CMU)을 선택하십시오."); return; }
            Prefs.setEnabled(this, true);
            Status.lastError = "";
            SenderService.start(this, "user");
        }
        updateToggle();
    }

    private void updateToggle() {
        toggle.setText(Prefs.enabled(this) ? "송신 끄기" : "송신 켜기");
    }

    private final Runnable refresh = new Runnable() {
        @Override public void run() {
            updateToggle();
            retry.setVisibility(breakerTripped() ? View.VISIBLE : View.GONE);
            String dev = Prefs.address(MainActivity.this);
            StringBuilder b = new StringBuilder();
            b.append("선택한 차량: ").append(dev == null ? "없음" : Prefs.name(MainActivity.this) + " (" + dev + ")").append('\n');
            b.append("송신 설정: ").append(Prefs.enabled(MainActivity.this) ? "켜짐" : "꺼짐").append('\n');
            b.append("상태: ").append(Status.running ? Status.state : "꺼짐").append('\n');
            b.append("CMU 연결: ").append(Status.connected ? "예" : "아니요").append('\n');
            b.append(String.format(Locale.ROOT, "보낸 프레임: %d · 응답한 PING: %d · 연결 횟수: %d%n",
                    Status.framesSentTotal, Status.pongsTotal, Status.connections));
            if (Status.connected) {
                b.append(String.format(Locale.ROOT, "마지막 PING 수신: %.1f초 전 · 자이로 %d Hz%n",
                        Status.lastPingElapsedMs / 1000.0, Status.gyroRateHz));
            }
            b.append("왕복 시간(RTT)은 CMU가 PING/PONG으로 계산하여 CMU 기록에 남깁니다.\n");
            if (!Status.breaker.isEmpty()) b.append("연결 차단: ").append(Status.breaker).append('\n');
            b.append("최근 오류: ").append(Status.lastError.isEmpty() ? "없음" : Status.lastError);
            status.setText(b.toString());
            ui.postDelayed(this, 1000);
        }
    };

    private boolean breakerTripped() {
        ReconnectPolicy p = new ReconnectPolicy(2000, 30000);
        long now = SenderService.CLOCK.nanos() / 1_000_000L;
        p.restore(Prefs.breaker(this), now, System.currentTimeMillis(), Prefs.bootCount(this));
        return p.state(now) != ReconnectPolicy.BreakerState.CLOSED;
    }

    /** The explicit 'retry now': stored breaker cleared (one more trip locks again), sender told. */
    private void retryNow() {
        ReconnectPolicy p = new ReconnectPolicy(2000, 30000);
        long now = SenderService.CLOCK.nanos() / 1_000_000L, wall = System.currentTimeMillis(), boot = Prefs.bootCount(this);
        p.restore(Prefs.breaker(this), now, wall, boot);
        p.retryNow(now);
        Prefs.setBreaker(this, p.snapshot(now, wall, boot));
        Status.breaker = "";
        if (Status.running) startService(new Intent(this, SenderService.class).setAction(SenderService.ACTION_RETRY_NOW));
    }

    private void exportLog() {
        final File out = LogProvider.exportFile(this);
        new Thread(() -> {
            String error = null;
            try { new RollingLog(getFilesDir(), SenderService.CLOCK).exportTo(out); }
            catch (Exception e) { error = e.toString(); }
            final String failed = error;
            ui.post(() -> {
                if (isDestroyed()) return;
                if (failed != null) { status.setText("진단 기록을 만들지 못했습니다: " + failed); return; }
                Intent send = new Intent(Intent.ACTION_SEND).setType("text/plain")
                        .putExtra(Intent.EXTRA_STREAM, LogProvider.uri())
                        .putExtra(Intent.EXTRA_SUBJECT, "MX5DR IMU 진단 기록")
                        .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
                send.setClipData(ClipData.newRawUri(LogProvider.NAME, LogProvider.uri()));
                startActivity(Intent.createChooser(send, "진단 기록 내보내기"));
            });
        }, "imu-export").start();
    }
}
