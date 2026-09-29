/*
 * Tests for the shadow rule in sight.c — a square is seen when any part of it
 * can be seen from the eye, not only when its middle can.
 *
 * Nothing here needs a sim: a bare map object is painted square by square and
 * the mask is read straight back. The rule as it was, one line to the middle of
 * every square, is still reachable as sightBuildMaskCentreLine, so the cases
 * that turn on the change say what each rule answers and the difference is on
 * the record rather than described.
 *
 * Everard Island comes in as its compressed bytes for the two cases that want
 * real ground under them: one to show the two rules still agree square for
 * square where there are no walls to disagree about, and one to time the pass
 * over the block the main view really asks for.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "global.h"
#include "types.h"
#include "bolo_map.h"
#include "client_mappreview.h"
#include "everard_map.h"
#include "pillbox.h"
#include "sight.h"
#include "test_harness.h"

/* E_MAP's compressed length, the same literal the loopback harness passes. */
#define SHADOW_EMAP_LEN E_MAP_LEN

/* How many times the cost case builds one block, and the most a build may cost
 * before the case calls it a fault rather than a slow machine. The main view
 * asks for one of these a frame and has a whole frame to play with, so the bar
 * is set where a build that had gone properly wrong would trip it and a build
 * on a busy machine would not. */
#define SHADOW_COST_RUNS   500
#define SHADOW_COST_MAX_MS 5.0

static struct mapObj shadowMapObj;
static map           shadowMap = &shadowMapObj;
static BYTE          visNew[SIGHT_MASK_BYTES];
static BYTE          visOld[SIGHT_MASK_BYTES];

/* The pill list the cases that are about pillboxes build. It stays NULL for
 * every other case, which is a caller that has no pills, so no pill blocks. */
static pillboxes shadowPills = NULL;

static pillboxes *shadowPillList(void) {
    return (shadowPills == NULL) ? NULL : &shadowPills;
}

/* A pill list with nothing on it. Called again, it throws the last one away
 * first, so a case never inherits another case's pills. */
static void shadowPillsBegin(void) {
    if (shadowPills != NULL) {
        pillsDestroy(&shadowPills);
        shadowPills = NULL;
    }
    pillsCreate(&shadowPills);
}

static void shadowPillsEnd(void) {
    if (shadowPills != NULL) {
        pillsDestroy(&shadowPills);
        shadowPills = NULL;
    }
}

/* One pillbox on the list. armour 0 is a dead one; inTank says it is being
 * carried, which is the state that means it is on no square at all. */
static bool shadowPillAdd(int x, int y, BYTE armour, bool inTank) {
    pillbox item; /* The pill to add */
    BYTE num;     /* Which pill number it came back as */

    memset(&item, 0, sizeof(item));
    item.x = (BYTE)x;
    item.y = (BYTE)y;
    item.owner = NEUTRAL;
    item.armour = armour;
    item.speed = PILLBOX_ATTACK_NORMAL;
    item.reload = PILLBOX_ATTACK_NORMAL;
    item.inTank = inTank;
    return pillsAddItem(&shadowPills, &item, &num) == TRUE;
}

static void shadowMapFill(BYTE terrain) {
    memset(shadowMapObj.mapItem, terrain, sizeof(shadowMapObj.mapItem));
}

static void shadowMapSet(int x, int y, BYTE terrain) {
    shadowMapObj.mapItem[x][y] = terrain;
}

/* An inclusive block, zeroed whole the way the map's own rects are. */
static OverviewRect shadowBlock(int left, int top, int right, int bottom) {
    OverviewRect r; /* Block to return */

    memset(&r, 0, sizeof(r));
    r.alpha = 255;
    r.left = left;
    r.top = top;
    r.right = right;
    r.bottom = bottom;
    return r;
}

/* The block the main view asks for: the widest one there is, with the eye's
 * square in the middle of it. */
static OverviewRect shadowBlockAround(int x, int y) {
    return shadowBlock(x - SIGHT_MAX_HALF, y - SIGHT_MAX_HALF,
                       x + SIGHT_MAX_HALF, y + SIGHT_MAX_HALF);
}

static bool shadowSeen(const BYTE *vis, const OverviewRect *block, int x,
                       int y) {
    int stride = block->right - block->left + 1;

    return vis[(y - block->top) * stride + (x - block->left)] == 1;
}

/* The two rules over the same block. The new one is asked from the middle of
 * the square unless a case says otherwise, which is where the old one always
 * looked from, so anything the two disagree about is the rule and not the spot
 * the player is standing on. */
static void shadowRun(BYTE originX, BYTE originY, const OverviewRect *block) {
    memset(visNew, 0xFF, sizeof(visNew));
    memset(visOld, 0xFF, sizeof(visOld));
    sightBuildMask(&shadowMap, shadowPillList(), originX, originY,
                   SIGHT_SUB_CENTRE, SIGHT_SUB_CENTRE, block, visNew);
    sightBuildMaskCentreLine(&shadowMap, shadowPillList(), originX, originY,
                             block, visOld);
}

static void shadowRunAt(BYTE originX, BYTE originY, BYTE offsetX, BYTE offsetY,
                        const OverviewRect *block) {
    memset(visNew, 0xFF, sizeof(visNew));
    sightBuildMask(&shadowMap, shadowPillList(), originX, originY, offsetX,
                   offsetY, block, visNew);
}

/* Everard Island into the module's own map object, walls and all. withPills
 * takes the map's own pillboxes with it, which is what the main view really
 * looks at; without them the pill list stays NULL and the case is about the
 * terrain on its own. */
static bool shadowLoadEverard(bool withPills) {
    BYTE emap[6000] = E_MAP;
    MapPreview *preview; /* The decompressed map, which is all that is wanted */

    shadowPillsEnd();
    preview = clientMapPreviewLoadFromBuffer(emap, SHADOW_EMAP_LEN);
    if (preview == NULL) {
        return false;
    }
    memcpy(&shadowMapObj, clientMapPreviewMap(preview), sizeof(shadowMapObj));
    if (withPills) {
        pillsCreate(&shadowPills);
        memcpy(shadowPills, clientMapPreviewPills(preview),
               sizeof(struct pillsObj));
    }
    clientMapPreviewDestroy(preview);
    return true;
}

/* Everard Island with every wall knocked down, which leaves the trees, the
 * water and the rest of the ground the map really has. */
static void shadowFlattenWalls(void) {
    int x; /* Looping variable */
    int y; /* Looping variable */

    for (y = 0; y < MAP_ARRAY_SIZE; y++) {
        for (x = 0; x < MAP_ARRAY_SIZE; x++) {
            if (SIGHT_OPAQUE(shadowMapObj.mapItem[x][y])) {
                shadowMapObj.mapItem[x][y] = GRASS;
            }
        }
    }
}

/* (a) With no wall anywhere on the map the two rules answer the same square for
 * square, because the only thing left to stop a line is the trees and both
 * count those the same way. Real ground rather than a painted case: Everard
 * Island's woods, water and shore, from five places on it, over the widest
 * block there is. */
static int sight_shadow_matches_the_old_rule_without_walls(void) {
    static const BYTE kOrigins[][2] = {
        {100, 100}, {120, 90}, {60, 140}, {200, 200}, {30, 30}
    };
    int i; /* Looping variable */
    int x; /* Looping variable */
    int y; /* Looping variable */

    UT_ASSERT_MSG(shadowLoadEverard(false),
                  "Everard Island did not decompress");
    shadowFlattenWalls();

    for (i = 0; i < (int)(sizeof(kOrigins) / sizeof(kOrigins[0])); i++) {
        OverviewRect block = shadowBlockAround(kOrigins[i][0], kOrigins[i][1]);

        shadowRun(kOrigins[i][0], kOrigins[i][1], &block);
        for (y = block.top; y <= block.bottom; y++) {
            for (x = block.left; x <= block.right; x++) {
                bool now = shadowSeen(visNew, &block, x, y);
                bool before = shadowSeen(visOld, &block, x, y);

                UT_ASSERT_MSG(now == before,
                              "from %u,%u square %d,%d is %s now and was %s "
                              "before, on ground with no wall on it",
                              (unsigned)kOrigins[i][0], (unsigned)kOrigins[i][1],
                              x, y, now ? "seen" : "hidden",
                              before ? "seen" : "hidden");
            }
        }
    }
    return 0;
}

/* (b) The case the rule was changed for. A pillbox with a wall run up beside
 * it: the wall at 103,101 is not between the player and the pillbox at 106,101,
 * but the line to the middle of the pillbox clips it, so the old rule called the
 * pillbox hidden and the player attacking it could not see what they were
 * shooting at. A corner of it is in plain view - the wall's shadow starts at the
 * ray through its far corner and the pillbox reaches past that ray - so the
 * shadow rule sees it.
 *
 * The square further round behind the same wall is hidden by both, which is
 * what says the wall is still a wall and the case is not simply seeing
 * everything. */
static int sight_shadow_sees_a_pill_past_a_blocker(void) {
    OverviewRect block = shadowBlock(96, 96, 110, 110);

    shadowMapFill(GRASS);
    shadowMapSet(103, 101, BUILDING);
    shadowRun(100, 100, &block);

    UT_ASSERT_MSG(shadowSeen(visNew, &block, 106, 101),
                  "the pillbox square past the blocker is hidden: no part of "
                  "it is being counted, only its middle");
    UT_ASSERT_MSG(!shadowSeen(visOld, &block, 106, 101),
                  "the old centre-line rule already saw the pillbox square, so "
                  "this case pins nothing");
    UT_ASSERT_MSG(shadowSeen(visNew, &block, 103, 101),
                  "the blocker itself is hidden, and a wall is drawn");
    UT_ASSERT_MSG(!shadowSeen(visNew, &block, 106, 102),
                  "the square squarely behind the blocker is seen, so the wall "
                  "has stopped casting a shadow at all");
    return 0;
}

/* (c) The sliver. One wall leaves about a quarter of a square of 105,101
 * showing - the strip between the wall's shadow edge and the top of the square,
 * which runs from about a seventh of a square at the near side to about three
 * tenths at the far one - and a quarter of a square is a square that is seen.
 * The old rule called it hidden.
 *
 * Then the strip is closed with one more wall at 104,100. That wall touches the
 * first only at the corner 104,101, and the two shadows have to come out as one
 * shadow with no crack in it or the sliver would still be there: both shadows
 * end on the ray through that very corner. */
static int sight_shadow_sees_a_sliver_and_loses_it(void) {
    OverviewRect block = shadowBlock(96, 96, 110, 110);

    shadowMapFill(GRASS);
    shadowMapSet(103, 101, BUILDING);
    shadowRun(100, 100, &block);
    UT_ASSERT_MSG(shadowSeen(visNew, &block, 105, 101),
                  "the strip of 105,101 left showing past the wall is not "
                  "enough to call the square seen");
    UT_ASSERT_MSG(!shadowSeen(visOld, &block, 105, 101),
                  "the old centre-line rule already saw 105,101, so this case "
                  "pins nothing");

    shadowMapSet(104, 100, BUILDING);
    shadowRun(100, 100, &block);
    UT_ASSERT_MSG(!shadowSeen(visNew, &block, 105, 101),
                  "105,101 is still seen with the strip closed: the two walls "
                  "meeting at 104,101 left a crack between their shadows");
    UT_ASSERT_MSG(shadowSeen(visNew, &block, 104, 100) &&
                      shadowSeen(visNew, &block, 103, 101),
                  "one of the two walls is itself hidden");
    return 0;
}

/* (d) Two walls that touch at a corner close the diagonal between them, from
 * wherever in the square the player is standing - the shadows meet on the ray
 * through the corner they share, and that ray is an end of both of them
 * whatever the eye's offset is. A square squarely behind a one-wide wall is
 * hidden from every offset too, and the wall itself is seen from all of them. */
static int sight_shadow_closes_a_corner(void) {
    static const BYTE kOffsets[][2] = {
        {SIGHT_SUB_CENTRE, SIGHT_SUB_CENTRE}, {1, 1}, {255, 255},
        {255, 1}, {1, 255}, {200, 40}, {40, 200}
    };
    OverviewRect block = shadowBlock(96, 96, 110, 110);
    int i; /* Looping variable */

    for (i = 0; i < (int)(sizeof(kOffsets) / sizeof(kOffsets[0])); i++) {
        BYTE sx = kOffsets[i][0];
        BYTE sy = kOffsets[i][1];

        shadowMapFill(GRASS);
        shadowMapSet(101, 100, BUILDING);
        shadowMapSet(100, 101, BUILDING);
        shadowRunAt(100, 100, sx, sy, &block);
        UT_ASSERT_MSG(!shadowSeen(visNew, &block, 101, 101),
                      "standing %u,%u inside the square, sight slipped through "
                      "the corner where two buildings meet",
                      (unsigned)sx, (unsigned)sy);
        UT_ASSERT_MSG(!shadowSeen(visNew, &block, 102, 102),
                      "standing %u,%u inside the square, the ground past that "
                      "corner is seen through it", (unsigned)sx, (unsigned)sy);
        UT_ASSERT_MSG(shadowSeen(visNew, &block, 101, 100) &&
                          shadowSeen(visNew, &block, 100, 101),
                      "standing %u,%u inside the square, one of the two "
                      "buildings making the corner is hidden",
                      (unsigned)sx, (unsigned)sy);

        /* One wall across the row, and the ground straight behind it. The wall
         * is nearer and fills more of the view than anything behind it can, so
         * there is nothing of that square to see from anywhere in the square
         * the player is standing in. */
        shadowMapFill(GRASS);
        shadowMapSet(103, 100, BUILDING);
        shadowRunAt(100, 100, sx, sy, &block);
        UT_ASSERT_MSG(shadowSeen(visNew, &block, 103, 100),
                      "standing %u,%u inside the square, the wall itself is "
                      "hidden", (unsigned)sx, (unsigned)sy);
        UT_ASSERT_MSG(!shadowSeen(visNew, &block, 105, 100),
                      "standing %u,%u inside the square, the square squarely "
                      "behind the wall is seen", (unsigned)sx, (unsigned)sy);
        UT_ASSERT_MSG(!shadowSeen(visNew, &block, 110, 100),
                      "standing %u,%u inside the square, the far end of the row "
                      "behind the wall is seen", (unsigned)sx, (unsigned)sy);
    }
    return 0;
}

/* (e) The trees answer exactly what they answered before: one row is seen
 * through, a wood two deep is stopped at its near edge, and inside
 * SIGHT_TREE_NEAR of the player the run hides nothing. Both rules are asked
 * each time and have to agree - the shadow pass hands the trees to the same
 * walk the old rule used, and this is what says so. */
static int sight_shadow_trees_are_unchanged(void) {
    OverviewRect block = shadowBlock(96, 96, 114, 114);
    int x; /* Looping variable */

    shadowMapFill(GRASS);
    shadowMapSet(105, 100, FOREST);
    shadowRun(100, 100, &block);
    UT_ASSERT_MSG(shadowSeen(visNew, &block, 108, 100),
                  "one row of trees hid the ground behind it");

    shadowMapSet(106, 100, FOREST);
    shadowMapSet(107, 100, FOREST);
    shadowRun(100, 100, &block);
    UT_ASSERT_MSG(shadowSeen(visNew, &block, 105, 100),
                  "the near edge of the wood is hidden");
    UT_ASSERT_MSG(!shadowSeen(visNew, &block, 106, 100),
                  "the wood is seen into past its near edge");

    shadowMapFill(GRASS);
    for (x = 101; x <= 108; x++) {
        shadowMapSet(x, 100, FOREST);
    }
    shadowRun(100, 100, &block);
    UT_ASSERT_MSG(shadowSeen(visNew, &block, 102, 100),
                  "the trees within %d squares are not seen into",
                  SIGHT_TREE_NEAR);
    UT_ASSERT_MSG(!shadowSeen(visNew, &block, 103, 100),
                  "the wood is seen into %d squares out, past the range the "
                  "tank hide gives up at", SIGHT_TREE_NEAR + 1);

    /* Nothing on this map is a wall, so the whole block has to match the old
     * rule square for square as well. */
    for (x = block.left; x <= block.right; x++) {
        int y; /* Looping variable */

        for (y = block.top; y <= block.bottom; y++) {
            bool now = shadowSeen(visNew, &block, x, y);
            bool before = shadowSeen(visOld, &block, x, y);

            UT_ASSERT_MSG(now == before,
                          "square %d,%d is %s under the shadow rule and %s "
                          "under the old one, with only trees on the map", x, y,
                          now ? "seen" : "hidden", before ? "seen" : "hidden");
        }
    }
    return 0;
}

/* (f) The two squares the rule answers without looking at any geometry: one off
 * the map, which is never seen, and the one the player is standing on, which
 * always is - from anywhere inside it, and even when it is itself a building,
 * which must not blind the player standing in it. */
static int sight_shadow_edges_and_origin(void) {
    static const BYTE kOffsets[][2] = {
        {SIGHT_SUB_CENTRE, SIGHT_SUB_CENTRE}, {1, 1}, {255, 255}, {255, 1}
    };
    OverviewRect edge = shadowBlock(-3, -3, 5, 5);
    OverviewRect block = shadowBlock(96, 96, 104, 104);
    int i; /* Looping variable */
    int x; /* Looping variable */
    int y; /* Looping variable */

    for (i = 0; i < (int)(sizeof(kOffsets) / sizeof(kOffsets[0])); i++) {
        BYTE sx = kOffsets[i][0];
        BYTE sy = kOffsets[i][1];

        shadowMapFill(GRASS);
        shadowRunAt(2, 2, sx, sy, &edge);
        for (y = -3; y <= 5; y++) {
            for (x = -3; x <= 5; x++) {
                bool onMap = (x >= 0 && y >= 0);

                UT_ASSERT_MSG(shadowSeen(visNew, &edge, x, y) == onMap,
                              "standing %u,%u inside the square, %d,%d is %s, "
                              "expected it %s", (unsigned)sx, (unsigned)sy, x, y,
                              shadowSeen(visNew, &edge, x, y) ? "seen"
                                                              : "hidden",
                              onMap ? "seen" : "hidden");
            }
        }

        shadowMapFill(GRASS);
        shadowMapSet(100, 100, BUILDING);
        shadowRunAt(100, 100, sx, sy, &block);
        UT_ASSERT_MSG(shadowSeen(visNew, &block, 100, 100),
                      "standing %u,%u inside a square that is itself a "
                      "building, the square is hidden", (unsigned)sx,
                      (unsigned)sy);
        UT_ASSERT_MSG(shadowSeen(visNew, &block, 104, 100),
                      "standing %u,%u inside a square that is itself a "
                      "building, the ground away from it is hidden: the square "
                      "cast a shadow over its own view", (unsigned)sx,
                      (unsigned)sy);
    }
    return 0;
}

/* (g) A pillbox is a wall for this. One standing a square in front of the
 * player hides the ground behind it, its own square is seen the way a wall
 * square is, and the ground out to the side of its shadow is untouched. */
static int sight_shadow_a_pill_blocks_like_a_wall(void) {
    OverviewRect block = shadowBlock(96, 96, 110, 110);

    shadowMapFill(GRASS);
    shadowPillsBegin();
    UT_ASSERT_MSG(shadowPillAdd(101, 100, 15, false),
                  "the pillbox would not go on the list");
    shadowRun(100, 100, &block);

    UT_ASSERT_MSG(shadowSeen(visNew, &block, 101, 100),
                  "the pillbox's own square is hidden, and a pillbox is drawn");
    UT_ASSERT_MSG(!shadowSeen(visNew, &block, 102, 100),
                  "the square right behind the pillbox is seen");
    UT_ASSERT_MSG(!shadowSeen(visNew, &block, 105, 100),
                  "the far ground behind the pillbox is seen");
    UT_ASSERT_MSG(shadowSeen(visNew, &block, 101, 103),
                  "ground out to the side of the pillbox's shadow is hidden");
    UT_ASSERT_MSG(!shadowSeen(visOld, &block, 105, 100),
                  "the old rule let the line through the pillbox, so the two "
                  "rules do not agree about what a pillbox is");
    shadowPillsEnd();
    return 0;
}

/* (h) A dead pillbox blocks nothing. It is drawn on its square, which is the
 * argument for letting it block, but a tank drives straight over a dead pill to
 * pick it up where a live one is impassable, so sight follows the movement
 * rather than the drawing. The live pill in (g) stands on the very same square
 * and hides the very same ground, so the two cases together are what say the
 * armour is the whole of the difference. */
static int sight_shadow_a_dead_pill_blocks_nothing(void) {
    OverviewRect block = shadowBlock(96, 96, 110, 110);

    shadowMapFill(GRASS);
    shadowPillsBegin();
    UT_ASSERT_MSG(shadowPillAdd(101, 100, 0, false),
                  "the dead pillbox would not go on the list");
    shadowRun(100, 100, &block);

    UT_ASSERT_MSG(shadowSeen(visNew, &block, 101, 100),
                  "the dead pillbox's own square is hidden");
    UT_ASSERT_MSG(shadowSeen(visNew, &block, 105, 100),
                  "a dead pillbox hid the ground behind it, and a tank can "
                  "drive straight over one to pick it up");
    shadowPillsEnd();
    return 0;
}

/* (i) A pillbox being carried in a tank is on no square at all, so it blocks
 * nothing - the ground it is remembered at is in plain view. */
static int sight_shadow_a_carried_pill_blocks_nothing(void) {
    OverviewRect block = shadowBlock(96, 96, 110, 110);

    shadowMapFill(GRASS);
    shadowPillsBegin();
    UT_ASSERT_MSG(shadowPillAdd(101, 100, 15, true),
                  "the carried pillbox would not go on the list");
    shadowRun(100, 100, &block);

    UT_ASSERT_MSG(shadowSeen(visNew, &block, 101, 100),
                  "the square a carried pillbox came from is hidden");
    UT_ASSERT_MSG(shadowSeen(visNew, &block, 105, 100),
                  "a pillbox that is in a tank hid the ground behind the "
                  "square it is remembered at");
    shadowPillsEnd();
    return 0;
}

/* (j) The case the whole change is for, with a real pillbox as the target: the
 * wall beside it must not take it away, and neither must its own shadow. A
 * square is never hidden by what is standing on it - the pillbox is the thing
 * the player is looking at. */
static int sight_shadow_a_pill_never_hides_itself(void) {
    OverviewRect block = shadowBlock(96, 96, 120, 110);

    shadowMapFill(GRASS);
    shadowMapSet(103, 101, BUILDING);
    shadowPillsBegin();
    UT_ASSERT_MSG(shadowPillAdd(106, 101, 15, false),
                  "the pillbox would not go on the list");
    shadowRun(100, 100, &block);

    UT_ASSERT_MSG(shadowSeen(visNew, &block, 106, 101),
                  "the pillbox past the blocker is hidden, so either the wall "
                  "took it or it shadowed itself");

    /* The same pillbox with the wall taken away, so what is left is the
     * pillbox's own shadow: it is still seen itself, and the ground squarely
     * behind it is not. 112,102 is where the line through the pillbox has got
     * to, far enough out that the pillbox fills the view of it. */
    shadowMapFill(GRASS);
    shadowRun(100, 100, &block);
    UT_ASSERT_MSG(shadowSeen(visNew, &block, 106, 101),
                  "the pillbox is hidden by its own shadow");
    UT_ASSERT_MSG(!shadowSeen(visNew, &block, 112, 102),
                  "the ground squarely behind the pillbox is seen, so the "
                  "pillbox is casting no shadow of its own");
    shadowPillsEnd();
    return 0;
}

/* What the pass costs. The main view builds one of these a frame, over the
 * whole back buffer, from a tank standing on real ground with real walls round
 * it, so that is what is timed. The number is printed whether the case passes
 * or not - it is the point of the case - and the bar it has to stay under is
 * loose enough that only a pass that had gone properly wrong would trip it. */
static int sight_shadow_cost(void) {
    static const BYTE kOrigins[][2] = {
        {100, 100}, {120, 90}, {60, 140}, {200, 200}
    };
    int origins = (int)(sizeof(kOrigins) / sizeof(kOrigins[0]));
    clock_t started; /* When the loop began */
    double ms;       /* What one build cost, on average */
    int i;           /* Looping variable */
    int run;         /* Looping variable */

    UT_ASSERT_MSG(shadowLoadEverard(true),
                  "Everard Island did not decompress");

    started = clock();
    for (run = 0; run < SHADOW_COST_RUNS; run++) {
        for (i = 0; i < origins; i++) {
            OverviewRect block =
                shadowBlockAround(kOrigins[i][0], kOrigins[i][1]);

            sightBuildMask(&shadowMap, shadowPillList(), kOrigins[i][0],
                           kOrigins[i][1], SIGHT_SUB_CENTRE, SIGHT_SUB_CENTRE,
                           &block, visNew);
        }
    }
    ms = ((double)(clock() - started) * 1000.0 / (double)CLOCKS_PER_SEC) /
         (double)(SHADOW_COST_RUNS * origins);
    fprintf(stderr, "  sight: %dx%d block, %d builds, %.4f ms each\n",
            SIGHT_MAX_SIDE, SIGHT_MAX_SIDE, SHADOW_COST_RUNS * origins, ms);
    UT_ASSERT_MSG(ms < SHADOW_COST_MAX_MS,
                  "one %dx%d build cost %.4f ms, over the %.1f ms bar",
                  SIGHT_MAX_SIDE, SIGHT_MAX_SIDE, ms, SHADOW_COST_MAX_MS);
    shadowPillsEnd();
    return 0;
}

int run_sight_shadow(void) {
    int rc;

    rc = sight_shadow_matches_the_old_rule_without_walls(); if (rc) return rc;
    rc = sight_shadow_sees_a_pill_past_a_blocker();         if (rc) return rc;
    rc = sight_shadow_sees_a_sliver_and_loses_it();         if (rc) return rc;
    rc = sight_shadow_closes_a_corner();                    if (rc) return rc;
    rc = sight_shadow_trees_are_unchanged();                if (rc) return rc;
    rc = sight_shadow_edges_and_origin();                   if (rc) return rc;
    rc = sight_shadow_a_pill_blocks_like_a_wall();          if (rc) return rc;
    rc = sight_shadow_a_dead_pill_blocks_nothing();         if (rc) return rc;
    rc = sight_shadow_a_carried_pill_blocks_nothing();      if (rc) return rc;
    rc = sight_shadow_a_pill_never_hides_itself();          if (rc) return rc;
    return 0;
}

int run_sight_shadow_cost(void) {
    return sight_shadow_cost();
}
