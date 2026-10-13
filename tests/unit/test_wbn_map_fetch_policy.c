/*
 * Tests for the two pure decisions behind the WinBolo.net map listing
 * (wbn_map_fetch_policy.h): when a listing that has not arrived is asked
 * for again, and how a folder path is walked up to a folder the catalogue
 * cache knows.
 *
 * Header-only and network-free. The provider that uses them
 * (wbnMapsListProvider in wbn_map_source.cpp) needs the chooser widget,
 * the HTTP module and a worker thread, so it is not linked here.
 */

#include <string.h>

#include "test_harness.h"
#include "wbn_map_fetch_policy.h"

/* A request that differs from the last one sent goes at once, whatever
 * the clock says: this is every folder click on a first visit. */
static int new_request_goes_at_once(void) {
    UT_ASSERT(wbnMapsFetchDue(false, 0, 0));
    UT_ASSERT(wbnMapsFetchDue(false, 5000, 5000));
    UT_ASSERT(wbnMapsFetchDue(false, 5001, 5000));
    return 0;
}

/* The same request again means the first one failed. It is held back for
 * WBN_MAPS_RETRY_MS and then sent — it used to be held back for ever,
 * which left the folder (and a starred row pointing at it) empty. */
static int repeat_request_waits_then_goes(void) {
    const uint64_t kick = 100000;
    UT_ASSERT(!wbnMapsFetchDue(true, kick, kick));
    UT_ASSERT(!wbnMapsFetchDue(true, kick + 1, kick));
    UT_ASSERT(!wbnMapsFetchDue(true, kick + WBN_MAPS_RETRY_MS - 1, kick));
    UT_ASSERT(wbnMapsFetchDue(true, kick + WBN_MAPS_RETRY_MS, kick));
    UT_ASSERT(wbnMapsFetchDue(true, kick + 10 * WBN_MAPS_RETRY_MS, kick));
    return 0;
}

/* Walking "A/B/C" up one folder at a time ends at the root (length 0). */
static int parent_walk_reaches_root(void) {
    const char *p = "ClassicMap's Maps/classics-popular/small";
    size_t len = strlen(p);

    len = wbnMapsParentLen(p, len);
    UT_ASSERT_MSG(len == strlen("ClassicMap's Maps/classics-popular"),
                  "len=%zu", len);
    len = wbnMapsParentLen(p, len);
    UT_ASSERT_MSG(len == strlen("ClassicMap's Maps"), "len=%zu", len);
    len = wbnMapsParentLen(p, len);
    UT_ASSERT_MSG(len == 0, "len=%zu", len);
    /* The root is its own parent, so a walk cannot run past it. */
    len = wbnMapsParentLen(p, len);
    UT_ASSERT_MSG(len == 0, "len=%zu", len);
    return 0;
}

static int parent_of_edge_paths(void) {
    UT_ASSERT(wbnMapsParentLen("", 0) == 0);
    UT_ASSERT(wbnMapsParentLen("Top", 3) == 0);
    /* Either separator ends a folder name, as in the chooser's own
     * "[..]" row. */
    UT_ASSERT(wbnMapsParentLen("A\\B", 3) == 1);
    /* Only the first len bytes are the path. */
    UT_ASSERT(wbnMapsParentLen("A/B/C", 3) == 1);
    return 0;
}

int run_wbn_map_fetch_policy(void) {
    int rc;
    rc = new_request_goes_at_once();        if (rc) return rc;
    rc = repeat_request_waits_then_goes();  if (rc) return rc;
    rc = parent_walk_reaches_root();        if (rc) return rc;
    rc = parent_of_edge_paths();            if (rc) return rc;
    return 0;
}
