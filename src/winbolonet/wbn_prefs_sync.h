/*
 * Copyright (c) 1998-2008 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          wbn_prefs_sync
 * Filename:      wbn_prefs_sync.h
 * Purpose:
 *   Pure cJSON parsers for the WinBolo.net cloud preferences
 *   sync responses (GET/PUT /api/v1/prefs). Deliberately
 *   libcurl-free so the unit tests can link them without
 *   dragging in http.c or its dependency closure. The network
 *   transport itself lives in http.c (wbn_prefs_get / _put).
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
    char deviceId[65];     /* "" if null/absent */
    char deviceLabel[129]; /* "" if null/absent */
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

#ifdef __cplusplus
}
#endif

#endif /* __WBN_PREFS_SYNC_H */
