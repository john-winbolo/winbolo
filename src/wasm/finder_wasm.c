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
 *   Joining a listed game navigates this tab to the game's
 *   /join/ address (wasmFinderJoin).
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

/* Join a listed game: load /join/<server key> in this tab, the address the
 * join path mints a join code from. The page unloads once the browser starts
 * the navigation; nothing here waits for it. A password-protected game is
 * joined the same way, and the join path asks for the password. */
EM_JS(void, wasmFinderJoin, (const char *serverKey), {
    window.location.href = '/join/' + encodeURIComponent(UTF8ToString(serverKey));
});
