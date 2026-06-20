/*
 * Tests for the pure cJSON parser in wbn_map.c: wbnMapParseResponse
 * (POST /api/v1/map 200 body), including its base64 decode of map_data.
 *
 * Parser-only: no network. The module references wbn_api_post (in
 * http.c) only from wbnMapFetchByMd5, which is stubbed in test_stubs.c
 * so the binary links without curl. wbnMapFetchByMd5 itself is
 * validated live, not here.
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

int run_wbn_map_parse(void) {
    int rc;
    rc = parse_found_and_decode(); if (rc) return rc;
    rc = parse_not_found();        if (rc) return rc;
    return 0;
}
