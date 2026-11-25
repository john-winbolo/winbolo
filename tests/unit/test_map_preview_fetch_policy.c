/*
 * Tests for the retry rule behind the game finder's map preview
 * (map_preview_fetch_policy.h): which failed lookups are tried again, and
 * when.
 *
 * Header-only and network-free. The pool that uses the rule
 * (map_preview_fetch.cpp) needs worker threads and the HTTP module, so it
 * is not linked here.
 */

#include "test_harness.h"
#include "map_preview_fetch_policy.h"

/* A lookup that got no answer is tried again, but not before the wait is
 * over: the finder asks every frame, so without the wait a machine with no
 * connection would post continuously. It used to be never tried again,
 * which left the map without a preview until the game was restarted. */
static int unanswered_lookup_waits_then_retries(void) {
    const uint64_t failedAt = 100000;
    MapPreviewRetry r = { false, 0 };
    mapPreviewRetryNoteFailed(&r, /*answered*/ false, failedAt);
    UT_ASSERT(!mapPreviewRetryDue(&r, failedAt));
    UT_ASSERT(!mapPreviewRetryDue(&r, failedAt + 1));
    UT_ASSERT(!mapPreviewRetryDue(&r, failedAt + MAP_PREVIEW_RETRY_MS - 1));
    UT_ASSERT(mapPreviewRetryDue(&r, failedAt + MAP_PREVIEW_RETRY_MS));
    UT_ASSERT(mapPreviewRetryDue(&r, failedAt + MAP_PREVIEW_RETRY_MS + 60000));
    return 0;
}

/* "WinBolo.net does not store this map" is the answer. Asking again would
 * send a signed request every wait for as long as the row is selected. */
static int answered_lookup_never_retries(void) {
    const uint64_t failedAt = 100000;
    MapPreviewRetry r = { false, 0 };
    mapPreviewRetryNoteFailed(&r, /*answered*/ true, failedAt);
    UT_ASSERT(!mapPreviewRetryDue(&r, failedAt));
    UT_ASSERT(!mapPreviewRetryDue(&r, failedAt + MAP_PREVIEW_RETRY_MS));
    UT_ASSERT(!mapPreviewRetryDue(&r, failedAt + 100 * MAP_PREVIEW_RETRY_MS));
    return 0;
}

/* An entry that has never failed is not due: this is a map that is still
 * being looked up, or one that loaded. */
static int fresh_entry_is_not_due(void) {
    MapPreviewRetry r = { false, 0 };
    UT_ASSERT(!mapPreviewRetryDue(&r, 0));
    UT_ASSERT(!mapPreviewRetryDue(&r, 1000000));
    return 0;
}

/* A retry that fails again starts a new wait from the second failure, and
 * one that is then answered stops. */
static int second_failure_starts_a_new_wait(void) {
    MapPreviewRetry r = { false, 0 };
    mapPreviewRetryNoteFailed(&r, false, 1000);
    UT_ASSERT(mapPreviewRetryDue(&r, 1000 + MAP_PREVIEW_RETRY_MS));
    mapPreviewRetryNoteFailed(&r, false, 1000 + MAP_PREVIEW_RETRY_MS);
    UT_ASSERT(!mapPreviewRetryDue(&r, 1000 + MAP_PREVIEW_RETRY_MS + 1));
    UT_ASSERT(mapPreviewRetryDue(&r, 1000 + 2 * MAP_PREVIEW_RETRY_MS));
    mapPreviewRetryNoteFailed(&r, true, 1000 + 2 * MAP_PREVIEW_RETRY_MS);
    UT_ASSERT(!mapPreviewRetryDue(&r, 1000 + 9 * MAP_PREVIEW_RETRY_MS));
    return 0;
}

int run_map_preview_fetch_policy(void) {
    int rc;
    rc = unanswered_lookup_waits_then_retries(); if (rc) return rc;
    rc = answered_lookup_never_retries();        if (rc) return rc;
    rc = fresh_entry_is_not_due();               if (rc) return rc;
    rc = second_failure_starts_a_new_wait();     if (rc) return rc;
    return 0;
}
