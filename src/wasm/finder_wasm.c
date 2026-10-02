/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 * Name:          finder_wasm
 * Filename:      finder_wasm.c
 * Purpose:
 *   The game finder's list of internet games for the browser
 *   build. The desktop finder fetches /api/v1/games on a
 *   std::thread through http.c; the WASM build has no threads
 *   and never links http.c, so the page fetches the list
 *   itself and the finder takes the reply when it lands.
 *
 *   Nothing here waits. The finder calls Take once a frame
 *   from inside its dialog loop, so Start hands the request to
 *   the page and returns, and Take reads whatever has arrived.
 *   Parsing stays in C (wbnServerListParse), shared with the
 *   desktop.
 *
 *   /api/v1/games needs no authentication and is served from
 *   the page's own origin (docs/web-hosting.md).
 *
 *   Joining a listed game closes the finder and starts the
 *   game in this page (wasmFinderJoin, main_wasm.c).
 *********************************************************/

#include <emscripten.h>

/* Start fetching the game list and return. The reply is parked in a
 * module-scope slot for wasmFinderFetchTake; a newer Start replaces the
 * slot, so a reply to an older request finds it gone and writes nothing.
 * A reply that is not a 2xx, or no reply at all, parks a null body. */
EM_JS(void, wasmFinderFetchStart, (void), {
    var slot = { pending: true, body: null };
    Module.__wbFinderSlot = slot;
    try {
        fetch('/api/v1/games', { cache: 'no-store' })
            .then(function(r) {
                if (!r.ok) throw new Error('HTTP ' + r.status);
                return r.text();
            })
            .then(function(text) {
                if (Module.__wbFinderSlot !== slot) return;
                slot.body    = text;
                slot.pending = false;
            })
            .catch(function(e) {
                if (Module.__wbFinderSlot !== slot) return;
                console.warn('[FINDER] game list fetch failed', e);
                slot.body    = null;
                slot.pending = false;
            });
    } catch (e) {
        console.warn('[FINDER] game list fetch could not start', e);
        slot.pending = false;
    }
});

/* 0 while no request has finished. Otherwise 1, and the slot is cleared:
 * *out takes a malloc'd, NUL-terminated copy of the body the caller frees,
 * or NULL when the fetch failed. */
EM_JS(int, wasmFinderFetchTake, (char **out), {
    var slot = Module.__wbFinderSlot;
    setValue(out, 0, '*');
    if (!slot || slot.pending) return 0;
    Module.__wbFinderSlot = null;

    if (typeof slot.body === 'string') {
        var n = lengthBytesUTF8(slot.body) + 1;
        var p = _malloc(n);
        if (p) {
            stringToUTF8(slot.body, p, n);
            setValue(out, p, '*');
        }
    }
    return 1;
});

/* Start the page's relay latency test (Module.wbRelayProbe, shell.html) and
 * return without waiting for it, so a Join from the finder finds the closest
 * relay already picked. The test runs once per page; later calls start
 * nothing. */
EM_JS(void, wasmRelayProbeStart, (void), {
    Module.wbRelayProbe();
});
