/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          wbn_prefs_sync
 * Filename:      wbn_prefs_sync.h
 * Purpose:
 *   cJSON parsers and the cloud-sync orchestration for the
 *   WinBolo.net cloud preferences (GET/PUT /api/v1/prefs). The
 *   parsers and the PUT-body builder are pure cJSON; the one
 *   network-bound entry point (wbnPrefsSyncOnce) calls the
 *   transport in http.c (wbn_prefs_get / _put). The unit tests
 *   link this module libcurl-free by stubbing the two transport
 *   symbols, so they never drag in http.c or its dependency
 *   closure.
 *********************************************************/

#ifndef __WBN_PREFS_SYNC_H
#define __WBN_PREFS_SYNC_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The action a cloud-sync round decides on after the initial GET. */
typedef enum {
    WBN_SYNC_NOOP,         /* nothing to do (clean local, transport error) */
    WBN_SYNC_ADOPT_SERVER, /* download the server doc into the local doc */
    WBN_SYNC_PUT_LOCAL,    /* upload the local doc; baseUpdatedAt set below */
    WBN_SYNC_REAUTH        /* 401: route to re-auth */
} WbnSyncActionKind;

typedef struct {
    WbnSyncActionKind kind;
    char baseUpdatedAt[33];  /* for PUT_LOCAL: "" means send JSON null */
    bool isConflict;         /* PUT_LOCAL where both sides changed
                              * (local wins, visibly) */
} WbnSyncAction;

/*********************************************************
 *NAME:          wbnPrefsDecideAfterGet
 *PURPOSE:
 * Decide the sync action from the GET outcome and local state.
 * Pure: equality only on the opaque updatedAt token, never
 * magnitude.
 *
 *ARGUMENTS:
 * localDirty      - prefsSyncDirty()
 * lastSynced      - prefsGetLastSyncedUpdatedAt ("" if never synced)
 * getStatus       - HTTP code from wbn_prefs_get (or -1)
 * serverUpdatedAt - parsed updatedAt on 200 ("" otherwise)
 *********************************************************/
WbnSyncAction wbnPrefsDecideAfterGet(bool localDirty, const char *lastSynced,
                                     int getStatus, const char *serverUpdatedAt);

/* Result of parsing a GET /api/v1/prefs 200 body. */
typedef struct {
    char updatedAt[33];    /* 32 hex + NUL; "" if absent */
    char deviceType[65];   /* platform string; "" if null/absent */
    char *prefs;           /* malloc'd JSON string of the prefs object;
                            * caller frees; NULL if absent */
} WbnPrefsGetResult;

/*********************************************************
 *NAME:          wbnPrefsParseGet
 *PURPOSE:
 * Parse a GET /api/v1/prefs 200 body. On success the fields
 * of *out are populated and out->prefs is a freshly-allocated
 * JSON string (the serialized prefs object) the caller frees.
 * Returns 0 on success, nonzero on malformed input.
 *********************************************************/
int wbnPrefsParseGet(const char *body, WbnPrefsGetResult *out);

/*********************************************************
 *NAME:          wbnPrefsParseUpdatedAt
 *PURPOSE:
 * Parse an {updatedAt} body (PUT 200, or PUT 409). On success
 * copies the 32-hex token into out[33]; if updatedAt is JSON
 * null or absent, sets out[0]='\0' and still returns 0.
 * Returns nonzero only on malformed JSON.
 *********************************************************/
int wbnPrefsParseUpdatedAt(const char *body, char out[33]);

/* ---- Cloud-sync orchestration (network; not unit-tested) ------------- */

/*********************************************************
 *NAME:          wbnPrefsBuildPutBody
 *PURPOSE:
 * Build the PUT /api/v1/prefs request body:
 *   {"baseUpdatedAt":<token>|null,
 *    "device":{"type":…},
 *    "prefs":<parsed prefsJson object>}
 * baseUpdatedAt "" (or NULL) serializes to JSON null. prefsJson is
 * parsed and embedded as a JSON object (not a quoted string).
 * Returns a malloc'd JSON string the caller frees, or NULL on OOM
 * or when prefsJson does not parse. Pure (cJSON only): unit-tested.
 *********************************************************/
char *wbnPrefsBuildPutBody(const char *baseUpdatedAt, const char *deviceType,
                           const char *prefsJson);

/* The result of one cloud-sync round (wbnPrefsSyncOnce). */
typedef enum {
    WBN_SYNC_OUT_NOOP,    /* nothing changed (clean, transport error, capped) */
    WBN_SYNC_OUT_ADOPTED, /* server doc downloaded; apply it locally */
    WBN_SYNC_OUT_PUSHED,  /* local doc uploaded; token is the new version */
    WBN_SYNC_OUT_REAUTH   /* 401: token is stale, sign out */
} WbnSyncOutcomeKind;

typedef struct {
    WbnSyncOutcomeKind kind;
    char token[33];              /* ADOPTED: server updatedAt; PUSHED: new token */
    char *serverPrefs;           /* ADOPTED only: parsed prefs JSON; caller
                                  * frees; NULL otherwise */
    char serverDeviceType[65];   /* ADOPTED: platform that last wrote the cloud doc */
    bool wasConflict;            /* PUSHED: local edits overwrote a diverged server */
} WbnSyncOutcome;

/*********************************************************
 *NAME:          wbnPrefsSyncOnce
 *PURPOSE:
 * Run one cloud-sync round on a worker thread: GET the server
 * prefs, decide via wbnPrefsDecideAfterGet, then adopt the server
 * doc or upload the local snapshot. A PUT that races a concurrent
 * writer (409) re-GETs, re-decides and retries with the fresh base,
 * bounded to three attempts; a re-decide that flips to adopt
 * returns ADOPTED instead. Network-bound (calls wbn_prefs_get /
 * wbn_prefs_put) and takes every piece of document state by value,
 * so it never touches the prefs.c globals. Not unit-tested.
 *
 *ARGUMENTS:
 * userToken      - WBN bearer token
 * uploadSnapshot - prefsSerializeForUpload() body (PUT_LOCAL only)
 * deviceType     - this device's platform string
 * localDirty     - prefsSyncDirty()
 * lastSynced     - prefsGetLastSyncedUpdatedAt ("" if never synced)
 *********************************************************/
WbnSyncOutcome wbnPrefsSyncOnce(const char *userToken, const char *uploadSnapshot,
                                const char *deviceType,
                                bool localDirty, const char *lastSynced);

#ifdef __cplusplus
}
#endif

#endif /* __WBN_PREFS_SYNC_H */
