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
    static const uint64_t kLong[] = { 1000, 2000, 4000, 8000, 16000,
                                      32000, 60000, 60000 };
    static const uint64_t kOdd[]  = { 500, 1000, 2000, 3000, 3000 };
    unsigned i;

    /* The listing's own numbers. With two sends the only wait it
     * reaches is the first one. */
    UT_ASSERT(WBN_MAPS_RETRY_MS == 1000u);
    UT_ASSERT(WBN_MAPS_RETRY_MAX_MS == 2000u);
    UT_ASSERT(WBN_MAPS_RETRY_MAX_SENDS == 2u);
    UT_ASSERT(wbnMapsRetryWaitMs(1) == 1000u);
    /* Nothing sent yet reads as the first wait, not zero. */
    UT_ASSERT(wbnMapsRetryWaitMs(0) == 1000u);
    /* Past the sends the listing makes, the rule still holds. */
    UT_ASSERT(wbnMapsRetryWaitMs(2) == 2000u);
    UT_ASSERT(wbnMapsRetryWaitMs(3) == 2000u);
    UT_ASSERT(wbnMapsRetryWaitMs(1000) == 2000u);
    /* The retry wait never adds to a send that ran to either limit. */
    UT_ASSERT(wbnMapsRetryWaitMs(1) <= WBN_MAPS_CONNECT_TIMEOUT_MS);
    UT_ASSERT(wbnMapsRetryWaitMs(1) <= WBN_MAPS_REQUEST_TIMEOUT_MS);

    /* The rule with other numbers, over several sends: 1 s first and a
     * 60 s cap doubles six times and then stays at the cap. */
    for (i = 0; i < sizeof(kLong) / sizeof(kLong[0]); i++) {
        uint64_t w = wbnMapsRetryWaitFor(i + 1, 1000, 60000);
        UT_ASSERT_MSG(w == kLong[i], "sends=%u wait %llu ms", i + 1,
                      (unsigned long long)w);
    }
    UT_ASSERT(wbnMapsRetryWaitFor(1000, 1000, 60000) == 60000u);
    /* A cap that is not a doubling of the first wait is still the most
     * the wait reaches. */
    for (i = 0; i < sizeof(kOdd) / sizeof(kOdd[0]); i++) {
        uint64_t w = wbnMapsRetryWaitFor(i + 1, 500, 3000);
        UT_ASSERT_MSG(w == kOdd[i], "sends=%u wait %llu ms", i + 1,
                      (unsigned long long)w);
    }
    UT_ASSERT(wbnMapsRetryWaitFor(0, 500, 3000) == 500u);
    /* The listing's wait is the rule with the listing's numbers. */
    for (i = 0; i < 6; i++) {
        UT_ASSERT(wbnMapsRetryWaitMs(i) ==
                  wbnMapsRetryWaitFor(i, WBN_MAPS_RETRY_MS,
                                      WBN_MAPS_RETRY_MAX_MS));
    }
    return 0;
}

/* Drives a request to a server that never answers: each send holds the
 * "in flight" flag for perSendMs (the time the HTTP limit that applies
 * takes to end it) and then fails, and the provider (one frame per
 * 10 ms) sends again only when nothing is in flight and the wait has
 * passed. Returns the time from the first send to the frame that finds
 * the retries stopped. */
static uint64_t runSilentServer(uint64_t startMs, uint64_t perSendMs,
                                int *sendsOut) {
    WbnMapsRetry r = { 0, 0 };
    uint64_t busyUntil = 0;
    uint64_t t;
    int n = 0;
    for (t = startMs; t <= startMs + 600000; t += 10) {
        bool inFlight = (n > 0 && t < busyUntil);
        bool same = (n > 0);
        if (inFlight) continue;
        if (wbnMapsRetriesStopped(&r, same)) break;
        if (wbnMapsFetchDue(&r, same, t)) {
            wbnMapsRetryNoteSent(&r, same, t);
            busyUntil = t + perSendMs;
            n++;
        }
    }
    *sendsOut = n;
    return t - startMs;
}

/* With a server that does not answer at all (the connect never
 * completes), each send ends at the connect limit: two sends of 3 s, at
 * 0 and 3 s, and the stop at 6 s. The 1 s wait is over before the first
 * send ends, so it adds nothing. It was 180 s (six sends of 30 s). */
static int silent_server_gives_up_inside_the_budget(void) {
    int sends = 0;
    uint64_t took = runSilentServer(50000, WBN_MAPS_CONNECT_TIMEOUT_MS,
                                    &sends);
    UT_ASSERT(WBN_MAPS_CONNECT_TIMEOUT_MS == 3000u);
    UT_ASSERT(WBN_MAPS_GIVE_UP_BUDGET_MS == 10000u);
    /* The connect limit is inside the total limit, or it would never
     * be the one that ends a send. */
    UT_ASSERT(WBN_MAPS_CONNECT_TIMEOUT_MS < WBN_MAPS_REQUEST_TIMEOUT_MS);
    UT_ASSERT_MSG(sends == (int)WBN_MAPS_RETRY_MAX_SENDS, "sends=%d", sends);
    UT_ASSERT_MSG(took == 6000u, "took %llu ms", (unsigned long long)took);
    UT_ASSERT_MSG(wbnMapsGiveUpAfterMs(WBN_MAPS_CONNECT_TIMEOUT_MS) == took,
                  "computed %llu ms", (unsigned long long)
                  wbnMapsGiveUpAfterMs(WBN_MAPS_CONNECT_TIMEOUT_MS));
    UT_ASSERT(took <= WBN_MAPS_GIVE_UP_BUDGET_MS);
    return 0;
}

/* A server that accepts the connection and then never answers holds
 * each send for the whole request limit: two sends of 5 s, at 0 and 5 s,
 * and the stop at 10 s. Nothing takes longer, so this is the worst case
 * and it is the whole budget. */
static int accepting_silent_server_is_the_worst_case(void) {
    int sends = 0;
    uint64_t took = runSilentServer(50000, WBN_MAPS_REQUEST_TIMEOUT_MS,
                                    &sends);
    UT_ASSERT(WBN_MAPS_REQUEST_TIMEOUT_MS == 5000u);
    UT_ASSERT_MSG(sends == (int)WBN_MAPS_RETRY_MAX_SENDS, "sends=%d", sends);
    UT_ASSERT_MSG(took == 10000u, "took %llu ms", (unsigned long long)took);
    UT_ASSERT_MSG(wbnMapsWorstCaseMs() == took, "worst case %llu ms",
                  (unsigned long long)wbnMapsWorstCaseMs());
    UT_ASSERT(wbnMapsWorstCaseMs() <= WBN_MAPS_GIVE_UP_BUDGET_MS);
    /* A send that fails at once costs only the wait: 1 s in all. */
    UT_ASSERT(wbnMapsGiveUpAfterMs(0) == 1000u);
    took = runSilentServer(50000, 0, &sends);
    UT_ASSERT_MSG(sends == (int)WBN_MAPS_RETRY_MAX_SENDS, "sends=%d", sends);
    UT_ASSERT_MSG(took == 1000u + 10u, "took %llu ms",
                  (unsigned long long)took);
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

/* A folder that fails at once every time is sent at 0 and 1 s (a wait
 * of 1 s) and then never again, so the error text is due at about 1 s.
 * With a fixed wait and no limit it was sent for as long as the tab was
 * open. */
static int failing_request_backs_off_and_stops(void) {
    static const uint64_t kExpect[] = { 0, 1000 };
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
    UT_ASSERT(r.sends == 1);
    UT_ASSERT(!wbnMapsRetriesStopped(&r, true));
    UT_ASSERT(!wbnMapsFetchDue(&r, true, 1000 + 999));
    UT_ASSERT(wbnMapsFetchDue(&r, true, 1000 + 1000));
    /* Stopped once the last send is made, however long ago. */
    wbnMapsRetryNoteSent(&r, true, 4000);
    UT_ASSERT(r.sends == 2);
    UT_ASSERT(wbnMapsRetriesStopped(&r, true));
    UT_ASSERT(!wbnMapsFetchDue(&r, true, 4000 + 3600000));

    /* An answer: the next failure of that folder waits 1 s again. */
    wbnMapsRetryReset(&r);
    UT_ASSERT(r.sends == 0);
    wbnMapsRetryNoteSent(&r, false, 20000);
    UT_ASSERT(r.sends == 1);
    UT_ASSERT(!wbnMapsFetchDue(&r, true, 20999));
    UT_ASSERT(wbnMapsFetchDue(&r, true, 21000));

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
    rc = silent_server_gives_up_inside_the_budget();  if (rc) return rc;
    rc = accepting_silent_server_is_the_worst_case(); if (rc) return rc;
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
