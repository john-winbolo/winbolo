/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

package com.winbolo.android;

import android.os.SystemClock;
import android.util.Log;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicReference;

/**
 * The HTTP transport the WinBolo.net module (src/winbolonet/http_jni.c)
 * calls through JNI: one request over HttpURLConnection, with the system
 * certificate store and nothing but https.
 *
 * The native side owns the request and the response. It hands over the
 * method, the URL, the header pairs and the body bytes, and takes the
 * response back as it arrives through {@link #nativeBegin} (the status and
 * the advertised length) and {@link #nativeChunk} (each run of body bytes),
 * so the buffer, the file, the byte ceiling and the progress reports all
 * live in C. The native side can stop a transfer from either upcall, and
 * {@link #nativePoll}, called on the requesting thread every 50 ms while
 * the transfer runs, lets it stop one that is waiting on the network (its
 * cancel flag, or the shutdown deadline).
 *
 * The transfer itself runs on a pooled thread, because HttpURLConnection
 * has no way to stop a blocked connect or read other than
 * {@link HttpURLConnection#disconnect()} from another thread. The
 * requesting thread blocks in {@link #request} until the transfer is over,
 * and no upcall can reach the native context after request has returned.
 *
 * Redirects are handled here rather than by HttpURLConnection, which would
 * follow a same-host one on its own and drop the body on a cross-host one.
 * A 3xx to the same scheme, host and port is followed with every header,
 * as libcurl follows one; one to another origin drops the Authorization
 * header and is followed only for a request with no body. A request with
 * a body (a POST or a PUT) is never redirected to another origin: the 3xx
 * is handed back as the answer, which every caller treats as a failure.
 * A 303, or a 301 or 302 on a POST, becomes a GET without the body, as
 * libcurl does by default; a 307 or 308 sends the body again.
 */
public final class WinBoloHttp {
    private static final String TAG = "WinBoloHttp";

    /** The request did not get an answer: the reason went to nativeError. */
    public static final int RESULT_TRANSPORT_ERROR = -1;
    /** nativePoll or nativeChunk stopped the transfer; the native side
     *  knows why. */
    public static final int RESULT_STOPPED = -2;
    /** The total time limit ran out before the answer was complete. */
    public static final int RESULT_TIMED_OUT = -3;

    /** How often the requesting thread asks the native side whether to go on. */
    private static final long POLL_MS = 50;
    /** Redirect hops before the request is given up. */
    private static final int MAX_REDIRECTS = 20;
    /** Body bytes handed to nativeChunk at a time. */
    private static final int CHUNK_BYTES = 16384;

    private static final ExecutorService POOL = Executors.newCachedThreadPool(r -> {
        Thread t = new Thread(r, "WinBolo.net HTTP");
        t.setDaemon(true);
        return t;
    });

    private WinBoloHttp() {}

    /* Implemented in src/winbolonet/http_jni.c and registered from there.
     * ctx is the address of the native request. */
    private static native void nativeBegin(long ctx, int status, long expectedLength);
    private static native boolean nativeChunk(long ctx, byte[] data, int length);
    private static native int nativePoll(long ctx);
    private static native void nativeError(long ctx, String reason);

    /**
     * Runs one request and streams its answer to the native side. Blocks
     * the calling thread until the transfer is over.
     *
     * @param ctx              the native request, passed back on every upcall
     * @param method           GET, POST or PUT
     * @param url              the full https URL
     * @param headers          header names and values, alternating
     * @param body             the request body, or null for none
     * @param connectTimeoutMs connect limit, or 0 for the total limit; it
     *                         covers name lookup, connect and the TLS
     *                         handshake together, as libcurl's does: the
     *                         connection's own connect timeout bounds the
     *                         TCP connect alone, so the loop below also ends
     *                         a request whose connection is not yet up when
     *                         this limit runs out
     * @param totalTimeoutMs   total limit for the whole request
     * @return the HTTP status (after any redirect), or one of the RESULT_
     *         values
     */
    public static int request(long ctx, String method, String url, String[] headers,
                              byte[] body, int connectTimeoutMs, int totalTimeoutMs) {
        Transfer transfer = new Transfer(ctx, method, url, headers, body,
                                         connectTimeoutMs, totalTimeoutMs);
        long startMs = SystemClock.elapsedRealtime();
        Future<Integer> future = POOL.submit(transfer);
        int result;
        try {
            for (;;) {
                try {
                    result = future.get(POLL_MS, TimeUnit.MILLISECONDS);
                    break;
                } catch (TimeoutException e) {
                    /* Still running: ask the native side, then the clock. */
                }
                if (nativePoll(ctx) != 0) {
                    transfer.abort();
                    result = RESULT_STOPPED;
                    break;
                }
                long elapsed = SystemClock.elapsedRealtime() - startMs;
                if (elapsed >= totalTimeoutMs ||
                    (connectTimeoutMs > 0 && !transfer.connected && elapsed >= connectTimeoutMs)) {
                    transfer.abort();
                    result = RESULT_TIMED_OUT;
                    break;
                }
            }
        } catch (ExecutionException e) {
            Throwable cause = e.getCause() != null ? e.getCause() : e;
            nativeError(ctx, cause.toString());
            result = RESULT_TRANSPORT_ERROR;
        } catch (InterruptedException e) {
            transfer.abort();
            nativeError(ctx, "interrupted");
            result = RESULT_TRANSPORT_ERROR;
        } finally {
            /* After this no upcall can start, and one in progress has
             * returned: the native request may be freed. */
            transfer.close();
        }
        Log.i(TAG, method + " " + withoutQuery(url) + " -> " + result + " ("
              + transfer.received + " bytes, "
              + (SystemClock.elapsedRealtime() - startMs) + " ms)");
        return result;
    }

    /** The URL for the log line: a query can carry an upload key. */
    private static String withoutQuery(String url) {
        int q = url.indexOf('?');
        return q >= 0 ? url.substring(0, q) + "?..." : url;
    }

    /** One request on its pooled thread. */
    private static final class Transfer implements java.util.concurrent.Callable<Integer> {
        private final long ctx;
        private final String url;
        private final String[] headers;
        private final int connectTimeoutMs;
        private final int totalTimeoutMs;
        private String method;
        private byte[] body;

        /* Set by abort(); read between hops and after a read fails. */
        private volatile boolean aborted;
        /* Set once the connection is up (name lookup, connect and handshake
         * done), which is what the connect limit bounds. */
        volatile boolean connected;
        private final AtomicReference<HttpURLConnection> current = new AtomicReference<>();

        /* Every upcall runs under this lock with closed false; close() sets
         * closed under the same lock, so it returns only once no upcall is
         * in progress and none can start. */
        private final Object lock = new Object();
        private boolean closed;

        volatile long received;

        Transfer(long ctx, String method, String url, String[] headers, byte[] body,
                 int connectTimeoutMs, int totalTimeoutMs) {
            this.ctx = ctx;
            this.method = method;
            this.url = url;
            this.headers = headers;
            this.body = body;
            this.connectTimeoutMs = connectTimeoutMs;
            this.totalTimeoutMs = totalTimeoutMs;
        }

        void abort() {
            aborted = true;
            HttpURLConnection conn = current.get();
            if (conn != null) {
                conn.disconnect();
            }
        }

        void close() {
            synchronized (lock) {
                closed = true;
            }
        }

        private boolean begin(int status, long expected) {
            synchronized (lock) {
                if (closed) return false;
                nativeBegin(ctx, status, expected);
                return true;
            }
        }

        private boolean chunk(byte[] data, int length) {
            synchronized (lock) {
                if (closed) return false;
                return nativeChunk(ctx, data, length);
            }
        }

        private static boolean isRedirect(int status) {
            return status == 301 || status == 302 || status == 303 ||
                   status == 307 || status == 308;
        }

        private static boolean sameOrigin(URL a, URL b) {
            int portA = a.getPort() == -1 ? a.getDefaultPort() : a.getPort();
            int portB = b.getPort() == -1 ? b.getDefaultPort() : b.getPort();
            return a.getProtocol().equalsIgnoreCase(b.getProtocol()) &&
                   a.getHost().equalsIgnoreCase(b.getHost()) &&
                   portA == portB;
        }

        @Override
        public Integer call() throws IOException {
            URL first = new URL(url);
            if (!"https".equalsIgnoreCase(first.getProtocol())) {
                throw new IOException("refused: the scheme is not https");
            }
            URL target = first;
            boolean dropAuthorization = false;
            int connectMs = connectTimeoutMs > 0 ? Math.min(connectTimeoutMs, totalTimeoutMs)
                                                 : totalTimeoutMs;

            for (int hop = 0; ; hop++) {
                if (aborted) return RESULT_STOPPED;
                HttpURLConnection conn = (HttpURLConnection) target.openConnection();
                current.set(conn);
                try {
                    conn.setInstanceFollowRedirects(false);
                    conn.setUseCaches(false);
                    conn.setConnectTimeout(connectMs);
                    conn.setReadTimeout(totalTimeoutMs);
                    conn.setRequestMethod(method);
                    for (int i = 0; i + 1 < headers.length; i += 2) {
                        if (dropAuthorization && "Authorization".equalsIgnoreCase(headers[i])) {
                            continue;
                        }
                        conn.setRequestProperty(headers[i], headers[i + 1]);
                    }
                    /* Bodies arrive as the server sent them, so the byte
                     * counts the native side sees are the real ones. */
                    conn.setRequestProperty("Accept-Encoding", "identity");
                    if (body != null) {
                        conn.setDoOutput(true);
                        conn.setFixedLengthStreamingMode(body.length);
                    }
                    /* The connection itself, before any of the request goes
                     * out, so the connect limit ends here. */
                    connected = false;
                    conn.connect();
                    connected = true;
                    if (body != null) {
                        OutputStream out = conn.getOutputStream();
                        out.write(body);
                        out.close();
                    }

                    int status = conn.getResponseCode();
                    if (aborted) return RESULT_STOPPED;

                    if (isRedirect(status) && hop < MAX_REDIRECTS) {
                        String location = conn.getHeaderField("Location");
                        URL next = location != null ? new URL(target, location) : null;
                        if (next != null) {
                            boolean same = sameOrigin(first, next);
                            if (same || body == null) {
                                if (!"https".equalsIgnoreCase(next.getProtocol())) {
                                    throw new IOException("redirect to a scheme other than https refused");
                                }
                                if (!same) {
                                    dropAuthorization = true;
                                }
                                if (status == 303 ||
                                    ((status == 301 || status == 302) && "POST".equals(method))) {
                                    method = "GET";
                                    body = null;
                                }
                                target = next;
                                continue;
                            }
                            Log.w(TAG, "redirect to another origin refused: the request has a body");
                        }
                    }

                    long expected = conn.getContentLengthLong();
                    if (!begin(status, expected > 0 ? expected : 0)) {
                        return RESULT_STOPPED;
                    }
                    InputStream in;
                    try {
                        in = conn.getInputStream();
                    } catch (IOException e) {
                        in = conn.getErrorStream();
                    }
                    if (in != null) {
                        try {
                            byte[] buf = new byte[CHUNK_BYTES];
                            int n;
                            while ((n = in.read(buf)) > 0) {
                                received += n;
                                if (!chunk(buf, n)) {
                                    return RESULT_STOPPED;
                                }
                            }
                        } catch (IOException e) {
                            if (aborted) return RESULT_STOPPED;
                            throw e;
                        } finally {
                            in.close();
                        }
                    }
                    return status;
                } catch (IOException e) {
                    if (aborted) return RESULT_STOPPED;
                    throw e;
                } finally {
                    current.set(null);
                    conn.disconnect();
                }
            }
        }
    }
}
