/*
 * The one branch of sight.c's shadow pile that terrain cannot reach: what
 * happens when the pile is full and one more separate shadow has to go on it.
 *
 * The pile holds SIGHT_SHADOW_MAX separate shadows, which ships at five
 * hundred and twelve. Touching shadows are folded into one as they go on, so
 * that counts the cracks of daylight left between the walls rather than the
 * walls themselves, and no map produces anything like that many: a sweep of
 * random 29x29 blocks from four to fifty per cent walls peaked at eleven, and
 * the most squares that can stand in a 29x29 block without any two of them
 * touching is about two hundred and twenty-five. So there is no map to paint
 * that would reach the branch, and building the case round the real cap is not
 * the way to cover it.
 *
 * What this file does instead is compile the module a second time with the cap
 * turned right down, so a handful of walls fills the pile and the branch is
 * taken. The two copies live side by side in the same binary - this one under
 * its own names, the one that ships under its own - and the case asks them the
 * same question about the same map and compares the two masks.
 *
 * What it pins is the direction. A full pile widens the shadow beside the one
 * it cannot hold out over the gap between them, so it hides a little more than
 * the geometry says. More, never less: a square the real module hides has to
 * still be hidden with the pile capped, and the case fails on any square that
 * goes the other way.
 */

/* The cap this copy is built with. Small enough that the four walls below
 * overrun it, and never anything the shipping build sets - see sight.h. */
#define SIGHT_SHADOW_MAX 2

/* The module's two exported names, taken over for this copy so it links beside
 * the one that ships rather than clashing with it. */
#define sightBuildMask           sightTinyPileBuildMask
#define sightBuildMaskCentreLine sightTinyPileBuildMaskCentreLine

#include "sight.c"

#undef sightBuildMask
#undef sightBuildMaskCentreLine

#include "test_harness.h"

/* The module as it really ships, out of the library beside this copy. sight.h
 * has already been read above under the taken-over names, so the one prototype
 * that is wanted back is written out here rather than included again. */
void sightBuildMask(map *mp, pillboxes *pb, BYTE originX, BYTE originY,
                    BYTE offsetX, BYTE offsetY, const OverviewRect *block,
                    BYTE *vis);

/* Where the eye stands, and the four walls it looks at. They sit in one row
 * two squares below it, far enough apart that no two of their shadows touch -
 * a wall further along the row is seen at a flatter angle, so squares merely
 * one apart still overlap and it takes a wider spacing than that to leave
 * daylight between them. Four separate shadows against a pile of two is two
 * more than it holds. */
#define OVERFLOW_EYE_X  100
#define OVERFLOW_EYE_Y  100
#define OVERFLOW_WALL_Y 102

static const int kOverflowWallX[] = {100, 97, 103, 92};
#define OVERFLOW_WALLS ((int)(sizeof(kOverflowWallX) / sizeof(kOverflowWallX[0])))

static struct mapObj overflowMapObj;
static map           overflowMap = &overflowMapObj;
static BYTE          visCapped[SIGHT_MASK_BYTES];
static BYTE          visReal[SIGHT_MASK_BYTES];

/* An inclusive block, zeroed whole the way the map's own rects are. */
static OverviewRect overflowBlock(int left, int top, int right, int bottom) {
    OverviewRect r; /* Block to return */

    memset(&r, 0, sizeof(r));
    r.alpha = 255;
    r.left = left;
    r.top = top;
    r.right = right;
    r.bottom = bottom;
    return r;
}

static bool overflowSeen(const BYTE *vis, const OverviewRect *block, int x,
                         int y) {
    int stride = block->right - block->left + 1;

    return vis[(y - block->top) * stride + (x - block->left)] == 1;
}

/* Grass with the row of walls painted on it. No pillbox list: this case is
 * about the pile, and terrain is the shortest way to fill it. */
static void overflowPaint(void) {
    int i; /* Looping variable */

    memset(overflowMapObj.mapItem, GRASS, sizeof(overflowMapObj.mapItem));
    for (i = 0; i < OVERFLOW_WALLS; i++) {
        overflowMapObj.mapItem[kOverflowWallX[i]][OVERFLOW_WALL_Y] = BUILDING;
    }
}

/* The case itself, from one spot inside the eye's square. */
static int overflowCompare(BYTE offsetX, BYTE offsetY) {
    OverviewRect block = overflowBlock(OVERFLOW_EYE_X - SIGHT_MAX_HALF,
                                       OVERFLOW_EYE_Y - SIGHT_MAX_HALF,
                                       OVERFLOW_EYE_X + SIGHT_MAX_HALF,
                                       OVERFLOW_EYE_Y + SIGHT_MAX_HALF);
    int hiddenOnly; /* Squares the capped pile hides and the real one shows */
    int i;          /* Looping variable */
    int x;          /* Looping variable */
    int y;          /* Looping variable */

    overflowPaint();
    memset(visCapped, 0xFF, sizeof(visCapped));
    memset(visReal, 0xFF, sizeof(visReal));
    sightTinyPileBuildMask(&overflowMap, NULL, OVERFLOW_EYE_X, OVERFLOW_EYE_Y,
                           offsetX, offsetY, &block, visCapped);
    sightBuildMask(&overflowMap, NULL, OVERFLOW_EYE_X, OVERFLOW_EYE_Y, offsetX,
                   offsetY, &block, visReal);

    /* Every wall is still drawn and the square the player is standing on is
     * still seen, whichever pile answered. A capped pile that had swallowed
     * those would be hiding more in a way that is nothing to do with the
     * branch, and the sweep below would call it a pass. */
    for (i = 0; i < OVERFLOW_WALLS; i++) {
        UT_ASSERT_MSG(overflowSeen(visCapped, &block, kOverflowWallX[i],
                                   OVERFLOW_WALL_Y),
                      "standing %u,%u inside the square, the capped pile hid "
                      "the wall at %d,%d, and a wall is drawn",
                      (unsigned)offsetX, (unsigned)offsetY, kOverflowWallX[i],
                      OVERFLOW_WALL_Y);
    }
    UT_ASSERT_MSG(overflowSeen(visCapped, &block, OVERFLOW_EYE_X,
                               OVERFLOW_EYE_Y),
                  "standing %u,%u inside the square, the capped pile hid the "
                  "square the player is standing on",
                  (unsigned)offsetX, (unsigned)offsetY);

    /* The direction the branch promises, square by square over the whole
     * block: a square the real module hides is hidden with the pile capped as
     * well. Anything the other way round is the fold letting sight through a
     * gap the geometry had closed, which is the one answer it must never
     * give. */
    hiddenOnly = 0;
    for (y = block.top; y <= block.bottom; y++) {
        for (x = block.left; x <= block.right; x++) {
            bool capped = overflowSeen(visCapped, &block, x, y);
            bool real = overflowSeen(visReal, &block, x, y);

            UT_ASSERT_MSG(real == TRUE || capped == FALSE,
                          "standing %u,%u inside the square, %d,%d is hidden "
                          "with the pile as it ships and seen with it capped: "
                          "a full pile has given back ground it should have "
                          "kept", (unsigned)offsetX, (unsigned)offsetY, x, y);
            if (capped == FALSE && real == TRUE) {
                hiddenOnly++;
            }
        }
    }

    /* And the branch was really taken. The two copies differ in nothing but
     * the cap, so a square the capped run hides and the real one shows can only
     * have come from the fold widening a shadow over a gap. Without one the
     * case would be passing on a pile that never filled up. */
    UT_ASSERT_MSG(hiddenOnly > 0,
                  "standing %u,%u inside the square, the capped pile and the "
                  "one that ships answered the same square for square, so the "
                  "full-pile branch was never reached",
                  (unsigned)offsetX, (unsigned)offsetY);
    return 0;
}

/* The overflow path, from the middle of the eye's square and from three
 * corners of it, because which shadow the odd one out is folded into turns on
 * where in the square the player is standing. */
static int sight_shadow_overflow_hides_at_least_as_much(void) {
    static const BYTE kOffsets[][2] = {
        {SIGHT_SUB_CENTRE, SIGHT_SUB_CENTRE}, {1, 1}, {255, 255}, {255, 1}
    };
    int i;  /* Looping variable */
    int rc; /* What the comparison from one spot came back with */

    for (i = 0; i < (int)(sizeof(kOffsets) / sizeof(kOffsets[0])); i++) {
        rc = overflowCompare(kOffsets[i][0], kOffsets[i][1]);
        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}

int run_sight_shadow_overflow(void) {
    return sight_shadow_overflow_hides_at_least_as_much();
}
