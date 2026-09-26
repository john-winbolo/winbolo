/*
 * A tank on a diagonal must be drawn stepping on both axes on the same
 * frame, not one pixel up and then one pixel across.
 *
 * The sim moves both axes in the same tick (tankMoveUnified), exactly as
 * the original game did. The staircase comes from drawing: each axis is cut
 * down to its game pixel on its own, and the two axes almost never share a
 * sub-pixel phase, so they cross their pixel lines on different ticks (and,
 * with render-clock interpolation, on different render frames). The drawer
 * now cuts a diagonal position to the diagonal pixel lattice instead
 * (tank_diagonal_snap.h), used by clientSnapshotRenderInterp for other
 * tanks and by mapViewDrawTanks for the own tank.
 *
 * Pins:
 *   1. A real sim run on each diagonal, with the axes half a pixel apart,
 *      staircases when cut per axis and steps on both axes when snapped,
 *      at the same average speed.
 *   2. Over every sub-pixel phase and three speeds, a per-tick diagonal
 *      never takes a one-axis step once snapped.
 *   3. The snap puts its result on whole pixels, leaves a snapped point
 *      where it is, stays close to the true position and leaves the other
 *      facings alone.
 *   4. The render-clock lerp keeps x + y fixed on a NE run, so the snap
 *      does not flicker between frames.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"
#include "game_sim.h"
#include "tank.h"
#include "bolo_map.h"
#include "interpolation.h"
#include "tank_diagonal_snap.h"
#include "util.h"
#include "test_harness.h"

/* Put tank 0 on a patch of one terrain, at rest, facing angle, offX/offY
 * world units from the middle of square (mx,my). */
static void placeDiag(ServerSim *sim, BYTE terrain, BYTE mx, BYTE my,
                      TURNTYPE angle, int offX, int offY) {
    int dx, dy;
    for (dy = -12; dy <= 12; dy++) {
        for (dx = -12; dx <= 12; dx++) {
            mapSetPos(&sim->sim, &sim->sim.mp, (BYTE)(mx + dx), (BYTE)(my + dy),
                      terrain, FALSE, FALSE);
        }
    }
    tankSetWorld(&sim->sim, &sim->sim.tanks[0],
                 (WORLD)((mx << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE + offX),
                 (WORLD)((my << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE + offY),
                 angle, FALSE);
    tankSetSpeed(&sim->sim.tanks[0], 0);
    tankSetOnBoat(&sim->sim.tanks[0], FALSE);
    sim->sim.tanks[0]->residualSpeed = 0;
    sim->sim.tanks[0]->autoSlowdown = FALSE;
}

/* Count the pixel steps between two drawn points: a one-axis step moves
 * only x or only y, a both-axis step moves both. */
static void countStep(int px0, int py0, int px1, int py1,
                      int *oneAxis, int *bothAxes) {
    int sx = (px1 != px0), sy = (py1 != py0);
    if (sx != sy) {
        (*oneAxis)++;
    } else if (sx) {
        (*bothAxes)++;
    }
}

/* Drive tank 0 along one diagonal for 120 ticks and count the drawn steps
 * over the last 60 (full speed), cut per axis and snapped. */
static int simRun(ServerSim *sim, const char *label, BYTE terrain,
                  TURNTYPE angle) {
    const int ticks = 120, from = 60;
    int t;
    int cutOne = 0, cutBoth = 0, snapOne = 0, snapBoth = 0;
    int cx0 = 0, cy0 = 0, sx0 = 0, sy0 = 0;
    int cxFrom = 0, cyFrom = 0, sxFrom = 0, syFrom = 0;
    int dir16 = tankDiagonalDir16((int)angle);

    /* Half a pixel between the two axes' sub-pixel phases. */
    placeDiag(sim, terrain, 100, 100, angle, 8, 0);
    for (t = 0; t <= ticks; t++) {
        WORLD x, y;
        int cx, cy, sx, sy;
        if (t > 0) {
            tankUpdate(&sim->sim, &sim->sim.tanks[0], TACCEL, FALSE, FALSE);
        }
        tankGetWorld(&sim->sim.tanks[0], &x, &y);
        /* Cut per axis, as tankGetScreenPX does. */
        cx = ((int)x - TANK_SUBTRACT) >> 4;
        cy = ((int)y - TANK_SUBTRACT) >> 4;
        /* Snapped, as the drawer does. */
        sx = (int)x - TANK_SUBTRACT;
        sy = (int)y - TANK_SUBTRACT;
        tankDiagonalSnap(dir16, &sx, &sy);
        UT_ASSERT_MSG((sx & 15) == 0 && (sy & 15) == 0,
                      "%s: snapped point off a pixel", label);
        sx /= 16;
        sy /= 16;
        if (t > from) {
            countStep(cx0, cy0, cx, cy, &cutOne, &cutBoth);
            countStep(sx0, sy0, sx, sy, &snapOne, &snapBoth);
        } else if (t == from) {
            cxFrom = cx; cyFrom = cy; sxFrom = sx; syFrom = sy;
        }
        cx0 = cx; cy0 = cy; sx0 = sx; sy0 = sy;
    }
    printf("  %-9s cut per axis: %2d both-axis, %2d one-axis steps;"
           " snapped: %2d both-axis, %2d one-axis; moved (%+d,%+d) vs (%+d,%+d) px\n",
           label, cutBoth, cutOne, snapBoth, snapOne,
           cx0 - cxFrom, cy0 - cyFrom, sx0 - sxFrom, sy0 - syFrom);
    /* The staircase is there without the snap ... */
    UT_ASSERT_MSG(cutOne > 0, "%s: expected a staircase cut per axis", label);
    /* ... and gone with it. */
    UT_ASSERT_MSG(snapOne == 0, "%s: %d one-axis steps after the snap",
                  label, snapOne);
    UT_ASSERT_MSG(snapBoth > 0, "%s: tank did not move", label);
    /* Same speed: over 60 ticks the snapped tank covers the same ground to
     * within the snap's reach at each end. */
    UT_ASSERT_MSG(abs((sx0 - sxFrom) - (cx0 - cxFrom)) <= 2 &&
                  abs((sy0 - syFrom) - (cy0 - cyFrom)) <= 2,
                  "%s: snapped distance differs", label);
    return 0;
}

/* Every sub-pixel phase of both axes, one diagonal, a per-tick step of v
 * world units on each axis: count one-axis steps cut per axis and snapped. */
static void latticeSweep(int dir16, int ddx, int ddy, int v,
                         int *cutOne, int *cutSteps,
                         int *snapOne, int *snapSteps) {
    int phx, phy, t;
    for (phy = 0; phy < 16; phy++) {
        for (phx = 0; phx < 16; phx++) {
            int x = 4096 + phx, y = 4096 + phy;
            int cx0 = x >> 4, cy0 = y >> 4;
            int sx0 = x, sy0 = y;
            tankDiagonalSnap(dir16, &sx0, &sy0);
            for (t = 0; t < 64; t++) {
                int sx, sy, cx, cy, dummy = 0;
                x += ddx * v;
                y += ddy * v;
                cx = x >> 4;
                cy = y >> 4;
                sx = x;
                sy = y;
                tankDiagonalSnap(dir16, &sx, &sy);
                if (cx != cx0 || cy != cy0) {
                    (*cutSteps)++;
                }
                countStep(cx0, cy0, cx, cy, cutOne, &dummy);
                if (sx != sx0 || sy != sy0) {
                    (*snapSteps)++;
                }
                countStep(sx0, sy0, sx, sy, snapOne, &dummy);
                cx0 = cx; cy0 = cy; sx0 = sx; sy0 = sy;
            }
        }
    }
}

int run_tank_diagonal_steps_both_axes(void) {
    static const struct { int dir16, ddx, ddy; const char *name; } kDiag[4] = {
        { 2, 1, -1, "NE" }, { 6, 1, 1, "SE" },
        { 10, -1, 1, "SW" }, { 14, -1, -1, "NW" },
    };
    int i;

    /* ---- 1. Real sim runs ---- */
    {
        ServerSim *sim = ut_make_running_sim("Tester");
        UT_ASSERT(sim != NULL && sim->sim.tanks[0] != NULL);
        printf("[tank_diagonal] sim, 60 full-speed ticks, axes half a pixel apart:\n");
        if (simRun(sim, "NE road", ROAD, (TURNTYPE)BRADIANS_NEAST) != 0 ||
            simRun(sim, "NE grass", GRASS, (TURNTYPE)BRADIANS_NEAST) != 0 ||
            simRun(sim, "SE grass", GRASS, (TURNTYPE)BRADIANS_SEAST) != 0 ||
            simRun(sim, "SW grass", GRASS, (TURNTYPE)BRADIANS_SWEST) != 0 ||
            simRun(sim, "NW grass", GRASS, (TURNTYPE)BRADIANS_NWEST) != 0) {
            serverSimDestroy(sim);
            return 1;
        }
        serverSimDestroy(sim);
    }

    /* ---- 2. Every phase, three speeds, four diagonals ---- */
    {
        static const int kSpeeds[3] = { 11, 8, 4 }; /* road, grass, swamp-ish */
        int s;
        printf("[tank_diagonal] per-tick drawing over all 256 phases:\n");
        for (s = 0; s < 3; s++) {
            int cutOne = 0, cutSteps = 0, snapOne = 0, snapSteps = 0;
            for (i = 0; i < 4; i++) {
                latticeSweep(kDiag[i].dir16, kDiag[i].ddx, kDiag[i].ddy,
                             kSpeeds[s], &cutOne, &cutSteps, &snapOne,
                             &snapSteps);
            }
            printf("  %2d wu/tick: cut per axis %5d of %5d steps one-axis (%d%%);"
                   " snapped %d of %d\n", kSpeeds[s], cutOne, cutSteps,
                   cutSteps ? (100 * cutOne + cutSteps / 2) / cutSteps : 0,
                   snapOne, snapSteps);
            UT_ASSERT(cutOne > 0);
            UT_ASSERT_MSG(snapOne == 0, "%d wu: %d one-axis steps snapped",
                          kSpeeds[s], snapOne);
            /* Fewer, bigger steps: each snapped step moves both axes where
             * the per-axis cut often spends two one-axis steps. The ground
             * covered is checked in part 1. */
            UT_ASSERT(snapSteps > 0);
        }
    }

    /* ---- 3. Snap properties ---- */
    {
        int worst = 0;
        int x, y;
        for (i = 0; i < 4; i++) {
            for (y = 1000; y < 1000 + 64; y++) {
                for (x = 3000; x < 3000 + 64; x++) {
                    int sx = x, sy = y, tx, ty, err;
                    tankDiagonalSnap(kDiag[i].dir16, &sx, &sy);
                    UT_ASSERT((sx & 15) == 0 && (sy & 15) == 0);
                    tx = sx; ty = sy;
                    tankDiagonalSnap(kDiag[i].dir16, &tx, &ty);
                    UT_ASSERT_MSG(tx == sx && ty == sy,
                                  "%s: snap of (%d,%d) not stable", kDiag[i].name,
                                  x, y);
                    /* Distance from the drawn pixel's corner to the true
                     * point's pixel corner (x & ~15), per axis. */
                    err = abs(sx - (x & ~15));
                    if (abs(sy - (y & ~15)) > err) {
                        err = abs(sy - (y & ~15));
                    }
                    if (err > worst) {
                        worst = err;
                    }
                }
            }
        }
        printf("[tank_diagonal] snap moves a tank at most %d world units (%d px) per axis\n",
               worst, worst / 16);
        UT_ASSERT_MSG(worst <= 16, "snap reaches %d world units", worst);

        /* Other facings are left alone. */
        for (i = 0; i < 16; i++) {
            int sx = 3007, sy = 1009;
            if (i == 2 || i == 6 || i == 10 || i == 14) {
                continue;
            }
            tankDiagonalSnap(i, &sx, &sy);
            UT_ASSERT(sx == 3007 && sy == 1009);
        }
        /* The facing buckets match utilGetDir. */
        UT_ASSERT(tankDiagonalDir16(BRADIANS_NEAST) == 2);
        UT_ASSERT(tankDiagonalDir16(BRADIANS_SEAST) == 6);
        UT_ASSERT(tankDiagonalDir16(BRADIANS_SWEST) == 10);
        UT_ASSERT(tankDiagonalDir16(BRADIANS_NWEST) == 14);
        UT_ASSERT(tankDiagonalDir16(8) == 0 && tankDiagonalDir16(9) == 1);
        UT_ASSERT(tankDiagonalDir16(248) == 15 && tankDiagonalDir16(249) == 0);
        {
            int a;
            for (a = 0; a < 256; a++) {
                UT_ASSERT(tankDiagonalDir16(a) == (int)utilGetDir((TURNTYPE)a));
            }
        }
    }

    /* ---- 4. Render-clock lerp on a NE run keeps x + y fixed ---- */
    {
        const BYTE PN = 1;
        InterpContext ctx;
        InterpSnapshot s[3];
        uint32_t now;
        int one = 0, both = 0, sx0 = 0, sy0 = 0;
        memset(s, 0, sizeof(s));
        for (i = 0; i < 3; i++) {
            s[i].worldX = (WORLD)(1000 + 11 * i);
            s[i].worldY = (WORLD)(3000 - 11 * i);
            s[i].angle = (TURNTYPE)BRADIANS_NEAST;
            s[i].alive = TRUE;
        }
        interpCreate(&ctx, 0);
        interpUpdate(&ctx, PN, &s[0], 100, 1000);
        interpUpdate(&ctx, PN, &s[1], 102, 1020);
        interpUpdate(&ctx, PN, &s[2], 104, 1040);
        /* Render frames 1 ms apart across the curr -> pending segment. */
        for (now = 1040; now <= 1060; now++) {
            WORLD x, y;
            TURNTYPE a;
            bool boat;
            int sx, sy;
            UT_ASSERT(interpGetRenderPosition(&ctx, PN, now, 0.0f, &x, &y, &a,
                                              &boat));
            UT_ASSERT_MSG((int)x + (int)y == 4000,
                          "lerp at %u ms: x %u + y %u != 4000", (unsigned)now,
                          (unsigned)x, (unsigned)y);
            sx = (int)x;
            sy = (int)y;
            tankDiagonalSnap(2, &sx, &sy);
            if (now > 1040) {
                countStep(sx0, sy0, sx, sy, &one, &both);
            }
            sx0 = sx; sy0 = sy;
        }
        UT_ASSERT_MSG(one == 0, "lerped NE run: %d one-axis steps", one);
    }

    return 0;
}
