/*
 * Coverage for playersLeaveAlliance's hand-over of pillboxes and bases
 * (issue #420).
 *
 * A leave hands the leaver's bases and planted pillboxes to the first
 * ally it finds. With no ally, the search loop used to run off the end
 * and leave its counter at 15, so everything went to seat 15 — empty or
 * an enemy — on the server and on every client that applied the
 * published CTRL_ALLIANCE_LEAVE. The GUI only offers Leave while you
 * have an ally, but the server takes the command from anyone, and two
 * allies leaving at once reach it with the second one allied to nobody.
 *
 * Every case runs the leave through serverSimLeaveAlliance, then feeds
 * the CTRL_ALLIANCE_LEAVE it published to a ClientSim holding the same
 * world, and checks the owners on both.
 *
 *   1-3. No ally: nothing moves, whether seat 15 is empty, an enemy, or
 *        the leaver.
 *   4-5. An ally: the leaver's planted pills and bases go to it, and
 *        seat 15 as the ally still gets them.
 *
 * A pill carried in the leaver's tank stays the leaver's in every case.
 */

#include <stdint.h>
#include <stdio.h>
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

#define AL_PILLS 4
#define AL_BASES 3
#define AL_TOP_SEAT ((BYTE)(MAX_TANKS - 1))

typedef struct {
    int          leaveCount;
    ControlEvent lastLeave;
} LeaveCapture;

static void capture_leave(void *ctx, const ControlEvent *evt) {
    LeaveCapture *c = (LeaveCapture *)ctx;
    if (evt->type == CTRL_ALLIANCE_LEAVE) {
        c->leaveCount++;
        c->lastLeave = *evt;
    }
}

/* Pill 1 planted and pill 2 carried, both the leaver's; pill 3 another
 * player's; pill 4 nobody's. Bases 1 and 2 the leaver's, base 3
 * nobody's. */
static void al_seed_world(GameSim *gs, BYTE leaver, BYTE other) {
    pillsSetNumPills(&gs->pb, AL_PILLS);
    (*gs->pb).item[0].owner  = leaver;
    (*gs->pb).item[0].inTank = FALSE;
    (*gs->pb).item[1].owner  = leaver;
    (*gs->pb).item[1].inTank = TRUE;
    (*gs->pb).item[2].owner  = other;
    (*gs->pb).item[2].inTank = FALSE;
    (*gs->pb).item[3].owner  = NEUTRAL;
    (*gs->pb).item[3].inTank = FALSE;

    basesSetNumBases(&gs->bs, AL_BASES);
    (*gs->bs).item[0].owner = leaver;
    (*gs->bs).item[1].owner = leaver;
    (*gs->bs).item[2].owner = NEUTRAL;
}

static int al_check_owners(GameSim *gs, BYTE leaver, BYTE other, BYTE heir,
                           const char *where) {
    const BYTE wantPills[AL_PILLS] = { heir, leaver, other, NEUTRAL };
    const BYTE wantBases[AL_BASES] = { heir, heir, NEUTRAL };
    BYTE i;

    for (i = 0; i < AL_PILLS; i++) {
        BYTE got = pillsGetPillOwner(&gs->pb, (BYTE)(i + 1));
        UT_ASSERT_MSG(got == wantPills[i],
                      "%s: pillbox %u is owned by %u, expected %u",
                      where, (unsigned)(i + 1), (unsigned)got,
                      (unsigned)wantPills[i]);
    }
    UT_ASSERT_MSG((*gs->pb).item[1].inTank == TRUE,
                  "%s: the carried pillbox left the leaver's tank", where);
    for (i = 0; i < AL_BASES; i++) {
        BYTE got = basesGetBaseOwner(&gs->bs, (BYTE)(i + 1));
        UT_ASSERT_MSG(got == wantBases[i],
                      "%s: base %u is owned by %u, expected %u",
                      where, (unsigned)(i + 1), (unsigned)got,
                      (unsigned)wantBases[i]);
    }
    return 0;
}

/* Seats the players in seats[], allies the leaver with ally unless ally
 * is NEUTRAL, has the leaver leave on the server, then applies the
 * published leave to a ClientSim set up the same way. The leaver's
 * planted pill and bases must end up with heir. other is a seated
 * non-leaver who owns one pill throughout. */
static int al_run_case(const BYTE *seats, int numSeats, BYTE leaver,
                       BYTE ally, BYTE other, BYTE heir) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim;
    GameSim *srvGs;
    ClientSim *cs;
    GameSim *cliGs;
    LeaveCapture cap;
    SubscriberHandle h;
    int failed;
    int i;

    sim = serverSimCreateCompressed(emap, E_MAP_LEN, "Everard Island",
                                    gameOpen, false, 0, -1);
    UT_ASSERT(sim != NULL);
    serverSimSetLobbyEnabled(sim, true);
    for (i = 0; i < numSeats; i++) {
        char name[16];
        snprintf(name, sizeof(name), "P%d", (int)seats[i]);
        serverSimAddPlayer(sim, seats[i], name, false);
    }
    /* The lobby deals new players onto teams, and the start allies each
       team. Team 0 is no team, so the only alliance is the one below. */
    for (i = 0; i < numSeats; i++) {
        serverSimSetTeamBatch(sim, seats[i], 0);
    }
    serverSimStartGameInPlace(sim);
    UT_ASSERT(sim->state == serverStateRunning);
    srvGs = serverSimGetGameSim(sim);

    if (ally != NEUTRAL) {
        serverSimAcceptAlliance(sim, ally, leaver);
        UT_ASSERT(playersIsAllie(&srvGs->plyrs, leaver, ally));
    }
    for (i = 0; i < numSeats; i++) {
        if (seats[i] != leaver && seats[i] != ally) {
            UT_ASSERT_MSG(!playersIsAllie(&srvGs->plyrs, leaver, seats[i]),
                          "test setup: %u is allied with %u",
                          (unsigned)leaver, (unsigned)seats[i]);
        }
    }
    al_seed_world(srvGs, leaver, other);

    memset(&cap, 0, sizeof(cap));
    h = serverSimRegisterSubscriber(sim, capture_leave, &cap);
    UT_ASSERT(h != SUBSCRIBER_HANDLE_INVALID);

    serverSimLeaveAlliance(sim, leaver);

    UT_ASSERT_MSG(cap.leaveCount == 1,
                  "the leave published %d CTRL_ALLIANCE_LEAVE events, "
                  "expected 1", cap.leaveCount);
    UT_ASSERT(cap.lastLeave.u.allianceLeave.playerNum == leaver);
    if (ally != NEUTRAL) {
        UT_ASSERT(!playersIsAllie(&srvGs->plyrs, leaver, ally));
    }
    /* The client is checked even when the server is wrong, so a failure
       says which sides got it wrong. */
    failed = al_check_owners(srvGs, leaver, other, heir, "server");
    serverSimUnregisterSubscriber(sim, h);
    serverSimDestroy(sim);

    /* The client side, looking on from the other player's seat. */
    cs = clientSimAlloc();
    UT_ASSERT(cs != NULL);
    clientSimCreate(cs);
    clientSimSetPlayerNum(cs, other);
    cliGs = clientSimGetGameSim(cs);
    for (i = 0; i < numSeats; i++) {
        cliGs->plyrs->item[seats[i]].inUse = TRUE;
    }
    if (ally != NEUTRAL) {
        playersAcceptAlliance(cliGs, &cliGs->plyrs, other, ally, leaver, FALSE);
    }
    al_seed_world(cliGs, leaver, other);

    clientSimApplyControl(cs, &cap.lastLeave);

    if (al_check_owners(cliGs, leaver, other, heir, "client") != 0) {
        failed = 1;
    }
    clientSimDestroy(cs);
    return failed;
}

/* 1. No ally, seat 15 empty. */
int run_alliance_leave_no_ally_top_seat_empty(void) {
    const BYTE seats[] = { 0, 1, 2 };
    return al_run_case(seats, 3, /*leaver*/ 0, /*ally*/ NEUTRAL,
                       /*other*/ 1, /*heir*/ 0);
}

/* 2. No ally, seat 15 an enemy. */
int run_alliance_leave_no_ally_top_seat_enemy(void) {
    const BYTE seats[] = { 0, 1, AL_TOP_SEAT };
    return al_run_case(seats, 3, /*leaver*/ 0, /*ally*/ NEUTRAL,
                       /*other*/ AL_TOP_SEAT, /*heir*/ 0);
}

/* 3. No ally, and the leaver is seat 15. */
int run_alliance_leave_no_ally_leaver_is_top_seat(void) {
    const BYTE seats[] = { 0, 1, AL_TOP_SEAT };
    return al_run_case(seats, 3, /*leaver*/ AL_TOP_SEAT, /*ally*/ NEUTRAL,
                       /*other*/ 0, /*heir*/ AL_TOP_SEAT);
}

/* 4. An ally takes the leaver's planted pills and bases. */
int run_alliance_leave_hands_to_ally(void) {
    const BYTE seats[] = { 0, 1, 2 };
    return al_run_case(seats, 3, /*leaver*/ 0, /*ally*/ 2,
                       /*other*/ 1, /*heir*/ 2);
}

/* 5. The ally is seat 15, the last seat the search looks at. */
int run_alliance_leave_hands_to_top_seat_ally(void) {
    const BYTE seats[] = { 0, 1, AL_TOP_SEAT };
    return al_run_case(seats, 3, /*leaver*/ 0, /*ally*/ AL_TOP_SEAT,
                       /*other*/ 1, /*heir*/ AL_TOP_SEAT);
}
