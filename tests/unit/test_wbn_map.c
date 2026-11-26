/*
 * Tests for the pure cJSON parser in wbn_map.c: wbnMapParseResponse
 * (POST /api/v1/map 200 body), including its base64 decode of map_data.
 *
 * No network. wbn_api_post (in http.c) is stubbed in test_stubs.c so the
 * binary links without curl; fetch_reports_answered sets the stub's status
 * and body to check which failures wbnMapFetchByMd5 reports as WinBolo.net's
 * answer and which as a request that never got one.
 */

#include <string.h>
#include <stdlib.h>

#include "test_harness.h"
#include "wbn_map.h"

/* "Qk9MTw==" is the standard base64 of the 4 bytes B O L O. */
static int parse_found_and_decode(void) {
    const char *body =
        "{"
        "\"found\":true,"
        "\"map_id\":7,"
        "\"name\":\"Test Map\","
        "\"map_md5\":\"0123456789abcdef0123456789abcdef\","
        "\"map_data\":\"Qk9MTw==\""
        "}";
    const uint8_t expect[4] = { 0x42, 0x4f, 0x4c, 0x4f };

    WbnMapResult r;
    UT_ASSERT(wbnMapParseResponse(body, &r));
    UT_ASSERT_MSG(r.found, "found should be true");
    UT_ASSERT_MSG(strcmp(r.name, "Test Map") == 0, "name=\"%s\"", r.name);
    UT_ASSERT_MSG(strcmp(r.mapMd5, "0123456789abcdef0123456789abcdef") == 0,
                  "mapMd5=\"%s\"", r.mapMd5);
    UT_ASSERT_MSG(r.mapDataLen == 4, "mapDataLen=%zu", r.mapDataLen);
    UT_ASSERT_MSG(r.mapData != NULL, "mapData should be allocated");
    UT_ASSERT_MSG(memcmp(r.mapData, expect, 4) == 0, "decoded bytes mismatch");

    wbnMapResultFree(&r);
    UT_ASSERT_MSG(r.mapData == NULL, "free should NULL mapData");
    return 0;
}

static int parse_not_found(void) {
    WbnMapResult r;
    UT_ASSERT(wbnMapParseResponse("{\"found\":false}", &r));
    UT_ASSERT_MSG(!r.found, "found should be false");
    UT_ASSERT_MSG(r.mapData == NULL, "mapData should be NULL when not found");
    UT_ASSERT_MSG(r.mapDataLen == 0, "mapDataLen=%zu", r.mapDataLen);
    wbnMapResultFree(&r);  /* no-op, must not crash */
    return 0;
}

/* Real-world base64 encodings: MIME line wrapping (whitespace), unpadded
 * input, and the URL-safe alphabet. */
static int parse_tolerant_b64(void) {
    const uint8_t bolo[4] = { 0x42, 0x4f, 0x4c, 0x4f };
    WbnMapResult r;

    /* Newline inside the base64 (MIME line wrapping). */
    UT_ASSERT(wbnMapParseResponse(
        "{\"found\":true,\"map_data\":\"Qk9M\\nTw==\"}", &r));
    UT_ASSERT_MSG(r.mapDataLen == 4, "wrapped len=%zu", r.mapDataLen);
    UT_ASSERT_MSG(r.mapData != NULL && memcmp(r.mapData, bolo, 4) == 0,
                  "wrapped bytes mismatch");
    wbnMapResultFree(&r);

    /* Unpadded standard base64 ("Qk9MTw==" without the padding). */
    UT_ASSERT(wbnMapParseResponse(
        "{\"found\":true,\"map_data\":\"Qk9MTw\"}", &r));
    UT_ASSERT_MSG(r.mapDataLen == 4, "unpadded len=%zu", r.mapDataLen);
    UT_ASSERT_MSG(r.mapData != NULL && memcmp(r.mapData, bolo, 4) == 0,
                  "unpadded bytes mismatch");
    wbnMapResultFree(&r);

    /* URL-safe alphabet ('_' == 63): "____" -> 0xFF 0xFF 0xFF. */
    {
        const uint8_t ff[3] = { 0xff, 0xff, 0xff };
        UT_ASSERT(wbnMapParseResponse(
            "{\"found\":true,\"map_data\":\"____\"}", &r));
        UT_ASSERT_MSG(r.mapDataLen == 3, "urlsafe len=%zu", r.mapDataLen);
        UT_ASSERT_MSG(r.mapData != NULL && memcmp(r.mapData, ff, 3) == 0,
                      "urlsafe bytes mismatch");
        wbnMapResultFree(&r);
    }
    return 0;
}

/* Build a JSON body whose map_data is `b64Chars` copies of 'A' (each a
 * zero sextet, so the blob decodes to (b64Chars/4)*3 zero bytes).
 * Caller frees. NULL on OOM. */
static char *make_oversized_body(size_t b64Chars) {
    const char *pre  = "{\"found\":true,\"map_data\":\"";
    const char *post = "\"}";
    size_t preLen  = strlen(pre);
    size_t postLen = strlen(post);
    char *body = malloc(preLen + b64Chars + postLen + 1);
    if (body == NULL) {
        return NULL;
    }
    memcpy(body, pre, preLen);
    memset(body + preLen, 'A', b64Chars);
    memcpy(body + preLen + b64Chars, post, postLen + 1);
    return body;
}

/* The decoder caps a decoded map at WBN_MAP_MAX_DECODED (64 KiB): a
 * blob just over the cap is rejected, one just under decodes fine.
 * Exercises the overflow-safe allocation + size-cap hardening. */
static int parse_map_size_cap(void) {
    WbnMapResult r;

    /* 87388 base64 chars -> 65541 decoded bytes, just over the 65536 cap. */
    char *over = make_oversized_body(87388);
    UT_ASSERT_MSG(over != NULL, "alloc oversized body");
    UT_ASSERT_MSG(!wbnMapParseResponse(over, &r),
                  "oversized map must be rejected");
    UT_ASSERT_MSG(r.mapData == NULL, "rejected result must NULL mapData");
    UT_ASSERT_MSG(r.mapDataLen == 0, "rejected len=%zu", r.mapDataLen);
    free(over);

    /* 87380 base64 chars -> 65535 decoded bytes, just under the cap. */
    char *under = make_oversized_body(87380);
    UT_ASSERT_MSG(under != NULL, "alloc under-cap body");
    UT_ASSERT_MSG(wbnMapParseResponse(under, &r),
                  "under-cap map must parse");
    UT_ASSERT_MSG(r.mapDataLen == 65535, "under-cap len=%zu", r.mapDataLen);
    UT_ASSERT_MSG(r.mapData != NULL, "under-cap mapData should be allocated");
    wbnMapResultFree(&r);
    free(under);

    return 0;
}

/* The stub in test_stubs.c: status and body the next wbn_api_post returns. */
extern int         g_stubWbnApiPostCode;
extern const char *g_stubWbnApiPostBody;

/* The game finder keeps "WinBolo.net does not store this map" for the
 * session and asks again after any other failure, so answered has to be
 * false for every failure that says nothing about the map. */
static int fetch_reports_answered(void) {
    const char *md5 = "0123456789abcdef0123456789abcdef";
    WbnMapResult r;
    int rc = 0;

    /* No connection. */
    g_stubWbnApiPostCode = -1;
    g_stubWbnApiPostBody = NULL;
    if (wbnMapFetchByMd5(md5, &r) || r.answered) rc = 1;
    wbnMapResultFree(&r);

    /* Server error and rate limit. */
    g_stubWbnApiPostCode = 500;
    g_stubWbnApiPostBody = "{\"error\":\"oops\"}";
    if (wbnMapFetchByMd5(md5, &r) || r.answered) rc = 2;
    wbnMapResultFree(&r);
    g_stubWbnApiPostCode = 429;
    if (wbnMapFetchByMd5(md5, &r) || r.answered) rc = 3;
    wbnMapResultFree(&r);

    /* A 200 whose body is not JSON. */
    g_stubWbnApiPostCode = 200;
    g_stubWbnApiPostBody = "<html>";
    if (wbnMapFetchByMd5(md5, &r) || r.answered) rc = 4;
    wbnMapResultFree(&r);

    /* A request timeout from the server: it asks for a retry. */
    g_stubWbnApiPostCode = 408;
    g_stubWbnApiPostBody = "{}";
    if (wbnMapFetchByMd5(md5, &r) || r.answered) rc = 7;
    wbnMapResultFree(&r);

    /* Not stored: the answer. */
    g_stubWbnApiPostCode = 404;
    g_stubWbnApiPostBody = "{\"error\":\"Map not found\"}";
    if (wbnMapFetchByMd5(md5, &r) || !r.answered) rc = 5;
    wbnMapResultFree(&r);

    /* A signature WinBolo.net will not take gets the same reply every
     * time. These used to be asked again every wait with no limit. */
    g_stubWbnApiPostCode = 401;
    g_stubWbnApiPostBody = "{\"error\":\"bad signature\"}";
    if (wbnMapFetchByMd5(md5, &r) || !r.answered) rc = 8;
    wbnMapResultFree(&r);
    g_stubWbnApiPostCode = 403;
    if (wbnMapFetchByMd5(md5, &r) || !r.answered) rc = 9;
    wbnMapResultFree(&r);

    /* Stored, but the data is not base64: no preview, and the same reply
     * next time, so it is not asked for again either. */
    g_stubWbnApiPostCode = 200;
    g_stubWbnApiPostBody = "{\"found\":true,\"map_data\":\"!!!!\"}";
    if (wbnMapFetchByMd5(md5, &r) || !r.answered) rc = 10;
    wbnMapResultFree(&r);

    /* Stored. */
    g_stubWbnApiPostCode = 200;
    g_stubWbnApiPostBody = "{\"found\":true,\"map_data\":\"Qk9MTw==\"}";
    if (!wbnMapFetchByMd5(md5, &r) || !r.answered || r.mapDataLen != 4) rc = 6;
    wbnMapResultFree(&r);

    g_stubWbnApiPostCode = -1;
    g_stubWbnApiPostBody = NULL;
    UT_ASSERT_MSG(rc == 0, "fetch_reports_answered case %d", rc);
    return 0;
}

int run_wbn_map_parse(void) {
    int rc;
    rc = parse_found_and_decode(); if (rc) return rc;
    rc = parse_not_found();        if (rc) return rc;
    rc = parse_tolerant_b64();     if (rc) return rc;
    rc = parse_map_size_cap();     if (rc) return rc;
    rc = fetch_reports_answered(); if (rc) return rc;
    return 0;
}
