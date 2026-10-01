/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
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
#include <string.h>

#include <emscripten.h>

#include "../common/prefs.h"
#include "../common/prefs_doc.h"  /* PREFS_ADOPT_OK */
#include "../winbolonet/http.h"
#include "../winbolonet/wbn_prefs_sync.h"

/* Apply a downloaded prefs document to the live game — keys, menu toggles,
 * game options, gamepad sensitivities and build options (defined in
 * main_wasm.c). Takes the inner prefs JSON object (the same shape the relay's
 * join-prefs frame carries), which is exactly what the adopt outcome hands us. */
extern void wasmApplyJoinPrefs(const char *prefsJson, int len);

/* The web's own settings as a JSON snapshot, and a writer for the ones that
 * differ between two snapshots (gamefront_wasm.c). */
extern char *wasmPrefsSnapshot(void);
extern void wasmPrefsWriteChanged(const char *baseline, const char *current);

/* The web's settings as the page set them up, taken at the page's first sync
 * before anything could change them. A late adopt compares against it to
 * find what the player changed. */
static char *s_pageStartPrefs = NULL;

/* Set once this page has taken in the account's server document. The web's
 * document starts empty on every page load, so until then it holds only what
 * this page wrote: it must never seed an account or replace a server
 * document it has not first taken in. Until this is set, a GET that finds no
 * document is reported as nothing to do, and a PUT is never sent. */
static bool s_adoptedThisPage = false;

/* Set when a GET before adoption answered 404: the account has no prefs
 * document, and this page will not make one (see s_adoptedThisPage). With
 * nothing to download and nothing allowed up, every later sync on this page
 * would be the same GET with the same answer, so the pump and the sync stop
 * here instead of asking again every debounce. A document the desktop
 * writes while this page is open is picked up on the next page load. */
static bool s_noServerDocument = false;

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
 * the libcurl contract in http.c.
 *
 * Before this page has adopted a server document, a 404 (no prefs on the
 * account) is reported as a transport error, which wbnPrefsSyncOnce answers
 * by doing nothing, rather than seeding the account with this page's
 * document; and a PUT is refused the same way without being sent. A GET that
 * returns the account's document is adopted as usual, whatever the local
 * state, because the page has no sync stamp of its own yet. */
int wbn_prefs_get(const char *bearerToken, char **response_out) {
    (void)bearerToken;
    char *body = NULL;
    int status = wbPrefsJsGet(&body);
    if (status == 404 && !s_adoptedThisPage) {
        s_noServerDocument = true;
        status = -1;
    }
    *response_out = body;
    return status;
}

int wbn_prefs_put(const char *bearerToken, const char *json_body,
                  char **response_out) {
    (void)bearerToken;
    *response_out = NULL;
    if (!s_adoptedThisPage) {
        return -1;
    }
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
    if (!wbPrefsHasAuth() || s_noServerDocument) {
        return;
    }
    if (!s_adoptedThisPage && s_pageStartPrefs == NULL) {
        s_pageStartPrefs = wasmPrefsSnapshot();
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
        case WBN_SYNC_OUT_ADOPTED: {
            /* Adopting clears sync-dirty, so ask first, and take the
             * settings as they are now before the adopt replaces them. */
            char *changed = NULL;
            if (prefsSyncDirty() && s_pageStartPrefs != NULL) {
                changed = wasmPrefsSnapshot();
            }
            if (o.serverPrefs != NULL &&
                prefsAdoptServerDocument(o.serverPrefs) == PREFS_ADOPT_OK) {
                prefsMarkSynced(o.token);
                s_adoptedThisPage = true;
                if (changed != NULL) {
                    /* The player changed settings on this page before the
                     * server's document arrived. Keep those, and only
                     * those: write each setting that differs from the
                     * page's start over the adopted document, which
                     * dirties it again, so the next sync sends the whole
                     * document with them on top. Then apply the merged
                     * document, so the server's other values take effect
                     * and the player's stay. */
                    wasmPrefsWriteChanged(s_pageStartPrefs, changed);
                    char *merged = prefsSerializeForUpload();
                    if (merged != NULL) {
                        wasmApplyJoinPrefs(merged, (int)strlen(merged));
                        free(merged);
                    }
                } else {
                    wasmApplyJoinPrefs(o.serverPrefs,
                                       (int)strlen(o.serverPrefs));
                }
            }
            free(changed);
            free(o.serverPrefs);
            if (s_adoptedThisPage) {
                /* Every later sync starts from the server's document. */
                free(s_pageStartPrefs);
                s_pageStartPrefs = NULL;
            }
            break;
        }
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
    /* Nothing can go up on this page (see s_noServerDocument), so a dirty
     * document is left dirty rather than re-fetched every debounce. */
    if (s_noServerDocument || !prefsSyncDirty()) {
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
