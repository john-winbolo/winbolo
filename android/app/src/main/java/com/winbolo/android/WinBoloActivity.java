package com.winbolo.android;

import android.content.Context;
import android.content.Intent;
import android.graphics.Insets;
import android.net.Uri;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.Bundle;
import android.util.Log;
import android.view.DisplayCutout;
import android.view.View;
import android.view.WindowInsets;
import android.view.inputmethod.InputMethodManager;
import androidx.core.content.FileProvider;
import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;

public class WinBoloActivity extends SDLActivity {
    private static final String TAG = "WinBolo";

    /* The copy of a recording handed over as a content:// stream, which
       native code cannot open itself, under its own directory in the files
       directory (not the cache directory, which Android purges under
       storage pressure). One name: each arrival replaces the last, so the
       directory never grows. */
    private static final String INCOMING_DIR = "incoming";
    private static final String INCOMING_RECORDING = "incoming.wbv";
    /* The name the copy is written under until it is complete, then renamed
       to INCOMING_RECORDING, so the recordings list never sees half a file.
       It does not end in .wbv, so the list skips it. */
    private static final String INCOMING_PARTIAL = "incoming.wbv.part";

    /* A recording is a zip archive: src/bolo/log.c writes it with minizip,
       and the WBOLOMOV header (LOG_HEADER, src/bolo/public/log.h) is inside
       its deflated log.dat member. The file itself therefore starts with a
       zip local file header's signature, which is what a stream must start
       with to be copied. */
    private static final byte[] RECORDING_MAGIC = { 'P', 'K', 3, 4 };

    /* The largest recording copied in, the viewer's own read limit
       (MOBILE_REC_MAX_BYTES, src/gui/mobile/mobile_recordings.cpp): a larger
       file would be refused there anyway. */
    private static final long RECORDING_MAX_BYTES = 64L * 1024 * 1024;

    /* Held while something needs incoming multicast, which the Wi-Fi driver
       otherwise drops before it reaches a socket: the LAN search's mDNS
       browse and a hosted game's mDNS advertiser, which ask through
       multicastHold (android_device.c). Holding it costs battery, so it is
       not held at other times. sMulticastHolds counts the holders; the lock
       is taken at the first and released at the last. Static, so a hold
       outlives a recreation of the activity. */
    private static WifiManager.MulticastLock sMulticastLock;
    private static int sMulticastHolds = 0;

    /* The on-screen keyboard's height in pixels while it is up, 0 while it
       is down, from the window's insets; Android 11 and later report them
       to a full-screen window, and older ones leave this at -1 (unknown).
       Read by native code (android_device.c) on SDL's thread, written on
       the main thread. */
    private static volatile int sImeHeightPx =
        (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) ? 0 : -1;

    /* The safe area's four insets in window pixels, meaning what iOS's safe
       area means: the display cutout and the navigation bar, nothing else.
       SDL's own safe area adds the system gesture zones, and in landscape
       that keeps a 48 dp band along the top of every screen (the
       notification shade's pull-down) out of the game. Null until the
       window's insets first arrive; they arrive again on every rotation.
       Read by native code (android_device.c) on SDL's thread, written on
       the main thread. */
    private static volatile int[] sSafeInsetsPx = null;

    @Override
    protected String[] getLibraries() {
        return new String[]{
            "SDL3",
            "main"
        };
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        WifiManager wifi = (WifiManager) getApplicationContext().getSystemService(Context.WIFI_SERVICE);
        synchronized (WinBoloActivity.class) {
            if (wifi != null) {
                sMulticastLock = wifi.createMulticastLock("WinBolo mDNS");
                sMulticastLock.setReferenceCounted(false);
                /* A recreation while a browse or an advertiser holds it. */
                if (sMulticastHolds > 0) {
                    sMulticastLock.acquire();
                    Log.i(TAG, "Multicast lock acquired again after recreation");
                }
            } else {
                Log.w(TAG, "No WifiManager; multicast lock not available");
            }
        }
        /* The decor view keeps its own handling: the listener only reads
           the keyboard's inset and the safe area on the way through. */
        getWindow().getDecorView().setOnApplyWindowInsetsListener(
            new View.OnApplyWindowInsetsListener() {
                @Override
                public WindowInsets onApplyWindowInsets(View v, WindowInsets insets) {
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                        sImeHeightPx = insets.isVisible(WindowInsets.Type.ime())
                            ? insets.getInsets(WindowInsets.Type.ime()).bottom : 0;
                    }
                    sSafeInsetsPx = safeInsetsOf(insets);
                    return v.onApplyWindowInsets(insets);
                }
            });
    }

    /* The cutout's and the navigation bar's insets, left, top, right,
       bottom. Android 11 and later name the two inset types; before that
       the cutout is its own object (Android 9 and later, null on a screen
       without one) and the navigation bar is the stable inset on the three
       sides it can sit on, never the top. */
    @SuppressWarnings("deprecation")
    private static int[] safeInsetsOf(WindowInsets insets) {
        int left = 0, top = 0, right = 0, bottom = 0;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            Insets safe = insets.getInsets(WindowInsets.Type.displayCutout()
                                           | WindowInsets.Type.navigationBars());
            left = safe.left;
            top = safe.top;
            right = safe.right;
            bottom = safe.bottom;
        } else {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
                DisplayCutout cutout = insets.getDisplayCutout();
                if (cutout != null) {
                    left = cutout.getSafeInsetLeft();
                    top = cutout.getSafeInsetTop();
                    right = cutout.getSafeInsetRight();
                    bottom = cutout.getSafeInsetBottom();
                }
            }
            left = Math.max(left, insets.getStableInsetLeft());
            right = Math.max(right, insets.getStableInsetRight());
            bottom = Math.max(bottom, insets.getStableInsetBottom());
        }
        return new int[]{ left, top, right, bottom };
    }

    /* The safe area's insets in window pixels, left, top, right, bottom
       (see sSafeInsetsPx), or null before the window's insets have
       arrived. */
    public static int[] safeInsetsPx() {
        return sSafeInsetsPx;
    }

    /* The configuration's smallest width in dp, the figure Android itself
       draws its large-screen line at (600): the phone or tablet decision in
       android_device.c. 0 without a context. */
    public static int smallestWidthDp() {
        Context ctx = getContext();
        if (ctx == null) {
            return 0;
        }
        return ctx.getResources().getConfiguration().smallestScreenWidthDp;
    }

    /* The on-screen keyboard's height in pixels while it is up, 0 while it
       is down, -1 where it is not known. */
    public static int imeHeightPx() {
        return sImeHeightPx;
    }

    /* Shows the keyboard again for SDL's text field after the Back key hid
       it: the field keeps the focus and SDL's text input stays on, and a
       hide and a show posted together from SDL cancel each other, so the
       input method is asked directly. Called from native (android_device.c)
       on SDL's thread. */
    public static void showKeyboardAgain() {
        final Context ctx = getContext();
        if (ctx == null || mSingleton == null) {
            return;
        }
        mSingleton.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                if (mTextEdit == null) {
                    return;
                }
                mTextEdit.setVisibility(View.VISIBLE);
                mTextEdit.requestFocus();
                InputMethodManager imm =
                    (InputMethodManager) ctx.getSystemService(Context.INPUT_METHOD_SERVICE);
                if (imm != null) {
                    imm.showSoftInput(mTextEdit, 0);
                }
            }
        });
    }

    @Override
    protected void onDestroy() {
        synchronized (WinBoloActivity.class) {
            if (sMulticastLock != null) {
                if (sMulticastLock.isHeld()) {
                    sMulticastLock.release();
                    Log.i(TAG, "Multicast lock released");
                }
                sMulticastLock = null;
            }
        }
        super.onDestroy();
    }

    /* Takes or gives back one hold on incoming multicast (see
       sMulticastLock). Called from native (android_device.c) on SDL's
       thread for a hosted game's advertiser and on the LAN search's own
       thread for its mDNS browse. */
    public static synchronized void multicastHold(boolean on) {
        if (on) {
            sMulticastHolds++;
            if (sMulticastHolds == 1 && sMulticastLock != null && !sMulticastLock.isHeld()) {
                sMulticastLock.acquire();
                Log.i(TAG, "Multicast lock acquired");
            }
        } else {
            if (sMulticastHolds > 0) {
                sMulticastHolds--;
            }
            if (sMulticastHolds == 0 && sMulticastLock != null && sMulticastLock.isHeld()) {
                sMulticastLock.release();
                Log.i(TAG, "Multicast lock released");
            }
        }
    }

    /* A cold launch: the winbolo:// link or the recording path reaches
       main() as its one argument. */
    @Override
    protected String[] getArguments() {
        Intent intent = getIntent();
        if (intent != null && Intent.ACTION_VIEW.equals(intent.getAction())) {
            Uri uri = intent.getData();
            if (uri != null && "winbolo".equals(uri.getScheme())) {
                return new String[]{ uri.toString() };
            }
            String recording = recordingPathFor(intent);
            if (recording != null) {
                return new String[]{ recording };
            }
        }
        return new String[]{};
    }

    /* A link or recording arriving while the app runs. Handed to native,
       which keeps it for the welcome screen: a game in progress is not
       interrupted. */
    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        if (intent == null || !Intent.ACTION_VIEW.equals(intent.getAction())) {
            return;
        }
        Uri uri = intent.getData();
        if (uri == null) {
            return;
        }
        if ("winbolo".equals(uri.getScheme())) {
            Log.i(TAG, "Link while running: " + uri);
            nativeOpenLink(uri.toString());
            return;
        }
        /* The copy can take seconds for a large file or a slow provider, and
           onNewIntent runs on the UI thread, so it runs on a thread of its
           own; the native side is told once the file is complete. A cold
           launch needs no thread: SDL calls getArguments on its own thread,
           not the UI thread. */
        final Intent handed = intent;
        new Thread(new Runnable() {
            @Override
            public void run() {
                String recording = recordingPathFor(handed);
                if (recording != null) {
                    Log.i(TAG, "Recording while running: " + recording);
                    nativeOpenRecording(recording);
                }
            }
        }, "WinBolo recording copy").start();
    }

    /* The path native code can open for a VIEW intent's recording, or null
       when the intent carries none. A content:// stream is copied into the
       incoming directory, since native code cannot open one. A file:// path is
       tried the same way first: one inside the app's own storage copies,
       while one outside it (Download, say) is refused on current Android,
       which grants a file only through a content URI, and is then handed
       over as it is so the viewer reports that it could not be opened. */
    private String recordingPathFor(Intent intent) {
        Uri uri = intent.getData();
        if (uri == null) {
            return null;
        }
        String scheme = uri.getScheme();
        if ("file".equals(scheme)) {
            String path = uri.getPath();
            if (path != null && isRecordingName(path)) {
                String copied = copyRecordingToIncoming(uri);
                return (copied != null) ? copied : path;
            }
            return null;
        }
        if ("content".equals(scheme)) {
            return copyRecordingToIncoming(uri);
        }
        return null;
    }

    /* The share sheet for a file under the app's files or cache directory
       (the two res/xml/file_paths.xml names), with a one-off read grant on
       its content URI for the app the player picks. Called from native
       (android_share.c) on SDL's thread; starting the chooser needs no
       particular thread. Returns false when the sheet could not be shown,
       which the FileProvider throws for a path outside those directories. */
    public static boolean shareFile(String path, String mime) {
        Context ctx = getContext();
        if (ctx == null) {
            Log.w(TAG, "No context; cannot share " + path);
            return false;
        }
        try {
            File file = new File(path);
            Uri uri = FileProvider.getUriForFile(ctx, ctx.getPackageName() + ".fileprovider", file);
            Intent send = new Intent(Intent.ACTION_SEND);
            send.setType(mime);
            send.putExtra(Intent.EXTRA_STREAM, uri);
            send.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            Intent chooser = Intent.createChooser(send, file.getName());
            chooser.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            ctx.startActivity(chooser);
            Log.i(TAG, "Share sheet for " + path + " as " + mime);
            return true;
        } catch (Exception e) {
            Log.w(TAG, "Could not share " + path + ": " + e);
            return false;
        }
    }

    private static boolean isRecordingName(String name) {
        return name.toLowerCase().endsWith(".wbv");
    }

    /* Copies the stream behind a content:// URI to
       <files>/incoming/incoming.wbv, removing every earlier copy first, and
       returns the path, or null when the copy failed. A stream that does
       not start with RECORDING_MAGIC, or is longer than RECORDING_MAX_BYTES,
       is refused. The bytes go to INCOMING_PARTIAL and are renamed to the
       final name only once all of them are written. Synchronized because
       onNewIntent's thread and getArguments on SDL's thread can both copy,
       and they share the one file name. */
    private synchronized String copyRecordingToIncoming(Uri uri) {
        File dir = new File(getFilesDir(), INCOMING_DIR);
        if (!dir.isDirectory() && !dir.mkdirs()) {
            Log.w(TAG, "Could not create " + dir);
            return null;
        }
        File[] old = dir.listFiles();
        if (old != null) {
            for (File f : old) {
                if (f.isFile()
                    && (isRecordingName(f.getName()) || INCOMING_PARTIAL.equals(f.getName()))
                    && !f.delete()) {
                    Log.w(TAG, "Could not remove old recording " + f);
                }
            }
        }
        File partial = new File(dir, INCOMING_PARTIAL);
        File out = new File(dir, INCOMING_RECORDING);
        boolean complete = false;
        try (InputStream in = getContentResolver().openInputStream(uri)) {
            if (in == null) {
                Log.w(TAG, "No stream for " + uri);
                return null;
            }
            byte[] buf = new byte[65536];
            int head = 0;
            while (head < RECORDING_MAGIC.length) {
                int n = in.read(buf, head, RECORDING_MAGIC.length - head);
                if (n < 0) {
                    break;
                }
                head += n;
            }
            if (head < RECORDING_MAGIC.length || !startsWithRecordingMagic(buf)) {
                Log.w(TAG, "Not a recording: " + uri);
                return null;
            }
            long total = head;
            try (OutputStream os = new FileOutputStream(partial)) {
                os.write(buf, 0, head);
                int n;
                while ((n = in.read(buf)) > 0) {
                    total += n;
                    if (total > RECORDING_MAX_BYTES) {
                        Log.w(TAG, "Refused " + uri + ": longer than "
                              + RECORDING_MAX_BYTES + " bytes");
                        return null;
                    }
                    os.write(buf, 0, n);
                }
            }
            if (!partial.renameTo(out)) {
                Log.w(TAG, "Could not rename " + partial + " to " + out);
                return null;
            }
            complete = true;
            Log.i(TAG, "Copied " + uri + " to " + out + " (" + total + " bytes)");
            return out.getPath();
        } catch (Exception e) {
            Log.w(TAG, "Could not copy " + uri + ": " + e);
            return null;
        } finally {
            if (!complete) {
                partial.delete();
            }
        }
    }

    private static boolean startsWithRecordingMagic(byte[] buf) {
        for (int i = 0; i < RECORDING_MAGIC.length; i++) {
            if (buf[i] != RECORDING_MAGIC[i]) {
                return false;
            }
        }
        return true;
    }

    /* Defined in src/android/android_drops.c. */
    private static native void nativeOpenLink(String url);
    private static native void nativeOpenRecording(String path);
}
