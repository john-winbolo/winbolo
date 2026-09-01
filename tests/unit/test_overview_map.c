/*
 * Overview region geometry (test_overview_map.c).
 *
 * overviewMapBuildRegions turns the local player's tank position and the
 * pillboxes they can view through into the set of map squares the overview
 * treats as live. This pins the shape of that set: a 29x29 block on the tank
 * and a 15x15 block on each viewable pill, trimmed at the map edges, the tank
 * rect always first and pills after it in index order, and never more rects
 * than the caller asked for.
 */

#include "global.h"
#include "server_sim.h"
#include "game_sim.h"
#include "overview_map.h"
#include "pillbox.h"
#include "players.h"
#include "allience.h"
#include "test_harness.h"

/* Fails the calling test unless the rect is exactly these edges. */
#define ASSERT_RECT(r, l, t, rt, b)                                           \
    UT_ASSERT_MSG((r).left == (l) && (r).top == (t) && (r).right == (rt) &&    \
                      (r).bottom == (b),                                      \
                  "rect {%d,%d,%d,%d}, expected {%d,%d,%d,%d}",                \
                  (r).left, (r).top, (r).right, (r).bottom, (l), (t), (rt), (b))

int run_overview_regions(void) {
    ServerSim *sim = ut_make_running_sim("Over");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");

    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL, "serverSimGetGameSim returned NULL");

    /* Slots 1 and 2 join; 0 allies with 1 and leaves 2 hostile. The map's own
     * pills are cleared out so every case below places exactly what it needs. */
    serverSimAddPlayer(sim, 1, "Allie", false);
    serverSimAddPlayer(sim, 2, "Enemy", false);
    players *plrs = &gs->plyrs;
    allienceAdd(&(*plrs)->item[0].allie, 1);
    allienceAdd(&(*plrs)->item[1].allie, 0);
    gs->pb->numPills = 0;

    OverviewRect out[OVERVIEW_MAX_REGIONS + 1];
    int n;

    /* Tank alone, well clear of every edge: one 29x29 rect on it. */
    n = overviewMapBuildRegions(gs, 0, TRUE, 100, 100, out, OVERVIEW_MAX_REGIONS);
    UT_ASSERT_MSG(n == 1, "tank with no pills gave %d regions, expected 1", n);
    ASSERT_RECT(out[0], 86, 86, 114, 114);

    /* Over the left and bottom edges the block is trimmed, not wrapped. */
    n = overviewMapBuildRegions(gs, 0, TRUE, 3, 250, out, OVERVIEW_MAX_REGIONS);
    UT_ASSERT_MSG(n == 1, "corner tank gave %d regions, expected 1", n);
    ASSERT_RECT(out[0], 0, 236, 17, 255);

    /* One pill of the player's own, alive and on the map. The tank's rect
     * comes first and the pill's follows it — the order the farewell stamp
     * replays. */
    gs->pb->numPills = 1;
    gs->pb->item[0].owner = 0;
    gs->pb->item[0].armour = PILLBOX_15;
    gs->pb->item[0].inTank = FALSE;
    gs->pb->item[0].x = 200;
    gs->pb->item[0].y = 50;

    n = overviewMapBuildRegions(gs, 0, TRUE, 100, 100, out, OVERVIEW_MAX_REGIONS);
    UT_ASSERT_MSG(n == 2, "tank + own pill gave %d regions, expected 2", n);
    ASSERT_RECT(out[0], 86, 86, 114, 114);
    ASSERT_RECT(out[1], 193, 43, 207, 57);

    /* An allied owner views the same as an own one. */
    gs->pb->item[0].owner = 1;
    n = overviewMapBuildRegions(gs, 0, TRUE, 100, 100, out, OVERVIEW_MAX_REGIONS);
    UT_ASSERT_MSG(n == 2, "allied pill gave %d regions, expected 2", n);
    ASSERT_RECT(out[1], 193, 43, 207, 57);

    /* A pill the player cannot view through contributes nothing: owned by
     * someone hostile, or dead, or carried in a tank. */
    gs->pb->item[0].owner = 2;
    n = overviewMapBuildRegions(gs, 0, TRUE, 100, 100, out, OVERVIEW_MAX_REGIONS);
    UT_ASSERT_MSG(n == 1, "enemy pill gave %d regions, expected 1", n);

    gs->pb->item[0].owner = 0;
    gs->pb->item[0].armour = 0;
    n = overviewMapBuildRegions(gs, 0, TRUE, 100, 100, out, OVERVIEW_MAX_REGIONS);
    UT_ASSERT_MSG(n == 1, "dead pill gave %d regions, expected 1", n);

    gs->pb->item[0].armour = PILLBOX_15;
    gs->pb->item[0].inTank = TRUE;
    n = overviewMapBuildRegions(gs, 0, TRUE, 100, 100, out, OVERVIEW_MAX_REGIONS);
    UT_ASSERT_MSG(n == 1, "carried pill gave %d regions, expected 1", n);

    /* No tank: a viewable pill still gives the player its block, and it is
     * the only rect. */
    gs->pb->item[0].inTank = FALSE;
    n = overviewMapBuildRegions(gs, 0, FALSE, 100, 100, out, OVERVIEW_MAX_REGIONS);
    UT_ASSERT_MSG(n == 1, "pill without a tank gave %d regions, expected 1", n);
    ASSERT_RECT(out[0], 193, 43, 207, 57);

    /* No tank and nothing to view through: no live squares at all. */
    gs->pb->item[0].armour = 0;
    n = overviewMapBuildRegions(gs, 0, FALSE, 100, 100, out, OVERVIEW_MAX_REGIONS);
    UT_ASSERT_MSG(n == 0, "no tank and no viewable pill gave %d regions", n);

    /* A full map of viewable pills plus the tank wants more rects than a
     * short buffer holds; the count stops at maxOut and the square past the
     * end is left alone. */
    {
        const int maxOut = 4;
        BYTE i;

        gs->pb->numPills = MAX_PILLS;
        for (i = 0; i < MAX_PILLS; i++) {
            gs->pb->item[i].owner = 0;
            gs->pb->item[i].armour = PILLBOX_15;
            gs->pb->item[i].inTank = FALSE;
            gs->pb->item[i].x = (BYTE)(20 + i * 10);
            gs->pb->item[i].y = 200;
        }
        out[maxOut].left = -1;
        out[maxOut].top = -1;
        out[maxOut].right = -1;
        out[maxOut].bottom = -1;

        n = overviewMapBuildRegions(gs, 0, TRUE, 100, 100, out, maxOut);
        UT_ASSERT_MSG(n == maxOut, "capped build returned %d, expected %d", n,
                      maxOut);
        ASSERT_RECT(out[maxOut], -1, -1, -1, -1);
    }

    serverSimDestroy(sim);
    return 0;
}
