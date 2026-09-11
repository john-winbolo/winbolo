/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * A mine that destroys a tank publishes the kill (test_mine_kill_event.c).
 *
 * Every other way a tank can be destroyed puts an EVENT_TANK_KILLED on the
 * server's event buffer: the two shell arms in shells.c and the drown arm of
 * tankUpdate. A mine used to be the one that did not, so a mine death reached
 * the damage track but never the scoreboard, the WinBolo.net kill event or the
 * attribution track's KILL record, and the round ended holding a destroyed
 * tank with nothing recorded against it.
 *
 * Both cases here go in through minesExpCheckFill rather than calling
 * tankMineDamage directly, because the layer the kill names is read from the
 * mine grid by that function and cleared by the minesRemoveItem in the same
 * block. Calling the damage function with a hand-written owner would not test
 * that the right byte reaches it.
 *
 * land: the mine is under the tank and was laid by another player, so the kill
 *       names that player as the killer and the dead tank as the victim, with
 *       LAST_DEATH_BY_MINES as the cause.
 * boat: the tank is in its boat and the mine goes off on the next square, one
 *       map square away and so inside the blast — the case where the tank is
 *       still on the water when the charge reaches it. The kill is published
 *       the same way and the tank loses the boat.
 * self: a tank on its own mine names itself, the shape the drown arm uses for
 *       a death with nobody else in it, and the derivation counts it as a mine
 *       death rather than as a kill for anyone.
 */

#include <string.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h" /* ServerSim::events / eventCount — T2 */
#include "server_sim_lifecycle.h"
#include "game_sim.h"
#include "bolo_map.h"
#include "mines.h"
#include "minesexp.h"
#include "tank.h"
#include "everard_map.h"
#include "test_harness.h"

/* A running two-player sim. The layer is a real player rather than a bare
 * slot number so the kill names something that could receive the credit. */
static ServerSim *mk_sim_two_players(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097, "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) {
        return NULL;
    }
    serverSimSetLobbyEnabled(sim, false);
    serverSimAddPlayer(sim, 0, "Victim", false);
    serverSimAddPlayer(sim, 1, "Layer", false);
    serverSimStartGame(sim);
    return sim;
}

/* Put a live mine laid by `layer` on one map square. Mined terrain is the
 * plain terrain plus MINE_SUBTRACT, which is what both lay paths write. */
static void mk_lay_mine(GameSim *gs, BYTE mx, BYTE my, BYTE layer) {
    mapSetPos(gs, &gs->mp, mx, my, (BYTE)(GRASS + MINE_SUBTRACT), FALSE, FALSE);
    minesAddItem(&gs->mns, mx, my);
    minesSetOwner(&gs->mns, mx, my, layer);
}

/* Leave the slot-0 tank one mine's worth short of surviving: MINE_DAMAGE has
 * to be strictly greater than the armour to destroy, so zero armour is the
 * state where the next blast kills. */
static void mk_arm_victim(GameSim *gs, bool onBoat) {
    tankSetDestroyed(&gs->tanks[0], FALSE);
    tankSetArmour(&gs->tanks[0], 0);
    tankSetDeathWait(&gs->tanks[0], 0);
    tankSetOnBoat(&gs->tanks[0], onBoat);
}

/* Detonate the mine at (mx, my) against the slot-0 tank only, the way the
 * per-half-step minesExpUpdate does with the compacted tank/lgm arrays. */
static void mk_detonate(ServerSim *sim, GameSim *gs, BYTE mx, BYTE my) {
    tank tanksArray[1];
    lgm *lgmPtrs[1];

    tanksArray[0] = gs->tanks[0];
    lgmPtrs[0] = &gs->lgmen[0];
    sim->eventCount = 0;
    minesExpCheckFill(gs, lgmPtrs, 1, mx, my, tanksArray, &gs->ss);
}

/* The one EVENT_TANK_KILLED on the buffer, or NULL if there is none. Fails the
 * caller's expectations loudly if a second one turns up, because a doubled kill
 * is the failure mode a second call site would produce. */
static const GameEvent *mk_only_kill(ServerSim *sim, int *count) {
    const GameEvent *found = NULL;
    int i;

    *count = 0;
    for (i = 0; i < (int)sim->eventCount; i++) {
        if (sim->events[i].type == EVENT_TANK_KILLED) {
            if (found == NULL) {
                found = &sim->events[i];
            }
            (*count)++;
        }
    }
    return found;
}

int run_mine_kill_publishes_event(void) {
    ServerSim *sim = mk_sim_two_players();
    GameSim *gs;
    const GameEvent *kill;
    BYTE mx;
    BYTE my;
    int killCount;

    UT_ASSERT_MSG(sim != NULL, "mk_sim_two_players returned NULL");
    gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL, "serverSimGetGameSim returned NULL");
    UT_ASSERT_MSG(gs->tanks[0] != NULL, "slot-0 tank not valid");

    mx = tankGetMX(&gs->tanks[0]);
    my = tankGetMY(&gs->tanks[0]);
    mk_lay_mine(gs, mx, my, 1);
    mk_arm_victim(gs, FALSE);

    mk_detonate(sim, gs, mx, my);

    UT_ASSERT_MSG(tankIsDestroyed(&gs->tanks[0]) == TRUE,
                  "the mine did not destroy the tank, so there is no kill to "
                  "publish — armour left %u",
                  (unsigned)tankGetArmour(&gs->tanks[0]));

    kill = mk_only_kill(sim, &killCount);
    UT_ASSERT_MSG(kill != NULL,
                  "a mine kill published no EVENT_TANK_KILLED (%u events on "
                  "the buffer)", (unsigned)sim->eventCount);
    UT_ASSERT_MSG(killCount == 1,
                  "a single mine kill published %d EVENT_TANK_KILLED events",
                  killCount);
    UT_ASSERT_MSG(kill->data[0] == 1,
                  "the kill named killer %u, wanted the layer (1)",
                  (unsigned)kill->data[0]);
    UT_ASSERT_MSG(kill->data[1] == 0,
                  "the kill named victim %u, wanted the dead tank (0)",
                  (unsigned)kill->data[1]);
    UT_ASSERT_MSG(kill->data[2] == LAST_DEATH_BY_MINES,
                  "the kill gave death cause %u, wanted LAST_DEATH_BY_MINES "
                  "(%u)", (unsigned)kill->data[2],
                  (unsigned)LAST_DEATH_BY_MINES);

    /* The funnel behind the same event: the victim is charged a death and a
     * mine death, and the layer is not credited a kill for it (only a shell
     * death moves the kill counters). */
    UT_ASSERT_MSG(serverSimGetRoundStats(sim, 0)->deaths == 1,
                  "victim deaths %u, wanted 1",
                  (unsigned)serverSimGetRoundStats(sim, 0)->deaths);
    UT_ASSERT_MSG(serverSimGetRoundStats(sim, 0)->mineDeaths == 1,
                  "victim mine deaths %u, wanted 1",
                  (unsigned)serverSimGetRoundStats(sim, 0)->mineDeaths);

    serverSimDestroy(sim);
    return 0;
}

int run_mine_kill_on_boat_publishes_event(void) {
    ServerSim *sim = mk_sim_two_players();
    GameSim *gs;
    const GameEvent *kill;
    BYTE mx;
    BYTE my;
    int killCount;

    UT_ASSERT_MSG(sim != NULL, "mk_sim_two_players returned NULL");
    gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL, "serverSimGetGameSim returned NULL");
    UT_ASSERT_MSG(gs->tanks[0] != NULL, "slot-0 tank not valid");

    mx = tankGetMX(&gs->tanks[0]);
    my = tankGetMY(&gs->tanks[0]);

    /* One square east of the tank: the blast reaches anything within 384 world
     * units of the mine's centre and a square is 256, so a tank sitting in the
     * middle of its own square is inside it. */
    mk_lay_mine(gs, (BYTE)(mx + 1), my, 1);
    mk_arm_victim(gs, TRUE);
    UT_ASSERT_MSG(tankIsOnBoat(&gs->tanks[0]) == TRUE,
                  "the victim did not take the boat");

    mk_detonate(sim, gs, (BYTE)(mx + 1), my);

    UT_ASSERT_MSG(tankIsDestroyed(&gs->tanks[0]) == TRUE,
                  "the mine beside the boat did not destroy the tank — armour "
                  "left %u", (unsigned)tankGetArmour(&gs->tanks[0]));
    UT_ASSERT_MSG(tankIsOnBoat(&gs->tanks[0]) == FALSE,
                  "the mine left the destroyed tank on its boat");

    kill = mk_only_kill(sim, &killCount);
    UT_ASSERT_MSG(kill != NULL,
                  "a mine kill on a boat published no EVENT_TANK_KILLED (%u "
                  "events on the buffer)", (unsigned)sim->eventCount);
    UT_ASSERT_MSG(killCount == 1,
                  "a single mine kill on a boat published %d "
                  "EVENT_TANK_KILLED events", killCount);
    UT_ASSERT_MSG(kill->data[0] == 1,
                  "the kill named killer %u, wanted the layer (1)",
                  (unsigned)kill->data[0]);
    UT_ASSERT_MSG(kill->data[1] == 0,
                  "the kill named victim %u, wanted the dead tank (0)",
                  (unsigned)kill->data[1]);
    UT_ASSERT_MSG(kill->data[2] == LAST_DEATH_BY_MINES,
                  "the kill gave death cause %u, wanted LAST_DEATH_BY_MINES "
                  "(%u)", (unsigned)kill->data[2],
                  (unsigned)LAST_DEATH_BY_MINES);

    serverSimDestroy(sim);
    return 0;
}

int run_mine_kill_own_mine_names_self(void) {
    ServerSim *sim = mk_sim_two_players();
    GameSim *gs;
    const GameEvent *kill;
    BYTE mx;
    BYTE my;
    int killCount;

    UT_ASSERT_MSG(sim != NULL, "mk_sim_two_players returned NULL");
    gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL, "serverSimGetGameSim returned NULL");
    UT_ASSERT_MSG(gs->tanks[0] != NULL, "slot-0 tank not valid");

    mx = tankGetMX(&gs->tanks[0]);
    my = tankGetMY(&gs->tanks[0]);
    mk_lay_mine(gs, mx, my, 0);
    mk_arm_victim(gs, FALSE);

    mk_detonate(sim, gs, mx, my);

    kill = mk_only_kill(sim, &killCount);
    UT_ASSERT_MSG(kill != NULL,
                  "a tank on its own mine published no EVENT_TANK_KILLED");
    UT_ASSERT_MSG(killCount == 1,
                  "a tank on its own mine published %d EVENT_TANK_KILLED "
                  "events", killCount);
    UT_ASSERT_MSG(kill->data[0] == 0 && kill->data[1] == 0,
                  "a tank on its own mine named killer %u / victim %u, wanted "
                  "0 / 0", (unsigned)kill->data[0], (unsigned)kill->data[1]);
    UT_ASSERT_MSG(kill->data[2] == LAST_DEATH_BY_MINES,
                  "the kill gave death cause %u, wanted LAST_DEATH_BY_MINES "
                  "(%u)", (unsigned)kill->data[2],
                  (unsigned)LAST_DEATH_BY_MINES);

    /* The derivation reads it as a mine death, not as a kill for anyone: a
     * self-kill on a mine must not hand the dead player a point. */
    UT_ASSERT_MSG(serverSimGetRoundStats(sim, 0)->mineDeaths == 1,
                  "self mine deaths %u, wanted 1",
                  (unsigned)serverSimGetRoundStats(sim, 0)->mineDeaths);
    UT_ASSERT_MSG(serverSimGetRoundStats(sim, 0)->kills == 0,
                  "a tank on its own mine was credited %u kills, wanted 0",
                  (unsigned)serverSimGetRoundStats(sim, 0)->kills);

    serverSimDestroy(sim);
    return 0;
}
