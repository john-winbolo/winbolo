/*
 * Base-death prediction: the collision path's replay-tick rule.
 *
 * A client is masked out of a live enemy base's armour, so it cannot tell the
 * square has stopped being solid until the death event reaches it — a round
 * trip after the server has already driven the tank through. For that window
 * the client snaps forward to the server pose each snapshot and replays its
 * unacked inputs straight back into a wall that is no longer there, bleeding
 * the speed off again each time (TANK_WALL_GLIDE is 0, so an oblique hit keeps
 * only the along-wall component). Once the client's own predicted shell is due
 * to drop the base to MIN_ARMOUR_CAPTURE it stamps
 * GameSim.basePredictedDeadTick[b] and derives "drivable" for itself, at the
 * same tick the server will.
 *
 * The rule under test is that the stamp is compared against
 * GameSim.replayTick — the tick being simulated — and not merely tested for
 * being set. A reconciliation replay spans ticks either side of the hit, and
 * the ticks before it must still see a wall; without the comparison the whole
 * replay resolves against one current answer and the tank predicts itself
 * through the base early.
 *
 * Driven on a ServerSim's GameSim: tankBuildingCollision is shared code and
 * both fields live on GameSim, so this exercises the same collision path the
 * client's replay runs, without standing up a networked client. The observable
 * is whether the building nudge ejects a tank sitting on the base's square.
 */

#include <string.h>

#include "global.h"
#include "server_sim.h"
#include "game_sim.h"              /* GameSim.bs / .tanks / the two new fields */
#include "bases.h"                 /* MIN_ARMOUR_CAPTURE, BASE_FULL_ARMOUR */
#include "bolo_map.h"              /* mapSetPos — clear the ground round the base */
#include "tank.h"                  /* tankUpdate, tankSetWorld, tankGetMX/MY */
#include "players.h"               /* playersIsAllie */
#include "test_harness.h"

/* A base's world centre, the same formula the visibility tests use. */
static WORLD bd_base_world(BYTE mapCoord) {
    return (WORLD)(((int)mapCoord << M_W_SHIFT_SIZE) + MAP_SQUARE_MIDDLE);
}

/* Put the tank on the base square, alive and free to move, and run one
 * movement tick. Returns TRUE if it is still on that square afterwards — i.e.
 * the square was drivable and the building nudge left it alone.
 *
 * The live-tank setup is re-applied per call: tankUpdate routes a tank that is
 * dead or still waiting to respawn away from tankMoveUnified entirely, and
 * then nothing about collision is being observed. */
static bool bd_stays_on_base(GameSim *gs, BYTE baseIdx) {
    BYTE bx = (*gs->bs).item[baseIdx].x;
    BYTE by = (*gs->bs).item[baseIdx].y;
    tankSetDeathWait(&gs->tanks[0], 0);
    tankSetArmour(&gs->tanks[0], TANK_FULL_ARMOUR);
    tankSetOnBoat(&gs->tanks[0], FALSE);
    tankSetSpeed(&gs->tanks[0], 0);
    gs->inStartFind = FALSE;
    tankSetWorld(gs, &gs->tanks[0], bd_base_world(bx), bd_base_world(by), 0, false);
    tankUpdate(gs, &gs->tanks[0], TNONE, FALSE, FALSE);
    return (tankGetMX(&gs->tanks[0]) == bx && tankGetMY(&gs->tanks[0]) == by);
}

int run_base_death_prediction_replay_tick(void) {
    ServerSim *sim = ut_make_running_sim("P0");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
    serverSimAddPlayer(sim, 1, "P1", false);

    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL, "serverSimGetGameSim returned NULL");
    UT_ASSERT_MSG(basesGetNumBases(&gs->bs) >= 2,
                  "Everard map has < 2 bases (%u)", basesGetNumBases(&gs->bs));
    UT_ASSERT_MSG(gs->tanks[0] != NULL, "slot-0 tank not valid for positioning");
    UT_ASSERT_MSG(playersIsAllie(&gs->plyrs, 0, 1) != TRUE,
                  "players 0 and 1 allied by default — breaks the enemy case");

    const BYTE b = 1;
    BYTE bx = (*gs->bs).item[b].x;
    BYTE by = (*gs->bs).item[b].y;

    /* Base b enemy and healthy: solid for player 0 under basesCantDrive. */
    (*gs->bs).item[b].owner  = 1;
    (*gs->bs).item[b].armour = 50;

    /* Clear the ring round the base to grass so the only thing that can push
     * the tank off the square is the base itself, whatever the map holds. */
    {
        int dx, dy;
        for (dy = -1; dy <= 1; dy++) {
            for (dx = -1; dx <= 1; dx++) {
                int nx = (int)bx + dx, ny = (int)by + dy;
                if (nx < 0 || ny < 0 || nx > 255 || ny > 255) continue;
                if (dx == 0 && dy == 0) continue;
                mapSetPos(gs, &gs->mp, (BYTE)nx, (BYTE)ny, GRASS, FALSE, FALSE);
            }
        }
    }

    /* Baseline — the square must actually be solid, or nothing below means
     * anything. */
    memset(gs->basePredictedDeadTick, 0, sizeof(gs->basePredictedDeadTick));
    gs->replayTick = 0;
    UT_ASSERT_MSG(bd_stays_on_base(gs, b) == FALSE,
                  "a healthy enemy base must push the tank off its square — "
                  "the rest of this test reads nothing otherwise");

    /* Prediction armed, but this replayed tick is before the hit: still solid.
     * This is the arm that a bare is-it-set test would get wrong. */
    memset(gs->basePredictedDeadTick, 0, sizeof(gs->basePredictedDeadTick));
    gs->basePredictedDeadTick[b] = 20;
    gs->replayTick = 10;
    UT_ASSERT_MSG(bd_stays_on_base(gs, b) == FALSE,
                  "a tick replayed before the predicted death (replayTick 10 < 20) "
                  "must still see a wall");

    /* The tick of the hit itself: drivable from here on. */
    gs->replayTick = 20;
    UT_ASSERT_MSG(bd_stays_on_base(gs, b) == TRUE,
                  "at the predicted death tick (replayTick 20 == 20) the square "
                  "must be drivable");

    /* And after it. */
    gs->replayTick = 35;
    UT_ASSERT_MSG(bd_stays_on_base(gs, b) == TRUE,
                  "after the predicted death tick (replayTick 35 > 20) the square "
                  "must stay drivable");

    /* replayTick 0 means "not predicting" — the server ticks the same code with
     * both fields zero and must never take the prediction path. */
    gs->replayTick = 0;
    UT_ASSERT_MSG(bd_stays_on_base(gs, b) == FALSE,
                  "outside prediction (replayTick 0) the stamp must be ignored");

    /* Clearing the stamp — what an authoritative base-stock arrival does —
     * puts the wall back even mid-replay, so a mispredicted death is taken
     * back rather than left standing. */
    gs->basePredictedDeadTick[b] = 0;
    gs->replayTick = 35;
    UT_ASSERT_MSG(bd_stays_on_base(gs, b) == FALSE,
                  "a cleared stamp must restore the wall");

    /* An allied base is drivable regardless, so the prediction is not what is
     * being observed above by accident. */
    (*gs->bs).item[b].owner = 0;
    memset(gs->basePredictedDeadTick, 0, sizeof(gs->basePredictedDeadTick));
    gs->replayTick = 0;
    UT_ASSERT_MSG(bd_stays_on_base(gs, b) == TRUE,
                  "an own base must be drivable with no prediction in play");

    serverSimDestroy(sim);
    return 0;
}
