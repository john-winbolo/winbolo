/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Where a builder starts when he is sent out.
 *
 * lgmNewPrimaryRequest (lgm.c) puts him on the tank's centre and then
 * lgmApplyStartOffset moves him LGM_START_OFFSET (64 world units) towards the
 * centre of the square he was sent to. The step is worked out in whole
 * numbers: the distance is the whole part of the square root, and each axis
 * is dx * 64 / distance with the fraction dropped. A square no further off
 * than 64 leaves him on the tank's centre.
 *
 * run_lgm_start_offset — straight east, west, north and south give exactly 64
 *                        along the axis; the four diagonals give (45,45) with
 *                        the signs of the direction; a square centre 64 or
 *                        less away leaves him on the tank; one 65 away moves
 *                        him the full 64; a tank 5 from the edge of a deep
 *                        sea square, sent beyond it, leaves him on the tank,
 *                        and the same with grass there moves him the full 64
 *                        across the edge; with the tank's own square turned
 *                        to river or deep sea he stays on the tank though
 *                        the square across the edge is grass; with the tank
 *                        on a boat he moves the full 64 whatever is there.
 *
 * Every case sends him to harvest a forest square. A harvest costs nothing
 * and is allowed on the tank's own square, which is the only way a square
 * centre can be within 64 of the tank. The squares are turned to forest
 * here, and the tank's own square to grass, so the case does not depend on
 * what the map has there. Calls
 * lgmNewPrimaryRequest directly on ut_make_running_sim (slot 0): it places
 * him afresh from the tank on every call, so no tick has to run between
 * cases.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* sim->sim */
#include "game_sim.h"              /* GameSim: mp/bs/pb, tanks[], lgmen[] */
#include "lgm.h"                   /* lgmNewPrimaryRequest, struct lgmObj */
#include "bolo_map.h"              /* mapSetPos and the terrain constants */
#include "pillbox.h"               /* pillsExistPos */
#include "bases.h"                 /* basesExistPos */
#include "mines.h"                 /* minesExistPos */
#include "tank.h"                  /* tankSetWorld */
#include "test_harness.h"

#define LSO_SLOT 0
#define LSO_FAR  10   /* squares from the tank to a far destination */

/* A square the man can be sent to harvest once it is forest: no base, no
 * pillbox and no mine on it. */
static bool lso_square_free(GameSim *gs, int x, int y) {
    return !basesExistPos(&gs->bs, (BYTE)x, (BYTE)y) &&
           !pillsExistPos(&gs->pb, (BYTE)x, (BYTE)y) &&
           !minesExistPos(&gs->mns, &gs->mp, (BYTE)x, (BYTE)y);
}

/* Find a square for the tank whose own square, the eight squares LSO_FAR
 * away along the axes and the diagonals, and the square next to it on the
 * east are all free. */
static bool lso_find_centre(GameSim *gs, BYTE *ox, BYTE *oy) {
    int x, y, dx, dy;
    for (y = 40; y <= 200; y++) {
        for (x = 40; x <= 200; x++) {
            bool ok = true;
            for (dy = -1; dy <= 1 && ok; dy++) {
                for (dx = -1; dx <= 1 && ok; dx++) {
                    ok = lso_square_free(gs, x + dx * LSO_FAR, y + dy * LSO_FAR);
                }
            }
            if (ok && lso_square_free(gs, x + 1, y)) {
                *ox = (BYTE)x;
                *oy = (BYTE)y;
                return true;
            }
        }
    }
    return false;
}

/* Put the tank at (tankWX, tankWY), send the man to harvest square (mx, my)
 * and check he starts (wantDX, wantDY) from the tank. */
static int lso_case(ServerSim *sim, const char *name, WORLD tankWX, WORLD tankWY,
                    BYTE mx, BYTE my, int wantDX, int wantDY) {
    GameSim *gs = &sim->sim;
    tank *tnk = &gs->tanks[LSO_SLOT];
    lgm *man = &gs->lgmen[LSO_SLOT];
    int gotDX, gotDY;

    tankSetWorld(gs, tnk, tankWX, tankWY, 0, FALSE);
    mapSetPos(gs, &gs->mp, mx, my, FOREST, FALSE, FALSE);
    lgmNewPrimaryRequest(gs, man, tnk, mx, my, LGM_TREE_REQUEST);

    /* Not vacuous: a refused order leaves him where the last case put him. */
    UT_ASSERT_MSG((*man)->state == LGM_STATE_GOING && (*man)->inTank == FALSE,
                  "%s: the harvest order to (%u,%u) never sent the man out "
                  "(state %u)", name, (unsigned)mx, (unsigned)my,
                  (unsigned)(*man)->state);

    gotDX = (int)(*man)->x - (int)tankWX;
    gotDY = (int)(*man)->y - (int)tankWY;
    UT_ASSERT_MSG(gotDX == wantDX && gotDY == wantDY,
                  "%s: the man starts (%d,%d) from the tank, expected (%d,%d)",
                  name, gotDX, gotDY, wantDX, wantDY);
    return 0;
}

int run_lgm_start_offset(void) {
    ServerSim *sim = ut_make_running_sim("Builder");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");

    GameSim *gs = &sim->sim;
    BYTE cx = 0, cy = 0;
    WORLD midX, midY, edgeX;
    int rc = 0;

    UT_ASSERT_MSG(gs->tanks[LSO_SLOT] != NULL && gs->lgmen[LSO_SLOT] != NULL,
                  "slot %d joined without a tank or a man", LSO_SLOT);
    UT_ASSERT_MSG(lso_find_centre(gs, &cx, &cy),
                  "no square found with its far squares and its east "
                  "neighbour free of bases, pillboxes and mines");

    /* The centre of the tank's square, the same way lgmNewPrimaryRequest
     * works out the centre of a destination square. */
    midX = (WORLD)(((WORLD)cx << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE);
    midY = (WORLD)(((WORLD)cy << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE);

    /* The offset is given only off a square a builder can walk on, so the
     * tank's own square is made grass whatever the map has there. */
    mapSetPos(gs, &gs->mp, cx, cy, GRASS, FALSE, FALSE);

    /* A tank starts the game on a boat, and a builder off a boat gets the
     * offset whatever the squares are. The tank is taken off it here so the
     * squares decide, and put back on for the two boat cases at the end. */
    tankSetOnBoat(&gs->tanks[LSO_SLOT], FALSE);

    /* Straight along an axis: the whole 64, the other axis untouched. */
    if (rc == 0) rc = lso_case(sim, "east", midX, midY, (BYTE)(cx + LSO_FAR), cy, 64, 0);
    if (rc == 0) rc = lso_case(sim, "west", midX, midY, (BYTE)(cx - LSO_FAR), cy, -64, 0);
    if (rc == 0) rc = lso_case(sim, "south", midX, midY, cx, (BYTE)(cy + LSO_FAR), 0, 64);
    if (rc == 0) rc = lso_case(sim, "north", midX, midY, cx, (BYTE)(cy - LSO_FAR), 0, -64);

    /* Diagonals: 2560 each way is 3620 away once the fraction is dropped,
     * and 2560 * 64 / 3620 is 45 with the fraction dropped. The same size in
     * all four directions, because C division drops towards zero. */
    if (rc == 0) rc = lso_case(sim, "south-east", midX, midY, (BYTE)(cx + LSO_FAR), (BYTE)(cy + LSO_FAR), 45, 45);
    if (rc == 0) rc = lso_case(sim, "north-west", midX, midY, (BYTE)(cx - LSO_FAR), (BYTE)(cy - LSO_FAR), -45, -45);
    if (rc == 0) rc = lso_case(sim, "north-east", midX, midY, (BYTE)(cx + LSO_FAR), (BYTE)(cy - LSO_FAR), 45, -45);
    if (rc == 0) rc = lso_case(sim, "south-west", midX, midY, (BYTE)(cx - LSO_FAR), (BYTE)(cy + LSO_FAR), -45, 45);

    /* The tank's own square, its centre 64 or less away: he stays on the
     * tank. (10,-5) from the centre is 11 away; (64,0) is exactly 64. */
    if (rc == 0) rc = lso_case(sim, "own square, 11 away", (WORLD)(midX + 10), (WORLD)(midY - 5), cx, cy, 0, 0);
    if (rc == 0) rc = lso_case(sim, "own square, 64 away", (WORLD)(midX + 64), midY, cx, cy, 0, 0);
    if (rc == 0) rc = lso_case(sim, "own square, on the centre", midX, midY, cx, cy, 0, 0);

    /* One past that: he moves the full 64, which leaves him 1 short. The
     * tank is still inside its own square, whose edge is 128 out. */
    if (rc == 0) rc = lso_case(sim, "own square, 65 east of centre", (WORLD)(midX + 65), midY, cx, cy, -64, 0);
    if (rc == 0) rc = lso_case(sim, "own square, 65 north of centre", midX, (WORLD)(midY - 65), cx, cy, 0, 64);

    /* The tank 5 short of its square's east edge, sent far east: the 64
     * would put him 59 into the next square. A builder cannot walk on deep
     * sea, so he stays on the tank, where his first step is refused the way
     * it always was. With grass there he can, and gets the full 64. */
    edgeX = (WORLD)((((WORLD)cx + 1) << TANK_SHIFT_MAPSIZE) - 5);
    mapSetPos(gs, &gs->mp, (BYTE)(cx + 1), cy, DEEP_SEA, FALSE, FALSE);
    if (rc == 0) rc = lso_case(sim, "deep sea across the edge", edgeX, midY, (BYTE)(cx + LSO_FAR), cy, 0, 0);
    mapSetPos(gs, &gs->mp, (BYTE)(cx + 1), cy, GRASS, FALSE, FALSE);
    if (rc == 0) rc = lso_case(sim, "grass across the edge", edgeX, midY, (BYTE)(cx + LSO_FAR), cy, 64, 0);

    /* The same place and the same grass across the edge, but the tank's own
     * square is one a builder cannot walk on. He has no speed there and
     * turns back at once, so he is not lifted over the edge onto the grass:
     * he stays on the tank. */
    mapSetPos(gs, &gs->mp, cx, cy, RIVER, FALSE, FALSE);
    if (rc == 0) rc = lso_case(sim, "tank in a river, grass across the edge", edgeX, midY, (BYTE)(cx + LSO_FAR), cy, 0, 0);
    mapSetPos(gs, &gs->mp, cx, cy, DEEP_SEA, FALSE, FALSE);
    if (rc == 0) rc = lso_case(sim, "tank in deep sea, grass across the edge", edgeX, midY, (BYTE)(cx + LSO_FAR), cy, 0, 0);

    /* The same deep sea square with the tank on a boat. A builder near his
     * boat walks over any square, so he gets the full 64, onto the grass
     * and onto deep sea alike. */
    tankSetOnBoat(&gs->tanks[LSO_SLOT], TRUE);
    if (rc == 0) rc = lso_case(sim, "tank on a boat, grass across the edge", edgeX, midY, (BYTE)(cx + LSO_FAR), cy, 64, 0);
    mapSetPos(gs, &gs->mp, (BYTE)(cx + 1), cy, DEEP_SEA, FALSE, FALSE);
    if (rc == 0) rc = lso_case(sim, "tank on a boat, deep sea across the edge", edgeX, midY, (BYTE)(cx + LSO_FAR), cy, 64, 0);
    tankSetOnBoat(&gs->tanks[LSO_SLOT], FALSE);

    serverSimDestroy(sim);
    return rc;
}
