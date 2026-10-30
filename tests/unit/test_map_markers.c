/*
 * The marker shapes (test_map_markers.c).
 *
 * map_markers.c draws a tank as a triangle when the map is too small for its
 * sprite. The log viewer repaints its map square by square and marks the
 * squares a tank covered, so it needs to know how far the triangle reaches:
 * the triangle is taller than its radius and the outline sits half outside
 * its edge, so it covers more than the sprite's square does, and a square it
 * spilled into that was not marked kept the triangle's edge after the tank
 * moved on.
 *
 * tank_reach: the reach covers the nose and the tail corners with the outline
 * on them, grows with the radius, is nothing for a radius that draws nothing,
 * and at the radius the viewer draws a tank with (0.45 of a 16 px square) it
 * passes the 8 px half-square the sprite stops at, which is the spill the
 * viewer has to mark.
 */

#include <math.h>

#include "test_harness.h"
#include "map_colours.h"
#include "map_markers.h"

int run_map_markers_tank_reach(void) {
    /* The triangle's proportions, as map_markers.c has them. */
    const float kNose = 1.2f, kHalf = 0.85f, kTail = 1.0f;
    const float radii[] = { 3.0f, 7.2f, 14.4f };
    float       last = 0.0f;

    UT_ASSERT_MSG(mapMarkerTankReach(0.0f) == 0.0f &&
                  mapMarkerTankReach(-1.0f) == 0.0f,
                  "a radius that draws nothing has a reach");

    for (int i = 0; i < (int)(sizeof(radii) / sizeof(radii[0])); i++) {
        float r     = radii[i];
        float reach = mapMarkerTankReach(r);
        float nose  = r * kNose + mapColourMarkerLayerGrow(0);
        float tail  = sqrtf((r * kHalf) * (r * kHalf) + (r * kTail) * (r * kTail));

        /* The outline's nose is at least the fill's nose plus the stroke
         * outside it, and the tail corners are further from the centre than
         * the fill's are. */
        UT_ASSERT_MSG(reach >= nose - 1.0e-4f,
                      "radius %.1f: reach %.2f is short of the outlined nose %.2f",
                      (double)r, (double)reach, (double)nose);
        UT_ASSERT_MSG(reach >= tail,
                      "radius %.1f: reach %.2f is short of the tail corners %.2f",
                      (double)r, (double)reach, (double)tail);
        /* And it is a bound on the shape, not a guess far past it: no corner
         * is further than the radius grown by the whole stroke on the
         * triangle's longest proportion. */
        UT_ASSERT_MSG(reach <= (r + MAP_COLOUR_STROKE_PX) * (kNose + kTail),
                      "radius %.1f: reach %.2f is far past the triangle",
                      (double)r, (double)reach);
        UT_ASSERT_MSG(reach > last,
                      "radius %.1f: reach %.2f did not grow past %.2f",
                      (double)r, (double)reach, (double)last);
        last = reach;
    }

    /* The viewer's own case: a 16 px square, so a radius of 7.2 and a sprite
     * that stops 8 px from the centre. The triangle passes that, so the
     * viewer must mark the squares past the sprite's own, and it stays under
     * 16 px, so one square each way is enough. */
    {
        float reach = mapMarkerTankReach(16.0f * 0.45f);
        UT_ASSERT_MSG(reach > 8.0f,
                      "a tank marker in a 16 px square reaches %.2f px, inside "
                      "the sprite's square, so nothing past it needs marking",
                      (double)reach);
        UT_ASSERT_MSG(reach < 16.0f,
                      "a tank marker in a 16 px square reaches %.2f px, more "
                      "than one square past its centre",
                      (double)reach);
    }
    return 0;
}
