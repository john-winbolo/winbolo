/*
 * Tests for the pure cJSON parsers in wbn_prefs_sync.c:
 * wbnPrefsParseGet (GET /api/v1/prefs 200 body) and
 * wbnPrefsParseUpdatedAt ({updatedAt} from PUT 200/409).
 *
 * Parser-only: no network. The module is libcurl-free, so it
 * links into the test binary without dragging in http.c.
 */

#include <string.h>
#include <stdlib.h>

#include "test_harness.h"
#include "wbn_prefs_sync.h"

static int parse_get_full_body(void) {
    const char *body =
        "{"
        "\"updatedAt\":\"0123456789abcdef0123456789abcdef\","
        "\"device\":{\"type\":\"Steam Deck\"},"
        "\"prefs\":{\"Sound\":\"on\",\"Volume\":7}"
        "}";
    WbnPrefsGetResult r;
    int rc = wbnPrefsParseGet(body, &r);
    UT_ASSERT_MSG(rc == 0, "expected success, got %d", rc);
    UT_ASSERT_MSG(strcmp(r.updatedAt, "0123456789abcdef0123456789abcdef") == 0,
                  "updatedAt=\"%s\"", r.updatedAt);
    UT_ASSERT_MSG(strcmp(r.deviceType, "Steam Deck") == 0,
                  "deviceType=\"%s\"", r.deviceType);
    UT_ASSERT_MSG(r.prefs != NULL, "prefs not populated");
    /* prefs round-trips a known key */
    UT_ASSERT_MSG(strstr(r.prefs, "\"Sound\"") != NULL,
                  "prefs missing Sound key: %s", r.prefs);
    UT_ASSERT_MSG(strstr(r.prefs, "\"Volume\"") != NULL,
                  "prefs missing Volume key: %s", r.prefs);
    free(r.prefs);
    return 0;
}

static int parse_get_null_device(void) {
    const char *body =
        "{"
        "\"updatedAt\":\"0123456789abcdef0123456789abcdef\","
        "\"device\":{\"type\":null},"
        "\"prefs\":{\"Sound\":\"on\"}"
        "}";
    WbnPrefsGetResult r;
    int rc = wbnPrefsParseGet(body, &r);
    UT_ASSERT_MSG(rc == 0, "expected success, got %d", rc);
    UT_ASSERT_MSG(r.deviceType[0] == '\0', "deviceType should be empty, got \"%s\"", r.deviceType);
    UT_ASSERT_MSG(r.prefs != NULL, "prefs not populated");
    free(r.prefs);
    return 0;
}

static int parse_get_rejects_malformed(void) {
    WbnPrefsGetResult r;
    /* Not JSON at all (404-style plain text). */
    UT_ASSERT(wbnPrefsParseGet("Not Found", &r) != 0);
    UT_ASSERT_MSG(r.prefs == NULL, "prefs should be NULL on failure");
    /* Valid JSON object but no prefs object. */
    UT_ASSERT(wbnPrefsParseGet("{\"updatedAt\":\"0123456789abcdef0123456789abcdef\"}", &r) != 0);
    UT_ASSERT_MSG(r.prefs == NULL, "prefs should be NULL on failure");
    /* NULL inputs. */
    UT_ASSERT(wbnPrefsParseGet(NULL, &r) != 0);
    return 0;
}

int run_wbn_prefs_parse_get(void) {
    int rc;
    rc = parse_get_full_body();          if (rc) return rc;
    rc = parse_get_null_device();        if (rc) return rc;
    rc = parse_get_rejects_malformed();  if (rc) return rc;
    return 0;
}

static int parse_updatedat_token(void) {
    char out[33];
    int rc = wbnPrefsParseUpdatedAt(
        "{\"updatedAt\":\"0123456789abcdef0123456789abcdef\"}", out);
    UT_ASSERT_MSG(rc == 0, "expected success, got %d", rc);
    UT_ASSERT_MSG(strcmp(out, "0123456789abcdef0123456789abcdef") == 0,
                  "out=\"%s\"", out);
    return 0;
}

static int parse_updatedat_null(void) {
    char out[33];
    int rc = wbnPrefsParseUpdatedAt("{\"updatedAt\":null}", out);
    UT_ASSERT_MSG(rc == 0, "null updatedAt should still succeed, got %d", rc);
    UT_ASSERT_MSG(out[0] == '\0', "out should be empty, got \"%s\"", out);

    /* Absent updatedAt -> empty + success. */
    rc = wbnPrefsParseUpdatedAt("{}", out);
    UT_ASSERT_MSG(rc == 0, "absent updatedAt should still succeed, got %d", rc);
    UT_ASSERT_MSG(out[0] == '\0', "out should be empty, got \"%s\"", out);
    return 0;
}

static int parse_updatedat_rejects_malformed(void) {
    char out[33];
    UT_ASSERT(wbnPrefsParseUpdatedAt("not json", out) != 0);
    UT_ASSERT(wbnPrefsParseUpdatedAt(NULL, out) != 0);
    return 0;
}

int run_wbn_prefs_parse_updatedat(void) {
    int rc;
    rc = parse_updatedat_token();            if (rc) return rc;
    rc = parse_updatedat_null();             if (rc) return rc;
    rc = parse_updatedat_rejects_malformed(); if (rc) return rc;
    return 0;
}

/* -------------------------------------------------------------------- */

#define TOK_A "0123456789abcdef0123456789abcdef"
#define TOK_B "fedcba9876543210fedcba9876543210"

static int decide_404_seeds(void) {
    WbnSyncAction a = wbnPrefsDecideAfterGet(false, "", 404, "");
    UT_ASSERT_MSG(a.kind == WBN_SYNC_PUT_LOCAL, "404 should PUT, got %d", a.kind);
    UT_ASSERT_MSG(a.baseUpdatedAt[0] == '\0', "404 base should be empty (null)");
    UT_ASSERT_MSG(!a.isConflict, "404 seed is not a conflict");
    /* localDirty state is irrelevant to the 404 seed path. */
    a = wbnPrefsDecideAfterGet(true, TOK_A, 404, "");
    UT_ASSERT_MSG(a.kind == WBN_SYNC_PUT_LOCAL, "404 should PUT regardless of dirty");
    UT_ASSERT_MSG(a.baseUpdatedAt[0] == '\0', "404 base should be empty (null)");
    return 0;
}

static int decide_200_not_dirty_adopts(void) {
    WbnSyncAction a = wbnPrefsDecideAfterGet(false, TOK_A, 200, TOK_B);
    UT_ASSERT_MSG(a.kind == WBN_SYNC_ADOPT_SERVER,
                  "200 not-dirty should adopt, got %d", a.kind);
    UT_ASSERT_MSG(!a.isConflict, "adopt is not a conflict");
    return 0;
}

static int decide_200_dirty_equal_puts(void) {
    WbnSyncAction a = wbnPrefsDecideAfterGet(true, TOK_A, 200, TOK_A);
    UT_ASSERT_MSG(a.kind == WBN_SYNC_PUT_LOCAL,
                  "200 dirty equal should PUT, got %d", a.kind);
    UT_ASSERT_MSG(strcmp(a.baseUpdatedAt, TOK_A) == 0,
                  "base should be lastSynced, got '%s'", a.baseUpdatedAt);
    UT_ASSERT_MSG(!a.isConflict, "equal tokens is not a conflict");
    return 0;
}

static int decide_200_dirty_differ_conflicts(void) {
    WbnSyncAction a = wbnPrefsDecideAfterGet(true, TOK_A, 200, TOK_B);
    UT_ASSERT_MSG(a.kind == WBN_SYNC_PUT_LOCAL,
                  "200 dirty differing should PUT, got %d", a.kind);
    UT_ASSERT_MSG(strcmp(a.baseUpdatedAt, TOK_B) == 0,
                  "base should be server token, got '%s'", a.baseUpdatedAt);
    UT_ASSERT_MSG(a.isConflict, "differing tokens is a conflict");
    return 0;
}

static int decide_401_reauths(void) {
    WbnSyncAction a = wbnPrefsDecideAfterGet(true, TOK_A, 401, "");
    UT_ASSERT_MSG(a.kind == WBN_SYNC_REAUTH, "401 should reauth, got %d", a.kind);
    return 0;
}

static int decide_other_noops(void) {
    WbnSyncAction a = wbnPrefsDecideAfterGet(true, TOK_A, -1, "");
    UT_ASSERT_MSG(a.kind == WBN_SYNC_NOOP, "-1 should noop, got %d", a.kind);
    a = wbnPrefsDecideAfterGet(true, TOK_A, 429, "");
    UT_ASSERT_MSG(a.kind == WBN_SYNC_NOOP, "429 should noop, got %d", a.kind);
    a = wbnPrefsDecideAfterGet(false, TOK_A, 500, "");
    UT_ASSERT_MSG(a.kind == WBN_SYNC_NOOP, "500 should noop, got %d", a.kind);
    return 0;
}

int run_wbn_prefs_decide(void) {
    int rc;
    rc = decide_404_seeds();               if (rc) return rc;
    rc = decide_200_not_dirty_adopts();    if (rc) return rc;
    rc = decide_200_dirty_equal_puts();    if (rc) return rc;
    rc = decide_200_dirty_differ_conflicts(); if (rc) return rc;
    rc = decide_401_reauths();             if (rc) return rc;
    rc = decide_other_noops();             if (rc) return rc;
    return 0;
}

/* -------------------------------------------------------------------- */

static int build_put_body_null_base(void) {
    /* Empty base serializes to JSON null (not "" or an omitted key). */
    char *body = wbnPrefsBuildPutBody("", "Steam Deck", "{\"Sound\":\"on\"}");
    UT_ASSERT_MSG(body != NULL, "expected a body");
    UT_ASSERT_MSG(strstr(body, "\"baseUpdatedAt\":null") != NULL,
                  "empty base should be JSON null, got %s", body);
    free(body);
    /* NULL base behaves the same as "". */
    body = wbnPrefsBuildPutBody(NULL, "Steam Deck", "{\"Sound\":\"on\"}");
    UT_ASSERT_MSG(body != NULL, "expected a body");
    UT_ASSERT_MSG(strstr(body, "\"baseUpdatedAt\":null") != NULL,
                  "NULL base should be JSON null, got %s", body);
    free(body);
    return 0;
}

static int build_put_body_embeds_object(void) {
    char *body = wbnPrefsBuildPutBody(TOK_A, "Steam Deck",
                                      "{\"Sound\":\"on\",\"Volume\":7}");
    UT_ASSERT_MSG(body != NULL, "expected a body");
    UT_ASSERT_MSG(strstr(body, "\"baseUpdatedAt\":\"" TOK_A "\"") != NULL,
                  "base token missing: %s", body);
    /* device embedded as an object carrying the platform type. */
    UT_ASSERT_MSG(strstr(body, "\"device\":{") != NULL, "device object missing: %s", body);
    UT_ASSERT_MSG(strstr(body, "\"type\":\"Steam Deck\"") != NULL,
                  "device type missing: %s", body);
    /* prefs embedded as a nested object, not a quoted string. */
    UT_ASSERT_MSG(strstr(body, "\"prefs\":{") != NULL,
                  "prefs not embedded as object: %s", body);
    UT_ASSERT_MSG(strstr(body, "\"Volume\":7") != NULL, "prefs body missing: %s", body);
    free(body);
    return 0;
}

static int build_put_body_rejects_bad_prefs(void) {
    /* Unparseable / missing prefs -> NULL (the caller treats it as a no-op). */
    UT_ASSERT(wbnPrefsBuildPutBody("", "t", "not json") == NULL);
    UT_ASSERT(wbnPrefsBuildPutBody("", "t", NULL) == NULL);
    return 0;
}

int run_wbn_prefs_build_put_body(void) {
    int rc;
    rc = build_put_body_null_base();         if (rc) return rc;
    rc = build_put_body_embeds_object();     if (rc) return rc;
    rc = build_put_body_rejects_bad_prefs(); if (rc) return rc;
    return 0;
}
