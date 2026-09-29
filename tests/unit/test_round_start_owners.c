/*
 * Pillbox and base owners stored in a map file survive a round start
 * (issue #422).
 *
 * A map file can carry owners, and the loader keeps any owner from 0 to
 * 15. Such an item belongs to the matching seat at round start. The
 * countdown start (serverSimStartGame) used to publish a
 * CTRL_ALLIANCE_LEAVE per connected seat straight after reloading the
 * map. The server's world reset had already emptied every alliance, so
 * its own leaves moved nothing, but a client still held last round's
 * alliances when those events arrived and applied each one with
 * playersLeaveAlliance, which handed the seat's items to its first ally.
 *
 * Each case builds a map blob whose items are owned by seats 0, 1 and 2,
 * creates the server from it, allies 0 and 1, and starts a round. The
 * owners are checked on the server, and on a ClientSim that holds the
 * same world and alliances and is fed the alliance events the start
 * published.
 *
 *   1. 0 and 1 allied by the lobby teams.
 *   2. 0 and 1 allied by hand during an earlier round, on no team.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "client_sim.h"
#include "client_sim_internal.h"
#include "client_sim_control.h"
#include "control_event.h"
#include "server_sim.h"
#include "server_sim_internal.h"
#include "server_sim_lifecycle.h"
#include "players.h"
#include "pillbox.h"
#include "bases.h"
#include "everard_map.h"
#include "test_harness.h"

/* Item k (0-based) is owned by seat k in the map file. */
#define RSO_SEATS 3

#define RSO_MAX_EVENTS 64

typedef struct {
    int          count;
    ControlEvent evts[RSO_MAX_EVENTS];
} AllianceCapture;

/* Keeps the alliance events in order; those are all a client needs to
   repeat what the start did to alliances. */
static void capture_alliance(void *ctx, const ControlEvent *evt) {
    AllianceCapture *c = (AllianceCapture *)ctx;
    if (evt->type != CTRL_ALLIANCE_LEAVE && evt->type != CTRL_ALLIANCE_RESET
        && evt->type != CTRL_ALLIANCE_ACCEPT) {
        return;
    }
    if (c->count < RSO_MAX_EVENTS) {
        c->evts[c->count] = *evt;
    }
    c->count++;
}

static void rso_set_owners(GameSim *gs) {
    BYTE k;
    for (k = 0; k < RSO_SEATS; k++) {
        (*gs->pb).item[k].owner  = k;
        (*gs->pb).item[k].inTank = FALSE;
        (*gs->bs).item[k].owner  = k;
    }
}

static int rso_check_owners(GameSim *gs, const char *where) {
    BYTE k;
    for (k = 0; k < RSO_SEATS; k++) {
        BYTE pill = pillsGetPillOwner(&gs->pb, (BYTE)(k + 1));
        BYTE base = basesGetBaseOwner(&gs->bs, (BYTE)(k + 1));
        UT_ASSERT_MSG(pill == k,
                      "%s: pillbox %u is owned by %u, the map file says %u",
                      where, (unsigned)(k + 1), (unsigned)pill, (unsigned)k);
        UT_ASSERT_MSG(base == k,
                      "%s: base %u is owned by %u, the map file says %u",
                      where, (unsigned)(k + 1), (unsigned)base, (unsigned)k);
    }
    return 0;
}

/* A server whose map file gives item k to seat k, with seats 0-2 in the
   lobby. */
static ServerSim *rso_make_sim(void) {
    static BYTE blob[65536];
    BYTE emap[6000] = E_MAP;
    ServerSim *src;
    ServerSim *sim;
    int len;
    BYTE k;

    src = serverSimCreateCompressed(emap, E_MAP_LEN, "Everard Island",
                                    gameOpen, false, 0, -1);
    if (src == NULL) return NULL;
    rso_set_owners(serverSimGetGameSim(src));
    len = serverSimGetCompressedMap(src, blob, (int)sizeof(blob));
    serverSimDestroy(src);
    if (len <= 0) return NULL;

    sim = serverSimCreateCompressed(blob, len, "Owned Everard",
                                    gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    for (k = 0; k < RSO_SEATS; k++) {
        char name[16];
        snprintf(name, sizeof(name), "P%u", (unsigned)k);
        serverSimAddPlayer(sim, k, name, false);
    }
    return sim;
}

/* Starts the round with the countdown path, then checks the owners on the
   server and on a client that applies what the start published. */
static int rso_start_and_check(ServerSim *sim) {
    AllianceCapture *cap;
    SubscriberHandle h;
    GameSim *srvGs;
    ClientSim *cs;
    GameSim *cliGs;
    int failed;
    int e;
    BYTE k;

    srvGs = serverSimGetGameSim(sim);
    UT_ASSERT(playersIsAllie(&srvGs->plyrs, 0, 1));
    UT_ASSERT(!playersIsAllie(&srvGs->plyrs, 0, 2));

    /* The client as it stood before the start: the same world and the
       same alliances. Seat 2 is looking on. */
    cs = clientSimAlloc();
    UT_ASSERT(cs != NULL);
    clientSimCreate(cs);
    clientSimSetPlayerNum(cs, 2);
    cliGs = clientSimGetGameSim(cs);
    for (k = 0; k < RSO_SEATS; k++) {
        cliGs->plyrs->item[k].inUse = TRUE;
    }
    playersAcceptAlliance(cliGs, &cliGs->plyrs, 2, 0, 1, FALSE);
    pillsSetNumPills(&cliGs->pb, RSO_SEATS);
    basesSetNumBases(&cliGs->bs, RSO_SEATS);
    rso_set_owners(cliGs);

    cap = (AllianceCapture *)calloc(1, sizeof(*cap));
    UT_ASSERT(cap != NULL);
    h = serverSimRegisterSubscriber(sim, capture_alliance, cap);
    UT_ASSERT(h != SUBSCRIBER_HANDLE_INVALID);

    serverSimStartGame(sim);

    serverSimUnregisterSubscriber(sim, h);
    srvGs = serverSimGetGameSim(sim);
    UT_ASSERT(sim->state == serverStateRunning);

    /* The client is checked even when the server is wrong, so a failure
       says which sides got it wrong. */
    failed = rso_check_owners(srvGs, "server");

    UT_ASSERT_MSG(cap->count <= RSO_MAX_EVENTS,
                  "the start published %d alliance events, more than the "
                  "test keeps", cap->count);
    for (e = 0; e < cap->count; e++) {
        clientSimApplyControl(cs, &cap->evts[e]);
    }
    if (rso_check_owners(cliGs, "client") != 0) {
        failed = 1;
    }

    free(cap);
    clientSimDestroy(cs);
    return failed;
}

/* 1. 0 and 1 are allied because they share a lobby team. */
int run_round_start_owners_kept_with_lobby_team(void) {
    ServerSim *sim = rso_make_sim();
    GameSim *gs;
    int failed;

    UT_ASSERT(sim != NULL);
    serverSimSetTeamBatch(sim, 0, 1);
    serverSimSetTeamBatch(sim, 1, 1);
    serverSimSetTeamBatch(sim, 2, 0);
    serverSimReapplyTeamAlliances(sim);
    gs = serverSimGetGameSim(sim);
    if (rso_check_owners(gs, "test setup") != 0) {
        serverSimDestroy(sim);
        return 1;
    }

    failed = rso_start_and_check(sim);
    if (failed == 0) {
        /* The teams still ally 0 and 1 in the new round. */
        gs = serverSimGetGameSim(sim);
        if (!playersIsAllie(&gs->plyrs, 0, 1)) {
            fprintf(stderr, "FAIL: after the start 0 and 1 are not allied\n");
            failed = 1;
        }
    }
    serverSimDestroy(sim);
    return failed;
}

/* 2. 0 and 1 allied by hand during a round before this one, on no team. */
int run_round_start_owners_kept_after_manual_alliance(void) {
    ServerSim *sim = rso_make_sim();
    GameSim *gs;
    int failed;
    BYTE k;

    UT_ASSERT(sim != NULL);
    for (k = 0; k < RSO_SEATS; k++) {
        serverSimSetTeamBatch(sim, k, 0);
    }
    serverSimStartGameInPlace(sim);
    UT_ASSERT(sim->state == serverStateRunning);
    serverSimAcceptAlliance(sim, 1, 0);

    /* The earlier round is not what is being tested, so its world is put
       back to what the file says before the next start. */
    gs = serverSimGetGameSim(sim);
    rso_set_owners(gs);

    failed = rso_start_and_check(sim);
    if (failed == 0) {
        /* No team, so nobody is allied in the new round. */
        gs = serverSimGetGameSim(sim);
        if (playersIsAllie(&gs->plyrs, 0, 1)) {
            fprintf(stderr, "FAIL: after the start 0 and 1 are still "
                            "allied\n");
            failed = 1;
        }
    }
    serverSimDestroy(sim);
    return failed;
}
