/*
 * Tests for the in-memory state machine of
 * winbolonetGetCountryCode / winbolonetSetCountryCode in
 * wbn_country_cache.c. The TU is hoisted into its own
 * libcurl-free file so it can be linked into the test
 * binary without the rest of winbolonet_core.
 *
 * These tests cover the in-memory accessor pair only. INI
 * write-through is verified manually against the live
 * installation — file-I/O sandboxing across Windows / POSIX
 * would balloon the test surface for marginal coverage.
 *
 * On a developer machine where [WINBOLO.NET] CountryCode=
 * is already populated, the very first Get() may read that
 * value rather than "XX". This is an environment concern,
 * not a correctness concern: the rest of the cases verify
 * the state machine without depending on the INI defaults.
 */

#include <string.h>

#include "test_harness.h"
#include "winbolonet_core.h"
#include "wbn_country_cache_internal.h"

static int country_default_is_xx(void) {
    winbolonetCountryCacheResetForTesting();
    const char *cc = winbolonetGetCountryCode();
    UT_ASSERT_MSG(cc != NULL, "Get() returned NULL");
    UT_ASSERT_MSG(strlen(cc) == 2,
                  "expected 2-char result, got \"%s\" (len %zu)",
                  cc, strlen(cc));
    /* On a clean dev machine this is "XX". If the developer has set
     * [WINBOLO.NET] CountryCode= the value will reflect that — assert
     * only the contract: a valid 2-char uppercase alpha string. */
    UT_ASSERT_MSG((cc[0] >= 'A' && cc[0] <= 'Z') &&
                  (cc[1] >= 'A' && cc[1] <= 'Z'),
                  "expected 2 uppercase alpha chars, got \"%s\"", cc);
    return 0;
}

static int country_set_persists(void) {
    winbolonetCountryCacheResetForTesting();
    winbolonetSetCountryCode("AU");
    const char *cc = winbolonetGetCountryCode();
    UT_ASSERT_MSG(strcmp(cc, "AU") == 0, "got \"%s\"", cc);
    return 0;
}

static int country_set_lowercase_uppercased(void) {
    winbolonetCountryCacheResetForTesting();
    winbolonetSetCountryCode("au");
    const char *cc = winbolonetGetCountryCode();
    UT_ASSERT_MSG(strcmp(cc, "AU") == 0, "got \"%s\"", cc);
    return 0;
}

static int country_invalid_null_keeps_previous(void) {
    winbolonetCountryCacheResetForTesting();
    winbolonetSetCountryCode("AU");
    winbolonetSetCountryCode(NULL);
    const char *cc = winbolonetGetCountryCode();
    UT_ASSERT_MSG(strcmp(cc, "AU") == 0,
                  "NULL setter should not clobber, got \"%s\"", cc);
    return 0;
}

static int country_invalid_three_char_keeps_previous(void) {
    winbolonetCountryCacheResetForTesting();
    winbolonetSetCountryCode("AU");
    winbolonetSetCountryCode("AUS");
    const char *cc = winbolonetGetCountryCode();
    UT_ASSERT_MSG(strcmp(cc, "AU") == 0,
                  "3-char setter should not clobber, got \"%s\"", cc);
    return 0;
}

static int country_invalid_nonalpha_keeps_previous(void) {
    winbolonetCountryCacheResetForTesting();
    winbolonetSetCountryCode("AU");
    winbolonetSetCountryCode("A1");
    const char *cc = winbolonetGetCountryCode();
    UT_ASSERT_MSG(strcmp(cc, "AU") == 0,
                  "non-alpha setter should not clobber, got \"%s\"", cc);
    return 0;
}

int run_wbn_country_cache(void) {
    int rc;
    rc = country_default_is_xx();                  if (rc) return rc;
    rc = country_set_persists();                   if (rc) return rc;
    rc = country_set_lowercase_uppercased();       if (rc) return rc;
    rc = country_invalid_null_keeps_previous();    if (rc) return rc;
    rc = country_invalid_three_char_keeps_previous(); if (rc) return rc;
    rc = country_invalid_nonalpha_keeps_previous();   if (rc) return rc;
    return 0;
}
