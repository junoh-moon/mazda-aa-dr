package me.sixtyfive.mx5imu.core;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

/** A connected byte stream (an RFCOMM socket on the phone, a fake in tests). close() unblocks read/write. */
public interface Transport {
    InputStream input() throws IOException;
    OutputStream output() throws IOException;
    void close();

    /** Opens a new connection; abort() cancels a blocking open from another thread. */
    interface Connector {
        Transport open() throws IOException;
        void abort();
    }
}
