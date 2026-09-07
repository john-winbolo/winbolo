/*
 * The overview map memory (test_overview_map.c).
 *
 * run_overview_regions is the module's pure pieces. overviewMapBuildRegions
 * turns the local player's tank position and the pillboxes they can view
 * through into the set of map squares the overview treats as live, and this
 * pins the shape of that set — a 29x29 block on the tank and a 15x15 block on
 * each viewable pill, trimmed at the map edges, the tank rect always first and
 * pills after it in index order, and never more rects than the caller asked
 * for. Where the tank's own block goes is the fog experiment's to decide and
 * overviewMapUpdate's to place, so the last cases there run through the update:
 * Envelope puts the 29x29 on the tank as it always has, and the lens puts a
 * 15x15 at the classic view instead, falling back to the tank when there is no
 * view to read and holding the view a dying tank last had. The two Headlights
 * centre their block on the tank whatever the classic view is doing - the
 * 29x29 for one and the 15x15 for the other - and light the squares within two
 * of the tank plus a wedge along the facing, leaving the rest of the block
 * hidden.
 * overviewMapDeathBlackout is where the overview goes black inside a
 * death wait: from the tick the classic view cuts to static through to the
 * respawn, never dropping once it is up, and a drowning given longer to watch
 * than a shell.
 *
 * The rest drive a real ClientSim through clientSimDisplayTick and check what
 * the memory ends up holding: a map install seeds every square dimmed on the
 * next tick and only live squares are bright over it, a square outside the
 * live set keeps the tile it was seeded or last stamped with and never picks
 * up a later terrain change the client already knows about, a region that
 * stops being live is stamped once more on the way out so a pill freezes dead
 * or in the captor's colours rather than a tick stale, a tank waiting to
 * respawn holds its block whole on the square it died on rather than dropping
 * it where its position now reads, and a round reset clears the lot while a
 * mid-game map resync leaves it alone and a fresh install re-seeds it.
 *
 * One case steps outside the memory to the accessor that feeds it,
 * clientSimGetMyTankMapPos, and pins what it reports for a tank that is dead
 * and waiting to respawn.
 *
 * Another steps forward to what the memory is for: the per-frame entity lists
 * the overview draws from cover the whole map, so an enemy tank the client
 * still knows about has to be dropped by the live-square filter once it
 * leaves the block the player can see. The player's own tank goes by the same
 * test — watching a pill under viewPolicyKey closes the block round the tank,
 * and the tank is filtered out on its own square until the view is left.
 *
 * Two more drive the client's own decay clocks: a display tick stamps the
 * items the tank is beside and only for the categories on viewPolicyDecay, it
 * skips an ally whose players entry has been zeroed by a hidden stub rather
 * than reading that as the map origin, an item's block appears while its clock
 * is inside the window, fades over the end of it and freezes what it was
 * showing once it runs out, and the clocks start over with the round. The view exit reads the same window, so an item
 * view whose clock has run out drops back to the tank and cannot be entered
 * again until the player has been near the item. The region build those two
 * lean on is pinned in test_overview_view_policy.c.
 *
 * One more covers the accessor the overview draws its own crosshair from,
 * clientSimGetGunsightPos: it declines for a hidden sight and for a tank that
 * is dead and waiting to respawn, where clientSimGetGunsightTile still answers
 * with the map origin.
 *
 * The last case runs the reveal checks again over the real UDP transport, where
 * the map arrives as a download and terrain changes arrive out of band as map
 * events, so the mask does not quietly depend on the client and the server
 * sharing a process.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "server_sim.h"
#include "game_sim.h"
#include "client_sim.h"
#include "client_net.h"
#include "client_connect_state.h" /* CLIENT_CONNECT_CONNECTED */
#include "client_sim_internal.h" /* installCompressedMap — the resync path */
#include "input_packet.h"
#include "transport_udp.h" /* the server-side staged map event and its readiness flag */
#include "overview_map.h"
#include "viewport.h" /* viewportCalcSquarePure — the only tile source */
#include "bolo_map.h" /* mapGetPos / mapSetPos — plant a raw terrain byte */
#include "tank.h"     /* tankSetWorld / tankDestroy */
#include "bases.h"
#include "pillbox.h"
#include "players.h"
#include "allience.h"
#include "everard_map.h" /* E_MAP — the blob the resync reinstalls */
#include "overview_camera.h" /* overviewEntityIsVisible — the drawing filter */
#include "test_harness.h"
#include "loopback_harness.h"

/* Fails the calling test unless the rect is exactly these edges. */
#define ASSERT_RECT(r, l, t, rt, b)                                           \
    UT_ASSERT_MSG((r).left == (l) && (r).top == (t) && (r).right == (rt) &&    \
                      (r).bottom == (b),                                      \
                  "rect {%d,%d,%d,%d}, expected {%d,%d,%d,%d}",                \
                  (r).left, (r).top, (r).right, (r).bottom, (l), (t), (rt), (b))

/* An inclusive block round a centre, trimmed at the map edges: both the tank
 * rect a caller hands overviewMapBuildRegions and, restated, something for the
 * assertions to check the built regions against that is not the code under
 * test. Zeroed whole so a rect handed in comes back out comparable byte for
 * byte. run_overview_regions is what pins the two together. */
static OverviewRect overviewTestBlock(int cx, int cy, int half) {
    OverviewRect r; /* Rect to return */

    memset(&r, 0, sizeof(r));
    r.alpha = 255;
    r.left = cx - half;
    r.top = cy - half;
    r.right = cx + half;
    r.bottom = cy + half;
    if (r.left < 0) {
        r.left = 0;
    }
    if (r.top < 0) {
        r.top = 0;
    }
    if (r.right > MAP_ARRAY_SIZE - 1) {
        r.right = MAP_ARRAY_SIZE - 1;
    }
    if (r.bottom > MAP_ARRAY_SIZE - 1) {
        r.bottom = MAP_ARRAY_SIZE - 1;
    }
    return r;
}

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
    OverviewRect tank; /* The block the caller hands the build */
    int n;

    /* The rules a server ships with, which is what every case here is about:
     * pillboxes always, bases off, allied tanks always with nobody viewable.
     * test_overview_view_policy.c is where the other policies are pinned. */
    OverviewViewInputs in;
    overviewViewInputsDefaults(&in);

    /* Tank alone, well clear of every edge: one 29x29 rect on it. */
    tank = overviewTestBlock(100, 100, OVERVIEW_TANK_HALF);
    n = overviewMapBuildRegions(gs, 0, &in, NULL, &tank, out,
                                OVERVIEW_MAX_REGIONS);
    UT_ASSERT_MSG(n == 1, "tank with no pills gave %d regions, expected 1", n);
    ASSERT_RECT(out[0], 86, 86, 114, 114);

    /* Over the left and bottom edges the block is trimmed, not wrapped. */
    tank = overviewTestBlock(3, 250, OVERVIEW_TANK_HALF);
    n = overviewMapBuildRegions(gs, 0, &in, NULL, &tank, out,
                                OVERVIEW_MAX_REGIONS);
    UT_ASSERT_MSG(n == 1, "corner tank gave %d regions, expected 1", n);
    ASSERT_RECT(out[0], 0, 236, 17, 255);

    /* A narrower block is the same rect drawn smaller, centred where it was;
     * at nothing left it is the single square the tank is on, and no rect at
     * all means no region. */
    tank = overviewTestBlock(100, 100, 4);
    n = overviewMapBuildRegions(gs, 0, &in, NULL, &tank, out,
                                OVERVIEW_MAX_REGIONS);
    UT_ASSERT_MSG(n == 1, "a narrowed block gave %d regions, expected 1", n);
    ASSERT_RECT(out[0], 96, 96, 104, 104);

    tank = overviewTestBlock(100, 100, 0);
    n = overviewMapBuildRegions(gs, 0, &in, NULL, &tank, out,
                                OVERVIEW_MAX_REGIONS);
    UT_ASSERT_MSG(n == 1, "a single-square block gave %d regions, expected 1",
                  n);
    ASSERT_RECT(out[0], 100, 100, 100, 100);

    n = overviewMapBuildRegions(gs, 0, &in, NULL, NULL, out,
                                OVERVIEW_MAX_REGIONS);
    UT_ASSERT_MSG(n == 0, "a block that has gone gave %d regions, expected 0",
                  n);

    /* One pill of the player's own, alive and on the map. The tank's rect
     * comes first and the pill's follows it — the order the farewell stamp
     * replays. */
    gs->pb->numPills = 1;
    gs->pb->item[0].owner = 0;
    gs->pb->item[0].armour = PILLBOX_15;
    gs->pb->item[0].inTank = FALSE;
    gs->pb->item[0].x = 200;
    gs->pb->item[0].y = 50;

    tank = overviewTestBlock(100, 100, OVERVIEW_TANK_HALF);
    n = overviewMapBuildRegions(gs, 0, &in, NULL, &tank, out,
                                OVERVIEW_MAX_REGIONS);
    UT_ASSERT_MSG(n == 2, "tank + own pill gave %d regions, expected 2", n);
    ASSERT_RECT(out[0], 86, 86, 114, 114);
    ASSERT_RECT(out[1], 193, 43, 207, 57);

    /* An allied owner views the same as an own one. */
    gs->pb->item[0].owner = 1;
    n = overviewMapBuildRegions(gs, 0, &in, NULL, &tank, out,
                                OVERVIEW_MAX_REGIONS);
    UT_ASSERT_MSG(n == 2, "allied pill gave %d regions, expected 2", n);
    ASSERT_RECT(out[1], 193, 43, 207, 57);

    /* A pill the player cannot view through contributes nothing: owned by
     * someone hostile, or dead, or carried in a tank. */
    gs->pb->item[0].owner = 2;
    n = overviewMapBuildRegions(gs, 0, &in, NULL, &tank, out,
                                OVERVIEW_MAX_REGIONS);
    UT_ASSERT_MSG(n == 1, "enemy pill gave %d regions, expected 1", n);

    gs->pb->item[0].owner = 0;
    gs->pb->item[0].armour = 0;
    n = overviewMapBuildRegions(gs, 0, &in, NULL, &tank, out,
                                OVERVIEW_MAX_REGIONS);
    UT_ASSERT_MSG(n == 1, "dead pill gave %d regions, expected 1", n);

    gs->pb->item[0].armour = PILLBOX_15;
    gs->pb->item[0].inTank = TRUE;
    n = overviewMapBuildRegions(gs, 0, &in, NULL, &tank, out,
                                OVERVIEW_MAX_REGIONS);
    UT_ASSERT_MSG(n == 1, "carried pill gave %d regions, expected 1", n);

    /* No tank: a viewable pill still gives the player its block, and it is
     * the only rect. */
    gs->pb->item[0].inTank = FALSE;
    n = overviewMapBuildRegions(gs, 0, &in, NULL, NULL, out,
                                OVERVIEW_MAX_REGIONS);
    UT_ASSERT_MSG(n == 1, "pill without a tank gave %d regions, expected 1", n);
    ASSERT_RECT(out[0], 193, 43, 207, 57);

    /* No tank and nothing to view through: no live squares at all. */
    gs->pb->item[0].armour = 0;
    n = overviewMapBuildRegions(gs, 0, &in, NULL, NULL, out,
                                OVERVIEW_MAX_REGIONS);
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

        tank = overviewTestBlock(100, 100, OVERVIEW_TANK_HALF);
        n = overviewMapBuildRegions(gs, 0, &in, NULL, &tank, out, maxOut);
        UT_ASSERT_MSG(n == maxOut, "capped build returned %d, expected %d", n,
                      maxOut);
        ASSERT_RECT(out[maxOut], -1, -1, -1, -1);
    }

    /* Which block the tank gets is the fog experiment's, and overviewMapUpdate
     * is the only thing that places it, so these run through it. The memory is
     * a hundred kilobytes and more, too much to put on the stack. */
    {
        OverviewMap *om;        /* The memory the update writes */
        OverviewViewInputs fog; /* Inputs carrying the experiment and the view */

        om = (OverviewMap *)calloc(1, sizeof(OverviewMap));
        UT_ASSERT_MSG(om != NULL, "no memory for an OverviewMap");
        gs->pb->numPills = 0;

        /* Envelope with the view fields zeroed, which is what a caller that
         * keeps no view state passes: the 29x29 on the tank, where it has
         * always been. */
        overviewMapReset(om);
        overviewViewInputsDefaults(&fog);
        overviewMapUpdate(om, gs, 0, &fog, TRUE, 0, 100, 100);
        UT_ASSERT_MSG(om->liveCount == 1,
                      "Envelope gave %d regions, expected 1", om->liveCount);
        ASSERT_RECT(om->live[0], 86, 86, 114, 114);

        /* Lens with a classic view to place from: the 15x15 sits at the view,
         * not at the tank, which is a hundred squares off it. */
        overviewMapReset(om);
        overviewViewInputsDefaults(&fog);
        fog.experiment = (uint8_t)fogExperimentLens;
        fog.viewValid = TRUE;
        fog.viewLeft = 40;
        fog.viewTop = 60;
        overviewMapUpdate(om, gs, 0, &fog, TRUE, 0, 100, 100);
        UT_ASSERT_MSG(om->liveCount == 1, "Lens gave %d regions, expected 1",
                      om->liveCount);
        ASSERT_RECT(om->live[0], 40, 60, 54, 74);

        /* Lens with no view to read - a dead tank, or an item view just left -
         * puts the same 15x15 round the tank instead. */
        overviewMapReset(om);
        overviewViewInputsDefaults(&fog);
        fog.experiment = (uint8_t)fogExperimentLens;
        overviewMapUpdate(om, gs, 0, &fog, TRUE, 0, 100, 100);
        UT_ASSERT_MSG(om->liveCount == 1,
                      "Lens without a view gave %d regions, expected 1",
                      om->liveCount);
        ASSERT_RECT(om->live[0], 93, 93, 107, 107);

        /* A lens over an edge is trimmed to the map, the way the Envelope
         * block over the corner above is, and so is the fallback round a tank
         * standing near one. */
        overviewMapReset(om);
        overviewViewInputsDefaults(&fog);
        fog.experiment = (uint8_t)fogExperimentLens;
        fog.viewValid = TRUE;
        fog.viewLeft = 250;
        fog.viewTop = 0;
        overviewMapUpdate(om, gs, 0, &fog, TRUE, 0, 100, 100);
        UT_ASSERT_MSG(om->liveCount == 1,
                      "a lens at the edge gave %d regions, expected 1",
                      om->liveCount);
        ASSERT_RECT(om->live[0], 250, 0, 255, 14);

        overviewMapReset(om);
        overviewViewInputsDefaults(&fog);
        fog.experiment = (uint8_t)fogExperimentLens;
        overviewMapUpdate(om, gs, 0, &fog, TRUE, 0, 3, 250);
        ASSERT_RECT(om->live[0], 0, 243, 10, 255);

        /* A tank that dies holds its block where the player was looking. The
         * dead tank reports no view of its own, so the block has to come from
         * what was recorded on the last update it was alive for - at the
         * wreck it would be the 15x15 round 100,100. */
        overviewMapReset(om);
        overviewViewInputsDefaults(&fog);
        fog.experiment = (uint8_t)fogExperimentLens;
        fog.viewValid = TRUE;
        fog.viewLeft = 40;
        fog.viewTop = 60;
        overviewMapUpdate(om, gs, 0, &fog, TRUE, 0, 100, 100);
        fog.viewValid = FALSE;
        fog.viewLeft = 0;
        fog.viewTop = 0;
        overviewMapUpdate(om, gs, 0, &fog, FALSE, TANK_DEATH_WAIT, 0, 0);
        UT_ASSERT_MSG(om->liveCount == 1,
                      "a dead tank gave %d regions, expected 1",
                      om->liveCount);
        ASSERT_RECT(om->live[0], 40, 60, 54, 74);

        /* Both Headlights blocks are centred on the tank and nowhere else: the
         * envelope-sized one is the 29x29 Envelope draws, the lens-sized one
         * the classic window's 15x15, and a classic view sitting a long way off
         * moves neither. What the beam does inside the block is the case after
         * this one; this is the rect it works over. */
        overviewMapReset(om);
        overviewViewInputsDefaults(&fog);
        fog.experiment = (uint8_t)fogExperimentHeadlightsEnvelope;
        fog.facing = 4; /* east */
        fog.viewValid = TRUE;
        fog.viewLeft = 40;
        fog.viewTop = 60;
        overviewMapUpdate(om, gs, 0, &fog, TRUE, 0, 100, 100);
        UT_ASSERT_MSG(om->liveCount == 1,
                      "Headlights over the envelope gave %d regions, "
                      "expected 1", om->liveCount);
        ASSERT_RECT(om->live[0], 86, 86, 114, 114);

        overviewMapReset(om);
        overviewViewInputsDefaults(&fog);
        fog.experiment = (uint8_t)fogExperimentHeadlightsLens;
        fog.facing = 4; /* east */
        fog.viewValid = TRUE;
        fog.viewLeft = 40;
        fog.viewTop = 60;
        overviewMapUpdate(om, gs, 0, &fog, TRUE, 0, 100, 100);
        UT_ASSERT_MSG(om->liveCount == 1,
                      "Headlights over the lens gave %d regions, expected 1",
                      om->liveCount);
        ASSERT_RECT(om->live[0], 93, 93, 107, 107);

        /* A block over an edge is trimmed to the map, not wrapped through it,
         * the way every other block is. */
        overviewMapReset(om);
        overviewViewInputsDefaults(&fog);
        fog.experiment = (uint8_t)fogExperimentHeadlightsLens;
        fog.facing = 4; /* east, into the right-hand edge */
        overviewMapUpdate(om, gs, 0, &fog, TRUE, 0, 250, 100);
        ASSERT_RECT(om->live[0], 243, 93, 255, 107);

        overviewMapReset(om);
        overviewViewInputsDefaults(&fog);
        fog.experiment = (uint8_t)fogExperimentHeadlightsEnvelope;
        fog.facing = 0; /* north, into the top-left corner */
        overviewMapUpdate(om, gs, 0, &fog, TRUE, 0, 3, 250);
        ASSERT_RECT(om->live[0], 0, 236, 17, 255);

        /* And the shape inside the block, read off the flags: the squares
         * within two of the tank are live whichever way it points, and past
         * that only the beam along the facing is - everything else in the rect
         * is hidden, so it keeps the tile it last showed and nothing moving on
         * it is drawn.
         *
         * The squares are written out rather than worked back out of the facing
         * table, so the case says which ones should be lit instead of restating
         * how the code works them out. Four facings: two axes, a diagonal, and
         * one of the sixteenths between them, which is where a beam that was
         * not the same width on every facing would show. */
        {
            static const struct {
                BYTE facing;           /* Which way the tank points */
                int  aheadX, aheadY;   /* Well along the facing */
                int  behindX, behindY; /* The same distance the other way */
                int  sideX, sideY;     /* And the same square to it */
            } kBeams[] = {
                { 0,  0, -8,   0,  8,   8,  0}, /* north */
                { 4,  8,  0,  -8,  0,   0,  8}, /* east */
                { 2,  6, -6,  -6,  6,   6,  6}, /* north-east */
                { 1,  3, -7,  -3,  7,   7,  3}  /* north-north-east */
            };
            const int kBeamCount = (int)(sizeof(kBeams) / sizeof(kBeams[0]));
            int b;  /* Looping variable */
            int f;  /* Looping variable */
            int dx; /* Looping variable */
            int dy; /* Looping variable */
            int mx; /* Looping variable */
            int my; /* Looping variable */

            /* The near squares first, under every one of the sixteen facings:
             * the tank's own square is in there, so the reticle is never
             * dropped, and so is every square of the 5x5 round it. */
            for (f = 0; f < 16; f++) {
                overviewMapReset(om);
                overviewViewInputsDefaults(&fog);
                fog.experiment = (uint8_t)fogExperimentHeadlightsEnvelope;
                fog.facing = (BYTE)f;
                overviewMapUpdate(om, gs, 0, &fog, TRUE, 0, 100, 100);
                for (dx = -OVERVIEW_HEADLIGHT_NEAR;
                     dx <= OVERVIEW_HEADLIGHT_NEAR; dx++) {
                    for (dy = -OVERVIEW_HEADLIGHT_NEAR;
                         dy <= OVERVIEW_HEADLIGHT_NEAR; dy++) {
                        BYTE flags = om->flags[100 + dx][100 + dy];

                        UT_ASSERT_MSG((flags & OVERVIEW_F_LIVE) != 0 &&
                                          (flags & OVERVIEW_F_HIDDEN) == 0,
                                      "facing %d, square %d,%d from the tank "
                                      "carries flags 0x%02X, expected it live "
                                      "whichever way the tank points",
                                      f, dx, dy, (unsigned)flags);
                    }
                }
            }

            /* Then the beam itself. Ahead is lit out to eight squares, behind
             * is not, and neither is the square-on direction: the beam is a
             * wedge along the facing rather than a ring or a half. */
            for (b = 0; b < kBeamCount; b++) {
                BYTE ahead;  /* Flags well along the facing */
                BYTE behind; /* and behind */
                BYTE side;   /* and square to it */

                overviewMapReset(om);
                overviewViewInputsDefaults(&fog);
                fog.experiment = (uint8_t)fogExperimentHeadlightsEnvelope;
                fog.facing = kBeams[b].facing;
                overviewMapUpdate(om, gs, 0, &fog, TRUE, 0, 100, 100);

                ahead = om->flags[100 + kBeams[b].aheadX]
                                 [100 + kBeams[b].aheadY];
                behind = om->flags[100 + kBeams[b].behindX]
                                  [100 + kBeams[b].behindY];
                side = om->flags[100 + kBeams[b].sideX]
                                [100 + kBeams[b].sideY];

                UT_ASSERT_MSG((ahead & (OVERVIEW_F_LIVE | OVERVIEW_F_SIGHT)) ==
                                      (OVERVIEW_F_LIVE | OVERVIEW_F_SIGHT) &&
                                  (ahead & OVERVIEW_F_HIDDEN) == 0,
                              "facing %u, the square %d,%d along it carries "
                              "flags 0x%02X, expected it lit by the beam",
                              (unsigned)kBeams[b].facing, kBeams[b].aheadX,
                              kBeams[b].aheadY, (unsigned)ahead);
                UT_ASSERT_MSG((behind & OVERVIEW_F_HIDDEN) != 0 &&
                                  (behind &
                                   (OVERVIEW_F_LIVE | OVERVIEW_F_SIGHT)) == 0,
                              "facing %u, the square %d,%d behind it carries "
                              "flags 0x%02X, expected it hidden",
                              (unsigned)kBeams[b].facing, kBeams[b].behindX,
                              kBeams[b].behindY, (unsigned)behind);
                UT_ASSERT_MSG((side & OVERVIEW_F_HIDDEN) != 0 &&
                                  (side &
                                   (OVERVIEW_F_LIVE | OVERVIEW_F_SIGHT)) == 0,
                              "facing %u, the square %d,%d square to it carries "
                              "flags 0x%02X, expected it hidden",
                              (unsigned)kBeams[b].facing, kBeams[b].sideX,
                              kBeams[b].sideY, (unsigned)side);
            }

            /* The near squares end where they say they do: one square further
             * out than the ring, off the beam, is hidden like the rest of the
             * block. */
            overviewMapReset(om);
            overviewViewInputsDefaults(&fog);
            fog.experiment = (uint8_t)fogExperimentHeadlightsEnvelope;
            fog.facing = 0; /* north */
            overviewMapUpdate(om, gs, 0, &fog, TRUE, 0, 100, 100);
            UT_ASSERT_MSG(
                (om->flags[100 + OVERVIEW_HEADLIGHT_NEAR + 1][100] &
                 OVERVIEW_F_HIDDEN) != 0,
                "the square just outside the near ring and off the beam "
                "carries flags 0x%02X, expected it hidden",
                (unsigned)om->flags[100 + OVERVIEW_HEADLIGHT_NEAR + 1][100]);
            UT_ASSERT_MSG(om->hiddenActive == TRUE,
                          "the map did not record the beam hiding squares");

            /* Nothing outside the block is touched by the beam, and the beam
             * reaches the block's own edge: on the lens-sized one that is seven
             * squares out rather than fourteen. */
            overviewMapReset(om);
            overviewViewInputsDefaults(&fog);
            fog.experiment = (uint8_t)fogExperimentHeadlightsLens;
            fog.facing = 4; /* east */
            overviewMapUpdate(om, gs, 0, &fog, TRUE, 0, 100, 100);
            UT_ASSERT_MSG((om->flags[107][100] &
                           (OVERVIEW_F_LIVE | OVERVIEW_F_SIGHT)) ==
                              (OVERVIEW_F_LIVE | OVERVIEW_F_SIGHT),
                          "the square at the lens edge along the beam carries "
                          "flags 0x%02X, expected it lit",
                          (unsigned)om->flags[107][100]);
            for (mx = 0; mx < MAP_ARRAY_SIZE; mx++) {
                for (my = 0; my < MAP_ARRAY_SIZE; my++) {
                    if ((om->flags[mx][my] & OVERVIEW_F_HIDDEN) == 0) {
                        continue;
                    }
                    UT_ASSERT_MSG(mx >= 93 && mx <= 107 && my >= 93 &&
                                      my <= 107,
                                  "square %d,%d is hidden and is outside the "
                                  "block round the tank", mx, my);
                }
            }
        }

        /* Halo is the lens with a wider block of ground under it. Two rects:
         * the halo first, round the tank, at half brightness and granting no
         * sight, then the lens at the view, whole. The order decides what a
         * square both cover ends up with, because the stamp assigns the flags
         * rather than merging them and the lens goes last. */
        overviewMapReset(om);
        overviewViewInputsDefaults(&fog);
        fog.experiment = (uint8_t)fogExperimentHalo;
        fog.viewValid = TRUE;
        fog.viewLeft = 96;
        fog.viewTop = 96;
        overviewMapUpdate(om, gs, 0, &fog, TRUE, 0, 100, 100);
        UT_ASSERT_MSG(om->liveCount == 2, "Halo gave %d regions, expected 2",
                      om->liveCount);
        ASSERT_RECT(om->live[0], 86, 86, 114, 114);
        ASSERT_RECT(om->live[1], 96, 96, 110, 110);
        UT_ASSERT_MSG(om->live[0].alpha == OVERVIEW_HALO_ALPHA &&
                          om->live[0].terrainOnly == 1,
                      "the halo came out at alpha %u terrainOnly %u, expected "
                      "%u and 1", (unsigned)om->live[0].alpha,
                      (unsigned)om->live[0].terrainOnly,
                      (unsigned)OVERVIEW_HALO_ALPHA);
        UT_ASSERT_MSG(om->live[1].alpha == 255 &&
                          om->live[1].terrainOnly == 0,
                      "the lens inside the halo came out at alpha %u "
                      "terrainOnly %u, expected 255 and 0",
                      (unsigned)om->live[1].alpha,
                      (unsigned)om->live[1].terrainOnly);

        /* A square both cover is live and in sight, so what is standing on it
         * is drawn; one only the halo covers is live ground with nothing
         * moving shown on it. */
        UT_ASSERT_MSG((om->flags[100][100] &
                       (OVERVIEW_F_LIVE | OVERVIEW_F_SIGHT)) ==
                          (OVERVIEW_F_LIVE | OVERVIEW_F_SIGHT),
                      "a square in both blocks carries flags 0x%02X, expected "
                      "live and sight", (unsigned)om->flags[100][100]);
        UT_ASSERT_MSG(overviewEntityIsVisible(om, 100, 100) == true,
                      "an enemy inside the lens would not be drawn");
        UT_ASSERT_MSG((om->flags[88][88] & OVERVIEW_F_LIVE) != 0 &&
                          (om->flags[88][88] & OVERVIEW_F_SIGHT) == 0,
                      "a square only the halo covers carries flags 0x%02X, "
                      "expected live without sight",
                      (unsigned)om->flags[88][88]);
        UT_ASSERT_MSG(overviewEntityIsVisible(om, 88, 88) == false,
                      "an enemy in the halo but outside the lens would be "
                      "drawn");

        /* Under every other experiment the two bits go together over the whole
         * map, which is what keeps the plain live assertions elsewhere in this
         * file saying the same thing about entities as they always did. */
        overviewMapReset(om);
        overviewViewInputsDefaults(&fog);
        overviewMapUpdate(om, gs, 0, &fog, TRUE, 0, 100, 100);
        {
            int x; /* Looping variable */
            int y; /* Looping variable */

            for (x = 0; x < MAP_ARRAY_SIZE; x++) {
                for (y = 0; y < MAP_ARRAY_SIZE; y++) {
                    BYTE flags = om->flags[x][y];

                    UT_ASSERT_MSG(((flags & OVERVIEW_F_LIVE) != 0) ==
                                      ((flags & OVERVIEW_F_SIGHT) != 0),
                                  "square %d,%d carries flags 0x%02X under "
                                  "Envelope, where live and sight go together",
                                  x, y, (unsigned)flags);
                }
            }
        }

        /* Afterimage is the lens with a memory: a square the block has left
         * fades back to fog a tick at a time instead of going dark the moment
         * it leaves. What is stored is the ticks the square has left, so the
         * span is the seconds times the caller's tick rate - a short rate
         * keeps the case to a handful of updates and says the same thing the
         * game's fifty a second does. */
        {
            unsigned span;      /* Ticks a square takes to fade right out */
            unsigned genBefore; /* generation before an update */
            int t;              /* Updates since the block left the square */

            span = (unsigned)OVERVIEW_AFTERIMAGE_SECS * 2;

            overviewMapReset(om);
            overviewViewInputsDefaults(&fog);
            fog.experiment = (uint8_t)fogExperimentAfterimage;
            fog.ticksPerSec = 2;
            fog.viewValid = TRUE;
            fog.viewLeft = 40;
            fog.viewTop = 60;
            overviewMapUpdate(om, gs, 0, &fog, TRUE, 0, 100, 100);
            UT_ASSERT_MSG(om->fadeSpan == (BYTE)span,
                          "Afterimage counts down from %u, expected %u",
                          (unsigned)om->fadeSpan, span);
            UT_ASSERT_MSG(om->fade[41][61] == (BYTE)span,
                          "a square inside the block is %u ticks from fog, "
                          "expected the whole span of %u",
                          (unsigned)om->fade[41][61], span);
            UT_ASSERT_MSG(om->fade[200][200] == 0,
                          "a square the block has never covered is %u ticks "
                          "from fog, expected 0",
                          (unsigned)om->fade[200][200]);

            /* The block moves right off it, and from there it steps down one a
             * tick until it is out. From the second update on nothing but the
             * fade is moving, so generation advancing is the fade reporting
             * itself - which is what keeps the fog being redrawn while the
             * ground dissolves. */
            fog.viewLeft = 100;
            for (t = 1; t <= (int)span; t++) {
                genBefore = om->generation;
                overviewMapUpdate(om, gs, 0, &fog, TRUE, 0, 100, 100);
                UT_ASSERT_MSG(om->fade[41][61] == (BYTE)(span - (unsigned)t),
                              "%d updates after the block left, the square is "
                              "%u ticks from fog, expected %u",
                              t, (unsigned)om->fade[41][61],
                              span - (unsigned)t);
                if (t >= 3) {
                    UT_ASSERT_MSG(om->generation > genBefore,
                                  "generation stuck at %u over an update where "
                                  "the fade moved", genBefore);
                }
            }

            /* And there it stays, with nothing left to redraw for. */
            genBefore = om->generation;
            overviewMapUpdate(om, gs, 0, &fog, TRUE, 0, 100, 100);
            UT_ASSERT_MSG(om->fade[41][61] == 0,
                          "a square past the end of its fade is %u ticks from "
                          "fog, expected 0", (unsigned)om->fade[41][61]);
            UT_ASSERT_MSG(om->generation == genBefore,
                          "generation went from %u to %u over an update where "
                          "nothing was fading", genBefore, om->generation);

            /* Leaving the experiment drops the trail on the tick it is left,
             * so coming back to it later does not show ground the player was
             * looking at minutes ago. */
            fog.experiment = (uint8_t)fogExperimentLens;
            overviewMapUpdate(om, gs, 0, &fog, TRUE, 0, 100, 100);
            UT_ASSERT_MSG(om->fadeSpan == 0,
                          "the span survived the experiment being left at %u",
                          (unsigned)om->fadeSpan);
            {
                int x; /* Looping variable */
                int y; /* Looping variable */

                for (x = 0; x < MAP_ARRAY_SIZE; x++) {
                    for (y = 0; y < MAP_ARRAY_SIZE; y++) {
                        UT_ASSERT_MSG(om->fade[x][y] == 0,
                                      "square %d,%d is %u ticks from fog after "
                                      "the experiment was left, expected 0",
                                      x, y, (unsigned)om->fade[x][y]);
                    }
                }
            }

            /* Every other experiment leaves both alone: a span of 0 is what
             * says nothing is fading, and nothing but Afterimage writes a
             * square. */
            {
                static const uint8_t kOthers[] = {
                    (uint8_t)fogExperimentEnvelope,
                    (uint8_t)fogExperimentLens,
                    (uint8_t)fogExperimentHeadlightsEnvelope,
                    (uint8_t)fogExperimentHeadlightsLens,
                    (uint8_t)fogExperimentHalo
                };
                int i; /* Looping variable */
                int x; /* Looping variable */
                int y; /* Looping variable */

                for (i = 0; i < (int)(sizeof(kOthers) / sizeof(kOthers[0]));
                     i++) {
                    overviewMapReset(om);
                    overviewViewInputsDefaults(&fog);
                    fog.experiment = kOthers[i];
                    fog.ticksPerSec = 2;
                    fog.viewValid = TRUE;
                    fog.viewLeft = 40;
                    fog.viewTop = 60;
                    overviewMapUpdate(om, gs, 0, &fog, TRUE, 0, 100, 100);
                    UT_ASSERT_MSG(om->fadeSpan == 0,
                                  "experiment %u came out with a span of %u, "
                                  "expected nothing fading",
                                  (unsigned)kOthers[i],
                                  (unsigned)om->fadeSpan);
                    for (x = 0; x < MAP_ARRAY_SIZE; x++) {
                        for (y = 0; y < MAP_ARRAY_SIZE; y++) {
                            UT_ASSERT_MSG(om->fade[x][y] == 0,
                                          "experiment %u left square %d,%d %u "
                                          "ticks from fog, expected 0",
                                          (unsigned)kOthers[i], x, y,
                                          (unsigned)om->fade[x][y]);
                        }
                    }
                }
            }

            /* No tick rate is no way to measure the seconds, so it reads as
             * the experiment being off rather than as a fade of no length. */
            overviewMapReset(om);
            overviewViewInputsDefaults(&fog);
            fog.experiment = (uint8_t)fogExperimentAfterimage;
            fog.viewValid = TRUE;
            fog.viewLeft = 40;
            fog.viewTop = 60;
            overviewMapUpdate(om, gs, 0, &fog, TRUE, 0, 100, 100);
            UT_ASSERT_MSG(om->fadeSpan == 0,
                          "Afterimage with no tick rate came out with a span "
                          "of %u, expected nothing fading",
                          (unsigned)om->fadeSpan);
            UT_ASSERT_MSG(om->fade[41][61] == 0,
                          "Afterimage with no tick rate left a square %u ticks "
                          "from fog, expected 0",
                          (unsigned)om->fade[41][61]);
        }

        /* Line of sight, which rides on top of whichever experiment is
         * running: inside the blocks round the tank a square with a building
         * between it and the tank keeps the tile it last showed and has
         * nothing drawn moving on it. The wall itself is seen - what is hidden
         * is the ground behind it - and a watched item's block is not masked
         * at all, because the player is seeing through the item rather than
         * from the tank. */
        {
            BYTE heldTile; /* The tile the hidden square is holding on to */
            int  x;        /* Looping variable */
            int  y;        /* Looping variable */

            /* A clear line east from 100,100 with one building across it, and
             * a pillbox of the player's own far enough off to have a block of
             * its own that the tank's wall could only reach through the mask.
             */
            mapSetPos(gs, &gs->mp, 101, 100, GRASS, TRUE, TRUE);
            mapSetPos(gs, &gs->mp, 102, 100, GRASS, TRUE, TRUE);
            mapSetPos(gs, &gs->mp, 104, 100, GRASS, TRUE, TRUE);
            mapSetPos(gs, &gs->mp, 105, 100, GRASS, TRUE, TRUE);
            mapSetPos(gs, &gs->mp, 106, 100, GRASS, TRUE, TRUE);
            mapSetPos(gs, &gs->mp, 103, 100, BUILDING, TRUE, TRUE);
            gs->pb->numPills = 1;
            gs->pb->item[0].owner = 0;
            gs->pb->item[0].armour = PILLBOX_15;
            gs->pb->item[0].inTank = FALSE;
            gs->pb->item[0].x = 200;
            gs->pb->item[0].y = 100;

            /* With the toggle off the block is stamped the way it always was,
             * which is what leaves the square a tile to hold on to. */
            overviewMapReset(om);
            overviewViewInputsDefaults(&fog);
            overviewMapUpdate(om, gs, 0, &fog, TRUE, 0, 100, 100);
            UT_ASSERT_MSG(om->hiddenActive == FALSE,
                          "the map hid squares with the toggle off");
            heldTile = om->tile[106][100];
            for (x = 0; x < MAP_ARRAY_SIZE; x++) {
                for (y = 0; y < MAP_ARRAY_SIZE; y++) {
                    UT_ASSERT_MSG((om->flags[x][y] & OVERVIEW_F_HIDDEN) == 0,
                                  "square %d,%d is hidden with line of sight "
                                  "off", x, y);
                }
            }

            /* And with it on the ground behind the building drops out of the
             * block while the building itself stays in it. */
            fog.sightMode = (uint8_t)fogSightBuildings;
            overviewMapUpdate(om, gs, 0, &fog, TRUE, 0, 100, 100);
            UT_ASSERT_MSG(om->hiddenActive == TRUE,
                          "the map hid nothing with the toggle on");
            UT_ASSERT_MSG((om->flags[106][100] & OVERVIEW_F_HIDDEN) != 0 &&
                              (om->flags[106][100] &
                               (OVERVIEW_F_LIVE | OVERVIEW_F_SIGHT)) == 0,
                          "the square behind the building carries flags "
                          "0x%02X, expected hidden with neither live nor "
                          "sight", (unsigned)om->flags[106][100]);
            UT_ASSERT_MSG((om->flags[103][100] &
                           (OVERVIEW_F_LIVE | OVERVIEW_F_SIGHT)) ==
                                  (OVERVIEW_F_LIVE | OVERVIEW_F_SIGHT) &&
                              (om->flags[103][100] & OVERVIEW_F_HIDDEN) == 0,
                          "the building itself carries flags 0x%02X, expected "
                          "live and in sight", (unsigned)om->flags[103][100]);
            UT_ASSERT_MSG((om->flags[100][100] & OVERVIEW_F_SIGHT) != 0 &&
                              (om->flags[100][100] & OVERVIEW_F_HIDDEN) == 0,
                          "the tank's own square carries flags 0x%02X, so the "
                          "reticle would not be drawn",
                          (unsigned)om->flags[100][100]);

            /* Nothing outside the block round the tank is ever hidden, which
             * is the watched pill's block as much as ground no region covers:
             * the wall in front of the tank says nothing about what the pill
             * can see. */
            for (x = 0; x < MAP_ARRAY_SIZE; x++) {
                for (y = 0; y < MAP_ARRAY_SIZE; y++) {
                    if ((om->flags[x][y] & OVERVIEW_F_HIDDEN) == 0) {
                        continue;
                    }
                    UT_ASSERT_MSG(x >= 86 && x <= 114 && y >= 86 && y <= 114,
                                  "square %d,%d is hidden and is outside the "
                                  "block round the tank", x, y);
                }
            }

            /* The terrain behind the building changes and the memory does not:
             * holding the tile it last showed is the whole of the effect. The
             * change is put past the per-square calculator first, so a square
             * whose tile would not have moved anyway fails here instead of
             * passing for the wrong reason. */
            {
                bool isMine = FALSE;
                BYTE before = viewportCalcSquarePure(gs, 0, 106, 100, &isMine);
                BYTE after;

                mapSetPos(gs, &gs->mp, 106, 100, DEEP_SEA, TRUE, TRUE);
                after = viewportCalcSquarePure(gs, 0, 106, 100, &isMine);
                UT_ASSERT_MSG(after != before,
                              "square 106,100 still draws as tile %u after the "
                              "terrain under it changed, so there is nothing "
                              "for the memory to hold back", (unsigned)before);
            }
            overviewMapUpdate(om, gs, 0, &fog, TRUE, 0, 100, 100);
            UT_ASSERT_MSG(om->tile[106][100] == heldTile,
                          "the hidden square moved to tile %u, expected it to "
                          "hold the %u it last showed",
                          (unsigned)om->tile[106][100], (unsigned)heldTile);

            /* Turning the toggle off puts the square back in the block, and
             * what it shows then is what has been there all along. */
            fog.sightMode = (uint8_t)fogSightOff;
            overviewMapUpdate(om, gs, 0, &fog, TRUE, 0, 100, 100);
            UT_ASSERT_MSG(om->hiddenActive == FALSE,
                          "the map went on hiding squares after the toggle was "
                          "dropped");
            UT_ASSERT_MSG(om->tile[106][100] != heldTile,
                          "the square came back into the block still showing "
                          "tile %u, so the terrain change never reached it",
                          (unsigned)heldTile);
            for (x = 0; x < MAP_ARRAY_SIZE; x++) {
                for (y = 0; y < MAP_ARRAY_SIZE; y++) {
                    UT_ASSERT_MSG((om->flags[x][y] & OVERVIEW_F_HIDDEN) == 0,
                                  "square %d,%d is still hidden a tick after "
                                  "the toggle was dropped", x, y);
                }
            }

            /* The last stamp a block gets as it stops being live is masked as
             * well. Without that, letting the block go - by dying, or by
             * watching a pillbox for a tick - would show the player everything
             * it had been keeping from them on the way out. */
            {
                bool isMine = FALSE;
                BYTE hiddenTile; /* What the square behind the wall holds */
                BYTE seenTile;   /* What the square in front of it held */
                BYTE before;     /* Tile a terrain change moves from */
                BYTE after;      /* and to */

                /* Ground the memory can hold again, and one update with the
                 * toggle off to put a tile on both squares. */
                mapSetPos(gs, &gs->mp, 106, 100, GRASS, TRUE, TRUE);
                overviewMapReset(om);
                overviewViewInputsDefaults(&fog);
                overviewMapUpdate(om, gs, 0, &fog, TRUE, 0, 100, 100);
                hiddenTile = om->tile[106][100];
                seenTile = om->tile[102][100];

                fog.sightMode = (uint8_t)fogSightBuildings;
                overviewMapUpdate(om, gs, 0, &fog, TRUE, 0, 100, 100);
                UT_ASSERT_MSG((om->flags[106][100] & OVERVIEW_F_HIDDEN) != 0,
                              "the square behind the building carries flags "
                              "0x%02X, expected it hidden before the block "
                              "goes", (unsigned)om->flags[106][100]);

                /* Terrain moves on both sides of the wall, and then the tank's
                 * block goes. */
                before = viewportCalcSquarePure(gs, 0, 106, 100, &isMine);
                mapSetPos(gs, &gs->mp, 106, 100, DEEP_SEA, TRUE, TRUE);
                after = viewportCalcSquarePure(gs, 0, 106, 100, &isMine);
                UT_ASSERT_MSG(after != before,
                              "square 106,100 still draws as tile %u after the "
                              "terrain under it changed, so there is nothing "
                              "for the farewell to hold back",
                              (unsigned)before);

                before = viewportCalcSquarePure(gs, 0, 102, 100, &isMine);
                mapSetPos(gs, &gs->mp, 102, 100, DEEP_SEA, TRUE, TRUE);
                after = viewportCalcSquarePure(gs, 0, 102, 100, &isMine);
                UT_ASSERT_MSG(after != before,
                              "square 102,100 still draws as tile %u after the "
                              "terrain under it changed, so it says nothing "
                              "about the farewell running", (unsigned)before);

                overviewMapUpdate(om, gs, 0, &fog, FALSE, 0, 0, 0);
                UT_ASSERT_MSG(om->tile[106][100] == hiddenTile,
                              "the block's last stamp moved the hidden square "
                              "to tile %u, expected it to hold the %u the "
                              "player last saw",
                              (unsigned)om->tile[106][100],
                              (unsigned)hiddenTile);

                /* The square the player could see took the change, so the
                 * farewell did run - what it left alone is the hidden part of
                 * the block and nothing else. */
                UT_ASSERT_MSG(om->tile[102][100] != seenTile,
                              "the square in front of the wall is still on "
                              "tile %u, so the farewell stamp never ran",
                              (unsigned)seenTile);

                UT_ASSERT_MSG((om->flags[106][100] & OVERVIEW_F_HIDDEN) == 0,
                              "the square carries flags 0x%02X after leaving "
                              "the live set, expected the hidden bit dropped "
                              "with the rest", (unsigned)om->flags[106][100]);
                UT_ASSERT_MSG(om->hiddenActive == FALSE,
                              "the map hid squares on an update with no block "
                              "round the tank");
            }

            /* The beam and line of sight stack: with both running a square has
             * to be inside the beam and have a clear line to it to stay live.
             * The building at 103,100 is straight ahead of a tank at 100,100
             * facing east, so the ground behind it is in the beam and hidden
             * all the same, while the square in front of it is live and one
             * six squares off to the side is dark for want of the beam. */
            overviewMapReset(om);
            overviewViewInputsDefaults(&fog);
            fog.experiment = (uint8_t)fogExperimentHeadlightsEnvelope;
            fog.sightMode = (uint8_t)fogSightBuildings;
            fog.facing = 4; /* east, down the line the building sits on */
            overviewMapUpdate(om, gs, 0, &fog, TRUE, 0, 100, 100);
            UT_ASSERT_MSG(om->hiddenActive == TRUE,
                          "the map hid nothing with the beam and sight both "
                          "on");
            UT_ASSERT_MSG((om->flags[102][100] &
                           (OVERVIEW_F_LIVE | OVERVIEW_F_SIGHT)) ==
                              (OVERVIEW_F_LIVE | OVERVIEW_F_SIGHT),
                          "the square in front of the building carries flags "
                          "0x%02X, expected it still live with the beam and "
                          "sight both on", (unsigned)om->flags[102][100]);
            UT_ASSERT_MSG((om->flags[106][100] & OVERVIEW_F_HIDDEN) != 0,
                          "the square behind the building carries flags "
                          "0x%02X, expected the beam cut short by it",
                          (unsigned)om->flags[106][100]);
            UT_ASSERT_MSG((om->flags[100][94] & OVERVIEW_F_HIDDEN) != 0,
                          "the square square-on to the facing carries flags "
                          "0x%02X, expected it outside the beam whatever the "
                          "line to it is like",
                          (unsigned)om->flags[100][94]);
        }

        free(om);
    }

    /* Where the blackout sits in the death wait: the player watches their own
     * explosion, then the view goes black from the tick the classic view cuts
     * to static through to the respawn. A shell death and a drowning run on
     * their own clocks, the drowning being given longer to watch exactly as
     * the static is. */
    {
        int wait; /* Ticks left on the death wait */

        UT_ASSERT_MSG(
            !overviewMapDeathBlackout(TANK_DEATH_WAIT, LAST_DEATH_BY_SHELL),
            "a tank that has just died is already blacked out");
        UT_ASSERT_MSG(
            !overviewMapDeathBlackout(STATIC_ON_TICKS + 1, LAST_DEATH_BY_SHELL),
            "the blackout started a tick early");
        UT_ASSERT_MSG(
            overviewMapDeathBlackout(STATIC_ON_TICKS, LAST_DEATH_BY_SHELL),
            "the blackout did not start where the static does");
        UT_ASSERT_MSG(overviewMapDeathBlackout(1, LAST_DEATH_BY_SHELL),
                      "the blackout stopped before the respawn");
        UT_ASSERT_MSG(!overviewMapDeathBlackout(0, LAST_DEATH_BY_SHELL),
                      "a tank that is not dead is blacked out");

        /* A drowning is given longer, so there is a stretch where it is black
         * and a shell death is not. */
        UT_ASSERT_MSG(
            overviewMapDeathBlackout(STATIC_ON_TICKS_DEEPSEA,
                                     LAST_DEATH_BY_DEEPSEA) &&
                !overviewMapDeathBlackout(STATIC_ON_TICKS_DEEPSEA,
                                          LAST_DEATH_BY_SHELL),
            "a drowning and a shell death black out on the same tick");

        /* A cause the schedule does not know takes the shell figure — a death
         * with no answer must still black out rather than stay watchable. */
        UT_ASSERT_MSG(overviewMapDeathBlackout(STATIC_ON_TICKS, 0xFF),
                      "an unrecognised death cause never blacks out");

        /* Once up it stays up: no tick between the start and the respawn puts
         * the picture back. */
        for (wait = STATIC_ON_TICKS; wait >= 1; wait--) {
            UT_ASSERT_MSG(
                overviewMapDeathBlackout(wait, LAST_DEATH_BY_SHELL),
                "the blackout dropped at %d ticks left", wait);
        }
    }

    serverSimDestroy(sim);
    return 0;
}

/* --------------------------------------------------------------------------
 * Live cases. A ClientSim wired to an in-process ServerSim and driven through
 * clientSimDisplayTick, so the assertions below run against the real wiring
 * rather than a hand-called overviewMapUpdate.
 * -------------------------------------------------------------------------- */

/* Rounds of the input-pair warm-up to run before the client's tank is
 * expected, and the ceiling on how long to keep pumping for it. The active
 * local transport runs two server half-steps per clientSimNetTick, so the
 * producer has to supply two tick numbers per tick or the stream starves into
 * substitutes. */
#define OVERVIEW_WARMUP_TICKS     4
#define OVERVIEW_MAX_WARMUP_TICKS 32

/* E_MAP's compressed length — the literal ut_make_running_sim hands to
 * serverSimCreateCompressed. */
#define OVERVIEW_EMAP_LEN 5097

/* Side of the tank block, for the tile snapshot the tank-removal arm takes. */
#define OVERVIEW_TANK_SIDE (2 * OVERVIEW_TANK_HALF + 1)

/* The map corner the overview was seen jumping to when the local player died.
 * The dead-tank case parks the tank there itself, so what it asserts does not
 * rest on how a dead tank's position goes bad. */
#define OVERVIEW_DEAD_TANK_M 0

typedef struct OverviewFixture {
    ServerSim *sim;
    ClientSim *cs;
    GameSim   *gs;      /* the client's sim, not the server's */
    BYTE       me;      /* the slot the local join landed on */
    BYTE       tankMX;  /* where the client's tank sits, as the overview reads it */
    BYTE       tankMY;
} OverviewFixture;

/* Tears the fixture down, in the order test_active_local_input_to_shot uses.
 * Safe on a half-built fixture so the start path can bail anywhere. */
static void overviewFixtureStop(OverviewFixture *f) {
    if (f->cs != NULL) {
        clientSimDestroy(f->cs);
        f->cs = NULL;
    }
    if (f->sim != NULL) {
        serverSimDestroy(f->sim);
        f->sim = NULL;
    }
}

/* Brings up a running ServerSim with an in-process ClientSim joined to it and
 * pumps the local transport until the client's own tank has landed. Returns
 * NULL on success, or a message naming what failed; either way the caller
 * finishes with overviewFixtureStop. */
static const char *overviewFixtureStart(OverviewFixture *f, const char *name) {
    uint32_t inputTick = 1;
    int i;

    memset(f, 0, sizeof(*f));

    f->sim = ut_make_running_sim("Host");
    if (f->sim == NULL) {
        return "ut_make_running_sim returned NULL";
    }

    f->cs = clientSimAlloc();
    if (f->cs == NULL) {
        return "clientSimAlloc returned NULL";
    }
    clientSimCreate(f->cs);
    if (clientSimConnectLocal(f->cs, f->sim, name, "", 0, 0) != TRUE) {
        return "clientSimConnectLocal failed";
    }

    f->me = clientSimGetMyPlayerNum(f->cs);
    for (i = 0; i < OVERVIEW_MAX_WARMUP_TICKS; i++) {
        InputPacket a, b;

        memset(&a, 0, sizeof(a));
        a.tick      = inputTick;
        a.playerNum = f->me;
        memset(&b, 0, sizeof(b));
        b.tick      = inputTick + 1;
        b.playerNum = f->me;
        clientSimNetSendInput(f->cs, &a);
        clientSimNetSendInput(f->cs, &b);
        clientSimNetTick(f->cs);
        inputTick += 2;

        if (i + 1 >= OVERVIEW_WARMUP_TICKS &&
            clientSimGetMyTankMapPos(f->cs, &f->tankMX, &f->tankMY) == TRUE) {
            break;
        }
    }

    f->gs = clientSimGetGameSim(f->cs);
    if (f->gs == NULL) {
        return "clientSimGetGameSim returned NULL";
    }
    if (clientSimGetMyTankMapPos(f->cs, &f->tankMX, &f->tankMY) != TRUE) {
        return "the client still has no tank after the warm-up";
    }
    return NULL;
}

/* The direction with room in it: everything below steps away from the nearer
 * map edge, so a block 60 or 100 squares off the tank still lands on the map
 * wherever the join dropped the tank. */
static int overviewAwayFromEdge(BYTE mapCoord) {
    return (mapCoord < MAP_ARRAY_SIZE / 2) ? 1 : -1;
}

/* TRUE when the square falls inside any of the rects. */
static bool overviewInAnyRect(const OverviewRect *r, int count, int x, int y) {
    int i; /* Looping variable */

    for (i = 0; i < count; i++) {
        if (x >= r[i].left && x <= r[i].right && y >= r[i].top &&
            y <= r[i].bottom) {
            return TRUE;
        }
    }
    return FALSE;
}

/* Reports the first square of the rect whose OVERVIEW_F_LIVE bit is not what
 * was wanted, so a failure can name it. FALSE when every square agrees. */
static bool overviewRectLiveMismatch(const OverviewMap *om,
                                     const OverviewRect *r, bool want,
                                     int *outX, int *outY) {
    int x; /* Looping variable */
    int y; /* Looping variable */

    for (x = r->left; x <= r->right; x++) {
        for (y = r->top; y <= r->bottom; y++) {
            bool live = (om->flags[x][y] & OVERVIEW_F_LIVE) != 0;
            if (live != want) {
                *outX = x;
                *outY = y;
                return TRUE;
            }
        }
    }
    return FALSE;
}

/* Fails the calling test unless every square of the rect carries (want ==
 * TRUE) or has dropped (want == FALSE) OVERVIEW_F_LIVE. */
#define ASSERT_RECT_LIVE(om, r, want, what)                                   \
    do {                                                                      \
        int bx_ = 0;                                                          \
        int by_ = 0;                                                          \
        UT_ASSERT_MSG(                                                        \
            overviewRectLiveMismatch((om), &(r), (want), &bx_, &by_) != TRUE, \
            "%s: square %d,%d is %s live", (what), bx_, by_,                  \
            ((want) == TRUE) ? "not" : "still");                              \
    } while (0)

/* A terrain byte that has to read differently from what is there now. Sea and
 * grass are far enough apart that no adjacency calculation can collapse the
 * two onto one sprite. */
static BYTE overviewFlipTerrain(BYTE terrain) {
    return (terrain == DEEP_SEA) ? (BYTE)GRASS : (BYTE)DEEP_SEA;
}

/* First square of the rect whose tile actually moves when the terrain under it
 * is swapped. Pill and base squares render from their owner whatever the
 * ground says, so a terrain check aimed at one would pass however the memory
 * behaved. Probes by writing and putting back, leaving the map exactly as it
 * was found; call it before the first display tick so the memory never sees
 * the probe. */
static bool overviewFindTerrainSquare(GameSim *gs, BYTE me,
                                      const OverviewRect *r, BYTE *outX,
                                      BYTE *outY) {
    int x; /* Looping variable */
    int y; /* Looping variable */

    for (x = r->left; x <= r->right; x++) {
        for (y = r->top; y <= r->bottom; y++) {
            bool isMine = FALSE;
            BYTE was = mapGetPos(&gs->mp, (BYTE)x, (BYTE)y);
            BYTE before = viewportCalcSquarePure(gs, me, (BYTE)x, (BYTE)y, &isMine);
            BYTE after;

            mapSetPos(gs, &gs->mp, (BYTE)x, (BYTE)y, overviewFlipTerrain(was),
                      TRUE, TRUE);
            after = viewportCalcSquarePure(gs, me, (BYTE)x, (BYTE)y, &isMine);
            mapSetPos(gs, &gs->mp, (BYTE)x, (BYTE)y, was, TRUE, TRUE);
            if (after != before) {
                *outX = (BYTE)x;
                *outY = (BYTE)y;
                return TRUE;
            }
        }
    }
    return FALSE;
}

int run_overview_reveal(void) {
    OverviewFixture f;
    const char *err = overviewFixtureStart(&f, "Reveal");
    UT_ASSERT_MSG(err == NULL, "%s", err);

    /* One pill of the client's own, a hundred squares off the tank, so the
     * live set is a union of two disjoint blocks rather than the tank's block
     * on its own. */
    BYTE pillX = (BYTE)((int)f.tankMX + overviewAwayFromEdge(f.tankMX) * 100);
    BYTE pillY = f.tankMY;
    f.gs->pb->numPills = 1;
    f.gs->pb->item[0].owner = f.me;
    f.gs->pb->item[0].armour = PILLBOX_15;
    f.gs->pb->item[0].inTank = FALSE;
    f.gs->pb->item[0].x = pillX;
    f.gs->pb->item[0].y = pillY;

    /* The first display tick of the client's life: everything the memory holds
     * afterwards was put there by this one update. */
    clientSimDisplayTick(f.cs, false);

    const OverviewMap *om = clientSimGetOverviewMap(f.cs);
    UT_ASSERT_MSG(om != NULL, "clientSimGetOverviewMap returned NULL");

    OverviewRect expect[OVERVIEW_MAX_REGIONS];
    OverviewViewInputs in;
    clientSimFillOverviewViewInputs(f.cs, &in);
    OverviewRect tankBlock = overviewTestBlock(f.tankMX, f.tankMY,
                                               OVERVIEW_TANK_HALF);
    int n = overviewMapBuildRegions(f.gs, f.me, &in, NULL, &tankBlock, expect,
                                    OVERVIEW_MAX_REGIONS);
    UT_ASSERT_MSG(n == 2, "expected the tank block and one pill block, got %d",
                  n);
    UT_ASSERT_MSG(om->liveCount == n, "liveCount %d, expected %d",
                  om->liveCount, n);

    /* Every square of the map, either side of the union: everything holds
     * what the per-square calculator says — the install seeded the whole map
     * on this tick and nothing has moved since — with the live flag exactly
     * on the union. */
    int x, y;
    for (x = 0; x < MAP_ARRAY_SIZE; x++) {
        for (y = 0; y < MAP_ARRAY_SIZE; y++) {
            BYTE flags = om->flags[x][y];
            bool inUnion = overviewInAnyRect(expect, n, x, y) == TRUE;
            bool isMine = FALSE;
            BYTE want = viewportCalcSquarePure(f.gs, f.me, (BYTE)x, (BYTE)y,
                                               &isMine);

            UT_ASSERT_MSG(((flags & OVERVIEW_F_LIVE) != 0) == inUnion,
                          "square %d,%d is %s the union but %s live",
                          x, y, inUnion ? "inside" : "outside",
                          inUnion ? "not" : "flagged");
            UT_ASSERT_MSG(om->tile[x][y] == want,
                          "square %d,%d holds tile %u, calculator says %u",
                          x, y, (unsigned)om->tile[x][y], (unsigned)want);
            UT_ASSERT_MSG(((flags & OVERVIEW_F_MINE) != 0) == (isMine == TRUE),
                          "square %d,%d mine flag %d, calculator says %d",
                          x, y, (flags & OVERVIEW_F_MINE) != 0,
                          isMine == TRUE);
        }
    }
    UT_ASSERT_MSG(om->seenCount ==
                      (unsigned)(MAP_ARRAY_SIZE * MAP_ARRAY_SIZE),
                  "seenCount %u after the seed, expected every square (%u)",
                  om->seenCount,
                  (unsigned)(MAP_ARRAY_SIZE * MAP_ARRAY_SIZE));

    /* A second tick with nothing touched in between. generation is what a
     * renderer reads to decide it can skip a redraw, so a tick where nothing
     * moved must not advance it. */
    unsigned genBefore = om->generation;
    unsigned seenBefore = om->seenCount;
    int liveBefore = om->liveCount;
    clientSimDisplayTick(f.cs, false);
    UT_ASSERT_MSG(om->generation == genBefore,
                  "generation went from %u to %u over a tick where nothing "
                  "moved", genBefore, om->generation);
    UT_ASSERT_MSG(om->seenCount == seenBefore,
                  "seenCount went from %u to %u over a tick where nothing "
                  "moved", seenBefore, om->seenCount);
    UT_ASSERT_MSG(om->liveCount == liveBefore,
                  "liveCount went from %d to %d over a tick where nothing "
                  "moved", liveBefore, om->liveCount);

    overviewFixtureStop(&f);
    return 0;
}

int run_overview_freeze_no_leak(void) {
    OverviewFixture f;
    const char *err = overviewFixtureStart(&f, "Freeze");
    UT_ASSERT_MSG(err == NULL, "%s", err);

    /* The map's own pills stay out of this one: with the tank block the only
     * live region, a square that leaves it is genuinely frozen. */
    f.gs->pb->numPills = 0;

    /* Sixty squares is more than the two 29x29 blocks can span between them,
     * so the tank's old block and its new one share no square. */
    int newMX = (int)f.tankMX + overviewAwayFromEdge(f.tankMX) * 60;
    OverviewRect oldRect = overviewTestBlock(f.tankMX, f.tankMY, OVERVIEW_TANK_HALF);
    OverviewRect newRect = overviewTestBlock(newMX, f.tankMY, OVERVIEW_TANK_HALF);

    /* Both squares are picked before the first tick, so the picker's probing
     * never reaches the memory. */
    BYTE frozenX = 0, frozenY = 0, liveX = 0, liveY = 0;
    UT_ASSERT_MSG(overviewFindTerrainSquare(f.gs, f.me, &oldRect, &frozenX,
                                            &frozenY) == TRUE,
                  "no terrain-driven square in the block at %u,%u",
                  (unsigned)f.tankMX, (unsigned)f.tankMY);
    UT_ASSERT_MSG(overviewFindTerrainSquare(f.gs, f.me, &newRect, &liveX,
                                            &liveY) == TRUE,
                  "no terrain-driven square in the block at %d,%u", newMX,
                  (unsigned)f.tankMY);

    const OverviewMap *om = clientSimGetOverviewMap(f.cs);
    UT_ASSERT_MSG(om != NULL, "clientSimGetOverviewMap returned NULL");

    clientSimDisplayTick(f.cs, false);
    UT_ASSERT_MSG((om->flags[frozenX][frozenY] & OVERVIEW_F_LIVE) != 0,
                  "square %u,%u is in the tank's block but not live",
                  (unsigned)frozenX, (unsigned)frozenY);
    BYTE frozenTile = om->tile[frozenX][frozenY];
    UT_ASSERT_MSG(frozenTile != OVERVIEW_UNSEEN,
                  "square %u,%u was live but reads unseen", (unsigned)frozenX,
                  (unsigned)frozenY);

    /* Drive off, and the square the tank left behind stops being live while
     * keeping the tile it carried. */
    tankSetWorld(f.gs, &f.gs->tanks[f.me], (WORLD)(newMX << TANK_SHIFT_MAPSIZE),
                 (WORLD)((int)f.tankMY << TANK_SHIFT_MAPSIZE), 0, false);
    clientSimDisplayTick(f.cs, false);
    ASSERT_RECT_LIVE(om, newRect, TRUE, "the block the tank drove into");
    UT_ASSERT_MSG((om->flags[frozenX][frozenY] & OVERVIEW_F_LIVE) == 0,
                  "square %u,%u is out of every region but still live",
                  (unsigned)frozenX, (unsigned)frozenY);
    UT_ASSERT_MSG(om->tile[frozenX][frozenY] == frozenTile,
                  "square %u,%u changed from tile %u to %u on the way out",
                  (unsigned)frozenX, (unsigned)frozenY, (unsigned)frozenTile,
                  (unsigned)om->tile[frozenX][frozenY]);

    /* The leak. The client takes a terrain change on the frozen square — it
     * knows about every change on the map, wherever it is — and the memory has
     * to go on showing what was there when the square was last seen. */
    {
        bool isMine = FALSE;
        BYTE was = mapGetPos(&f.gs->mp, frozenX, frozenY);
        BYTE now = overviewFlipTerrain(was);

        mapSetPos(f.gs, &f.gs->mp, frozenX, frozenY, now, TRUE, TRUE);
        UT_ASSERT_MSG(mapGetPos(&f.gs->mp, frozenX, frozenY) == now,
                      "the client did not take the terrain change at %u,%u",
                      (unsigned)frozenX, (unsigned)frozenY);
        UT_ASSERT_MSG(viewportCalcSquarePure(f.gs, f.me, frozenX, frozenY,
                                             &isMine) != frozenTile,
                      "terrain %u and %u render as the same tile at %u,%u — "
                      "the leak check would pass on its own",
                      (unsigned)was, (unsigned)now, (unsigned)frozenX,
                      (unsigned)frozenY);
    }
    clientSimDisplayTick(f.cs, false);
    UT_ASSERT_MSG(om->tile[frozenX][frozenY] == frozenTile,
                  "the memory picked up a terrain change on frozen square "
                  "%u,%u: tile %u, expected %u",
                  (unsigned)frozenX, (unsigned)frozenY,
                  (unsigned)om->tile[frozenX][frozenY], (unsigned)frozenTile);

    /* The same change inside the live block does reach the memory, so the
     * freeze above is the region mask at work and not a dead update. */
    UT_ASSERT_MSG((om->flags[liveX][liveY] & OVERVIEW_F_LIVE) != 0,
                  "square %u,%u is in the tank's new block but not live",
                  (unsigned)liveX, (unsigned)liveY);
    BYTE liveTile = om->tile[liveX][liveY];
    unsigned genBefore = om->generation;
    BYTE wantTile;
    {
        bool isMine = FALSE;
        BYTE was = mapGetPos(&f.gs->mp, liveX, liveY);
        BYTE now = overviewFlipTerrain(was);

        mapSetPos(f.gs, &f.gs->mp, liveX, liveY, now, TRUE, TRUE);
        wantTile = viewportCalcSquarePure(f.gs, f.me, liveX, liveY, &isMine);
        UT_ASSERT_MSG(wantTile != liveTile,
                      "terrain %u and %u render as the same tile at %u,%u — "
                      "the live check would pass on its own",
                      (unsigned)was, (unsigned)now, (unsigned)liveX,
                      (unsigned)liveY);
    }
    clientSimDisplayTick(f.cs, false);
    UT_ASSERT_MSG(om->tile[liveX][liveY] == wantTile,
                  "live square %u,%u holds tile %u, expected %u",
                  (unsigned)liveX, (unsigned)liveY,
                  (unsigned)om->tile[liveX][liveY], (unsigned)wantTile);
    UT_ASSERT_MSG(om->generation > genBefore,
                  "generation stuck at %u over a changed live square",
                  genBefore);

    overviewFixtureStop(&f);
    return 0;
}

int run_overview_pill_capture(void) {
    OverviewFixture f;
    const char *err = overviewFixtureStart(&f, "Pills");
    UT_ASSERT_MSG(err == NULL, "%s", err);

    /* One pill, a hundred squares off the tank so its 15x15 block never meets
     * the tank's 29x29 one, and off any base square — a base renders from its
     * owner whatever sits on it, which would make the carried-pill arm below
     * read the wrong thing. */
    BYTE pillX = (BYTE)((int)f.tankMX + overviewAwayFromEdge(f.tankMX) * 100);
    BYTE pillY = f.tankMY;
    int step;
    for (step = 0; step < 16 && basesExistPos(&f.gs->bs, pillX, pillY) == TRUE;
         step++) {
        pillY = (BYTE)(pillY + 1);
    }
    UT_ASSERT_MSG(basesExistPos(&f.gs->bs, pillX, pillY) != TRUE,
                  "no base-free square near %u,%u for the pill",
                  (unsigned)pillX, (unsigned)f.tankMY);

    f.gs->pb->numPills = 1;
    f.gs->pb->item[0].x = pillX;
    f.gs->pb->item[0].y = pillY;
    f.gs->pb->item[0].inTank = FALSE;
    f.gs->pb->item[0].armour = PILLBOX_15;
    f.gs->pb->item[0].owner = NEUTRAL;

    OverviewRect pillRect = overviewTestBlock(pillX, pillY, OVERVIEW_PILL_HALF);
    OverviewRect tankRect = overviewTestBlock(f.tankMX, f.tankMY, OVERVIEW_TANK_HALF);

    /* Any slot but the client's own is hostile: alliances start empty. */
    BYTE hostile = (BYTE)((f.me == 0) ? 1 : 0);
    UT_ASSERT_MSG(playersIsAllie(&f.gs->plyrs, f.me, hostile) != TRUE,
                  "slots %u and %u are allied — breaks the enemy-pill arm",
                  (unsigned)f.me, (unsigned)hostile);

    const OverviewMap *om = clientSimGetOverviewMap(f.cs);
    UT_ASSERT_MSG(om != NULL, "clientSimGetOverviewMap returned NULL");

    bool isMine = FALSE;

    /* Neutral: nothing to view through, so the block never lights up. */
    clientSimDisplayTick(f.cs, false);
    ASSERT_RECT_LIVE(om, pillRect, FALSE, "a neutral pill's block");

    /* Captured: the block goes live and the pill draws in the captor's
     * colours. */
    f.gs->pb->item[0].owner = f.me;
    clientSimDisplayTick(f.cs, false);
    ASSERT_RECT_LIVE(om, pillRect, TRUE, "a captured pill's block");
    BYTE aliveTile = viewportCalcSquarePure(f.gs, f.me, pillX, pillY, &isMine);
    UT_ASSERT_MSG(om->tile[pillX][pillY] == aliveTile,
                  "the live pill holds tile %u, calculator says %u",
                  (unsigned)om->tile[pillX][pillY], (unsigned)aliveTile);

    /* Killed: the block freezes, and the farewell stamp leaves the pill's own
     * square dead rather than a tick stale on full armour. */
    f.gs->pb->item[0].armour = 0;
    clientSimDisplayTick(f.cs, false);
    BYTE deadTile = viewportCalcSquarePure(f.gs, f.me, pillX, pillY, &isMine);
    UT_ASSERT_MSG(deadTile != aliveTile,
                  "a dead pill renders as tile %u, the same as a live one — "
                  "nothing here for the farewell stamp to show",
                  (unsigned)deadTile);
    ASSERT_RECT_LIVE(om, pillRect, FALSE, "a dead pill's block");
    UT_ASSERT_MSG(om->tile[pillX][pillY] == deadTile,
                  "the dead pill froze on tile %u, expected %u",
                  (unsigned)om->tile[pillX][pillY], (unsigned)deadTile);

    /* Alive again so there is a live block to leave, then handed to someone
     * hostile: the same freeze, with the enemy's colours stamped in. */
    f.gs->pb->item[0].armour = PILLBOX_15;
    clientSimDisplayTick(f.cs, false);
    ASSERT_RECT_LIVE(om, pillRect, TRUE, "a repaired pill's block");
    f.gs->pb->item[0].owner = hostile;
    clientSimDisplayTick(f.cs, false);
    BYTE enemyTile = viewportCalcSquarePure(f.gs, f.me, pillX, pillY, &isMine);
    UT_ASSERT_MSG(enemyTile != aliveTile,
                  "an enemy pill renders as tile %u, the same as an allied one",
                  (unsigned)enemyTile);
    ASSERT_RECT_LIVE(om, pillRect, FALSE, "an enemy pill's block");
    UT_ASSERT_MSG(om->tile[pillX][pillY] == enemyTile,
                  "the captured pill froze on tile %u, expected %u",
                  (unsigned)om->tile[pillX][pillY], (unsigned)enemyTile);

    /* Alive and owned again, then picked up: the square goes back to the
     * ground that was under it. */
    f.gs->pb->item[0].owner = f.me;
    clientSimDisplayTick(f.cs, false);
    ASSERT_RECT_LIVE(om, pillRect, TRUE, "a recaptured pill's block");
    f.gs->pb->item[0].inTank = TRUE;
    clientSimDisplayTick(f.cs, false);
    BYTE groundTile = viewportCalcSquarePure(f.gs, f.me, pillX, pillY, &isMine);
    UT_ASSERT_MSG(groundTile != aliveTile && groundTile != deadTile &&
                      groundTile != enemyTile,
                  "the ground under the pill renders as tile %u, which is one "
                  "of the pill tiles", (unsigned)groundTile);
    ASSERT_RECT_LIVE(om, pillRect, FALSE, "a carried pill's block");
    UT_ASSERT_MSG(om->tile[pillX][pillY] == groundTile,
                  "the carried pill's square froze on tile %u, expected %u",
                  (unsigned)om->tile[pillX][pillY], (unsigned)groundTile);

    /* The tank goes the same way: its block stops being live and keeps every
     * tile it already had. */
    BYTE saved[OVERVIEW_TANK_SIDE * OVERVIEW_TANK_SIDE];
    int x, y, i;
    ASSERT_RECT_LIVE(om, tankRect, TRUE, "the tank's block before it is removed");
    i = 0;
    for (x = tankRect.left; x <= tankRect.right; x++) {
        for (y = tankRect.top; y <= tankRect.bottom; y++) {
            saved[i] = om->tile[x][y];
            i++;
        }
    }

    tankDestroy(f.gs, &f.gs->tanks[f.me]);
    f.gs->tanks[f.me] = NULL;
    clientSimDisplayTick(f.cs, false);
    ASSERT_RECT_LIVE(om, tankRect, FALSE, "the removed tank's block");
    UT_ASSERT_MSG(om->liveCount == 0,
                  "liveCount %d with no tank and the only pill carried",
                  om->liveCount);
    i = 0;
    for (x = tankRect.left; x <= tankRect.right; x++) {
        for (y = tankRect.top; y <= tankRect.bottom; y++) {
            UT_ASSERT_MSG(om->tile[x][y] == saved[i],
                          "square %d,%d changed from tile %u to %u when the "
                          "tank went", x, y, (unsigned)saved[i],
                          (unsigned)om->tile[x][y]);
            i++;
        }
    }

    overviewFixtureStop(&f);
    return 0;
}

int run_overview_dead_tank(void) {
    OverviewFixture f;
    const char *err = overviewFixtureStart(&f, "Dead");
    UT_ASSERT_MSG(err == NULL, "%s", err);

    /* No pills: the tank's block is the only live region, so whatever is live
     * after the death is the dead tank's doing and nothing else's. */
    f.gs->pb->numPills = 0;

    const OverviewMap *om = clientSimGetOverviewMap(f.cs);
    UT_ASSERT_MSG(om != NULL, "clientSimGetOverviewMap returned NULL");

    OverviewRect tankRect =
        overviewTestBlock(f.tankMX, f.tankMY, OVERVIEW_TANK_HALF);
    OverviewRect deadRect = overviewTestBlock(OVERVIEW_DEAD_TANK_M,
                                              OVERVIEW_DEAD_TANK_M,
                                              OVERVIEW_TANK_HALF);
    /* Sixty squares is more than the two 29x29 blocks can span between them,
     * so the block the tank respawns into shares no square with the one it
     * died holding. */
    int respawnMX = (int)f.tankMX + overviewAwayFromEdge(f.tankMX) * 60;
    OverviewRect respawnRect =
        overviewTestBlock(respawnMX, f.tankMY, OVERVIEW_TANK_HALF);
    int x, y, i;

    /* The square the terrain arm moves later on, picked before the first tick
     * so the picker's probing never reaches the memory. */
    BYTE liveX = 0, liveY = 0;
    UT_ASSERT_MSG(overviewFindTerrainSquare(f.gs, f.me, &tankRect, &liveX,
                                            &liveY) == TRUE,
                  "no terrain-driven square in the block at %u,%u",
                  (unsigned)f.tankMX, (unsigned)f.tankMY);

    clientSimDisplayTick(f.cs, false);
    ASSERT_RECT_LIVE(om, tankRect, TRUE, "a living tank's block");
    UT_ASSERT_MSG(om->liveCount == 1,
                  "liveCount %d with a living tank and no pills",
                  om->liveCount);

    /* Every tile of the tank's block, to check dying leaves what is there
     * alone rather than rewriting it from wherever the tank now reads. */
    BYTE saved[OVERVIEW_TANK_SIDE * OVERVIEW_TANK_SIDE];
    i = 0;
    for (x = tankRect.left; x <= tankRect.right; x++) {
        for (y = tankRect.top; y <= tankRect.bottom; y++) {
            saved[i] = om->tile[x][y];
            i++;
        }
    }

    /* The block the tank is about to be sent to. Checked not-live rather than
     * assumed, so a join that dropped the tank within reach of the corner says
     * so instead of leaving the case proving nothing; its seeded tiles are
     * captured so the death tick can be shown to leave them alone. */
    BYTE corner[OVERVIEW_TANK_SIDE * OVERVIEW_TANK_SIDE];
    i = 0;
    for (x = deadRect.left; x <= deadRect.right; x++) {
        for (y = deadRect.top; y <= deadRect.bottom; y++) {
            UT_ASSERT_MSG((om->flags[x][y] & OVERVIEW_F_LIVE) == 0,
                          "square %d,%d of the corner the dead tank is sent to "
                          "is already live", x, y);
            corner[i] = om->tile[x][y];
            i++;
        }
    }

    /* Dead but still in its slot, which is what the server leaves behind for
     * the whole of deathWait: over full armour, the way a drowning writes it,
     * with a full wait left to run and a shell as the cause. The move to the
     * corner stands in for the map origin a dead tank's position reads as, so
     * what the corner arm below asserts does not rest on how that position
     * goes bad. */
    tankSetArmour(&f.gs->tanks[f.me], (BYTE)(TANK_FULL_ARMOUR + 1));
    tankSetLastTankDeath(&f.gs->tanks[f.me], LAST_DEATH_BY_SHELL);
    tankSetDeathWait(&f.gs->tanks[f.me], TANK_DEATH_WAIT);
    tankSetWorld(f.gs, &f.gs->tanks[f.me],
                 (WORLD)(OVERVIEW_DEAD_TANK_M << TANK_SHIFT_MAPSIZE),
                 (WORLD)(OVERVIEW_DEAD_TANK_M << TANK_SHIFT_MAPSIZE), 0, false);
    clientSimDisplayTick(f.cs, false);

    /* A dead tank reveals nothing where it reads: the block is held on the
     * square it died on, never on the corner underneath it, so the corner
     * stays out of the live set with its seeded tiles untouched. */
    i = 0;
    for (x = deadRect.left; x <= deadRect.right; x++) {
        for (y = deadRect.top; y <= deadRect.bottom; y++) {
            UT_ASSERT_MSG((om->flags[x][y] & OVERVIEW_F_LIVE) == 0,
                          "a dead tank lit square %d,%d", x, y);
            UT_ASSERT_MSG(om->tile[x][y] == corner[i],
                          "a dead tank restamped square %d,%d: tile %u, "
                          "expected %u", x, y, (unsigned)om->tile[x][y],
                          (unsigned)corner[i]);
            i++;
        }
    }
    UT_ASSERT_MSG(om->liveCount == 1,
                  "liveCount %d with a dead tank and no pills", om->liveCount);

    /* The block it died holding stays live, tiles intact, so the player
     * watches the explosion and the ground round it in colour. */
    ASSERT_RECT_LIVE(om, tankRect, TRUE, "a dead tank's block");
    i = 0;
    for (x = tankRect.left; x <= tankRect.right; x++) {
        for (y = tankRect.top; y <= tankRect.bottom; y++) {
            UT_ASSERT_MSG(om->tile[x][y] == saved[i],
                          "square %d,%d changed from tile %u to %u when the "
                          "tank died", x, y, (unsigned)saved[i],
                          (unsigned)om->tile[x][y]);
            i++;
        }
    }

    /* And the block is genuinely still live rather than merely still flagged:
     * a terrain change inside it reaches the memory while the tank is dead. */
    BYTE liveTile = om->tile[liveX][liveY];
    BYTE wantTile;
    {
        bool isMine = FALSE;
        BYTE was = mapGetPos(&f.gs->mp, liveX, liveY);
        BYTE now = overviewFlipTerrain(was);

        mapSetPos(f.gs, &f.gs->mp, liveX, liveY, now, TRUE, TRUE);
        UT_ASSERT_MSG(mapGetPos(&f.gs->mp, liveX, liveY) == now,
                      "the client did not take the terrain change at %u,%u",
                      (unsigned)liveX, (unsigned)liveY);
        wantTile = viewportCalcSquarePure(f.gs, f.me, liveX, liveY, &isMine);
        UT_ASSERT_MSG(wantTile != liveTile,
                      "terrain %u and %u render as the same tile at %u,%u — "
                      "the live check would pass on its own",
                      (unsigned)was, (unsigned)now, (unsigned)liveX,
                      (unsigned)liveY);
    }
    clientSimDisplayTick(f.cs, false);
    UT_ASSERT_MSG(om->tile[liveX][liveY] == wantTile,
                  "square %u,%u sat in a dead tank's block and held tile %u "
                  "over a terrain change, expected %u", (unsigned)liveX,
                  (unsigned)liveY, (unsigned)om->tile[liveX][liveY],
                  (unsigned)wantTile);

    /* The wait run down past the tick the classic view cuts to static, which is
     * where the view goes black. The block underneath is untouched by that —
     * whole, live and still taking terrain — because what the player is denied
     * is the picture, not the memory. */
    tankSetDeathWait(&f.gs->tanks[f.me], STATIC_ON_TICKS - 1);
    clientSimDisplayTick(f.cs, false);
    ASSERT_RECT_LIVE(om, tankRect, TRUE, "a blacked-out tank's block");
    UT_ASSERT_MSG(om->liveCount == 1,
                  "liveCount %d with a dead tank blacked out", om->liveCount);
    UT_ASSERT_MSG(clientSimIsMyTankDeathBlackout(f.cs) == TRUE,
                  "the view is not black a tick after the static would start");
    {
        BYTE was = mapGetPos(&f.gs->mp, liveX, liveY);
        BYTE now = overviewFlipTerrain(was);
        bool isMine = FALSE;
        BYTE want;

        BYTE held = om->tile[liveX][liveY];

        mapSetPos(f.gs, &f.gs->mp, liveX, liveY, now, TRUE, TRUE);
        want = viewportCalcSquarePure(f.gs, f.me, liveX, liveY, &isMine);
        UT_ASSERT_MSG(want != held,
                      "terrain %u and %u render as the same tile at %u,%u — "
                      "the live check would pass on its own", (unsigned)was,
                      (unsigned)now, (unsigned)liveX, (unsigned)liveY);
        clientSimDisplayTick(f.cs, false);
        UT_ASSERT_MSG(om->tile[liveX][liveY] == want,
                      "square %u,%u held tile %u through a terrain change "
                      "while the view was black, expected %u", (unsigned)liveX,
                      (unsigned)liveY, (unsigned)om->tile[liveX][liveY],
                      (unsigned)want);
    }

    /* Respawned sixty squares off: the block follows the tank there and the
     * square it died on lets go, so the remembered position lasts exactly as
     * long as the tank has none of its own. */
    tankSetWorld(f.gs, &f.gs->tanks[f.me],
                 (WORLD)(respawnMX << TANK_SHIFT_MAPSIZE),
                 (WORLD)((int)f.tankMY << TANK_SHIFT_MAPSIZE), 0, false);
    tankSetArmour(&f.gs->tanks[f.me], (BYTE)TANK_FULL_ARMOUR);
    tankSetDeathWait(&f.gs->tanks[f.me], 0);
    clientSimDisplayTick(f.cs, false);
    ASSERT_RECT_LIVE(om, respawnRect, TRUE, "a respawned tank's block");
    ASSERT_RECT_LIVE(om, tankRect, FALSE,
                     "the block the tank respawned away from");
    UT_ASSERT_MSG(om->liveCount == 1, "liveCount %d after the tank came back",
                  om->liveCount);

    overviewFixtureStop(&f);
    return 0;
}

/* Seeded into the out-params before every read below. Non-zero, so the map
 * origin a dead tank's position used to read as shows up as a write. */
#define TANK_POS_SENTINEL 0xEE

int run_tank_pos_dead(void) {
    OverviewFixture f;
    const char *err = overviewFixtureStart(&f, "TankPos");
    UT_ASSERT_MSG(err == NULL, "%s", err);

    BYTE gotX = TANK_POS_SENTINEL;
    BYTE gotY = TANK_POS_SENTINEL;

    UT_ASSERT_MSG(clientSimGetMyTankMapPos(f.cs, &gotX, &gotY) == TRUE,
                  "a living tank reported no map position");
    UT_ASSERT_MSG(gotX == f.tankMX && gotY == f.tankMY,
                  "a living tank reads %u,%u, expected %u,%u", (unsigned)gotX,
                  (unsigned)gotY, (unsigned)f.tankMX, (unsigned)f.tankMY);

    /* Dead but still in its slot, which is where the server leaves it for the
     * whole of deathWait. The position underneath reads as the map origin, so
     * the accessor has nothing true to report. */
    tankSetArmour(&f.gs->tanks[f.me], (BYTE)(TANK_FULL_ARMOUR + 1));
    gotX = TANK_POS_SENTINEL;
    gotY = TANK_POS_SENTINEL;
    UT_ASSERT_MSG(clientSimGetMyTankMapPos(f.cs, &gotX, &gotY) != TRUE,
                  "a dead tank reported a map position");
    /* A caller that ignores the return value should at least not be handed a
     * square the tank is not on. */
    UT_ASSERT_MSG(gotX == TANK_POS_SENTINEL && gotY == TANK_POS_SENTINEL,
                  "a failed read overwrote the caller's variables with %u,%u",
                  (unsigned)gotX, (unsigned)gotY);

    /* Respawned: the accessor answers again, with the square it had. */
    tankSetArmour(&f.gs->tanks[f.me], (BYTE)TANK_FULL_ARMOUR);
    gotX = TANK_POS_SENTINEL;
    gotY = TANK_POS_SENTINEL;
    UT_ASSERT_MSG(clientSimGetMyTankMapPos(f.cs, &gotX, &gotY) == TRUE,
                  "a respawned tank reported no map position");
    UT_ASSERT_MSG(gotX == f.tankMX && gotY == f.tankMY,
                  "a respawned tank reads %u,%u, expected %u,%u",
                  (unsigned)gotX, (unsigned)gotY, (unsigned)f.tankMX,
                  (unsigned)f.tankMY);

    overviewFixtureStop(&f);
    return 0;
}

/* Builds the three per-frame entity lists and keeps the tanks. The other two
 * are created and thrown away here because clientSimPrepareOverviewEntities
 * fills all three and the heap lists have to be freed either way. */
static void overviewEntitiesPrepare(ClientSim *cs, screenTanks *tks) {
    screenLgm lgms;     /* Built and dropped — this case is about tanks */
    screenBullets sb;   /* Same */

    screenTanksCreate(tks);
    screenLgmCreate(&lgms);
    sb = screenBulletsCreate();
    clientSimPrepareOverviewEntities(cs, tks, &lgms, &sb);
    screenBulletsDestroy(&sb);
    screenLgmDestroy(&lgms);
}

/* The square a player's tank came back on, or FALSE when the list does not
 * carry that player at all. */
static bool overviewFindTank(const screenTanks *tks, BYTE playerNum, BYTE *outX,
                             BYTE *outY) {
    BYTE total = screenTanksGetNumEntries(tks);
    BYTE count; /* Looping variable */

    for (count = 1; count <= total; count++) {
        BYTE mx;
        BYTE my;
        BYTE px;
        BYTE py;
        BYTE frame;
        BYTE pn;
        char name[PLAYER_NAME_LEN];

        screenTanksGetItem(tks, count, &mx, &my, &px, &py, &frame, &pn, name);
        if (pn == playerNum) {
            *outX = mx;
            *outY = my;
            return TRUE;
        }
    }
    return FALSE;
}

int run_overview_entities(void) {
    OverviewFixture f;
    const char *err = overviewFixtureStart(&f, "Entities");
    UT_ASSERT_MSG(err == NULL, "%s", err);

    /* No pills, so the tank's 29x29 block is the whole live set and a square
     * outside it is a square the player genuinely cannot see. */
    f.gs->pb->numPills = 0;

    /* Any slot but the client's own is hostile: alliances start empty. */
    BYTE hostile = (BYTE)((f.me == 0) ? 1 : 0);
    UT_ASSERT_MSG(playersIsAllie(&f.gs->plyrs, f.me, hostile) != TRUE,
                  "slots %u and %u are allied", (unsigned)f.me,
                  (unsigned)hostile);

    /* Five squares off is inside the 29x29 block; twenty is outside it and
     * still inside the 55x55 box the server culls other tanks at, which is
     * the case the filter exists for — the client is told about the tank and
     * has to decline to draw it. */
    int dir = overviewAwayFromEdge(f.tankMX);
    BYTE nearX = (BYTE)((int)f.tankMX + dir * 5);
    BYTE farX = (BYTE)((int)f.tankMX + dir * 20);
    BYTE atY = f.tankMY;

    /* A tank standing in trees is hidden from the list by sight rules that
     * have nothing to do with the overview, which would leave the far arm
     * below passing for the wrong reason. Grass under both squares takes that
     * out of the picture. */
    mapSetPos(f.gs, &f.gs->mp, nearX, atY, GRASS, TRUE, TRUE);
    mapSetPos(f.gs, &f.gs->mp, farX, atY, GRASS, TRUE, TRUE);

    /* The hostile tank, driven straight into the client's own player table —
     * the same state a snapshot would have left behind. pixelX/pixelY of 8
     * put it in the middle of its square, so the list reports the square it
     * was placed on. */
    player *bogey = &f.gs->plyrs->item[hostile];
    bogey->inUse = TRUE;
    strcpy(bogey->playerName, "Bogey");
    bogey->frame = 0;
    bogey->onBoat = FALSE;
    bogey->pixelX = 8;
    bogey->pixelY = 8;
    bogey->mapY = atY;

    const OverviewMap *om = clientSimGetOverviewMap(f.cs);
    UT_ASSERT_MSG(om != NULL, "clientSimGetOverviewMap returned NULL");

    screenTanks tks;
    BYTE gotX = 0;
    BYTE gotY = 0;

    /* Inside the block: in the list, and the filter lets it through. */
    bogey->mapX = nearX;
    clientSimDisplayTick(f.cs, false);
    overviewEntitiesPrepare(f.cs, &tks);
    UT_ASSERT_MSG(overviewFindTank(&tks, hostile, &gotX, &gotY) == TRUE,
                  "the hostile tank is missing from the prepared list at "
                  "%u,%u", (unsigned)nearX, (unsigned)atY);
    UT_ASSERT_MSG(gotX == nearX && gotY == atY,
                  "the prepared list puts the hostile tank on %u,%u, expected "
                  "%u,%u — the full-map rect is not reporting absolute "
                  "squares", (unsigned)gotX, (unsigned)gotY, (unsigned)nearX,
                  (unsigned)atY);
    UT_ASSERT_MSG(overviewEntityIsVisible(om, gotX, gotY) == true,
                  "square %u,%u is inside the tank's block and the filter "
                  "still hides an enemy on it", (unsigned)gotX,
                  (unsigned)gotY);

    /* Our own tank comes back too, and passes on the square at the centre of
     * its own block. */
    UT_ASSERT_MSG(overviewFindTank(&tks, f.me, &gotX, &gotY) == TRUE,
                  "the local tank is missing from the prepared list");
    UT_ASSERT_MSG(overviewEntityIsVisible(om, gotX, gotY) == true,
                  "the local tank is filtered out of its own square %u,%u",
                  (unsigned)gotX, (unsigned)gotY);
    screenTanksDestroy(&tks);

    /* Twenty squares off: still in the list — the client has lost none of
     * what it knew — and the filter is what takes it off the picture. */
    bogey->mapX = farX;
    clientSimDisplayTick(f.cs, false);
    overviewEntitiesPrepare(f.cs, &tks);
    UT_ASSERT_MSG(overviewFindTank(&tks, hostile, &gotX, &gotY) == TRUE,
                  "the hostile tank dropped out of the prepared list at %u,%u "
                  "— this case would then prove absence, not filtering",
                  (unsigned)farX, (unsigned)atY);
    UT_ASSERT_MSG(gotX == farX && gotY == atY,
                  "the prepared list puts the hostile tank on %u,%u, expected "
                  "%u,%u", (unsigned)gotX, (unsigned)gotY, (unsigned)farX,
                  (unsigned)atY);
    UT_ASSERT_MSG((om->flags[farX][atY] & OVERVIEW_F_LIVE) == 0,
                  "square %u,%u is live — it was meant to be outside the "
                  "tank's block", (unsigned)farX, (unsigned)atY);
    UT_ASSERT_MSG(overviewEntityIsVisible(om, gotX, gotY) == false,
                  "an enemy tank on unseen square %u,%u would be drawn",
                  (unsigned)gotX, (unsigned)gotY);
    screenTanksDestroy(&tks);

    /* The case the exception used to hide: an item view under viewPolicyKey
     * takes the block round the tank away, and the tank has to go dark with
     * the ground it was standing on. Driven through the client's own view
     * entry rather than by writing the memory, so what is pinned here is the
     * path the player takes.
     *
     * One pill of the client's own, forty squares off, so its 15x15 block is
     * nowhere near the tank's square. */
    BYTE pillX = (BYTE)((int)f.tankMX + dir * 40);
    f.gs->pb->numPills = 1;
    f.gs->pb->item[0].owner = f.me;
    f.gs->pb->item[0].armour = PILLBOX_15;
    f.gs->pb->item[0].inTank = FALSE;
    f.gs->pb->item[0].x = pillX;
    f.gs->pb->item[0].y = f.tankMY;

    /* The item view is held or dropped by the per-tick UI pass, which is
     * skipped while the game is starting, over or off the network. Say plainly
     * that it is running so the view stays where the case put it. */
    clientSimSetRunning(f.cs, TRUE);
    clientSimSetNetStatus(f.cs, netRunning);
    clientSimSetGmeStartDelay(f.cs, 0);
    clientSimSetGmeLength(f.cs, -1);

    f.cs->viewPolicy[viewCategoryPill] = viewPolicyKey;
    clientSimPillView(f.cs, 0, 0);
    UT_ASSERT_MSG(clientSimGetViewKind(f.cs) == VIEW_KIND_PILL,
                  "the pill view did not take (kind %u)",
                  (unsigned)clientSimGetViewKind(f.cs));

    clientSimDisplayTick(f.cs, false);
    UT_ASSERT_MSG((om->flags[f.tankMX][f.tankMY] & OVERVIEW_F_LIVE) == 0,
                  "the tank's own square %u,%u is still live inside a key pill "
                  "view — the block round the tank was meant to close",
                  (unsigned)f.tankMX, (unsigned)f.tankMY);
    UT_ASSERT_MSG(overviewEntityIsVisible(om, f.tankMX, f.tankMY) == false,
                  "the local tank would still be drawn on square %u,%u with "
                  "the ground round it gone", (unsigned)f.tankMX,
                  (unsigned)f.tankMY);

    /* Out of the view: the block is built again and the tank is back. */
    clientSimTankView(f.cs);
    clientSimDisplayTick(f.cs, false);
    UT_ASSERT_MSG((om->flags[f.tankMX][f.tankMY] & OVERVIEW_F_LIVE) != 0,
                  "the tank's own square %u,%u did not go live again on "
                  "leaving the view", (unsigned)f.tankMX, (unsigned)f.tankMY);
    UT_ASSERT_MSG(overviewEntityIsVisible(om, f.tankMX, f.tankMY) == true,
                  "the local tank is still hidden on square %u,%u after "
                  "leaving the view", (unsigned)f.tankMX, (unsigned)f.tankMY);

    overviewFixtureStop(&f);
    return 0;
}

/* Seeded into the gunsight out-params before every read, so a call that fails
 * and still writes something shows it. */
#define GUNSIGHT_SENTINEL 0xEE

int run_overview_gunsight(void) {
    OverviewFixture f;
    const char *err = overviewFixtureStart(&f, "Gunsight");
    UT_ASSERT_MSG(err == NULL, "%s", err);

    BYTE mx = GUNSIGHT_SENTINEL;
    BYTE my = GUNSIGHT_SENTINEL;
    BYTE px = GUNSIGHT_SENTINEL;
    BYTE py = GUNSIGHT_SENTINEL;

    /* A tank starts with the sight hidden — the same flag the Show Gunsight
     * preference and auto-hide drive. Nothing to draw, and nothing written. */
    UT_ASSERT_MSG(clientSimGetGunsightPos(f.cs, &mx, &my, &px, &py) != TRUE,
                  "a hidden gunsight reported a position");
    UT_ASSERT_MSG(mx == GUNSIGHT_SENTINEL && my == GUNSIGHT_SENTINEL &&
                      px == GUNSIGHT_SENTINEL && py == GUNSIGHT_SENTINEL,
                  "a failed read overwrote the caller's variables with %u,%u "
                  "%u,%u", (unsigned)mx, (unsigned)my, (unsigned)px,
                  (unsigned)py);

    /* Shown, on a living tank: the case the crosshair is drawn for. */
    clientSimSetGunsight(f.cs, true);
    UT_ASSERT_MSG(clientSimGetGunsightPos(f.cs, &mx, &my, &px, &py) == TRUE,
                  "a shown gunsight on a living tank reported nothing");

    /* Dead but still in its slot, which is what the server leaves behind for
     * the whole of deathWait. The sight is still shown, so dying is the only
     * thing the accessor can be answering to. */
    tankSetArmour(&f.gs->tanks[f.me], (BYTE)(TANK_FULL_ARMOUR + 1));
    mx = my = px = py = GUNSIGHT_SENTINEL;
    UT_ASSERT_MSG(clientSimGetGunsightPos(f.cs, &mx, &my, &px, &py) != TRUE,
                  "a dead tank reported a gunsight position");
    UT_ASSERT_MSG(mx == GUNSIGHT_SENTINEL && my == GUNSIGHT_SENTINEL &&
                      px == GUNSIGHT_SENTINEL && py == GUNSIGHT_SENTINEL,
                  "a failed read overwrote the caller's variables with %u,%u "
                  "%u,%u", (unsigned)mx, (unsigned)my, (unsigned)px,
                  (unsigned)py);

    /* The difference this accessor exists for, pinned on the same state: the
     * tile accessor answers for a dead tank, and answers with the map origin
     * — a crosshair drawn from it would sit in the map's top-left corner. */
    {
        BYTE tileX = GUNSIGHT_SENTINEL;
        BYTE tileY = GUNSIGHT_SENTINEL;

        UT_ASSERT_MSG(clientSimGetGunsightTile(f.cs, &tileX, &tileY) == TRUE,
                      "clientSimGetGunsightTile stopped answering for a dead "
                      "tank — this case would then pin no difference");
        UT_ASSERT_MSG(tileX == 0 && tileY == 0,
                      "clientSimGetGunsightTile reads %u,%u for a dead tank, "
                      "expected the map origin", (unsigned)tileX,
                      (unsigned)tileY);
    }

    /* Respawned: the accessor answers again, so discounting a dead tank is not
     * a one-way door. */
    tankSetArmour(&f.gs->tanks[f.me], (BYTE)TANK_FULL_ARMOUR);
    UT_ASSERT_MSG(clientSimGetGunsightPos(f.cs, &mx, &my, &px, &py) == TRUE,
                  "a respawned tank reported no gunsight position");

    overviewFixtureStop(&f);
    return 0;
}

int run_overview_reset(void) {
    OverviewFixture f;
    const char *err = overviewFixtureStart(&f, "Reset");
    UT_ASSERT_MSG(err == NULL, "%s", err);

    const OverviewMap *om = clientSimGetOverviewMap(f.cs);
    UT_ASSERT_MSG(om != NULL, "clientSimGetOverviewMap returned NULL");

    clientSimDisplayTick(f.cs, false);
    UT_ASSERT_MSG(om->seenCount > 0, "nothing was seen after a tick");

    /* Round over: the memory of the round that ended goes with it. */
    clientSimResetWorld(f.cs);
    UT_ASSERT_MSG(om->seenCount == 0, "seenCount %u after a world reset",
                  om->seenCount);
    UT_ASSERT_MSG(om->liveCount == 0, "liveCount %d after a world reset",
                  om->liveCount);
    {
        int x, y;
        for (x = 0; x < MAP_ARRAY_SIZE; x++) {
            for (y = 0; y < MAP_ARRAY_SIZE; y++) {
                UT_ASSERT_MSG(om->tile[x][y] == OVERVIEW_UNSEEN,
                              "square %d,%d holds tile %u after a world reset",
                              x, y, (unsigned)om->tile[x][y]);
                UT_ASSERT_MSG(om->flags[x][y] == 0,
                              "square %d,%d carries flags %u after a world reset",
                              x, y, (unsigned)om->flags[x][y]);
            }
        }
    }

    clientSimDisplayTick(f.cs, false);
    unsigned seenAgain = om->seenCount;
    UT_ASSERT_MSG(seenAgain > 0, "nothing was seen after the reset and a tick");

    /* A mid-game resync reinstalls the same map with the viewport left alone,
     * and the memory has to survive it — the player has not stopped having
     * been where they have been. */
    {
        BYTE emap[6000] = E_MAP;
        UT_ASSERT_MSG(installCompressedMap(f.cs, emap, OVERVIEW_EMAP_LEN,
                                           "Everard Island", false) == TRUE,
                      "installCompressedMap rejected the Everard Island blob");
    }
    UT_ASSERT_MSG(om->seenCount == seenAgain,
                  "a resync moved seenCount from %u to %u", seenAgain,
                  om->seenCount);

    /* A fresh install — the lobby map-change path — resets the memory and
     * arms the seed: black until the next tick, then the whole map reads
     * dimmed again. */
    {
        BYTE emap[6000] = E_MAP;
        UT_ASSERT_MSG(installCompressedMap(f.cs, emap, OVERVIEW_EMAP_LEN,
                                           "Everard Island", true) == TRUE,
                      "installCompressedMap rejected the fresh install");
    }
    UT_ASSERT_MSG(om->seenCount == 0,
                  "a fresh install left seenCount at %u before the seeding "
                  "tick", om->seenCount);
    clientSimDisplayTick(f.cs, false);
    UT_ASSERT_MSG(om->seenCount ==
                      (unsigned)(MAP_ARRAY_SIZE * MAP_ARRAY_SIZE),
                  "seenCount %u one tick after a fresh install, expected "
                  "every square (%u)", om->seenCount,
                  (unsigned)(MAP_ARRAY_SIZE * MAP_ARRAY_SIZE));

    overviewFixtureStop(&f);
    return 0;
}

/* --------------------------------------------------------------------------
 * The client's own decay clocks, driven through the same fixture: what a
 * display tick stamps, what the region build makes of it, and the view exit
 * that reads the same window.
 * -------------------------------------------------------------------------- */

/* Ticks a stamp is good for under this client's rules, and the stretch of it
 * the fade takes. */
static uint32_t overviewDecayWindow(const ClientSim *cs, ViewCategory cat) {
    return (uint32_t)clientSimGetViewDecaySecs(cs, cat) *
           CLIENT_VIEW_DECAY_TICKS_SEC;
}

static uint32_t overviewDecayFade(void) {
    return (uint32_t)VIEW_DECAY_FADE_SECS * CLIENT_VIEW_DECAY_TICKS_SEC;
}

/* The brightness of the region centred on that square, or -1 when the build
 * gave it none. */
static int overviewRegionAlphaAt(const OverviewMap *om, int cx, int cy,
                                 int half) {
    OverviewRect want = overviewTestBlock(cx, cy, half);
    int i; /* Looping variable */

    for (i = 0; i < om->liveCount; i++) {
        if (om->live[i].left == want.left && om->live[i].top == want.top &&
            om->live[i].right == want.right &&
            om->live[i].bottom == want.bottom) {
            return (int)om->live[i].alpha;
        }
    }
    return -1;
}

int run_overview_decay_mirror(void) {
    OverviewFixture f;
    const char *err = overviewFixtureStart(&f, "Decay");
    UT_ASSERT_MSG(err == NULL, "%s", err);

    UT_ASSERT_MSG(basesGetNumBases(&f.gs->bs) >= 2,
                  "this case needs two bases to move, the map has %u",
                  (unsigned)basesGetNumBases(&f.gs->bs));

    /* A base beside the tank and one a hundred squares off, for the proof that
     * a category off viewPolicyDecay is never stamped and that a stamped one
     * only reaches what the tank is beside. Moved before the pills are placed,
     * so the base-free search below steps off this one too. */
    int away = overviewAwayFromEdge(f.tankMX);
    BYTE nearX = (BYTE)((int)f.tankMX + away * 5);
    BYTE farX = (BYTE)((int)f.tankMX + away * 100);
    f.gs->bs->item[0].x = (BYTE)((int)f.tankMX + away * 4);
    f.gs->bs->item[0].y = f.tankMY;
    f.gs->bs->item[1].x = farX;
    f.gs->bs->item[1].y = f.tankMY;

    /* Two pills of the client's own: one five squares off the tank, well
     * inside VIEW_DECAY_NEAR_TILES, and one a hundred squares away that the
     * player has never been near. The far one is kept off base squares — a
     * base renders from its owner whatever sits on it, and the freeze arm
     * below reads terrain. */
    BYTE farY = f.tankMY;
    int step;
    for (step = 0; step < 16 && basesExistPos(&f.gs->bs, farX, farY) == TRUE;
         step++) {
        farY = (BYTE)(farY + 1);
    }
    UT_ASSERT_MSG(basesExistPos(&f.gs->bs, farX, farY) != TRUE,
                  "no base-free square near %u,%u for the far pill",
                  (unsigned)farX, (unsigned)f.tankMY);

    f.gs->pb->numPills = 2;
    f.gs->pb->item[0].owner = f.me;
    f.gs->pb->item[0].armour = PILLBOX_15;
    f.gs->pb->item[0].inTank = FALSE;
    f.gs->pb->item[0].x = nearX;
    f.gs->pb->item[0].y = f.tankMY;
    f.gs->pb->item[1].owner = f.me;
    f.gs->pb->item[1].armour = PILLBOX_15;
    f.gs->pb->item[1].inTank = FALSE;
    f.gs->pb->item[1].x = farX;
    f.gs->pb->item[1].y = farY;

    OverviewRect farRect = overviewTestBlock(farX, farY, OVERVIEW_PILL_HALF);
    BYTE probeX = 0, probeY = 0;
    UT_ASSERT_MSG(overviewFindTerrainSquare(f.gs, f.me, &farRect, &probeX,
                                            &probeY) == TRUE,
                  "no terrain-driven square in the far pill's block at %u,%u",
                  (unsigned)farX, (unsigned)farY);

    const OverviewMap *om = clientSimGetOverviewMap(f.cs);
    UT_ASSERT_MSG(om != NULL, "clientSimGetOverviewMap returned NULL");

    /* The rules the case runs under, said here rather than taken from
     * whatever the join delivered: the shipped ones to start with, and a
     * window long enough to have a fade inside it. */
    f.cs->viewPolicy[viewCategoryPill] = viewPolicyAlways;
    f.cs->viewPolicy[viewCategoryBase] = viewPolicyOff;
    f.cs->viewPolicy[viewCategoryAlly] = viewPolicyAlways;
    f.cs->viewDecaySecs[viewCategoryPill] = VIEW_DECAY_DEFAULT_SECS;
    f.cs->viewDecaySecs[viewCategoryBase] = VIEW_DECAY_DEFAULT_SECS;

    /* Nothing is on decay, so nothing has a clock worth keeping. */
    clientSimDisplayTick(f.cs, false);
    UT_ASSERT_MSG(f.cs->pillNearTick[0] == 0,
                  "a pill was stamped at %u with no category on decay",
                  (unsigned)f.cs->pillNearTick[0]);

    /* Pills on decay: the one the tank is beside is stamped every tick, the
     * one a hundred squares off never is. Proximity is the whole test — both
     * are the player's own and both are alive. */
    f.cs->viewPolicy[viewCategoryPill] = viewPolicyDecay;
    clientSimDisplayTick(f.cs, false);
    UT_ASSERT_MSG(f.cs->pillNearTick[0] == f.cs->viewDecayTick,
                  "the pill beside the tank reads %u, expected this tick (%u)",
                  (unsigned)f.cs->pillNearTick[0],
                  (unsigned)f.cs->viewDecayTick);
    UT_ASSERT_MSG(f.cs->pillNearTick[1] == 0,
                  "the pill a hundred squares off was stamped at %u",
                  (unsigned)f.cs->pillNearTick[1]);
    UT_ASSERT_MSG(f.cs->baseNearTick[0] == 0,
                  "the base beside the tank was stamped at %u while its "
                  "category is off decay", (unsigned)f.cs->baseNearTick[0]);

    /* Bases on decay as well, and the same split falls out. */
    f.cs->viewPolicy[viewCategoryBase] = viewPolicyDecay;
    clientSimDisplayTick(f.cs, false);
    UT_ASSERT_MSG(f.cs->baseNearTick[0] == f.cs->viewDecayTick,
                  "the base beside the tank reads %u, expected this tick (%u)",
                  (unsigned)f.cs->baseNearTick[0],
                  (unsigned)f.cs->viewDecayTick);
    UT_ASSERT_MSG(f.cs->baseNearTick[1] == 0,
                  "the base a hundred squares off was stamped at %u",
                  (unsigned)f.cs->baseNearTick[1]);
    f.cs->viewPolicy[viewCategoryBase] = viewPolicyOff;

    /* The far pill's block through a whole window, with its clock written by
     * hand — the tank never goes near it, so nothing re-stamps behind the
     * case's back. Never been near it: no block. */
    uint32_t window = overviewDecayWindow(f.cs, viewCategoryPill);
    uint32_t fade = overviewDecayFade();
    UT_ASSERT_MSG(window > fade,
                  "this case needs a window longer than the fade (%u vs %u)",
                  (unsigned)window, (unsigned)fade);
    ASSERT_RECT_LIVE(om, farRect, FALSE, "a pill the player has never neared");

    /* Driven past: the block lights up, fully bright. */
    f.cs->pillNearTick[1] = f.cs->viewDecayTick;
    clientSimDisplayTick(f.cs, false);
    ASSERT_RECT_LIVE(om, farRect, TRUE, "a pill inside its decay window");
    UT_ASSERT_MSG(overviewRegionAlphaAt(om, farX, farY, OVERVIEW_PILL_HALF) ==
                      255,
                  "a fresh decay block came out at alpha %d, expected 255",
                  overviewRegionAlphaAt(om, farX, farY, OVERVIEW_PILL_HALF));
    BYTE liveTile = om->tile[probeX][probeY];

    /* Half way through the fade: still there, no longer full. */
    f.cs->pillNearTick[1] = f.cs->viewDecayTick - (window - fade / 2);
    clientSimDisplayTick(f.cs, false);
    int fadedAlpha = overviewRegionAlphaAt(om, farX, farY, OVERVIEW_PILL_HALF);
    ASSERT_RECT_LIVE(om, farRect, TRUE, "a pill part way through its fade");
    UT_ASSERT_MSG(fadedAlpha > 0 && fadedAlpha < 255,
                  "half way through the fade the block is at alpha %d",
                  fadedAlpha);

    /* Run out: the block goes, and what it was showing freezes. */
    f.cs->pillNearTick[1] = f.cs->viewDecayTick - window;
    clientSimDisplayTick(f.cs, false);
    ASSERT_RECT_LIVE(om, farRect, FALSE, "a pill whose window has run out");
    UT_ASSERT_MSG(overviewRegionAlphaAt(om, farX, farY, OVERVIEW_PILL_HALF) < 0,
                  "an expired pill still has a region");
    BYTE frozenTile = om->tile[probeX][probeY];
    UT_ASSERT_MSG(frozenTile != OVERVIEW_UNSEEN,
                  "square %u,%u went unseen when its window ran out",
                  (unsigned)probeX, (unsigned)probeY);
    UT_ASSERT_MSG(frozenTile == liveTile,
                  "the farewell stamp moved square %u,%u from tile %u to %u "
                  "with nothing on the ground changed",
                  (unsigned)probeX, (unsigned)probeY, (unsigned)liveTile,
                  (unsigned)frozenTile);

    {
        bool isMine = FALSE;
        BYTE was = mapGetPos(&f.gs->mp, probeX, probeY);
        BYTE now = overviewFlipTerrain(was);

        mapSetPos(f.gs, &f.gs->mp, probeX, probeY, now, TRUE, TRUE);
        UT_ASSERT_MSG(viewportCalcSquarePure(f.gs, f.me, probeX, probeY,
                                             &isMine) != frozenTile,
                      "terrain %u and %u render as the same tile at %u,%u — "
                      "the freeze check would pass on its own",
                      (unsigned)was, (unsigned)now, (unsigned)probeX,
                      (unsigned)probeY);
    }
    clientSimDisplayTick(f.cs, false);
    UT_ASSERT_MSG(om->tile[probeX][probeY] == frozenTile,
                  "an expired block picked up a terrain change at %u,%u: tile "
                  "%u, expected %u", (unsigned)probeX, (unsigned)probeY,
                  (unsigned)om->tile[probeX][probeY], (unsigned)frozenTile);

    /* Driving past again starts the window over and the block comes back. */
    f.cs->pillNearTick[1] = f.cs->viewDecayTick;
    clientSimDisplayTick(f.cs, false);
    ASSERT_RECT_LIVE(om, farRect, TRUE, "a pill the player has come back to");
    UT_ASSERT_MSG(overviewRegionAlphaAt(om, farX, farY, OVERVIEW_PILL_HALF) ==
                      255,
                  "coming back left the block at alpha %d, expected 255",
                  overviewRegionAlphaAt(om, farX, farY, OVERVIEW_PILL_HALF));

    /* An ally the server is not sending arrives as a hidden stub, which zeroes
     * its players entry. A tank of the player's own parked by the map corner is
     * the only place that zeroed entry could ever read as near, so that is
     * where the stamp has to be shown to skip it. */
    {
        BYTE ally = MAX_TANKS;
        BYTE cornerM = (BYTE)(VIEW_DECAY_NEAR_TILES / 2);
        BYTE slot;

        for (slot = 0; slot < MAX_TANKS; slot++) {
            if (slot != f.me && playersIsInUse(&f.gs->plyrs, slot) != TRUE) {
                ally = slot;
                break;
            }
        }
        UT_ASSERT_MSG(ally < MAX_TANKS,
                      "no free player slot for the stubbed ally");

        playersSetPlayer(f.cs, &f.gs->plyrs, f.me, ally, (char *)"Stub", "??",
                         0, 0, 0, 0, 0, FALSE, 0, NULL, FALSE);
        tankSetWorld(f.gs, &f.gs->tanks[f.me],
                     (WORLD)(cornerM << TANK_SHIFT_MAPSIZE),
                     (WORLD)(cornerM << TANK_SHIFT_MAPSIZE), 0, false);
        f.cs->viewPolicy[viewCategoryAlly] = viewPolicyDecay;
        f.cs->viewDecaySecs[viewCategoryAlly] = VIEW_DECAY_DEFAULT_SECS;
        f.cs->allyNearTick[ally] = 0;

        clientSimDisplayTick(f.cs, false);
        UT_ASSERT_MSG(f.cs->allyNearTick[ally] == 0,
                      "a stubbed ally was stamped at %u off its zeroed entry",
                      (unsigned)f.cs->allyNearTick[ally]);

        /* The same slot with a real square beside the tank still stamps, so
         * what the skip reads is the zeroed entry and not the whole category. */
        playersUpdate(&f.gs->plyrs, ally, cornerM, (BYTE)(cornerM + 1), 0, 0, 0,
                      FALSE, 0, 0, 0, 0, 0);
        clientSimDisplayTick(f.cs, false);
        UT_ASSERT_MSG(f.cs->allyNearTick[ally] == f.cs->viewDecayTick,
                      "an ally beside the tank reads %u, expected this tick (%u)",
                      (unsigned)f.cs->allyNearTick[ally],
                      (unsigned)f.cs->viewDecayTick);
        f.cs->viewPolicy[viewCategoryAlly] = viewPolicyAlways;
    }

    /* Where the player has been belongs to the round they were in: a world
     * reset and a fresh map install each start the clocks over. */
    clientSimResetWorld(f.cs);
    UT_ASSERT_MSG(f.cs->viewDecayTick == 0 && f.cs->pillNearTick[1] == 0,
                  "a world reset left the clock at tick %u, pill %u",
                  (unsigned)f.cs->viewDecayTick,
                  (unsigned)f.cs->pillNearTick[1]);

    f.cs->pillNearTick[1] = 4242;
    {
        BYTE emap[6000] = E_MAP;
        UT_ASSERT_MSG(installCompressedMap(f.cs, emap, OVERVIEW_EMAP_LEN,
                                           "Everard Island", false) == TRUE,
                      "installCompressedMap rejected the resync blob");
    }
    UT_ASSERT_MSG(f.cs->pillNearTick[1] == 4242,
                  "a mid-game resync cleared a clock it should have left "
                  "alone (%u)", (unsigned)f.cs->pillNearTick[1]);

    {
        BYTE emap[6000] = E_MAP;
        UT_ASSERT_MSG(installCompressedMap(f.cs, emap, OVERVIEW_EMAP_LEN,
                                           "Everard Island", true) == TRUE,
                      "installCompressedMap rejected the fresh install");
    }
    UT_ASSERT_MSG(f.cs->pillNearTick[1] == 0,
                  "a fresh map install left the clock at %u",
                  (unsigned)f.cs->pillNearTick[1]);

    overviewFixtureStop(&f);
    return 0;
}

int run_overview_decay_view_exit(void) {
    OverviewFixture f;
    const char *err = overviewFixtureStart(&f, "DecayView");
    UT_ASSERT_MSG(err == NULL, "%s", err);

    /* One pill of the client's own, a hundred squares off, so the tank is
     * never near enough to re-stamp its clock. */
    BYTE pillX = (BYTE)((int)f.tankMX + overviewAwayFromEdge(f.tankMX) * 100);
    f.gs->pb->numPills = 1;
    f.gs->pb->item[0].owner = f.me;
    f.gs->pb->item[0].armour = PILLBOX_15;
    f.gs->pb->item[0].inTank = FALSE;
    f.gs->pb->item[0].x = pillX;
    f.gs->pb->item[0].y = f.tankMY;

    /* The exit lives in the per-tick UI pass, which is skipped while the game
     * is starting, over or off the network. Say plainly that it is running, so
     * what the case drives is the exit and not one of those. */
    clientSimSetRunning(f.cs, TRUE);
    clientSimSetNetStatus(f.cs, netRunning);
    clientSimSetGmeStartDelay(f.cs, 0);
    clientSimSetGmeLength(f.cs, -1);

    f.cs->viewPolicy[viewCategoryPill] = viewPolicyAlways;
    f.cs->viewDecaySecs[viewCategoryPill] = VIEW_DECAY_DEFAULT_SECS;

    clientSimPillView(f.cs, 0, 0);
    UT_ASSERT_MSG(clientSimGetViewKind(f.cs) == VIEW_KIND_PILL &&
                      clientSimGetViewTarget(f.cs) == 0,
                  "the pill view did not take (kind %u target %u)",
                  (unsigned)clientSimGetViewKind(f.cs),
                  (unsigned)clientSimGetViewTarget(f.cs));

    /* Under the shipped rule the view is the player's to keep, tick after
     * tick — the control for everything below. */
    clientSimDisplayTick(f.cs, false);
    UT_ASSERT_MSG(clientSimGetViewKind(f.cs) == VIEW_KIND_PILL,
                  "a tick under viewPolicyAlways dropped the pill view");

    /* On decay, with a pill the player has never been near, the server has
     * already stopped sending its squares — so the view goes. */
    f.cs->viewPolicy[viewCategoryPill] = viewPolicyDecay;
    clientSimDisplayTick(f.cs, false);
    UT_ASSERT_MSG(clientSimGetViewKind(f.cs) == VIEW_KIND_TANK,
                  "an unstamped pill left the client in view kind %u",
                  (unsigned)clientSimGetViewKind(f.cs));

    /* The cycling reads the same window, so the view cannot be re-entered on
     * a pill whose clock has run out either — an expired category is an empty
     * cycle, and an empty cycle stays in the tank view. */
    clientSimPillView(f.cs, 0, 0);
    UT_ASSERT_MSG(clientSimGetViewKind(f.cs) == VIEW_KIND_TANK,
                  "an unstamped pill was still enterable, view kind %u",
                  (unsigned)clientSimGetViewKind(f.cs));

    /* Freshly driven past, the pill is back in the cycle and the view holds. */
    f.cs->pillNearTick[0] = f.cs->viewDecayTick;
    clientSimPillView(f.cs, 0, 0);
    UT_ASSERT_MSG(clientSimGetViewKind(f.cs) == VIEW_KIND_PILL,
                  "the pill view did not take a second time");
    clientSimDisplayTick(f.cs, false);
    UT_ASSERT_MSG(clientSimGetViewKind(f.cs) == VIEW_KIND_PILL,
                  "a pill inside its window lost the view");

    /* And the tick the window runs out is the tick the view drops. */
    f.cs->pillNearTick[0] =
        f.cs->viewDecayTick - overviewDecayWindow(f.cs, viewCategoryPill);
    clientSimDisplayTick(f.cs, false);
    UT_ASSERT_MSG(clientSimGetViewKind(f.cs) == VIEW_KIND_TANK,
                  "an expired window left the client in view kind %u",
                  (unsigned)clientSimGetViewKind(f.cs));

    overviewFixtureStop(&f);
    return 0;
}

/* --------------------------------------------------------------------------
 * The same reveal over the real UDP path, where the map arrives as a download
 * and terrain changes arrive out of band as map events rather than being
 * written into the client's own sim. The mask has to come out the same.
 * -------------------------------------------------------------------------- */

/* Convergence ceilings for a clean path. Every pump costs a millisecond of
 * SDL_Delay, so these are bounds to report a failure against, not expected
 * counts - the harness contract is "converges within N pumps", never a packet
 * trace. */
#define OVERVIEW_LOOP_CONNECT_MAX 2000
#define OVERVIEW_LOOP_READY_MAX   2000
#define OVERVIEW_LOOP_MAP_MAX     2000

/* Seeds the harness's bolo_rand stream. Nothing on a clean path draws from it;
 * it keeps the run reproducible regardless. */
#define OVERVIEW_LOOP_SEED 0x5EED0Fu

/* Half-width of the block the hidden square is picked from, far enough off the
 * tank that the tank's own 29x29 block cannot reach it. */
#define OVERVIEW_LOOP_FAR_HALF 4

static bool overviewLoopConnected(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED;
}

/* The server only flips its own download-complete flag once the map's bytes
 * are acked back on CHANNEL_BULK, a round-trip after the client reports
 * CONNECTED. Waiting for it means snapshots have been flowing for a while, so
 * the tank position read below is one the wire delivered rather than whatever
 * the slot-assignment path left on a freshly created tank. */
static bool overviewLoopServerReady(LoopbackHarness *h, void *user) {
    (void)user;
    return transportUdpServerTestDownloadComplete(
        (int)clientSimGetMyPlayerNum(h->cs));
}

static bool overviewLoopHaveTank(LoopbackHarness *h, void *user) {
    BYTE mx = 0; /* Discarded - the position is read again once settled */
    BYTE my = 0;

    (void)user;
    return clientSimGetMyTankMapPos(h->cs, &mx, &my) == TRUE;
}

/* The square and terrain overviewLoopTerrainApplied is waiting on. */
typedef struct OverviewTerrainWait {
    BYTE x;
    BYTE y;
    BYTE terrain;
} OverviewTerrainWait;

static bool overviewLoopTerrainApplied(LoopbackHarness *h, void *user) {
    const OverviewTerrainWait *w = (const OverviewTerrainWait *)user;

    return clientSimGetMapTerrain(h->cs, w->x, w->y) == w->terrain;
}

int run_overview_loopback(void) {
    LoopbackHarness h;
    int at; /* Pump the last wait converged on, or -1 */

    if (loopbackHarnessStart(&h, "Loop", /*lobbyMode*/ false,
                             /*impairSpec*/ NULL, OVERVIEW_LOOP_SEED) != true) {
        loopbackHarnessStop(&h);
        UT_FAIL("harness start (overview loopback) failed");
    }

    at = loopbackHarnessPumpUntil(&h, OVERVIEW_LOOP_CONNECT_MAX,
                                  overviewLoopConnected, NULL);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the client never reached CONNECTED within %d pumps",
                OVERVIEW_LOOP_CONNECT_MAX);
    }
    at = loopbackHarnessPumpUntil(&h, OVERVIEW_LOOP_READY_MAX,
                                  overviewLoopServerReady, NULL);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the server never saw the map download acked within %d pumps",
                OVERVIEW_LOOP_READY_MAX);
    }
    at = loopbackHarnessPumpUntil(&h, OVERVIEW_LOOP_READY_MAX,
                                  overviewLoopHaveTank, NULL);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the client never got a tank within %d pumps",
                OVERVIEW_LOOP_READY_MAX);
    }

    GameSim *gs = clientSimGetGameSim(h.cs);
    BYTE me = clientSimGetMyPlayerNum(h.cs);
    int slot = (int)me;
    BYTE tankMX = 0;
    BYTE tankMY = 0;
    if (gs == NULL || clientSimGetMyTankMapPos(h.cs, &tankMX, &tankMY) != TRUE) {
        loopbackHarnessStop(&h);
        UT_FAIL("no client sim or tank position after convergence");
    }

    /* Nothing below touches pills or terrain before the assertions: the world
     * being checked is the one the download and the snapshots delivered. Any
     * pill the map ships that the player can view through simply adds its block
     * to the expected set. */
    clientSimDisplayTick(h.cs, false);

    const OverviewMap *om = clientSimGetOverviewMap(h.cs);
    if (om == NULL) {
        loopbackHarnessStop(&h);
        UT_FAIL("clientSimGetOverviewMap returned NULL");
    }

    OverviewRect expect[OVERVIEW_MAX_REGIONS];
    OverviewViewInputs in;
    clientSimFillOverviewViewInputs(h.cs, &in);
    OverviewRect tankBlock = overviewTestBlock(tankMX, tankMY,
                                               OVERVIEW_TANK_HALF);
    int n = overviewMapBuildRegions(gs, me, &in, NULL, &tankBlock, expect,
                                    OVERVIEW_MAX_REGIONS);
    if (n < 1) {
        loopbackHarnessStop(&h);
        UT_FAIL("a client with a tank produced %d regions", n);
    }
    if (om->liveCount != n) {
        loopbackHarnessStop(&h);
        UT_FAIL("liveCount %d, expected %d", om->liveCount, n);
    }

    /* Every square of the map, either side of the union. The mismatch is
     * carried out of the loop rather than asserted in it, so the harness is
     * always torn down before the failure returns. */
    const char *bad = NULL;
    int badX = 0;
    int badY = 0;
    unsigned badGot = 0;
    unsigned badWant = 0;
    int x, y;
    for (x = 0; x < MAP_ARRAY_SIZE && bad == NULL; x++) {
        for (y = 0; y < MAP_ARRAY_SIZE; y++) {
            BYTE flags = om->flags[x][y];

            if (overviewInAnyRect(expect, n, x, y) == TRUE) {
                bool isMine = FALSE;
                BYTE want = viewportCalcSquarePure(gs, me, (BYTE)x, (BYTE)y,
                                                   &isMine);
                if ((flags & OVERVIEW_F_LIVE) == 0) {
                    bad = "is inside a region but not live";
                } else if (om->tile[x][y] != want) {
                    bad = "holds a tile the calculator disagrees with";
                    badGot = om->tile[x][y];
                    badWant = want;
                } else if (((flags & OVERVIEW_F_MINE) != 0) !=
                           (isMine == TRUE)) {
                    bad = "carries the wrong mine flag";
                    badGot = (flags & OVERVIEW_F_MINE) != 0;
                    badWant = (isMine == TRUE);
                }
            } else {
                if ((flags & OVERVIEW_F_LIVE) != 0) {
                    bad = "is outside every region but live";
                } else if (om->tile[x][y] == OVERVIEW_UNSEEN) {
                    bad = "was left out of the seed";
                    badGot = OVERVIEW_UNSEEN;
                    badWant = 0;
                }
            }
            if (bad != NULL) {
                badX = x;
                badY = y;
                break;
            }
        }
    }
    if (bad != NULL) {
        loopbackHarnessStop(&h);
        UT_FAIL("square %d,%d %s (got %u, expected %u)", badX, badY, bad,
                badGot, badWant);
    }
    if (om->seenCount != (unsigned)(MAP_ARRAY_SIZE * MAP_ARRAY_SIZE)) {
        loopbackHarnessStop(&h);
        UT_FAIL("seenCount %u after the seed, expected every square (%u)",
                om->seenCount, (unsigned)(MAP_ARRAY_SIZE * MAP_ARRAY_SIZE));
    }

    /* One square the tank can see and one nothing has revealed. Both are found
     * by probing for a tile that actually moves when the ground under it is
     * swapped, which steps off pill and base squares - those render from their
     * owner, so a terrain change on one would prove nothing either way. The
     * probe puts back what it wrote and no pump runs during it, so the client's
     * map is untouched by the time the events are staged. */
    OverviewRect liveRect = expect[0]; /* the tank's rect is written first */
    OverviewRect farRect = overviewTestBlock(
        (int)tankMX + overviewAwayFromEdge(tankMX) * 100, (int)tankMY,
        OVERVIEW_LOOP_FAR_HALF);
    BYTE liveX = 0, liveY = 0, farX = 0, farY = 0;

    if (overviewFindTerrainSquare(gs, me, &liveRect, &liveX, &liveY) != TRUE) {
        loopbackHarnessStop(&h);
        UT_FAIL("no terrain-driven square in the tank's block at %u,%u",
                (unsigned)tankMX, (unsigned)tankMY);
    }
    if (overviewFindTerrainSquare(gs, me, &farRect, &farX, &farY) != TRUE) {
        loopbackHarnessStop(&h);
        UT_FAIL("no terrain-driven square in the block 100 off the tank");
    }
    if ((om->flags[farX][farY] & OVERVIEW_F_LIVE) != 0 ||
        overviewInAnyRect(expect, n, farX, farY) == TRUE) {
        loopbackHarnessStop(&h);
        UT_FAIL("the square meant to stay hidden, %u,%u, is live",
                (unsigned)farX, (unsigned)farY);
    }

    BYTE liveTileBefore = om->tile[liveX][liveY];
    BYTE farTileBefore = om->tile[farX][farY];
    OverviewTerrainWait wait;

    /* Both changes go out the way a sim tick emits one: the server's own map
     * moves and an EVENT_MAP_CHANGE rides CHANNEL_MAP to the client. */
    wait.x = liveX;
    wait.y = liveY;
    wait.terrain = overviewFlipTerrain(clientSimGetMapTerrain(h.cs, liveX, liveY));
    if (transportUdpServerTestAddMapEvent(h.sim, slot, liveX, liveY,
                                          wait.terrain) != true) {
        loopbackHarnessStop(&h);
        UT_FAIL("staging a map event on the live square %u,%u was rejected",
                (unsigned)liveX, (unsigned)liveY);
    }
    at = loopbackHarnessPumpUntil(&h, OVERVIEW_LOOP_MAP_MAX,
                                  overviewLoopTerrainApplied, &wait);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the change on live square %u,%u never reached the client "
                "within %d pumps", (unsigned)liveX, (unsigned)liveY,
                OVERVIEW_LOOP_MAP_MAX);
    }

    wait.x = farX;
    wait.y = farY;
    wait.terrain = overviewFlipTerrain(clientSimGetMapTerrain(h.cs, farX, farY));
    if (transportUdpServerTestAddMapEvent(h.sim, slot, farX, farY,
                                          wait.terrain) != true) {
        loopbackHarnessStop(&h);
        UT_FAIL("staging a map event on the hidden square %u,%u was rejected",
                (unsigned)farX, (unsigned)farY);
    }
    at = loopbackHarnessPumpUntil(&h, OVERVIEW_LOOP_MAP_MAX,
                                  overviewLoopTerrainApplied, &wait);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the change on hidden square %u,%u never reached the client "
                "within %d pumps", (unsigned)farX, (unsigned)farY,
                OVERVIEW_LOOP_MAP_MAX);
    }

    clientSimDisplayTick(h.cs, false);

    /* The live square follows the change. The tile it should now show is
     * recomputed here rather than before the pumps, so tree growth landing on
     * or beside it while the events were in flight moves the memory and the
     * expectation together instead of breaking the comparison. */
    if ((om->flags[liveX][liveY] & OVERVIEW_F_LIVE) == 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("square %u,%u dropped out of the live set while the change was "
                "in flight - did the tank move?", (unsigned)liveX,
                (unsigned)liveY);
    }
    {
        bool isMine = FALSE;
        BYTE wantTile = viewportCalcSquarePure(gs, me, liveX, liveY, &isMine);

        if (wantTile == liveTileBefore) {
            loopbackHarnessStop(&h);
            UT_FAIL("terrain %u renders as tile %u at %u,%u either way - the "
                    "live check would pass on its own",
                    (unsigned)wait.terrain, (unsigned)wantTile,
                    (unsigned)liveX, (unsigned)liveY);
        }
        if (om->tile[liveX][liveY] != wantTile) {
            loopbackHarnessStop(&h);
            UT_FAIL("live square %u,%u holds tile %u, expected %u",
                    (unsigned)liveX, (unsigned)liveY,
                    (unsigned)om->tile[liveX][liveY], (unsigned)wantTile);
        }
    }

    /* The hidden square does not. The client has the new terrain - the pump
     * above waited for it - and the memory still shows what the seed stamped
     * there, so a change on ground the player cannot see stays invisible. */
    if (om->tile[farX][farY] != farTileBefore) {
        loopbackHarnessStop(&h);
        UT_FAIL("a map event restamped hidden square %u,%u: tile %u, "
                "expected %u", (unsigned)farX, (unsigned)farY,
                (unsigned)om->tile[farX][farY], (unsigned)farTileBefore);
    }

    loopbackHarnessStop(&h);
    return 0;
}
