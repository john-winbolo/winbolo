/*
 * Tests for the fog of war look the player picks (fog_look.h), and for the
 * road outline the Darker + roads look draws (fog_roads.h).
 *
 * Two pure pieces, no SDL and no renderer:
 *
 *   - the look table: which colour each of the four styles washes towards,
 *     which of them washes at all, which draws road edges, and that the
 *     setting store clamps a value from outside the enum back to the
 *     default the game had before the setting existed;
 *   - the edge mask: which sides of a square get a band, given the tiles
 *     round it.
 */

#include <string.h>

#include "test_harness.h"

#include "global.h" /* GRASS — a terrain number doubling as a tile number */
#include "fog_look.h"
#include "fog_roads.h"
#include "gfx_settings.h"

/* ------------------------------------------------------------------ */
/* The look table                                                     */
/* ------------------------------------------------------------------ */

int run_fog_style_looks(void) {
    unsigned char r = 1, g = 2, b = 3;

    /* Grey is 0, so a prefs file that has never carried the key reads as the
     * look the game already drew. */
    UT_ASSERT_MSG((int)FOG_STYLE_GREY == 0, "Grey must be style 0, is %d",
                  (int)FOG_STYLE_GREY);

    UT_ASSERT_MSG(fogLookColour(FOG_STYLE_GREY, &r, &g, &b) != 0,
                  "Grey has to wash");
    UT_ASSERT_MSG(r == FOG_LOOK_R && g == FOG_LOOK_G && b == FOG_LOOK_B,
                  "Grey washes towards %d,%d,%d, expected %d,%d,%d",
                  (int)r, (int)g, (int)b, FOG_LOOK_R, FOG_LOOK_G, FOG_LOOK_B);

    /* Darker is the dimming the grey replaced: a wash to black. */
    r = g = b = 200;
    UT_ASSERT_MSG(fogLookColour(FOG_STYLE_DARK, &r, &g, &b) != 0,
                  "Darker has to wash");
    UT_ASSERT_MSG(r == 0 && g == 0 && b == 0,
                  "Darker washes towards %d,%d,%d, expected black",
                  (int)r, (int)g, (int)b);

    /* Darker + roads is the same wash. Only the outline is added. */
    r = g = b = 200;
    UT_ASSERT_MSG(fogLookColour(FOG_STYLE_DARK_ROADS, &r, &g, &b) != 0,
                  "Darker + roads has to wash");
    UT_ASSERT_MSG(r == 0 && g == 0 && b == 0,
                  "Darker + roads washes towards %d,%d,%d, expected black",
                  (int)r, (int)g, (int)b);

    /* None draws nothing, and leaves the caller's bytes where they were. */
    r = 11; g = 22; b = 33;
    UT_ASSERT_MSG(fogLookColour(FOG_STYLE_NONE, &r, &g, &b) == 0,
                  "None must not wash");
    UT_ASSERT_MSG(r == 11 && g == 22 && b == 33,
                  "None wrote a colour: %d,%d,%d", (int)r, (int)g, (int)b);

    /* Only the one look draws the outline. */
    UT_ASSERT_MSG(fogLookDrawsRoadEdges(FOG_STYLE_DARK_ROADS) != 0,
                  "Darker + roads has to draw the outline");
    UT_ASSERT_MSG(fogLookDrawsRoadEdges(FOG_STYLE_GREY) == 0 &&
                      fogLookDrawsRoadEdges(FOG_STYLE_DARK) == 0 &&
                      fogLookDrawsRoadEdges(FOG_STYLE_NONE) == 0,
                  "Only Darker + roads draws the outline");

    /* The fade runs down to nothing and never back up. */
    {
        int band;
        int prev = 256;
        for (band = 0; band < FOG_ROAD_BANDS; band++) {
            int a = (int)fogRoadBandAlpha(band);
            UT_ASSERT_MSG(a < prev, "band %d alpha %d did not fall below %d",
                          band, a, prev);
            UT_ASSERT_MSG(a > 0, "band %d alpha is %d, expected above 0",
                          band, a);
            prev = a;
        }
        UT_ASSERT_MSG(fogRoadBandAlpha(0) == FOG_ROAD_ALPHA,
                      "the band at the edge carries %d, expected %d",
                      (int)fogRoadBandAlpha(0), FOG_ROAD_ALPHA);
        UT_ASSERT_MSG(fogRoadBandAlpha(FOG_ROAD_BANDS) == 0,
                      "past the last band has to be nothing, got %d",
                      (int)fogRoadBandAlpha(FOG_ROAD_BANDS));
    }

    return 0;
}

/* The store the prefs loader writes through: every value of the enum survives
 * a set and a get, and anything else lands on the default. That is what keeps
 * a hand-edited prefs file from leaving the views in a look neither of them
 * knows how to draw. */
int run_fog_style_setting(void) {
    int i; /* Looping variable */
    FogStyle was = gfxGetFogStyle();

    UT_ASSERT_MSG(was == FOG_STYLE_GREY,
                  "the fog style starts at %d, expected Grey (%d)",
                  (int)was, (int)FOG_STYLE_GREY);

    for (i = 0; i < FOG_STYLE_COUNT; i++) {
        gfxSetFogStyle((FogStyle)i);
        UT_ASSERT_MSG((int)gfxGetFogStyle() == i,
                      "set fog style %d, read back %d", i,
                      (int)gfxGetFogStyle());
    }

    gfxSetFogStyle((FogStyle)FOG_STYLE_COUNT);
    UT_ASSERT_MSG(gfxGetFogStyle() == FOG_STYLE_GREY,
                  "a style past the end left %d, expected Grey",
                  (int)gfxGetFogStyle());

    gfxSetFogStyle((FogStyle)99);
    UT_ASSERT_MSG(gfxGetFogStyle() == FOG_STYLE_GREY,
                  "a nonsense style left %d, expected Grey",
                  (int)gfxGetFogStyle());

    gfxSetFogStyle(was);
    return 0;
}

/* ------------------------------------------------------------------ */
/* The edge mask                                                      */
/* ------------------------------------------------------------------ */

/* Two squares that are not road, named so a grid below reads as a picture. */
#define G ((unsigned char)GRASS)  /* plain ground */
#define W ((unsigned char)DEEP_SEA_SOLID)
#define R ((unsigned char)ROAD_SOLID)

int run_fog_road_edges(void) {
    /* The run of tile numbers that counts as road, at both ends and just
     * outside each. */
    UT_ASSERT_MSG(fogRoadIsRoadTile((unsigned char)ROAD_HORZ) &&
                      fogRoadIsRoadTile((unsigned char)ROAD_SOLID) &&
                      fogRoadIsRoadTile((unsigned char)ROAD_WATER11) &&
                      fogRoadIsRoadTile((unsigned char)ROAD_SIDE4),
                  "a road piece was not read as road");
    UT_ASSERT_MSG(!fogRoadIsRoadTile((unsigned char)(ROAD_HORZ - 1)) &&
                      !fogRoadIsRoadTile((unsigned char)(ROAD_SIDE4 + 1)) &&
                      !fogRoadIsRoadTile((unsigned char)BASE_GOOD) &&
                      !fogRoadIsRoadTile((unsigned char)BUILD_SOLID) &&
                      !fogRoadIsRoadTile((unsigned char)DEEP_SEA_SOLID),
                  "something that is not a road piece was read as road");

    /* A square that is never seen comes over as 255, which must not band. */
    UT_ASSERT_MSG(!fogRoadIsRoadTile((unsigned char)255),
                  "an unseen square was read as road");

    /* A square that is not road is never banded, whatever is beside it. */
    UT_ASSERT_MSG(fogRoadEdges(G, R, R, R, R) == 0,
                  "grass surrounded by road was banded");

    /* A lone road square is banded on all four sides. */
    UT_ASSERT_MSG(fogRoadEdges(R, G, G, G, G) ==
                      (FOG_ROAD_EDGE_LEFT | FOG_ROAD_EDGE_RIGHT |
                       FOG_ROAD_EDGE_TOP | FOG_ROAD_EDGE_BOTTOM),
                  "a lone road square was banded %d, expected all four sides",
                  (int)fogRoadEdges(R, G, G, G, G));

    /* A square in the middle of a wide road is not banded at all, so a road
     * is outlined round the outside and not ruled into squares. */
    UT_ASSERT_MSG(fogRoadEdges(R, R, R, R, R) == 0,
                  "a square inside a road was banded %d, expected none",
                  (int)fogRoadEdges(R, R, R, R, R));

    /* A square in a road running left to right is banded top and bottom
     * only — the two sides that face something else. */
    UT_ASSERT_MSG(fogRoadEdges(R, R, R, G, G) ==
                      (FOG_ROAD_EDGE_TOP | FOG_ROAD_EDGE_BOTTOM),
                  "a left-right road was banded %d, expected top and bottom",
                  (int)fogRoadEdges(R, R, R, G, G));

    /* Water is not road either, so a road along a shore is banded on the
     * water side. */
    UT_ASSERT_MSG(fogRoadEdges(R, R, R, W, R) == FOG_ROAD_EDGE_TOP,
                  "a road beside water was banded %d, expected the top only",
                  (int)fogRoadEdges(R, R, R, W, R));

    return 0;
}

/* The whole-grid walk: the same answers, reached from a picture rather than
 * from four neighbours handed in by hand, plus what happens at the edge of
 * the grid itself. */
int run_fog_road_edge_masks(void) {
    /* A four by four with a road running across the middle row and one arm
     * going down from it, and the road touching the right hand edge of the
     * grid.
     *
     *   . . . .
     *   R R R R   <- runs off the right edge
     *   . R . .
     *   . . . .
     */
    static const unsigned char tiles[16] = {
        G, G, G, G,
        R, R, R, R,
        G, R, G, G,
        G, G, G, G
    };
    unsigned char out[16];
    int i; /* Looping variable */

    memset(out, 0xEE, sizeof(out));
    fogRoadEdgeMasks(tiles, 4, 4, out);

    /* Nothing that is not road carries a band. */
    for (i = 0; i < 16; i++) {
        if (fogRoadIsRoadTile(tiles[i])) continue;
        UT_ASSERT_MSG(out[i] == 0, "square %d is not road but was banded %d",
                      i, (int)out[i]);
    }

    /* The left end of the run: banded left, because the grid ends there and
     * a square off the grid is open ground. Banded top too, and banded
     * bottom because the arm is not under it. Not banded right — the road
     * goes on. */
    UT_ASSERT_MSG(out[4] == (FOG_ROAD_EDGE_LEFT | FOG_ROAD_EDGE_TOP |
                             FOG_ROAD_EDGE_BOTTOM),
                  "the left end of the run was banded %d", (int)out[4]);

    /* The square the arm hangs from: road left, road right and road below,
     * so only the top is banded. */
    UT_ASSERT_MSG(out[5] == FOG_ROAD_EDGE_TOP,
                  "the junction was banded %d, expected the top only",
                  (int)out[5]);

    /* The square beside it, over open ground: top and bottom. */
    UT_ASSERT_MSG(out[6] == (FOG_ROAD_EDGE_TOP | FOG_ROAD_EDGE_BOTTOM),
                  "the square beside the junction was banded %d", (int)out[6]);

    /* The right hand end, which runs off the grid: banded on the right for
     * the same reason the left end is. */
    UT_ASSERT_MSG(out[7] == (FOG_ROAD_EDGE_RIGHT | FOG_ROAD_EDGE_TOP |
                             FOG_ROAD_EDGE_BOTTOM),
                  "the right end of the run was banded %d", (int)out[7]);

    /* The arm itself: road above it, open ground on the other three. */
    UT_ASSERT_MSG(out[9] == (FOG_ROAD_EDGE_LEFT | FOG_ROAD_EDGE_RIGHT |
                             FOG_ROAD_EDGE_BOTTOM),
                  "the arm was banded %d, expected all but the top",
                  (int)out[9]);

    /* A grid with no road in it writes zeroes and nothing else. */
    {
        static const unsigned char empty[4] = { G, G, W, W };
        unsigned char none[4];
        memset(none, 0xEE, sizeof(none));
        fogRoadEdgeMasks(empty, 2, 2, none);
        UT_ASSERT_MSG(none[0] == 0 && none[1] == 0 && none[2] == 0 &&
                          none[3] == 0,
                      "a grid with no road was banded %d,%d,%d,%d",
                      (int)none[0], (int)none[1], (int)none[2], (int)none[3]);
    }

    return 0;
}

#undef G
#undef W
#undef R
