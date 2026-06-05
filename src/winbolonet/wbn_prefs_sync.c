/*
 * Copyright (c) 1998-2008 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          wbn_prefs_sync
 * Filename:      wbn_prefs_sync.c
 * Purpose:
 *   cJSON parsers, the PUT-body builder and the cloud-sync
 *   orchestration for the WinBolo.net cloud preferences. The
 *   parsers and wbnPrefsBuildPutBody are pure cJSON; wbnPrefsSyncOnce
 *   drives the transport (wbn_prefs_get / wbn_prefs_put) in http.c.
 *********************************************************/

#include "wbn_prefs_sync.h"

#include <string.h>
#include <stdlib.h>

#include "cJSON.h"
#include "http.h"
#include "../common/wb_log.h"

WbnSyncAction wbnPrefsDecideAfterGet(bool localDirty, const char *lastSynced,
                                     int getStatus, const char *serverUpdatedAt) {
    WbnSyncAction a;
    a.kind = WBN_SYNC_NOOP;
    a.baseUpdatedAt[0] = '\0';
    a.isConflict = false;

    if (lastSynced == NULL) lastSynced = "";
    if (serverUpdatedAt == NULL) serverUpdatedAt = "";

    if (getStatus == 404) {
        /* No prefs on the account yet: seed it. baseUpdatedAt "" -> null. */
        a.kind = WBN_SYNC_PUT_LOCAL;
        return a;
    }
    if (getStatus == 200) {
        if (!localDirty) {
            a.kind = WBN_SYNC_ADOPT_SERVER;
            return a;
        }
        a.kind = WBN_SYNC_PUT_LOCAL;
        if (strcmp(serverUpdatedAt, lastSynced) == 0) {
            /* Server unchanged since our last sync; our edits win. */
            strncpy(a.baseUpdatedAt, lastSynced, sizeof(a.baseUpdatedAt) - 1);
            a.baseUpdatedAt[sizeof(a.baseUpdatedAt) - 1] = '\0';
        } else {
            /* Both sides changed since last sync: genuine conflict.
             * Local wins, visibly, off the server's current token. */
            strncpy(a.baseUpdatedAt, serverUpdatedAt, sizeof(a.baseUpdatedAt) - 1);
            a.baseUpdatedAt[sizeof(a.baseUpdatedAt) - 1] = '\0';
            a.isConflict = true;
        }
        return a;
    }
    if (getStatus == 401) {
        a.kind = WBN_SYNC_REAUTH;
        return a;
    }
    /* Transport error, 429, 5xx, anything else: fail quietly and retry later. */
    return a;
}

/* True when s is exactly 32 lowercase/uppercase hex digits. */
static int isUpdatedAtToken(const char *s) {
    if (s == NULL) {
        return 0;
    }
    size_t i;
    for (i = 0; i < 32; i++) {
        char c = s[i];
        if (c == '\0') {
            return 0; /* shorter than 32 */
        }
        int isHex = (c >= '0' && c <= '9') ||
                    (c >= 'a' && c <= 'f') ||
                    (c >= 'A' && c <= 'F');
        if (!isHex) {
            return 0;
        }
    }
    return s[32] == '\0'; /* reject anything longer */
}

/* Copies a string item into dst (capacity dstSize) when it's a
 * non-null string; leaves dst empty otherwise. */
static void copyStringField(const cJSON *item, char *dst, size_t dstSize) {
    dst[0] = '\0';
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        strncpy(dst, item->valuestring, dstSize - 1);
        dst[dstSize - 1] = '\0';
    }
}

int wbnPrefsParseGet(const char *body, WbnPrefsGetResult *out) {
    if (body == NULL || out == NULL) {
        return 1;
    }

    out->updatedAt[0] = '\0';
    out->deviceId[0] = '\0';
    out->deviceLabel[0] = '\0';
    out->prefs = NULL;

    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return 1;
    }
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return 1;
    }

    /* prefs object is required for a well-formed 200 body. */
    cJSON *prefs = cJSON_GetObjectItemCaseSensitive(root, "prefs");
    if (!cJSON_IsObject(prefs)) {
        cJSON_Delete(root);
        return 1;
    }

    char *prefsStr = cJSON_PrintUnformatted(prefs);
    if (prefsStr == NULL) {
        cJSON_Delete(root);
        return 1;
    }
    out->prefs = prefsStr;

    /* updatedAt: only accept a 32-hex token; treat other shapes as absent. */
    cJSON *updatedAt = cJSON_GetObjectItemCaseSensitive(root, "updatedAt");
    if (cJSON_IsString(updatedAt) && isUpdatedAtToken(updatedAt->valuestring)) {
        memcpy(out->updatedAt, updatedAt->valuestring, 32);
        out->updatedAt[32] = '\0';
    }

    /* device: { id, label } — both may be null/absent. */
    cJSON *device = cJSON_GetObjectItemCaseSensitive(root, "device");
    if (cJSON_IsObject(device)) {
        copyStringField(cJSON_GetObjectItemCaseSensitive(device, "id"),
                        out->deviceId, sizeof(out->deviceId));
        copyStringField(cJSON_GetObjectItemCaseSensitive(device, "label"),
                        out->deviceLabel, sizeof(out->deviceLabel));
    }

    cJSON_Delete(root);
    return 0;
}

int wbnPrefsParseUpdatedAt(const char *body, char out[33]) {
    if (body == NULL || out == NULL) {
        return 1;
    }
    out[0] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return 1;
    }
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return 1;
    }

    cJSON *updatedAt = cJSON_GetObjectItemCaseSensitive(root, "updatedAt");
    /* JSON null or absent -> empty token, still success. A 32-hex string
     * is copied out; any other shape is treated as absent. */
    if (cJSON_IsString(updatedAt) && isUpdatedAtToken(updatedAt->valuestring)) {
        memcpy(out, updatedAt->valuestring, 32);
        out[32] = '\0';
    }

    cJSON_Delete(root);
    return 0;
}

/* The server rejects bodies over this size; skip rather than 413. */
#define WBN_PREFS_UPLOAD_CAP 65536

char *wbnPrefsBuildPutBody(const char *baseUpdatedAt, const char *deviceId,
                           const char *deviceLabel, const char *prefsJson) {
    if (prefsJson == NULL) {
        return NULL;
    }
    cJSON *prefs = cJSON_Parse(prefsJson);
    if (prefs == NULL) {
        return NULL;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        cJSON_Delete(prefs);
        return NULL;
    }

    if (baseUpdatedAt != NULL && baseUpdatedAt[0] != '\0') {
        cJSON_AddStringToObject(root, "baseUpdatedAt", baseUpdatedAt);
    } else {
        cJSON_AddNullToObject(root, "baseUpdatedAt");
    }

    cJSON *device = cJSON_CreateObject();
    cJSON_AddStringToObject(device, "id", deviceId != NULL ? deviceId : "");
    cJSON_AddStringToObject(device, "label", deviceLabel != NULL ? deviceLabel : "");
    cJSON_AddItemToObject(root, "device", device);

    /* Embed the prefs as a parsed object, not a quoted string. Transfers
     * ownership of prefs into root. */
    cJSON_AddItemToObject(root, "prefs", prefs);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

/* Copy a GET result into an ADOPTED outcome, transferring ownership of the
 * parsed prefs string. Leaves res->prefs NULL so the caller's free is a no-op. */
static void fillAdopted(WbnSyncOutcome *out, WbnPrefsGetResult *res) {
    out->kind = WBN_SYNC_OUT_ADOPTED;
    strncpy(out->token, res->updatedAt, sizeof(out->token) - 1);
    out->token[sizeof(out->token) - 1] = '\0';
    out->serverPrefs = res->prefs;
    res->prefs = NULL;
    strncpy(out->serverDeviceId, res->deviceId, sizeof(out->serverDeviceId) - 1);
    out->serverDeviceId[sizeof(out->serverDeviceId) - 1] = '\0';
    strncpy(out->serverDeviceLabel, res->deviceLabel, sizeof(out->serverDeviceLabel) - 1);
    out->serverDeviceLabel[sizeof(out->serverDeviceLabel) - 1] = '\0';
}

/* GET + parse helper for the sync loop. Returns the HTTP status; on a parsed
 * 200, *haveGet is set and serverUpdatedAt[33] receives the token. */
static int prefsGetAndParse(const char *userToken, WbnPrefsGetResult *res,
                            bool *haveGet, char serverUpdatedAt[33]) {
    char *body = NULL;
    int status = wbn_prefs_get(userToken, &body);
    res->prefs = NULL;
    *haveGet = false;
    serverUpdatedAt[0] = '\0';
    if (status == 200 && body != NULL && wbnPrefsParseGet(body, res) == 0) {
        *haveGet = true;
        strncpy(serverUpdatedAt, res->updatedAt, 32);
        serverUpdatedAt[32] = '\0';
    }
    free(body);
    return status;
}

/* Upload the local snapshot, resolving up to three 409 races by re-GETting
 * and retrying off the fresh base. A race that flips the decision to adopt
 * returns ADOPTED instead. */
static WbnSyncOutcome prefsPutWithRetry(const char *userToken,
                                        const char *uploadSnapshot,
                                        const char *deviceId,
                                        const char *deviceLabel,
                                        WbnSyncAction action, bool localDirty,
                                        const char *lastSynced) {
    WbnSyncOutcome out;
    memset(&out, 0, sizeof(out));
    out.kind = WBN_SYNC_OUT_NOOP;

    if (strlen(uploadSnapshot) > WBN_PREFS_UPLOAD_CAP) {
        static bool s_capLogged = false;
        if (!s_capLogged) {
            WB_LOG_WARN(WB_LOG_CAT_NET,
                        "wbn_prefs: upload snapshot %zu bytes over %d-byte cap; skipping",
                        strlen(uploadSnapshot), WBN_PREFS_UPLOAD_CAP);
            s_capLogged = true;
        }
        return out;
    }

    int attempt;
    for (attempt = 0; attempt < 3; attempt++) {
        char *body = wbnPrefsBuildPutBody(action.baseUpdatedAt, deviceId,
                                          deviceLabel, uploadSnapshot);
        if (body == NULL) {
            return out; /* NOOP */
        }
        char *putBody = NULL;
        int putStatus = wbn_prefs_put(userToken, body, &putBody);
        free(body);

        if (putStatus == 200) {
            out.kind = WBN_SYNC_OUT_PUSHED;
            if (putBody != NULL) {
                wbnPrefsParseUpdatedAt(putBody, out.token);
            }
            out.wasConflict = action.isConflict;
            free(putBody);
            return out;
        }
        if (putStatus == 401) {
            free(putBody);
            out.kind = WBN_SYNC_OUT_REAUTH;
            return out;
        }
        if (putStatus != 409) {
            free(putBody); /* 429 / 5xx / transport error -> retry later */
            return out;
        }
        free(putBody);

        /* 409: a concurrent writer moved the server. Re-GET, re-decide, retry. */
        WbnPrefsGetResult res;
        bool haveGet = false;
        char serverUpdatedAt[33];
        int getStatus = prefsGetAndParse(userToken, &res, &haveGet, serverUpdatedAt);

        WbnSyncAction next = wbnPrefsDecideAfterGet(localDirty, lastSynced,
                                                    getStatus, serverUpdatedAt);
        if (next.kind == WBN_SYNC_ADOPT_SERVER && haveGet) {
            fillAdopted(&out, &res);
            free(res.prefs);
            return out;
        }
        if (next.kind == WBN_SYNC_REAUTH) {
            free(res.prefs);
            out.kind = WBN_SYNC_OUT_REAUTH;
            return out;
        }
        if (next.kind != WBN_SYNC_PUT_LOCAL) {
            free(res.prefs);
            return out; /* NOOP */
        }
        action = next; /* retry with the fresh base */
        free(res.prefs);
    }
    return out; /* three 409s in a row: give up quietly */
}

WbnSyncOutcome wbnPrefsSyncOnce(const char *userToken, const char *uploadSnapshot,
                                const char *deviceId, const char *deviceLabel,
                                bool localDirty, const char *lastSynced) {
    WbnSyncOutcome out;
    memset(&out, 0, sizeof(out));
    out.kind = WBN_SYNC_OUT_NOOP;

    WbnPrefsGetResult res;
    bool haveGet = false;
    char serverUpdatedAt[33];
    int getStatus = prefsGetAndParse(userToken, &res, &haveGet, serverUpdatedAt);

    WbnSyncAction action = wbnPrefsDecideAfterGet(localDirty, lastSynced,
                                                  getStatus, serverUpdatedAt);
    switch (action.kind) {
        case WBN_SYNC_ADOPT_SERVER:
            if (haveGet) {
                fillAdopted(&out, &res);
            }
            break;
        case WBN_SYNC_REAUTH:
            out.kind = WBN_SYNC_OUT_REAUTH;
            break;
        case WBN_SYNC_PUT_LOCAL:
            out = prefsPutWithRetry(userToken, uploadSnapshot, deviceId,
                                    deviceLabel, action, localDirty, lastSynced);
            break;
        case WBN_SYNC_NOOP:
        default:
            break;
    }

    free(res.prefs);
    return out;
}
