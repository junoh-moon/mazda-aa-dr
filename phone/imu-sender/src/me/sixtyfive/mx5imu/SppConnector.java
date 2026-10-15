package me.sixtyfive.mx5imu;

import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothSocket;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.UUID;
import me.sixtyfive.mx5imu.core.Transport;

/**
 * Stage 2: classic Bluetooth serial-port (SPP) client to a paired ELM327-class OBD-II adapter
 * (validation/OBD_YAW_CHANNEL_DESIGN_2026-10-15.md). Secure RFCOMM first; many clones accept only
 * an insecure socket, so that is tried once after a secure failure. The adapter accepts one client:
 * while another app (Car Scanner, RaceChrono) holds it, connecting fails and ObdLink backs off.
 */
final class SppConnector implements Transport.Connector {
    static final UUID SPP = UUID.fromString("00001101-0000-1000-8000-00805F9B34FB");
    private final BluetoothDevice device;
    private volatile BluetoothSocket pending;
    private volatile boolean aborted;

    SppConnector(BluetoothDevice device) { this.device = device; }

    @Override public Transport open() throws IOException {
        aborted = false;
        try {
            return connect(device.createRfcommSocketToServiceRecord(SPP));
        } catch (IOException secure) {
            if (aborted) throw secure;
            try {
                return connect(device.createInsecureRfcommSocketToServiceRecord(SPP));
            } catch (IOException insecure) {
                throw new IOException("secure: " + secure.getMessage() + "; insecure: " + insecure.getMessage());
            }
        } catch (SecurityException e) {
            throw new IOException("no permission: " + e.getMessage());
        }
    }

    private Transport connect(final BluetoothSocket s) throws IOException {
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
        aborted = true;
        BluetoothSocket s = pending;
        if (s != null) try { s.close(); } catch (IOException ignored) {}
    }
}
