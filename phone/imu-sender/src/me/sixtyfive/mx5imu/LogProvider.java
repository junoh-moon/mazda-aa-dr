package me.sixtyfive.mx5imu;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;
import java.io.File;
import java.io.FileNotFoundException;

/** Serves exactly one read-only file, the exported diagnostic log, to a share target granted by URI. */
public final class LogProvider extends ContentProvider {
    static final String AUTHORITY = "me.sixtyfive.mx5imu.logs";
    static final String NAME = "mx5imu-log.txt";

    static File exportFile(android.content.Context c) {
        File dir = new File(c.getCacheDir(), "export");
        if (!dir.isDirectory() && !dir.mkdirs()) return new File(c.getCacheDir(), NAME);
        return new File(dir, NAME);
    }

    static Uri uri() { return Uri.parse("content://" + AUTHORITY + "/" + NAME); }

    private File resolve(Uri uri) throws FileNotFoundException {
        if (!NAME.equals(uri.getLastPathSegment()) || getContext() == null) throw new FileNotFoundException();
        return exportFile(getContext());
    }

    @Override public boolean onCreate() { return true; }

    @Override public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        if (!"r".equals(mode)) throw new FileNotFoundException("read only");
        return ParcelFileDescriptor.open(resolve(uri), ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override public String getType(Uri uri) { return "text/plain"; }

    @Override public Cursor query(Uri uri, String[] projection, String selection, String[] args, String sort) {
        File f;
        try { f = resolve(uri); } catch (FileNotFoundException e) { return null; }
        MatrixCursor c = new MatrixCursor(new String[] {OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE});
        c.addRow(new Object[] {NAME, f.length()});
        return c;
    }

    @Override public Uri insert(Uri uri, ContentValues values) { throw new UnsupportedOperationException(); }
    @Override public int delete(Uri uri, String s, String[] a) { throw new UnsupportedOperationException(); }
    @Override public int update(Uri uri, ContentValues v, String s, String[] a) { throw new UnsupportedOperationException(); }
}
