/*
 * test_braintest_tank_dead.c - BrainTest does not draw a dead tank.
 *
 * In a loaded session a dead tank was drawn on the spot where it died,
 * turning, until it respawned. The recording keeps the tank's place and
 * its changing angle for the whole respawn wait, and the map renderer
 * decided "alive" from deathWait alone, which the recorder fills for
 * slot 0 only.
 *
 * braintest_tank_status_dead: the playback rule. A recorded tank is dead
 * when its tankStatus has TANK_STATUS_DEAD or TANK_STATUS_DESTROYED; the
 * boat bit has no effect.
 *
 * braintest_tank_render_dead: the live rule. serverSimGetTankRender
 * reports a tank not alive in its respawn wait, and also when it is
 * destroyed with the wait run out (no start found yet). A live tank
 * reports alive.
 */

#include <stdint.h>

#include "global.h"
#include "server_sim.h"
#include "game_sim.h"
#include "tank.h"
#include "input_packet.h"
#include "braintest_tank_dead.h"
#include "test_harness.h"

int run_braintest_tank_status_dead(void) {
    UT_ASSERT(!braintestTankStatusIsDead(0));
    UT_ASSERT(!braintestTankStatusIsDead(TANK_STATUS_ON_BOAT));
    UT_ASSERT(braintestTankStatusIsDead(TANK_STATUS_DEAD));
    UT_ASSERT(braintestTankStatusIsDead(TANK_STATUS_DESTROYED));
    UT_ASSERT(braintestTankStatusIsDead(TANK_STATUS_DEAD | TANK_STATUS_DESTROYED));
    UT_ASSERT(braintestTankStatusIsDead(TANK_STATUS_DEAD | TANK_STATUS_DESTROYED |
                                        TANK_STATUS_ON_BOAT));
    UT_ASSERT(braintestTankStatusIsDead(TANK_STATUS_DESTROYED | TANK_STATUS_ON_BOAT));
    return 0;
}

int run_braintest_tank_render_dead(void) {
    ServerSim *sim = ut_make_running_sim("P0");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL && gs->tanks[0] != NULL, "no slot-0 tank");
    TankRenderInfo info;

    tankSetDeathWait(&gs->tanks[0], 0);
    tankSetDestroyed(&gs->tanks[0], FALSE);
    UT_ASSERT(serverSimGetTankRender(sim, 0, &info));
    UT_ASSERT_MSG(info.alive, "a live tank must report alive");

    /* In the respawn wait. */
    tankSetDeathWait(&gs->tanks[0], 100);
    tankSetDestroyed(&gs->tanks[0], TRUE);
    UT_ASSERT(serverSimGetTankRender(sim, 0, &info));
    UT_ASSERT_MSG(!info.alive, "a tank in its respawn wait must not report alive");

    /* Wait run out, still destroyed: no start found yet. */
    tankSetDeathWait(&gs->tanks[0], 0);
    UT_ASSERT(serverSimGetTankRender(sim, 0, &info));
    UT_ASSERT_MSG(!info.alive, "a destroyed tank must not report alive");

    /* Respawned. */
    tankSetDestroyed(&gs->tanks[0], FALSE);
    UT_ASSERT(serverSimGetTankRender(sim, 0, &info));
    UT_ASSERT_MSG(info.alive, "a respawned tank must report alive");

    serverSimDestroy(sim);
    return 0;
}
