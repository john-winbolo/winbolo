/*
 * Tests for the retry rule behind the game finder's map preview
 * (map_preview_fetch_policy.h): which failed lookups are tried again,
 * where, and when.
 *
 * Header-only and network-free. The pool that uses the rule
 * (map_preview_fetch.cpp) needs worker threads and the HTTP module, so it
 * is not linked here.
 */

#include "test_harness.h"
#include "map_preview_fetch_policy.h"

#define NONE  MAP_PREVIEW_LOOKUP_NONE
#define LOCAL MAP_PREVIEW_LOOKUP_LOCAL
#define FULL  MAP_PREVIEW_LOOKUP_FULL

/* A lookup that got no answer is tried again in full, but not before the
 * wait is over: the finder asks every frame, so without the wait a machine
 * with no connection would post continuously. It used to be never tried
 * again, which left the map without a preview until the game was
 * restarted. */
static int unanswered_lookup_waits_then_retries(void) {
    const uint64_t failedAt = 100000;
    MapPreviewRetry r = { false, false, 0 };
    mapPreviewRetryNoteFailed(&r, /*answered*/ false, failedAt);
    UT_ASSERT(mapPreviewRetryDue(&r, failedAt) == NONE);
    UT_ASSERT(mapPreviewRetryDue(&r, failedAt + 1) == NONE);
    UT_ASSERT(mapPreviewRetryDue(&r, failedAt + MAP_PREVIEW_RETRY_MS - 1) == NONE);
    UT_ASSERT(mapPreviewRetryDue(&r, failedAt + MAP_PREVIEW_RETRY_MS) == FULL);
    UT_ASSERT(mapPreviewRetryDue(&r, failedAt + MAP_PREVIEW_RETRY_MS + 60000) == FULL);
    return 0;
}

/* "WinBolo.net does not store this map" is the answer: asking again would
 * send a signed request every wait for as long as the row is selected. But
 * the player can save the map or subscribe to it later in the session, so
 * this machine's files are searched again after each wait. That search was
 * never made again before, and the map stayed without a preview until the
 * game was restarted. */
static int answered_lookup_searches_this_machine_only(void) {
    const uint64_t failedAt = 100000;
    MapPreviewRetry r = { false, false, 0 };
    mapPreviewRetryNoteFailed(&r, /*answered*/ true, failedAt);
    UT_ASSERT(mapPreviewRetryDue(&r, failedAt) == NONE);
    UT_ASSERT(mapPreviewRetryDue(&r, failedAt + MAP_PREVIEW_RETRY_MS - 1) == NONE);
    UT_ASSERT(mapPreviewRetryDue(&r, failedAt + MAP_PREVIEW_RETRY_MS) == LOCAL);
    UT_ASSERT(mapPreviewRetryDue(&r, failedAt + 100 * MAP_PREVIEW_RETRY_MS) == LOCAL);
    return 0;
}

/* An entry that has never failed is not due: this is a map that is still
 * being looked up, or one that loaded. */
static int fresh_entry_is_not_due(void) {
    MapPreviewRetry r = { false, false, 0 };
    UT_ASSERT(mapPreviewRetryDue(&r, 0) == NONE);
    UT_ASSERT(mapPreviewRetryDue(&r, 1000000) == NONE);
    return 0;
}

/* A retry that fails again starts a new wait from the second failure. Once
 * WinBolo.net has answered, it is not asked again, and a search of this
 * machine that finds nothing (recorded as a failure with no answer of its
 * own) does not send the map back to WinBolo.net. */
static int second_failure_starts_a_new_wait(void) {
    MapPreviewRetry r = { false, false, 0 };
    mapPreviewRetryNoteFailed(&r, false, 1000);
    UT_ASSERT(mapPreviewRetryDue(&r, 1000 + MAP_PREVIEW_RETRY_MS) == FULL);
    mapPreviewRetryNoteFailed(&r, false, 1000 + MAP_PREVIEW_RETRY_MS);
    UT_ASSERT(mapPreviewRetryDue(&r, 1000 + MAP_PREVIEW_RETRY_MS + 1) == NONE);
    UT_ASSERT(mapPreviewRetryDue(&r, 1000 + 2 * MAP_PREVIEW_RETRY_MS) == FULL);
    mapPreviewRetryNoteFailed(&r, true, 1000 + 2 * MAP_PREVIEW_RETRY_MS);
    UT_ASSERT(mapPreviewRetryDue(&r, 1000 + 2 * MAP_PREVIEW_RETRY_MS + 1) == NONE);
    UT_ASSERT(mapPreviewRetryDue(&r, 1000 + 3 * MAP_PREVIEW_RETRY_MS) == LOCAL);
    mapPreviewRetryNoteFailed(&r, false, 1000 + 3 * MAP_PREVIEW_RETRY_MS);
    UT_ASSERT(mapPreviewRetryDue(&r, 1000 + 9 * MAP_PREVIEW_RETRY_MS) == LOCAL);
    return 0;
}

int run_map_preview_fetch_policy(void) {
    int rc;
    rc = unanswered_lookup_waits_then_retries();       if (rc) return rc;
    rc = answered_lookup_searches_this_machine_only(); if (rc) return rc;
    rc = fresh_entry_is_not_due();                     if (rc) return rc;
    rc = second_failure_starts_a_new_wait();           if (rc) return rc;
    return 0;
}
