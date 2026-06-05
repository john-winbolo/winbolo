/*
 * Copyright (c) 1998-2008 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          wbn_prefs_sync
 * Filename:      wbn_prefs_sync.c
 * Purpose:
 *   Pure cJSON parsers for the WinBolo.net cloud preferences
 *   sync responses. Libcurl-free by design — the transport
 *   (wbn_prefs_get / wbn_prefs_put) lives in http.c.
 *********************************************************/

#include "wbn_prefs_sync.h"

#include <string.h>
#include <stdlib.h>

#include "cJSON.h"

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
