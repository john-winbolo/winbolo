/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          prefs_bridge_wasm
 * Filename:      prefs_bridge_wasm.c
 * Purpose:
 *   Cloud-prefs sync for the browser build. The desktop client
 *   drives wbnPrefsSyncOnce() (GET, reconcile, PUT) over libcurl
 *   in http.c. The WASM build has no libcurl and must never embed
 *   the signing key, so this file instead provides the two
 *   transport entry points wbnPrefsSyncOnce needs —
 *   wbn_prefs_get / wbn_prefs_put — backed by the browser's
 *   fetch() (which carries the page's existing WinBolo.net
 *   session). ASYNCIFY lets those fetches block the C caller, so
 *   the whole desktop decision/retry/outcome machine runs here
 *   unchanged.
 *
 *   All HTTP detail (endpoint, auth, credentials) lives in JS on
 *   the Module object (see shell.html), so this side never sees a
 *   token or a key.
 *********************************************************/

#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>

#include <emscripten.h>

#include "../common/prefs.h"
#include "../common/prefs_doc.h"  /* PREFS_ADOPT_OK */
#include "../winbolonet/http.h"
#include "../winbolonet/wbn_prefs_sync.h"

/* Re-apply the live key bindings from the prefs document after a download
 * adopts new server settings mid-session (defined in gamefront_wasm.c). At
 * bootstrap this path is unused: the sync runs before gameFrontStart reads
 * keys, so the fresh read already sees the adopted document. */
extern void gameFrontReapplyKeysFromPrefs(void);

/* ---- Async transport primitives (EM_ASYNC_JS defines C-callable JS) -------
 * Each awaits the Module.wbPrefs* fetch helper in shell.html and writes a
 * malloc'd response body back through *out (pointer args arrive as addresses).
 * Returns the HTTP status, or -1 on a transport error. */
EM_ASYNC_JS(int, wbPrefsJsGet, (char **out), {
    try {
        const r = await Module.wbPrefsHttpGet();
        if (r && typeof r.body === 'string') {
            const len = lengthBytesUTF8(r.body) + 1;
            const p = _malloc(len);
            stringToUTF8(r.body, p, len);
            setValue(out, p, '*');
        }
        return (r && r.status) ? r.status : -1;
    } catch (e) {
        console.error('[WBN prefs] GET failed', e);
        return -1;
    }
});

EM_ASYNC_JS(int, wbPrefsJsPut, (const char *reqBody, char **out), {
    try {
        const r = await Module.wbPrefsHttpPut(UTF8ToString(reqBody));
        if (r && typeof r.body === 'string') {
            const len = lengthBytesUTF8(r.body) + 1;
            const p = _malloc(len);
            stringToUTF8(r.body, p, len);
            setValue(out, p, '*');
        }
        return (r && r.status) ? r.status : -1;
    } catch (e) {
        console.error('[WBN prefs] PUT failed', e);
        return -1;
    }
});

/* The two symbols wbnPrefsSyncOnce pulls from http.h. bearerToken is ignored —
 * the browser attaches the session itself. Caller frees *response_out, matching
 * the libcurl contract in http.c. */
int wbn_prefs_get(const char *bearerToken, char **response_out) {
    (void)bearerToken;
    char *body = NULL;
    int status = wbPrefsJsGet(&body);
    *response_out = body;
    return status;
}

int wbn_prefs_put(const char *bearerToken, const char *json_body,
                  char **response_out) {
    (void)bearerToken;
    char *body = NULL;
    int status = wbPrefsJsPut(json_body, &body);
    *response_out = body;
    return status;
}

/* True when the page reports a logged-in WinBolo.net session. Single-player
 * (tutorial / practise) reports false and skips sync entirely. */
static bool wbPrefsHasAuth(void) {
    return EM_ASM_INT({
        return (Module.wbPrefsHasAuth && Module.wbPrefsHasAuth()) ? 1 : 0;
    }) != 0;
}

/* One GET-reconcile-PUT pass, mirroring gameFront's desktop sync driver but
 * synchronous (the fetches block via ASYNCIFY rather than running on a
 * worker thread). Safe to call at bootstrap (before keys are read) and from
 * the debounced upload pump. No-op when not signed in. */
void wbPrefsSyncNow(void) {
    if (!wbPrefsHasAuth()) {
        return;
    }
    char *snapshot = prefsSerializeForUpload();
    if (snapshot == NULL) {
        return;
    }
    char lastSynced[33];
    prefsGetLastSyncedUpdatedAt(lastSynced, sizeof(lastSynced));

    /* userToken is the JS-side concern; pass a non-empty sentinel so the
     * orchestrator does not short-circuit on an empty bearer. */
    WbnSyncOutcome o = wbnPrefsSyncOnce("web-session", snapshot, "web",
                                        prefsSyncDirty(), lastSynced);
    free(snapshot);

    switch (o.kind) {
        case WBN_SYNC_OUT_ADOPTED:
            if (o.serverPrefs != NULL &&
                prefsAdoptServerDocument(o.serverPrefs) == PREFS_ADOPT_OK) {
                prefsMarkSynced(o.token);
                gameFrontReapplyKeysFromPrefs();
            }
            free(o.serverPrefs);
            break;
        case WBN_SYNC_OUT_PUSHED:
            prefsMarkSynced(o.token);
            break;
        case WBN_SYNC_OUT_REAUTH:
            EM_ASM({ if (Module.wbPrefsOnReauth) Module.wbPrefsOnReauth(); });
            break;
        case WBN_SYNC_OUT_NOOP:
        default:
            break;
    }
}

/* Debounced upload: once the local document is sync-dirty, push it no more
 * than once per WB_PREFS_UPLOAD_DEBOUNCE_MS. The PUT blocks via ASYNCIFY, so
 * this runs from the main loop only when there is actually something to send
 * (a settings change), never every frame. */
#define WB_PREFS_UPLOAD_DEBOUNCE_MS 15000

void wbPrefsPumpUpload(uint64_t nowMs) {
    static uint64_t s_lastAttemptMs = 0;
    static bool s_armed = false;
    if (!prefsSyncDirty()) {
        s_armed = false;
        return;
    }
    if (s_armed && (nowMs - s_lastAttemptMs) < WB_PREFS_UPLOAD_DEBOUNCE_MS) {
        return;
    }
    s_armed = true;
    s_lastAttemptMs = nowMs;
    wbPrefsSyncNow();
}
