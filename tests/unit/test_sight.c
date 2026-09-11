/*
 * Tests for line of sight (sight.c) — which squares of a block can be seen
 * from one square of the map. Nothing here needs a sim: a bare map object is
 * painted square by square and the mask is read straight back.
 */

#include <string.h>

#include "global.h"
#include "types.h"
#include "sight.h"
#include "test_harness.h"

/* The mask plus a tail nothing may write into. Every byte the builder writes
 * is inside the block it was handed, so a block that failed to keep to its own
 * stride would run off the end of the buffer into this. */
#define SIGHT_GUARD_BYTES 32
#define SIGHT_GUARD_FILL  0x5A

static struct mapObj sightMapObj;
static map           sightMap = &sightMapObj;
static BYTE          visBuf[SIGHT_MASK_BYTES + SIGHT_GUARD_BYTES];

/* A map of one terrain, so every case starts from ground that stops nothing
 * and puts in only what it is about. */
static void sightMapFill(BYTE terrain) {
    memset(sightMapObj.mapItem, terrain, sizeof(sightMapObj.mapItem));
}

static void sightMapSet(int x, int y, BYTE terrain) {
    sightMapObj.mapItem[x][y] = terrain;
}

/* An inclusive block, zeroed whole the way the map's own rects are. */
static OverviewRect sightBlock(int left, int top, int right, int bottom) {
    OverviewRect r; /* Block to return */

    memset(&r, 0, sizeof(r));
    r.alpha = 255;
    r.left = left;
    r.top = top;
    r.right = right;
    r.bottom = bottom;
    return r;
}

/* Fills the buffer with the guard byte and builds over it, so anything the
 * builder did not write reads as neither seen nor hidden. */
static void sightRun(BYTE originX, BYTE originY, const OverviewRect *block) {
    memset(visBuf, SIGHT_GUARD_FILL, sizeof(visBuf));
    sightBuildMask(&sightMap, originX, originY, block, visBuf);
}

static bool sightSeen(const OverviewRect *block, int x, int y) {
    int stride = block->right - block->left + 1;

    return visBuf[(y - block->top) * stride + (x - block->left)] == 1;
}

static int sightGuardIntact(void) {
    int i; /* Looping variable */

    for (i = 0; i < SIGHT_GUARD_BYTES; i++) {
        BYTE got = visBuf[SIGHT_MASK_BYTES + i];

        UT_ASSERT_MSG(got == SIGHT_GUARD_FILL,
                      "guard byte %d was written: 0x%02X, expected 0x%02X", i,
                      (unsigned)got, (unsigned)SIGHT_GUARD_FILL);
    }
    return 0;
}

/* Every square of the block came out as a yes or a no, which is what says the
 * builder wrote the block at the block's own width rather than the buffer's. */
static int sightBlockFullyWritten(const OverviewRect *block) {
    int width = block->right - block->left + 1;
    int height = block->bottom - block->top + 1;
    int i; /* Looping variable */

    for (i = 0; i < width * height; i++) {
        UT_ASSERT_MSG(visBuf[i] == 0 || visBuf[i] == 1,
                      "byte %d of a %dx%d block reads 0x%02X, so it was never "
                      "written", i, width, height, (unsigned)visBuf[i]);
    }
    return sightGuardIntact();
}

/* The square the player is standing on is always seen — it is where they are —
 * and it never stops its own lines, so a tank that has somehow ended up inside
 * a building still sees out of it rather than going blind. */
static int sight_origin_is_always_seen(void) {
    OverviewRect block = sightBlock(96, 96, 104, 104);

    sightMapFill(GRASS);
    sightRun(100, 100, &block);
    UT_ASSERT_MSG(sightSeen(&block, 100, 100),
                  "the origin square is not seen on open ground");

    sightMapSet(100, 100, BUILDING);
    sightRun(100, 100, &block);
    UT_ASSERT_MSG(sightSeen(&block, 100, 100),
                  "an origin square that is itself a building is not seen");
    UT_ASSERT_MSG(sightSeen(&block, 104, 100),
                  "the ground away from an origin standing in a building is "
                  "hidden, so the origin blocked its own line");
    return sightGuardIntact();
}

/* A building across the line hides everything behind it, and the building
 * itself is seen: the player sees the wall, not what is on the other side. */
static int sight_wall_across_the_line_hides_what_is_behind(void) {
    OverviewRect block = sightBlock(86, 86, 114, 114);
    int x; /* Looping variable */

    sightMapFill(GRASS);
    sightMapSet(103, 100, BUILDING);
    sightRun(100, 100, &block);

    for (x = 101; x <= 103; x++) {
        UT_ASSERT_MSG(sightSeen(&block, x, 100),
                      "square %d,100 between the tank and the wall — the wall "
                      "itself at 103 — is hidden", x);
    }
    for (x = 104; x <= 114; x++) {
        UT_ASSERT_MSG(!sightSeen(&block, x, 100),
                      "square %d,100 behind the wall at 103,100 is seen", x);
    }
    return sightBlockFullyWritten(&block);
}

/* The same wall one square off the line hides nothing on it: the line is the
 * squares it actually passes through, not a corridor round them. */
static int sight_wall_beside_the_line_hides_nothing(void) {
    OverviewRect block = sightBlock(86, 86, 114, 114);
    int x; /* Looping variable */

    sightMapFill(GRASS);
    sightMapSet(103, 101, BUILDING);
    sightMapSet(103, 99, BUILDING);
    sightRun(100, 100, &block);

    for (x = 101; x <= 114; x++) {
        UT_ASSERT_MSG(sightSeen(&block, x, 100),
                      "square %d,100 is hidden by a wall that is beside the "
                      "line, not on it", x);
    }
    return sightGuardIntact();
}

/* Where two buildings touch along an edge the diagonal between them is closed.
 * One building on its own leaves it open, so the case says the pair is what
 * shuts it rather than either square. */
static int sight_corner_between_two_buildings_is_closed(void) {
    OverviewRect block = sightBlock(96, 96, 104, 104);

    sightMapFill(GRASS);
    sightMapSet(101, 100, BUILDING);
    sightRun(100, 100, &block);
    UT_ASSERT_MSG(sightSeen(&block, 101, 101),
                  "the diagonal neighbour is hidden with only one building "
                  "beside it");

    sightMapSet(100, 101, BUILDING);
    sightRun(100, 100, &block);
    UT_ASSERT_MSG(!sightSeen(&block, 101, 101),
                  "sight slipped through the corner where two buildings meet");
    UT_ASSERT_MSG(!sightSeen(&block, 102, 102),
                  "the ground past that corner is seen through it");

    /* Both squares of the pair are still seen themselves — it is the gap
     * between them that is shut, not the buildings that are hidden. */
    UT_ASSERT_MSG(sightSeen(&block, 101, 100) && sightSeen(&block, 100, 101),
                  "one of the two buildings making the corner is hidden");
    return sightGuardIntact();
}

/* A block can name squares off the map — it is a rect of ints and a tank near
 * the top-left corner has one — and those are never seen. */
static int sight_off_map_squares_are_unseen(void) {
    OverviewRect block = sightBlock(-3, -3, 5, 5);
    int x; /* Looping variable */
    int y; /* Looping variable */

    sightMapFill(GRASS);
    sightRun(2, 2, &block);

    for (y = -3; y <= 5; y++) {
        for (x = -3; x <= 5; x++) {
            bool onMap = (x >= 0 && y >= 0);

            UT_ASSERT_MSG(sightSeen(&block, x, y) == onMap,
                          "square %d,%d is %s, expected it %s", x, y,
                          sightSeen(&block, x, y) ? "seen" : "hidden",
                          onMap ? "seen" : "hidden");
        }
    }
    return sightBlockFullyWritten(&block);
}

/* One square of terrain on the line stops it only if it is a building or a
 * half building. Forest above all: a single tree is seen past - it takes a
 * stand of them to stop a line, which is the depth cases below - and the tree
 * hide already covers a tank standing in one. */
static int sight_one_square_blocks_only_if_it_is_a_building(void) {
    static const BYTE kTerrains[] = {
        BUILDING,   RIVER,       SWAMP,      CRATER,     ROAD,
        FOREST,     RUBBLE,      GRASS,      HALFBUILDING, BOAT,
        MINE_SWAMP, MINE_CRATER, MINE_ROAD,  MINE_FOREST, MINE_RUBBLE,
        MINE_GRASS, DEEP_SEA
    };
    OverviewRect block = sightBlock(86, 86, 114, 114);
    int i; /* Looping variable */

    for (i = 0; i < (int)(sizeof(kTerrains) / sizeof(kTerrains[0])); i++) {
        BYTE terrain = kTerrains[i];
        bool blocks = (terrain == BUILDING || terrain == HALFBUILDING);
        bool behind; /* Is the square past it seen */

        sightMapFill(GRASS);
        sightMapSet(103, 100, terrain);
        sightRun(100, 100, &block);
        behind = sightSeen(&block, 106, 100);

        UT_ASSERT_MSG(behind != blocks,
                      "with terrain %u on the line the square behind it is %s, "
                      "expected it %s", (unsigned)terrain,
                      behind ? "seen" : "hidden", blocks ? "hidden" : "seen");
        UT_ASSERT_MSG(sightSeen(&block, 103, 100),
                      "the square carrying terrain %u is itself hidden",
                      (unsigned)terrain);
    }
    return sightGuardIntact();
}

/* A block the origin is nowhere near, which is what the lens hands over once
 * the classic view has scrolled away from the tank. Every square of it is
 * behind the wall, and with the wall gone every square of it is seen — which
 * is also what pins the mask being written at the block's own width. */
static int sight_block_away_from_the_origin(void) {
    OverviewRect block = sightBlock(110, 95, 124, 109);
    int x;  /* Looping variable */
    int y;  /* Looping variable */
    int rc; /* Result of the whole-block check */

    sightMapFill(GRASS);
    for (y = 90; y <= 114; y++) {
        sightMapSet(105, y, BUILDING);
    }
    sightRun(100, 100, &block);
    for (y = 95; y <= 109; y++) {
        for (x = 110; x <= 124; x++) {
            UT_ASSERT_MSG(!sightSeen(&block, x, y),
                          "square %d,%d is seen through the wall at x=105", x,
                          y);
        }
    }
    rc = sightBlockFullyWritten(&block);
    if (rc) {
        return rc;
    }

    sightMapFill(GRASS);
    sightRun(100, 100, &block);
    for (y = 95; y <= 109; y++) {
        for (x = 110; x <= 124; x++) {
            UT_ASSERT_MSG(sightSeen(&block, x, y),
                          "square %d,%d of an open block is hidden", x, y);
        }
    }
    return sightBlockFullyWritten(&block);
}

/* A block bigger than the buffer holds is refused rather than written past the
 * end of it, and so is one with no squares in it. */
static int sight_oversize_block_is_refused(void) {
    OverviewRect blocks[3];
    int b; /* Looping variable */
    int i; /* Looping variable */

    blocks[0] = sightBlock(0, 0, SIGHT_MAX_SIDE, 0);  /* one square too wide */
    blocks[1] = sightBlock(0, 0, 0, SIGHT_MAX_SIDE);  /* one square too tall */
    blocks[2] = sightBlock(10, 10, 9, 9);             /* no squares at all */

    sightMapFill(GRASS);
    for (b = 0; b < 3; b++) {
        sightRun(0, 0, &blocks[b]);
        for (i = 0; i < SIGHT_MASK_BYTES + SIGHT_GUARD_BYTES; i++) {
            UT_ASSERT_MSG(visBuf[i] == SIGHT_GUARD_FILL,
                          "block %d should have been refused, and byte %d "
                          "reads 0x%02X", b, i, (unsigned)visBuf[i]);
        }
    }
    return 0;
}

/* Trees stop a line only once there are more than SIGHT_TREE_MAX_DEPTH of them
 * in a row, so a hedge one or two squares thick is seen straight through. */
static int sight_shallow_trees_are_seen_through(void) {
    OverviewRect block = sightBlock(86, 86, 114, 114);

    sightMapFill(GRASS);
    sightMapSet(103, 100, FOREST);
    sightRun(100, 100, &block);
    UT_ASSERT_MSG(sightSeen(&block, 106, 100),
                  "one tree on the line hid the ground behind it");

    sightMapSet(104, 100, FOREST);
    sightRun(100, 100, &block);
    UT_ASSERT_MSG(sightSeen(&block, 106, 100),
                  "two trees in a row hid the ground behind them");
    return sightGuardIntact();
}

/* Three in a row do stop it. The third tree is seen itself — the square at the
 * far end of a line is never tested, so the player sees the wood they cannot
 * see into — and the ground past it is not. */
static int sight_three_trees_in_a_row_block(void) {
    OverviewRect block = sightBlock(86, 86, 114, 114);
    int x; /* Looping variable */

    sightMapFill(GRASS);
    sightMapSet(103, 100, FOREST);
    sightMapSet(104, 100, FOREST);
    sightMapSet(105, 100, FOREST);
    sightRun(100, 100, &block);

    for (x = 101; x <= 105; x++) {
        UT_ASSERT_MSG(sightSeen(&block, x, 100),
                      "square %d,100 — up to and including the third tree at "
                      "105 — is hidden", x);
    }
    for (x = 106; x <= 114; x++) {
        UT_ASSERT_MSG(!sightSeen(&block, x, 100),
                      "square %d,100 behind three trees in a row is seen", x);
    }
    return sightBlockFullyWritten(&block);
}

/* A mined tree is still a tree. The square reads back as MINE_FOREST, so
 * without that in the count a mine laid in the middle of a wood would open a
 * hole in it - a change the player cannot see on the map, on ground that has
 * not moved. */
static int sight_a_mined_tree_still_counts_as_a_tree(void) {
    OverviewRect block = sightBlock(86, 86, 114, 114);

    sightMapFill(GRASS);
    sightMapSet(103, 100, FOREST);
    sightMapSet(104, 100, MINE_FOREST);
    sightMapSet(105, 100, FOREST);
    sightRun(100, 100, &block);
    UT_ASSERT_MSG(sightSeen(&block, 105, 100),
                  "the third tree of the run is hidden");
    UT_ASSERT_MSG(!sightSeen(&block, 106, 100),
                  "the ground behind three trees is seen because the mine on "
                  "the middle one took it out of the count");
    return sightGuardIntact();
}

/* The run is consecutive rather than cumulative. Two bands of two trees with a
 * square of grass between them are four trees on the line and stop nothing,
 * where a tally of every tree the line passed through would have shut it;
 * filling the gap makes one run of five and does stop it. */
static int sight_a_gap_between_bands_of_trees_resets_the_run(void) {
    OverviewRect block = sightBlock(86, 86, 114, 114);

    sightMapFill(GRASS);
    sightMapSet(101, 100, FOREST);
    sightMapSet(102, 100, FOREST);
    sightMapSet(104, 100, FOREST);
    sightMapSet(105, 100, FOREST);
    sightRun(100, 100, &block);
    UT_ASSERT_MSG(sightSeen(&block, 106, 100),
                  "two bands of two trees with a square of grass between them "
                  "hid the ground behind them, so the count is being carried "
                  "across the gap");

    sightMapSet(103, 100, FOREST);
    sightRun(100, 100, &block);
    UT_ASSERT_MSG(!sightSeen(&block, 106, 100),
                  "filling the gap makes one run of five trees and the ground "
                  "behind it is still seen");
    return sightGuardIntact();
}

/* Trees never close the diagonal corner two buildings do: the depth rule is
 * about how far into a wood a player sees, and a pair of trees is not a wall. */
static int sight_corner_between_two_trees_stays_open(void) {
    OverviewRect block = sightBlock(96, 96, 104, 104);

    sightMapFill(GRASS);
    sightMapSet(101, 100, FOREST);
    sightMapSet(100, 101, FOREST);
    sightRun(100, 100, &block);
    UT_ASSERT_MSG(sightSeen(&block, 101, 101),
                  "sight was stopped at the corner where two trees meet");
    UT_ASSERT_MSG(sightSeen(&block, 102, 102),
                  "the ground past that corner is hidden");
    return sightGuardIntact();
}

int run_sight(void) {
    int rc;

    rc = sight_origin_is_always_seen();                    if (rc) return rc;
    rc = sight_wall_across_the_line_hides_what_is_behind(); if (rc) return rc;
    rc = sight_wall_beside_the_line_hides_nothing();       if (rc) return rc;
    rc = sight_corner_between_two_buildings_is_closed();   if (rc) return rc;
    rc = sight_off_map_squares_are_unseen();               if (rc) return rc;
    rc = sight_one_square_blocks_only_if_it_is_a_building(); if (rc) return rc;
    rc = sight_block_away_from_the_origin();               if (rc) return rc;
    rc = sight_oversize_block_is_refused();                if (rc) return rc;
    rc = sight_shallow_trees_are_seen_through();           if (rc) return rc;
    rc = sight_three_trees_in_a_row_block();               if (rc) return rc;
    rc = sight_a_mined_tree_still_counts_as_a_tree();      if (rc) return rc;
    rc = sight_a_gap_between_bands_of_trees_resets_the_run(); if (rc) return rc;
    rc = sight_corner_between_two_trees_stays_open();      if (rc) return rc;
    return 0;
}
