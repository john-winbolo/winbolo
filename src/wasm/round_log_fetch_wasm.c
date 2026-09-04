/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          round_log_fetch_wasm
 * Filename:      round_log_fetch_wasm.c
 * Purpose:
 *   Round-log download for the browser build's recap reel.
 *   The desktop client fetches a WinBolo.net-hosted round log
 *   on a std::thread through http.c; the WASM build has no
 *   threads and must never embed the signing key, so this file
 *   supplies the same transfer over the browser's fetch().
 *
 *   The reel keeps its retry ladder, its attempt cap and its
 *   size ceiling — only the transport lives here, and it is
 *   non-blocking on purpose. The reel drives it from inside an
 *   ImGui frame, so nothing here may suspend the C stack:
 *   Start hands the request to the page and returns, and Poll
 *   reads the outcome a frame at a time.
 *
 *   /logdownload needs no authentication and is served from the
 *   page's own origin, so this side never sees a token or a key.
 *   All HTTP detail lives in JS on the Module object (see
 *   shell.html).
 *********************************************************/

#include <stdint.h>
#include <stddef.h>

#include <emscripten.h>

#include "../bolo/public/wire_limits.h"  /* ROUND_LOG_MAX_BYTES */

/* Hand the request to the page and return. The outcome is parked in a
 * module-scope slot that Poll below drains; the slot identity is what tells a
 * settled request from one a cancel has already thrown away, so a reply that
 * arrives after the round moved on writes nothing. */
EM_JS(void, wbRoundLogFetchStart, (const char *key), {
    var slot = { pending: true, status: 0, bytes: null, ctl: null };
    Module.__wbRoundLogSlot = slot;
    try {
        slot.ctl = (typeof AbortController !== 'undefined')
                       ? new AbortController() : null;
        Module.wbRoundLogFetch(UTF8ToString(key),
                               slot.ctl ? slot.ctl.signal : undefined)
            .then(function(r) {
                if (Module.__wbRoundLogSlot !== slot) return;
                slot.status  = (r && r.status) ? r.status : -1;
                slot.bytes   = (r && r.body) ? r.body : null;
                slot.pending = false;
            })
            .catch(function(e) {
                if (Module.__wbRoundLogSlot !== slot) return;
                console.error('[REEL] round log fetch failed', e);
                slot.status  = -1;
                slot.bytes   = null;
                slot.pending = false;
            });
    } catch (e) {
        console.error('[REEL] round log fetch could not start', e);
        slot.status  = -1;
        slot.pending = false;
    }
});

/* Drained once per settled attempt: the slot is cleared before the bytes are
 * copied, so a caller that asks again gets 0 rather than the same reply twice.
 * maxBytes comes from the C wrapper below so the ceiling is stated once, in
 * the header that owns it. A body over it is refused whole — the status still
 * reports the 200 the server gave, and the caller reads "no bytes" and arms
 * its next rung, the same as any other attempt that produced nothing. */
EM_JS(int, wbRoundLogFetchPollJs,
      (uint8_t **outBuf, int *outLen, int maxBytes), {
    var slot = Module.__wbRoundLogSlot;
    if (!slot || slot.pending) return 0;
    Module.__wbRoundLogSlot = null;

    var bytes = slot.bytes;
    if (slot.status === 200 && bytes && bytes.length > 0) {
        if (bytes.length > maxBytes) {
            console.warn('[REEL] round log is ' + bytes.length +
                         ' bytes, over the ' + maxBytes + ' byte ceiling');
        } else {
            var p = _malloc(bytes.length);
            if (p) {
                HEAPU8.set(bytes, p);
                setValue(outBuf, p, '*');
                setValue(outLen, bytes.length, 'i32');
            }
        }
    }
    return slot.status;
});

/* Drop an in-flight request and the slot it would have written into, so the
 * next round starts clean. Nothing running is the ordinary case — the reel
 * calls this whenever it lets a summary go. */
EM_JS(void, wbRoundLogFetchCancel, (void), {
    var slot = Module.__wbRoundLogSlot;
    Module.__wbRoundLogSlot = null;
    if (slot && slot.ctl) {
        try { slot.ctl.abort(); } catch (e) { }
    }
});

/* 0 while the request is in flight; otherwise the HTTP status, or -1 when it
 * never completed. On a 200 whose body fits the ceiling, *outBuf takes a
 * malloc'd buffer the caller owns — the reel hands it straight to lvEmbedBegin,
 * which frees it with plain free() whether or not it accepts the data. */
int wbRoundLogFetchPoll(uint8_t **outBuf, int *outLen) {
    return wbRoundLogFetchPollJs(outBuf, outLen, (int)ROUND_LOG_MAX_BYTES);
}
