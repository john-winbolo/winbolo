package com.winbolo.android;

import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.net.wifi.WifiManager;
import android.os.Bundle;
import android.util.Log;
import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;

public class WinBoloActivity extends SDLActivity {
    private static final String TAG = "WinBolo";

    /* The copy of a recording handed over as a content:// stream, which
       native code cannot open itself. One name: each arrival replaces the
       last, so the cache never grows. */
    private static final String INCOMING_RECORDING = "incoming.wbv";

    /* Held for the life of the activity so the Wi-Fi driver passes incoming
       multicast to the app; without it the LAN search's mDNS replies and the
       hosted game's mDNS queries are dropped before they reach a socket. */
    private WifiManager.MulticastLock mMulticastLock;

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
        if (wifi != null) {
            mMulticastLock = wifi.createMulticastLock("WinBolo mDNS");
            mMulticastLock.setReferenceCounted(false);
            mMulticastLock.acquire();
            Log.i(TAG, "Multicast lock acquired: " + mMulticastLock.isHeld());
        } else {
            Log.w(TAG, "No WifiManager; multicast lock not acquired");
        }
    }

    @Override
    protected void onDestroy() {
        if (mMulticastLock != null) {
            if (mMulticastLock.isHeld()) {
                mMulticastLock.release();
            }
            mMulticastLock = null;
            Log.i(TAG, "Multicast lock released");
        }
        super.onDestroy();
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
        String recording = recordingPathFor(intent);
        if (recording != null) {
            Log.i(TAG, "Recording while running: " + recording);
            nativeOpenRecording(recording);
        }
    }

    /* The path native code can open for a VIEW intent's recording, or null
       when the intent carries none. A file:// URI is its own path; a
       content:// stream is copied into the cache directory first. */
    private String recordingPathFor(Intent intent) {
        Uri uri = intent.getData();
        if (uri == null) {
            return null;
        }
        String scheme = uri.getScheme();
        if ("file".equals(scheme)) {
            String path = uri.getPath();
            if (path != null && isRecordingName(path)) {
                return path;
            }
            return null;
        }
        if ("content".equals(scheme)) {
            return copyRecordingToCache(uri);
        }
        return null;
    }

    private static boolean isRecordingName(String name) {
        return name.toLowerCase().endsWith(".wbv");
    }

    /* Copies the stream behind a content:// URI to <cache>/incoming.wbv,
       removing every earlier copy first, and returns the path, or null when
       the copy failed. */
    private String copyRecordingToCache(Uri uri) {
        File dir = getCacheDir();
        File[] old = dir.listFiles();
        if (old != null) {
            for (File f : old) {
                if (f.isFile() && isRecordingName(f.getName()) && !f.delete()) {
                    Log.w(TAG, "Could not remove old recording " + f);
                }
            }
        }
        File out = new File(dir, INCOMING_RECORDING);
        try (InputStream in = getContentResolver().openInputStream(uri);
             OutputStream os = new FileOutputStream(out)) {
            if (in == null) {
                Log.w(TAG, "No stream for " + uri);
                return null;
            }
            byte[] buf = new byte[65536];
            long total = 0;
            int n;
            while ((n = in.read(buf)) > 0) {
                os.write(buf, 0, n);
                total += n;
            }
            Log.i(TAG, "Copied " + uri + " to " + out + " (" + total + " bytes)");
            return out.getPath();
        } catch (Exception e) {
            Log.w(TAG, "Could not copy " + uri + ": " + e);
            out.delete();
            return null;
        }
    }

    /* Defined in src/android/android_drops.c. */
    private static native void nativeOpenLink(String url);
    private static native void nativeOpenRecording(String path);
}
