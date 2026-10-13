/*
 * Tests for the pure decisions behind the WinBolo.net map listing
 * (wbn_map_fetch_policy.h): when a listing that has not arrived is asked
 * for again, how a folder path is walked up to a folder the catalogue
 * cache knows, and when the chooser is moved onto the folder that loaded.
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
    WbnMapsRetry r = { 0, 0 };
    UT_ASSERT(wbnMapsFetchDue(&r, false, 0));
    r.sends = 1; r.lastKickMs = 5000;
    UT_ASSERT(wbnMapsFetchDue(&r, false, 5000));
    UT_ASSERT(wbnMapsFetchDue(&r, false, 5001));
    /* Even when the previous request had been given up on. */
    r.sends = WBN_MAPS_RETRY_MAX_SENDS;
    UT_ASSERT(wbnMapsFetchDue(&r, false, 5001));
    return 0;
}

/* The same request again means the first one failed. It is held back for
 * WBN_MAPS_RETRY_MS and then sent — it used to be held back for ever,
 * which left the folder (and a starred row pointing at it) empty. */
static int repeat_request_waits_then_goes(void) {
    const uint64_t kick = 100000;
    WbnMapsRetry r = { 0, 0 };
    wbnMapsRetryNoteSent(&r, false, kick);
    UT_ASSERT(!wbnMapsFetchDue(&r, true, kick));
    UT_ASSERT(!wbnMapsFetchDue(&r, true, kick + 1));
    UT_ASSERT(!wbnMapsFetchDue(&r, true, kick + WBN_MAPS_RETRY_MS - 1));
    UT_ASSERT(wbnMapsFetchDue(&r, true, kick + WBN_MAPS_RETRY_MS));
    UT_ASSERT(wbnMapsFetchDue(&r, true, kick + 10 * WBN_MAPS_RETRY_MS));
    return 0;
}

/* The wait doubles with each send of the same request and stops growing
 * at the cap. */
static int retry_wait_doubles_to_the_cap(void) {
    UT_ASSERT(WBN_MAPS_RETRY_MS == 3000u);
    UT_ASSERT(WBN_MAPS_RETRY_MAX_MS == 60000u);
    UT_ASSERT(WBN_MAPS_RETRY_MAX_SENDS == 6u);
    UT_ASSERT(wbnMapsRetryWaitMs(1) == 3000u);
    UT_ASSERT(wbnMapsRetryWaitMs(2) == 6000u);
    UT_ASSERT(wbnMapsRetryWaitMs(3) == 12000u);
    UT_ASSERT(wbnMapsRetryWaitMs(4) == 24000u);
    UT_ASSERT(wbnMapsRetryWaitMs(5) == 48000u);
    UT_ASSERT(wbnMapsRetryWaitMs(6) == 60000u);
    UT_ASSERT(wbnMapsRetryWaitMs(7) == 60000u);
    UT_ASSERT(wbnMapsRetryWaitMs(1000) == 60000u);
    /* Nothing sent yet reads as the first wait, not zero. */
    UT_ASSERT(wbnMapsRetryWaitMs(0) == 3000u);
    return 0;
}

/* Drives a request that always fails, one provider frame per 100 ms of
 * a made-up clock, and writes down when it was sent. Returns the number
 * of sends. */
static int runFailingRequest(WbnMapsRetry *r, uint64_t startMs,
                             uint64_t runMs, uint64_t *sentAt, int maxSent) {
    int n = 0;
    uint64_t t;
    for (t = startMs; t <= startMs + runMs; t += 100) {
        bool same = (n > 0);
        if (wbnMapsFetchDue(r, same, t)) {
            wbnMapsRetryNoteSent(r, same, t);
            if (n < maxSent) sentAt[n] = t - startMs;
            n++;
        }
    }
    return n;
}

/* A folder that fails every time is sent at 0, 3, 9, 21, 45 and 93 s
 * (waits of 3, 6, 12, 24 and 48 s) and then never again. With a fixed
 * 3 s wait and no limit it was sent for as long as the tab was open:
 * 201 times in the ten minutes run here. */
static int failing_request_backs_off_and_stops(void) {
    static const uint64_t kExpect[] = { 0, 3000, 9000, 21000, 45000, 93000 };
    uint64_t sentAt[16];
    WbnMapsRetry r = { 0, 0 };
    int n, i;

    memset(sentAt, 0, sizeof(sentAt));
    n = runFailingRequest(&r, 50000, 600000, sentAt, 16);
    UT_ASSERT_MSG(n == (int)WBN_MAPS_RETRY_MAX_SENDS, "sends=%d", n);
    for (i = 0; i < n; i++) {
        UT_ASSERT_MSG(sentAt[i] == kExpect[i], "send %d at %llu ms", i,
                      (unsigned long long)sentAt[i]);
    }
    UT_ASSERT(wbnMapsRetriesStopped(&r, true));
    UT_ASSERT(!wbnMapsFetchDue(&r, true, 50000 + 3600000));
    /* Stopped is about this request only. */
    UT_ASSERT(!wbnMapsRetriesStopped(&r, false));
    return 0;
}

/* The count starts again on an answer, on a different request and when
 * the chooser is reopened (the provider resets on all three). */
static int retry_count_starts_again(void) {
    uint64_t sentAt[16];
    WbnMapsRetry r = { 0, 0 };
    int n;

    /* Not stopped while sends remain. */
    wbnMapsRetryNoteSent(&r, false, 1000);
    wbnMapsRetryNoteSent(&r, true, 4000);
    UT_ASSERT(r.sends == 2);
    UT_ASSERT(!wbnMapsRetriesStopped(&r, true));
    UT_ASSERT(!wbnMapsFetchDue(&r, true, 4000 + 5999));
    UT_ASSERT(wbnMapsFetchDue(&r, true, 4000 + 6000));

    /* An answer: the next failure of that folder waits 3 s again. */
    wbnMapsRetryReset(&r);
    UT_ASSERT(r.sends == 0);
    wbnMapsRetryNoteSent(&r, false, 20000);
    UT_ASSERT(r.sends == 1);
    UT_ASSERT(wbnMapsFetchDue(&r, true, 23000));

    /* Given up, then reset: the full run of sends is there again. */
    n = runFailingRequest(&r, 100000, 600000, sentAt, 16);
    UT_ASSERT(wbnMapsRetriesStopped(&r, true));
    wbnMapsRetryReset(&r);
    UT_ASSERT(!wbnMapsRetriesStopped(&r, true));
    n = runFailingRequest(&r, 1000000, 600000, sentAt, 16);
    UT_ASSERT_MSG(n == (int)WBN_MAPS_RETRY_MAX_SENDS, "sends=%d", n);

    /* Given up, then a different request: it goes at once and counts
     * from one. */
    UT_ASSERT(wbnMapsFetchDue(&r, false, 2000000));
    wbnMapsRetryNoteSent(&r, false, 2000000);
    UT_ASSERT(r.sends == 1);
    UT_ASSERT(!wbnMapsRetriesStopped(&r, true));
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

/* ── A stand-in for the provider's folder branch ─────────────────────
 * The catalogue below is what the server holds. A Sim starts knowing
 * none of it, as a fresh process does, and simStep makes the same
 * decisions wbnMapsListProvider makes each frame with the same policy
 * functions: resolve the wanted path to a folder id, move onto the
 * cached folder when that is the rule, otherwise fetch. A fetch answers
 * at once and teaches the table the folder and its subfolders, as
 * wbnMapsParseFolderJson does. */
typedef struct { const char *path; int id; } SimFolder;

static const SimFolder kCatalogue[] = {
    { "",       0 },
    { "A",     10 },
    { "A/B",   20 },
    { "A/B/C", 30 },
    { "Z",     40 },
};
#define SIM_CATALOGUE_N (sizeof(kCatalogue) / sizeof(kCatalogue[0]))

typedef struct {
    bool        known[SIM_CATALOGUE_N]; /* the path to id table */
    bool        hasData;
    const char *cachedPath;
    int         cachedId;
    const char *wanted;                 /* the chooser's currentDir */
    int         fetches;
    int         lastFetchedId;
} Sim;

static bool simLookup(void *ctx, const char *path, size_t len, int *idOut) {
    const Sim *sim = (const Sim *)ctx;
    size_t i;
    for (i = 0; i < SIM_CATALOGUE_N; i++) {
        if (sim->known[i] && strlen(kCatalogue[i].path) == len &&
            memcmp(kCatalogue[i].path, path, len) == 0) {
            *idOut = kCatalogue[i].id;
            return true;
        }
    }
    return false;
}

static void simFetch(Sim *sim, int id) {
    size_t i, j;
    sim->fetches++;
    sim->lastFetchedId = id;
    for (i = 0; i < SIM_CATALOGUE_N; i++) {
        const char *p = kCatalogue[i].path;
        size_t plen = strlen(p);
        if (kCatalogue[i].id != id) continue;
        sim->hasData    = true;
        sim->cachedPath = p;
        sim->cachedId   = id;
        sim->known[i]   = true;
        for (j = 0; j < SIM_CATALOGUE_N; j++) {
            const char *c = kCatalogue[j].path;
            size_t clen = strlen(c);
            if (clen > 0 && wbnMapsParentLen(c, clen) == plen &&
                memcmp(c, p, plen) == 0 && (plen == 0 || c[plen] == '/')) {
                sim->known[j] = true;
            }
        }
        return;
    }
}

/* One provider frame. Returns true when the list for wanted is shown. */
static bool simStep(Sim *sim) {
    int target = wbnMapsResolveTarget(sim->wanted, simLookup, sim);
    if (sim->hasData && strcmp(sim->cachedPath, sim->wanted) == 0) return true;
    if (sim->hasData &&
        wbnMapsRepointDue(sim->wanted[0] == '\0', sim->cachedPath[0] == '\0',
                          sim->cachedId, target)) {
        sim->wanted = sim->cachedPath;
        return true;
    }
    simFetch(sim, target);
    return false;
}

static void simInit(Sim *sim, const char *wanted) {
    memset(sim, 0, sizeof(*sim));
    sim->cachedPath = "";
    sim->wanted     = wanted;
}

/* A starred folder two levels down, opened in a fresh process: nothing
 * is known, so the root is asked for, then one folder per fetch until
 * the starred one is listed. The table lookup used to fall back to the
 * root whenever the full path was unknown, so the second fetch asked
 * for the root again and the folder never loaded. */
static int starred_two_levels_resolves_one_per_fetch(void) {
    Sim sim;
    simInit(&sim, "A/B");
    UT_ASSERT(!simStep(&sim));
    UT_ASSERT_MSG(sim.lastFetchedId == 0, "id=%d", sim.lastFetchedId);
    UT_ASSERT(!simStep(&sim));
    UT_ASSERT_MSG(sim.lastFetchedId == 10, "id=%d", sim.lastFetchedId);
    UT_ASSERT(!simStep(&sim));
    UT_ASSERT_MSG(sim.lastFetchedId == 20, "id=%d", sim.lastFetchedId);
    UT_ASSERT(simStep(&sim));
    UT_ASSERT_MSG(sim.fetches == 3, "fetches=%d", sim.fetches);
    UT_ASSERT(strcmp(sim.cachedPath, "A/B") == 0);
    UT_ASSERT(strcmp(sim.wanted, "A/B") == 0);
    return 0;
}

static int starred_three_levels_resolves_one_per_fetch(void) {
    static const int kExpect[] = { 0, 10, 20, 30 };
    Sim sim;
    size_t i;
    simInit(&sim, "A/B/C");
    for (i = 0; i < sizeof(kExpect) / sizeof(kExpect[0]); i++) {
        UT_ASSERT(!simStep(&sim));
        UT_ASSERT_MSG(sim.lastFetchedId == kExpect[i], "step %zu id=%d",
                      i, sim.lastFetchedId);
    }
    UT_ASSERT(simStep(&sim));
    UT_ASSERT_MSG(sim.fetches == 4, "fetches=%d", sim.fetches);
    UT_ASSERT(strcmp(sim.cachedPath, "A/B/C") == 0);
    UT_ASSERT(strcmp(sim.wanted, "A/B/C") == 0);
    return 0;
}

/* A path the catalogue does not have (the starred folder was deleted or
 * renamed) ends on the nearest folder that does exist, with a list
 * shown, and nothing more is fetched after that. */
static int unknown_path_ends_on_nearest_folder(void) {
    Sim sim;
    int i;
    simInit(&sim, "A/B/Gone/Deeper");
    for (i = 0; i < 10 && !simStep(&sim); i++) {}
    UT_ASSERT_MSG(strcmp(sim.wanted, "A/B") == 0, "wanted='%s'", sim.wanted);
    UT_ASSERT_MSG(sim.fetches == 3, "fetches=%d", sim.fetches);
    UT_ASSERT(simStep(&sim));
    UT_ASSERT_MSG(sim.fetches == 3, "fetches=%d", sim.fetches);

    /* Unknown at the top level: the nearest folder is the root. */
    simInit(&sim, "Gone");
    for (i = 0; i < 10 && !simStep(&sim); i++) {}
    UT_ASSERT_MSG(sim.wanted[0] == '\0', "wanted='%s'", sim.wanted);
    UT_ASSERT_MSG(sim.fetches == 1, "fetches=%d", sim.fetches);
    return 0;
}

/* The walk itself, against a table that knows only part of the path. */
static int resolve_stops_at_deepest_known(void) {
    Sim sim;
    simInit(&sim, "");
    UT_ASSERT(wbnMapsResolveTarget("A/B/C", simLookup, &sim) == 0);
    UT_ASSERT(wbnMapsResolveTarget("", simLookup, &sim) == 0);
    sim.known[1] = true; /* "A" */
    UT_ASSERT(wbnMapsResolveTarget("A/B/C", simLookup, &sim) == 10);
    UT_ASSERT(wbnMapsResolveTarget("A", simLookup, &sim) == 10);
    /* "Ab" is not under "A". */
    UT_ASSERT(wbnMapsResolveTarget("Ab/C", simLookup, &sim) == 0);
    sim.known[3] = true; /* "A/B/C", with "A/B" still unknown */
    UT_ASSERT(wbnMapsResolveTarget("A/B/C", simLookup, &sim) == 30);
    UT_ASSERT(wbnMapsResolveTarget("A/B", simLookup, &sim) == 10);
    return 0;
}

/* The chooser is never moved off the root, and a cached folder that
 * came with no id (so it reads as 0, the root's id) is not taken for
 * the root. Either would copy a folder path over the root path, and
 * "[..]" and the "Maps" crumb could then never reach the root. */
static int repoint_never_traps_the_root(void) {
    Sim sim;

    /* Wanted the root; the cache holds folder "A" that came with no id. */
    UT_ASSERT(!wbnMapsRepointDue(true, false, 0, 0));
    /* Wanted an unknown path that resolves to the root; same cache. */
    UT_ASSERT(!wbnMapsRepointDue(false, false, 0, 0));
    /* Wanted the root, cache holds a folder with a real id. */
    UT_ASSERT(!wbnMapsRepointDue(true, false, 10, 0));
    /* The rule itself: same folder under another path, and not that
     * folder. */
    UT_ASSERT(wbnMapsRepointDue(false, false, 20, 20));
    UT_ASSERT(wbnMapsRepointDue(false, true, 0, 0));
    UT_ASSERT(!wbnMapsRepointDue(false, false, 20, 10));

    /* End to end: at the root with that id-less "A" in the cache, the
     * root is fetched and shown; wanted stays the root. */
    simInit(&sim, "");
    sim.hasData    = true;
    sim.cachedPath = "A";
    sim.cachedId   = 0;
    UT_ASSERT(!simStep(&sim));
    UT_ASSERT_MSG(sim.wanted[0] == '\0', "wanted='%s'", sim.wanted);
    UT_ASSERT_MSG(sim.fetches == 1 && sim.lastFetchedId == 0,
                  "fetches=%d id=%d", sim.fetches, sim.lastFetchedId);
    UT_ASSERT(simStep(&sim));
    UT_ASSERT(sim.wanted[0] == '\0' && sim.cachedPath[0] == '\0');
    return 0;
}

int run_wbn_map_fetch_policy(void) {
    int rc;
    rc = new_request_goes_at_once();        if (rc) return rc;
    rc = repeat_request_waits_then_goes();  if (rc) return rc;
    rc = retry_wait_doubles_to_the_cap();        if (rc) return rc;
    rc = failing_request_backs_off_and_stops();  if (rc) return rc;
    rc = retry_count_starts_again();             if (rc) return rc;
    rc = parent_walk_reaches_root();        if (rc) return rc;
    rc = parent_of_edge_paths();            if (rc) return rc;
    rc = starred_two_levels_resolves_one_per_fetch();    if (rc) return rc;
    rc = starred_three_levels_resolves_one_per_fetch();  if (rc) return rc;
    rc = unknown_path_ends_on_nearest_folder();          if (rc) return rc;
    rc = resolve_stops_at_deepest_known();               if (rc) return rc;
    rc = repoint_never_traps_the_root();                 if (rc) return rc;
    return 0;
}
