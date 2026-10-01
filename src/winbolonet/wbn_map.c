/*
 * Copyright (c) 1998-2008 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 * Name:          wbn_map
 * Filename:      wbn_map.c
 * Purpose:
 *   Resolves a stored map by its md5 (POST /api/v1/map).
 *   wbnMapParseResponse is a pure cJSON parser that
 *   base64-decodes the returned map bytes; wbnMapFetchByMd5
 *   drives the transport via wbn_api_post in http.c.
 *********************************************************/

#include "wbn_map.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "http.h"

/* Hard cap on a decoded map blob. A real .map never exceeds the wire
 * download limit (MAP_DOWNLOAD_MAX_SIZE, 64 KiB, defined in the T2
 * header netpacks.h that winbolonet_core may not include), so reject
 * anything larger: a hostile or corrupt /api/v1/map body must not be
 * able to drive an unbounded allocation. */
#define WBN_MAP_MAX_DECODED 65536u

/* Decode value of a base64 character, accepting both the standard
 * (+, /) and URL-safe (-, _) alphabets, or -1 for any character not in
 * either alphabet (the '=' pad is handled separately). */
static int b64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

/* Decode a base64 string into a freshly allocated buffer, tolerant of
 * real-world encodings: ASCII whitespace (MIME line wrapping) is skipped
 * anywhere, both the standard and URL-safe alphabets are accepted, '='
 * ends the data, and the input need not be padded to a multiple of four.
 * *outLen receives the true decoded length. Returns false (freeing and
 * NULL-ing *out) on any character outside the alphabet/whitespace/pad, or
 * a trailing group of a single leftover character (invalid base64). */
static bool b64Decode(const char *in, uint8_t **out, size_t *outLen) {
    *out = NULL;
    *outLen = 0;
    if (in == NULL) {
        return false;
    }

    size_t inLen = strlen(in);
    /* Reject an absurdly large body before allocating. A real map's
     * base64 — even with MIME line-wrap whitespace — stays well under
     * twice the decoded cap, so this can only fire on a hostile or
     * corrupt response, and it bounds both the allocation and the
     * decode loop. */
    if (inLen > (size_t)WBN_MAP_MAX_DECODED * 2) {
        return false;
    }
    /* Upper bound: 4 base64 chars yield 3 bytes; +3 covers any partial
     * trailing group and keeps the allocation non-zero for empty input.
     * Computed as (inLen/4)*3 — never inLen*3 — so the multiply cannot
     * overflow size_t on a 32-bit build; (inLen/4)*3 + 3 still bounds
     * the at-most floor(inLen*3/4) decoded bytes. */
    uint8_t *buf = malloc(inLen / 4 * 3 + 3);
    if (buf == NULL) {
        return false;
    }

    size_t outIdx = 0;
    uint32_t bits = 0;
    int nbits = 0;
    size_t dataChars = 0;
    for (size_t i = 0; i < inLen; i++) {
        char c = in[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            continue;
        }
        if (c == '=') {
            break;  /* end of data */
        }
        int dv = b64Value(c);
        if (dv < 0) {
            free(buf);
            return false;
        }
        bits = (bits << 6) | (uint32_t)dv;
        nbits += 6;
        dataChars++;
        if (nbits >= 8) {
            nbits -= 8;
            buf[outIdx++] = (uint8_t)((bits >> nbits) & 0xFF);
            bits &= ((uint32_t)1 << nbits) - 1;
        }
    }

    /* A trailing group of exactly one base64 char carries only 6 bits —
     * not enough to complete a byte and not a legal base64 tail. */
    if ((dataChars % 4) == 1) {
        free(buf);
        return false;
    }

    *out = buf;
    *outLen = outIdx;
    return true;
}

/* Copy a JSON string item into a fixed buffer, truncating to fit.
 * Non-string / NULL items leave dst as an empty string. */
static void copyStringField(const cJSON *item, char *dst, size_t dstSize) {
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        snprintf(dst, dstSize, "%s", item->valuestring);
    } else {
        dst[0] = '\0';
    }
}

bool wbnMapParseResponse(const char *json, WbnMapResult *out) {
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    if (json == NULL) {
        return false;
    }

    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        return false;
    }

    const cJSON *foundItem = cJSON_GetObjectItemCaseSensitive(root, "found");
    out->found = cJSON_IsTrue(foundItem) ? true : false;

    copyStringField(cJSON_GetObjectItemCaseSensitive(root, "name"),
                    out->name, sizeof(out->name));
    copyStringField(cJSON_GetObjectItemCaseSensitive(root, "map_md5"),
                    out->mapMd5, sizeof(out->mapMd5));

    if (out->found) {
        const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "map_data");
        if (!cJSON_IsString(data) || data->valuestring == NULL ||
            !b64Decode(data->valuestring, &out->mapData, &out->mapDataLen) ||
            out->mapDataLen > WBN_MAP_MAX_DECODED) {
            free(out->mapData);
            memset(out, 0, sizeof(*out));
            cJSON_Delete(root);
            return false;
        }
    }

    cJSON_Delete(root);
    return true;
}

bool wbnMapFetchByMd5(const char *md5Hex, WbnMapResult *out) {
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    if (md5Hex == NULL || strlen(md5Hex) != 32) {
        return false;
    }

    cJSON *req = cJSON_CreateObject();
    if (req == NULL) {
        return false;
    }
    cJSON_AddStringToObject(req, "map_md5", md5Hex);
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (body == NULL) {
        return false;
    }

    char *resp = NULL;
    int code = wbn_api_post("map", body, &resp);
    free(body);

    if (code < 200 || code >= 300) {
        free(resp);
        return false;
    }

    bool ok = wbnMapParseResponse(resp, out);
    free(resp);
    return ok && out->found;
}

void wbnMapResultFree(WbnMapResult *out) {
    if (out == NULL) {
        return;
    }
    free(out->mapData);
    memset(out, 0, sizeof(*out));
}
