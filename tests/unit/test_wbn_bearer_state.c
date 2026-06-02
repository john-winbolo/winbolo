/*
 * Bearer-storage state machine for the WinBolo.net server bearer
 * token. wbn_bearer is hoisted into its own libcurl-free TU
 * precisely so these state-only tests can link without dragging
 * the rest of winbolonet_core/http/curl into WinBoloUnitTests.
 *
 * The refuse-to-send branch of wbn_api_call_server isn't covered
 * here: exercising it would require linking the curl path, which
 * the test binary intentionally avoids. The branch is small enough
 * to be reviewable in code.
 */

#include <string.h>
#include <stddef.h>

#include "test_harness.h"
#include "wbn_bearer.h"

static int bearer_set_get_roundtrip(void) {
    httpClearServerBearerToken();
    httpSetServerBearerToken("abc123");
    char buf[WBN_SERVER_TOKEN_LEN];
    memset(buf, 0xAA, sizeof(buf));
    winboloNetGetServerToken(buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "abc123") == 0,
                  "expected \"abc123\", got \"%s\"", buf);
    return 0;
}

static int bearer_clear_zeros(void) {
    httpSetServerBearerToken("abc123");
    httpClearServerBearerToken();
    char buf[WBN_SERVER_TOKEN_LEN];
    memset(buf, 0xAA, sizeof(buf));
    winboloNetGetServerToken(buf, sizeof(buf));
    UT_ASSERT_MSG(buf[0] == '\0',
                  "expected empty string after clear, got \"%s\"", buf);
    return 0;
}

static int bearer_null_clears(void) {
    httpSetServerBearerToken("abc123");
    httpSetServerBearerToken(NULL);
    char buf[WBN_SERVER_TOKEN_LEN];
    memset(buf, 0xAA, sizeof(buf));
    winboloNetGetServerToken(buf, sizeof(buf));
    UT_ASSERT_MSG(buf[0] == '\0',
                  "NULL setter should clear, got \"%s\"", buf);
    return 0;
}

static int bearer_empty_clears(void) {
    httpSetServerBearerToken("abc123");
    httpSetServerBearerToken("");
    char buf[WBN_SERVER_TOKEN_LEN];
    memset(buf, 0xAA, sizeof(buf));
    winboloNetGetServerToken(buf, sizeof(buf));
    UT_ASSERT_MSG(buf[0] == '\0',
                  "empty-string setter should clear, got \"%s\"", buf);
    return 0;
}

static int bearer_idempotent_reset(void) {
    httpClearServerBearerToken();
    httpSetServerBearerToken("x");
    httpSetServerBearerToken("y");
    char buf[WBN_SERVER_TOKEN_LEN];
    memset(buf, 0xAA, sizeof(buf));
    winboloNetGetServerToken(buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "y") == 0,
                  "expected \"y\", got \"%s\"", buf);
    return 0;
}

static int bearer_getter_truncates_safely(void) {
    /* 64-char hex token (max real-world size). */
    const char *full = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    httpSetServerBearerToken(full);
    char buf[10];
    memset(buf, 0xAA, sizeof(buf));
    winboloNetGetServerToken(buf, sizeof(buf));
    UT_ASSERT_MSG(buf[9] == '\0',
                  "getter did not NUL-terminate the truncated buffer");
    UT_ASSERT_MSG(strncmp(buf, full, 9) == 0,
                  "truncated prefix wrong: \"%s\"", buf);
    return 0;
}

static int bearer_getter_zero_outsize(void) {
    httpSetServerBearerToken("abc123");
    /* outSize 0 must not crash and must not touch the buffer. */
    char sentinel = (char)0xAA;
    winboloNetGetServerToken(&sentinel, 0);
    UT_ASSERT_MSG(sentinel == (char)0xAA,
                  "outSize=0 should not write to the buffer");
    return 0;
}

int run_wbn_bearer_state(void) {
    int rc;
    rc = bearer_set_get_roundtrip();        if (rc) return rc;
    rc = bearer_clear_zeros();              if (rc) return rc;
    rc = bearer_null_clears();              if (rc) return rc;
    rc = bearer_empty_clears();             if (rc) return rc;
    rc = bearer_idempotent_reset();         if (rc) return rc;
    rc = bearer_getter_truncates_safely();  if (rc) return rc;
    rc = bearer_getter_zero_outsize();      if (rc) return rc;
    return 0;
}
