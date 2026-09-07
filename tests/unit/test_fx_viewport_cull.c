/*
 * FX viewport cull (test_fx_viewport_cull.c).
 *
 * Pins serverSimBuildViewports + inAnyViewport, the shared visibility set the
 * snapshot cull and the best-effort fx (sound/explosion) cull both use. The
 * fix: an effect near one of your owned/allied pillboxes — but off your tank
 * screen — must still be deliverable, so the pillbox's surroundings have to be
 * part of the viewport set. This test places a tank and a far-away owned pill
 * and asserts both regions are covered while a point between them is not.
 */

#include <string.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* serverSimBuildViewports / ViewportRect / inAnyViewport */
#include "game_sim.h"
#include "tank.h"
#include "pillbox.h"
#include "test_harness.h"

int run_fx_viewport_cull(void) {
    ServerSim *sim = ut_make_running_sim("Fx");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");

    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL, "serverSimGetGameSim returned NULL");
    UT_ASSERT_MSG(pillsGetNumPills(&gs->pb) >= 1,
                  "Everard map has no pills (%u) — test needs one",
                  pillsGetNumPills(&gs->pb));
    UT_ASSERT_MSG(gs->tanks[0] != NULL, "slot-0 tank not valid for positioning");

    /* Tank near map (50,50); owned pill far away at (200,200) — several
     * screens apart so neither region overlaps the other. */
    const BYTE tankMX = 50, tankMY = 50;
    const BYTE pillMX = 200, pillMY = 200;
    WORLD tankWX = (WORLD)(((int)tankMX << M_W_SHIFT_SIZE) + MAP_SQUARE_MIDDLE);
    WORLD tankWY = (WORLD)(((int)tankMY << M_W_SHIFT_SIZE) + MAP_SQUARE_MIDDLE);
    tankSetWorld(gs, &gs->tanks[0], tankWX, tankWY, 0, false);

    gs->pb->item[0].owner  = 0;     /* owned by player 0 */
    gs->pb->item[0].inTank = FALSE; /* placed on the map */
    gs->pb->item[0].armour = PILL_MAX_HEALTH; /* alive — a dead pill grants no view */
    gs->pb->item[0].x      = pillMX;
    gs->pb->item[0].y      = pillMY;

    ViewportRect vps[MAX_VIEWPORTS];
    int n = serverSimBuildViewports(sim, 0, vps, MAX_VIEWPORTS);
    UT_ASSERT_MSG(n >= 2, "expected tank + pill viewports, got %d", n);

    /* The tank screen is covered. */
    UT_ASSERT_MSG(inAnyViewport(vps, n, tankMX, tankMY),
                  "tank position (%u,%u) not in any viewport", tankMX, tankMY);

    /* The owned-pillbox screen is covered — the fix. */
    UT_ASSERT_MSG(inAnyViewport(vps, n, pillMX, pillMY),
                  "owned pillbox (%u,%u) not in any viewport", pillMX, pillMY);

    /* A point midway between, beyond halfView of both, is in neither. */
    UT_ASSERT_MSG(!inAnyViewport(vps, n, 125, 125),
                  "midpoint (125,125) should be outside both viewports");

    serverSimDestroy(sim);
    return 0;
}
