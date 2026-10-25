/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

package com.winbolo.android;

import android.content.Context;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.util.Log;
import org.libsdl.app.SDL;

/**
 * The kind of network the phone's traffic goes out on, for the warning the
 * game front shows before an Internet game is hosted on mobile data
 * (src/android/android_netpath.c, which reads {@link #kind} through JNI).
 *
 * The kind is kept here, updated by the default network callback
 * ConnectivityManager delivers on its own thread, so the native query is a
 * read of one field and returns at once, as the game front requires. Until
 * the first callback the active network is read directly.
 */
public final class WinBoloNet {
    private static final String TAG = "WinBoloNet";

    /* The values android_netpath.c maps onto GameFrontNetPath. */
    public static final int KIND_UNKNOWN  = 0;
    public static final int KIND_WIFI     = 1;
    public static final int KIND_ETHERNET = 2;
    public static final int KIND_CELLULAR = 3;
    public static final int KIND_NONE     = 4;

    private static volatile int sKind = KIND_UNKNOWN;
    private static ConnectivityManager.NetworkCallback sCallback;

    private WinBoloNet() {}

    /** The kind last seen; read from native code on the game's main thread. */
    public static int kind() {
        return sKind;
    }

    /** Starts watching the default network. True when the watch is up. */
    public static synchronized boolean start() {
        if (sCallback != null) {
            return true;
        }
        Context context = SDL.getContext();
        ConnectivityManager cm = context == null ? null
                : (ConnectivityManager) context.getSystemService(Context.CONNECTIVITY_SERVICE);
        if (cm == null) {
            Log.w(TAG, "No ConnectivityManager; network kind stays unknown");
            return false;
        }
        Network active = cm.getActiveNetwork();
        update(active == null ? KIND_NONE : kindOf(cm.getNetworkCapabilities(active)));
        ConnectivityManager.NetworkCallback callback = new ConnectivityManager.NetworkCallback() {
            @Override
            public void onCapabilitiesChanged(Network network, NetworkCapabilities caps) {
                update(kindOf(caps));
            }

            @Override
            public void onLost(Network network) {
                update(KIND_NONE);
            }

            @Override
            public void onUnavailable() {
                update(KIND_NONE);
            }
        };
        try {
            cm.registerDefaultNetworkCallback(callback);
        } catch (RuntimeException e) {
            Log.w(TAG, "Could not watch the default network: " + e);
            return false;
        }
        sCallback = callback;
        return true;
    }

    /* Cellular is tested first: a network that uses it goes out on mobile
       data whatever else it reports. */
    private static int kindOf(NetworkCapabilities caps) {
        if (caps == null) {
            return KIND_UNKNOWN;
        }
        if (caps.hasTransport(NetworkCapabilities.TRANSPORT_CELLULAR)) {
            return KIND_CELLULAR;
        }
        if (caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)) {
            return KIND_WIFI;
        }
        if (caps.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET)) {
            return KIND_ETHERNET;
        }
        return KIND_UNKNOWN;
    }

    private static void update(int kind) {
        if (sKind != kind) {
            sKind = kind;
            Log.i(TAG, "Network kind: " + name(kind));
        }
    }

    private static String name(int kind) {
        switch (kind) {
            case KIND_WIFI:     return "Wi-Fi";
            case KIND_ETHERNET: return "ethernet";
            case KIND_CELLULAR: return "mobile data";
            case KIND_NONE:     return "none";
            default:            return "unknown";
        }
    }
}
