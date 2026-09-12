/*
 * FX viewport cull (test_fx_viewport_cull.c).
 *
 * Pins serverSimBuildViewports + inAnyViewport, the shared visibility set the
 * snapshot cull and the best-effort fx (sound/explosion) cull both use. The
 * fix: an effect near one of your owned/allied pillboxes — but off your tank
 * screen — must still be deliverable, so the pillbox's surroundings have to be
 * part of the viewport set. This test places a tank and a far-away owned pill
 * and asserts both regions are covered while a point between them is not.
 *
 * run_viewport_floor below pins how far a rect reaches, so a change to
 * SNAPSHOT_VIEWPORT_MARGIN fails here with a message that says what moved
 * rather than obscurely in the fixtures that assume the extent.
 */

#include <string.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* serverSimBuildViewports / ViewportRect / inAnyViewport */
#include "game_sim.h"
#include "tank.h"
#include "pillbox.h"
#include "view_policy.h"           /* viewPolicyAlways / viewPolicyOff — the rules each case sets */
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
    gs->pb->item[0].armour = PILLS_MAX_ARMOUR; /* alive — a dead pill grants no view */
    gs->pb->item[0].x      = pillMX;
    gs->pb->item[0].y      = pillMY;

    /* An owned pillbox granting a rect is the subject, so the pill category
     * is set here rather than inherited: a sim comes up on viewPolicyKey,
     * under which only the pill the player is watching grants one, and this
     * case — which claims no view — would see the tank's rect alone. */
    serverSimSetViewPolicy(sim, viewCategoryPill, viewPolicyAlways,
                           VIEW_DECAY_DEFAULT_SECS);

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

/* How far a tank's rect reaches, in map squares.
 *
 * VF_FLOOR is OVERVIEW_TANK_HALF, the half-width of the block the overview
 * reveals round a tank. A client cannot draw ground it was never sent, so the
 * server has to cover at least this much whatever else changes.
 *
 * VF_LAST_IN and VF_FIRST_OUT are the extent itself, which is
 * SNAPSHOT_SCREEN_SIZE / 2 + SNAPSHOT_VIEWPORT_MARGIN. The numbers are written
 * out rather than derived so that moving the margin fails this case: the
 * overview header is client-side and the snapshot builder does not include it.
 */
#define VF_FLOOR      14
#define VF_LAST_IN    19
#define VF_FIRST_OUT  20

/* Somewhere with VF_FIRST_OUT squares of map on every side. */
#define VF_TANK_MX   128
#define VF_TANK_MY   128

/* Each side and each corner, so an extent that is wrong on one axis only, or
 * in one direction only, is still caught. */
static const struct {
    int         dx;
    int         dy;
    const char *name;
} vfDirs[] = {
    { -1,  0, "west" },       {  1,  0, "east" },
    {  0, -1, "north" },      {  0,  1, "south" },
    { -1, -1, "north-west" }, {  1, -1, "north-east" },
    { -1,  1, "south-west" }, {  1,  1, "south-east" },
};

/* The extent of a recipient's own tank rect, pinned on all eight directions:
 * the overview's reveal block is inside it, the last square of the extent is
 * inside it, and one square further out is not. */
int run_viewport_floor(void) {
    ServerSim *sim = ut_make_running_sim("Vf");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");

    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL, "serverSimGetGameSim returned NULL");
    UT_ASSERT_MSG(gs->tanks[0] != NULL, "slot-0 tank not valid for positioning");

    /* Nothing but the tank may grant a rect, so a square outside the extent is
     * outside every rect the recipient has whatever the map ships with. */
    serverSimSetViewPolicy(sim, viewCategoryPill, viewPolicyOff, VIEW_DECAY_DEFAULT_SECS);
    serverSimSetViewPolicy(sim, viewCategoryBase, viewPolicyOff, VIEW_DECAY_DEFAULT_SECS);
    serverSimSetViewPolicy(sim, viewCategoryAlly, viewPolicyOff, VIEW_DECAY_DEFAULT_SECS);

    /* The cull measures from wx >> 8, so the square centre puts the tank
     * exactly on VF_TANK_MX/MY. */
    WORLD wx = (WORLD)((VF_TANK_MX << M_W_SHIFT_SIZE) + MAP_SQUARE_MIDDLE);
    WORLD wy = (WORLD)((VF_TANK_MY << M_W_SHIFT_SIZE) + MAP_SQUARE_MIDDLE);
    tankSetWorld(gs, &gs->tanks[0], wx, wy, 0, false);

    WORLD lwx = 0, lwy = 0;
    UT_ASSERT_MSG(serverSimGetTankState(sim, 0, &lwx, &lwy),
                  "no tank state for slot 0");
    UT_ASSERT_MSG((int)(lwx >> 8) == VF_TANK_MX && (int)(lwy >> 8) == VF_TANK_MY,
                  "tank sits at %d,%d, expected %d,%d",
                  (int)(lwx >> 8), (int)(lwy >> 8), VF_TANK_MX, VF_TANK_MY);

    ViewportRect vps[MAX_VIEWPORTS];
    int n = serverSimBuildViewports(sim, 0, vps, MAX_VIEWPORTS);
    UT_ASSERT_MSG(n == 1, "expected the tank rect alone, got %d", n);

    for (size_t d = 0; d < sizeof(vfDirs) / sizeof(vfDirs[0]); d++) {
        int fx = VF_TANK_MX + vfDirs[d].dx * VF_FLOOR;
        int fy = VF_TANK_MY + vfDirs[d].dy * VF_FLOOR;
        int ix = VF_TANK_MX + vfDirs[d].dx * VF_LAST_IN;
        int iy = VF_TANK_MY + vfDirs[d].dy * VF_LAST_IN;
        int ox = VF_TANK_MX + vfDirs[d].dx * VF_FIRST_OUT;
        int oy = VF_TANK_MY + vfDirs[d].dy * VF_FIRST_OUT;

        UT_ASSERT_MSG(inAnyViewport(vps, n, fx, fy),
                      "(%d,%d), %d squares %s of the tank, is outside the rect "
                      "— the overview reveals that ground, so the server has to "
                      "send it", fx, fy, VF_FLOOR, vfDirs[d].name);
        UT_ASSERT_MSG(inAnyViewport(vps, n, ix, iy),
                      "(%d,%d), %d squares %s of the tank, is outside the rect "
                      "— the extent has shrunk", ix, iy, VF_LAST_IN,
                      vfDirs[d].name);
        UT_ASSERT_MSG(!inAnyViewport(vps, n, ox, oy),
                      "(%d,%d), %d squares %s of the tank, is inside the rect "
                      "— the extent has grown", ox, oy, VF_FIRST_OUT,
                      vfDirs[d].name);
    }

    serverSimDestroy(sim);
    return 0;
}
